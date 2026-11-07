# YuanGuardHV 新窗口恢复提示词（2026-10-30 重写，9.439 更新）

> **本文件是唯一入口。** 读完第 0、1、3、4 节就能接手。
> 完整历史在 `docs/YUANMOD_HANDOFF_CURRENT.md`（13858 行，按 9.3xx 编号），
> 只在需要溯源某条结论时去查，**不要从头读**。

---

## 0. 一句话现状

**裸机 SVM/NPT hypervisor：加载、虚拟化、隐藏页、保护、卸载 —— 全链路闭环。**
**卸载判据 `invmrun at unload = 0x0` 一直是干净的；性能已在 9.527 大幅改善。**

| 能做什么 | 状态 |
|---|---|
| 加载驱动 | **可以**（`sc start` → RUNNING，0.9–7 秒）|
| 虚拟化 + NPT | **可以**，**默认只虚拟化物理核**（这台机器 6 个，12 逻辑核）|
| 隐藏页 / decoy | **可以**，且在**虚拟化的核上**实测有效（`npt-view` 显示 frame 指向零页）|
| Tier 9.1 无设备 CPUID 通道 | **可以**（设备可移除，门铃可达）|
| Tier 9.2 控制通道认证 | **可以但默认 off**（G41 13/13；需 `sys-auth enforce`）|
| **卸载驱动** | **可以**（`sc stop` → STOPPED，2.4–7 秒，`WIN32_EXIT_CODE=0`）|

##### 性能（9.527 实测，这是本树第一次真正的性能优化）

| 基准 | 优化前（12 逻辑核）| **优化后（6 物理核，默认）** |
|---|---|---|
| 单线程 | 7.6× | **1.1×** |
| **12 线程** | **12.8×** | **1.2×** |

改法：`yghv_s206_core_limit()` 默认从「全部逻辑核」改成**物理核数**
（用 `KeQueryLogicalProcessorRelationship(RelationProcessorCore)` 数记录得来）。
**代价是覆盖范围**：未虚拟化的 6 个逻辑核上 NPT 保护不生效。可用
`HKLM\SYSTEM\CurrentControlSet\Services\yghvpa\Parameters\CoreCount`（REG_DWORD）覆盖。

##### G25 回归：**15/17**（9.533 在 6 核默认下实测）

```
T1-T6, T8, T10-T12  PASS
T7 FAIL  mechanism=UNKNOWN, the page is still visible to the guest
T9 FAIL  no mechanism identified in T7
```

**这不是缺陷，是「部分虚拟化」的直接后果**：`mmf-read` 是短命外部进程，
被 Windows 调度到 **native 的 6 个核**上就不经过 NPT，DECOY 对它不存在。
**要 G25 全绿就设 `CoreCount=12`，代价是 11.6×。这是明确的取舍。**

##### 卸载判据（唯一可信的「没坏」证据）

```
s206 invmrun at unload = 0x0
WIN32_EXIT_CODE        = 0
```

##### 下一步待办

1. **取舍决策**：默认 6 核（1.2×，T7/T9 失败）还是 12 核（11.6×，全绿）—— operator 定。
   **9.536 已确认 1.2× 是本架构极限**：每核成本系数 k 从 6 核 0.035 涨到 12 核 0.881（25 倍），
   6 核已落在最平的一段；6 核下 handler 只占 **0.01%**。再降只能减核（= 覆盖）或换硬件。
2. ~~藏 `g_npt` 页的真实后果~~ —— **9.535 已答**：不锁死（DECOY 不产生 #NPF，不会有递归），
   真正的后果是**那一页上 25 个控制流变量被一次清零**（`g_watchdog_stop` / `g_os_guest_stop_requested` /
   `g_s203_threads[]` 等）。**该页列为不可藏，不再做这个实验。**
3. Tier 6.1 启动期控制 —— 台账唯一悬空项，需重启，窗口不可回滚。

已结案（不是待办）：`.data` 段隐藏（目标不成立）、`loader_stealth`（9.499）、
`mem-bp dr`（9.507 改成拒绝）、`mmf-hold` 多页（9.529）、
`mem-pte` MISS/HIT（9.531）、`sys-guard` 拒绝规则（9.532 判为无法安全实现）。

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
HEAD = 9c535b4  docs: 修正 QUICKSTART 抬头 G25 状态（当前）
       8d1c628  fix(9.530-9.531)  mem-bp 地址类型 / mem-pte MISS-HIT 区分
       0c99795  docs(9.533)      G25 在 6 核下 2 项 FAIL
       aec8b7e  fix(9.529)       mmf-hold 多页提交
       20d9c72  feat(9.527)      默认虚拟化物理核（12线程 12.8x->1.2x）
       53e870a  feat(9.525)      CoreCount 运行时参数
       8b8e0fd  feat(9.521-9.524) handler-cost / 三桩实测 / 单核实验
       451adef  docs(9.520)      四个读数的分工
       5dd3777  docs(9.519)      单核实验定论：hlt 确实在等
       4abf8d5  fix(9.516)       纯 hlt 桩保留
       31568c8  docs(9.512)      更正 9.511 错误结论
       b52434a  fix(9.445b)      卸载首次干净
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

