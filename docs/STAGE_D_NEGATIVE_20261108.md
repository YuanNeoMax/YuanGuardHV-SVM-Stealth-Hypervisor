# 9.550 阶段 D 前提验证：DPC 停止在本架构下不成立

> 2026-11-08。**验证结果是否定的 —— 但这是一个有价值的确定结论，不是失败。**

---

## 一、要验证的问题

阶段 D 假设：用 DPC 扇出，在每个核上主动执行 `vmmcall`（像参考项目的
`SvmTerminateCpu` 那样），就能停下来。

**前提条件**：DPC 能在 RealGuestMode 的 guest 运行期间，于目标核上执行我们自己的代码。

---

## 二、验证过程

### 2.1 `Sv206CoreThread` 的结构

```c
static VOID Sv206CoreThread(PVOID Context)
{
    KeSetSystemAffinityThread(...);

    ctx->Status = Sv206VirtualizeC(ctx->SharedVpData);   // → SvVirtualizeProcessor → Sv206LaunchVm

    InterlockedIncrement(&g_S206CoresExited);            // 停止握手依赖这个

    terminateFn(STATUS_SUCCESS);                         // PsTerminateSystemThread
}
```

### 2.2 RealGuestMode 下的实际执行顺序

```
1. SvVirtualizeProcessor: RtlCaptureContext(contextRecord)
2. setup: GuestVmcb.Rip = ContextRecord->Rip     ← 函数【内部】的返回点
3. Sv206LaunchVm(...)  →  vmrun rax              ← 进入 guest，永远不返回
4. guest 从 Rip 继续执行 → 落到 if(installed) → 早退分支
5. return STATUS_SUCCESS                          ← ★ 在 guest 里返回
6. 回到 Sv206VirtualizeC → 回到 Sv206CoreThread
7. InterlockedIncrement(&g_S206CoresExited)       ← ★ 在 guest 里执行
8. terminateFn(STATUS_SUCCESS)                    ← ★ 在 guest 里终止线程
```

**第 5–8 步全部发生在 guest 模式里。**

### 2.3 结论：那个核的 `vmrun` 帧永久悬挂

`vmrun` 从未返回。线程在 guest 里走完了 `Sv206CoreThread` 的整个尾部，
然后终止了自己 —— **但它终止的是"guest 里的那个执行流"，而 VMRUN 的宿主帧仍然挂在那里。**

**这正是 9.442 记录过的现象：**

> *"Twelve cores, twelve suspended frames, and after DriverUnload unmaps the image
> their `vmsave` runs on dead code: the 0xD1 / PAGED_IP at Sv206LaunchVm+0x14"*

**这一次是 `+0x1E`（`vmsave`），同一条指令，同一个机制。**

---

## 三、为什么 DPC 方案解决不了

参考项目的 `SvmTerminateCpu` 能在核上执行 `vmmcall`，因为它满足两个条件：

| 条件 | 参考项目 | 我们 |
|---|---|---|
| 该核上运行的是**我们自己的代码** | ✅ DPC 就是我们的代码 | ❌ 该核正在 guest 里跑 Windows 代码 |
| `vmrun` 帧有对应的 host 执行流来返回 | ✅ `AsmSvmLaunchVm` 之后就是 `SvmTerminateCpu` | ❌ `vmrun` 的 host 帧**从未恢复** |

**关键差异**：参考项目的 `RtlCaptureContext` 捕获的 RIP **在 `SvmVirtualizeCpu` 内部**，
guest 恢复后走完那个函数**但仍在 guest 里** —— 然后它**继续在 guest 里跑到
`SvmTerminateCpu` 的 `vmmcall`**，那个 `vmmcall` 产生 `#VMEXIT`，**才真正回到 host**。

**而我们的早退路径终止了线程，没有 `vmmcall` 把控制权交回 host。**

---

## 四、正确的修法（如果要做）

**问题不在停止机制，在早退路径的终点。**

参考项目在早退之后**继续在 guest 里执行**，直到遇到 `vmmcall`。
我们的早退之后**终止线程**。

**要让它能停，早退路径必须：**

```
早退 return 之后，不要 terminateFn()
   → 而是执行一个 vmmcall
        → #VMEXIT → handler 看到停止标志 → 正常停止 → iretq
```

**即：把 `Sv206CoreThread` 的尾部从「终止线程」改成「执行 vmmcall 并等待」。**

这是**对 `Sv206CoreThread` 的改写**，不是对停止机制的改写。

---

## 五、为什么这一轮仍然有价值

**验证了一个假设，并明确否定了它。**

阶段 D 的原方案（DPC 扇出主动 vmmcall）**不成立**，原因不是实现难度，
而是**结构上缺一个 host 执行流来接收 `vmrun` 的返回**。

**如果直接去写 DPC 扇出，会在完全错误的方向上投入大量时间，然后以又一次蓝屏收场。**

---

## 六、当前状态

```
注册表 : Parameters 已删除
机器   : 已重启（07:51:21），驱动 STOPPED
bin    : D830A5A0C7305C72
桩模式 : 完全可用（invmrun=0x0）
提交   : 6ed47d3
```

---

## 七、真实的位置

| 项 | 状态 |
|---|---|
| RealGuestMode 启动 | ✅ |
| `early-returns=6`（C 帧展开）| ✅ |
| RealGuestMode 停止 | ❌ **结构上缺 host 执行流** |
| 桩模式 | ✅ 完全可用 |

**要真正实现「Windows 当 guest + 可卸载」，需要把 `Sv206CoreThread` 的尾部
从「终止线程」改成「执行 vmmcall 并把控制权交回 host」。**

这是一个明确、有限的改动点 —— 但它会碰到 9.443–9.445 那四次蓝屏换来的
线程终止路径，风险不低。
