# Windows-as-Guest：全部结论（截至 2026-11-08）

> 本文是 RealGuestMode 研究的**唯一权威总结**。
> 过程记录在 `archive/2026-11-08/`，此处只留结论。

---

## 一、一句话结论

> **真实 Windows 在 guest 态运行，在本机（AMD Ryzen 5 5500 / Zen 3）是可行的 ——
> 实测启动成功、稳定运行、`early-returns` 0→6、比桩模式快 38%。
> 但它无法在运行时干净停止，因此当前编译期封禁（`9.552`）。**

---

## 二、已验证的事实

### 2.1 能跑（这是真的）

```
RealGuestMode=1 实测（9.539 / 9.548 / 9.546）:
  sc start        : 0.18 秒，STATE = RUNNING
  30 秒观察       : 全程 RUNNING
  CPU             : 51-60%（不是 100%）
  exit-codes      : 1 个非零码，7,686 次退出
  sys-health      : degraded=0 total=0 fatal=0
  12 线程吞吐     : 1.933 秒（桩模式 3.104 秒，快 38%）
  early-returns   : 6 / 6 核  ← 关键
```

### 2.2 `early-returns` 0→6 的含义

这是**参考项目模型成立的证明**：

| 模式 | entries | early-returns |
|---|---|---|
| 桩模式 | 6 | **0**（桩不回到 C 函数）|
| RealGuestMode | 6 | **6**（guest 回到 `SvVirtualizeProcessor`）|

**6 个核全部走了早退分支** —— guest 恢复后回到 `SvVirtualizeProcessor`，
C 帧沿正常路径展开。**这正是参考项目 `iretq` 能安全工作的前提。**

### 2.3 关键配置对照（与参考项目 Type2-AMD-HV）

| 项 | 参考项目 | 我们 |
|---|---|---|
| `InterceptMisc2` | `VMRUN \| VMMCALL` | ✅ 一致 |
| `InterceptMisc1` | 只有 `MSR_PROT` | ✅ 不含 INTR/NMI |
| guest RIP 来源 | `RtlCaptureContext` 的输出 | ✅ 同 |
| MachineFrame | 宿主栈 +0x198（编译期偏移）| ⚠️ 已有槽位（9.542），未使用 |
| 停止触发 | DPC 扇出 + 主动 `vmmcall` | ❌ 缺 |
| 退出 | `iretq` | ❌ 仍用 `jmp rbx` |

**拦截位从一开始就一致 —— 差别在 guest 上下文和执行流。**

---

## 三、为什么停不下来（根因，已确定）

### 3.1 停止握手依赖 guest 执行 `vmmcall`

`main.c` 的 `DriverUnload` 等待 `g_S206CoresExited == 核数`。
这个计数由 **`Sv206LeaveToHost`**（ASM）递增 —— 而它只在
**`SvLV20` 出口被调用时**才执行。

**桩模式**：桩执行 `vmmcall` → `#VMEXIT` → handler 看到停止标志 → `ExitVm`
→ `SvLV20` → `Sv206LeaveToHost` ✅

**RealGuestMode**：guest 是 `SvVirtualizeProcessor` 的早退轨迹 ——
它跑完 C 函数的 `return`，然后执行 `Sv206CoreThread` 尾部
**包括 `PsTerminateSystemThread`**，全部发生在 **guest 模式里**。

**`vmrun` 的宿主帧从未恢复。**

### 3.2 崩溃形态（dump 证实）

```
0xD1  P1=P4=0x...80CE  P3=8（取指失败）
Sv206LaunchVm+0x1E = vmsave  ← vmrun 返回后的第一条指令
```

**`vmrun` 返回了，但取不到 `vmsave`** —— 因为镜像已被 unmap，
那个核还停在 guest 里。

**9.462 的注释早就写过同一个崩溃机制**（那时是"桩停在被踢醒之前"）。

