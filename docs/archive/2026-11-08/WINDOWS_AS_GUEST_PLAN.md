# Windows-as-Guest：参考项目对照与真正的分歧点

> 2026-11-08。**本文推翻此前「本机不可行」的结论。**

---

## 一、反例存在

### Type2-AMD-HV（whosstyler，开源）

> **"Once loaded, every logical processor is put under VMRUN. Windows keeps running normally."**

AMD SVM/NPT 实现，README 明确写了完整做法：

| 环节 | 做法 |
|---|---|
| 接管 | `BroadcastVirtualize`：DPC 扇出到每个 CPU，各自 `RtlCaptureContext` 后进 VMRUN |
| **中断** | **host 有独立 IDT**（拷贝 Windows 的，覆盖 0-20，NMI 保留原样）|
| NPT | dual-NCR3 双树（`npt_main` 读 / `npt_hooks` 执行，解决 AMD 无 execute-only）|
| MSR | 只影子化 EFER 和 VM_HSAVE_PA |
| **拦截范围** | **只有 VMRUN / VMMCALL / 自己的 NPF。"Everything else passes through."** |

**同样 AMD、同样 NPT、同样是 Windows 当 guest。**

### 另两个参考

- **miniSVM**（Vu2n）：Type-1 bare-metal，UEFI 阶段装好，"hosts whatever boots next"
- **vmmcall.org**："Building a Windows Hypervisor for Systems Research"

---

## 二、真正的分歧点（关键发现）

### 2.1 我们 8 月试的是什么

`_ARCHIVE_HANDOFF_9.1-9.299.md` 9.174 节原文：

> **step23（阻塞常驻 + INTR/NMI 拦截 + host ISR + 全部修复：GS selector/TSS
> 隔离/GS MSR 虚拟化/ASID 卫生 FLUSH_ALL）实测：仍 <5s 硬冻结。**
>
> **决定性含义**：…… 对阻塞常驻（guest 内真实 Windows 调度器上下文切换）而言，
> **INTR/NMI 拦截 + host ISR 也不能解冻**
>
> **剩余唯一未测驱动级方向**：**拦截 guest 的 CR3 写**

### 2.2 所以两条路从来没被分开过

| 配置 | 试过？ | 结果 |
|---|---|---|
| **INTR/NMI 拦截 + host ISR** | ✅ 试过（step23）| 仍冻结 |
| **不拦截 INTR/NMI，让 guest 自己处理** | ❌ **从未试过** | — |

**8 月的实验矩阵里，所有配置都在「拦截中断」这一侧。** 唯一 PASS 的 step20 也是
「INTR/NMI 拦截 + host ISR」，只是 guest 是自旋桩、不执行真实 Windows 调度器。

**「让 Windows 的 ISR 在 guest 态原生执行」这个配置，从未被测试。**

### 2.3 而参考项目走的正是这条没试过的路

Type2-AMD-HV 的 "Everything else passes through" 意味着：

- **不拦截 INTR**：Windows 的 ISR 在 guest 态直接跑
- **不拦截 APIC 访问**：guest 直接操作自己的 APIC（在虚拟机里它就是 native）
- **没有 EOI/TPR 转发**：因此**没有持续的 APIC 寄存器访问流**

---

## 三、errata 1363 的含义需要重新界定

**AMD 官方（Revision Guide 56683，Family 19h）：**

> **#1363 — Persistent Stream of APIC Register Accesses May Cause System to Hang or Reset**
> **No fix planned**

**关键在「Persistent Stream（持续的流）」这个词：**

| 方案 | 是否产生持续的 APIC 访问流 |
|---|---|
| **host ISR 代管**（我们的 step23）| ✅ **是** —— 每次中断都要 host 转发 EOI/TPR 到物理 APIC |
| **guest 自己处理**（参考项目）| ❌ **否** —— ISR 直接操作，无转发，无额外流 |

**所以 errata 1363 是「host 代管中断」方案的必然结果，不是「Windows-as-guest」的必然结果。**

