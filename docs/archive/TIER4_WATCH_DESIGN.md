# Tier 4.6 设计：NPF 监控点（watchpoint）的完整裁决循环

> 2026-10-17。起因：`mem-watch add` 之后机器**硬冻结**（无 dump、无 BSOD，21:06:56）。
> 本文是**设计文档**，不含可执行改动。先想清楚，再动手。

---

## 一、事故回顾：我做错了什么

### 1.1 现象
```
21:07:51  W2: mem-watch add OK
21:06:56  意外关闭（无 dump、无 BSOD）→ 21:08:52 重启
```

### 1.2 我实现的（`mem_watch.c`）
```c
int yghv_watch_add(uint64_t gpa, uint32_t kinds) {
    ...
    yghv_protect_npt_set_perm(page, NPT_PERM_PRESENT);   // 只做这一半
    ...
}
```

撤销写权限 → 目标进程任何写都触发 `#NPF`。

### 1.3 我漏掉的
`NPF handler` 里**没有 watch 页的裁决分支**：

```c
yghv_protect_on_npf_write_bare(curCr3, cpl, gpa, &rearmGpa, &flip)
    → g_protect.active ? 查 g_protect 表 : return YGHV_NPF_NONE
    → watch 页不在 g_protect 表 → YGHV_NPF_NONE
    → cpp 侧 NONE → SvInjectPageFault()
    → 目标进程写失败
    → 目标重试 → 又一次 NPF → 无限循环 → 冻结
```

**这不是"参数写错"，是"机制只实现了一半"。**

---

## 二、为什么这是第三次同类失误

| # | 场景 | 我实现的一半 | 漏掉的一半 |
|---|---|---|---|
| 9.307 | 自建映射 | 改 PTE 指向新帧 | PFN 引用计数 |
| 9.311 | NPT remap | 改表项地址 | 完整属性位 |
| **本次** | **watch** | **撤销权限** | **恢复权限 + 单步重试** |

**共同模式：我把"状态改变"当成了"功能完成"，但每一个都还需要一个配对的反向操作。**

**根因**：我没有把机制写成"循环"来想，而是写成"动作"来想。

---

## 三、完整的 watchpoint 循环（设计）

### 3.1 现有 armed 页的完整循环（可参照的正确答案）

```
① arm_page:  npt_set_page_perm(PRESENT)          撤写权限
② guest 写 → #NPF
③ on_npf_write_bare:
     ALLOW: npt_set_page_perm(PRESENT|WRITABLE)  恢复写权限
            *rearm_gpa_out = gpa                  记下要重新武装的页
            *flip_out = 1
④ cpp 侧:  Rflags |= TF                        单步
           写入重试（权限已恢复）→ 成功
⑤ #DB (单步异常)
     arm_page_bare(rearmGpa):  npt_set_page_perm(PRESENT)   重新撤权
     Rflags &= ~TF
⑥ 回到 ②
```

**关键点**：
- **恢复权限是必须的**，否则 guest 永远写不进去
- **单步（TF）是必须的**，否则无法在"写完成"之后重新武装
- **`rearm_gpa_out` 是必须的**，因为 handler 需要知道给哪一页重新上锁

### 3.2 watchpoint 需要的循环（同样的骨架，不同的裁决）

```
① watch_add:  npt_set_page_perm(PRESENT)          撤写权限（保留读）
② guest 写 → #NPF
③ NPF handler:
     ★ 新增分支：yghv_watch_match(gpa, WRITE) ?
         → yghv_watch_record(...)                  记录事件
         → npt_set_page_perm(PRESENT|WRITABLE)     恢复权限（关键！）
         → *rearm_gpa_out = gpa
         → *flip_out = 1
         → return YGHV_NPF_ALLOW                   ← 复用现有 ALLOW 路径
④ cpp 侧:  TF 单步 → 写入重试成功
⑤ #DB:  arm_page_bare(rearmGpa)                   重新撤权
⑥ 回到 ②
```

**核心洞察：watchpoint 不需要新机制，它需要复用 `ALLOW` 的完整路径，只是在恢复权限之前多记录一次事件。**

### 3.3 为什么不能简单地"永久撤销权限"

如果只撤权不恢复：
- 目标进程的写永远失败
- 它重试 → 每次重试都触发 NPF
- **CPU 100% 在 NPF 里循环** → 看门狗/系统挂起

**这就是本次冻结的机制。**

---

## 四、实现清单（按依赖顺序）

### 4.1 `mem_watch.c` 需要新增

```c
/* 在 NPF 里调用：watch 页命中时返回 1，调用方走 ALLOW 路径 */
int yghv_watch_on_npf(uint64_t cr3, uint64_t rip, uint64_t gpa,
                      uint32_t kind, uint32_t cpl);
```

语义：
1. `yghv_watch_match(gpa, kind)` → 不命中返回 0
2. `yghv_watch_record(...)` 记录
3. `yghv_protect_npt_set_perm(gpa, PRESENT|WRITABLE)` **恢复权限**
4. 返回 1

**必须检查返回值**：如果恢复权限失败，**不能**返回 1（否则又是无限循环）。

