# docs 目录索引

> 22 个文件，按用途分成四组。**新人从 `RESUME_PROMPT.md` 开始读。**

---

## 一、入口（先读这三个）

| 文件 | 用途 | 大小 |
|---|---|---|
| **`QUICKSTART.md`** | **★★ 跨会话快速承接** —— 三分钟上手、当前状态、下一步、七个坑。**新会话先读这个** | 9 KB |
| **`RESUME_PROMPT.md`** | 深一层的基线、雷区、方法论、加速查证 | 49 KB |
| `YUANMOD_HANDOFF_CURRENT.md` | 交接主文档：当前状态、下一步、9.300–9.497 变更记录 | 478 KB |
| `TIER_LEDGER.md` | Tier 清单最终台账：8 完成 / 8 不做(附理由) / 1 需重启 | 7.6 KB |

## 二、写内核代码前必读

| 文件 | 用途 |
|---|---|
| **`KERNEL_API_CHECKLIST.md`** | **★ 三次蓝屏换来的 API 契约核对清单** |

## 三、历史归档（考古用）

| 文件 | 内容 |
|---|---|
| `_ARCHIVE_HANDOFF_9.1-9.299.md` | 早期变更记录（2026-08 到 2026-10 中），523 KB，6319 行 |
| `WINDOW_HANDOFF_20261031.md` | 上一轮窗口交接快照 |
| `YUANMOD_NEXT_WINDOW_PROMPT.md` | 更早的窗口接手提示词（2026-10-11，已被 `RESUME_PROMPT.md` 取代） |
| `YUANMOD_HANDOFF_NEXT_WINDOW.md` | 最早期的接手文档（2026-08-14） |
| `TASKS.md` | 早期任务清单与 P0 复核结论（2026-08-14） |

## 四、Tier 计划原始文档

这些是**计划书**，不是状态报告。状态请查 `TIER_LEDGER.md`。

| 文件 | 覆盖的轴 |
|---|---|
| `MEM_ENGINE_PLAN.md` | 总纲 |
| `MEM_ENGINE_PLAN_TIER2.md` | Tier 2：访问通道 |
| `MEM_ENGINE_PLAN_TIER4.md` | Tier 4：视图与时间 |
| `MEM_ENGINE_PLAN_TIER5.md` | Tier 5：内存语义（TSC / CPUID） |
| `MEM_ENGINE_PLAN_TIER6.md` | Tier 6：跨 VM |
| `MEM_ENGINE_PLAN_TIER7.md` | Tier 7：自我隐藏 |
| `MEM_ENGINE_PLAN_TIER8_9.md` | Tier 8 生命周期 + Tier 9 控制面 |
| `TIER4_WATCH_DESIGN.md` | Tier 4 watch 的专项设计 |

## 五、专题研究（按日期）

| 文件 | 主题 |
|---|---|
| `YGHV_OS_AS_GUEST_RESEARCH_20260814.md` | OS-as-guest 可行性研究 |
| `YGHV_OS_AS_GUEST_SUMMARY_20260815.md` | 同上，阶段性总结 |
| `YGHV_SIMPLEVM_LEVERAGE_20260918.md` | SimpleSvm/HelloAmdHv 构型复审 + 本机 CPU 权威记录 |
| `YGHV_FULL_REVIEW_20260814.md` | 全面代码审查（YGHV-REV-001..043） |
| `YGHV_STEALTH_AUDIT_20260813.md` | 隐藏矩阵与反侦查审计 |
| `YGHV_HOOK_LOCK_AND_0x5AA_REDESIGN_20260813.md` | hook lock 与 0x5AA 重设计 |

---

## 编号体系

变更记录用 `9.x` 编号，**单调递增，不重复使用**。当前最新 **9.497**。

- `9.1`–`9.299` → `_ARCHIVE_HANDOFF_9.1-9.299.md`
- `9.300`–`9.497` → `YUANMOD_HANDOFF_CURRENT.md`

归档不是删除。每一次决策、每一个蓝屏的原因、每一次被推翻的推断都在里面。
写新记录时不需要回头改归档。

---

## 维护约定

1. **新变更记进 `YUANMOD_HANDOFF_CURRENT.md`，编号接着上一个，不跳号。**
2. 主文档超过约 800 KB 时，把 `9.x00` 之前的段切进归档文件，并在主文档顶部更新分界。
3. `TIER_LEDGER.md` 的状态只在**实测之后**才改成「完成」——文档写「完成」而实际不可用，
   比写「未做」更糟（9.495 就修正了三处这种）。
