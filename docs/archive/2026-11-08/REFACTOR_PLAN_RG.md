# 重构方案：把启动/停止模型改成参考项目的形态

> 2026-11-08。目标：让 RealGuestMode 能干净停止。依据：`D:\ref\Type2-AMD-HV` 源码。

---

## 一、参考项目的模型（读源码得出）

### 1.1 核心是「用 RtlCaptureContext 造一个不会返回的 C 帧」

```c
// svm.c:316-384  SvmVirtualizeCpu
Efer.SvmEnable = 1;  __writemsr(EFER, ...);

StartContext.ContextFlags = CONTEXT_ALL;
RtlCaptureContext(&StartContext);        // ★ 捕获当前 RIP

if (Vcpu->Virtualized)                   // ★ 第二次进入走这里
    return STATUS_SUCCESS;               //   直接返回，不重复 setup

SvmiSetupGuest(Vcore, &StartContext, ...);  // guest RIP = 捕获点之后那条
Vcpu->Virtualized = TRUE;                   // 先进标志

AsmSvmLaunchVm(&Vcore->GuestVmcbPa);    // 进 VMRUN，正常不返回
```

**机制**：
1. 第一次调用：捕获 RIP → 设 guest RIP = 该点 → 置 `Virtualized` → VMRUN
2. VMRUN 把 CPU 切到 guest，guest 从 `RtlCaptureContext` 的**返回地址**继续执行
3. 那个地址就是 `if (Vcpu->Virtualized) return` —— **于是直接返回**
4. 从调用者角度看，`SvmVirtualizeCpu` **正常返回了**，但实际是在 guest 态返回的

**这就是为什么 iretq 不会炸**：C 帧没有被"瞬移"绕过 —— 它是被 guest **接着往下执行**了。

### 1.2 停止时 iretq 回到哪里

```c
// vmexit.c:909-964  SvmiHandleStopHv
Frame->Rip = Control->NRip;   // = vmmcall 之后那条
...
return FALSE;                 // -> asm iretq
```

`iretq` 用的 `NRip` 是 **`HvCallDevirtualize` 那个 `vmmcall` 之后**的地址，
而那个 `vmmcall` 是在 `SvmTerminateCpu` 里、由 DPC 在目标核上执行的：

```c
// svm.c:386-403  SvmTerminateCpu
Req.Code = HvCallDevirtualize;
AsmSvmVmmcall(Req.Value, 0);   // ★ 在该核上主动 vmmcall
```

**所以 iretq 之后，控制流回到 `AsmSvmVmmcall` 的下一行**（即 `SvmTerminateCpu` 里），
**而那时 RSP/CS/SS 已经是 guest 的 —— 但它们在那一刻恰好就是"当前线程的"，
因为这个核上跑的就是被虚拟化的那个线程。**

---

## 二、我们的现状与差距

| 环节 | 参考项目 | 我们 | 差距 |
|---|---|---|---|
| 启动 | DPC → `RtlCaptureContext` → VMRUN，guest 接管 C 帧 | `KeIpiGenericCall` → `Sv206LaunchVm` → **guest 是桩** | ★ 模型不同 |
| guest RIP | 捕获点（真实 Windows 代码） | `Sv206GuestStub` | RealGuestMode 已能切 |
| 停止触发 | **DPC 扇出，每核主动 `vmmcall`** | 设标志等 guest 的 vmmcall | ★ 缺 |
| 退出 | `iretq` + 栈上 MachineFrame | `mov rsp,rcx ; jmp rbx` | ★ 缺 |
| MachineFrame 位置 | **宿主栈 +0x198** | 无（曾误放全局）| ★ 缺 |

---

## 三、重构分四步（每步可独立验证）

### 第 1 步：把 MachineFrame 放进宿主栈（不改行为）

**为什么先做这个**：`iretq` 必须从一个**栈上**的帧弹出，而不是全局变量。
第 2 步的 `iretq` 依赖它。

