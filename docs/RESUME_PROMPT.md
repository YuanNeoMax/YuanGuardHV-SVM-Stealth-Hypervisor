# YuanGuardHV 新窗口恢复提示词（2026-10-30 重写，9.439 更新）

> **本文件是唯一入口。** 读完第 0、1、3、4 节就能接手。
> 完整历史在 `docs/YUANMOD_HANDOFF_CURRENT.md`（13858 行，按 9.3xx 编号），
> 只在需要溯源某条结论时去查，**不要从头读**。

---

## 0. 一句话现状

**裸机 SVM/NPT hypervisor：加载、虚拟化、隐藏页、保护、卸载 —— 全链路闭环。
卸载路径在 29 次崩溃后于 9.445 首次干净成功；G25 17 项在 9.447 全绿。**

| 能做什么 | 状态 |
|---|---|
| 加载驱动 | **可以**（`sc start` → RUNNING，0.8–6.5 秒）|
| 虚拟化 12 核 + NPT | **可以**（`g_npt.pml4_pa` 有效、12 个 VCPU 全建好）|
| 隐藏页 / decoy / 全功能回归 | **可以**（**G25 17 项 ALL PASS**，9.447 实测两次）|
| Tier 9.1 无设备 CPUID 通道 | **可以**（设备可移除，门铃可达）|
| Tier 9.2 控制通道认证 | **可以**（G41 13/13）|
| **卸载驱动** | **可以**（`sc stop` → STOPPED，2.6–3.2 秒，`WIN32_EXIT_CODE=0`）|

**已知代价**：比 9.445 之前的版本慢约 12 倍（12 个核持续在 guest 里跑循环）。见 8.4。

**当前代码状态：可用，且已回归验证。**

```
s206 exits at unload   = 288,352,654     （2.88 亿次 VMEXIT，说明循环正常在跑）
s206 vmmcall at unload = 287,973,382     （vmmcall 拦截正常）
s206 invmrun at unload = 0               ← 归零：12 个核全部干净离开 VMRUN
WIN32_EXIT_CODE        = 0

G25 (17 项):  OVERALL: ALL PASS          ← 9.447 实测两次
```

**下一步待办**：无阻塞项。可选方向见第 8.3（清死代码）和第 8.4（降性能开销）。

---

## 0.1 卸载为什么曾经崩（一句话版）

**guest 曾是「调用者续行」，于是 `SvVirtualizeProcessor` 以 guest 身份 `return`，
控制流回到 `SvExecuteOnEachProcessor` 继续下一个核，而上一帧永不返回 —— 12 个核
留下 12 个悬挂的 `vmrun` 帧。驱动 unmap 后这些帧执行 `vmsave` 撞失效代码页。**

修法是两件事，**都不推导、都不手工展开 C 帧**：

1. `Sv206LaunchVm` 在 `mov rsp,rcx` 之前用自己的 RSP 存进 `VMCB.ControlArea.HostExitRsp`
   —— 那是唯一同时持有「线程栈」和「VpData」的地方。
2. `Sv206LeaveToHost` 在正确栈上直接 `call PsTerminateSystemThread` 结束线程。

**六次失败的共同教训**（详见 `YUANMOD_HANDOFF_CURRENT.md` 9.445.3）：
> **C 函数的栈帧只能由它自己的 epilogue 离开。**
> 它拥有 `/GS` cookie、SEH 链、对齐。任何「算好偏移手工 ret/jmp」都会在某一层
> ABI 上撞墙，而且每次撞的墙不一样。
> **正确的退出不是「重建栈」，而是「在正确的栈上做一次合法调用」。**

---

## 1. 用户铁律（违反即失败）

1. **会改变机器状态的命令必须先得到明确确认**：`sc create/start/stop/delete`、
   加载/卸载驱动、压力测试、重启、改启动链、推 GitHub。
   *只读操作可以直接执行*。
2. **只修用户报告过的问题**，不擅自扩大修复范围。
3. **每次改动/决策都写进 `docs/YUANMOD_HANDOFF_CURRENT.md`**，编号 9.3xx。
4. **构建和蓝屏取证不需要确认**。
5. **绝不推送 GitHub**（全部本地提交）。
6. **不要用 `python -c` 改 .h/.c**，用 edit 工具。
7. **蓝屏后自动取证 → 定位 → 修复 → 重跑，不中断**。
8. **优先级**：重启类最后、压测最后、按蓝屏风险从小到大做；
   高风险（会崩/会重启）先确认。

---

## 2. 环境

```
工作目录      D:\yuanguard
驱动源码      D:\yuanguard\YuanGuardHV\hv\
测试脚本      D:\aaaaaavm\
客户端        D:\yuanguard\YuanGuardHV\tools\yghv_ctl.ps1
构建          cd YuanGuardHV && set YGHV_BAREMETAL_STEP=206 && set YGHV_206B_COEXIST=1
              && set YGHV_206B_GNPT=1 && build.bat
带符号构建    ... && set YGHV_NO_DEVICE=1 && build.bat   （Tier 9.1 用）
调试器        "C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\kd.exe"
              必须用 -y "C:\symbols"（本地符号），srv* 会 Could not write to pipe
dump 配置     CrashDumpEnabled=2, CrashDumpType 已删除, AlwaysKeepMemoryDump=1
              → C:\Windows\MEMORY.DMP（2-3 GB，含物理内存）
机器          12 核 AMD，16 GB RAM，Win10 19045
```

**构建成功的判据不是 md5**（每次重签都变）。看 7 个静态检查全绿 + `Build SUCCESS`。

---

## 3. 当前代码状态与第一件事

```
HEAD = b52434a  fix(9.445b)  卸载首次干净成功，invmrun 归零
       f04d0ff  fix(9.445)   Sv206LaunchVm 自己保存线程 RSP
       23f34df  fix(9.444b)  exit 栈指针移入 VMCB
       d6af154  fix(9.444)   两步返回协议（失败，被 9.445 取代）
       eacde8b  fix(9.444)   两步返回协议（失败）
       cf13c30  feat(9.443)  方案 B：专用 guest 桩 + 每核线程（失败）
       354db2c  docs(9.440)  复核 9.434 因果链
       5866fb5  fix(9.439)   回滚 9.436 + VMMCALL 改 ExitVm
```

