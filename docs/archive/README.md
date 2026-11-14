# archive/ 索引

> 这里的东西**不是当前状态**，是过程与历史。当前状态见 [`../INDEX.md`](../INDEX.md)。
> 本文件补上 `docs_check` 无法判断的东西：**每份文档是什么、什么时候有用**。

---

## 交接与工作清单

| 文档 | 时间 | 是什么 | 什么时候有用 |
|---|---|---|---|
| [`_ARCHIVE_HANDOFF_9.1-9.299.md`](_ARCHIVE_HANDOFF_9.1-9.299.md) | 2026-08 ~ 10 | **1027 KB**，9.1–9.499 的逐条工程记录 | 只溯源某条旧结论时查，**不要从头读** |
| [`RESUME_PROMPT_S8_WORKLIST_20260814.md`](RESUME_PROMPT_S8_WORKLIST_20260814.md) | 2026-08-14 | 原 `RESUME_PROMPT.md` 第 8 节（747 行），已被 `TIER_LEDGER` 取代 | 想知道"曾经计划过什么" |
| [`TASKS.md`](TASKS.md) | 2026-08-09 | 最早的 Tier 任务清单 | 考古项目起点 |
| [`WINDOW_HANDOFF_20261031.md`](WINDOW_HANDOFF_20261031.md) | 2026-10-31 | 窗口交接（C 线时期） | 想了解 C 线换设计的上下文 |
| [`YUANMOD_HANDOFF_NEXT_WINDOW.md`](YUANMOD_HANDOFF_NEXT_WINDOW.md) | 2026-08-14 | 早期新窗口提示词 | 历史；现行的是 `../QUICKSTART.md` |
| [`YUANMOD_NEXT_WINDOW_PROMPT.md`](YUANMOD_NEXT_WINDOW_PROMPT.md) | 2026-10-11 | 9.290 / C6 冻结时期的新窗口提示词 | 历史；**其中的状态已全部过期** |

## 设计与审计

| 文档 | 时间 | 是什么 | 什么时候有用 |
|---|---|---|---|
| [`TIER4_WATCH_DESIGN.md`](TIER4_WATCH_DESIGN.md) | 2026-10-17 | NPF watchpoint 的完整裁决循环设计（起因是一次硬冻结） | 改 `mem-watch` 前读 |
| [`YGHV_STEALTH_AUDIT_20260813.md`](YGHV_STEALTH_AUDIT_20260813.md) | 2026-08-13 | 全链路隐藏与反侦查审计 | 评估隐藏能力边界 |
| [`YGHV_HOOK_LOCK_AND_0x5AA_REDESIGN_20260813.md`](YGHV_HOOK_LOCK_AND_0x5AA_REDESIGN_20260813.md) | 2026-08-13 | hook lock 与 `0x5AA` 命名重设计 | 查 `0x5AA` 相关历史 |
| [`YGHV_FULL_REVIEW_20260814.md`](YGHV_FULL_REVIEW_20260814.md) | 2026-08-14 | 全面代码审查（YGHV-REV-001..043） | 想了解 8 月那次全仓审查 |
| [`design/`](design/) | — | 7 份 Tier 2/4/5/6/7/8-9 设计计划书 | **实施已结束**，查设计意图用 |

## OS-as-guest 研究（2026-08 那一条线）

| 文档 | 是什么 | 注意 |
|---|---|---|
| [`YGHV_OS_AS_GUEST_RESEARCH_20260814.md`](YGHV_OS_AS_GUEST_RESEARCH_20260814.md) | 8 月的研究方案 | 结论已被 9.539 起的实测推翻适用范围 |
| [`YGHV_OS_AS_GUEST_SUMMARY_20260815.md`](YGHV_OS_AS_GUEST_SUMMARY_20260815.md) | 114 个实验的总结，结论是"本机不可行" | **那个结论错了** —— 见 [`../WINDOWS_AS_GUEST_FINDINGS.md`](v0.4.2/WINDOWS_AS_GUEST_FINDINGS.md) 第六节的更正 |
| [`YGHV_SIMPLEVM_LEVERAGE_20260918.md`](YGHV_SIMPLEVM_LEVERAGE_20260918.md) | SimpleSvm / HelloAmdHv 构型复审 + C0/C1 实机验证方案 | 含本机 CPU 权威记录 |

## 2026-11-08 会话过程记录

[`2026-11-08/`](2026-11-08/) —— 26 份。按主题：

| 主题 | 读这些 |
|---|---|
| RealGuestMode 是怎么一步步排除的 | `REWRITE_STARTUP_MODEL.md` 起，按 STAGE_A → C → D 顺序 |
| 参考项目 Type2-AMD-HV 怎么做停止 | `DEVIRTUALIZE_REFERENCE.md` |
| 卸载 4 秒怎么修的 | `UNLOAD_4S_FIX_20261108.md` |
| 两个计数器混用的问题 | `COUNTER_CONFUSION_20261108.md` |
| 等待循环与计数器审计 | `AUDIT_WAITS_COUNTERS_20261108.md` |
| 文档索引本身是怎么错的 | `DOCS_INDEX_AUDIT_20261108.md` |
| 文档精简记录 | `DOCS_SLIM_20261108.md` |

---

## 维护约定

1. **新增 archive 文档必须在本文件登记**（`docs_check` 只能查链接，查不出"这是什么"）。
2. 历史记录**不改写**。发现旧结论错了，在**现行文档**里写更正并指过来，
   而不是回去把原文改掉 —— 否则无法复核当时为什么那么判断。
