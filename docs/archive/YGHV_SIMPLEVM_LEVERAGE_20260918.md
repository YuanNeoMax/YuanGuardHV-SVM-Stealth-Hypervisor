# YuanGuardHV：用 SimpleSvm/HelloAmdHv 构型复审 OS-as-Guest 冻结（2026-09-18）

> 背景：`YGHV_OS_AS_GUEST_SUMMARY_20260815.md` 判定 OS-as-guest 阻塞常驻在本机
> （Ryzen 5 5500 裸机）与 VMware 嵌套均"平台级不可行"。本文档以用户指定的四个
> 参考项目（SimpleSvm、DdiMon、HelloAmdHvPkg、SimpleVisor）+ AMD APM 为依据做
> 复审，结论：该判定的两个支点都不牢（拦截构型偏离参考实现；errata 1363 对本机
> 适用性未证实）。据此落两个实验：**C0** = 上游 SimpleSvm 原版实机对照（本机构建
> 已完成），**C1** = 本仓库新增 `step203`「SimpleSvm 等价」门控构型（代码已入库，
> 构建验证已完成，待实机）。
>
> 参考：`docs/YUANMOD_HANDOFF_CURRENT.md` 9.227；构建归档
> `D:\aaaaaavm\yuanguard_hv_step203_20260918.sys`（SHA256 `83b0c85b...`）、
> `thirdparty\SimpleSvm\bin\SimpleSvm.sys`（SHA256 `2ce8c17c...`）。

---

## 1. 参考项目构型提取（2026-09-18 逐文件核实）

### 1.1 SimpleSvm（tandasat，SVM 驱动态 OS-as-guest）

来源 `SimpleSvm/SimpleSvm.cpp` + `SimpleSvm/x64.asm`（master @ 2026-09-18）：

| 项 | 值 | 含义 |
|---|---|---|
| 拦截集 | `CPUID` + `VMRUN` + `MSR_PROT`，且 MSRPM 只置位 **EFER** | 全仓库唯一退出源，全部为**同步**事件 |
| INTR/NMI/HLT/异常/CR/MWAIT | **一律不拦截**，无 IOPM | 物理中断走 virtual wire 由 guest（真实 Windows）自己的 IDT/ISR 服务，VMM 不介入 |
| GuestAsid / tlb_ctl | `1` / `0`（从不刷） | 与 YGHV 的"OS-as-guest 必须共享 CR3 → 冻结"场景完全同类 |
| NPT | 2MB 页 identity，0–512GB 全 RW，无 1GB 页（"VMware 不支持"）| 无权限注入、不藏 HV 内存 |
| 进入 | **所有活动处理器**逐个（affinity 切换）在 PASSIVE/DISPATCH 级 vmload/vmrun 无缝进 guest；此后 OS 永为 guest | 常驻方式 = "翻转即忘"，宿主侧只在 VMEXIT 窗口存在 |
| 卸载 | ring-0 `CPUID 0x41414141` 后门：该核 host 循环返回 → STGI + 清 EFER.SVME → 跳回 guest 流；`DriverUnload` 用同一机制逐核反虚拟化后干净卸载 | 可反复加载/卸载 |
| VMEXIT 岛 | 纯 C（`SvHandleVmExit`），**禁止任何 NT API**（不 DbgPrint）；处理内容仅 trap frame + CPUID/MSR 仿真 | 见 §2 的 issue 证据 |

### 1.2 HelloAmdHvPkg（tandasat，UEFI 型 type-1：OS 从启动起即 guest）

来源 `Drivers/HelloAmdHvDxe/Setup.c`：拦截 = **#SX 一项**（INIT-SIPI-SIPI 重定向）+
CPUID + VMRUN；不拦 INTR、不配 MSRPM；ASID=1、tlb_ctl 不设；NPT 双份（BSP 把 APIC
页拆 4KB 以做 #SX 陷阱，AP 全 identity）；全核经 `StartupAllAPs(Serial=TRUE)` 进
guest；无 EVENTINJ/VINTR。**价值**：提供"VMM 在 OS 之下"的隐形终态形态（Windows
出生即 guest），绕开"驱动常驻 VMEXIT 环"整类问题；README：baremetal + VMware 均测过。

