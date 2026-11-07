# 成功案例对照：怎么在「Windows 当 guest」下干净停止

> 2026-11-08。来源：`D:\ref\Type2-AMD-HV`（已克隆的开源参考项目，AMD SVM/NPT，Windows 当 guest）。

---

## 一、我们的问题

RealGuestMode 下 Windows 能在 guest 态稳定跑（实测 30 秒、快 38%），但 `sc stop` 卡在
`STOP_PENDING` 永不返回。

**我们的停止路径**（`SvLV20`）：

```asm
mov rsp, rcx        ; 换成 host 栈
mov ecx, 'SSVM'
jmp rbx             ; 跳到 NRip —— 即"把控制权还给 guest"
```

**这个设计只在「guest 是桩」时成立**：桩停在 `vmmcall` 上，`NRip` 是它自己循环里的下一条。
真实 Windows 线程的 `NRip` 可能在任意函数中间 —— **jmp 回去就是静默损坏**（9.442 已经写过）。

---

## 二、参考项目的做法：`iretq` + Machine Frame

### 2.1 结构（`svm.h:461-475`）

```c
typedef struct _SVM_MACHINE_FRAME {
    UINT64 Rip;
    UINT64 Cs;
    UINT64 Fill1[?];
    UINT32 Eflags;
    UINT32 Fill2;
    UINT64 Rsp;
    UINT64 Ss;
    UINT64 Fill3[?];
} SVM_MACHINE_FRAME, *PSVM_MACHINE_FRAME;
C_ASSERT(sizeof(SVM_MACHINE_FRAME) == 0x28);
```

**这正是 x86-64 的 `iretq` 帧布局**（RIP / CS / RFLAGS / RSP / SS）。

### 2.2 停止时的完整序列（`vmexit.c:909-964`）

```c
/* 1. 填 iretq 帧：从 VMCB 的 NRip / CS / Rflags / RSP / SS */
Frame->Rip    = Control->NRip;
Frame->Cs     = StateSave->Cs.Selector;
Frame->Eflags = (UINT32)StateSave->Rflags;
Frame->Rsp    = StateSave->Rsp;
Frame->Ss     = StateSave->Ss.Selector;

/* 2. 恢复 guest 的 FS/GS/TR/LDTR/KernelGsBase/STAR/LSTAR... */
__svm_vmload(Vcore->GuestVmcbPa);

/* 3. 恢复 guest CR3 */
__writecr3(StateSave->Cr3);

/* 4. 恢复 guest IDT */
Idtr.Limit = StateSave->Idtr.Limit;
Idtr.Base  = StateSave->Idtr.Base;
__lidt(&Idtr);

/* 5. 关中断 + STGI（恢复 GIF，让 NMI/SMI 正常）*/
_disable();
__svm_stgi();

/* 6. 清 EFER.SVME —— 离开 SVM 模式 */
Efer.Value = StateSave->Efer;
Efer.SvmEnable = 0;
__writemsr(MSR_IA32_EFER, Efer.Value);

/* 7. 清 VM_HSAVE_PA */
__writemsr(MSR_VM_HSAVE_PA, 0);

/* 8. 返回 FALSE -> asm 退出循环 */
return FALSE;
```

### 2.3 asm 侧（`svm.asm:165-171`）

```asm
    ; If handler returned non-zero, loop back to VMRUN.
    jnz     VmRunLoop

    ; Stop hypervisor: exit via IRETQ.
    ; Advance RSP past XMMs(0x100) + GuestVmcbPa(8) + HostVmcbPa(8) + Self(8)
    ; to reach the MachineFrame.
    add     rsp, 118h
    iretq
```

**`iretq` 用 MachineFrame 里的 RIP/CS/RFLAGS/RSP/SS 直接返回 guest —— 不 jump 到 NRip，
而是"假装从中断返回"。**

---

## 三、为什么这解决了我们的问题

| | 我们的 `SvLV20`（jmp rbx）| 参考项目（iretq）|
|---|---|---|
| 控制流 | `jmp NRip` —— **跳进 guest 的下一条指令**，RSP 是 host 栈 | `iretq` —— **完整恢复 guest 的 CS/RFLAGS/RSP/SS**，然后继续 |
| 桩 guest | ✅ 可以（桩不用栈）| ✅ 也可以 |
| 真实 Windows guest | ❌ **RSP 错、段寄存器错 → 静默损坏** | ✅ **栈和段全部正确恢复** |

**关键差异：`iretq` 恢复的是「guest 的完整执行环境」，`jmp` 只改了 RIP。**

我们的 `SvLV20` 是 `mov rsp, rcx; jmp rbx` —— `rcx` 是 host 栈。
参考项目是 `iretq` —— RSP 来自 **VMCB.StateSave.Rsp**，即 guest 自己的栈。

---

## 四、我们要改什么（对照清单）

