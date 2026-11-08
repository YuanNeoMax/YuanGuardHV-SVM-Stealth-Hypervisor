# YuanGuardHV OS-as-Guest 完整实验总结（2026-08-15 定稿）

> 本文档是 OS-as-Guest（整机虚拟化）研究线的**完整、系统性总结**，覆盖 9.84-9.197
> 全部实验路径、问题、判别与结论。配套权威逐条记录见
> `docs/YUANMOD_HANDOFF_CURRENT.md` 9.84-9.197；本文件为横向归纳版。

## 1. 项目背景（一句话）

AMD-V SVM/NPT 隐形 Hypervisor 内核驱动（YuanGuardHV），目标是保护 Minecraft/Forge
进程；"OS-as-Guest" 是把**整台 Windows 放进 guest 模式**（实现整机隐形与不可卸载）的
远期里程碑，也是后续 P0-P2 里"整机隐形"的前提。

## 2. 一句话结论（先看这个）

**本机（Ryzen 5 5500）无法运行 OS-as-Guest：只要让 guest 在 guest 态执行真实 Windows
内核代码（调度器切换 / 中断 ISR），本机就在 <10s 内失败（整机硬冻结或 0x101 时钟看门狗）——
无论独立 CR3、APIC 虚拟化、NPT 全权限、ASID/TLB 卫生、HLT 拦截、host ISR 如何组合。
判定为平台级限制（AMD errata 1363 类，官方无修复），非驱动逻辑可修。**

本机唯一稳定形态 = **合成最小页表心跳 guest**（9.152，从不运行真实 Windows 内核代码）。

## 3. 完整实验路径总览

### 3.1 第一阶段：常驻线最初停线（9.84-9.102，v93-v102）

| # | 路径 | 问题/结果 |
|---|---|---|
| v93 | MSR_PROT + VM_CR 读返回 SVMDIS | 硬冻结 |
| v94 | VMRUN 前清 VMCB clean bits | 硬冻结 |
| v95 | step23 阻塞 + host ISR（无 AVIC） | 硬冻结 |
| v96 | step20 PASS 基线 + 持续写物理 APIC TPR | **PASS**（45-140s，TPR 写流单独不冻结） |
| v97 | +HLT/MWAIT 拦截 | 硬冻结（更早） |
| v98 | xAPIC MMIO 影子 + 转发 TPR/ICR/timer | 硬冻结 |
| v99 | 全核 12 核对称常驻 | 硬冻结（排除"单核不对称"） |
| step100 | 阻塞 + 每 vcpu 环形缓冲诊断 | 0x139（guest 内文件 I/O 触发） |
| v100b/c | 去文件 I/O 隔离 | 仍 0x139 / 硬冻结 |
| v101 | #DF/#NP/#SS/#GP 拦截 | 无捕获，硬冻结 |
| v102 | 全 32 异常 + HLT 拦截 | 无捕获，硬冻结 → **平台级停机判定** |

### 3.2 第二阶段：共享 CR3 根因定位（9.149-9.156）

| # | 路径 | 问题/结果 |
|---|---|---|
| 9.149-9.151 | 隔离矩阵（核数 1-12 / 退出率 / C 状态 / 无 churn） | 全冻结 → 排除核数/退出率/C 状态 |
| 9.152 | **独立 CR3（最小页表）合成 resident** | **稳定 5 分钟**（首个解冻变量） |
| 9.155 | hook 调用三重重置 = host_stack 只映射 1/4 页 | 修复后完整 workload 12 核 ACTIVE |

**关键发现**：冻结 = guest 共享宿主内核 CR3 → guest TLB 活动与宿主 TLB 冲突 → CPU 全核
锁死；**修复 = 独立 guest 地址空间**。但该独立 CR3（`yghv_build_guest_cr3`）是合成心跳
guest 专用，**对真 OS-as-guest 不可直接套用**（guest 需完整 Windows 地址空间）。

### 3.3 第三阶段：OS-as-guest 重开（9.157-9.175）

