# 9.540 iretq 出口实验：两次蓝屏与回滚记录

> 2026-11-08。结论：**未成功**。失败在 `0x139`，与之前所有失败形态都不同。

---

## 一、做了什么

按 `docs/DEVIRTUALIZE_REFERENCE.md` 第 1 步，把退出路径改成参考项目（Type2-AMD-HV）的
`iretq` + MachineFrame，但**保留原有 `jmp rbx` 作为回退**：

```c
// C 侧：从 VMCB 填 iretq 帧
g_S206IretFrame.Rip    = vp->GuestVmcb.ControlArea.NRip;
g_S206IretFrame.Cs     = (UINT64)vp->GuestVmcb.StateSaveArea.CsSelector;
g_S206IretFrame.Rflags = vp->GuestVmcb.StateSaveArea.Rflags;
g_S206IretFrame.Rsp    = vp->GuestVmcb.StateSaveArea.Rsp;
g_S206IretFrame.Ss     = (UINT64)vp->GuestVmcb.StateSaveArea.SsSelector;
InterlockedExchange(&g_S206IretReady, 1);
```

```asm
; asm 侧：运行时二选一
    xor     eax, eax
    mov     rax, qword ptr g_S206IretReady
    test    rax, rax
    jnz     SvLV30
    ...原有路径...
    mov rsp, rcx
    mov ecx, 'SSVM'
    jmp rbx

SvLV30:
    lea     rsp, g_S206IretFrame
    iretq
```

**产物验证通过**（两条路径都在）：
```
0x140028118: mov rsp,rcx
0x14002811B: mov ecx,'SSVM'
0x140028120: jmp rbx              ← 原有
0x140028122: lea rsp,[0x140063018]
0x140028129: iretq                ← 新增
```

---

## 二、结果：`0x139` KERNEL_SECURITY_CHECK_FAILURE

```
时间   2026-11-08 06:54:25
代码   0x000000139
参数1  0x0000000000000004      ← FAST_FAIL_...（参数 4 通常指 stack cookie / CFG 检查）
```

**这个退出码与 8 月所有实验、以及 9.538 的 `0xD1` 都不同。**

`0x139` 参数 4 通常表示 **`FAST_FAIL_STACK_COOKIE_CHECK_FAILURE`** ——
即 `/GS` 栈保护检查失败。

### 推断（未从 dump 验证）

`iretq` 把控制流"瞬移"到 `g_S206IretFrame.Rip`（= `VMCB.NRip`），
跳过了 `svm_simplevm206_x64.asm` 里 `Sv206LaunchVm` 的正常返回路径。

**该函数的 C 调用者（`Sv206VirtualizeC` / `Sv206CoreThread`）持有的栈帧从未被正常展开**，
而 `/GS` cookie 在 `SvLV20` 之后、`iretq` 之前的那段代码里处于"已弹出但未校验"的状态。

**换句话说：原路径 `jmp rbx` 跳到一个 `ret` thunk，会正常走完函数的 epilogue；
`iretq` 直接跳到 guest 的 RIP，绕过了 epilogue，栈 cookie 检查因此失败。**

这与参考项目的差异在于：**参考项目的 `AsmSvmLaunchVm` 是 `PROC` 形式、有自己的 epilogue
和 MachineFrame 在栈上的固定偏移**（`add rsp, 118h` 是精确算过的），
而我用的是**全局变量 + `lea rsp`**，栈布局完全不同。

**这就是"照抄参考项目的机制，但没有照抄它的栈布局"的代价。**

---

## 三、第二次蓝屏是我的操作失误

第二次 `0xD1`（06:58:49）**不是新问题**：我 `git checkout` 回滚了源码，
但**没有立即重建二进制**，机器加载的仍是带 `iretq` 的旧 `A64121A9`。

> **教训：回滚源码 ≠ 回滚部署。改完 .asm/.cpp 后，源码回滚必须紧跟一次重建+重新部署，
> 否则机器会继续加载崩溃版本。**

---

## 四、回滚与恢复

```
源码        : git checkout 恢复（已验证 0 处 9.540 残留）
重建        : bin = 09DAD6C47D430696
产物验证    : iretq 只剩 1 个（原有的 drax iretq），g_S206IretFrame 引用为 0
部署        : C:\yghv_pa.sys = 09DAD6C47D430696
注册表      : Parameters 键已删除（无 RealGuestMode）
冒烟        : RUNNING / degraded=0 / stop 1.88s / invmrun=0x0  ← 全部正常
```

**桩模式完全恢复，机器可用。**

---

## 五、真正的教训

**照抄机制不等于照抄实现。**

参考项目的 `iretq` 之所以成立，靠的不只是"用 iretq"这个想法，而是：

1. `AsmSvmLaunchVm` 是一个 `PROC`，**MachineFrame 在栈上有精确偏移**
   （`add rsp, 118h` 是数出来的）
2. 它的整个 asm 循环**从没给 C 函数留下未展开的帧** —— VMEXIT 之后直接 PUSHAQ/ POPAQ,
   没有经过 C 的 `call` 返回