### 3.3 为什么 DPC 方案不成立（9.550 验证）

参考项目的 `SvmTerminateCpu` 能在核上执行 `vmmcall`，是因为：

| 条件 | 参考项目 | 我们 |
|---|---|---|
| 核上运行的是**我们自己的代码** | ✅ DPC 就是 | ❌ guest 里跑 Windows 代码 |
| `vmrun` 后有 host 执行流接收返回 | ✅ `AsmSvmLaunchVm` 后就是 `SvmTerminateCpu` | ❌ **从未恢复** |

**结构上缺一个 host 执行流。**

---

## 四、已排除的解法（不要再试）

| 方案 | 版本 | 失败原因 |
|---|---|---|
| 在 `Sv206CoreThread` 尾部 tick 一次 `vmmcall` | 9.551 | **时机错**：那时停止标志还没置位，VMEXIT 后继续 vmrun，反而让崩溃提前 |
| DPC 扇出主动 `vmmcall` | 9.550（分析）| **结构上不成立**：缺 host 执行流 |
| `iretq` + 全局变量帧 | 9.540 | `0x139`：`lea rsp, <global>` 抛弃了宿主栈，C 帧无人展开 |
| 给 `HostStackLayout` 加字段放帧 | 9.541 | `0x1E`/取指失败：`DECLSPEC_ALIGN(PAGE_SIZE)` 让 union 涨一页，所有汇编硬编码偏移失效 |
| 在 `SvHandleVmExit` 首行加 interlocked | 9.546 | **诊断代码本身成了故障源**：RealGuestMode 启动即崩 |

---

## 五、★ 全核 RealGuestMode 打通了（9.573–9.574）

> **结论先给：12 个逻辑核全部以真 guest 运行，能加载、能稳定、能干净卸载。**
> 判据：`entries=12 / early-returns=12`、`invmrun at unload = 0x0`、卸载 0.334 s / 0.069 s。

### 5.1 两次失败，两个完全不同的原因

前面七次蓝屏被归成一个原因（"结构上缺一个 host 执行流"），**那是错的**。
把它们分开看，是两个独立缺陷，而且各自都有确定的修法：

| # | 现象 | 真正的原因 | 修法 |
|---|---|---|---|
| 1–4 次 | `0xD1` @ `Sv206LaunchVm+0x1E` | 见下 5.2 | — |
| 5–6 次 | 同上 | 同上 | **9.573** |
| 7 次 | `0x101 CLOCK_WATCHDOG` @ CPU4 | **tick 循环不让出 CPU** | **9.574** |

### 5.2 缺陷一：guest 把承载 VMRUN 帧的那张栈销毁了（9.573）

RealGuestMode 下 `SvVirtualizeProcessor` 用 `RtlCaptureContext` 把 guest RIP
设在函数内部，guest 从那里继续 → 走「第二遍」分支 → `return` → 一路展开到
`Sv206CoreThread` 尾部 → **`terminateFn(STATUS_SUCCESS)`**。

**这些全部在 guest 里执行。** 于是：

```
Sv206LaunchVm 的 VMRUN 帧  仍然挂在那张栈上
调度器                     把那张栈回收了
帧                         变成野指针
卸载时                     vmsave 从死页取指 -> 0xD1
```

**判据（决定性）**：

```
桩模式    : invmrun at unload = 0x0
真 guest  : invmrun at unload = 0x6      <- 六个孤儿帧
```

`0x6` 从来不是"停止信号太弱"，是**六个核的栈之记录已经不存在了**。
9.569 的强制反虚拟化救不了它 —— 清 SVME 只能弹出一个**正要离开**的核，
不可能复活一张已经释放的栈。

**修法**：真 guest 尾部改成 tick 循环，**永不返回**。线程活着 → 栈有效 → 停止握手有 exit 可答。

### 5.3 缺陷二：让出 CPU，而不是占用它（9.574）