### 1.3 DdiMon / SimpleVisor（Intel VT-x）

同族形态（驱动翻转所有核 + EPT 影子 + 原生中断），印证"最小干预 + VMEXIT 岛
不调 NT API"是这一类系统的通行纪律，非 AMD 特有。对 YGHV 无直接可搬代码。

---

## 2. 决定性机理证据：SimpleSvm issue #1

上游仓库唯一一条挂起类 issue #1（"system hangs shortly after virtualizing
processors"，卡 `ntoskrnl!KiIpiSendRequest` 后的 `PAUSE`，2021-01）：

- 作者诊断（tandasat 原话）：IPIs 不会被送到"正处于 #VMEXIT 处理中"的核 →
  若在 VMEXIT 处理路径里调用 NT API（其内部会发 IPI/走调度器），**会死锁**；
  "remove DbgPrint that is called within (and under) HandleVmExit"。
- 最终根因：用户在 VMEXIT handler 里下了调试断点。上游原版无此问题报告。
- 硬件语义：VMEXIT 后 GIF=0，该核不响应 INTR/NMI/IPI；SimpleSvm 蹦床注释同理
  （"VMRUN ... on exit disables interrupts by clearing the global interrupt
  flag"）。因此"在 guest 里跑着完整 OS、宿主窗口里跑着**同一个 OS 的中断/调度
  代码**"是这类设计明令回避的反模式。

**对照 YGHV**：此前全部裸机常驻系都命中该反模式——
`svm_core.c` 裸机路径强制 `INTR|NMI|SHUTDOWN` 拦截；step17/99/变体 E/F/M 保留
宿主 ISR（在 GIF=0 窗口里跑完整 Windows ISR 派发，含 DPC/定时器/IPI）；拦截集
另有 RDTSC、MSR_PROT(VM_CR + KERNEL_GS_BASE——Windows 每次线程切换都 WRMSR
KERNEL_GS_BASE → 每次切换一次退出)、`ZwYieldExecution()`/KeSetEvent/文件 trace
出现在常驻循环里。**"guest 态中断处理是墙"的结论只在"拦 INTR + 宿主代服务"构型
内成立**；参考实现证明另一条路（不拦中断）本身可行且被广泛实机使用，而这条路
从未在本仓库裸机测试过（v99 全核形态仍带 INTR 拦截 + 宿主 ISR）。

## 3. Errata 1363 适用性：判定需复核

1. 本机 CPUID 实测 `AMD64 Family 25 Model 80` = **Family 19h Model 50h = Zen3
   Cezanne**。1363/1235 的编号属于 **Family 17h（Zen1/2）Revision Guide 56215**
   系列；9.173 当时由 web 检索归因，本会话因网络受限未能取到原文核对覆盖面。
   需人工查 Family 19h Model 50h 对应 Revision Guide（Zen3/Cezanne）中是否有
   SVM 死锁类条目及其 workaround（多为 BIOS/微码 chicken bit）。
2. KVM `svm.c`（2026-09-18 master）无 1363 处理；同一硅片家族（19h）上 Hyper-V /
   KVM 以 SVM 模式大规模跑 OS-as-guest 整机（微软/红帽产品线）→ "guest 态中断
   即死锁"若为硅片级，该生态不成立。
3. 本仓库自证：9.189 **BIOS/AGESA 更新**把 B-1min 从硬冻结（不可转储）变为可转储
   的 0x101——纯硅片 bug 不应随固件变化形态；行为依赖固件/配置，更像软件栈问题。

→ 结论：把"平台级不可修"降级为"未证实的归因"，由 C0（原版 SimpleSvm 实机对照）
直接裁决。

## 4. C0：上游 SimpleSvm 实机对照（构建已完成，待上机）

- 源获取（直连 git 被网络限制）：
  `curl -L -o /tmp/ss.tar.gz https://codeload.github.com/tandasat/SimpleSvm/tar.gz/refs/heads/master`
  → 解包至 `thirdparty/SimpleSvm`（已在 `.gitignore`，不入库）。
