# YuanGuardHV 新窗口恢复提示词

> **入口先看 `INDEX.md`；本文件是"深一层"的基线、雷区、方法论。**
> 逐条变更：9.500+ 在 `YUANMOD_HANDOFF_CURRENT.md`，9.1–9.499 在
> `archive/_ARCHIVE_HANDOFF_9.1-9.299.md`。**只溯源某条结论时去查，不要从头读。**

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
| **卸载驱动** | **可以**（`sc stop` → STOPPED，**0.046 秒**，`WIN32_EXIT_CODE=0`）—— 9.556 前是 1.88–4.12 秒，那段等待的条件不可满足，每次都在空转等超时 |

##### 性能（9.527 实测，这是本树第一次真正的性能优化）

| 基准 | 优化前（12 逻辑核）| **优化后（6 物理核，默认）** |
|---|---|---|
| 单线程 | 7.6× | **1.1×** |
| **12 线程** | **12.8×** | **1.2×** |

改法（**9.577 已推翻，见下方更正**）：`yghv_s206_core_limit()` 默认从「全部逻辑核」改成**物理核数**
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
**9.577 更正：那个 11.6× 是桩模式的数字，真 guest 没有这个代价。**

9.575 重测（固定工作量、带正确性门的编译基准）：

| 配置 | 12 线程吞吐 | G25 |
|---|---|---|
| 6 核桩模式（**9.527-9.576 的旧默认**）| 1.13-1.34× | 15/17 |
| 12 核桩模式 | 4.0-4.4× | — |
| **12 核真 guest（9.577 起的默认）** | **1.01-1.08×** | **17/17** |

根因：桩模式每核每秒几万次 `vmmcall` 空转（实测 1,255,119 次退出），
真 guest 的 guest 是 Windows 自己（27,914 次）。**"减核换性能"优化的是桩的空转，
不是 hypervisor。**

所以 `CoreCount` 不再是"要功能就得牺牲性能"的旋钮 —— 默认全核，
它现在只用于**退回部分核**（例如排查某核上的问题）。

##### 卸载判据（唯一可信的「没坏」证据）

```
s206 invmrun at unload = 0x0
WIN32_EXIT_CODE        = 0
```

##### 下一步待办

1. **取舍决策（三选一）**：9.537 补测了中间点 —— **6 核 1.2×（T7 FAIL）/ 11 核 ≈4×（T7 PASS）/ 12 核 11.6×**。
   11 核仍留 1 个空闲逻辑处理器给 Windows，代价停在 4× 而不是 11.6×。
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

## 8. 剩余工作（见 `TIER_LEDGER.md`，此处只列真正还开着的）

> **9.560 精简：本节原有 747 行，已移到 `archive/RESUME_PROMPT_S8_WORKLIST_20260814.md`。**
> 原因：那是「曾经计划过什么」，不是「还欠什么」。当前的完成度以 `TIER_LEDGER.md` 为准。

| 状态 | 项 |
|---|---|
| **还开着（1 项）** | Tier 6.1 启动期控制 —— 实现存在，验收需要重启。**风险：启动期接管失败可能让机器开不了机。** |
| **已判定不做（9 项）** | 见 `TIER_LEDGER.md`，每项都有理由 |
| **已完成（8 项）** | 见 `TIER_LEDGER.md` |

**另外三件本会话新增、不在原台账里的**：

| 项 | 状态 |
|---|---|
| 卸载 4 秒 → 0.046 秒 | ✅ 9.555+9.556 |
| RealGuestMode 能跑（`early-returns` 0→6）| ✅ 9.548 |
| **RealGuestMode 停止** | ✅ **9.573+9.574 已解决，9.577 起是默认** —— 根因是 guest 把承载 VMRUN 帧的栈销毁了（0xD1），以及 tick 循环不让出 CPU（0x101）。判据 `invmrun at unload = 0x0` |

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