9.573 的循环写成了 `for(;;) { spin 200000; vmmcall; }` —— **一个永不让出的忙线程**。
同一个构建、两种配置，读数把原因钉死了：

| 配置 | 结果 |
|---|---|
| 6 核（默认） | `entries=6/6`，稳定 23 s，卸载 **0.04–0.05 s**，`invmrun=0x0`，**两轮通过** |
| 12 核（`CoreCount=12`） | `entries=12/12`，RUNNING，**~20 s 后 `0x101`** |

dump 说明的是**饥饿的形状**：

```
FAULTING_PROCESSOR: 4
FAILURE_BUCKET_ID: CLOCK_WATCHDOG_TIMEOUT_INTERRUPTS_DISABLED_nt!KiSystemCall64
```

一个处理器停在关中断的内核路径上，再也没回来取时钟中断。

**这不是 hypervisor 缺陷，是调度账算错了**：每个被虚拟化的核上有一个内核线程 100% 烧着。
6/12 时还剩 6 个逻辑 CPU 给 Windows 处理时钟，撑得住；**12/12 时一个不剩，watchdog 就响了。**

**修法**：循环里**阻塞**而不是自旋 —— `KeDelayExecutionThread(KernelMode, FALSE, -10000)` = 1 ms。
这个原语**同一位置的 fallback 循环本来就在用**，所以在这个代码点上阻塞是既有做法，不是新发明。

实测 CPU 占用：

```
9.573（自旋）: 100%（12 核全占满 -> 0x101）
9.574（阻塞）: 21–29%
```

### 5.4 最终实测（`C964377579999E02`）

```
stub 基线     : 卸载 0.051 s，invmrun=0x0        <- 未回归
6 核真 guest  : entries=6/6，20 s 稳定，卸载 0.055 s，invmrun=0x0
12 核真 guest : entries=12/12，稳定 >30 s，卸载 0.334 s，invmrun=0x0   <- 第 1 轮
12 核真 guest : entries=12/12，稳定 >30 s，卸载 0.069 s，invmrun=0x0   <- 第 2 轮
CPU 占用      : 21–29%（自旋版是 100%）
```

### 5.5 ★ 性能与功能：全核真 guest 现在是**两项都最优**的配置

两轮实测（9.575 性能 / 9.576 功能），把困扰这个项目最久的取舍推翻了。

| 配置 | 12 线程吞吐 | G25 功能 |
|---|---|---|
| 基线（驱动停） | 794–804 ms = **1.00×** | — |
| 6 核桩模式（**旧默认**）| 904–1070 ms = **1.13–1.34×** | **15/17**（T7/T9 FAIL）|
| 12 核桩模式 | 3211–3505 ms = **4.0–4.4×** | — |
| 6 核真 guest | 815–875 ms = 1.03–1.09× | — |
| **12 核真 guest** | **799–838 ms = 1.01–1.05×** | **17/17 ALL PASS** |

**旧的默认值依据是「6 核 1.2×（T7 FAIL）/ 11 核 ≈4×（T7 PASS）/ 12 核 11.6×」，**
即「要功能就得牺牲性能」。那个 11.6× 是**桩模式**的数字。

**为什么真 guest 几乎零开销**（判据）：

```
桩模式 12 核   : VMMCALL 退出 1,255,119 次
真 guest 12 核 : VMMCALL 退出    27,914 次     <- 45 倍
handler 单次   : 1,394 ticks ≈ 0.4 us，全核总 CPU 时间 0.012 s
```

**开销不在 hypervisor，在桩自己的 `hlt/vmmcall/jmp` 空转循环。**
真 guest 的 guest 是 Windows 自己，只在真正需要拦截的地方退出。
所以「减核换性能」从来不是在优化 hypervisor，是在减少桩的空转。

**T7 的翻转最能说明问题**：6 核时判据进程被漏到 native 核上，读到真实值
`0xC4C40000000000`；全核真 guest 下读到 `value=0x0`，DECOY 真实生效。