**卸载路径已修好，三轮实机验证通过。** 关键改动只有三处：

| # | 位置 | 改动 |
|---|---|---|
| 1 | `Sv206LaunchVm`（ASM 开头） | `mov rax,rsp` → `mov [rcx-5FD0h+6110h],rax`，在切栈**之前**保存线程 RSP |
| 2 | `Sv206LeaveToHost`（ASM） | `sub rsp,28h / xor ecx,ecx / call rax`，在正确栈上结束线程，不碰 C 帧 |
| 3 | `SvHandleVmExit` 的 VMMCALL 分支 | 写 `VpRegs->Rcx`（栈）/`Rax`（终止例程）/`Rbx`（thunk）三个槽 |

**验证读数**（`progress.log`）：

```
s206 exits at unload   = 0x112e658e = 288,352,654
s206 vmmcall at unload = 0x11285c06 = 287,973,382
s206 invmrun at unload = 0x0          ← 归零
```

**G25 17 项已在 9.447 跑通两次（`OVERALL: ALL PASS`）** —— 卸载修好之后，
保护功能也确认完好。见第 11 节。跑 G25 前必读 8.2 的四个超时值。

---

## 4. 核心结论：卸载为什么崩，以及怎么修好的

### 4.1 真正的根因（9.442 定位，9.445 验证）

```
DriverEntry
  └─ Sv206Entry()
       └─ SvVirtualizeAllProcessors()
            └─ 每核 SvVirtualizeProcessor
                 └─ RtlCaptureContext(contextRecord)      ← 抓本函数当下的 RIP
                 └─ SvPrepareForVirtualization
                      └─ StateSaveArea.Rip = ContextRecord->Rip   ★ guest = 本函数续行
                 └─ Sv206LaunchVm()
                      └─ vmrun  ← guest 以 guest 身份跑完本函数剩余代码
                                → 执行 `return`
                                → 控制流回到 SvExecuteOnEachProcessor 的 for 循环
                                → 线程被 pin 到下一个核 → 再次 vmrun
                                → 但上一帧永不返回，lock dec 永不执行
```

**12 核走完 = 12 个永久悬挂的 vmrun 帧。** 每核的 `vpData` 独立分配，
所以 12 个 `HostStackLayout` 各自持有一个永不返回的帧。

卸载时 `DriverUnload` 走完 14 步、返回、操作系统 unmap 驱动镜像，
那些悬挂帧的下一条 `vmsave` 执行了已失效的代码页 → `0xD1 PAGED_IP`。

**9.434 的推理错在哪**：它说「这行之后没有拦截指令」—— 对**那个函数剩下的三行**成立，
但那三行 `return` 之后进入的是 `SvExecuteOnEachProcessor` 和整个内核，
VMEXIT 一直在发生。**它把「函数剩余部分」当成了「guest 的全部生命周期」。**

### 4.2 实测数据（9.441 加的 exit 计数器第一次给出答案）

```
s206 exits at unload  = 213,601      ← 9.434 说「永不产生 VMEXIT」，错
s206 vmmcall at unload = 0            ← 排除整个 trampoline 假设族
s206 invmrun at unload = 12           ← 12 个悬挂帧，不是「卡死」
```

`vmmcall = 0` 是决定性的：拦截位武装着，21 万次退出里一次 vmmcall 都没有 ——
因为 guest 执行的是**真实内核代码**，内核不发 `vmmcall`。

### 4.3 修法：两件事，都不推导、都不手工展开 C 帧

**（1）`Sv206LaunchVm` 自己保存线程 RSP**（在切栈之前）

```asm
Sv206LaunchVm proc frame
        mov rax, rsp                              ; 线程真实栈
        mov qword ptr [rcx - 5FD0h + 6110h], rax  ; -> VMCB.ControlArea.HostExitRsp
        mov rsp, rcx                              ; 然后才切到 HostStackLayout
```

`rcx` = `&HostStackLayout` = `VPD + 0x5FD0`，所以 `0x6110 - 0x5FD0 = 0x140`。
**这是唯一同时持有「线程栈」和「VpData」的地方。**

**（2）`Sv206LeaveToHost` 在正确栈上结束线程**

```asm
Sv206LeaveToHost proc
        sub     rsp, 28h          ; 0x20 shadow + 8 对齐
        xor     ecx, ecx          ; STATUS_SUCCESS
        call    rax               ; rax = PsTerminateSystemThread
        add     rsp, 28h
Sv206LeavePark:
        hlt
        jmp     Sv206LeavePark
Sv206LeaveToHost endp
```

`SvHandleVmExit` 的 VMMCALL 分支写三个槽（`GUEST_REGISTERS` 偏移）：
`Rcx(+0x70)` = 线程栈、`Rax(+0x78)` = 终止例程、`Rbx(+0x60)` = thunk。

### 4.4 六次失败的谱系 —— 每次一个 ABI 违规

| 版本 | 做法 | 崩溃 |
|---|---|---|
| 9.443 | 驱动自造栈 `&HostStackLayout` | `0x1E` `KiSwapContext` #GP |
| 9.444a | `_AddressOfReturnAddress()` | `0x0E` `KiStartSystemThread` |
| 9.444b | 全局表 + `KeGetCurrentProcessorNumber()` 索引 | `0x50` `MiSystemFault` |
| 9.444c | 手工越过 `/GS` 帧 | `0xF7` `_report_gsfailure` |
| 9.445a | `call rax` 覆盖栈顶返回槽 | `0x1E` `yghv_pa+0x2501c` |
| 9.445b | `call rax` 但 `rax` 没设 | `0x1E` `yghv_pa+0x2501b` |

> **共同教训：C 函数的栈帧只能由它自己的 epilogue 离开。**
> 它拥有 `/GS` cookie、SEH 链、对齐。任何「算好偏移手工 ret/jmp」都会在某一层
> ABI 上撞墙，而且每次撞的墙不一样。
> **正确的退出不是「重建栈」，而是「在正确的栈上做一次合法调用」。**