---

## 四、8 月结论的准确表述

它写的是：

> 「**OS-as-Guest（真实 Windows 在 guest 态执行调度器/ISR）在本机不可行**」

**但它的实验里，「guest 态执行 ISR」这个前提从未被满足过。**

- 拦截中断 → guest 不执行 ISR（host 代管）
- 不拦截中断 → **从未测试**

**它验证的是「host 代管中断的方案不行」，不是「Windows 当 guest 不行」。**

---

## 五、可行路线

### 5.1 关键配置：中断完全不拦截

```
VMCB.ControlArea.InterceptMisc1：
  ✅ 拦截 VMRUN      （必需，APM Vol2 Table B-1）
  ✅ 拦截 VMMCALL    （自己的 hypercall 通道）
  ❌ 不拦截 INTR     ← 关键
  ❌ 不拦截 NMI
  ❌ 不拦截 HLT
  ❌ 不拦截 APIC 访问（不做 MMIO 影子、不做 EOI/TPR 转发）
  ❌ 不拦截异常      （或只拦截必要的）
```

### 5.2 需要的组件对照

| 组件 | 我们现状 | 参考项目 | 差距 |
|---|---|---|---|
| 独立 host CR3 | ✅ | ✅ | — |
| **独立 host IDT** | ❓ 需确认 | ✅（拷贝 + 覆盖 0-20，NMI 保留）| 可能要补 |
| MSR 影子（EFER / VM_HSAVE_PA）| ⚠️ 部分 | ✅ | 要补全 |
| 每 CPU 一个 VMCB | ✅ | ✅ | — |
| DPC/IPI 扇出接管 | ✅（`KeIpiGenericCall`）| ✅ | — |
| **不拦截中断的配置** | ❌ | ✅ | **这是要补的** |

### 5.3 最小验证实验

**只改拦截位，不改架构，保留现有桩方案作回退：**

```
1. 新增一个模式标志（不动现有 206-B 路径）
2. guest 上下文 = RtlCaptureContext 捕获的真实 Windows RIP
3. InterceptMisc1 只留 VMRUN | VMMCALL
4. 不拦截 INTR / NMI / HLT / APIC
5. 观察稳定性（目标：跑满 5 分钟）
```

**如果稳定 → 8 月结论被推翻，Windows-as-guest 在本机可行。**

---

## 六、风险与未验证项

1. **参考项目未必在我们这台 CPU 上验证过。** Type2-AMD-HV 的 README 没说测试机型。
   **「它能行」是推断，不是本机实测。**

2. **8 月的文档是横向归纳版，细节可能在逐条记录里。** 我搜了「不拦截中断」「pass-through」
   「仅拦截 VMRUN」等关键词，全部 0 命中；找到的是「**剩余唯一未测方向是 CR3 写拦截**」这句话
   —— 说明当时**认为**中断这条路已经穷尽（因为它只试了「拦截」这一侧）。

3. **NPT 仍需正确。** 9.177-9.179 说「OS-as-guest 必须共享宿主页表」，
   而参考项目用 dual-NCR3 双树。这两者是否冲突需要核对源码。

4. **曾经的 114 次蓝屏记录是资产，不是障碍** —— 它告诉我们哪些组合不行，
   正好用来缩小这次的搜索空间。

---

## 七、下一步（按顺序）

1. **拉 Type2-AMD-HV 源码**，对照它的：
   - `HvInitialize` 完整启动序列
   - VMCB 拦截位的具体设置
   - host IDT 的构建方式
2. **核对 9.177-9.179 的「必须共享宿主页表」与参考项目的 dual-NCR3 是否矛盾**
3. **做最小实验**（只改拦截位 + 保留回退）

---

## 八、诚实说明

- 本文的结论是「**有可能，值得一试**」，不是「**一定能成**」
- 参考项目是间接证据（别人的机器、别人的实现），不是本机实测
- 我上一版规划写「换 Intel 机器是唯一出路」，**那个结论下早了**