**G25 在 9.497 的镜像上是 `OVERALL: ALL PASS`；9.533 在 6 核默认下重跑为 15/17
（T7/T9 FAIL，原因见第 0 节 —— 部分虚拟化的必然结果，不是缺陷）。** 卸载修好之后，
保护功能也确认完好；这一轮覆盖了 9.448 的 35 个 VMEXIT case 与 9.465 的 RIP 推进。
（9.447 那两次跑的是 9.445b，不能当作当前代码的结论。）见第 11 节与 9.497。

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

### 8.2 G25 回归：已在当前代码上重跑全绿（9.497）

**最后一次全绿：2026-11-01 12:16–12:38（9.497 镜像，12 核），17 项全 True。**
**2026-11-08（9.533）在 6 核默认下重跑：15/17，T7/T9 FAIL —— 部分虚拟化的代价，见第 0 节。**
那次跑在 9.465（`Rip = NRip`）与 9.448（VMEXIT 12→35）之后的镜像上 ——
**9.447 那两次是在 9.445b 上跑的，不能拿来代表当前代码。** 见第 11 节与 9.497。

**T12 的 target→T12 间隔 = 1056 秒**（`12:16:46` 建目标 → `12:34:22` 进 T12），
`mmf-hold` 3600 秒，余量 3.4×。这个数必须跟 OVERALL 一起记，理由见本节的坑。

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

### 8.4 性能代价：已查清到底，并且修掉了一个真 bug（9.462–9.470）

**9.513 推翻了两条结论。** 原文写「CPU 100% 在这台机器上无法消除，只能接受或换机器」——
**那句是错的**：本机 C1 空闲占用 **78.84%**，CPU 100% 是**桩的形态问题**，不是硬件约束。
正确的部分保留：VMEXIT 风暴确实降了 19,200 倍（9.465 的 RIP 修复）。

#### 8.4.0 两条被推翻的判据（先读这段，后面全是它的证据）

| 曾写下的结论 | 实测 | 错在哪 |
|---|---|---|
| 「本机固件无 C-state，`hlt` 是 nop」 | 空闲 `% C1 Time = 78.84%` | 判据用的是 `powercfg /availablesleepstates`，那是**系统睡眠 S1/S2/S3**，不是 CPU 的 C-state |
| 「CPU 100% 无法消除」 | 单线程 **1.045s → 8.640s**（慢 8.3×）；12 并行 **1.36s → 28.27s**（慢 20.8×）| 只看了利用率，没测**吞吐**；根因在桩里 |

**S-state ≠ C-state** 这一条要单独记住：

```
powercfg /availablesleepstates:
    待机 (S3)                ← 有
    待机 (S1) 系统固件不支持    ← 这行被读成了「无 C-state」
    待机 (S2) 系统固件不支持
```

CPU 的 C-state 要用 PerfMon 的 `Processor Information\% C1 Time` 系列，不是 `powercfg`。
**名字里都有「待机」，但一个讲整机挂起，一个讲单核空闲 —— 换了判据，结论就反了。**

#### 8.4.1 那个真 bug（9.465，这一轮最重要的发现）

三轮"优化空转"的实验全部失效，原因是 `case VMEXIT_VMMCALL` **不推进 RIP**：

```c
case VMEXIT_VMMCALL:
    InterlockedIncrement64(&g_S206Vmmcalls);
    if (Sv206VmrunShouldExit() != 0) { guestContext.ExitVm = TRUE; }
    break;                       // ← 没有 Rip = NRip
```

RIP 不推进 → 每次 VMRUN 都从**同一条 `vmmcall`** 重新进入 guest →
桩里前面的 `mov ecx,N` 和整个 `pause` 循环**一次都没执行过**。

**桩不是循环，是同一条指令的无尽重复。** 所以改桩里的任何延时都毫无效果，
而读数看起来"稳定"，被误读成"那条指令在这个平台上不做事"。

加一行 `Rip = NRip` 之后：**1418 万/秒 → 738/秒（降 19,200 倍）**。

#### 8.4.2 hlt 的真实能力（9.466 重测，前提才正确）

| 桩形态 | vmmcall 速率 | CPU |
|---|---|---|
| 纯 vmmcall 重复（9.462–9.464 实际测的） | **1418 万/秒** | 100% |
| 仅 `hlt` | **19,190/秒** | 100% |
| `hlt` + `pause 1000000`（当前采用） | **738/秒** | 100% |

`hlt` **不是 nop**（它把回到 host 的频率降了 739 倍）。

但「没有 C-state 就不会真正休眠」**这半句 9.513 已推翻**：
9.464 用 `powercfg /availablesleepstates` 判定「无 C-state」，而那张表报的是
**系统睡眠 S1/S2**。PerfMon 的 `% C1 Time` 实测：空闲 78.84，加载后 0.00。
**C1 一直可用，只是加载后被桩占住、核没机会进。**

> 因此这三行表的读法也变了：
> 「仅 hlt = 19,190/秒」与「hlt + pause 1000000 = 738/秒」的差别**不是**「hlt 无效」，
> 而是 **pause 把 hlt 换来的休眠烧掉了** —— 见 8.4.5。

**9.464 曾判定「hlt 是 nop」——那个结论是在 RIP bug 还活着时得出的，
hlt 从未被执行过。已更正。**

