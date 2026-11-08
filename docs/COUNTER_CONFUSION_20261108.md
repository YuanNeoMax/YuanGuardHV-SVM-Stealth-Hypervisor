# 9.554 推翻一个基础假设：停止等待从未真正成功过

> 2026-11-08。**这个发现影响对本树所有历史"干净卸载"结论的理解。**

---

## 一、两个计数器，我一直混用

| 计数器 | 谁维护 | 谁读 | 含义 |
|---|---|---|---|
| `g_inVmrunCount` | asm 的 `lock inc/dec`（`Sv206LaunchVm+0x14/0x21`）| `yghv_s206_invmrun_count()` | **当前有几个核在 VMRUN 里** |
| `g_S206CoresExited` | `Sv206CoreThread` 6737 行 | `yghv_s206_vmrun_stopped_count()` | **有几个核永久离开了** |

**我一直在用的判据 `s206 invmrun at unload=0x0` 是第一个。**

**而 `main.c:5123` 的停止循环等的是第二个：**

```c
stopped = yghv_s206_vmrun_stopped_count();      // = g_S206CoresExited
while ((ULONG)stopped < active && waited < 20000) {
    yghv_s206_kick_parked_cores();
    for (spin = 0; spin < 200000UL; spin++) { }
    stopped = yghv_s206_vmrun_stopped_count();
}
```

---

## 二、`g_S206CoresExited` 永远不会涨

### 2.1 正常停止路径不经过它

桩停止时的链路：

```
桩执行 vmmcall
  → SvHandleVmExit: Sv206VmrunShouldExit() != 0 → guestContext.ExitVm = TRUE
  → SvHandleVmExit 尾部（4842）:
        4844: NT_ASSERT(ExitCode == VMEXIT_CPUID)    // Release 下是空操作
        4853: VpRegs->Rbx = NRip
        5008: if (ExitCode == VMEXIT_VMMCALL) Rbx = &Sv206LeaveToHost   ← 这里覆盖
  → SvLV20: mov rsp,rcx ; jmp rbx   →  跳到 Sv206LeaveToHost
  → Sv206LeaveToHost: sub rsp,28h ; call rax (PsTerminateSystemThread)   ← 直接终止
```

**`Sv206LeaveToHost` 用 `PsTerminateSystemThread` 终止线程，永不返回。**

**所以 `Sv206CoreThread` 6737 行的 `InterlockedIncrement(&g_S206CoresExited)` 永远不执行。**

### 2.2 时间对得上

```
20000 次 × 200000 空循环 = 4×10⁹ 次迭代
按 ~10⁹/s 估计 ≈ 4 秒

实测 sc stop 耗时：1.88 s / 1.93 s / 2.07 s / 2.19 s / 3.19 s / 4.12 s
```

**高度吻合。停止循环确实是在【超时】之后才继续的。**

---

## 三、这意味着什么

### 3.1 `invmrun=0x0` 是真的，但它不是"安全"的证据

`g_inVmrunCount` 归零是 `lock dec` 的效果 —— 那条指令在 **`vmrun` 返回后立即执行**（`+0x21`）。

**所以 `invmrun=0x0` 只证明："没有核还在 VMRUN 里"**，这是真的、有意义的。

**但它不证明"每个核都安全地走完了退出协议"** —— 那个是 `g_S206CoresExited` 的职责，而它从未成功过。

### 3.2 桩模式为什么仍然安全

**桩模式的安全来自另一个机制，不是那个等待循环：**

`SvLV20` → `Sv206LeaveToHost` 在**线程自己的栈上**、**SVM 已关闭**（EFER.SVME 已清）、
**GIF 已恢复**的状态下终止线程。

**这个终止是同步的、在当前核上完成的。** `DriverUnload` 等待超时的那 4 秒里，
所有核其实早已完成了终止。

**所以超时是无害的 —— 但它是"恰好无害"，不是"设计正确"。**

### 3.3 为什么 RealGuestMode 会崩而桩模式不会

**RealGuestMode**：guest 在 `SvVirtualizeProcessor` 内部走完 `return`，
然后执行 `Sv206CoreThread` 尾部 → `PsTerminateSystemThread`。

**这个终止发生在 guest 模式里**，而 `vmrun` 的宿主帧**从未恢复** ——
那个核永远停在 `vmrun` 之后，等镜像被 unmap，取指 `vmsave` 失败。

**桩模式**：guest 停在 `vmmcall`，`ExitVm` → `SvLV20` 正常退出 → `Sv206LeaveToHost` 在 host 态终止。

**两条路的区别不是"计数器有没有涨"，而是"VMEXIT 之后有没有走 SvLV20"。**

---

## 四、修正后的正确结论

**RealGuestMode 的问题可以更精确地表述为：**

> 它的 guest 走的是 C 函数的 `return`，而不是 `vmmcall` → `#VMEXIT` → `SvLV20`。
> **没有 `SvLV20`，就有 `vmrun` 帧永远悬挂。**

**而 9.551 试图加的 `vmmcall` tick 方向是对的**，但它加在了
`Sv206CoreThread` 尾部 —— 那里已经被 guest 的 C 返回链走过了，
且停止标志还没置位。

**正确的位置是：让 guest 在早退后【跳进桩的循环】，而不是走完 C 函数。**

**这条路的可行性**：需要一个从 C 代码 `jmp` 进 `Sv206GuestStub` 的动作，
且必须在 guest 态、栈状态可控时完成。

---

## 五、顺带发现的另一个问题

**`main.c:5123` 的等待循环，其成功条件是永不满足的。**

它现在依赖超时（4 秒），这在每次卸载时都是**固定开销**。

**如果把它改成等 `g_inVmrunCount == 0`（即 `invmrun`），会立刻返回。**
但那个数值在 `vmrun` 返回时就归零，**早于** `Sv206LeaveToHost` 完成终止 ——
**所以直接改会造成竞争。**

**正确的修法是让 `Sv206LeaveToHost` 在终止前递增 `g_S206CoresExited`。**
那是一个独立、有限、低风险的改动：在 `call rax` 之前加一条 `lock inc`。

---

## 六、当前状态

```
RealGuestMode : 编译期固定关闭
bin = 部署    : 25B1263D918F9DE2
机器          : 正常，驱动 STOPPED
桩模式        : 可用
```

**下一步（低风险、可独立验证）**：在 `Sv206LeaveToHost` 里递增 `g_S206CoresExited`，
让停止等待真正生效，卸载耗时从 ~4 秒降到接近 0。