### 5.6 仍然不要做的事

- **不要把默认 `CoreCount` 改成 12。** 全核跑通≠全核划算：9.527/9.537 测过
  12 核 12 线程负载代价 **11.6×**，6 核是 **1.2×**。全核现在是**可选模式**，
  注册表 `CoreCount=12` 才开，默认仍是物理核数。
- **不要把这套开关做成默认开启。** `RealGuestMode` 仍是注册表 opt-in（默认 0），
  编译期 `YGHV_REAL_GUEST_PINNED=1` 仍能一键钉死。
- **性能未测。** 本轮只证明了"能跑、能停、不蓝屏"，**没有测吞吐**。
  9.539 记录的"快 38%"是 6 核配置下的旧读数，全核下的数字现在**不存在**，不要引用。



**先给结论：这条路在 9.572 又被否掉了，而且这次否得比以前彻底。**

### 5.0 9.569 为什么敢再试，以及 9.572 看到什么

9.569 把闸门重新打开，理由有两条，**两条本身都成立**：

1. 停止路径有了可检查的契约（`tests/stub_stack_contract.ps1`：桩不许动 RSP）；
2. 一个**从未被虚拟化**的物理核可以按物理地址把每个 VMCB 的 `EFER.SVME` 清零
   （`yghv_s206_force_devirtualize()`）。

实测记录，按时间顺序：

| 时刻 | 动作 | 结果 |
|---|---|---|
| 10:30 | 桩模式加载/卸载（带新的强制反虚拟化代码） | **通过**：RUNNING、`sys-health` 干净、卸载 **0.177 s**、`invmrun=0x0` |
| 10:31 | `reg add RealGuestMode=1` + `sc start` | **通过**：0.24 s 进 RUNNING，`entries=6 / early-returns=6`，`sys-health` 干净，稳定 10 s |
| 10:32 | `sc stop` | **蓝屏 `0x1D`**：P1=P4=`fffff805644885ee`，P2=`0xe`，P3=`8`（取指失败）<br>`FAILURE_BUCKET: AV_CODE_AV_STACKPTR_ERROR_PAGED_IP yghv_pa+0x285ee` |
| 之后 | 两次引导 | **两次硬冻结** |

**前四次的每一项防护都在，第五、六次仍然发生，而且落点与前四次同一个地方：卸载，驱动代码页。**

### 5.1 判据给的答案：`invmrun` 这次不是 0

```
9.568 桩模式卸载: s206 invmrun at unload = 0x0000000000000000
9.572 真guest卸载: s206 invmrun at unload = 0x0000000000000006
```

**六个核在 DriverUnload 继续走的时候还在 VMRUN 里。**
`yghv_s206_force_devirtualize()` 把六个 VMCB 的 SVME 都清了，核**仍然没有回来**。

### 5.2 结构性结论（这一次能精确说清楚了）

> **`SVME=0` 是一个陷阱条件，不是一个唤醒。**
> 它只会在核**下一次 #VMEXIT** 时把核弹出去；它不能驱逐一个不打算退出的核。

而 guest 跑的是**任意 Windows 线程的代码** —— 它可以很久不产生一次我们拦截的退出，
也可以永远不产生（例如它自己进了 `hlt` 且没有我们拦截的中断）。

**所以缺的从来不是"host 执行流"（9.550 的说法），也不是"更强的停止信号"。**
缺的是：**一个在被我们无法控制的核上、保证会被执行的退出点。** 本树目前没有。

### 5.3 处置：钉回去，而且钉得比 9.552 硬

```
yghv_s206_real_guest_mode()  ->  return 0;   （无条件）
yghv_s206_force_devirtualize() 的调用   ->  #if YGHV_REAL_GUEST_UNPINNED
```

**两条一起开、一起关。** 一个只为某个模式服务的救援路径，不该在那个模式关掉之后
还在每次卸载上付一次物理读写。

