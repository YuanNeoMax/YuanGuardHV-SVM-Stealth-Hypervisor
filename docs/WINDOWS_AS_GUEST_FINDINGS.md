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

## 五、唯一剩下的方向（未验证）

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