**做什么**：
1. 在 `SV206_CORE_THREAD_CONTEXT` 或 VPD 里加 `S206_IRET_FRAME IretFrame;`
2. 让 `Sv206LaunchVm` 的 RSP 布局包含它 —— 与参考项目同样做法：
   在 `HostStackLayout` **之前**（更低地址）预留 0x28 字节
3. 不调用 iretq，**行为完全不变**

**验证**：桩模式仍 RUNNING / stop 干净 / invmrun=0。

### 第 2 步：把退出改成 iretq（**这一步有蓝屏风险**）

**关键差异**：参考项目的 `AsmSvmLaunchVm` 从头到尾**没有"被 C 调用然后返回"的路径** ——
它的返回永远走 iretq。我们的 `Sv206LaunchVm` 目前走 `jmp rbx` → thunk → `PsTerminateSystemThread`。

**做法**：
```asm
; 在 SvLV20 之前
    lea     rsp, [rsp + <IretFrameOffset>]   ; 指向栈上的帧
    iretq
```
**注意**：`RSP` 此刻在 `HostStackLayout`，帧必须在它**下方**（更小地址）
或用一个已知的 RSP 偏移 —— 必须精确，这就是第 1 步要先做好的原因。

### 第 3 步：加 `Virtualized` 早退（为 RealGuestMode 准备）

照抄参考项目的模式：
```c
RtlCaptureContext(&StartContext);
if (VpData->RealGuestStarted) return;   // 第二次进入直接返回
Sv206SetupGuest(..., &StartContext, ...);
VpData->RealGuestStarted = TRUE;
Sv206LaunchVm(...);
```

### 第 4 步：DPC 扇出停止

把 `main.c` 里现有的
```c
for (...) { yghv_s206_kick_parked_cores(); spin; }
```
改成参考项目的
```c
KeInitializeDpc + KeInsertQueueDpc 到每个核
    -> DPC 里：AsmSvmVmmcall(HvCallDevirtualize)
```

---

## 四、诚实的风险评估

| 步骤 | 风险 | 已知的坑 |
|---|---|---|
| 1 | **低**（不改行为）| 栈偏移算错 → 覆盖 HostStackLayout |
| 2 | **高（蓝屏）** | 9.443–9.445 四次蓝屏都在这条路上；`0x139` 已见 |
| 3 | **中** | `RtlCaptureContext` 在 DPC 里的上下文 |
| 4 | **中** | DPC 在已虚拟化核上执行 vmmcall |

**第 2 步是真正的赌注。** 我们的 `Sv206LaunchVm` 有 C 调用者，而参考项目的没有。

**在做第 2 步之前，必须先回答一个问题**：
> `iretq` 之后，控制流回到哪里？

- 参考项目：回到 `SvmTerminateCpu` 里的 `AsmSvmVmmcall` 之后（**那个 C 帧还活着**）
- 我们（桩模式）：`jmp rbx` 到 thunk，`call rax` = `PsTerminateSystemThread`

**如果第 2 步只改退出方式而不改第 3/4 步，iretq 会跳到一个没有对应 C 帧的地方 ——
这大概就是 `0x139` 的来源。**

**所以第 2 步必须和第 3/4 步一起设计，或者至少先有第 3 步的早退结构。**

---

## 五、建议的实施顺序（修正）

原计划「1 → 2 → 3 → 4」是错的，第 2 步会孤立地炸。

**正确顺序**：

```
1. MachineFrame 放进宿主栈（低风险，可独立验证）
3. Virtualized 早退（为 RealGuestMode 准备，桩模式不受影响）
4. DPC 扇出停止（新能力，但桩模式仍走原路径）
2. 最后才把退出换成 iretq —— 此时 C 帧结构已经和参考项目一致
```

**每一步都必须：**
- 桩模式完全回归通过（RUNNING / degraded=0 / stop 干净 / invmrun=0）
- 保留前一步的回退路径
- 源码回滚后**立即重建部署**（9.540 的教训）

---

## 六、当前状态

```
机器   : 正常
bin    : 09DAD6C47D430696（桩模式，可用）
提交   : acdfcd3 + 反引号修正
RealGuestMode : 已从注册表移除
```

**下一步：第 1 步（MachineFrame 进栈）。** 低风险，可独立验证。