#### 8.4.3 第三条路（去掉 pause，只留 hlt）—— **9.514 判失败，9.516 用吞吐判据反转，保留**

| 方案 | 判断 |
|---|---|
| 不虚拟化（按需进 guest）| 仍是重架构改动，NPT 保护会变成窗口式 |
| 换一台有 C-state 的机器 | 不需要 —— 本机 C1 空闲 78.84% |
| **去掉 pause、只留 hlt** | **实测失败，且更差 39×。见下表** |

| 桩形态 | vmmcall 速率 | CPU | `% C1` | **吞吐（4M 乘加）** |
|---|---|---|---|---|
| 驱动卸载（参照）| — | — | — | **0.521 s** |
| `hlt` + 100 万 `pause`（旧）| 539 /s | 100% | 0.00 | **4.730 s** |
| 纯 `hlt`（9.516 采用）| 1,891 /s | 100% | 0.00 | **3.492 s**（快 26%）|

**9.514 曾据「CPU 100% 且 C1 = 0」判定纯 hlt 无效并回滚。9.516 用吞吐判据反转了这个结论。**

##### 判据的独立性比数量重要（这是本轮最贵的一条）

| 判据 | 可用吗 | 理由 |
|---|---|---|
| `% C1 Time` | **不可用** | 加载后恒为 0.00，无法区分「没进 C1」和「进了但 guest 看不见」|
| `CPU / % Processor Time` | **不可用** | 恒为 100%，无法分辨其中多少是等待 |
| **吞吐量基准** | **可用** | 直接测「还能干多少活」，与 C-state 无关 |

> **当两个判据都不信时，它们的互相印证不产生任何可信度。**
> 9.514 用两个都要读 guest 侧计数器的判据互相印证，得出「hlt 不 park」；
> 换成一个完全不读计数器的判据（吞吐），结论直接反转。

##### 9.517 标尺法：把「每轮 529 µs」拆成斜率 + 截距

**方法**：每轮时间 `T(N) = C + N × t_pause`，N = 桩里的 pause 个数。
两点标定，**完全不读 CPU 计数器**，只用 vmmcall 次数与墙钟时间。

| N | 实测 vmmcall | 每轮时间 |
|---|---|---|
| **0**（纯 hlt）| **1,891 /s 每核** | **529 µs** |
| **1,000,000**（旧桩）| **45 /s 每核** | **22,263 µs** |

```
t_pause = (22,263 − 529)/1,000,000 = 21.73 ns/pause = 78 周期 @3.6GHz
C       = 529 µs                            <- 每轮固定成本
```

**78 周期与 Zen 3 的 PAUSE 实现吻合（公开资料约 64–78）。标尺法本身成立。**

##### 截距 529 µs：**9.519 已测定，假设 A 成立 —— hlt 真的在等**

两个候选，预言相反，**用核数把它们分开了**：

| 假设 | 预言（单核时）|
|---|---|
| **A. hlt 真在等** | 仍 ~529 µs/轮 |
| **B. 12 核串行化** | 529/12 = **44 µs/轮**（速率涨约 12 倍）|

**实验**：把 `svm_simplevm206.cpp` 里**两处** `numOfProcessors` 改成 1
（5362 的 prepare 循环、**6184 的线程创建循环**）—— 两处都要改，只改一处不生效。

实测 CPU：core 0 = 100%，core 1–11 = 13–61%（回到基线）。**改动生效。**

| 核数 | vmmcall 速率 | 每轮时间 |
|---|---|---|
| **12 核** | 1,891 /s 每核 | **529 µs** |
| **1 核** | **2,442 /s** | **409 µs** |

**核数减到 1/12，每轮只快 23%，不是 12 倍。假设 A 成立。**

##### 9.520 把「在等」的物理含义钉死（这是三个读数的分工问题）

| 量 | STOPPED | 12 核纯 hlt | 这个量在说什么 |
|---|---|---|---|
| vmmcall 速率 | — | 1,891/s 每核（529 µs/轮）| guest 执行了多少轮 |
| 单核时 | — | **2,442/s（409 µs/轮）** | 核数减 12×，每轮只快 23% |
| `CPU / % Processor Time` | 19.73 | **100.00** | Windows 的**调度**统计 |
| `% C1 Time` | **62–77** | **0.00** | CPU 的**物理**睡眠状态 |
| **吞吐量（4M 乘加）** | **0.521 s** | **3.294 s**（慢 6.3×）| **机器还能干多少活** |

```
若 hlt 是 nop  -> 4 条指令 + VMEXIT 往返 ≈ 2 µs -> 应 500,000/s
实测           -> 2,442/s                          -> 慢 205 倍

若 hlt park 到定时器（NtQueryTimerResolution 实测 999 µs）
               -> 平均等 999/2 ≈ 500 µs
实测           -> 409 µs                            -> 吻合
```

**409 µs 与「等一个定时器半周期」吻合，与「4 条指令的往返」差 205 倍。hlt 确实在等。**

##### ⚠ `% C1 = 0` 的真正含义（同一个读数第二次被误读）