| # | 路径 | 问题/结果 |
|---|---|---|
| 9.157 | 有界试点重开（step12/14/16） | ✅ 全 PASS（机制完好，非常驻） |
| 9.158 | step20 spin 常驻（历史唯一 PASS 形态） | 运行 PASS + 卸载 0xCE |
| 9.163 | 0xCE 卸载修复（stop 标志 + join） | ✅ step20 spin 常驻可干净卸载 |
| 9.159-9.161 | ASID/TLB 卫生（每核唯一 ASID + FLUSH_BY_ASID） | 无效（阻塞常驻仍冻结） |
| 9.170 | GS selector 镜像宿主 | 0x139→硬冻结（移除早期症状但未解决） |
| 9.171 | TSS 隔离 | 无效 |
| 9.172 | FLUSH_ALL | 无效（非纯 TLB 别名） |
| 9.174 | GS base MSR 虚拟化 | 无效 |
| 9.175 | CR3 写拦截 | 无效 |
| 9.177-9.179 | 独立 CR3 深/浅拷贝克隆 | 深拷贝 0x7E、浅拷贝冻结；**OS-as-guest 架构上必须共享宿主页表**（进程切换拉回真实页表） |
| 9.173 | AMD errata 检索 | 1363（SVM guest 中断死锁，无修复）/1235（AVIC） |

**问题**：全部驱动级假设穷尽（ASID/TLB、GS、TSS、MSR、CR3、异常、HLT、APIC 系列），
OS-as-guest 常驻在本机冻结。

### 3.4 第四阶段：B 路线（APIC 虚拟化，9.184-9.197，本次会话）

| 实验 | 改动 | 结果 | 判别 |
|---|---|---|---|
| B-0（step202） | spin+注入，**无** APIC 影子 | **硬冻结** | guest ISR 直接碰物理 APIC = errata 1363 |
| B-1min | +RAM 影子 | **0x101** | 影子消除硬冻结，但时钟不被服务 |
| B-1min-fixed | +看门狗时序修复 | 0x101 | 深拷贝阶段 0xE2 误报已修 |
| B-1full | +写陷阱 + EOI/ICR/timer 转发 | **仍 0x101** | APIC 完整虚拟化不是瓶颈 |
| D1/D1b/D1c | +NPF 诊断（1s/100ms/10ms） | 0x101；D1c **深拷贝竞态 0x50** | 深拷贝慢(600ms+)+竞态 |
| D2 | 浅拷贝快 setup | 0x101（<10ms） | 轮询抓不到进入后数据 |
| D3 | host ISR + 受控 100ms 单注入 | 0x101 | 被浅拷贝基座混淆 |
| D3-base | 控制组（无注入器） | **0x101** | **注入不是变量，基座本身不稳** |
| D4-base | 深拷贝+host ISR+APIC 影子（无注入） | **0x101** | 深拷贝竞态已修，但基座仍 0x101 → 受控注入测试被阻断（无稳定基座） |

### 3.5 外部 AI 假说判别（本次会话）

| 假说 | 判别 | 依据 |
|---|---|---|
| NPT 权限不全卡 ISR 路径 | **证伪** | B/D 全用独立 CR3 + 全 RW identity NPT（无权限限制），仍 0x101；0x101 非 NPF 型崩溃 |
| Host 饥饿（全核给 guest） | **证伪** | MSV-1/2 单核 guest + 11 空闲宿主核，核心 1 仍 0x101 |
| "独立 CR3 稳定 = ISR 硬件可行" | **误读** | 9.152 合成 resident 是纯心跳且**中断全在宿主态服务，从不执行 guest 态 ISR** |
| 深拷贝慢/竞态 | **证实并已修** | D1c 0x50；D4 克隆期暂停 diag 修复（无 0x50） |

## 4. 决定性证据链

1. **唯一 PASS 常驻形态**：step20（spin + INTR/NMI 拦截 + host ISR，中断宿主态服务、
   guest 永不处理中断），9.163 起可干净卸载；
2. **合成最小页表心跳 guest 稳定 5 分钟**（9.152）——但从不运行真实 Windows 内核代码；
3. **任何让 guest 处理注入中断的配置**（B/D 全系）在 <10ms 内 0x101；
4. **深拷贝全量页表 + host ISR + APIC 影子（无注入）**（D4-base）也 0x101 → 受控注入测试
   无稳定基座可做；