### 4.5 已证伪的推断（别再重走）

| 推断 | 证伪依据 |
|---|---|
| `npt_cleanup` 提前释放活表（9.381）| `npt-range` 工具：NPT 在 `0xC7B...`，故障地址全不在 |
| VPD 页对齐是根因（9.386）| 改真页对齐后崩溃不变 |
| AVIC timer 是元凶（9.388）| 加了 cancel+drain 后崩溃不变 |
| ack 从未被写是根因（9.378/9.383）| ack 早在 ASM 桩 `SvLV20` 里就有 |
| stop check 位置错（9.413/9.414/9.416）| 三次都死在 `SvLV20` 的坏 rsp 上（9.383），不是检查本身 |
| `vmsave` 写已释放 VMCB（9.421）| `PAGED_IP` —— 是代码页失效不是数据 |
| guest 是 trampoline（9.431）| 9.434 读了赋值语句，9.443 实测加桩后加载挂死 |
| 「永不产生 #VMEXIT」（9.434）| **`exits = 213,601`** |
| 「有第二套 VMRUN 循环」（9.395 证伪）| 证伪所用三个读数全部失效（见 9.440）|

### 4.6 三个曾经被当成「实测」的恒等式（9.440）

| 读数 | 实际情况 |
|---|---|
| `g_S206VrLog` 为空 | 只说明「没有受保护页的写 NPF」。唯一写入点在 NPF 写裁决分支，**没 arm 保护页的机器必然为空** |
| `snap_vmrun_stopped = 0` | **恒等式**。唯一递增点 `Sv206VmrunShouldExit()` 在产物里零引用（9.419 删了 ASM 检查，C 侧没删）|
| `snap_resident_threads = 0` | **恒等式**。206-B 不启动 resident |

**判据**：一个计数器能当证据，前提是**它在被测事件没发生时也会动**。
做不到这一点的计数器，为空和为 0 都不说明任何事。

---

## 5. 架构约束（违反会崩或挂）

来自 9.3xx 的实测，**每条都是踩出来的**：

### 5.1 VMCB 位是 per-core，且跨驱动重载存活
`prepare` 每核只跑一次。运行中改某核的拦截位必须用
`SvExecuteOnEachProcessor`（已在 `SvTscApplyOnCore` 里验证）。
**且 `prepare` 必须显式清零任何我们可能设过的位** —— 「这次不设置」≠「清除」。

### 5.2 `SvExecuteOnEachProcessor` 不是通用原语
它只保证当前线程绑到目标核，**不保证那个核不在 VMRUN 里、不保证没有 guest 正在碰
要改的状态**。TSC 位用它成功了（没有东西在用）；`#BP` 位用它两次 `0x139`。

### 5.3 VM-exit 帧里不要调用 C 函数
`SvLV10` / `SvLV20` 这类 ASM 桩的栈布局是按「不碰 guest 状态」设计的。
在里面 `call` 一个 C 函数会因序言（`__chkstk` / SEH frame）失败：
- 9.413 → `0x139 MISSING_GSFRAME`
- 9.414 → `0x7F` 双故障
- 9.416 → **零 call 的内联版也 `0x7F`** ⇒ 连纯内联都不能随便加

### 5.4 `KeIpiGenericCall` 只保证到达，不保证上下文
回调里 `wrmsr` 在 GIF=0 的核上会被拒（`0x1E`）。
合法清 `EFER.SVME` 只有一个窗口：`ExitVm` 路径里
`vmload` → `stgi` → `wrmsr`。

### 5.5 `NT_ASSERT` 在 free build 里是空的
任何「只靠 `NT_ASSERT` 支撑」的前置条件（对齐、范围、互斥）在 release 驱动里
**等于不存在**。判据：去掉 assert 后代码会算错地址 ⇒ 它必须是运行时检查。

### 5.6 页表遍历用 `mem_pte.c`，不是 `protect.c`
`protect.c` 的 `yghv_pt_read` 用 `MmGetVirtualForPhysical`（本项目自己标记为
*not reliable for arbitrary physical pages*）。`mem_pte.c` 用 `MmCopyMemory(PHYSICAL)`，
注释明确 *safe at DISPATCH_LEVEL and takes no locks*。**两套并存，选错一次赔一台机器。**

### 5.7 观测点必须满足三个条件
1. 只有那个过程会写（不被被观测的过程改写）
2. 单调（写进去不会被清零）
3. 在要观测的事件之前已有值

不满足就会出现「双义值」：卸载过程中的读数天然会被卸载本身改写
（`g_PowerCallbackRegistration` 被清成 NULL、`g_resident_thread_count` 被清成 0）。

### 5.8 跨崩溃存活的存储不能属于会被卸载的模块
静态变量、模块 `.bss`、模块内分配的页 —— 驱动 unmap 后**都不再属于你**。
本树为此踩了两次（9.390 的静态页、9.391）。

---

## 6. 可用的观测手段（都验证过）

### 6.1 卸载进度页（`yghv_ut_mark`）
`hv/yghv_unload_trace.c` —— 加载时 `MmAllocateContiguousMemory` 分配，
**故意永不释放**（4 KB/次加载的泄漏，换一份能活过卸载的记录）。
120 槽不取模历史环 + 独立快照区。

埋点：`1 START / 10 NOTIFY_OFF / 20 COOP_UNLOAD / 21 skip-walk / 25 COOP+5 /
30 STOP_REQ / 40 JOIN_GUEST / 45 stopped / 46 active / 47 cond / 50 JOIN_203 /
51 in-vmrun / 52 waited / 53 skipped / 55-57 最终等待 / 80 CTRL / 90 PROTECT /
100 NPT / 110 CORE / 120 DONE`

**读法**（PA 必须**卸载前**记录，见 6.2）：

```
kd -z C:\Windows\MEMORY.DMP -y "C:\symbols" -c "!dd <PA> L40; q"
```

