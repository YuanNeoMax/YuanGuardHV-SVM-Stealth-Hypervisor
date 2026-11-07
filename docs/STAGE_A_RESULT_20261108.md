# 9.544 阶段 A 结果：`disagrees=4` —— 检查第一次运行就抓到了真问题

> 2026-11-08。**阶段 A 的全部价值在于它什么都没改，却发现了阶段 B 会炸的地方。**

---

## 一、读数

```
virt-counts: entries=6  early-returns=0  secondPass=1  disagrees=4
```

| 量 | 值 | 含义 |
|---|---|---|
| `entries` | **6** | 6 个核各进入 `SvVirtualizeProcessor` 一次 |
| `early-returns` | **0** | 没有一次走早退分支 |
| `secondPass` | **1** | 标志已被设置（说明至少一个核走完了 setup）|
| `disagrees` | **4** | **CPUID 与标志在 4 个核上结论不一致** |

---

## 二、`disagrees=4` 意味着什么

阶段 A 的判断是：

```c
if ((installed != FALSE) != (secondPass != 0))
    InterlockedIncrement64(&g_S206FlagDisagrees);
```

即：**“CPUID 说已安装” 应该等价于 “标志说第二次进入”。**

**实测不等价，而且差 4 次。**

最可能的形态：**标志是全局的（不分核），第一个核设置后，其余 5 个核进入时
`secondPass` 已经是 1，但它们自己的 CPUID 还没被虚拟化 —— 所以返回“未安装”。**

```
核 0: installed=FALSE, secondPass=0 → 一致，执行 setup，设 flag=1
核 1: installed=FALSE, secondPass=1 → 不一致！(计 1)
核 2: installed=FALSE, secondPass=1 → 不一致！(计 2)
核 3: installed=FALSE, secondPass=1 → 不一致！(计 3)
核 4: installed=?       secondPass=1 → 不一致！(计 4)
核 5: installed=TRUE,   secondPass=1 → 一致
```

**这正好解释了 4 vs 6。**

---

## 三、为什么这是"阶段 A 救了一次"

如果直接做阶段 B（让标志决定分支）：

```c
if (secondPass != 0) return;      // ← 阶段 B 的写法
```

**那么核 1–4 会在 setup 之前就 `return`，只做了一次 setup（核 0），
其余核根本没有虚拟化 —— 而 `invmrun` 会显示 1 而不是 6。**

**这会是一个"看起来启动了、实际只虚拟化了一个核"的故障**，
而且因为不蓝屏，很可能被误判成"成功"。

> **阶段 A 的设计（两个都问、旧答案说了算、分歧计数）就是为了在改行为之前
> 让这种问题先显形。它做到了。**

---

## 四、正确的做法（阶段 B 必须这么写）

**标志必须是 per-core 的，不是全局的。**

参考项目就是这样：

```c
// Type2-AMD-HV: Vcpu->Virtualized，每个 VCPU 一个
if (Vcpu->Virtualized) return;
```

我们用的是 `g_S206SecondPass`（全局 LONG），**这在多核下是错的**。

### 修正方案

```c
// 改为 per-core
// 方案 1：放进 VIRTUAL_PROCESSOR_DATA（但那是结构体，加字段 = 9.541 的老问题）
// 方案 2：用一个小数组 + 核索引
static volatile LONG g_S206SecondPass[SVM_MAX_CORES] = { 0 };

// 判断时用当前核索引：
ULONG core = KeGetCurrentProcessorNumber();
if (core < SVM_MAX_CORES && g_S206SecondPass[core]) return;
...
g_S206SecondPass[core] = 1;
```

**为什么不用结构体**：9.541 的教训 —— 往 `VIRTUAL_PROCESSOR_DATA` 加字段
会因页对齐撑大一页，破坏所有硬编码偏移。

**数组是安全的**：新变量，独立分配，不影响任何既有布局。

---

## 五、还要确认的一件事

`entries=6` 但没有一个核走早退 —— 这说明 **6 个核都执行了 setup + VMRUN**，
而 VMRUN 之后**没有一个回到这个函数**（符合"桩不回来"的结论）。

**所以 `early-returns=0` 是真实读数，不是计数器损坏** —— 阶段 A 同时确认了这一点
（因为它现在只在真正的早退分支里递增，不再无条件递增）。

---

## 六、当前状态

```
bin = 部署 = D043588F56516D4A
virt-counts: entries=6  early-returns=0  secondPass=1  disagrees=4
RUNNING / degraded=0 / stop 1.94s / invmrun=0x0
```

**桩模式完全可用，行为未变**（`disagrees` 只计数，不影响分支）。

---

## 七、下一步（阶段 B 的前置）

**先修标志的作用域**：把它从全局改成 per-core 数组。

这一步：
- 不改任何分支逻辑（仍是 `probe && flag == 0` 才 setup）
- 但会让 `disagrees` **降到 0**（每个核的 flag 和它自己的 CPUID 应该一致）

**如果 `disagrees` 降到 0，才说明标志语义正确，阶段 B 才有意义。**
**如果仍是 4，说明我对"为什么分歧"的推断错了，必须重新查。**