| 步骤 | 我们现状 | 需要 |
|---|---|---|
| iretq 帧结构 | ❌ 无 | 新增 `SVM_MACHINE_FRAME`（0x28 字节）|
| 停止时填帧 | ❌ 无 | 从 `NRip`/`Cs`/`Rflags`/`Rsp`/`Ss` 填 |
| `__svm_vmload` | ⚠️ 部分（`SvLV20` 里有 `vmsave`，不是 `vmload`）| 确认恢复 guest 段寄存器 |
| 恢复 guest CR3 | ❓ | `__writecr3(StateSave.Cr3)` |
| 恢复 guest IDT | ❓ | `__lidt` |
| `__svm_stgi` | ✅（有）| — |
| 清 `EFER.SVME` | ✅（有）| — |
| 清 `VM_HSAVE_PA` | ❓ | `__writemsr(MSR_VM_HSAVE_PA, 0)` |
| **asm 出口** | ❌ `mov rsp,rcx; jmp rbx` | **`iretq`** |

---

## 五、其它成功案例（同一模式）

| 项目 | 停止方式 |
|---|---|
| **Type2-AMD-HV** | `iretq` + MachineFrame，`HvCallDevirtualize` 触发 |
| **miniSVM**（Vu2n）| Type-1 bare-metal，UEFI 阶段装 —— 无运行时卸载需求 |
| **SimpleSvm**（juliotec / tandasat）| 教学项目，guest 是合成桩，用 `vmmcall` 停止（和我们一样）|
| **hvpp / HyperDbg** | Intel VT-x，`vmlaunch`/`vmresume` 退出用 `vmxoff` + 类似 iretq 恢复 |

**凡是「真实 OS 当 guest」的项目，都用 iretq 类机制退出；凡是「合成桩当 guest」的，
都用 vmmcall 停止。** 我们的困境正是因为用桩时代的方法处理真实 guest。

---

## 六、要先验证的假设

**`iretq` 真的能在这里工作吗？** 需要确认：

1. **`VMCB.StateSave.Rsp` 在真实 guest 运行时是有效的用户栈** —— 是的，因为 guest 就是
   Windows 线程，它一直在用自己的栈
2. **`NRip` 是"下一条要执行的指令"** —— 在 `VMMCALL` 退出时，`NRip` 是 `vmmcall` 之后那条
3. **`iretq` 之后 GIF 已经恢复（STGI）** —— 参考项目明确做了

**但有一个我们没有的条件**：参考项目用 `HvCallDevirtualize`（用户态主动 `vmmcall` 触发）。
**我们是被 `sc stop` 从外部叫停**，没有"让某个线程主动执行 vmmcall"的机会。

**这可能是我们和它的真正差异 —— 需要先解决"谁来触发停止"。**

---

## 七、★★ 最关键的一环：停止怎么触发（不是从外部，是从核内部）

我原来的疑问是「我们被 sc stop 从外部叫停，没有让线程主动执行 vmmcall 的机会」。
**参考项目解决了这件事，而且方案很干净。**

### 7.1 触发链（broadcast.c + svm.c）

`
DriverUnload
   -> BroadcastTerminate(VcpuArray, CpuCount)          // hv.c:231
        -> 对每个 CPU 挂一个 DPC，KeInsertQueueDpc
             -> BroadcastiDpcRoutine 在【该 CPU 上】执行
                  -> SvmTerminateCpu(Vcpu)
                       -> AsmSvmVmmcall(HvCallDevirtualize)   // ★ 在该核上主动执行 VMMCALL
                            -> #VMEXIT
                                 -> SvmiHandleStopHv()
                                      -> 填 MachineFrame + vmload + wrmsr(SVME=0)
                                      -> return FALSE
                                           -> asm: add rsp,118h ; iretq
                                                -> 回到 SvmTerminateCpu 之后，该核已 native
`

**关键点：AsmSvmVmmcall() 是由 DPC 在目标 CPU 上直接执行的，不是靠 guest 自己走到某条指令。**

因为 **DPC 在 PASSIVE_LEVEL/DISPATCH_LEVEL 上可以在被虚拟化的核上运行** ——
一旦在那个核上跑起来，执行 mmcall 就会产生 #VMEXIT，然后走正常退出路径。

### 7.2 我们为什么做不到

| | 参考项目 | 我们 |
|---|---|---|
| 触发 | **DPC 扇出，每核主动 mmcall** | 设标志，**等 guest 自己走到 vmmcall** |
| guest 是桩 | — | ✅ 桩每轮都 vmmcall，所以等得到 |
| guest 是真实 Windows | — | ❌ **Windows 不会替我们执行 vmmcall** |

**这就是差别的全部。** 我们依赖 guest 配合，而真实 Windows 不配合。

### 7.3 我们要补的正好是这一块

`
现在： Sv206VmrunStopRequested = 1
       -> 等 guest 的 vmmcall 退出

要改： Sv206VmrunStopRequested = 1
       -> KeIpiGenericCall / DPC 扇出到每个核
            -> 在该核上执行 vmmcall（我们自己的，不是 guest 的）
                 -> #VMEXIT -> 停止路径 -> iretq
`

**而且 main.c:5143-5146 已经有 yghv_s206_kick_parked_cores() 的调用循环** ——
但它是**注入 NMI**，不是**直接执行 vmmcall**。这是可改的方向。

## 八、下一步（不盲目改）

1. **先查参考项目是否有"外部触发停止"的路径**（除了 `HvCallDevirtualize`）
2. **查 miniSVM / hvpp / HyperDbg 有没有从 DriverUnload 停止的做法**
3. **我们的桩模式已验证可用** —— 新方案必须与它并存，通过 flag 切换

**在拿到这些之前，不动代码。**