### 4.2 `svm_simplevm206.cpp` 需要改

在 `VMEXIT_NPF` 分支里，**在现有 `on_npf_write_bare` 调用之前**插入：

```c
if (writeFault &&
    yghv_watch_on_npf(curCr3, NRip, gpa, 0x2, cpl)) {
    *rearmGpaOut = gpa;
    flip = 1;
    vr = YGHV_NPF_ALLOW;        /* 复用现有 ALLOW 路径 */
} else {
    vr = yghv_protect_on_npf_write_bare(...);
}
```

**为什么放在 `on_npf_write_bare` 之前**：watch 页不在 `g_protect` 表里，
`on_npf_write_bare` 会返回 `NONE`。必须先问 watch。

### 4.3 读监控（kind = READ）

读监控需要撤销 `PRESENT` 位，但那样**目标连读都做不了**。
正确的做法：
- 撤 `PRESENT` → 任何访问都 NPF
- handler 里**立即恢复 `PRESENT`**（但保留只读，不给写）
- 记录事件
- 让读通过

**代价**：每次读都 NPF，性能极差。**只适合短时监控，不适合长期开启。**

### 4.4 执行监控（kind = EXEC）

**当前不可实现**：
- NPT 只有 `NX` 位，没有"按页允许执行但记录"的机制
- `NX=0` → 执行不产生 NPF → 无法记录
- `NX=1` → 执行产生 NPF → 但整个页不可执行，目标无法运行

**结论：执行监控在当前架构下不可行。** 需要 Intel PT 或 AMD IBS 之类的硬件追踪，
那不是 NPF 能做的。**这一项应该从 4.6 里剔除。**

---

## 五、诚实的边界（必须写进文档）

| 能力 | 是否真的做到 |
|---|---|
| 无数量限制的监控点 | ✅ 16 个页（`YGHV_WATCH_MAX_PAGES`），远超 4 个 DR |
| 不改代码（无 int3） | ✅ 只改 NPT 权限 |
| 不碰 DR0-DR3 | ✅ 目标读 DR 看不到 |
| **写监控** | ✅ 完整循环可实现 |
| **读监控** | ⚠️ 可实现但每次读都 NPF，性能极差 |
| **执行监控** | ❌ **不可行**（NPT 无此粒度） |
| **按字节粒度** | ❌ 只有页粒度 |
| **多核一致性** | ⚠️ 一个核在 NPF 里恢复权限时，另一个核可能已经在写 —— 会漏事件 |

---

## 六、风险评估

| 风险 | 等级 | 说明 |
|---|---|---|
| 恢复权限失败 → 无限 NPF | **高** | 必须检查返回值，失败时返回 DENY 而不是 ALLOW |
| 多核竞态漏事件 | 中 | 可接受（监控本来就是采样式） |
| 读监控性能 | 中 | 文档标注"仅短时" |
| 与现有 armed 页冲突 | 中 | 同一页同时 armed + watched 需要明确的优先级 |
| 冻结风险 | **高** | 本次已发生一次 |

**优先级规则（必须先定）**：
```
如果一页同时在 g_protect（armed）和 g_watch 里：
  armed 优先（现有保护语义不应被 watch 破坏）
  watch 只记录，不改变 armed 的裁决
```

---

## 七、验证计划（如果实现）

| # | 断言 | 方法 |
|---|---|---|
| 1 | watch add 后目标进程仍能正常运行 | 目标进程存活 > 5 秒 |
| 2 | 写触发事件记录 | drain 返回 ≥1 事件 |
| 3 | 事件 gpa 正确 | 与 mem-pte 的 PA 比对 |
| 4 | **写最终成功**（不无限重试） | 读回 == 写入值 |
| 5 | watch del 后不再记录 | drain 返回 0 |
| 6 | **全程无冻结** | 每步都有超时保护 |
| 7 | 与 armed 页共存 | 同一页 arm + watch，验证保护仍生效 |

**关键**：断言 4 和 6 是本次失败的直接原因，**必须验证**。

---

## 八、我的建议

**不要现在实现。**

理由：
1. 我已经连续三次在"机制只做一半"上失误
2. 今天 8 次崩溃/冻结
3. 4.6 的**执行监控部分本来就不可行**，能做的只有写监控
4. 写监控的实用价值 < 风险

**建议**：
- **把本设计文档提交**，作为未来实现的基础
- **代码标注"实现不完整，见设计文档"**
- **`mem-watch add` 改为直接返回失败**（安全阀），避免误用导致冻结
- **转向收线**：整理已完成的 6 个 Phase + Tier 4.1/4.3，推送

---

## 九、如果将来要实现，第一步是什么

**不是写代码，是写一个"单页、单核、带完整超时保护"的最小验证**：

```
1. 目标进程一个页
2. watch add（撤写权限）
3. 目标写一次
4. 立即检查：目标是否还活着？（超时 2 秒）
5. 如果活着，检查事件是否记录
6. 检查写是否最终成功
```

**任何一步失败，立即 stop 驱动，不继续。**

这次的教训是：**验证要能证明"没坏"，而不只是"某个计数器变了"。**