> 这两个读数（`CPU 100%` 与 `% C1 = 0`）**都在 guest 里**，
> 而 guest 是 Windows —— 它看到的是自己的调度视图。12 个核上各有一个我们的线程占着
> 「可运行」状态，Windows 的 Idle 线程轮不到，idle 统计归零。而 `% C1 Time`
> 依赖 Windows 进入自己的 idle 路径去读 MSR —— **那条路径没被执行到，所以它报 0。**
>
> **它报的是「没测到」，不是「没发生」。**

这与 9.514 的错误是同一个形状：把 `% C1 = 0` 读成了「没有 C1」。

##### 9.519 的结论要精确化

吞吐量：12 核 3.294 s vs 卸载 0.521 s = **慢 6.3 倍**。

> **不是「纯串行化」（那会是 12 倍），也不是「完全无争抢」（那会是 1 倍），
> 而是两者之间：核在等，但等待期间仍互相拖累约 6.3 倍。**

##### 剩下的优化方向（按可验证性排序）

| 方向 | 为什么 | 怎么验证 |
|---|---|---|
| **降低退出频率** | 12 核 × 1,891/s = **22,700 次/秒**的往返 | 减核数已验证有效（9.519）|
| **减少每次退出的处理成本** | `KeRaiseIrqlToDpcLevel` + `yghv_sentry_exit_begin` + `__svm_vmload` **每次退出都执行** | 用 rdtsc 在 handler 首尾打点，测单次成本 |
| **中断合并** | 定时器 999 µs 驱动大部分唤醒 | 需改 Windows timer resolution，系统级 |

**下一步的正确动作是给 handler 装 rdtsc 打点** —— 把「一次退出花多少周期」变成读数，
而不是继续从 CPU 利用率推断。

##### ⚠ 这条推翻了一直在用的推理链

先前的链条是：「CPU 100% ⇒ 没有休眠 ⇒ hlt 是 nop」。**实测推翻了它。**

> **「CPU 利用率高」与「没有休眠」是两件事。**
> 一个每秒醒来 2000 次、每次处理 200 µs 的核，利用率就是 40% —— 它确实睡了，
> 但醒得足够频繁、每次醒着的代价足够大。

12 核时是 1,891 次/s × 12 = 22,700 次/秒的 VMRUN/VMEXIT 往返，**这些往返的开销
把核占满了**，不是「一直不睡」。

##### 真正的优化方向（现在有依据了）

既然 hlt 在等、退出频率由定时器驱动，**降低每轮开销**比「让 hlt 睡更久」更有意义：

1. **减少每次 VMRUN/VMEXIT 的处理成本** —— 每轮 ~200 µs 的处理时间才是真开销
2. **中断合并** —— 多个定时器 tick 合成一次唤醒
3. **降低定时器频率**（若能改 Windows timer resolution）

**「去掉 pause」和「换有 C-state 的机器」这两条早先的方案，方向都是错的。**

**要分开 A 与 B 只有两条路，都需要改机器状态：**

1. **改核数**（1–2 核跑桩）—— 要动 `SVM_MAX_CORES`，并重新验证 12 核路径的全部前提
   （`invmrun` 判据、卸载等待、NPT 一致性）。**是一轮独立工作，不是一次实验。**
2. **去掉 vmmcall**（只 hlt + jmp）—— **会失去停止能力**：停止请求只在 VMMCALL 退出里被读，
   没有它 `sc stop` 会挂死，只能断电重启。**风险 = 重启，不做。**

##### 这个数已经拿到了

> **单核 409 µs/轮 —— A 成立。** 9.519 已实测，不再是未知量。
> 下一轮要优化的是「每轮那 ~200 µs 的处理成本」，而不是「让 hlt 睡更久」。

##### 本轮净收益

- pause 的代价被精确量化：**21.73 ns/条 × 100 万 = 21.7 ms/轮** —— 这正是旧桩比纯 hlt
  慢的那部分（22,263 − 529 = 21,734 µs，**误差 0**）。
- 固定成本 **529 µs 被隔离出来**，不再混在 22 ms 里。
- 确认**标尺法可用**：两个绝对量的比值，不受任何 CPU 计数器影响。

##### 为什么 hlt 不 park（三条实测，不是推断）

1. **四个唤醒前提全部满足，C1 仍是 0：**
   `guest core#0: Rflags=0x40286  IF=1  VIntr=0x0  shadow=0  EventInj=0x0`
2. **HLT 没有被拦截** —— 新增的 `exit-codes` 命令实测非零码只有两个，
   里面**没有** `0x8C(HLT)` 或 `0x78`：
   `````
   `0x72  DR6_WRITE    4003`
   `0x81  VMMCALL     27464`
   `````
3. **所以 hlt 执行了，只是没有 park** —— 这个现象 9.464 就观察到过
   （「hlt 在每次 pass 上都返回」），但当时归因成「固件无 C-state」，
   把真正的问题关掉了 50 个版本。

##### 判据方向错在哪（这是本轮最贵的一条）

> **「取消等待」不等于「得到休眠」。** 9.513 的证据只证明了 `pause` 在烧周期，
> 没有证明 `hlt` 会因此得到 C1。去掉 pause 之后，周期消耗变成了 VMRUN 往返。
>
> 应该问的是「hlt 执行后 C1 有没有上升」，而不是「pause 是不是在烧周期」。
> 前者是**要达成的效果**，后者只是**当前的成本** —— 把成本找出来不等于拿到了效果。