**不要因为"这一次检查更全了"再解开它。** 两次解开时检查都是绿的：
第一次绿在 `stub_stack_contract`，第二次绿在强制反虚拟化 —— **而失败都发生在检查之外。**

### 5.4 如果将来真要再碰，先满足这一条

**必须有一个"保证会执行"的退出点**，候选只有两类，且都要先离线证明：

| 候选 | 要证明什么 | 现状 |
|---|---|---|
| 拦截**每一条** guest 会走的退出 | 代价是全拦截，性能与正确性都要重测 | 未做 |
| 让 guest **不能**长时间不退出（例如拦截 `hlt`/`sti`） | 与桩模式的 `hlt` 设计直接冲突 | 未做 |

在这条被满足之前，本文件第五节应读作：**此路已封，理由是可复现的。**


**让 guest 在早退后 `jmp` 进 `Sv206GuestStub`，而不是走完 C 函数终止线程。**

```
guest 完成 SvVirtualizeProcessor 的早退（C 帧展开 ✅）
  → 不 terminateFn
  → jmp Sv206GuestStub
  → 进入桩循环，开始 tick
  → 停止时握手可用（和桩模式一模一样）
```

**为什么可能成立**：两个要求同时满足 —— C 帧展开 ✅ + 停止握手可用 ✅。

**风险**：
- 需要从 guest 态的 C 代码跳到汇编桩
- `Sv206CoreThread` 的帧会留在栈上
- **这条路上已经四次蓝屏**

**若要尝试，必须先解决**：如何在不加载驱动的情况下验证"从 C 跳到桩"这个动作。

### 5.1 9.567 的拆解：这一步不是"再试一次"，而是一个可先离线验证的跳转

之前四次蓝屏都把"从 C 跳到桩"和"整机虚拟化"捆在一起做 —— 一旦错了，
**唯一读数就是蓝屏码，拿不到"跳转本身对不对"的信息。**

拆开看，这个动作其实只有两个量需要正确，而且两个都能算：

| 需要成立 | 现在的事实 | 怎么验 |
|---|---|---|
| `Sv206GuestStub` 的**运行期地址** | 桩在 ASM 里，C 只能拿到链接期地址 | 用 `x64` 的 `lea rax, Sv206GuestStub` 取，**不用** `offset`（PECOFF 重定位会把 `offset` 变成链接期常量） |
| 跳到桩时 **RSP 必须指向线程自己的真栈** | 9.444 的第四台机器的教训：**外来栈上不能做上下文切换** | 桩里**不碰栈**（`hlt` / `vmmcall` / `jmp` 都是无栈指令），RSP 全程保持进去时的值 |

**桩本来就不碰栈** —— 这一点写在 `Sv206GuestStub` 的注释里（"the stub never touches
the stack ... that value is never dereferenced"）。所以第二个条件在**桩侧已经成立**，
剩下的只有第一个：**C 侧怎么把桩的地址拿到手，并且保证链接器不会把它变成错的常量。**

### 5.2 影子实验：先把地址与栈算出来，不加载驱动

三条判据，全部只读、全部可在**不启动驱动**的情况下取得：

1. **地址是运行期的。** 构建后从 `.obj`/`.sys` 的符号表读出 `Sv206GuestStub` 的
   RVA，同时用 `lea` 版本编译一次，确认 `lea` 形式在反汇编里是
   `lea rax, [rip+disp]` 而不是 `mov rax, imm64`。
   —— **`mov rax, imm64` 就是重定位没生效的样子，跳过去必崩。**
2. **栈指针在跳转点上没有变化。** 在 `Sv206CoreThread` 里，调用 `Sv206VirtualizeC`
   之前的 RSP 与"早退返回之后"的 RSP 必须是同一个值；这一条可以在**桩模式下**
   （`RealGuestMode=0`，已实测稳定）加一次只读打印验证，不需要真 guest。