**.bss 不可达时怎么办**：kernel dump 只保存内核可达的页，驱动 `.bss` 时有时无。
用 6.2 的 PA 定位进度页（它在池里，通常可达）。

### 6.2 PA 记录流程（`t_paflow.ps1`）
```powershell
pwsh -File D:\aaaaaavm\t_paflow.ps1 -Image <sys> -Label <tag>
```
它做三件事：加载 → **用 `unload-trace` 读出 PA 写进 `pa_<tag>_<stamp>.txt`** →
`sc stop`。崩溃后拿那个 PA 去 dump 里查。

**必须这么做**：PA 每次加载都变，事后算不出来。

### 6.3 `npt-range`
```
yghv_ctl.ps1 npt-range            → 页数 + 最低/最高 PA
yghv_ctl.ps1 npt-range <hexpa>    → 该地址是不是我们的 NPT 页
```
**用途**：把「这地址像不像 NPT」从推断变成算术。它已经证伪了一个错了三次的假设。

### 6.4 `unload-trace`
```
yghv_ctl.ps1 unload-trace   → page PA / last step / value
```

### 6.5 dump 分析的三条硬规矩
1. **必须 `-y "C:\symbols"`**（本地）。`srv*` 会 `Could not write to pipe`，
   `!analyze` 完全失效（实测 248631 行错误、0 个结果字段）。
2. **`.map` 的 RVA 用 map 里 `00000001400xxxxx` 那一列**，不是 `0003:000xxxxx`
   段内偏移。用错读到的全是 0。
3. **映射前先看相邻符号间隔**：6-16 字节的是 import thunk 表，不是函数体。

---

## 7. PowerShell / 构建陷阱

1. **脚本必须 UTF-8 with BOM 或纯 ASCII**。没有 BOM 的中文会让 PS 5.1 报
   「缺少 `}`」这种远离真因的错误。
2. **单引号字符串里不要放 em dash（`—`）**，会破坏引号配对。
3. **`throw '...<hex>...'` 里的 `<` 会被当成重定向**。
4. **`[uint32]$negativeInt` 会 InvalidCast**，用
   `[BitConverter]::ToUInt32([BitConverter]::GetBytes($v),0)`。
5. **`0x...u` 后缀是 C 语法**，PowerShell 没有。
6. **空串参数不会以空串到达**：`f('')` 收到的是 `$null`。需要显式 token（如 `key clear`）。
7. **`0xDEADBEEF` 在 PS 里是负数**，用 `[uint32]3735928559`。
8. **`git refs/heads/main` 会被重启中断写成全 NUL**。修复：
   `tail -1 .git/logs/HEAD | awk '{print $2}'` → 写回 `.git/refs/heads/main`。
9. **测试脚本自己会造假象**：断言必须能区分它声称要测的机制。
   本树有 7 个「测试断言了自己的假设」的案例。

---

## 8. 剩余工作清单（诚实版）

### 8.1 已完成（9.439 → 9.445）

1. ~~回滚 9.436，恢复 `Rip = ContextRecord->Rip`~~ —— 9.439
2. ~~`case VMEXIT_VMMCALL` 改成 `ExitVm = TRUE`~~ —— 9.439
3. ~~修 9.383 的 `rsp = 'SSVM'` 栈损坏~~ —— 9.441
4. ~~加无条件 exit 计数器~~ —— 9.441（这个仪表带来了后面全部结论）
5. ~~卸载路径修好~~ —— 9.445b，三轮连续验证通过

### 8.2 G25 回归：已全绿（9.447 完成）

**在 9.445b 上跑了两次，`OVERALL: ALL PASS`。** 见第 11 节。

**跑 G25 时的四个超时（都被实测打脸后调过，别改回去）**

| 项 | 值 | 为什么 |
|---|---|---|
| 各命令 `-TimeoutSec` | **150 / 180** | 实测单命令 40–50 秒（9.445b 比旧版慢 12 倍）|
| `HangTimeoutSec` | **240** | 同上 |
| `mmf-hold` 寿命 | **3600 秒** | 全流程 ~20 分钟，原来的 900 秒会让目标进程在 T12 前退出 |
| `$img` | 可切换 | 做新旧对照 |

**`mmf-hold` 那条尤其要记住**：目标进程到期退出后，`mem-read` 返回
`FAILED status=0x00000000`、`mem-pte` 返回 `NOT MAPPED` —— 看起来完全像驱动回归，
实际上是测试工具超时。9.446 就误判过一次（见 9.447）。

### 8.3 已知的三处死代码 / 假判据（不影响功能，影响读数可信度）

1. `g_VmrunStopped` / `Sv206VmrunShouldExit` —— 无调用者（9.419 删了 ASM 检查）。
   只要它还在，`stopped == 0` 就会被误读成「一个都没停」。
2. `main.c` 里 `stopped == 0 && resident_threads_at_entry == 0` ——
   两个条件在 206-B 下恒真（见 4.6）。9.443 起遍历已改为无条件执行，
   但这个判据的注释还留着旧结论。
3. `g_S206VrLog` 的注释 —— 它不是 exit 日志，是 NPF 写裁决日志。

### 8.4 已知的性能代价（不是缺陷，但要知道）

9.445b 比 9.445 之前的版本慢约 **12 倍**（T1：4 秒 → 42 秒）。
原因是 9.443 的每核线程模型让 **12 个核持续在 guest 里跑 VMRUN 循环**
（实测 `exits` / `vmmcall` 各 2.88 亿次）。

这是「常驻虚拟化」的固有开销。**如果将来需要降下来**，方向是让 guest 桩
在没有工作时 `hlt` 而不是空转 —— 但那要先解决「`hlt` 之后如何被唤醒」，
不是一个可以顺手改的东西。