##### 还没验的（下一个人从这里开始）

- HLT 没被拦截、四个唤醒条件也满足，**为什么仍不进 C1**？要查 VMCB 里与 halt 相关的
  状态（`VIntr` 之外的字段）、以及固件是否在 SVM 客户态下抑制 C-state 进入。
- 另一个方向：**不让核 idle 而是少核**。12 个核各跑一个桩是当前架构；
  如果只虚拟化 1-2 个核，CPU 基线会低得多 —— 这是架构问题，不是桩的问题。

---

#### 8.4.4 测量方法（这条被坑过，别再用旧的）

`Get-Counter '\Processor(_Total)\% Processor Time'`，采样 3 次。
**基线 29%，加载后 100%。**

不要用 `PerfRawData` 差值法：`exit-mask` 命令自身每次耗时数十秒，
拿它做时间基准会让时间窗被污染，曾经给出过假的「CPU 0%」。
测速率同理 —— **必须把两次读取的实际墙钟间隔一起记下来**。

#### 8.4.5 9.513 的实测数据与根因（本轮性能工作的全部依据）

##### 三组对照

| 测量 | 驱动 STOPPED | 驱动 RUNNING | 倍数 |
|---|---|---|---|
| 单线程基准（8M 乘加，取 3 次最小）| **1.045 s** | **8.640 s** | **慢 8.3×** |
| 12 份并行（4M 乘加）| **1.36 s** | **28.27 s** | **慢 20.8×** |
| `% C1 Time` | **78.84** | **0.00** | — |
| `% Idle Time` | 79.35 | 0.00 | — |
| `% Processor Time` | 19.73 | 100.00 | — |

**所以 100% 不是读数假象 —— 机器真的慢 8–20 倍。**

##### 根因：`pause` 循环把 `hlt` 换来的休眠全部烧掉

`Sv206GuestStub`（`svm_simplevm206_x64.asm:406`）每轮的序列：

```asm
Sv206Spin:
    hlt                         ; 进 C1
    mov     ecx, 1000000
Sv206SpinLoop:
    pause                       ; ~67 周期
    dec     ecx
    jnz     Sv206SpinLoop       ; 100 万次 ≈ 67M 周期
    mov     rax, 1
    vmmcall                     ; 被拦截 -> VMEXIT
    jmp     Sv206Spin
```

**从实测反推，每一步都对得上：**

| 量 | 值 | 来源 |
|---|---|---|
| vmmcall 速率 | **539 /s**（全机）| 两次 `exit-mask` 读数 ÷ 实际墙钟间隔（53.6 s）|
| 每核 | **45 /s** | 539 ÷ 12 |
| 每轮墙钟 | **22.2 ms** | 1000 ÷ 45 |
| 每轮周期 | **66.7 M** | 22.2 ms × 3 GHz |
| **每个 pause 的周期** | **67** | 66.7 M ÷ 1,000,000 |

**67 周期/PAUSE 与 Zen 3 的 `PAUSE`（约 64 周期）吻合。** 一轮 22.2 ms 里，
**21.99 ms 烧在 100 万个 `pause` 上**，`hlt` 那一行几乎没起作用 ——
不是因为它无效 —— **而是 9.514 证明它在这台机器的 guest 里根本不 park**；
pause 循环只是把「已经没在休眠」这件事变得更慢。

**「去掉 pause 就能拿到 C1」这半步是错的，9.514 实测了：**

| 桩形态 | vmmcall 速率 | CPU | `% C1` |
|---|---|---|---|
| `hlt` + 100 万 `pause` | **539 /s** | 100% | 0.00 |
| 纯 `hlt`（无 pause）| **20,774 /s**（涨 **39×**）| 100% | **0.00** |

**去掉 pause 之后 C1 仍然是 0，退出率涨 39 倍。** pause 是**成本**，不是**阻碍** ——
砍掉成本并不能得到那个从未出现过的效果。

> **这是「参数怎么调读数都不变」的第三种形态** —— 但 9.514 证明**这个命名也早了一步**：
> 不是没执行（9.465）、不是判据看不见（9.459）、也不是「被执行的东西被后面一句抵消」；
> 真相是 **`hlt` 本身在这台机器的 guest 里就不 park**，
> `pause` 只是把「已经没在休眠」这件事变得更慢而已。
>
> **把成本当成阻碍，会给出一条「砍掉成本」的方案 —— 而那条方案已经实测失败。**

##### 顺带测到的退出构成

```
exit 速率   22,816 /s
  vmmcall     539 /s
  其余     22,277 /s   = CPUID
```

`control_device.c:1747` 自己写明了：`distinct observed` **恒为 2**，因为 CPUID 与 VMMCALL
默认都武装 —— 那个数字从来不能用来判断「谁占主导」。

---

