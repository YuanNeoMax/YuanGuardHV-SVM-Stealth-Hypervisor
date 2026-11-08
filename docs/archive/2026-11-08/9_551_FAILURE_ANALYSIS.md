# 9.553 9.551 的 vmmcall tick 失败：时机错了

> 2026-11-08。**9.551 的设计推理正确，但放错了位置。**

---

## 一、9.551 做了什么

在 `Sv206CoreThread` 尾部、`InterlockedIncrement(&g_S206CoresExited)` 之前：

```c
if (yghv_s206_real_guest_mode())
{
    yghv_s206_real_guest_tick();   // 一次 vmmcall
}
InterlockedIncrement(&g_S206CoresExited);
```

**意图**：让 RealGuestMode 的早退路径产生一个 `#VMEXIT`，好让停止握手能应答。

---

## 二、结果：仍然 0xD1，且崩溃提前到启动阶段

```
08:09:45  dump 写入（0xD1）
08:09:41  boot
=> 崩溃发生在【上一个 boot】，即 9.551 实验期间

崩溃点：yghv_pa+0x280EE，P3=8（取指失败）
当前构建 Sv206LaunchVm @ 0x140027F00:
  +0x1B vmrun
  +0x1E vmsave     <- 同一偏移
```

**崩溃仍在 `vmsave`，而且我还没来得及读到 `virt-counts`（`sc start` 后 10 秒内就崩了）。**

**对比 9.548：那次探针关闭后，RealGuestMode 启动成功、`early-returns=6`、稳定运行 20 秒。**

**所以 9.551 让情况变差了。**

---

## 三、错在哪：`vmmcall` 的时机不对

### 3.1 停止标志此时还没置位

```c
// main.c DriverUnload（停止时才会跑）
yghv_s206_vmrun_stop_request();   // 设置停止标志
```

**9.551 的 tick 在 `Sv206CoreThread` 尾部执行，那时是【启动阶段】，停止标志 = 0。**

所以：

```
vmmcall  →  #VMEXIT  →  handler
                          ↓
                    Sv206VmrunShouldExit() 返回 FALSE（无停止请求）
                          ↓
                    继续 vmrun（SvLV10）
```

**这个 vmmcall 完全没起到作用** —— 它只是白跑一次退出。

### 3.2 更糟的是它破坏了 guest 的执行流

`vmmcall` 之后的指令是 `ret`（`Sv206TickVmmcall` 的返回），
它在 guest 里返回到 `yghv_s206_real_guest_tick`，再返回到 `Sv206CoreThread`，
然后继续 `InterlockedIncrement` → `terminateFn` → **在 guest 里终止线程**。

**和 9.548 完全一样，问题没解决，只是多了一次无意义的 VMEXIT。**

### 3.3 为什么反而崩溃提前

**这是推测，未验证**：多一次 `#VMEXIT` + `vmrun` 重入，在 guest 已经跑完 C 函数、栈状态异常的情况下，
更容易触发那个 `vmsave` 取指失败。

**9.548 能撑到停止时才崩，9.551 在启动阶段就崩了。**

---

## 四、正确的做法应该是什么

**`vmmcall` 必须在停止请求【之后】执行，不能在启动阶段的 C 返回链上执行。**

参考项目是怎么做的：

```c
// Type2-AMD-HV: SvmTerminateCpu
AsmSvmVmmcall(HvCallDevirtualize);   // 由 DPC 在【停止时】于该核上执行
```

**关键**：
1. **在停止时**，不是启动时
2. **由 host 侧（DPC）在当前核上执行**，不是 guest 的 C 代码执行

**而我们的 RealGuestMode 做不到第 2 点**（9.550 已验证）：
那个核正在 guest 里，DPC 无法在其中执行。

**第 1 点也做不到**：guest 的 C 返回链在启动阶段就跑完了，停止时它已经终止了。

---

## 五、真正的结论

**RealGuestMode 的早退路径的问题是它是"一次性"的。**

- 启动时：guest 跑完 `SvVirtualizeProcessor` → `Sv206CoreThread` → 终止
- 停止时：没有任何 guest 代码在跑，无法响应

**要解决，必须让 guest 在早退之后【不要终止】，而是停在一个可以反复 tick 的地方。**

**那就回到了桩模式的形态** —— 一个循环。

**讽刺的是**：桩模式正是这样做的，而且它工作。

---

## 六、一个可能的正确方向（未验证）

**让 RealGuestMode 的 guest 在早退后跳进桩的循环，而不是终止线程。**

```c
// 早退 return 之后（在 guest 里）：
// 不 terminateFn，而是 jmp Sv206GuestStub
```

**这样**：
- guest 完成了 `SvVirtualizeProcessor` 的早退（C 帧展开 ✅）
- 然后进入桩循环，开始 tick（停止握手可用 ✅）

**但**：`Sv206CoreThread` 的尾部是 C 代码，"jmp 到桩"需要汇编，
而且要保证栈状态允许 —— 这是它可能失败的地方。

**这是一个具体、有限的改动，但风险不明。**

---

## 七、当前状态

```
RealGuestMode : 编译期固定关闭（9.552，编译器优化为 xor eax,eax;ret）
bin = 部署    : 25B1263D918F9DE2（桩模式）
机器          : 正常，驱动 STOPPED
桩模式        : 可用（invmrun=0x0）
```

**9.551 的改动仍在源码里（现在不可达），可作为下一步的基础。**
