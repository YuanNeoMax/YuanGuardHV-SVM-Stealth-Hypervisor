# docs 归档整理计划

> 2026-11-08。目标：为新会话留下一个干净、不混淆的交接面。

---

## 一、现状

```
48 个文件，1.56 MB
其中 22 份是 2026-11-08 本会话新建的过程文档
YUANMOD_HANDOFF_CURRENT.md 一份就占 602 KB（还在不断追加）
_ARCHIVE_HANDOFF_9.1-9.299.md 522 KB（已经是归档）
```

**问题**：
1. **22 份日更文档平铺在同一层**，没有入口，新会话不知道从哪读
2. **11-08 的文档里有大量"过程记录"**（某次失败、某次诊断），
   结论已经沉淀但过程文档还在
3. **交接文档 602 KB**，一次读不完
4. **同一个结论散落在多份文档里**（如 RealGuestMode 的结论出现在 8 份里）

---

## 二、分类

### A 类：保留在 docs 根目录（长期资产）

| 文件 | 理由 |
|---|---|
| `QUICKSTART.md` | 上手入口 |
| `INDEX.md` | 索引（需要重写）|
| `TIER_LEDGER.md` | 台账，唯一权威的完成度表 |
| `RESUME_PROMPT.md` | 恢复工作用（需要更新）|
| `YUANMOD_HANDOFF_CURRENT.md` | 交接主文档（需要瘦身）|
| `KERNEL_API_CHECKLIST.md` | 参考 |
| `MEM_ENGINE_PLAN*.md`（7 份）| 设计文档，仍有参考价值 |
| `YGHV_OS_AS_GUEST_SUMMARY_20260815.md` | 2026-08 研究总结，是他路线的依据 |
| `YGHV_FULL_REVIEW_20260814.md` | 全量评审 |

### B 类：移入 `docs/archive/2026-11-08/`（本会话过程记录）

**这 22 份全部移走，但保留**（它们是证据，不是垃圾）：

```
AUDIT_WAITS_COUNTERS_20261108.md
COUNTER_CONFUSION_20261108.md
DEVIRTUALIZE_REFERENCE.md
IRETQ_FAILURE_20261108.md
NEXT_PLAN_20261108.md
REALGUEST_D1_ROOTCAUSE_20261108.md
REALGUEST_EXPERIMENT_20261108.md
REFACTOR_PLAN_RG.md
REWRITE_STARTUP_MODEL.md
SAFETY_PIN_20261108.md
STAGE_A_RESULT_20261108.md
STAGE_A2_RESULT_20261108.md
STAGE_C_DIAG_20261108.md
STAGE_C_SUCCESS_20261108.md
STAGE_D_BLOCKED_20261108.md
STAGE_D_NEGATIVE_20261108.md
STEP1_FAILURE_20261108.md
STEP1_SUCCESS_20261108.md
STEP3_DIAGNOSIS_20261108.md
9_551_FAILURE_ANALYSIS.md
STOP_HANDSHAKE_ROOTCAUSE_20261108.md
UNLOAD_4S_FIX_20261108.md
WINDOWS_AS_GUEST_PLAN.md
```

### C 类：移入 `docs/archive/`（更早的归档）

```
_ARCHIVE_HANDOFF_9.1-9.299.md     （已是归档）
WINDOW_HANDOFF_20261031.md
YGHV_HOOK_LOCK_AND_0x5AA_REDESIGN_20260813.md
YGHV_STEALTH_AUDIT_20260813.md
YGHV_SIMPLEVM_LEVERAGE_20260918.md
YGHV_OS_AS_GUEST_RESEARCH_20260814.md
TASKS.md
YUANMOD_HANDOFF_NEXT_WINDOW.md
YUANMOD_NEXT_WINDOW_PROMPT.md
TIER4_WATCH_DESIGN.md
```

---

## 三、要新建的核心文档

### `docs/WINDOWS_AS_GUEST_FINDINGS.md` ⭐ 最重要

**把 RealGuestMode 的全部结论合成一份**，取代 8 份分散文档。

内容：
- 一句话结论
- 已验证的事实（含实测读数）
- 已排除的解法（三个方向，各自的失败原因）
- 唯一剩下的方向
- 相关文档的索引（指向 archive 里的证据）

### 重写 `docs/INDEX.md`

**分类目录 + 一句话说明**，让新会话 3 分钟知道该读什么。

### 瘦身 `YUANMOD_HANDOFF_CURRENT.md`

**602 KB 太大。** 做法：
- **9.500 之前的全部**移入 `archive/_ARCHIVE_HANDOFF_9.1-9.499.md`（追加到已有归档）
- **只保留 9.500–9.557**，约 60 KB
- 加一份「历史索引」指向归档

**风险**：这个文件是唯一权威的逐条记录。
**必须用脚本做，且验证行数守恒。**

---

## 四、执行顺序

```
1. 建 archive 子目录
2. 移 B 类 23 份 → archive/2026-11-08/
3. 移 C 类 10 份 → archive/
4. 写 WINDOWS_AS_GUEST_FINDINGS.md（新，核心）
5. 重写 INDEX.md
6. 瘦身 YUANMOD_HANDOFF_CURRENT.md（脚本 + 行数守恒验证）
7. 更新 RESUME_PROMPT.md 的入口段落
8. 更新 QUICKSTART.md 的文档地图
9. 全量校验：每个 .md 无控制字符、内部链接不指向已移走的文件
10. 提交
```

---

## 五、不做什么

- **不删任何文件** —— 全部归档，保留证据
- **不改代码** —— 本次只动文档
- **不碰 `_ARCHIVE_HANDOFF_9.1-9.299.md`** —— 它是历史，只追加不修改