### 8.5 原清单（Tier 5/6/8/9，共 30 项）
| Tier | 项 | 状态 |
|---|---|---|
| 5.1 TSC | offset 平移 / freeze | offset **成立**；freeze **已禁用**（无法证明释放）|
| 5.4 CPUID 视图 | 三档 | **完成**（`016aa31`）|
| 5.6 轨迹导出 | | **完成**（`9e1b2bc`）|
| 9.1 无设备通道 | | **完成**（设备可移除，门铃可达）|
| 9.2 通道认证 | HMAC + 序号 | **完成**（G41 13/13）|
| **7.1 自我隐藏** | **观测面 + 段报告 + 段隐藏** | **完成并实测**（见 8.6，`9.455`–`9.460`）|
| **常驻开销** | **VMEXIT 风暴** | **降 19,200 倍**（`9.465`）；CPU 100% 是硬件约束 |
| 9.3/9.4/9.5 | 冗余/远程/隐蔽 | 未做（9.4 建议跳过并写原因）|
| 6.4 | 跨 VM 操纵 | **跳过**（需写原因）|
| 6.1 | 启动期控制 | **需要重启，最后做且要确认** |
| 其余 5.x / 6.x / 8.x | | 未做 |

### 8.6 自我隐藏（Tier 7.1）：**已完成**（9.455–9.460 收尾）

#### 8.6.1 已实测落地的部分

| 面 | 命令 | 状态 |
|---|---|---|
| 观测面 | `npt-view <pa>` | 完成，含 UNMAP / DECOY / 大页三种判据 |
| 段报告 | `sections` | 完成，PE32+ / 6 段 / IAT 定位 |
| **`.pdata` 段隐藏** | `section-hide 3` | **实测通过**：guest `present=1` 但 `frame` 指向零页，host 可读 |
| 三张 guard 表页 | `sys-guard hidecore` | 实测 `hid 3 页` |
| 符号链接 | `link-hide` | 实测 5 秒窗口自恢复 |
| `.rdata` / `.text` | `section-hide 1` / `0` | **拒绝**（含 IAT / 代码，正确行为）|

#### 8.6.2 两个必须记住的判据

**`npt-view` 的判据从「present 是什么」改成「它还解析到自己那一帧吗」：**

| 读数 | 判定 |
|---|---|
| `present=0` | HIDDEN (UNMAP) |
| `present=1` 且 `frame != pa` | **HIDDEN (DECOY)** |
| `present=1` 且 `large=1` | 大页未拆，该 4K 页没被隐藏 |
| `present=1` 且 `frame == pa` | 真的可见 |

**9.460 的教训**：第一版只认 `present=0`，把 DECOY 模式（present 保持 + 指向零页）
报成「VISIBLE」，于是**一次成功的隐藏被读成了失败**，还写进了 HANDOFF 当结论。

**`sections` 必须报 `has_iat`**：IAT 在 `.rdata` 的第一页（RVA `0x28000`）。
隐藏 `.rdata` = 隐藏我们自己的导入表 → 之后任何 `call [IAT slot]` 读 0 跳到地址 0。
实测崩溃：`0x3B C0000005, rip=0` 在 `call IoAcquireRemoveLockEx` 上 ——
**指令本身完全正确，目标被我们自己藏掉了。**

一个我们 **CALL THROUGH** 的页不能藏，和「一个我们执行它的页不能藏」是同一条原则。

#### 8.6.3 `.data`：**9.508–9.513 已结案 —— 不做，理由完整**

早期这里写的是「含活状态，风险最高，不建议动」。那是对的但不够 ——
9.508–9.513 把它量成了**一个不成立的目标**，而不是一个待办。

##### 段构成（这是整个结论的基础）

```
.data 段  VirtualSize   0x35694 = 53.4 页
          SizeOfRawData 0x200   =  0.1 页    <- 只有 512 字节来自文件
                                  53.3 页是 .bss（运行时才有的活状态）
```

**它不是「54 页数据」，是「0.1 页常量 + 53.3 页我们自己的全局变量」。**

##### 54 页里装的是什么（map 逐页取证）

| 页 | 内容 |
|---|---|
| **1** | `g_npt`（NPT 管理器本身）、`g_persistent_mode`、`g_watchdog_stop`… **75 个符号** |
| **12** | `g_npt_alt_ready`、`g_fake_mode`… **77 个符号** |
| **52** | `g_S206Exits`、**`g_S206NpfIdx`**、**`g_S206NpfLog`**（NPF 处理体自己读的）|
| **53** | `g_S206Vmmcalls`、`g_S206DbgIdx`、`g_CpuidView`… **31 个符号** |
| **40 / 44** | `g_code_size` / `g_code_start`（**ALLOW 循环判断「是不是我们自己的故障」用的**）|
| 42–45 | `g_guard`、`g_guard_count`、`g_state_objs`、`g_decoy_pa` |
| 2,3,10,11,20,21,35 | 无符号（7 页）|

##### 三个硬阻碍

1. **藏 `g_npt` 所在页 = 递归死锁。** NPF 处理体自己读 `g_npt`：
   我们的读 → #NPF → 处理体读 `g_npt` → 又一次 #NPF → …
2. **藏 page 52 = 同样递归。** `g_S206NpfIdx` / `g_S206NpfLog` 是**每次写故障**都要写的遥测。
3. **藏 page 40/44 = ALLOW 循环失效。** `g_code_start` 是 `yghv_guard_fault_is_ours()` 的判据，
   藏了它整个自隐藏机制的前提就没了。