### 8.5 原清单（Tier 5/6/8/9，共 30 项）
| Tier | 项 | 状态 |
|---|---|---|
| 5.1 TSC | offset 平移 / freeze | offset **成立**；freeze **已禁用**（无法证明释放）|
| 5.4 CPUID 视图 | 三档 | **完成**（`016aa31`）|
| 5.6 轨迹导出 | | **完成**（`9e1b2bc`）|
| 9.1 无设备通道 | | **完成**（设备可移除，门铃可达）|
| 9.2 通道认证 | HMAC + 序号 | **完成**（G41 13/13）|
| 9.3/9.4/9.5 | 冗余/远程/隐蔽 | 未做（9.4 建议跳过并写原因）|
| 6.4 | 跨 VM 操纵 | **跳过**（需写原因）|
| 6.1 | 启动期控制 | **需要重启，最后做且要确认** |
| 其余 5.x / 6.x / 8.x | | 未做 |

### 8.6 自我隐藏（Tier 7.1）：做完一半，另一半有明确下一步（9.450）

**先做的是观测面，不是隐藏。** `npt_exclude_self` 一直存在，但没有任何东西能回答
「X 号页现在到底被排除了没有」—— 而一个页完全可以在每个计数器上 `hidden=1`，
NPT 条目却仍写着 present。

```powershell
yghv_ctl.ps1 npt-view <hex_pa>
```

NPT 是 identity map（`GPA == SPA`），一个 PA 一次查表。输出关键是一对字段：

| `guest` | `host` | 含义 |
|---|---|---|
| 0 | 1 | **正确隐藏** —— guest 够不到，我们够得到 |
| 0 | 0 | **把自己锁在外面**（9.343 的形态）|
| 1 | * | 根本没隐藏 |

**第一个假阴性就在这一对上**：第一版把 `host` 也做成「NPT present」，
于是隐藏成功的页报 `host=0`，判成 `LOCKED OUT`。

> 错在把两套翻译混成一个。NPT 是 **guest 的**翻译；我们走 **host 页表**（自己的 CR3），
> NPT 管不到。隐藏页 `guest=0` 而我们照常可读，是因为 `host` 字段用的是 **物理直读**（`yghv_pa_read`），它根本不经过 NPT。
> 改成「真的去读一次这个帧」后判据才正确。

**实测结论**

```
hidden page:   guest present=0  host=1   -> HIDDEN（正确）
驱动自己的页:  guest present=1  perm=RWX, frame=0xC6E00000 large=1
               -> VISIBLE to the guest（未隐藏）
```

**驱动自己的页落在 2MB identity 大页里。** 这就是 `npt_exclude_self` 没起作用的原因：
它只在 `if(!pd[p1].large_page)` 里剔页表页，**2MB 大页整段跳过**；
而且它的设计范围只有 PML4/PDPT/PD 表页，**不含代码/数据页**。

**诚实状态**

| 面 | 状态 |
|---|---|
| `npt-view` 观测面 | **完成** |
| 三张 guard 表页隐藏 + host 仍可读 | **完成并实测** |
| 符号链接 `\\.\YuanGuardHV` | **完成并实测**（9.453，5 秒窗口自恢复）|
| 驱动代码/数据页逐页拆剔 | 数据页**可做未做**；代码页见下 |
| 服务注册表键 / SCM 条目 | 实测可见，未隐藏 |
| `loader_stealth` 模块摘链 | 代码在，默认 0，未验证 |

**为什么代码页那一半不能顺手补**

`HIDE_UNMAP` 清 present，访问即 NPF。**注意：没有 ALLOW loop** —— `yghv_guard_fault_is_ours` 无调用者，`unmap_bare` 也没有放行分支。我们之所以还能读隐藏页，靠的是物理直读绕开 NPT，不是被放行。
数据页可以（访问稀疏）；**代码页不行** —— 每条指令取指一次 VMEXIT，机器立刻不可用。
DECOY 也不行（present 保持但指向全零，我们的取指会拿到零字节）。

唯一可行方向是把驱动代码搬到 guest 从未映射过的物理区、NPT 里根本不建条目 ——
那是 Tier 2.2 自建映射的**反向应用**，是独立的一轮工作，不是一个补丁。

### 8.7 性能基准：`mem-bench`，实测 B 快 3.7–4.0×（9.449，Tier 1.5 完成）

**`MEM_ENGINE_PLAN.md` 4.3 那张表写 `1–3× / 5–20× / ~100×` —— 从没有运行支撑。**
现在有 `mem-bench`（IOCTL 0x82B）。

```powershell
yghv_ctl.ps1 mem-bench <pid> <hex_pattern> [start_hex] [end_hex] [budget]
```

**实测**（A 跑第二次，热缓存给 A，所以 B 的数是保守的）

| 目标 | 范围 | **B 快** | A/B 读次数 | hits |
|---|---|---|---|---|
| javaw 1.4GB 工作集 | 32 MB（全 4K 页） | **3.96×** | 1.0× | 223 = 223 |
| explorer | 1.25 MB（页稀疏） | **3.72×** | 3.0× | 0 = 0 |

**怎么读这张表**
- 32MB 全 4K 页时读次数相同（各 8192），**优势就是「每页一次翻译」vs「每地址一次翻译」**
- 页稀疏时 B 少读 3 倍 —— 这才是文档说的「页表驱动」在数字上的样子
- **`~100×`（大页）没有测到**：本机这几个进程全是 4K 页（`mem-pte` 确认 `kind=4K`）。
  那一行现在在文档里仍标注为**设计推断，非实测**

**两个被基准自己抓到的错（这条比数字更有价值）**

1. **功能分歧**：第一版在 javaw 32MB 上报 `A hits=223 / B hits=242`。
   根因不是性能，是 **A 从不越过范围末端（`va+4096<=end`），B 按整页读跨了出去** ——
   两个引擎没在扫同一批字节，比较无效。修完 `223 = 223`。
   > 设计时刻意把 `a_hits`/`b_hits` 分开输出、绝不平均，它真的就触发了一次。
2. **大页缓冲**：第一版给 B 只有 4KB 缓冲，2MB 大页被读成 512 次 4K，
   传输退化到和 A 一样，大页优势被自己扔掉。

**两道自查**：先看 `bytes` 是否相等（相等则 `hits` 自然会一致），再看 `hits`。

**基准必须能报自己的失败**：第一版在无页范围上报 `26,594×` —— 纯属产物。
现在输出 `walk_cr3`，且 `pages_visited==0` 时打 `INVALID`。