3. **桩对 RSP 的假设是可查的。** `hv/*.asm` 里桩体内不得出现
   `push` / `pop` / `call` / `ret` / `sub rsp` / `add rsp` —— 这条可以做成**静态检查器**
   （和 `exit_rip_contract` 同形状：按 `proc` 边界切片，扫指令）。

**第 3 条是这一轮唯一确定要交付的东西**：它是唯一一条"不需要实机就能失败"的判据，
而且它把 9.444 那条最贵的教训（外来栈上做上下文切换 → `0x1E`）变成了**机器会拦的规则**。

> 判据的局限要写明：静态检查只能证明**桩不碰栈**，
> **不能**证明"跳到桩之后停止握手一定能应答" —— 后者要实机。
> 所以这不是"验证了方案可行"，而是"把最贵的那条已知失败先钉死"。

---

## 六、相关的 2026-08 研究（另一条线的结论）

`archive/YGHV_OS_AS_GUEST_SUMMARY_20260815.md` 记录了 114 个实验，
结论是「OS-as-Guest 本机不可行，平台级限制（errata 1363）」。

**本会话推翻了这个结论的适用范围**：

| 8 月的结论 | 本会话实测 |
|---|---|
| 「<10s 硬冻结或 0x101」 | **30 秒稳定，无冻结无蓝屏** |
| 「平台限制，errata 1363」 | **errata 没触发**（中断不拦截，无持续 APIC 流）|
| 「本机不可行」 | **能跑，但不能停** |

**8 月失败的原因是「host ISR 代管中断」，不是「Windows 当 guest」。**
errata 1363 的措辞是 *"Persistent Stream of APIC Register Accesses"* ——
那是 host 转发中断时的必然产物，让 guest 自己处理中断就不会产生。

---

## 七、当前状态（封禁原因）

```
RealGuestMode : 编译期固定关闭
  实现：yghv_s206_real_guest_mode() 第一行 return 0
  验证：编译器优化为 `xor eax,eax ; ret`
        （注册表写入 1 后，驱动仍报 0）
封禁理由：一次会话内 4 次 0xD1 蓝屏 + 机器启动出现 BIOS 引导异常
```

**解除封禁的方法**：删除那个 `return 0`。
**解除前必须知道**：这是重新打开一条已验证会产生蓝屏的路径。

---

## 八、证据索引

| 主题 | 文档（在 `archive/2026-11-08/`）|
|---|---|
| 首次实测成功 | `REALGUEST_EXPERIMENT_20261108.md` |
| 参考项目对照 | `DEVIRTUALIZE_REFERENCE.md` |
| 重构方案（5 阶段）| `REWRITE_STARTUP_MODEL.md`、`REFACTOR_PLAN_RG.md` |
| 阶段 A / A2 | `STAGE_A_RESULT_20261108.md`、`STAGE_A2_RESULT_20261108.md` |
| 阶段 C（早退达成）| `STAGE_C_DIAG_20261108.md`、`STAGE_C_SUCCESS_20261108.md` |
| 阶段 D（否定）| `STAGE_D_BLOCKED_20261108.md`、`STAGE_D_NEGATIVE_20261108.md` |
| 停止握手根因 | `STOP_HANDSHAKE_ROOTCAUSE_20261108.md` |
| `vmsave` 崩溃 | `REALGUEST_D1_ROOTCAUSE_20261108.md` |
| 9.551 失败 | `9_551_FAILURE_ANALYSIS.md` |
| 封禁 | `SAFETY_PIN_20261108.md` |
| `iretq` 失败 | `IRETQ_FAILURE_20261108.md` |
| 加字段失败 | `STEP1_FAILURE_20261108.md`、`STEP1_SUCCESS_20261108.md` |
| 早退不可达诊断 | `STEP3_DIAGNOSIS_20261108.md` |