- 本地补丁（仅可加载性，**不触碰任何虚拟化语义**）：
  1. `ExAllocatePool2` → `ExAllocatePoolWithTag` + 清零（2 处；Win10 19045 无
     该导出。上游面向 Win11）。
  2. 构建脚本 `YuanGuardHV/tools/build_simplesvm.bat`（绕开 WDK MSBuild 集成，
     风格对齐本仓库 build.bat；WDK 10.0.26100.0 头/库，MSVC 14.44）。
- 构建验证：链接成功；导入表 dumpbin 全为 Win10 19045 存在的内核导出；signtool
  测试证书签名；SHA256 `2ce8c17c71f658689ca7d5d6b544b612dd0b7ffffbf043af7f728cce6d863a09`
  （路径 `thirdparty\SimpleSvm\bin\SimpleSvm.sys`）。
- 上机 runbook（需重启预案就位；先确认 Hyper-V/VBS 未占用 SVM——本机历史实验
  可用即证明当前占用为无）：
  ```bat
  copy thirdparty\SimpleSvm\bin\SimpleSvm.sys C:\SimpleSvm.sys
  sc create SimpleSvm type= kernel binPath= C:\SimpleSvm.sys start= demand
  sc start SimpleSvm
  :: 观测 ≥10 min：GUI/网络响应、任意调度压测、（如有 WinDbg）Kd 输出 DbgPrint
  sc stop SimpleSvm     :: 上游 DriverUnload 逐核反虚拟化，应干净卸载
  sc delete SimpleSvm
  ```
- 裁决逻辑：原版稳定 ≥10min → **"平台级限制"判定作废**，后续移植按 C1/§5；
  原版也冻结 → 归因升级为"SimpleSvm 构型在本硅片同样死"，回到固件/errata 线
  （比对上游成功机型清单 + 19h Revision Guide 原文 + 微码版本）。
- **实测结果（2026-09-18，已执行）：PASS。** `sc start SimpleSvm` → RUNNING
  （12 核无缝进 guest）；`yghv_stress 600s x6`（CPUID 风暴+调度 churn）全程心跳
  无中断，期间系统级命令响应正常。历史判定"guest 执行真实 Windows 代码 <15-60s
  必冻"在本构型下不成立 → 冻结为**构型问题**（INTR 拦截 + GIF=0 宿主服务窗口 +
  高频退出/NT API 混入），非硅片问题。
- **卸载 quirk（实测记录）**：SimpleSvm 的 `sc stop` 返回 1052
  （NOT_STOPPABLE），`NtUnloadDriver` 返回 0xC0000010（尽管反汇编确认
  DriverObject->DriverUnload(+0x68) 有效赋值）→ **只能重启清除**。服务注册已
  `sc delete`，重启后不复加载；镜像暂存 `D:\aaaaaavm\SimpleSvm.sys.keep`。
  另：git-bash 下 `sc create/config` 的 `binPath=` 会被路径转换破坏，需
  `MSYS_NO_PATHCONV=1` + 双反斜杠。
- 完整逐条记录见 handoff **9.228**；C1 的实机脚本：
  `D:\aaaaaavm\run_c1_step203.ps1`（重启后一键：清理→部署 203→观测+压测→
  sc stop 验干净卸载→结果写 `D:\aaaaaavm\c1_results.log`）。

## 5. C1：本仓库 step203「SimpleSvm 等价」门控（代码已入库）

设计 = 在 v99 全核 barrier 骨架上做**纯减法**（`main.c`：
`yghv_os_guest_simplevm_thread` + `step == 203` 编排 + DriverUnload join）：

