# YuanGuardHV 文档

> 2026-11-08（9.583 重新测量）。**根目录 8 份，就这些。** 其余全在 `archive/`。
> 本页每一个数字都是量出来的，不是估的 —— 上一版有 5 处过期、且**没有检查器管文档**，见
> [`archive/2026-11-08/DOCS_INDEX_AUDIT_20261108.md`](archive/2026-11-08/DOCS_INDEX_AUDIT_20261108.md)。

---

## 按问题找文档

| 你想… | 读 | 说明 |
|---|---|---|
| **接手项目 / 知道现在什么状态** | **[`QUICKSTART.md`](QUICKSTART.md)** | 3 分钟上手、当前状态、下一步、八个坑、方法论 |
| **知道为什么这么设计、踩过哪些雷** | [`RESUME_PROMPT.md`](RESUME_PROMPT.md) | 深一层：用户铁律、架构约束、观测手段、坑表（691 行）|
| **知道哪些做了、哪些不做** | [`TIER_LEDGER.md`](TIER_LEDGER.md) | 唯一权威的完成度表：8 完成 / 9 不做 / 1 需重启 |
| **知道还欠什么、下一版做什么** | **[`ISSUES.md`](ISSUES.md)** | **★ 唯一的问题清单**：P0–P3 分级 + 版本规划 + 判据纪律 |
| **了解 Windows-as-Guest** | **[`WINDOWS_AS_GUEST_FINDINGS.md`](WINDOWS_AS_GUEST_FINDINGS.md)** | 全部结论：能跑、快 38%、停不下来、根因、已排除解法 |
| **写内核代码前** | [`KERNEL_API_CHECKLIST.md`](KERNEL_API_CHECKLIST.md) | 三次蓝屏换来的 API 契约 |
| **改文档之后** | `YuanGuardHV/tests/README.md`（代码树内，用路径而非链接）| `docs_check.ps1` 会拦住过期数字（构建期自动跑）|
| **查某条变更** | [`YUANMOD_HANDOFF_CURRENT.md`](YUANMOD_HANDOFF_CURRENT.md) | 9.500 起逐条记录，**开头有 80 条索引**（标题 + 真实行数）|
| **查更早的变更** | [`archive/_ARCHIVE_HANDOFF_9.1-9.299.md`](archive/_ARCHIVE_HANDOFF_9.1-9.299.md) | 9.1–9.499 |

---

## 当前状态（2026-11-14 10:50 实测）

```
驱动          : STOPPED
镜像          : bin == ad81fc5b833129d4
                部署 C:\yghv_pa.sys 仍是 BCC4FECDEA8DCEE5（9.588 的构建还没部署）
默认配置      : 真 guest + 全核（9.577 起），12 线程 1.05-1.08×
卸载          : 0.07 s，invmrun at unload = 0x0
台账          : 8 完成 / 9 不做 / 1 需重启（Tier 6.1 启动期验收）
HEAD          : 不写死 —— 写进文档的那一刻就开始过期，看 `git log -1`
最新编号      : 9.602
```

> `镜像` 与 `最新编号` 两行由 `docs_check.ps1` 核对（前者比 `bin/yuanguard_hv.sys`
> 的真实 SHA-256 前 16 位，后者比交接文档的最高记录号）。**写死 HEAD 是上一版的做法，
> 它必然过期** —— 提交发生在写文档之后，这个数字在任何时刻都不可能是对的。

---

## archive/ 里有什么

| 目录 | 份数 | 内容 |
|---|---|---|
| [`archive/2026-11-08/`](archive/2026-11-08/) | **26** | 本会话过程记录：RealGuestMode 实验全过程、`iretq` 失败、卸载修复、等待循环审计、docs 审计 |
| [`archive/design/`](archive/design/) | **7** | Tier 2/4/5/6/7/8-9 设计计划书。**实施已结束，留作设计参考** |
| [`archive/*.md`](archive/) | **14** | 13 份历史文档（历史交接、2026-08 的评审与研究、S8 旧工作清单）+ 1 份逐份索引。**先读 [`archive/README.md`](archive/README.md)** —— 每份文档是什么、该不该读，都在那里 |
| [`superpowers/`](superpowers/) | **2** | 2026-08-11 Phase 3 进程保护的设计书 + 实现计划（**唯一入口**，此前未在本页登记）|

**值得翻的几份**：

| 想了解 | 读 |
|---|---|
| RealGuestMode 是怎么一步步排除的 | `archive/2026-11-08/REWRITE_STARTUP_MODEL.md` 起，按 STAGE_A → C → D 顺序 |
| 参考项目 Type2-AMD-HV 怎么做停止 | `archive/2026-11-08/DEVIRTUALIZE_REFERENCE.md` |
| 卸载 4 秒怎么修的 | `archive/2026-11-08/UNLOAD_4S_FIX_20261108.md` |
| 两个计数器混用的问题 | `archive/2026-11-08/COUNTER_CONFUSION_20261108.md` |
| 索引本身是怎么错的 | `archive/2026-11-08/DOCS_INDEX_AUDIT_20261108.md` |
| 2026-08 的 114 个 OS-as-guest 实验 | `archive/YGHV_OS_AS_GUEST_SUMMARY_20260815.md` |
| Phase 3 的原始设计意图 | `superpowers/specs/2026-08-11-phase3-protection-design.md` |

---

## 维护约定

1. **根目录只放"现在还有用"的。** 结论进根目录，过程进 `archive/YYYY-MM-DD/`。
2. **本索引是唯一入口** —— 根目录与 `superpowers/` 下新增文档必须在此登记。
3. **编号单调递增**，`9.1–9.499` 在归档，`9.500+` 在主文档。
4. **`TIER_LEDGER.md` 只在实测之后才写「完成」** —— 写「完成」而实际不可用，比写「未做」更糟。
5. **`ISSUES.md` 是唯一的问题清单。** `TIER_LEDGER`（Tier 完成度）、`RESUME_PROMPT` §8、
   `README` 的已知限制是**历史视角**，新增待办只进 `ISSUES.md`，避免三处冲突。
6. **写数字前先量，写完再量一次。** 份数用 `ls | wc -l`，哈希用 `Get-FileHash`，
   编号用 `grep -o '9\.[0-9]\{3\}' | sort -V | tail -1`。**整理动作本身会让旧数字失效。**
