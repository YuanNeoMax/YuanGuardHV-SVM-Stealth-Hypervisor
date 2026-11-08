# YuanGuardHV 新窗口无缝衔接提示词（2026-08-14 会话交接）

> 将此文档内容作为新窗口的首条提示词（或保存为文件供新会话读取）。

你正在接管 **YuanGuardHV** 项目——一个 **AMD-V SVM/NPT 隐形 Hypervisor 内核驱动**
（保护 Minecraft/Forge 进程），仓库 **D:\yuanguard**（Windows，分支 main）。

## 当前状态
- **HEAD = `3e92057`**（驱动二进制 SHA `2E79F4C4` = 独立 guest CR3 + 完整 workload
  功能），工作区干净。
- **本机（Ryzen 5 5500）安全**：`C:\yuanguard_hv.sys` = 稳定默认版 `70888311...`，
  服务 `yuanguard` STOPPED，电源正常（IDLEDISABLE=0）。
- **本机定位**：可作构建/静态验证平台；**真实 hypervisor 运行测试风险高**（本机
  在持续 SVM/NPT 负载下会 CPU 级锁死，已 4 次看门狗捕获证实）——需换平台。

## 本窗口已完成的关键工作（先读 `docs/YUANMOD_HANDOFF_CURRENT.md` 9.141-9.158）
1. **冻结根因+修复（9.152-9.155，上窗口）**：guest 原本共享宿主内核 CR3 → guest TLB
   与宿主 TLB 冲突 → 本机 CPU 全核锁死。修复 = **独立 guest CR3**（main.c
   `yghv_build_guest_cr3` 构建专用页表，persistent 各核 `cr3=guest_cr3`）。
2. **hook 调用三重重置根因+修复（9.155）**：`svm_alloc_vcpu` 的 `host_stack` 是 4 页
   （SVM_HOST_STACK_PAGES=4），guest RSP 在第 4 页，但 guest CR3 只映射了
   第 1 页 → call 压栈 NPF → 三重重置。修复 = **映射全部 4 页栈**（gv[] 加到 64）。
3. **完整功能恢复（9.155-9.156）**：persistent c0 跑完整 workload guest（写保护
   NPF/rearm + hook 调用），**12 核全 ACTIVE，selftest PASS，60s+ 稳定**。
4. **本窗口 9.157：OS-as-guest 有界试点重开验证全部 PASS**：step12（单核有界 5000
   轮）、step14（全核 12 核有界）、step16（全核 12 核无缝有界）——counter 精确匹配、
   `sc stop` 干净、恢复稳定版。OS-as-guest 无缝进入/VMEXIT 分发机制在本机完好。
5. **本窗口 9.158：常驻线根因代码级核实 + step20 实机 0xCE**：
   - 9.152 独立 CR3 修复只覆盖合成 resident；OS-as-guest 路径仍 `cr3=yg_read_cr3()`
     共享宿主 CR3 直通（`np_enable=0`、`guest_asid=1`）→ 就是 TLB 冲突冻结根因场景；
     9.156"另一个机制"不准确，实为同一机制。
   - step20（spin 常驻）运行 PASS（resident alive 35s+）但 **`sc stop` 卸载 0xCE 蓝屏**
     （转储 `081426-14453-01.dmp`）：OS-as-guest 线程未注册进多核 join 表、spin
     guest 不检查停止标志 → 模块卸载后线程仍执行驱动代码。首个带转储的卸载失败样本，
     修复方向见 9.158。

## 关键文档索引
- `docs/YUANMOD_HANDOFF_CURRENT.md`（9.141-9.158 = 完整历史；
  9.157-9.158 = 本窗口 OS-as-guest 重开验证 + 0xCE 分析）
- `docs/YGHV_FULL_REVIEW_20260814.md`（全面审查 + 任务清单 REV-001..048）
- `docs/TASKS.md`（任务清单 + 部署基线）
- `docs/YGHV_HOOK_LOCK_AND_0x5AA_REDESIGN_20260813.md`（REV-001 锁序重构方案）
- `docs/YGHV_STEALTH_AUDIT_20260813.md`（隐藏审计）
- `docs/YUANMOD_NEXT_WINDOW_PROMPT.md`（协作铁律 4 条 + 常用命令）

## OS-as-guest 结论（本窗口更新）
本机**不建议直接全核 OS-as-guest**（最重 SVM 负载，冻结风险高）。**有界试点
（12-16）本机可安全验证且已全 PASS**；**常驻线（17+/99/100）本机仍会冻结**——
代码级核实：OS-as-guest 路径仍共享宿主 CR3（`cr3=yg_read_cr3()` 直通），正是 9.152
判定的 TLB 冲突冻结根因，且 guest 需跑整个 Windows 地址空间、无法用独立最小页表
替代。spin 形态（step20）可运行但卸载必 0xCE（未注册 join 表）。**应换平台重开
OS-as-guest 常驻**；本机维持合成 resident 形态。

## 构建/测试命令
- 构建：`cd D:\yuanguard\YuanGuardHV && cmd /c build.bat`
  （静态校验 + 编译 + 签名，输出 `bin\yuanguard_hv.sys`）
- 客户端：`tools\yghv_ctl.ps1`（state/list-pages/selftest/exit-test 等）
- **实机部署测试流程**（每次有冻结风险，需用户备好重启）：
  备份 `C:\yuanguard_hv.sys` → 覆盖 → `sc start yuanguard` →
  验证（watchdog + state/selftest）→ `sc stop` → 恢复 `70888311...`
- 看门狗：`C:\Windows\yghv_watchdog.log`

## 剩余问题
- **P3（需换平台/kd）**：REV-001 锁序重构（HOOK_LOCK 文档有方案）、
  REV-035 INTR/NMI kd 复核。
- **P2 延后**：REV-029（设备 SD）、REV-043/046（R1 族）。
- **本机冻结**：已诊断（4 次捕获 → 平台级），驱动修复为独立 CR3，
  合成 resident 稳定。
- **OS-as-guest 常驻卸载 0xCE（9.158）**：OS-as-guest 线程未注册多核 join 表 +
  spin guest 不检查停止标志；修复方向见 9.158（换平台时一并处理）。

## 协作铁律
1. 任何代码/文档改动前先说明并取得确认。
2. 只修用户反馈的问题。
3. 每次修改记录到 `docs/YUANMOD_HANDOFF_CURRENT.md`。
4. 不擅自回滚；先查 `git status`。

## 下一步候选
- **A**：审查 OS-as-guest 门控代码（os_guest 线程 + os_enter 汇编），写
  "换平台重开 OS-as-guest 准备清单 + 风险分析"（零机器风险）。
- **B**：本机试探性逐步重开 OS-as-guest —— **9.157-9.158 已完成**：有界试点
  （12/14/16）全 PASS；常驻线已代码级证实本机不可行（共享 CR3 TLB 冲突），
  step20 spin 可运行但卸载 0xCE。
- **C**：维持合成 resident 稳定形态，做收尾/清理。