##### 9.511 的「47 页从未被触及」已被 9.512 推翻

9.511 量出「54 页里 7 页被访问」，据此写下「47 页从未」。9.512 用同一台机器、
同一份二进制、同一判据重复，**最短窗口就有 13–35 页 A=True**。

真因：`.bss` 全是我们自己的热路径全局，而 206-B COEXIST 下**驱动的 C 代码以 guest 身份执行** ——
每次读写自己的全局都经过 NPT、都会置 A/D。**这些页不是「别人在读我们的数据」，
是我们自己在读自己的数据，只是我们恰好是 guest。**

> **所以「能藏的那部分没有可藏的信息；有信息的那部分不能藏，因为我们自己在用。」**
> 这不是「没做完」，是**目标本身不成立**。详细实测见 9.508–9.513 的交接记录。

##### 仍然可做、也有价值的一件事

在 `sys-guard add` 里加一条**拒绝规则**：目标页若含 `g_npt` / `g_S206NpfIdx` /
`g_S206Vmmcalls` / `g_code_start`，直接拒绝并说明原因。
这是把「递归死锁」**从推断写进代码**，而不是留一个能被误触的入口。**尚未实现。**

#### 8.6.4 旧的「先做观测面」记录（保留，因为方法论仍有价值）

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
| 服务注册表键 / SCM 条目 | **不做**（删键=自断卸载路径，与 9.453 同类错误且无恢复手段）|
| `loader_stealth` 模块摘链 | **修好并验证「设了会跑」**（9.454：之前调用点在被编译掉的分支里，从未执行过）|

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
### 8.9 loader_stealth 从来没被执行过；审计查错了服务名（9.454）

**YGHV_LOADER_STEALTH 在唯一发布的构建配置里从未执行。**

```c
#if YGHV_BAREMETAL_STEP == 206 && !defined(YGHV_206B_COEXIST)
    { return Sv206Entry(d, r); }      // COEXIST 下整块不编译
#endif
#if YGHV_LOADER_STEALTH
    yghv_loader_stealth(d, r);        // 调用点在这里，永远到不了
#endif
```

我们发布的是 YGHV_206B_COEXIST=1，第一个 #if 为假，整块排除；
调用点在其后，所以从未进过任何路径。

**证据链**

| 检查 | 结果 |
|---|---|
| =1 构建符号表 | yghv_loader_stealth @ 0x1400137B0 **存在** |
| =1 构建反汇编找 call 0x1400137B0 | **零命中** |

> 「代码存在」和「代码会跑」是两件事，二进制里两者都能查到——只是需要去查。

修法：调用放到 COEXIST 的真实入口，**在 Sv206Entry 之前**
（yghv_unlink_module 要遍历 PsLoadedModuleList，只有还没被接管时才找得到）。
修完：=1 有 call，不设无 call——开关现在诚实了。

**审计脚本一直查错了服务名**

yghv_stealth_check.ps1 写死查 yuanguard，而实际服务叫 yghvpa。
查一个不存在的名字当然全 CLEAN——**监控范围不含被测对象**的教科书形态：
报告读起来是「我查过了，没有暴露」，实际是「我查的是别的东西」。

改成接受 -ServiceName，并新增 **UNKNOWN** 状态：PsLoadedModuleList
用户态读不到，以前归到 CLEAN，现在诚实报 UNKNOWN——
「我没能看到」和「我看到了，是干净的」是两个答案。

驱动 RUNNING 时的真实结果：isible=4 unknown=1。

**注册表键 / SCM 条目：不做**

删服务键后 SCM 找不到服务 = **驱动无法 sc stop = 只能重启**。
这和 9.453「摘符号链接把客户端自己的门一起摘了」是同类错误，
而且这次**没有定时器能恢复**（SCM 的数据结构不是我们能重建的）。

### 8.10 VMEXIT 覆盖：206 主线已补到 35 个（9.448）

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

### 8.11 ★★★★★ 本轮的六条方法论（比任何单个修复都值钱）

这一轮（9.455–9.470）修复了 4 个真 bug，但真正值得带走的不是它们，是下面六条。
每一条都来自**我这次犯的错**，不是读书读来的。

#### 一、判据的局限 ≠ 被测对象的结论

`npt-view` 只认 `present=0`，于是把 DECOY 模式的**成功隐藏**报成"没隐藏"，
我据此写下"guard 表登记成功，NPT 条目没真正落地"——**那个结论是错的**。

同形态在 9.440 已犯过三次（三个恒为零的计数器）。第四次才是这次。

**规则：写结论前问一句「我的判据能观察到这个事件吗」。**
判据的偏好要写在判据旁边，否则下次还会把「我没看见」写成「它没发生」。

#### 二、当一个参数怎么调读数都不变时，先确认那个参数有没有被执行

9.463 写「`pause 100000` 没有效果」，9.464 又写「`hlt` 没有效果」——
两次都在解释"为什么这个指令不起作用"，而事实是**这些指令根本没走到**
（RIP 不推进，桩是同一条 `vmmcall` 的无尽重复）。

这是第一条的另一面：**不是判据看不见，而是执行流根本没经过被测代码。**

#### 三、一个被判为「无效」的手段，可能在错误的前提下被否决了