| 维度 | step203 | 说明 |
|---|---|---|
| 拦截集 | `CPUID`；`VMRUN|VMMCALL`(general2) | 其余全 0（含 RDTSC/SHUTDOWN/MSR_PROT/异常/CR/DR）|
| 中断 | 不拦，IF=1 进 guest（`svm_trampoline_os_enter(v,1)`）| guest = 真 Windows 自服务 |
| MSRPM | 位仍在但 MSR_PROT 拦截关 → 全直通 | VM_CR/GS_BASE 不再退出 |
| 干预项 | 不做 TSS 隔离/TLB hygiene/CR3 拦截/克隆/APIC 影子 | 对齐 SimpleSvm |
| 常驻环 | guest 延续体 = ~10ms TSC 节流 CPUID(1) 轮询；宿主窗口零 NT API（CPUID 分支仅 `__cpuidex`+内存写回）| 停止契约 9.162：`g_os_guest_stop_requested` → 下一次轮询退出返回 1 → host_done |
| 卸载 | `sc stop` 触发 DriverUnload：置停止标志 → 逐核 ≤10ms 轮询退出 → join 全部线程 → `svm_core_cleanup` 的 IPI 恢复 per-core EFER.SVME/VM_HSAVE | 顺带补 step203 线程的 join 表（v99 未入 join 表）|
| 观测 | `resident alive` 每 5s 写 `C:\Windows\yghv_progress.log`（guest 态运行的直接证据）；watchdog 停摆 3s 主动 KeBugCheck 0xE2 留 dump；v100 ring 记最后 64 次退出点 | |
| 构建门 | `set YGHV_BAREMETAL_STEP=203 && build.bat`（默认构建已验证不含 203 代码；顺带修正 `step>202` 上界 → `203`，这是 /O2 下 DCE 吞掉整个 step 函数的根因）| 产物 `83b0c85b...`，已归档 `D:\aaaaaavm\yuanguard_hv_step203_20260918.sys` |

Runbook（本机、裸机、重启预案就位）：
```bat
copy YuanGuardHV\bin\yuanguard_hv.sys C:\yuanguard_hv_s203.sys
sc create yuang203 type= kernel binPath= C:\yuanguard_hv_s203.sys start= demand
sc start yuang203
:: 读 C:\Windows\yghv_progress.log：期望序列
::   bm os simplevm resident start → s203 enter × Ncore → bm os simplevm
::   resident running → bm done → 此后每 5s "resident alive" 持续 ≥10min
sc stop yuang203        :: 期望干净停止（逐核轮询退出 + join）
sc delete yuang203
del C:\yuanguard_hv_s203.sys
```
决策树：
1. **203 稳定 + alive 持续输出** → 平台判定作废成立；下一步在 203 上叠加保护语义
   （NPF 写保护页、VMMCALL 认证、NPT 自剔除 R1 系列），产品化 OS-as-guest 重启。
2. **203 冻结/0x101**：对照 C0——
   - C0 也挂 → 真平台问题；查 19h Revision Guide 原文 + 微码/chicken bit；
   - C0 稳 → 残余差异定位：203 仍保留 clean-bits 强制 0、per-resume VM_HSAVE
     重写、v100 ring 内存写、蹦床 GPR 环、以及 `g_os_resident_exits` 等宿主侧
     变量可见性——逐项对齐 SimpleSvm 再试（每次单变量，沿用既有门控惯例）。
3. **可转储蓝屏** → v100 ring + dump 直接定位 exitcode/RIP（优于历史上"硬冻结无
   转储"形态，因为 203 无 INTR 拦截时看门狗/NMI 通道保持活）。

残余风险（记录在案）：不拦 SHUTDOWN → guest triple fault 会重启机器（SimpleSvm
同构，属实验固有）；NPT 只映到 `npt_init` 的 max-physical，若设备 MMIO 在其上
→ NPF → 现 NPF 处理 fail-close（可转储诊断，不再硬冻结）。

## 6. 文件索引

- `YuanGuardHV/hv/main.c`：新增 `g_s203_*` 状态、`yghv_os_guest_simplevm_thread`、
  `step == 203` 编排、DriverUnload 逐核 join、`step > 202` 上界修正。
- `YuanGuardHV/tools/build_simplesvm.bat`：C0 构建脚本。
- `thirdparty/SimpleSvm/`：上游克隆 + 2 处可加载性补丁（gitignored）。
- `.gitignore`：`thirdparty/`。
- 证据引用：SimpleSvm master `SimpleSvm.cpp`/`x64.asm`/issue #1；HelloAmdHvPkg
  `Drivers/HelloAmdHvDxe/Setup.c`；本仓库 handoff 9.84-9.226。

## 7. step204 之后：faithful 移植规格（2026-09-18 逐行比对 ground truth 得出）

