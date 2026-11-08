# 重写启动模型：完整方案

> 2026-11-08。目标：采用参考项目（`D:\ref\Type2-AMD-HV`）的模型，使
> **Windows 当 guest + 可运行时卸载**能够同时成立。

---

## 一、为什么现在的模型做不到

### 1.1 两个模型对照

**现行（桩模型）**

```
Sv206CoreThread
  └── SvVirtualizeC
        └── SvVirtualizeProcessor
              ├── RtlCaptureContext       ← 捕获，但 guest RIP 不用它
              ├── SvPrepareForVirtualization
              └── Sv206LaunchVm ──┐
                                   │
        ┌──────────────────────────┘
        ↓  VMRUN
   guest = Sv206GuestStub
        hlt / mov rax,1 / vmmcall / jmp    ← 永远在自己的循环里
        │
        ↓ 停止时
   jmp rbx → Sv206LeaveToHost(ret) → PsTerminateSystemThread
```

**问题**：桩从不回到 `SvVirtualizeProcessor`，所以那条 `if (installed) return`
**永远不执行**（实测 `entries=6, early=0`）。C 帧从 VMRUN 起就永久悬挂。

**参考项目（guest 接管模型）**

```
SvmVirtualizeCpu            ← DPC 在该核上调用
  ├── RtlCaptureContext(&StartContext)
  ├── if (Virtualized) return;          ← ★ 第二次进入走这里
  ├── SvmiSetupGuest(..., &StartContext)  ← guest RIP = 捕获点（函数内部）
  ├── Virtualized = TRUE
  └── AsmSvmLaunchVm ──┐
                        ↓ VMRUN
   guest = 真实 Windows 线程，但从 RtlCaptureContext 的【返回点】继续
        │
        ↓ 于是落到 `if (Virtualized) return` → 正常返回 → C 帧展开
```

**关键**：guest 恢复后**执行的是 `SvmVirtualizeCpu` 自己的返回路径**，
所以 C 帧被正常展开。**这才是 iretq 安全的真正原因。**

### 1.2 我们的 RealGuestMode 为什么放弃了这条路

```c
VpData->GuestVmcb.StateSaveArea.Rip = ContextRecord->Rip;   // Windows 线程的 PC
```

**`ContextRecord` 来自 `RtlCaptureContext`，但在 `SvVirtualizeProcessor` 里用的是
`ContextRecord->Rip`，那是**捕获点之后 caller 的地址**，不是函数内部。

而参考项目直接把 `captured.Rip`（= `RtlCaptureContext` 的返回地址，在函数体内）当 guest RIP。

**一行之差，模型完全不同。**

---

## 二、要改什么（精确到函数）

### 改动 1：guest RIP 改用捕获点，而不是 Windows 线程 PC

```c
// 现在（RealGuestMode，9.539）
VpData->GuestVmcb.StateSaveArea.Rip = ContextRecord->Rip;

// 改为（参考项目方式）
// ContextRecord 已经是 RtlCaptureContext 的输出，直接用它的 Rip/Rsp
// 不需要"Windows 线程的 PC"——那正是让它回不来的原因
VpData->GuestVmcb.StateSaveArea.Rip = ContextRecord->Rip;   // ← 语义变了
VpData->GuestVmcb.StateSaveArea.Rsp = ContextRecord->Rsp;
```

**表面上代码一样，语义完全不同：**
- 9.539 时 `ContextRecord` 的 `Rip` 被理解成"Windows 线程的 PC"
- 参考项目里它是"`RtlCaptureContext` 返回后要执行的那条指令"

**实际上 9.539 的代码可能已经是对的** —— 问题在别处：**`Virtualized` 标志没有被正确使用。**

### 改动 2：加 `Virtualized` 标志并让它真的生效

```c
// SvVirtualizeProcessor 开头
static volatile LONG g_S206Virtualized = 0;

NTSTATUS SvVirtualizeProcessor(PVOID Context)
{
    ...
    RtlCaptureContext(contextRecord);

    if (InterlockedCompareExchange(&g_S206Virtualized, 0, 0) != 0)
    {
        // ★ 第二次进入：guest 已经接管并执行到这里
        return STATUS_SUCCESS;
    }

    // 第一次
    SvPrepareForVirtualization(vpData, sharedVpData, contextRecord);
    InterlockedExchange(&g_S206Virtualized, 1);
    Sv206LaunchVm(...);
    ...
}
```