### 8.8 符号链接隐藏：`link-hide`，5 秒窗口自恢复（9.453）

`powershell
 yghv_ctl.ps1 link-hide        # 读回 link_present / device_present
 yghv-ctl.ps1 link-hide on     # 摘掉 \\.\YuanGuardHV
 yghv_ctl.ps1 link-hide off    # 恢复
`$nl：隐藏后用户态 **NOT OPENABLE**，5 秒后自动 **OPENED**。

**第一版是单向开关，实测立刻打脸**：`link-hide off` 报 ERROR_FILE_NOT_FOUND ——**客户端自己就是经 `\\.\` 打开设备的**，摘掉名字等于把撤销它所需的那扇门一起摘了。
`\Device\` 用户态单独够不到，没有第二扇门。

> 考虑过用 CPUID 通道或共享窗做撤销，否决了：那会让**撤销路径的可达性和执行路径不一样**，
> 而不对等的撤销是证明不了的撤销。改为**隐藏自带恢复定时器**。

**第二个错**：DPC 里直接调 `IoCreateSymbolicLink` —— DPC 在 DISPATCH，这个 API 要 PASSIVE。 DPC 只发现「窗口结束」，恢复交给 `IoAllocateWorkItem` 排到 PASSIVE。

`device_present` 全程为真，**这是诚实标注**：设备本体一直在 `\Device\YuanGuardHV`，。把「少一个名字」说成「不可见」正是 7.1 反复警告的过称。
### 8.9 VMEXIT 覆盖：206 主线已补到 35 个（9.448）

**`MEM_ENGINE_PLAN_TIER5/6/8_9.md` 开头写「现成 VMEXIT 已实现 35 种」——
那句话原本只对 `vmexit.c`（旧 resident 路径）成立。** 206 主线只有 12 个 case。
9.448 补齐了，现在两边都是 35 个。

**怎么用（`exit-mask`，IOCTL 0x82A）**

```powershell
yghv_ctl.ps1 exit-mask              # 读回当前掩码 + 已观察到几个码 + 总退出数
yghv_ctl.ps1 exit-mask quiet        # HLT + PAUSE（已验证安全）
yghv_ctl.ps1 exit-mask full         # 可证明安全的那一组
yghv_ctl.ps1 exit-mask events       # INTR/NMI 等事件重投递 —— 实验性
yghv_ctl.ps1 exit-mask host-insn    # VMSAVE/STGI 等 —— host 自己执行，会自指崩
yghv_ctl.ps1 exit-mask clear        # 回到默认（不武装额外位）
```

**立即生效，不需要重载**（输出会报 `applied: N live VMCB(s)`）。

**两次实测**

| 预设 | Misc1 | Misc2 | 观察到的码数 | 卸载 |
|---|---|---|---|---|
| `quiet` | `0x01800000` | `0` | 3 | `invmrun=0x0` |
| `full` | `0x07C08000` | `0x00003F00` | 6 | `invmrun=0x0`，1,922,428,533 exits |

**`full` 里面有什么、没有什么（这是可证明的边界，不要扩大）**

`full` 只含：HLT / PAUSE / INVD / WBINVD / INVLPG / INVLPGA / RDPMC。

`full` **不含**、但可以按名字单独启用：

| 组 | 预设 | 为什么不进 full |
|---|---|---|
| INTR/NMI/SMI/INIT/VINTR/RSM | `events` | 事件重投递**未证明**；秒级千次，"看起来对"不够 |
| VMLOAD/VMSAVE/STGI/CLGI/SKINIT | `host-insn` | **host 自己执行**（prepare 里两次 `__svm_vmsave`）→ 拦住自己 |
| IDTR/GDTR/LDTR/TR 读写、TASKSR、FERR | `host-insn` | VMSAVE 会搬这些寄存器，同一条自指路 |
| PUSHF/POPF/IRET/SWINT | — | 介入控制转移是**真行为改变**，不是观测 |

**这条规则要记住**：一个拦截位只能为「**guest 会执行**」的指令武装。
host 自己执行的指令武装起来就是自指，第一次开 `full` 会几秒内重启。

**9.448 花了三次重启才走到这里**，三个根因都不同（ring 非原子递增 /
改位后未置 `VmcbClean=0` / 把 INVLPG 这类**有副作用**的指令当成无事发生）。
详见 `YUANMOD_HANDOFF_CURRENT.md` 9.448。

---

## 9. 关键文件

| 文件 | 作用 |
|---|---|
| `hv/svm_simplevm206.cpp` | 206 主线：VMCB 准备、`SvHandleVmExit`、CPUID |
| `hv/svm_simplevm206_x64.asm` | VMRUN 桩（`SvLV10` 循环 / `SvLV20` 出口）。**改这里极度危险** |
| `hv/svm_simplevm206.hpp` | VMCB 结构 + 拦截位定义 |
| `hv/svm_core.c` | 旧 resident 机制（`g_resident_thread_count=0` ⇒ 未启用）|
| `hv/svm_trampoline.S` / `svm_trampoline.asm` | 测试 guest（三条指令）。**当前未被用作 guest 入口** |
| `hv/npt_core.c` | NPT。`npt_cleanup` 的 `pt_reg` 已修失效复用（9.381）|
| `hv/mem_pte.c` vs `hv/protect.c` | **两套页表遍历**，用前者 |
| `hv/yghv_unload_trace.c` | 卸载进度页（第 6.1 节）|
| `tools/yghv_ctl.ps1` | 客户端，63 条命令。有 `unload-trace` / `npt-range` / `sys-auth` |
| `tests/` | 7 个静态检查脚本，`build.bat` 自动跑 |

---

## 10. 新窗口的第一个动作（照着做）

```powershell
# 1. 确认状态（只读，可以直接跑）
cd D:\yuanguard && git log --oneline -3 && git status --porcelain
sc.exe query yghvpa                       # 期望 STOPPED（不是 START_PENDING）