5. **0x101 转储**：Arg4=1（核心 1 = guest 核），时钟中断不被服务（`IDLE_THREAD_INVALID_
   CONTEXT` / `INVALID_CONTEXT`），核心 0 存活（可诊断，非硬冻结）；
6. **AMD errata 1363**（SVM guest 态中断处理死锁，`No fix planned`）、1235（AVIC 禁用）
   ——硬件级解释。

## 5. 最终结论

1. **OS-as-Guest（真实 Windows 在 guest 态执行调度器/ISR）在本机不可行**，属平台级限制
   （errata 1363 类），非驱动逻辑 bug、无软件修复方案；
2. **本机可达成边界**：
   - 有界试点（step12/14/16 全 PASS）——机制验证；
   - **step20 自旋常驻**（可加载 / 稳定 / 9.163 起可干净卸载）——本机可运行的常驻形态；
3. **换平台重启条件**（OS-as-guest 若继续）：
   - 其它 AMD-V / Intel VT-x 机器（无 errata 1363 类问题）；
   - 顺序：有界试点 → ASID/TLB 卫生（负结果记录）→ 修 0xCE 卸载（已修）→ 独立 CR3 +
     APIC 虚拟化 → 阻塞常驻；
4. **非驻留保护路线不受影响**（内存页写保护 / 终止 / 句柄保护 / 真实目标接入 / Java 客户端
   均稳定 PASS）。

## 6. 本会话产物与归档

### 6.1 驱动构建归档（D:\aaaaaavm\，2026-08-15）

| 构建 | SHA256（前 8） | 说明 |
|---|---|---|
| MSV | 27A9C34A | 深拷贝独立 CR3 + CR3 写 fail-close + HLT 拦截（step200） |
| MSV-2 | 2DC9C9C0 | +HLT 拦截（0x101，HLT 无效） |
| B-0 | DC565069 | spin+注入，无 APIC 影子（硬冻结） |
| B-1min | 9EAF9B7C | +RAM 影子（0x101；首次测试因看门狗时序无效） |
| B-1min2 | EA8FEEC4 | 看门狗时序修复（0x101） |
| B-1full | 0464D52D | +写陷阱+EOI 转发（仍 0x101） |
| D1 | DE6881E9 | 1s NPF 诊断 |
| D1b | AF8DC542 | 100ms 诊断 |
| D1c | 303FDEC9 | 10ms 诊断（深拷贝竞态 0x50） |
| D2 | 38AF3937 | 浅拷贝快 setup |
| D3 | B7F5A068 | host ISR + 受控注入 |
| D3-base | 554D6A7A | 控制组（无注入器，基座 0x101） |
| D4-base | BB955FA9 | 深拷贝+host ISR+APIC 影子（基座仍 0x101） |

### 6.2 转储归档（D:\aaaaaavm\yghv_bsod_*.dmp）

- `yghv_bsod_msv_20260815_0943.dmp`（0x101）
- `yghv_bsod_msv2_20260815_0957.dmp`（0x101）
- `yghv_bsod_b1min_20260815_1031.dmp`（0xE2 看门狗，测试无效）
- `yghv_bsod_b1min2_20260815_1127.dmp`（0x101）
- `yghv_bsod_b1full_20260815_1140.dmp`（0x101）
- `yghv_bsod_d1_20260815_1231.dmp`（0x101）
- `yghv_bsod_d1c_20260815_1256.dmp`（0x50 深拷贝竞态）
- `yghv_bsod_d2_20260815_1306.dmp`（0x101）
- `yghv_bsod_d3_20260815_1316.dmp`（0x101）
- `yghv_bsod_d3base_20260815_1503.dmp`（0x101）
- `yghv_bsod_d4base_20260815_1516.dmp`（0x101）

### 6.3 代码状态

- 工作区：`main.c`、`vmexit.c`、`docs/YUANMOD_HANDOFF_CURRENT.md` 有未提交改动
  （B/D 系列：step200/202 分发、深/浅拷贝、APIC 影子/写陷阱、NPF 诊断、受控注入、
  clone 竞态修复、文档 9.180-9.197）；
