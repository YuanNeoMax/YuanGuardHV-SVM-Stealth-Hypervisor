# 9.543 第 3 步诊断：早退路径**从未被执行过**（entries=6, early=0）

> 2026-11-08。这一步本来是"补上缺失的 Virtualized 早退"。
> **实测发现：代码早就在，但它一次都没跑过。**

---

## 一、我原先的假设，和它为什么错

**假设**：`SvVirtualizeProcessor` 里已有 `RtlCaptureContext` + `if (installed) return` 的早退，
桩模式下 guest 每轮 VMRUN 恢复时都会重新进入这个函数，所以早退会不停发生。

**实测**：

```
virt-counts: entries=6  early-returns=0
```

**6 次进入，0 次早退。**

---

## 二、为什么早退从未发生

`SvVirtualizeProcessor` 的结构（5595–5605 行）：

```c
RtlCaptureContext(contextRecord);

if (SvIsSimpleSvmHypervisorInstalled() == FALSE)   // 读 CPUID 40000000h
{
    ... SvPrepareForVirtualization ...
    Sv206LaunchVm(...);     // VMRUN，正常不返回
}

return STATUS_SUCCESS;      // 第二次进入才走这里
```

**早退要求：guest 恢复后重新执行到 `SvVirtualizeProcessor`。**

但我们的 guest 是 `Sv206GuestStub`，它执行的是：

```asm
Sv206Spin:
    hlt
    mov rax, 1
    vmmcall
    jmp Sv206Spin        ← 回到自己的循环，不回 C 函数
```

**桩永远在 guest 里循环，从不回到 `SvVirtualizeProcessor`。**
停止时走 `jmp rbx` → `Sv206LeaveToHost`（一个 `ret`）→ `PsTerminateSystemThread`。

**所以"第二次进入"这件事，在桩模式下从不发生。**

`entries=6` 是 6 个核各进入一次，之后就再也没有了。

---

## 三、这解释了 0x139

参考项目的 `iretq` 之所以安全，是因为它依赖那条早退：

```
第一次进入 → RtlCaptureContext → VMRUN
guest 恢复 = 从 RtlCaptureContext 的返回点继续 → 落到 `if (installed)` → 直接 return
```

**那个 `return` 会正常展开 C 帧**，所以之后 iretq 跳走时，
栈上是干净的（该返回的已经返回了）。

**我们没有这条路**：桩不回来，C 帧从 VMRUN 那一刻起就永远挂在那里。
`iretq` 一跳出去，就跳进了一个"没人会再展开"的栈 —— **`0x139`（stack cookie 检查失败）正是这个。**

---

## 四、所以第 3 步真正要做的不是"补代码"

代码已经有了，缺的是**让 guest 回到那条路**。

而这恰恰是 RealGuestMode 才能提供的：
**真实 Windows 线程会继续执行，其中某个时刻可能走到 `SvVirtualizeProcessor`？**

**不会。** Windows 线程根本不知道这个函数存在。

参考项目能做，是因为它的 guest **就是**那个 C 函数自己 ——
`RtlCaptureContext` 捕获的 RIP 就在 `SvVirtualizeProcessor` 里面，
guest 恢复后当然回到那里。

**我们的 RealGuestMode 用的是 `ContextRecord->Rip`（Windows 线程的 RIP），
不是 `SvVirtualizeProcessor` 内部的地址。**

**这就是根本差异，而且是可以修的**：

| | 参考项目 | 我们（RealGuestMode）| 应该改成 |
|---|---|---|---|
| guest RIP 来源 | `RtlCaptureContext` 的**返回值**（在函数内部）| `ContextRecord->Rip`（Windows 线程）| **同参考项目** |

**用 `RtlCaptureContext` 而不是 `ContextRecord`。**

---

## 五、这才是第 3 步的正确内容

```c
// 现在（RealGuestMode）
VpData->GuestVmcb.StateSaveArea.Rip = ContextRecord->Rip;   // Windows 线程的 PC

// 应该（参考项目方式）
CONTEXT captured;
RtlCaptureContext(&captured);
VpData->GuestVmcb.StateSaveArea.Rip = captured.Rip;         // 本函数内部！
// ... VMRUN ...
// guest 恢复 -> 落在 RtlCaptureContext 之后 -> if (installed) -> return
```

**这样 guest 就在"虚拟化函数自己"里跑，而不是在 Windows 线程里跑。**

**但这也意味着：Windows 不在 guest 态了。** 回到桩的语义 ——
所以这条路**并不能实现"Windows 当 guest"**，它只是让 iretq 能安全退出。

---

## 六、诚实的总结

**`entries=6, early=0` 这个读数，比之前三轮蓝屏加起来更有价值**，因为它指出：

1. **"参考项目有早退机制，所以我也能有"是个错误推理** ——
   它的早退能成立，是因为它的 guest **就是那个函数**；我们的 guest 不是。
2. **RealGuestMode 用 `ContextRecord->Rip` 的那一刻，就已经放弃了早退这条路。**
3. **要 iretq 能退出，必须把 guest 放回 `SvVirtualizeProcessor` 内部** ——
   但那样 Windows 就不在 guest 态了，与 RealGuestMode 的目标矛盾。

**所以：RealGuestMode（Windows 当 guest）+ 运行时卸载，这两个目标在现有结构下是互斥的。**

---

## 七、当前状态

```
bin = 部署 = 8D1065BC95519F4E
virt-counts: entries=6  early-returns=0   ← 诊断量已就位，可随时复测
RUNNING / degraded=0 / stop 1.91s / invmrun=0x0
新命令: virt-counts (IOCTL 0x836, 只读)
parity: C=55 PS=55 Java=55，客户端 78 条
```

**桩模式完全可用，未受影响。**