# 2. 确认卸载修复还在（只读）
git log --oneline -3 -- YuanGuardHV/hv/svm_simplevm206_x64.asm
findstr /C:"HostExitRsp" YuanGuardHV\hv\svm_simplevm206.hpp     # 应有 +0x110 槽
grep -n "sub     rsp, 28h" YuanGuardHV/hv/svm_simplevm206_x64.asm

# 3. 构建（改动后才需要；构建和蓝屏取证不需要确认）
cd YuanGuardHV && set YGHV_BAREMETAL_STEP=206 && set YGHV_206B_COEXIST=1 `
  && set YGHV_206B_GNPT=1 && build.bat    # 必须 7/7 全绿

# 4. 部署 + 验证（会改机器状态，需要用户明确确认）
Copy-Item D:\yuanguard\YuanGuardHV\bin\yuanguard_hv.sys C:\yghv_pa.sys -Force
sc.exe start yghvpa      # 期望 RUNNING
sc.exe stop  yghvpa      # 期望 STOPPED，WIN32_EXIT_CODE=0，无蓝屏
Get-Content C:\Windows\yghv_progress.log | Select-String "invmrun at unload"
#   ★ 期望 invmrun = 0x0 —— 这是卸载成功的唯一判据
```

**判据只有一条：`invmrun at unload = 0x0`。**
非 0 说明还有核没离开 VMRUN，驱动 unmap 后必崩。

**第 5 步（可选）：跑 G25 全功能回归**

```powershell
# 会装独立服务 yghvg25，与 yghvpa 不冲突；约 20 分钟
# 跑之前确认 D:\aaaaaavm\g25_tier7.ps1 里这四个数是对的（见 8.2）
powershell -NoProfile -ExecutionPolicy Bypass -File D:\aaaaaavm\g25_tier7.ps1
Get-Content D:\aaaaaavm\g25_result.txt | Select-String "OVERALL"
#   期望 OVERALL: ALL PASS
```

**注意 `mmf-hold` 的 3600 秒寿命**：如果它到期退出，T12 会以
`mem-read: FAILED status=0` / `mem-pte: NOT MAPPED` 失败 —— 看起来像驱动回归，
实际是测试工具超时。9.446 误判过一次（见 9.447）。

---

## 11. 累积已验证历史（实机跑过）

| 测试 | 内容 | 结果 |
|---|---|---|
| **G25** | **17 项全功能回归，在 9.445b 上跑了两次** | **`OVERALL: ALL PASS`** |
| **9.445b** | **加载 → 运行 → 卸载，三轮连续** | **全部通过，无蓝屏** |
| G33 | snapshot 非破坏 + drain 消费 | ALL PASS |
| G39 | TSC 逐核武装与释放 | ALL PASS |
| G40 | 逐核 walk 可达性（12/12 核） | ALL PASS |
| G41 | 控制通道认证 | 13/13 |
| G43 | 无设备 CPUID 通道 | 部分（设备可移除已验证）|
| G18/G28/G29/G30/G31 | hide/decoy/border/why | 见各 9.3xx |

**G25 的 17 项（9.445b 实测）**

```
T1 sha256 selftest   T2 arm        T2b clean verify  T3 hidecore
T4 alive after hide  T4b hidden    T5 uncovered      T6 sha tamper
T7 decoy mechanism   T8 exit timing T9 escape        T10 surface
T11 health           T12 regression T13 unload       T7 mechanism name
```

**9.445b 的三轮加载/卸载**

| 轮 | start | stop | `invmrun` |
|---|---|---|---|
| 1 | 6.54s | 3.2s | `0x0` |
| 2 | 0.85s | 2.64s | `0x0` |
| 3 | 0.79s | 2.73s | `0x0` |

第一轮 `exits = 288,352,654`、`vmmcall = 287,973,382`、`WIN32_EXIT_CODE = 0`。

**性能代价（9.447 实测）**：9.445b 比 `zAV_final` 慢约 **12 倍**
（T1 从 4 秒变 42 秒），因为 12 个核全在 guest 里跑 VMRUN 循环。
G25 因此从 ~2 分钟变成 ~20 分钟，**所有测试超时必须按新量级设置**（见 8.2）。

**回归基线镜像**：`zAV_final`（`yuanguard_hv_step206zAV_final_20261018.sys`）
是 9.445 之前的最后一个可用版本 —— 但它的 `sc stop` 会挂死（T13 FAIL）。
**当前可用镜像是 9.445b 构建**（见第 3 节）。

---

## 12. 加速查证（别重复劳动）

```powershell
# 当前所有 yghv 服务
sc.exe query yghvpa; sc.exe query yghvfin

# 蓝屏事件
Get-WinEvent -FilterHashtable @{LogName='System';Id=6008,1001} -MaxEvents 3

# dump 是否存在及时间
ls C:\Windows\MEMORY.DMP

# 从 dump 读进度页（PA 来自 pa_*.txt）
kd -z C:\Windows\MEMORY.DMP -y "C:\symbols" -c "!dd 0xC7BD5000 L40; q"

# 从 dump 读某个全局变量（RVA 用 map 里 14000xxxxx 那列 + 模块基址）
kd -z C:\Windows\MEMORY.DMP -y "C:\symbols" -c "dq <addr> L1; q"

# 9.439 新增：不信源码就信产物 —— 反汇编复核关键赋值
dumpbin /disasm:nobytes YuanGuardHV\bin\yuanguard_hv.sys > dis.txt
# 找 RIP 是否来自 ContextRecord（应见 [r13+0xF8]，且无 trampoline 引用）
findstr /C:"000000F8h" dis.txt
# 找 VMMCALL case 是否真的设了 ExitVm（应见 lock inc g_S206Vmmcalls 后紧跟 mov dil,1）
findstr /C:"14005FFE0h" dis.txt

# 9.440 新增：查某个函数在产物里到底有没有调用者（判死代码）
# 先从 map 取 RVA，再数产物里的引用次数；0 = 死代码，它的计数器是恒等式
findstr /C:"Sv206VmrunShouldExit" YuanGuardHV\bin\yuanguard_hv.map
dumpbin /disasm:nobytes YuanGuardHV\bin\yuanguard_hv.sys | findstr /C:"1400223340h"
# 同理可查 g_VmrunStopped 的唯一递增点是否可达

# 9.445 新增：卸载路径的三处关键指令（改动后必查）
dumpbin /disasm:nobytes YuanGuardHV\bin\yuanguard_hv.sys > dis.txt
# a) Sv206LaunchVm 开头：保存线程 RSP（rcx=VPD+5FD0，所以偏移是 140h）
findstr /C:"mov         rax,rsp" dis.txt
# b) Sv206LeaveToHost：sub rsp,28h / xor ecx,ecx / call rax / hlt
findstr /C:"sub         rsp,28h" dis.txt
# c) SvHandleVmExit 写三个槽：+70h Rcx / +78h Rax / +60h Rbx
findstr /C:"r14+70h" dis.txt
findstr /C:"r14+78h" dis.txt

# 卸载后确认成功（不是看 md5）
Get-Content C:\Windows\yghv_progress.log | Select-String "invmrun at unload"
#   期望 invmrun = 0x0；非 0 说明还有核没离开 VMRUN
```

