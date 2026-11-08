# YuanGuardHV 文档索引

> 最后更新：2026-11-08。**新会话从这里开始读。**
>
> 结构约定：**结论在根目录，过程在 `archive/`。**

---

## 一、五分钟上手

| 想知道什么 | 读哪份 |
|---|---|
| **项目是什么、怎么跑、当前状态、七个坑** | **[`QUICKSTART.md`](QUICKSTART.md)** ★ 新会话先读这个 |
| **深一层：基线、雷区、方法论** | [`RESUME_PROMPT.md`](RESUME_PROMPT.md) |
| **哪些完成了、哪些不做（附理由）** | [`TIER_LEDGER.md`](TIER_LEDGER.md) |
| **最近改了什么（逐条，9.500+）** | [`YUANMOD_HANDOFF_CURRENT.md`](YUANMOD_HANDOFF_CURRENT.md) |

---

## 二、核心结论

| 文档 | 内容 |
|---|---|
| **[`WINDOWS_AS_GUEST_FINDINGS.md`](WINDOWS_AS_GUEST_FINDINGS.md)** ⭐ | **Windows-as-Guest 全部结论的唯一权威总结**：能跑、快 38%、停不下来、根因、已排除的解法、唯一剩下的方向 |
| [`YGHV_OS_AS_GUEST_SUMMARY_20260815.md`](YGHV_OS_AS_GUEST_SUMMARY_20260815.md) | 2026-08 的 114 个实验总结。**其「本机不可行」结论已被上文修正** —— 8 月失败的原因是「host 代管中断」，不是「Windows 当 guest」 |
| [`YGHV_FULL_REVIEW_20260814.md`](YGHV_FULL_REVIEW_20260814.md) | 全面代码审查（YGHV-REV-001..043）|

---

## 三、写内核代码前必读

| 文档 | 内容 |
|---|---|
| **[`KERNEL_API_CHECKLIST.md`](KERNEL_API_CHECKLIST.md)** | **★ 三次蓝屏换来的 API 契约核对清单** |

---

## 四、设计文档（计划书，不是状态报告）

状态请查 [`TIER_LEDGER.md`](TIER_LEDGER.md)。

| 文档 | 覆盖的轴 |
|---|---|
| [`MEM_ENGINE_PLAN.md`](MEM_ENGINE_PLAN.md) | 总纲 |
| [`MEM_ENGINE_PLAN_TIER2.md`](MEM_ENGINE_PLAN_TIER2.md) | Tier 2：访问通道 |
| [`MEM_ENGINE_PLAN_TIER4.md`](MEM_ENGINE_PLAN_TIER4.md) | Tier 4：视图与时间 |
| [`MEM_ENGINE_PLAN_TIER5.md`](MEM_ENGINE_PLAN_TIER5.md) | Tier 5：内存语义（TSC / CPUID）|
| [`MEM_ENGINE_PLAN_TIER6.md`](MEM_ENGINE_PLAN_TIER6.md) | Tier 6：跨 VM |
| [`MEM_ENGINE_PLAN_TIER7.md`](MEM_ENGINE_PLAN_TIER7.md) | Tier 7：自我隐藏 |
| [`MEM_ENGINE_PLAN_TIER8_9.md`](MEM_ENGINE_PLAN_TIER8_9.md) | Tier 8 生命周期 + Tier 9 控制面 |

---

## 五、归档（考古用，不影响当前工作）

| 位置 | 内容 |
|---|---|
| [`archive/`](archive/) | 历史交接、专题研究、`_ARCHIVE_HANDOFF_9.1-9.299.md`（522 KB，6319 行）|
| [`archive/2026-11-08/`](archive/2026-11-08/) | **2026-11-08 本次会话的 24 份过程记录** |

### 归档里值得看的

| 想看 | 读 |
|---|---|
| RealGuestMode 是怎么一步步排除的 | `archive/2026-11-08/REWRITE_STARTUP_MODEL.md` 起，按 STAGE_A → C → D 顺序 |
| 参考项目怎么做停止 | `archive/2026-11-08/DEVIRTUALIZE_REFERENCE.md` |
| 卸载 4 秒怎么修的 | `archive/2026-11-08/UNLOAD_4S_FIX_20261108.md` |
| 两个计数器混用的问题 | `archive/2026-11-08/COUNTER_CONFUSION_20261108.md` |
| 等待循环/计数器审计 | `archive/2026-11-08/AUDIT_WAITS_COUNTERS_20261108.md` |
| 2026-08 的 OS-as-guest 研究 | `archive/YGHV_OS_AS_GUEST_RESEARCH_20260814.md` |

---

## 六、当前状态（2026-11-08）

```
驱动          : STOPPED
镜像          : bin == 部署（sha256 见 QUICKSTART）
桩模式        : 1.2×，卸载 0.046 s，invmrun at unload = 0x0
RealGuestMode : 编译期封禁（一次会话 4 次蓝屏后，见 WINDOWS_AS_GUEST_FINDINGS.md）
台账          : 8 完成 / 9 不做 / 1 需重启（Tier 6.1 启动期验收）
```

---

## 七、编号体系

变更记录用 `9.x` 编号，**单调递增，不重复使用**。当前最新 **9.557**。

| 范围 | 位置 |
|---|---|
| `9.1` – `9.299` | `archive/_ARCHIVE_HANDOFF_9.1-9.299.md` |
| `9.300` – 当前 | `YUANMOD_HANDOFF_CURRENT.md` |

**归档不是删除。** 每一次决策、每一个蓝屏的原因、每一次被推翻的推断都在里面。

---

## 八、维护约定

1. **结论进根目录，过程进 `archive/YYYY-MM-DD/`。** 根目录只放"现在还有用"的。
2. **新变更记进 `YUANMOD_HANDOFF_CURRENT.md`**，编号接着上一个，不跳号。
3. **`TIER_LEDGER.md` 的状态只在实测之后才改成「完成」** ——
   文档写「完成」而实际不可用，比写「未做」更糟（9.495 修正过三处）。
4. **本索引是唯一入口** —— 根目录新增文档必须在此登记。
5. **`YUANMOD_HANDOFF_CURRENT.md` 超过约 800 KB 时**，
   把旧段切进 `archive/`，并在主文档顶部更新分界。
