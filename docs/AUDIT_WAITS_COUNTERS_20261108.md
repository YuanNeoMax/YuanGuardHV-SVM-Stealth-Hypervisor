# 审计报告：等待循环与计数器语义（2026-11-08）

> 只读审计。目的：找出与 `active=12 vs 6核` 同类的"靠超时兜底"缺陷。

---

## 一、等待循环（全树共 2 处）

| 位置 | 条件 | 能退出吗 | 结论 |
|---|---|---|---|
| `main.c:5148` | `stopped < waitFor && waited < 20000` | ✅ **已修（9.556）** | 上界现在是 VPD 数（6），可满足 |
| `main.c:5313` | `inside == 0` 或 `waited >= 600` | ✅ **可以** | 检查 `g_inVmrunCount`，它在 `vmrun` 返回时 `lock dec` 归零 |

**第 2 处是健康的**：它等的是"没有核还在 VMRUN 里"，而 `g_inVmrunCount` 由 asm 的
`lock inc/dec`（`Sv206LaunchVm+0x14/+0x21`）维护，`vmrun` 返回即减 —— 条件可以满足。

**所以本树没有第二个 `active=12 vs 6` 那样的不可满足等待。**

---

## 二、计数器语义（47 个，重点看有跨文件读写的）

### 2.1 `g_S206CoresExited` —— **死变量，建议删除**

```
svm_simplevm206.cpp:54   static volatile LONG g_S206CoresExited = 0;
```

**引用统计**：
- **写入点：0 个**（`InterlockedIncrement(&g_S206CoresExited)` 已在 9.555 改为 `...Asm`）
- 读取点：0 个（两个访问器都改读 `...Asm`）
- **只剩注释提到它**（5 处）

**问题：这是纯混淆。** 任何人读代码都会以为它在用，实际它恒为 0。
**9.555 的注释甚至写着 "this file keeps using g_S206CoresExited"——那句话现在是错的。**

### 2.2 `g_VmrunStopped` —— **同样是死变量**

```
svm_simplevm206.cpp:1879   extern "C" { volatile LONG g_VmrunStopped = 0; }
```

**引用统计**：
- 写入点：**1 个，且是清零**（`2020: InterlockedExchange(&g_VmrunStopped, 0)`）
- **没有递增点**
- 读取点：1 个（`2027` 的 `vmrun_stopped_for_trace`）

**而且 4675 行的注释写着**：

> *"g_VmrunStopped is incremented here so the unload waiter has a number that
> actually moves. It counts CORES THAT HAVE LEFT, not ticks."*

**但那个 `case VMEXIT_VMMCALL` 里根本没有递增代码**（4678–4709 只有 `g_S206Vmmcalls`、
RIP 推进、`ExitVm` 判断）。

**注释描述的代码不存在。**

### 2.3 `g_inVmrunCount` —— 健康

- 写入：asm `lock inc`（`+0x14`）/ `lock dec`（`+0x21`）
- 读取：`yghv_s206_invmrun_count()`、`main.c:5314` 的等待循环
- 语义清晰：**当前在 VMRUN 里的核数**

**这是本树里最健康的一个计数器**，而且它是唯一一个语义与实现完全一致的。

---

## 三、真正的发现：注释与代码脱节

**审计没有找到第二个"不可满足的等待"，但找到了另一个同类问题：**

> **代码里有 3 处注释描述的行为，在实际代码中不存在。**

| 位置 | 注释说 | 实际 |
|---|---|---|
| `svm_simplevm206.cpp:4675` | `g_VmrunStopped` 在这里递增 | **没有递增** |
| `svm_simplevm206.cpp:66` | "this file keeps using g_S206CoresExited" | **已不再使用** |
| `main.c:5164` | "woke and left increments g_S206CoresExited" | 现在递增的是 `...Asm` |

**这和 `active=12` 是同一类病：让人相信一件不成立的事。**

`active=12` 让循环相信"等 12 个核停止"是可能的；
这些注释让读者相信"某个变量在被维护"。**两者都不会报错，只会误导。**

---

## 四、处置建议

### 4.1 立即做（低风险，纯清理）

1. **删除 `g_S206CoresExited`**（死变量），把 9.555 注释里那句错误的
   "this file keeps using g_S206CoresExited" 改掉
2. **删除 `g_VmrunStopped`**，或明确标注 `/* never incremented; see 9.419 */`
3. **修正 4675 行注释**：它描述的是一个已经不存在的行为

### 4.2 值得加进检查脚本

**"注释引用的符号必须存在且有写入点"** —— 这条检查能抓住上面全部 3 处。

**但实现成本不低**（要解析注释里的标识符），而收益是防止误导。
**建议先做 4.1 的人工清理，暂不自动化。**

---

## 五、对接下来 RealGuestMode 工作的意义

**审计确认：本树的等待/计数器机制里，没有第二个隐藏的"不可满足条件"会干扰 RealGuestMode 实验。**

**这意味着 RealGuestMode 的失败不能被归因于"另一个计数器 bug"** ——
它的根因（早退后在 guest 里终止线程，`vmrun` 帧悬挂）是结构性的，审计不支持别的解释。

**同时也排除了一种可能**：如果 `g_VmrunStopped` 有隐藏写入，它可能干扰停止判断。
**实际上没有 —— 它是死的。**

---

## 六、审计结论

```
等待循环     : 2 处，都健康（一处已修，一处本就正确）
计数器       : 47 个，1 个健康（g_inVmrunCount），2 个死变量
新发现       : 3 处注释与代码脱节
第二个 active=12 类缺陷 : 不存在
```

**下一步可以放心回到 RealGuestMode —— 没有隐藏的计数器问题会干扰它。**
