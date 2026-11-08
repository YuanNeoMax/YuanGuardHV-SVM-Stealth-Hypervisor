# Changelog

本项目的版本级变更摘要。逐条工程记录（编号 `9.x`，当前 63 条）在
[`docs/YUANMOD_HANDOFF_CURRENT.md`](docs/YUANMOD_HANDOFF_CURRENT.md)；
本文只回答「两个版本之间发生了什么」。

格式参考 [Common Changelog](https://common-changelog.org)，版本号遵循
[语义化版本](https://semver.org/lang/zh-CN/)。

> **注意本项目的版本号语义**：`0.x` 阶段，MINOR 版本可能包含**破坏性变更**
> （运行时开关语义、默认配置）。每次有此类变更都会在下面明确标注 **Breaking**。

---

## v0.4.1 — 配置不一致可见化 + 真 guest 下重测内存基准

**日期**：2026-11-08
**范围**：v0.4.0..v0.4.1

### Added

- **`RealGuestMode` 覆盖决定现在写进 `progress.log`**（每轮加载一次，含原始注册表值）。
  9.577 反转该值语义后，带着旧约定 `RealGuestMode=1` 的机器会**静默**退回桩模式，
  而可观察症状 `early-returns=0` 在桩模式下也是正常读数 —— 用户分不清
  「你把它关了」和「它默认就是关的」。三种情形实测：

  | 注册表 | `early-returns` | 日志 |
  |---|---|---|
  | 键不存在 | 12 | 无（默认开着） |
  | `=1` | **0** | **两行**（含 `raw=0x1`）|
  | `=0` | 12 | 无（与默认一致）|

### Changed

- `docs/ISSUES.md` 成为**唯一问题清单**（P0–P3 分级 + 版本规划 + 判据纪律）。
  此前清单散在 `TIER_LEDGER` / `RESUME_PROMPT` §8 / `README` 三处且互相冲突。

### Fixed

- **`docs/WINDOWS_AS_GUEST_FINDINGS.md` 自相矛盾**：9.573–9.577 之后，
  同一份文档同时写着「编译期封禁」和「已打通」，而且第七节教人
  「删除那个 `return 0`」——**代码里已经没有那行了**。下一个会话会照它操作然后对不上。
  处置：编号归一（5.1–5.8，原本有两个 5.1/5.2/5.3/5.4），旧内容降为「历史」并保留教训，
  重写一句话结论与第七节。

### Performance — `mem-bench` 在真 guest 下重测

9.449 的 3.96× 是**桩模式**读数。本轮在真 guest 下用同一批字节重测
（判据：`bytes` 相等 + `hits` 相等，见 9.449.4）。

| 模式 | B (pte) 中位数 | faster (A/B) |
|---|---|---|
| 桩 | 18,702,972 cycles | 2.16–2.47×（一个 5.12× 离群）|
| **真 guest** | **21,146,805 cycles** | **1.81–2.38×** |

**B 比 A 快 1.8–2.4×；真 guest 下 B 比桩模式慢约 13%** —— 方向合理：
B 走页表遍历 + 物理读，真 guest 下每次访问过 NPT 翻译；A 走 `MmCopyVirtualMemory`。

> **判据纪律**：A 的方差很大（44–89M cycles），**比值不可靠**，要看 B 的绝对值。
> 9.449 的 3.96× 与这里的 2.4× **不能直接对比** —— 目标进程（javaw 32MB vs
> explorer 6MB）、范围、页密度都不同。

### Verification

```
ALL SOURCE CHECKS PASS   （24 PASS / 0 FAIL）
docs_check PASS          （64 条记录，链接全通）
默认配置                 entries=12 / early-returns=12
卸载                     0.406 s，invmrun at unload = 0x0
镜像                     82BA3E70645B74B8...
```

### Not done (deliberately)

- **P2-1 全核真 guest 挂机 ≥1 小时** —— operator 明确跳过。
  目前最长连续运行仍是约 30 秒 + G25 的 3 分钟。

---

## v0.4.0 — 全核真 guest（默认），性能 1.05×，G25 17/17

**日期**：2026-11-08
**范围**：v0.3.0..v0.4.0（222 个提交，工程记录 9.305–9.577）

### Added

- **全核真 guest 成为默认配置**。真实 Windows 以 guest 态运行在全部 12 个逻辑核上。
- `tools/bench_core.cs`：固定工作量、**带正确性门**的 CPU 基准
  （12 条 lane 必须返回互不相同且非零的 sum）。
- `tests/docs_check.ps1`：**文档门** —— 相对链接、索引行数、空记录、最新编号、归档份数。
- `tests/docs_check_selftest.ps1`：4 个真缺陷的变异自检 + 1 条反向对照。
- `tests/stub_stack_contract.ps1`：`Sv206GuestStub` 体内禁移动 RSP，且必须仍有 `vmmcall`。
- `docs/INDEX.md`、`docs/QUICKSTART.md`、`docs/WINDOWS_AS_GUEST_FINDINGS.md`：
  文档结构重整为「按问题索引」。

### Changed

- **Breaking — `RealGuestMode` 语义反转**：注册表值现在是**禁用开关**。
  旧：`=1` 打开 / 键不存在 = 关。新：`=1` **关闭** / 键不存在 = **开**。
  > 一台机器上如果留着旧约定写下的 `RealGuestMode=1`，升级后会**被它自己的覆盖关掉**。
  > 这个方向是有意选的 —— 事故应该倒向已验证的桩模式。
- **Breaking — `CoreCount` 默认值**：从「物理核数」改为「全部处理器」。
  `0` 或键不存在 = 全核。
- `virt-counts` 在 `early-returns=0` 时的提示文案：不再断言「C frame is never unwound」
  （那句话把健康的桩模式渲染成缺陷），改为按模式分别说明。

### Fixed

- **`0xD1` @ `Sv206LaunchVm+0x1E`（真 guest 卸载必崩，七次蓝屏中的五、六次）**：
  guest 把承载 VMRUN 帧的内核栈销毁了 —— `RtlCaptureContext` 把 guest RIP 设在
  `SvVirtualizeProcessor` 内部，guest 走完 `return` 一路展开到 `Sv206CoreThread` 尾部
  执行 `terminateFn`，**全程在 guest 里**。栈被调度器回收，帧变野指针。
  判据：桩模式 `invmrun at unload = 0x0` / 真 guest `= 0x6`（六个孤儿帧）。
  修法：尾部改为 tick 循环，永不返回。
- **`0x101 CLOCK_WATCHDOG_TIMEOUT`（全核首次运行）**：tick 循环是 `for(;;) { spin; vmmcall; }`，
  每个虚拟化核一个 100% 忙线程；12/12 时 Windows 没有逻辑核处理时钟中断。
  修法：`KeDelayExecutionThread(KernelMode, FALSE, -1ms)`。**CPU 100% → 21–29%。**
- **卸载 4 秒 → 0.046 秒**：等待条件拿 `KeQueryActiveProcessorCountEx()`（12）当上界，
  而 206 只虚拟化 6 个核，**条件不可满足**，每次卸载都空转等超时。
- `safety_checks.ps1` 读的三个文档在重构中被移入 `archive/`，导致这个安全检查
  在 `Get-Content` 上抛出、**一行都没检查**，而且是在 `build.bat` 里抛的。
- 删除两个死变量 `g_S206CoresExited` / `g_VmrunStopped`。后者不只是死，
  它还在污染诊断 —— `snap_vmrun_stopped` 每次都是 0，「没有核停过」与「字段没接线」
  在 dump 里长得一样。

### Removed

- 8 处确认零引用的死代码：`yghv_emit_u8`、`yghv_protect_resolve_va`、
  `yghv_exclude_driver`、`yghv_avic_load_idtr`、`yg_svm_vmload`、`ch_wr32`、
  `yghv_time_readcr3`、`yghv_sentry_find_name`、`Sv206IretSlot`+`S206_IRET_SLOT`。
- **保留**（不是漏掉）：18 个 `*_pad` 变量是故意的页隔离；两个
  `*_unsafe_do_not_call` 是 Phase 2b 的设计记录；`yghv_s206_physical_cores()`
  现在零调用但保留为拓扑查询入口。

### Performance

基准：`tools/bench_core.cs`，12 线程 × 3 亿次 xorshift，固定工作量。

| 配置 | 耗时 (ms) | 相对基线 |
|---|---|---|
| 基线（驱动 STOPPED） | 794 / 801 / 804 | 1.00× |
| 桩模式 6 核（v0.3.0 的默认） | 904 / 1070 | 1.13–1.34× |
| 桩模式 12 核 | 3211 / 3505 | 4.0–4.4× |
| 真 guest 6 核 | 815 / 825 / 875 | 1.03–1.09× |
| **真 guest 12 核（默认）** | **799 / 812 / 823 / 815 / 838** | **1.01–1.08×** |

**成本不在 hypervisor 里**：handler 单次 0.4 µs，全核总 CPU 时间 0.012 s。
桩模式退出量 1,255,119 次 vs 真 guest 27,914 次（45 倍）——
桩的开销来自它自己的 `hlt/vmmcall` 空转。

> **更正 v0.3.0 的一项结论**：v0.3.0 的已知限制写着「12 核持续在 guest 里运行，
> 基线 29% → 加载后 100%，无法在本机消除；唯一解法是换一台有 C-state 的机器」。
> 那句话描述的是**桩模式**。真 guest 下 CPU 21–29%，"换硬件"不再是解法。

### Verification

- **G25 功能验证：17/17 ALL PASS**（全核真 guest，注册表清空走默认）。
  对比 v0.3.0 的 6 核桩模式：15/17（T7/T9 FAIL）。
- T7 判据：hide 之后 **guest 侧读到 `value=0x0`**，不是 seed `0xC4C40000000000` —— DECOY 真实生效。
- 卸载判据：`invmrun at unload = 0x0`，耗时 0.07 s。
- 构建期检查：13 脚本 / 24 条 PASS（含 9 条自检），0 FAIL。

---

## v0.3.0 — 周期同步守卫（绕开 Zen3 NPT TLB 限制的换设计）

**日期**：2026-10-11

### Added

- **周期同步守卫**：既然 NPT 权限翻转无法即时生效，那就不翻转 ——
  页面在 NPT 里**始终 present+WRITABLE**，硬件从不需要失效翻译，限制自然不适用。
  改为后台线程**轮询快照比对**，发现漂移即回滚。**延迟 92 秒 → 2.3 秒（40 倍）。**
- `docs/KERNEL_API_CHECKLIST.md`：三次蓝屏换来的写前检查表。
- `build.bat` 加 `/MAP` —— 崩溃取证时把「模块+偏移」一次映射到函数名。

### Fixed

| Bug | BugCheck | 根因 |
|---|---|---|
| 1 | `0xE2` | `ExAcquireFastMutex` **递归死锁**（不可递归） |
| 2 | `0x3B` | `MmGetVirtualForPhysical` 用于**用户页**（它只映射系统页） |
| 3 | `0x7E` | 注册表 `KEY_VALUE_PARTIAL_INFORMATION.Data` **未对齐** + 无长度校验 |
| 4 | `0x7E` | `KeStackAttachProcess` + `memmove` **野指针**（物理地址跨进程不唯一） |

- 缺陷 C：`DriverUnload` 补 `yghv_trace_close()`（`progress.log` 句柄泄漏）。
- 发现本项目构建用 `/EHs-c-`，**`__try/__except` 完全不生成处理器** —— 此前写的 SEH 是装饰性的。

### Changed

- `config sync <0|1> [interval_ms]` 并入 `config`，不再需要单独的 `config-sync` 往返。
- `YGHV_SYNC_MAX` 16 → 64，新增 `refused` 计数（arm 因表满被拒不再静默）。

### Verification

四步挨个来，全程零蓝屏：最小面 6/6 → 单页 5/5 → 16 页 5/5 → **64 页 5/5**。
性能：每页每轮 ≈2.97 μs；64 页 @20 polls/s ≈ **0.38% 单核**。

---

## v0.2.0 — OS-as-guest 206 里程碑

**日期**：2026-10-05

- vendored 上游 SimpleSvm 逐字进场 + coexist 全核无缝虚拟化 + 512GB identity NPT。
- 206-C1–C5：IOCTL 控制面可用 → 页写保护端到端 → 控制面加固 → 跨进程保护 → 实进程试点。
- 206-C6：假写路线六条 NPT 冲刷路径全部证伪（Zen3 硬件事实）。

---

## v0.1.0 — 首个可用版本

**日期**：2026-08-29

- SVM 初始化 + VMRUN 单核 → 多核 DPC → 裸机回归。
- 内存写保护 / 终止保护 / 句柄保护 / 常驻。
- Java/JNI 客户端、真实进程接入（`protect/unprotect/scan`）。