实机弧线 C1/C1s/diag/diag2/203-q/204 全部 FAIL，但已把根因从"平台"彻底移到
"我们 `svm_trampoline_os_enter` 的进出模型"。本轮把 SimpleSvm 的
`SvVirtualizeProcessor`/`SvPrepareForVirtualization`/`SvLaunchVm`(asm)/`SvHandleVmExit`
逐行读完，并回读 YGHV trampoline 做 diff。**两个曾怀疑的具体点被证伪**（都已在 YGHV 里）：
- host 段/GS：`os_enter_exit` 有 `vmsave guest; vmload host_vmcb`（等价 SimpleSvm
  `SvHandleVmExit` 首行 `__svm_vmload(HostVmcbPa)`）；
- CPUID 的 RAX：`svm_finish_exit→svm_writeback_gprs` 有 `vmcb->state.rax = regs.rax`
  （等价 SimpleSvm 869 行）。
所以 203/204 的 `CORRUPT_LIST_ENTRY`/`0xB8` **不是**这两处，离线静态审查已到收益递减点。

**结构性差异（SimpleSvm 有、203 无，且尚未等价）——即 faithful 移植的必修项：**
1. **guest 入口来源**：SimpleSvm 的 guest 就是"DriverEntry 线程 vmrun 之后的自身继续"
   （`SvLaunchVm` 的 vmrun 下一条即 guest 起点），**没有**独立的 yghv 常驻线程在 guest 里
   跑；YGHV 203 用一个 PsCreateSystemThread 线程进入并在其中轮询/ret，204 让其
   PsTerminate——但 host 环的**栈/上下文来源仍是那个 yghv 线程**，与 SimpleSvm 的
   "DriverEntry 线程即 host 环、guest 即其 vmrun 后延续"不同。
2. **host 栈布局**：SimpleSvm 把 host 栈**内嵌在 VpData 尾部**（`HostStackLayout`，
   rsp 设到 `&HostStackLayout.GuestVmcbPa`，与 GuestVmcb/HostVmcb/HostStateArea 同页对齐
   连续体）；YGHV 用**独立** `host_stack`(4页)+`host_vmcb`+`hsave` 分离分配，
   `os_enter_resume` 每轮重 `wrmsr VM_HSAVE_PA` 并强制 `vmcb_clean_bits=0`。
3. **退出处理器最小化**：SimpleSvm 的 `SvHandleVmExit` 只 `vmload host` + 处理
   CPUID/MSR/VMRUN，**绝不** `KeRaiseIrqlToDpcLevel` 以下做任何可阻塞/文件/内存分配；
   连日志都被刻意禁（"NT API in VMEXIT → deadlock/corruption"）。YGHV 的
   `svm_dispatch_exit` 在 diag 版里曾写 trace（已挪到观察者），但观察者线程本身仍是
   guest 态普通线程做 NtWriteFile——204 的 `CORRUPT_LIST_ENTRY` 正落在这条写日志路径上，
   说明此时内核链表已被破坏（破坏源在别处，写日志只是先崩）。

**移植做法（推荐，避免重造 trampoline）**：不要在 203 骨架上继续改，而是
**把 SimpleSvm 的 `SvLaunchVm`+`SvPrepareForVirtualization`+`SvHandleVmExit` 原样
port 成 YGHV 内的 step205**（其 host 环用 SimpleSvm 的 VpData/HostStackLayout 布局与
"DriverEntry 线程即 host 环"模型），仅把两处 handler 换成 YGHV 语义：CPUID 处理里
加 VMMCALL/心跳识别、以及后续挂 NPF 页保护。**NPT 复用 YGHV 现有 `g_npt`。**
这样"能跑的进出模型"逐字来自 C0 已验证的 SimpleSvm，差异只剩我们主动加的 hook，
可二分定位。**代价**：205 是新 trampoline + 新 VpData 布局，实现约 200-300 行
（含 asm），且**每轮验证要一次 boot**；预计 2-4 轮收敛。C0（原版 SimpleSvm）已是
"移植正确即会工作"的下界证明——若 205 port 忠实却仍不如 C0，即说明差异在我们引入的
NPT 初始化或 handler，而不是进出模型。

**执行前必须与用户确认**：这仍是持续消耗物理重启的硬活；C0 已达成关键科学结论
（OS-as-guest 在本机可行）。是否现在就投入 205 移植，由用户定。