3. 我们的 `Sv206LaunchVm` 是**被 C 调用的**，中间有 `call SvHandleVmExit`，
   栈上还留着 C 帧和 `/GS` cookie

**在"我们的 asm 被 C 调用"这个结构下，`iretq` 不能简单插进去。**
要照抄，必须连它的栈布局一起照抄——那不是第 1 步，是重构 `Sv206LaunchVm`。

---

## 六、★ 读完参考项目 asm 后的真正原因（本轮补）

### 6.1 两边的栈布局对比

**参考项目（svm.asm 文件头，第 24-32 行）：**

`
+0x000  GPRs        (16 * 8  = 0x80)
+0x080  XMMs        (16 * 16 = 0x100)
+0x180  GuestVmcbPa (8)
+0x188  HostVmcbPa  (8)
+0x190  Self        (8)
+0x198  MachineFrame (0x28)   ← ★ 帧就在宿主栈上
+0x1C0  Storage     (0x20)
`

**入口：**
```asm
AsmSvmLaunchVm PROC
    mov     rsp, rcx      ; rcX = vcore 的宿主栈
    sub     rsp, 100h     ; 指向 XMM 区下方
`

**它的 RSP 从第一条指令起就指向「vcore 的宿主栈」，整个循环期间不变。
所以 add rsp, 118h ; iretq 中那个 118h 是把 RSP 从 XMM 区推到 MachineFrame 的
**编译期已知常量**：

- RSP 在 XMM 区（+0x080）
- MachineFrame 在 +0x198
- 差 = 0x198 - 0x080 = **0x118** ✓

**我们的（svm_simplevm206_x64.asm 第 113-119 行）：**

`
Rsp => 0xfd0  GuestVmcbPa     ; HostStackLayout
       0xfd8  HostVmcbPa
       0xfe0  Self
       0xfe8  SharedVpData
       0xff0  Padding1
       0xff8  Reserved1
`

**入口：**
```asm
Sv206LaunchVm proc frame
    mov     rax, rsp
    mov     qword ptr [rcx - 5FD0h + 6110h], rax   ; 保存线程真实 RSP
    mov     rsp, rcx                                ; rcX = HostStackLayout
`

**我们的 RSP 指向 HostStackLayout，那块内存里只有 6 个 qword（0xfd0..0xff8），
**没有 MachineFrame**。我把帧放在了全局变量 g_S206IretFrame，用 lea rsp 指过去。

### 6.2 为什么这必然触发 0x139

**关键事实？** 我们的 RSP 在进入 asm 前**已经被 mov rsp, rcx 换成了宿主栈**，
而**线程的原始 RSP 被存在 HostExitRsp** 里（0x6110 偏移）。

iretq 一旦执行，就会把 RSP 换成帧里的 StateSaveArea.Rsp（guest 的栈），
然后跳到 NRip。**它不会回到 Sv206LaunchVm 的调用者**
（那个 C 栈帧至今仍挂在原地，带着 /GS cookie）。

**参考项目为什么没这个问题？** 因为它的 AsmSvmLaunchVm 是 **PROC（有 frame）但
从未"返回"过** —— 它靠 iretq 把 RSP 换成了 guest 的栈，而它的**调用者本身就是一个
在 guest 里跑的线程上下文**，那个 C 帧在 iretq 之后就"随 guest 一起"继续了。

**我们不同：我们的 Sv206LaunchVm 是被 Sv206VirtualizeC / Sv206CoreThread
调用的，是一个真实的 C 调用链。iretq 把控制流瞬移走，那条链永远不展开，
栈上留下悬空的 /GS cookie 与 SEH 链。**

### 6.3 所以真正的结论

> **不是 iretq 本身不行，是「在一条真实的 C 调用链里用 iretq 瞬移走」不行。**

参考项目能这么做，是因为它的整个执行模型是「DPC 里 RtlCaptureContext 然后 VMRUN」，
**那个 C 帧本来就是"故意不返回"的**（它靠 iretq 回到 guest 的 RIP，
而 guest 的 RIP 就是捕获点之后那条指令 —— 相当于 C 帧"被 guest 接管"了）。

**要照抄它，必须连这个执行模型一起照抄：**
- 不是"在 DriverUnload 里设标志等 guest 停"
- 而是"用 DPC 在每个核上 RtlCaptureContext + VMRUN，退出时 iretq 回到捕获点"

**那就是重写整个启动/停止模型，不是改一个退出指令。**

## 七、下一步（若要继续）

按代价排序：

1. **放弃 iretq，回到桩模式的现状** —— 已可用，性能 1.2×
2. **读完参考项目的 `svm.asm` 全文**，弄清它的栈布局与我方的根本差异，
   再决定是否值得为 RealGuestMode 重构 `Sv206LaunchVm`
3. **只保留 RealGuestMode 的"能跑"发现**（30 秒稳定、快 38%），
   把它当作"未来换机器时的施工图"，不在本机追求运行时卸载

**当前选择：1。** 机器已恢复，桩模式可用。

**未验证的推断**：`0x139` = stack cookie 失败。我没有从 dump 里确认这一点
（kd 分析两次被打断）。要确认需要重新分析 `C:\Windows\MEMORY.DMP`。