**现在用的是 `SvIsSimpleSvmHypervisorInstalled()`（读 CPUID）**，那是"检测 hypervisor 是否安装"，
**不是"这是我第几次进入"**。两者在桩模式下效果一样（桩不走这条路），
但在 guest 接管模式下必须换成显式标志。

### 改动 3：启动用 DPC 扇出（不是核心线程）

```c
// 现在：PsCreateSystemThread 每核一个线程，各自 SvVirtualizeProcessor
// 改为：KeInitializeDpc + KeInsertQueueDpc 到每核
//        DPC 里 RtlCaptureContext + VMRUN
```

**为什么要改**：DPC 在目标核的**当前线程上下文**里执行，
`RtlCaptureContext` 捕获的就是那个线程的现场 —— guest 接管后
**恢复的是那个线程**，语义完整。

而 `PsCreateSystemThread` 创建的是**专用线程**，它没有"业务上下文"可接管。

### 改动 4：停止用 DPC 扇出 + 主动 vmmcall

```c
// 现在：设标志，等 guest 走到 vmmcall
// 改为：每核 DPC 里 AsmSvmVmmcall(HvCallDevirtualize)
```

### 改动 5：退出用 iretq（此时才安全）

因为改动 2+3 之后，C 帧会被 guest 正常展开，栈是干净的。

---

## 三、分阶段实施（每阶段可独立验证）

| 阶段 | 内容 | 风险 | 验证 |
|---|---|---|---|
| **A** | 加 `g_S206Virtualized` 标志（不改行为，只用它替代 CPUID 检测）| 低 | 桩模式仍 `entries>0`，`early` 仍为 0（因为桩不回来）|
| **B** | 加 DPC 扇出的虚拟化路径（新函数，不动现有线程路径）| 中 | 新路径能启动，桩模式不受影响 |
| **C** | guest RIP 用捕获点 + `Virtualized` 早退（在 B 的路径上）| **高** | `early-returns` 开始增长 |
| **D** | 停止走 DPC + vmmcall | 中 | `sc stop` 能返回 |
| **E** | 退出换 iretq | **高** | `invmrun=0`，无蓝屏 |

**关键里程碑是 C**：`virt-counts` 的 `early-returns` 一旦开始增长，
说明模型真的成立；在那之前做 D/E 是没有意义的。

---

## 四、诚实的风险

1. **这是一次架构级重写**，不是打补丁。参考项目是完整的 hypervisor 项目，
   我们是在既有 5000+ 行上改造。
2. **A–E 每一阶段都可能蓝屏**，而且 C/E 是已知的高危点（已经炸过两次）。
3. **现有卸载路径（9.443–9.445 四次蓝屏换来）可能被放弃**。
   如果新模型失败，回退是 `git revert`，但那意味着之前的稳定性工作白费。
4. **全部工作都在"桩模式仍可用"的前提下进行** —— 每阶段都要跑桩模式回归。

---

## 五、我建议的实施起点

**阶段 A：加 `g_S206Virtualized` 标志。**

理由：
- **零行为改变**（只是把 CPUID 检测换成显式标志）
- **可独立验证**（桩模式 `entries>0, early=0` 应该不变）
- **是后续所有阶段的前提**

**并且它立刻能回答一个问题**：
> 如果把检测换成显式标志，桩模式下的 `early` 还是 0 吗？

如果还是 0，说明"桩不回来"这个结论被独立确认；
如果变成非 0，说明我之前对代码路径的理解有误 —— **那必须在那之前搞清楚**。

---

## 六、当前状态

```
机器      : 正常，bin = 部署 = 8D1065BC95519F4E
桩模式    : 可用（1.2×），stop 1.91s，invmrun=0x0
virt-counts: entries=6  early-returns=0   ← 关键诊断量
提交      : 157e563
```
