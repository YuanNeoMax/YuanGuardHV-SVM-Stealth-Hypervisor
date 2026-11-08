# Windows-as-Guest：全部结论（截至 2026-11-08）

> 本文是 RealGuestMode 研究的**唯一权威总结**。
> 过程记录在 `archive/2026-11-08/`，此处只留结论。

---

## 一、一句话结论

> **真实 Windows 在 guest 态运行在全部 12 个逻辑核上，是可行的、稳定的、可以干净停止的 ——
> 而且它现在是默认配置。**
>
> ```
> 12 线程吞吐 : 1.01-1.08x（基线 794-804 ms，加载后 799-838 ms）
> G25 功能    : 17/17 ALL PASS
> 卸载        : 0.07 s，invmrun at unload = 0x0
> CPU 占用    : 21-29%
> ```
>
> **停止问题在 9.573-9.574 解决**，根因是两个独立缺陷（见第五节）：
> guest 把承载 VMRUN 帧的内核栈销毁了（`0xD1`），以及 tick 循环不让出 CPU（`0x101`）。
> 9.552 的编译期封禁已于 9.577 解除 —— 现在注册表 `RealGuestMode` 是**禁用开关**。

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

### 5.6 现在仍然不要做的事

> 这一节在 9.572 时期写的是「不要把默认 CoreCount 改成 12」「RealGuestMode 仍是 opt-in」——
> **两条都已被 9.575–9.577 推翻并实施。** 下面是与当前代码一致的版本。

- **不要为了让性能好看而把 `CoreCount` 调小。** 9.575 实测：全核真 guest 1.01–1.08×，
  6 核桩模式 1.13–1.34×。**限核在任何配置下都不更快**，而且 9.576 证明 6 核时
  G25 只有 15/17（T7/T9 FAIL，判据进程被漏到 native 核上）。
- **不要在真 guest 里加回自旋循环。** tick 循环必须让出处理器
  （`KeDelayExecutionThread`）。9.574 的 `0x101 CLOCK_WATCHDOG` 就是自旋导致的：
  12/12 虚拟化时 Windows 没有逻辑核处理时钟中断。CPU 100% → 21–29% 是这个改动换来的。
- **不要把 `terminateFn` 放回真 guest 路径。** 9.573 的 `0xD1` 就是它：
  guest 里终止线程会销毁承载 VMRUN 帧的内核栈，判据是 `invmrun at unload = 0x6`。
- **不要用签名状态判断镜像完整性。** 自签根不在受信任根，`Get-AuthenticodeSignature`
  会报 `UnknownError`，那是正常表现。用 SHA256。

### 5.7 历史：9.569–9.572 的失败（已被 9.573 取代，保留为教训）

> 这一段记录的是**同一条路的另一次尝试**，它在 9.572 失败后被钉回去，
> 随后被 9.573–9.574 用完全不同的修法解决。**结论已被取代，教训仍然有效。**

9.569 打开闸门时，理由有两条，**两条本身都成立**：

1. 停止路径有了可检查的契约（`tests/stub_stack_contract.ps1`：桩不许动 RSP）；
2. 一个**从未被虚拟化**的物理核可以按物理地址把每个 VMCB 的 `EFER.SVME` 清零
   （`yghv_s206_force_devirtualize()`）。

| 时刻 | 动作 | 结果 |
|---|---|---|
| 10:30 | 桩模式加载/卸载（带新的强制反虚拟化代码） | 通过：卸载 0.177 s、`invmrun=0x0` |
| 10:31 | `RealGuestMode=1` + `sc start` | 通过：`entries=6 / early-returns=6`，稳定 10 s |
| 10:32 | `sc stop` | **蓝屏 `0xD1`**：`AV_CODE_AV_STACKPTR_ERROR_PAGED_IP yghv_pa+0x285ee` |
| 之后 | 两次引导 | **两次硬冻结** |

**判据给出的答案**：

```
9.568 桩模式卸载   : invmrun at unload = 0x0
9.572 真guest卸载  : invmrun at unload = 0x6
```

当时把这个 `0x6` 读成「`SVME=0` 是陷阱条件，不是唤醒」——**那个解读只对了一半**。
它确实解释了为什么强制反虚拟化没用，但没有问出「为什么核会需要一个唤醒」。
9.573 问出了那个问题：**因为承载 VMRUN 帧的栈已经被 guest 自己销毁了。**

**留在这里的教训（比结论更值钱）**：

> **两次解开闸门时，所有检查都是绿的。**
> 第一次绿在 `stub_stack_contract`，第二次绿在强制反虚拟化 ——
> **而失败都发生在检查之外。**
>
> 所以「这次检查更全了」不是重新打开一条危险路径的理由。
> 真正的理由只能是：**上一次失败的那个具体机制，这次被单独解释了。**

### 5.8 历史：9.567 的影子实验计划（部分已落地，其余作废）

9.567 曾把「从 C 跳到桩」拆成两个可离线验证的量：

| 需要成立 | 怎么验 | 现状 |
|---|---|---|
| 桩的**运行期地址**正确 | 用 `lea`，**不用** `offset`（PECOFF 重定位会把后者变成链接期常量）| **作废** —— 9.573 的修法不需要跳转 |
| 跳到桩时 **RSP 仍是线程真栈** | 桩内不碰栈 → 可静态查 | **已落地**：`tests/stub_stack_contract.ps1` |

**第二条留下来并成为构建期规则**，因为它把 9.444 那次最贵的蓝屏
（`0x1E`，#GP 在 `KiSwapContext`，根因是外来栈上做上下文切换）变成了机器会拦的规则。

第一条随 9.573 作废：真 guest 的修法**不是**跳进桩，而是让 guest 自己的
`Sv206CoreThread` 尾部不再终止线程 —— 桩始终是桩模式的 guest，两条路径不必汇合。

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

## 七、当前状态（默认开启）

```
RealGuestMode : 默认 ON
  实现：yghv_s206_real_guest_mode() 默认返回 1
  关闭方法：注册表 RealGuestMode = 非零（语义是「禁用」，9.577 起反转）
  编译期钉死：build with YGHV_REAL_GUEST_PINNED=1
CoreCount     : 默认 = 全部处理器（12）
  限制方法：注册表 CoreCount = N
```

**默认配置实测**（注册表清空）：

```
virt-counts : entries=12  early-returns=12  cores-marked=12  disagrees=0
sys-health  : degraded=0 total=0 fatal=0
卸载        : 0.07 s，invmrun at unload = 0x0
```

**为什么可以默认开启**：9.575 测出它比旧默认（6 核桩模式）在**性能和功能上都更好**
（1.05× vs 1.13–1.34×；G25 17/17 vs 15/17）。没有任何配置让旧默认更优。

> ⚠️ **不要用签名状态判断镜像完整性。** 自签根不在受信任根，
> `Get-AuthenticodeSignature` 报 `UnknownError` 是正常表现。用 SHA256。

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