- HEAD `edf42ce`（MSV 提交，含 9.180-9.183）；
- C 盘：稳定默认版 `70888311...`，服务 STOPPED，机器安全；
- 当前默认构建（HEAD 源码）：SHA256 详见交接文档（非 `70888311`，因 HEAD 已含多目标等
  更新，客户端脚本需配当前默认版）。

## 7. 交接建议

- OS-as-guest 本机线**正式收尾**（证据完备）；换平台前不再在本机进行 VMRUN 常驻类实验
  （本机 hypervisor 负载有平台级冻结/0x101 史）；
- 后续开发走**非驻留保护路线**（产品主线）；OS-as-guest 作为换平台项记录；
- 提交本轮 B/D 代码与文档时，保留 step200/202 门控实验（默认构建不激活），并在
  `docs/YUANMOD_HANDOFF_CURRENT.md` 9.197 之后补本轮收尾记录。

## 8. 补充章节（2026-08-15 下半场）：VMware 嵌套 VM 路线完整实验与结论（9.198-9.218）

> 本节补充 2026-08-15 下半场在 **VMware Workstation 17.6.4 嵌套 VM**（Windows 10 x64，
> 官方镜像 19045.2965，`vhv.enable=TRUE`，KD 命名管道）上重新开启 OS-as-guest 的完整
> 实验。**结论方向：VM 能跑有界/自旋形态（裸机做不到），但阻塞常驻同样到顶——VMware
> 嵌套的 L1 双层虚拟化存在平台级限制。**

### 8.1 前置（关键纠错与基建）

- **历史纠错**：原提示词把 "VMware 启动任何 VM 硬卡死" 标为 9.85-9.88，编号有误——
  9.85-9.88 实为云服务器 KDNET 调试（Realtek RTL8168 硬件级不支持）。真实 VMware 冻结
  是 8.1-8.5 节（2026-08-09/10，精简镜像+调试管道组合）；9.4 官方镜像重装后 VM 稳定。
- **KD 串口关键教训**：serial0 是 pipe server 端，**必须先让 kd 客户端连上
  `\\.\pipe\yuanhv_debug` 再启动/重启 VM**，串口后端才就绪、guest 才枚举 COM1；否则
  kd 一直 Waiting to reconnect。
- **kd 断点教训**：`run_kd.bat` 原 `-c ".reload;bu yuanguard!DriverEntry;g"` 在 kd 真连
  接时，驱动加载到 DriverEntry 即命中断点→guest 冻结等待 kd 输入（SCM 显示 RUNNING 但
  guest 卡住）。已改为 `.reload;g`（无断点）。

### 8.2 有界/自旋形态：VM 全 PASS（远超裸机）

| step | 形态 | VM 结果 |
|---|---|---|
| step11 | 有界 2 核心跳（INTR/NMI/SHUTDOWN 拦截） | ✅ PASS（NPT/FLUSHBYASID 均暴露） |
| step12 | 单核有界 OS guest（5000 CPUID/RDTSC） | ✅ **PASS（裸机 0x7E/硬冻结→VM 成功）** |
| step14 | 全核有界 OS guest | ✅ PASS |
| step16 | 全核 seamless | ✅ PASS |
| step20 | **自旋常驻** + INTR/host ISR | ✅ **PASS（可干净卸载，alive 0x28）** |

**核心突破**：VMware 软件 L0 绕开了裸机 errata 1363 类限制，guest 态执行真实 Windows
代码（有界/自旋）在 VM 里全部可行——裸机做不到。

### 8.3 阻塞常驻：逐层归因（严格对照实验）

| 变体 | CR3 | APIC | 结果 | 归因 |
|---|---|---|---|---|
| step17 原始 | 浅拷贝+拦截+克隆 | 未映射 | 硬冻结 | **APIC 页未映射→NPF→硬冻结** |
| 变体 A | 深拷贝 | 无拦截 | 冻结 | **深拷贝 CR3=冻结源** |
| 变体 B | 浅拷贝 | 重定向 shadow+拦截 | 冻结 | **APIC 拦截=冻结源** |
| 变体 C' | 浅拷贝 | identity+清 W 拦截写 | 冻结 | **拦截 APIC 写本身即冻结** |
| 变体 D | 浅拷贝+APIC 直通 | identity 直通 | **0x139 蓝屏（活 10min）** | **CR3 拦截+克隆静态快照→栈崩溃** |
| 变体 E/F | APIC 直通+宿主 CR3 直通(+host ISR) | 直通 | **guest 态真实 Windows 持续运行（有画面）** | **突破：最小干预** |
| 变体 G | 全核（core0+1 都进 guest） | 直通 | 仍死锁（无 alive） | **跨核协作在 L1 双层虚拟化下有问题** |
| 变体 H | 单核（VMX numvcpus=1） | 直通 | VM 立即软重启（triple fault 类） | **单核无第二核分担，仍崩** |