9.464 判定 `hlt` 无效时，`hlt` 一次都没执行过。RIP 修好后重测，
它其实带来 **739 倍**改善。

**规则：如果某个结论的依据是「我观察到 X 没有效果」，而 X 所在的执行流
后来被改过，这个结论必须重测，不能继承。**

#### 四、检查器自己也需要被检查

`exit_rip_contract.ps1` 的三版都出过错，每次都是"检查器错了，代码是对的"：

| 版本 | 假失败 |
|---|---|
| 把"下一个 case"当 body 结尾 | 25 个 |
| 只查 body 字面量 | `VMRUN` 假失败 |
| 窗口太窄（差 17 行） | `SvHandleMsrAccess` 假失败 |

**一个把好树报成坏的检查器比没有更糟**——它会训练人忽略它。
所以它现在有自检：每次构建都往副本里注入一次真缺陷，要求检查器必须抓到。

#### 五、采样由硬件持续更新的字段时，单次读数不能当成状态

`VIntr` 在 `0x0` 和 `0xE` 之间来回变。只采一次就会把 `0xE` 误判成
一个需要修的泄漏。`VIntr` / `ExitCode` / `ExitInfo1` 这类 ControlArea 字段
在核运行时是**移动的**。

**规则：要么连续采样看变化范围，要么在核停在已知位置时再读。**

#### 六、测量必须把时间基准一起记下来

用 `exit-mask` 两次读数算速率，却假定间隔是 10 秒（实际 30–55 秒），
报出过「降 183 倍」和「CPU 0%」两个假结论。`PerfRawData` 差值法同样被污染过。

**规则：算速率就把实际墙钟间隔一起打印出来；测 CPU 用
`Get-Counter '\Processor(_Total)\% Processor Time'` 采三次。**

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
| `hv/yghv_sections.c` | **Tier 7.1** 段报告：PE 头解析 + IAT 定位（`has_iat`）。只读 |
| `tools/yghv_ctl.ps1` | 客户端，**70 条命令**（`command_parity.ps1` 是权威计数）|
| `tests/` | 源级 5 项 + 链接后 1 项，`build.bat` 自动跑；详见 `tests/README.md` |
| **`tests/exit_rip_contract.ps1`** | **38 个 VMEXIT case 的 RIP 契约 + 自检**。改 `SvHandleVmExit` 前必看 |
| **`docs/WINDOW_HANDOFF_20261031.md`** | **★ 一次性交接摘要** —— 状态、硬边界、六条方法论、下一步 |

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
| **G25** | **17 项全功能回归** | **9.497 上 `ALL PASS`（12 核）；9.533 在 6 核默认下 15/17（T7/T9 因部分虚拟化失败）** |
| **9.497 部署+冒烟** | **C:\yghv_pa.sys = B0532743 哈希一致 → start 0.42s / sys-health 26.9s / stop 3.92s** | **`invmrun=0x0`，WIN32_EXIT_CODE=0，无蓝屏** |
| **9.445b** | **加载 → 运行 → 卸载，三轮连续** | **全部通过，无蓝屏** |
| G33 | snapshot 非破坏 + drain 消费 | ALL PASS |
| G39 | TSC 逐核武装与释放 | ALL PASS |
| G40 | 逐核 walk 可达性（12/12 核） | ALL PASS |
| G41 | 控制通道认证 | 13/13 |
| G43 | 无设备 CPUID 通道 | 部分（设备可移除已验证）|
| G18/G28/G29/G30/G31 | hide/decoy/border/why | 见各 9.3xx |

**G25 的 17 项（2026-11-01 与 9.445b 两次实测，结果相同）**

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

**9.497 单轮（当前代码，9.465 之后）**

| start | stop | `exits` | `vmmcall` | `invmrun` |
|---|---|---|---|---|
| 0.42s | 3.92s | 2,519,244 | 45,375 | `0x0` |

退出/vmmcall ≈ **55.5**（9.445b 时代是 1.0009）——同一个判据，换了一个数量级的形态：
桩现在是真循环，不是同一条 `vmmcall` 的无尽重复。

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

**9.470 追加最后一条，因为它打的是「代码写了就等于代码跑了」这个默认假设**：

这一轮有三轮实验（9.462 `hlt`、9.463 `pause`、9.464 `hlt+pause`）全部无效，
而每一轮我都写了一篇解释，说明「为什么这个指令在这台机器上不起作用」——
包括一条**当时听起来很扎实的硬件结论**：「本机固件没有 C-state，所以 hlt 是 nop」。
（**9.513 已推翻**：那条结论的判据是 `powercfg /availablesleepstates`，
而它报的是**系统睡眠 S1/S2/S3**；PerfMon 的 `% C1 Time` 实测空闲 78.84。
这是同一病的第四层：**连「平台属性」都可以是一个选错的判据。**）

真相是 `case VMEXIT_VMMCALL` 不推进 RIP，核每次都从**同一条 `vmmcall`**
重新进入 guest。**桩里我改的那些指令，一次都没有被执行过。**

三层解释，一层比一层有理，全部建立在「那段代码跑过」这个从未验证的前提上。

> **改一段代码之后，先确认它被执行到。** 在一条没有观测的路径上，
> 「写了」和「跑了」之间隔着整个这一轮。