**构建判据**：不是 md5（每次重签都变）。看 `ALL SOURCE CHECKS PASS` +
`ALL MAP CHECKS PASS` + `Build SUCCESS` 三行。

**读数判据（9.440 新增）**：在把这棵树上的任何计数器写进结论之前，先问三个问题：
1. 它**只有**那个过程会写吗？（`g_S206VrLog` 只有 NPF 分支写 ⇒ 它不测 exit）
2. 它的写入点**在产物里可达**吗？（`g_VmrunStopped` 唯一递增点零引用 ⇒ 恒等式）
3. 在这个 build 模式下**那个机制启动了吗**？（206-B 不启 resident ⇒ 该计数器恒为 0）

三个问题里有一个答不上，那个读数就不能当证据。**本树已有 3 个读数栽在这里。**

---

## 13. 一句话给新会话

**这个项目的难点不是写代码，是「在没有观测能力的路径上，任何改动都不可验证」。**
今晚 29 次重启里，真正修好的根因有 8 个，而**证伪自己的推断有 8 个** ——
后者花的代价和前者一样多。

**所以：先建观测，再改代码。**

**9.439 追加一条更狠的**：**读源码得出的结论，也要用产物复核一遍。**
9.434 靠读一行赋值推翻了 9.431，那是对的；但 9.436 又靠读同一行推出了「换成测试 guest」，
结果加载挂死。同一个动作，一次纠正了错误，一次制造了错误 —— 差别不在读没读源码，
在**有没有问「改完之后谁会不满意」**。`SvVirtualizeAllProcessors` 是同步的，
一个永不返回的 guest 必然让 `sc start` 挂住 —— 这一条在动手前就能推出来。

**9.440 追加一条最狠的，因为它打的是「实测」这两个字本身**：

**这个项目最大的风险不是改错代码，是把「恒等式」当成「测量结果」。**
今晚的因果链上有三个这样的读数：一个只测了别的东西（`g_S206VrLog` 测 NPF 不测 exit）、
一个的写入点根本没有调用者（`g_VmrunStopped`）、一个测的机制在本 build 下没启动
（`g_resident_thread_count`）。它们都以「已实测」的身份进了文档，
并支撑了至少一条「已证伪」结论（4.4 第 3 条）。

**判据只有一句：一个计数器能当证据，前提是它在被测事件没发生时也会动。**
做不到这一点的计数器，为空和为 0 都不说明任何事 —— 它只会让一个未经验证的假设
看起来像一次观测。

---

**9.445 追加最后一条，关于「怎么修」而不是「怎么看」**：

**C 函数的栈帧只能由它自己的 epilogue 离开。**

卸载路径修了六次，每次都在同一个地方换一种算法：怎么把线程的真实栈指针
「重建」出来，然后 `ret` 或 `jmp` 回某个 C 帧。六次撞了六堵不同的 ABI 墙：

```
0x1E KiSwapContext #GP     驱动自造栈
0x0E KiStartSystemThread   函数自己的 cookie 槽
0x50 MiSystemFault         全局表索引错位
0xF7 _report_gsfailure     手工越过 /GS 帧
0x1E +0x2501c              call 覆盖栈顶返回槽
0x1E +0x2501b              call rax 但 rax 没设
```

**最后一次成功的改动反而更小**：不再重建任何东西 —— `Sv206LaunchVm` 在切栈前
用自己的 RSP 存一下（一条指令），退出时在**那个栈上做一次合法调用**结束线程，
一个 C 帧都不碰。

> **所以：当修复方案需要「算偏移手工跳转」时，那是设计错了的信号，不是偏移算错了。**
> 正确的退出不是「重建栈」，而是「在正确的栈上做一次合法调用」。

---

**9.447 追加最后一条，关于「怎么判断 A 是不是由 B 引起的」**：

**换了一个变量，结果就变了 —— 这不能推出"是它造成的"。**

9.446 用两个镜像跑 G25，看到失败项正好互换（旧版 T13 挂、新版 T12 挂），
就写下了「9.445 用 T13 的修复换来 T12 的回归」。

**它漏掉了第二个变量：运行时长。** 9.445 比旧版慢 12 倍，于是
`mmf-hold` 创建的目标进程在 T12 之前就到期退出了。T12 读一个已经不存在的进程，
当然失败。真正的判据不是「哪个镜像」，而是**「target 创建到 T12 之间隔了多少秒」**：

```
baseline  102s  PASS
run1      808s  PASS
run2      948s  FAIL   <- 超过 mmf-hold 的 900 秒
run3      913s  PASS   <- 仍然超 900，但把寿命改成 3600 秒
```

**判据换到时间这一列，结论立刻反转。** 和 4.6「把恒等式当测量」是同一个病：
都以为自己抓住了一个因果，其实只抓住了一个恰好同时变动的量。

> **做对照实验时，先列出所有会变的量。** 只盯着你以为的那个，
> 另外那个就会替它背锅 —— 而且背得很有说服力。
