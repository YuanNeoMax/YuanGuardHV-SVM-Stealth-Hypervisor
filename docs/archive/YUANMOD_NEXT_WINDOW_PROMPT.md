# YuanGuardHV 新窗口提示词（2026-10-11 更新，对应 9.290 / C6 冻结）

你接手 YuanGuardHV（AMD-V SVM/NPT 隐形 Hypervisor）项目，仓库根
D:\yuanguard，分支 main，HEAD 含 9.290（**本地领先 origin/main 约 20+ 提交，
未推送——用户说推才推**）。工作区干净。

【协作铁律（最高优先级）】
1. 任何影响机器状态的命令（驱动加载/sc/压测/重启）**先向用户说明并取得明确
   确认**——重启也必须确认；只读操作（文件/事件日志/进程状态）可直接做。
2. 只修用户反馈的问题，不擅自扩大范围。
3. 每次修改与决策必须记录到 docs/YUANMOD_HANDOFF_CURRENT.md（判读用
   9.2xx 编号顺延）。
4. 构建与蓝屏取证可直接做；构建前 marker 验证（206 构建含 's206b coexist'
   不含 'hook test start'）。
5. **不要用 python -c 编辑 .h/.c 文件**（转义事故清空过 protect.h，git 恢复）——
   用 Edit 工具；跨工具链编辑 .h 前先备份。
6. 不主动推送 GitHub。

【当前状态（2026-10-11，9.290）】
- 机器：Windows 10 Pro for Workstations 19045，Ryzen 5 5500（6C12T Zen3），
  16GB，**驱动已卸载（裸金属）**。
- **当前稳定镜像 = 206c7e**（md5 356c5b6b，归档
  `D:\aaaaaavm\yuanguard_hv_step206c7e_20261011.sys`）= C4/C5/C6(v4 语义)
  全 PASS 线。206c9（假写+INVLPGA 诊断版）归档保留。
- 里程碑全 PASS：206-A/B（coexist 进入/在线卸载）、C1（控制面）、C2（页写
  保护端到端）、C3（看门狗/lasthit）、**C4（跨进程保护 protect-page fn
  0x810/0x811）**、**C5（实进程试点 + WPM 旁路定性）**、**C6 v4（影子假写：
  外来内核写→影子，真页无损，假成功幻觉完备）**。
- **C6 冻结**：NCr3=alt 切换在 Zen3 上延迟生效（1.05M 次重故障自旋 92s，自然
  TLB 逐出才生效）——INVLPGA/VmcbClean=0/TlbControl=1 均无法立即冲刷 NPT
  （GPA→HPA）翻译。**继续定位需要 KD 单步 NPF 路径的 TLB 行为（需第二台机器
  或 KDNET 主机）**。假写默认关（config-fake 0x812），关闭时零影响。
- **Zen3 硬件事实（本项目实测，与 APM 文档语义不符，务必记住）**：
  1. TlbControl=1 不冲刷 NPT（GPA→HPA）翻译——位翻转类修改侥幸生效（walk
     cache 未命中时），PA 变更类修改不可见；
  2. VMCB clean bits 缓存 nCr3 字段写——写 NCr3 必须 VmcbClean=0 才可能生效；
  3. 即便如此，nCr3 切换的翻译可见性仍延迟至自然 TLB 逐出；
  4. NPF（VMEXIT）不写 CR2——注入 #PF 必须手动设 SSA.Cr2（否则陈旧 CR2 →
     spurious-retry 自旋，c12 风暴同源）。
- WPM 旁路 = 已知限制（README 已记录）：MmCopyVirtualMemory 双形态
  （MiDoPoolCopy 调用方 CR3 / KeStackAttachProcess 目标 CR3+cpl0），假写可吞
  但有上述延迟问题；C6 重启需 KD 环境。

【关键文件】
- `docs/YUANMOD_HANDOFF_CURRENT.md`：全程判读史（**最近 9.244–9.290 是
  206/C4-C5/C6 线**，每轮含蓝屏判读）。
- `YuanGuardHV/tools/yghv_ctl.ps1`：控制面客户端（**34 命令**，含跨进程
  protect-page/mmf-*/scan-pid/wpm-write/**wpm-read**/config-fake/
  **config-sync**/**config sync**/TryRead 等）。
  - 9.303 新增 `config sync <0|1> [interval_ms]`：sync 参数已并入
    `config` 结构（16B），**一次调用即可配置 guard**，不必再走单独的
    `config-sync` 往返；`config` 读回含 `sync=` / `sync_ms=`。
  - `wpm-read <pid> <hex_va>`：**纯读**（无副作用），用于验证回滚 ——
    `wpm-write` 会扰动被测页，验证回滚必须用纯读探针。
  - `mmf-hold <path> <sec> [info]`：**不自写**目标（映射后只睡），
    用于需要无歧义判据的测试；`mmf-loop` 每 400ms 自写，会污染判据。
- `D:\aaaaaavm\run_c16_step206c4.ps1`：C4 回归脚本（按镜像名部署，哈希门）。
  （C6 假写验证脚本 = `D:\aaaaaavm\run_c18_step206c6.ps1`，勿混。）
- `C:\aaaaaavm\kd_c6f.bat`：kd 转储取证模板（`C:\aaaaaavm\kd_last.bat <dump>`
  通用）。**9.291 已改直连**：原 `HTTP_PROXY=127.0.0.1:7890` 实测无监听
  （连接被拒），两脚本已注释掉代理行；本机 msdl 直连可达，实测 14s/37s
  全栈还原通过。仅直连不通时再启用代理行。