**三项关键代码修复（均生效）**：
1. **v98fix**：APIC 影子 NPT 映射前补 `npt_identity_map_range(&g_npt, 0xFEE00000, +0x1000)`
   （APIC 页是 MMIO 非 RAM，`yghv_npt_map_ram` 未映射 → `npt_map_page failed`）；
2. **解码器修复**：`yghv_apic_mmio_npf` 放宽寻址检查（支持 `[rax+disp32]` 基址寻址，
   GPA 直接从 exitinfo2 取，C7 imm32 用 next_rip 定位）——Decode-Assist 填充正常
   （nf=15），原代码只支持 `[disp32]` 导致 Windows APIC 写解码失败；
3. **run_kd.bat**：去掉 DriverEntry 断点。

**0x139 精确定位**（dump `yghv_bsod_variantD.dmp`）：`FAST_FAIL_INCORRECT_STACK`，
崩溃线程 vmtoolsd.exe（普通进程）在 `NtWriteFile → KiSystemServiceCopyEnd` 返回路径栈
指针出界（RSP 0x619470c0 远低于线程栈 Limit 0x62109000）——根因是 CR3 拦截+克隆静态
快照（`yghv_os_guest_clone_cr3_apply` 注释明说"静态快照，PTE 更新分歧"）导致跨进程栈
翻译错误。

### 8.4 VM 路线最终结论

- **VM 能跑有界/自旋（远超裸机）**；**阻塞常驻所需的所有关键技术（APIC 影子/深拷贝/
  CR3 拦截+克隆）在 VMware 嵌套下均不可用**；
- 最小干预（APIC 直通 + 宿主 CR3 直通）让 guest 态真实 Windows 持续运行（有画面），
  但多核跨核 IPI 死锁、全核同样死锁、单核 VM 重启——**阻塞常驻的可用性（交互/稳定）
  在所有形态下未达成**；
- **根本限制**：VMware 嵌套的 L1 双层虚拟化（两个独立 VMRUN 循环）对真实 Windows 调度
  的跨核协作/异常处理存在平台级限制，非驱动逻辑可修；
- 换平台候选：**KVM/Proxmox（AMD 原生嵌套）或真实硬件**。

### 8.5 归档（2026-08-15 下半场 VM 实验）

- 驱动：`D:\aaaaaavm\yuanguard_hv_step1X_20260815.sys`、`_step17_apicpassthrough_
  20260815.sys`、`_step17_minimal_20260815.sys`、`_step17_hostisr_20260815.sys`、
  `_step99_allcore_20260815.sys`、`_step17_singlecore_20260815.sys`；
- 蓝屏 dump：`D:\aaaaaavm\yghv_bsod_variantD.dmp`（0x139 FAST_FAIL_INCORRECT_STACK）；
- 代码备份：`D:\aaaaaavm\main.c.bak-20260815-v98fix/-step202-ctlA/-step17-ctlD/
  -step202-ctlC/-step17-ctlE/-step99-ctlG`、`build.bat.bak-20260815-ctlA`、
  `Windows 10 x64.vmx.bak-20260815-singlecore`；
- VM 当前配置：`numvcpus=1`（单核方案遗留，如需多核改回 4）。

### 8.6 交接建议（VM 路线）

- VM 路线（OS-as-guest 阻塞常驻）同样**收尾**——证据完备，VMware 嵌套平台限制实锤；
- 有界/自旋形态（step11-20）可在 VM 继续用于回归/机制研究；
- 换平台（KVM/真实硬件）时，复用本轮三项修复（v98fix/解码器/run_kd）+ 最小干预配置
  （APIC 直通 + 宿主 CR3 直通）；
