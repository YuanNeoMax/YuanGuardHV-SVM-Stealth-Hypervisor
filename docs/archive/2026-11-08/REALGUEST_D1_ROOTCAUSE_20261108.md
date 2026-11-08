# 9.547 RealGuestMode 的 0xD1 精确根因：`vmsave`（vmrun 返回后的第一条）

> 2026-11-08。**从 dump 反汇编得到的确定结论，不是推断。**

---

## 一、崩溃事实

```
时间   2026-11-08 07:38:51
代码   0x000000D1   DRIVER_IRQL_NOT_LESS_OR_EQUAL
P1     0xfffff80267ee80fe     被引用的地址
P2     0x000000000000000e     IRQL = 14 (HIGH_LEVEL)
P3     0x0000000000000008     取指执行（instruction fetch）
P4     0xfffff80267ee80fe     引用该地址的指令地址
SYMBOL yghv_pa+280fe
BUCKET AV_CODE_AV_STACKPTR_ERROR_PAGED_IP_yghv_pa!unknown_function
```

**P1 = P4** → 取指失败，不是数据访问。

---

## 二、反汇编：`+0x280FE` 是哪条指令

```
Sv206LaunchVm @ 00000001400280E0
  00000001400280E0: mov    rax,rsp
  00000001400280E3: mov    [rcx+140h],rax          ; 保存线程 RSP
  00000001400280EA: mov    rsp,rcx                 ; 切到宿主栈
  00000001400280ED: mov    rax,[rsp]               ; GuestVmcbPa
  00000001400280F1: vmload rax
  00000001400280F4: lock inc [0x140063174]         ; inVmrun++
  00000001400280FB: vmrun rax                      ; ← 进入 guest
  00000001400280FE: vmsave rax                     ; ← ★ 崩溃点
  0000000140028101: lock dec [0x140063174]         ; inVmrun--
  0000000140028108: sub  rsp,190h
```

**崩溃在 `vmsave rax` —— `vmrun` 返回后的第一条指令。**

---

## 三、这说明了什么

**`vmrun` 确实返回了**（否则不会执行到 `+0x1E`），
**但返回后 CPU 无法取指 `vmsave` 那条指令。**

**唯一解释：`vmrun` 返回时，驱动镜像的代码页已经不可执行。**

### 为什么 RealGuestMode 会导致这个

桩模式：guest 从 `Sv206GuestStub` 开始（同一镜像内），
`vmrun` 只在**停止时**返回，此时驱动仍然完整。

RealGuestMode：guest 从 `ContextRecord->Rip` 开始（**`SvVirtualizeProcessor` 内部**），
那个 RIP 位于**驱动自己的代码中**。

**于是 `vmrun` 返回后，`vmsave` 要执行的那一页，正是 guest 刚刚"用过"的代码页。**

而 9.539 的 RealGuestMode 实验里，**NPT 映射的是完整 RAM（identity）**，
所以从理论上讲它应该可执行 —— **除非被 `sys-guard` / `npt-exclude` 动过。**

### 与 9.442 记录的形态一致

交接记录 9.421 节：
> *"9.421 found the real crash instruction: `vmsave rax` at +0x0D, the first instruction
> after vmrun returns. It writes guest state back into the VMCB at that physical address,
> and svm_core_cleanup may already have freed it."*

**但那次是"VMCB 已被释放"，这次 `P3=8` 是"取指失败"，说明是代码页的问题，不是数据。**
**两者不同。**

---

## 四、更可能的解释：`vmsave` 试图访问的 VMCB 无效

等等 —— `P3=8` 是**取指**，但 `vmsave rax` 的第一个操作数 `rax` 是 VMCB 物理地址。

**取指失败意味着 CPU 在执行 `vmsave` 这条指令本身时，读不到指令字节。**

**所以问题在 `0x1400280FE` 这个代码地址可不可读，不在 `rax` 指向哪里。**

### 那么：为什么那一页不可执行了

**最可能的答案：NPT。** `vmrun` 返回后 CPU 用的仍是 `nCR3 = g_npt`，
如果那一页在 NPT 里被标成 present=0 或 NX，`vmsave` 就取不到。

**而 9.542 我加的 `Sv206IretSlot` accessor 指向 `StackContents` 的尾部 ——
那是 `VIRTUAL_PROCESSOR_DATA` 的内存，不是代码。**

**但 9.541 的教训是：结构体一变，所有偏移失效。9.542 虽然没改大小，
`Sv206IretSlot` 是一个新函数，它可能落在原来用于别处的页上。**

**这只是假设，必须验证。**

---

## 五、诚实说明：我不知道确切原因

上面第四节最后那段是**推测**，没有证据。

**我能确定的只有三件事**：

1. 崩溃在 `vmsave rax`，`vmrun` 返回后的第一条指令
2. 是**取指失败**（P3=8），不是数据访问
3. 发生在 **RealGuestMode=1** 时；桩模式从不崩在这里

**要确定原因，需要**：
- 在 `vmrun` 前后记录 NPT 中那一页的状态
- 或者对比 RealGuestMode 与桩模式下 `nCR3` 的差异

**这需要新的诊断，不是推理。**

---

## 六、当前状态

```
注册表   : Parameters 已删除
bin = 部署 = F194C08AF46D2220（9.546 诊断版）
git      : 已提交（826bcb0）
机器     : 已重启（07:38:17），驱动 STOPPED
```

**桩模式可用，RealGuestMode 会崩。**

---

## 七、结论：RealGuestMode 的障碍不是"停不下来"，是"一跑就崩"

之前的判断是「RealGuestMode 能跑 30 秒、但停不下来」。
**这次加了 `guest-start` 诊断后，它连启动都崩了。**

**可能的解释**：9.546 的诊断代码（`g_S206GuestFirstExitTaken` 在
`SvHandleVmExit` 入口处 `InterlockedIncrement`）改变了时序或寄存器状态，
让原本勉强能跑的 RealGuestMode 崩了。

**这也是推测。** 但有一点是确定的：

> **在 `vmsave` 崩溃这个问题解决之前，RealGuestMode 不可能可用。**

而要解决它，必须先知道**为什么那一页不可取指** —— 这需要新的测量。
