# 9.545 阶段 A2：per-core 标志，`disagrees` 4 → 0

> 2026-11-08。**推断被证实，阶段 B 的前提条件满足。**

---

## 一、结果

```
改前（9.544 全局标志）: entries=6  early-returns=0  secondPass=1   disagrees=4
改后（9.545 per-core）: entries=6  early-returns=0  cores-marked=6  disagrees=0
```

**`disagrees` 从 4 降到 0。**

**`cores-marked=6`** —— 6 个核全部被正确标记，不是 1 个。

---

## 二、证实了什么

### 2.1 前一轮的分歧推断是对的

9.544 推测：全局标志被核 0 设置后，其余核看到 `secondPass=1`，但它们自己的
CPUID 还没被虚拟化（仍返回"未安装"），于是产生分歧。

**改成 per-core 后分歧归零 —— 推断被实测证实，不是猜的。**

### 2.2 为什么 `cores-marked=6` 重要

如果只虚拟化了核 0，这个数会是 **1**。

它是 6，说明 **6 个核各自独立完成了 setup + 标记**。
这正是阶段 B（让标志决定分支）能安全的前提：
每个核的"我是第几次进入"都是**它自己的事**，不是全局状态。

### 2.3 计数器语义也修正了

9.543 时 `early-returns` 在函数末尾**无条件递增**，那会把"首次 setup 成功返回"
也算成早退 —— 在真正的早退从不发生的模式下，这个读数会看起来健康。

9.544 起它只在**真正的早退分支**里递增。所以现在的 `early-returns=0` 是真实读数。

---

## 三、这一步改变了什么，没改变什么

**改变了**：标志的作用域（全局 → per-core 数组），以及 `secondPass` 的语义
（单个值 → 已标记核数）。

**没改变**：分支逻辑仍是

```c
if (SvIsSimpleSvmHypervisorInstalled() == FALSE)   // 仍由 CPUID 决定
{
    ... setup ...
}
```

**行为完全一致，桩模式未受影响。**

---

## 四、现在可以做的阶段 B

前提已满足：

| 条件 | 状态 |
|---|---|
| 标志 per-core 正确 | ✅ `cores-marked=6, disagrees=0` |
| 标志能区分首次/二次 | ✅ 每个核独立 |
| 行为可回退 | ✅ 当前分支仍由 CPUID 决定 |

**阶段 B 的内容**：把分支改成

```c
if (secondPass != 0)      // 让标志说了算
    return STATUS_SUCCESS;
... setup ...
```

**但阶段 B 在桩模式下不会有任何变化** —— 因为 `early-returns=0` 说明
没有任何核走第二次。**阶段 B 只有在真实 guest 接管（阶段 C）之后才有意义。**

---

## 五、当前状态

```
bin = 部署 = F5EE488A60CF8B87
virt-counts: entries=6  early-returns=0  cores-marked=6  disagrees=0
RUNNING / degraded=0 / stop 4.12s / invmrun=0x0
```

**桩模式（1.2×）完全可用。**

---

## 六、下一步

按方案，阶段 B 之后是阶段 C（关键里程碑）：

```
B: 让标志决定分支        ← 桩模式下无变化，可安全做
C: guest RIP 用捕获点     ← ★ 关键：early-returns 应开始增长
D: 停止走 DPC + vmmcall
E: 退出换 iretq
```

**C 的验证标准很明确**：`early-returns` 从 0 变成非 0。
那一刻才说明"guest 接管 C 帧"这个模型真的成立。

**如果 C 之后 `early-returns` 仍是 0，说明模型没建立，D/E 就绝对不要做。**
