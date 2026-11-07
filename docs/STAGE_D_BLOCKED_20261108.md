# 9.548 结果：启动与早退成功，停止崩在 `vmsave`

> 2026-11-08。**阶段 C 达成，阶段 D/E 的障碍被精确定位。**

---

## 一、成功的部分（这是真突破）

**RealGuestMode=1，探针关闭后：**

```
entries=6  early-returns=6  cores-marked=6  disagrees=0
guest-start: realGuestMode=1  writers=6
  guest Rip given = 0xFFFFF801BDB2531F
  guest Rsp given = 0xFFFFE283826D2B50
sys-health: degraded=0 total=0 fatal=0
```

**启动成功，稳定运行，`early-returns=6`。**

**6 个核全部走了早退分支** —— guest 恢复后回到了 `SvVirtualizeProcessor`，
C 帧沿正常路径展开。**参考项目的模型在本机成立了。**

---

## 二、两次蓝屏的区分

| 时间 | 探针 | 结果 | 崩溃点 |
|---|---|---|---|
| **07:38:51** | **ON** | **启动即崩** | `Sv206LaunchVm+0x1E` |
| **07:51:38** | **OFF** | **启动成功 → 停止时崩** | `Sv206LaunchVm+0x1E` |

**同一个崩溃点，不同触发路径。**

### 反汇编核对（map 验证）

```
当前 Sv206LaunchVm 镜像 VA = 0x1400280B0
本次 P1 = 0xFFFFF801BDB280CE
P1 - 0x1E = 0xFFFFF801BDB280B0   ← 与 map 完全一致
```

**确认是 `Sv206LaunchVm+0x1E`，即 `vmsave rax`（`vmrun` 返回后第一条指令）。**

---

## 三、`vmsave` 崩溃的正确解释

```
0x1400280B0: Sv206LaunchVm 入口
  +0x00  mov rax, rsp
  +0x03  mov [rcx+140h], rax      ← 保存线程 RSP
  +0x0A  mov rsp, rcx             ← 切到宿主栈
  +0x0D  mov rax, [rsp]           ← GuestVmcbPa
  +0x11  vmload rax
  +0x14  lock inc [inVmrun]
  +0x1B  vmrun rax                ← 进入 guest
  +0x1E  vmsave rax               ← ★ 崩溃（P3=8 取指）
```

**`vmrun` 返回了，但 CPU 取不到 `vmsave` 那条指令。**

`P3=8` = **取指失败**，不是数据访问失败。

### 为什么桩模式不崩、RealGuestMode 崩

**桩模式**：停止时 `vmrun` 返回 → `vmsave` 执行 → 正常。
**崩溃从不发生在桩模式**，说明 `vmsave` 本身没问题。

**RealGuestMode**：`vmrun` 返回前，guest 已经跑完了 `SvVirtualizeProcessor` 的早退路径，
**并且可能改变了 NPT 映射或其自身的内存状态**。

**最可能**：早退路径里的 `SvFreePageAlingedPhysicalMemory(vpData)` 或
`ExFreePoolWithTag(contextRecord)` 释放了 `vmsave` 需要的 VMCB。

**看代码**（`SvVirtualizeProcessor` 的 Exit 段）：

```c
Exit:
    if (contextRecord != nullptr)
        ExFreePoolWithTag(contextRecord, 'MVSS');      // 释放 CONTEXT

    if ((!NT_SUCCESS(status)) && (vpData != nullptr))
        SvFreePageAlingedPhysicalMemory(vpData);       // 只在失败时释放 VPD
    ...
    return status;
```

**注意**：早退时 `status = STATUS_SUCCESS`（在早退分支里设的），
所以 `vpData` **不会被释放**。**这个假设不成立。**

**那 `vmsave rax` 的 `rax` 还有效吗？** 它来自 `mov rax,[rsp]`（+0x0D），
`rsp` 是宿主栈。**早退路径不碰宿主栈。**

**但取指失败说明问题在代码页，不在 `rax` 指向的地方。**

---

## 四、诚实的判断

**我不知道确切原因，但能确定的是：**

1. `vmsave` 崩溃发生在 `vmrun` **返回后**，且是**取指失败**
2. 只在 RealGuestMode 下发生
3. **启动时也会发生**（9.547，探针 ON 时）

**第 3 点很重要**：同一个崩溃点在两种时机都出现，说明它**不是停止路径特有的**，
而是 **`vmrun` 在 RealGuestMode 下返回时，某种状态与桩模式不同。**

**候选（未验证）**：
- RealGuestMode 下 guest 执行了 `SvVirtualizeProcessor` 的代码，
  而那段代码所在的页可能被 NPT 认为不可执行
- `vmrun` 返回时 `nCR3` 或 NPT 条目被 guest 改过

**要区分，需要在 `vmrun` 前后记录 NPT 中 `Sv206LaunchVm` 所在页的状态。**

---

## 五、当前状态

```
注册表   : Parameters 已删除（RealGuestMode 不存在）
机器     : 已重启（07:51:21），驱动 STOPPED
bin      : 桩模式版本（含 if(0&&) 关掉的探针）
           —— 桩模式已验证可用
creash** : 两次都在 Sv206LaunchVm+0x1E
```

**注意**：`if (0 && ...)` 的改动仍在源码里，需要决定是保留还是去掉。

---

## 六、下一步

**阶段 C 已达成**（early-returns=6 证明模型成立）。

**阶段 D/E 被 `vmsave` 崩溃阻塞**，而那个崩溃的确切原因还没找到。

**要查清，唯一的方法是**：在 `Sv206LaunchVm` 的 `vmrun` 前后，
记录 `Sv206LaunchVm` 所在页在 NPT 中的 present/NX 状态。

**但那是 ASM 里的位置，加 C 调用会重演 9.413-9.419 的灾难**
（在 asm 里 `call` 到 C 导致 0x139 / 0x7F）。

**所以更现实的做法**：接受这个障碍，把 RealGuestMode 标记为「实验性、会蓝屏」，
**不作为默认路径**，桩模式继续保持可用。