- 归档镜像：`D:\aaaaaavm\yuanguard_hv_step206*.sys`（206c7e = 稳定，
  206c9 = 假写诊断版）。
- 蓝屏取证流程：minidump → kd !analyze -v（符号直连，已实测）→ 栈/模块
  定位 → 判读入库。**切勿直接改代码猜原因**。

**⚠️ 写内核代码前必读**：`docs/KERNEL_API_CHECKLIST.md`
—— 2026-10-11 三次蓝屏换来的 API 契约核对清单（锁不可递归 / 物理地址跨进程
不唯一 / MmGetVirtualForPhysical 只管系统页 / `/EHs-c-` 下 SEH 失效 /
非对齐柔性数组 / ZwCreateKey 不建中间路径 / attach 需 PASSIVE / 进程引用计数 /
/map 用法 / 测试纪律 / 崩溃取证流程）。**本轮的三个 bug 都在那里面写着。**

【本机雷区（全部实测）】
- **⚠️ ExAcquireFastMutex 不可递归**（9.299 蓝屏根因）：任何取 `g_protect_lock` 的
  新函数，若其调用链上已持锁 → 同线程二次获取 → 永久自旋 → guest 三重故障
  → 0xE2/0x20601 蓝屏。**新增加锁函数前必查调用链**。修法：拆 `_locked` 版
  （假定持锁）+ 公开版（加锁后委托）。
- **⚠️ 加载驱动的测试脚本必须有超时保护**：9.299 事故中脚本无超时，挂起 23 分钟
  才崩。用 `Invoke-Guarded`（任一步超时即强杀驱动）——见 `c_line_run_safe.ps1`。

- python -c 转义事故清空过 protect.h（已 git 恢复）——.h/.c 编辑用 Edit 工具。
- default 构建会覆盖 bin\yuanguard_hv.sys——**归档前 marker 验证**（206 构建含
  's206b coexist'）。现 bin 里那份（md5 BF8AE4A3）是 default 残留，**勿当
  206 镜像**。
- run_c* 脚本自带残留 VMM guard + 部署哈希门（防旧镜像静默运行）。
- **run_c18 判据已于 9.291 修正**（原 gate1 旧值前缀写错、gate2 正则永不
  匹配 → 206c9 末轮假 FAIL）；离线回归 12/12 PASS。
- 目标进程 90s 窗口会被长自旋耗尽（gate3 的 err 87 教训）。
- 跨工具链 .h 编辑前先备份（protect.h 清空事故）。
- ~~`C:\Windows\yghv_progress.log` 被内核句柄独占（DriverUnload 未关句柄）~~
  **✅ 已修复（9.292/9.293/9.294）**：`DriverUnload` 现调用 `yghv_trace_close()`。
  验证：A/B 同会话对照 —— 206c10(修复) 卸载后 `holder=none`；206c9(无修复)
  卸载后 `holder=pid=4 System`。**因果链已闭合。** 注意：每次跑无修复的旧镜像
  仍会留下僵尸句柄，需重启清理。

【下一步候选（用户定）】
1. **C6 性能问题（9.290 遗留，真正的未解问题）**：假写功能达成但自旋延迟 92s
   （1.05M 次重故障），需 KD 单步 NPF 路径的 TLB 行为。**需 KD 环境**（第二台
   机器或 KDNET；本机 debug=No、dbgsettings 仍为 Serial，KDNET 未配置）。
2. 真实产品目标流程化（Minecraft/Forge 目标选择+页选取——控制面已就绪）。
3. 推送 GitHub（本地领先 **28** 提交；**用户说推才推**）。
4. 若要 206c11 生效为部署基线，需重启（当前 progress.log 被 206c9 僵尸句柄占用）。

【2026-10-11 后续窗口补充（9.292–9.295）】
- **缺陷 C 已修复并验证 PASS**：`DriverUnload` 补 `yghv_trace_close()`
  （main.c:4943，+10 行）。A/B 同会话对照证明因果。
- **新稳定基线 = 206c11**（md5 `C73B4ABD`，归档
  `D:\aaaaaavm\yuanguard_hv_step206c11_20261011.sys`）= **206 主线 + 缺陷C修复**。
  206c7e 降为「C6 v4 PASS 线」历史参考。
- **C6 v5/INVLPGA 无法从构建中剥离**：只有 `#if STEP==206` 门控（无独立开关），
  但**运行时由 `g_fake_mode`（默认 0）休眠** → 代码在内、永不执行、零影响。
- **构建可复现性已证明**：新构建 vs 206c10 差异 **325 字节**，
  `.text/.data/.pdata/INIT` **逐字节完全一致**；差异全为 PE TimeDateStamp(5) +
  .rdata 嵌入时间戳(2) + Authenticode 签名区(318)。**无任何代码差异。**
- **本机签名事实**：`testsigning=Yes` + `nointegritychecks=Yes` + SecureBoot=False
  → **内核不校验驱动签名**，签名对本地加载零影响。`build.bat` 签名行
  （`/fd SHA256 /a /f yuanguard_test.cer`）实测 exit=0，**无需改动**。
  免测试模式分发唯一路径 = Microsoft attestation signing（需 EV 证书）。
- **⚠️ 源码仍未提交**：缺陷C修复（main.c +10 行）与 9.293–9.295 文档均为工作树
  改动，**未新建 commit**（按规矩：无用户明确指示不 commit/不 push）。

