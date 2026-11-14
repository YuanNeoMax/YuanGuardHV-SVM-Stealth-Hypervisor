# YuanGuardHV

> AMD-V (SVM/NPT) 隐形 Hypervisor —— 把进程保护逻辑下沉到虚拟化层。
> 前身是内核驱动版 YuanGuard（Minecraft/Forge 进程保护），本仓库将其保护能力迁移到 AMD SVM 虚拟化层实现。

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

**⚠️ 实验性项目。** 本项目涉及内核模式虚拟化、驱动加载与反侦查（stealth）技术，仅供拥有合法授权的安全研究/学习用途。驱动加载需要管理员权限与测试签名环境，运行不当可能导致系统崩溃（蓝屏）。详见文末[合规与法律声明](#合规与法律声明)。

---

## 目录

- [一句话现状](#一句话现状)
- [项目背景](#项目背景)
- [核心架构](#核心架构)
- [性能（实测）](#性能实测)
- [目录结构](#目录结构)
- [功能里程碑](#功能里程碑)
- [环境要求](#环境要求)
- [构建](#构建)
- [加载与运行](#加载与运行)
- [客户端用法](#客户端用法)
- [测试](#测试)
- [文档索引](#文档索引)
- [已知限制与路线图](#已知限制与路线图)
- [版本与变更记录](#版本与变更记录)
- [合规与法律声明](#合规与法律声明)

---

## 一句话现状

**真实 Windows 以 guest 态运行在全部 12 个逻辑核上，12 线程性能 1.05×，G25 功能验证 17/17，卸载 0.07 秒且判据干净。**

```
默认配置      : 真 guest + 全核（9.577 起）
12 线程吞吐   : 840-860 ms（基线 794-804 ms）≈ 1.05-1.08×
功能验证      : G25 17/17 ALL PASS
卸载          : 0.07 s，invmrun at unload = 0x0
```

> 判据 `invmrun at unload = 0x0` 是唯一可信的「没坏」证据：
> 被虚拟化的每个核都干净离开了 VMRUN，镜像可以安全 unmap。

---

## 项目背景

- 早期版本是运行在 Ring 0 的内核驱动（YuanGuard），直接挂钩内核函数来保护 Minecraft/Forge 进程。
- 本仓库（YuanGuardHV）把同样的一套保护逻辑（内存写保护、进程终止保护、句柄保护、常驻）**搬进 AMD SVM 虚拟化层**，通过 NPT（Nested Page Table）+ VMMCALL + VMEXIT 拦截实现：
  - 保护语义与宿主 OS 解耦，宿主内核对保护逻辑"看不见"；
  - **OS-as-guest 已打通**：vendored 上游 SimpleSvm 逐字进场，真实 Windows 以 guest 态运行，
    页写保护全链（arm→NPF→岛内裸判→重武装→看门狗→在线卸载）硬件实证。
- 双路线并存：非驻留合成 guest 路线（驱动在真实 Windows 中运行，用合成 resident guest 承载 VMRUN 验证链）+ **206 主线**（OS-as-guest，全核虚拟化下的跨进程页保护）。

---

## 核心架构

```
                    ┌──────────────────────────────────┐
   Ring 0 (kernel)  │   yuanguard_hv.sys (DriverEntry)  │
                    │   ├─ svm_core    SVM 初始化/VMRUN   │
                    │   ├─ npt_core    NPT 映射/NPF 权限  │
                    │   ├─ vmexit      VMEXIT 分发        │
                    │   ├─ vmmcall     VMMCALL 处理/认证  │
                    │   ├─ multi_core  每核系统线程/DPC    │
                    │   ├─ protect     保护逻辑           │
                    │   ├─ control_device  IOCTL 控制面   │
                    │   └─ loader_stealth  加载隐藏(门控)  │
                    └──────────────┬───────────────────┘
                                   │ DeviceIoControl (\\.\YuanGuardHV)
                    ┌──────────────▼───────────────────┐
   User mode        │  yghv_ctl.ps1  /  YghvCtl (Java)  │
                    └──────────────────────────────────┘
```

- **SVM 虚拟化**：`VMRUN`/`VMLOAD`/`VMSAVE`/`CLGI`，VMCB 字段对照 AMD APM 实现。
- **206 主线（OS-as-guest）**：`svm_simplevm206.cpp`（vendored 上游 SimpleSvm 逐字进场，仅 4 处缝合）承载全核虚拟化；NPF/CR3/CR4/#DB 在 VMEXIT 内由**岛内裸判**处理（GIF=0、无锁、禁一切 NT API），页写保护裁决与遥测环全部岛内完成。

### guest 是什么：桩模式 vs 真 guest

| | 桩模式（stub） | **真 guest（默认）** |
|---|---|---|
| guest RIP 来源 | `Sv206GuestStub`（我们自己的 `hlt/vmmcall` 循环） | `RtlCaptureContext` 捕获的真实线程上下文 |
| guest 里跑的是 | 我们的空转桩 | **真实 Windows 代码** |
| 退出量（12 核） | 1,255,119 次 | **27,914 次** |
| 12 线程吞吐 | 3,211–3,505 ms（4.0–4.4×） | **840–860 ms（1.05×）** |
| G25 功能验证 | 15/17（6 核时） | **17/17** |

> **桩模式的开销来自桩自己**：它每核每秒几万次 `hlt/vmmcall` 空转。
> 真 guest 的 guest 是 Windows 自己，只在真正需要拦截的地方退出。
> 所以「减核换性能」优化的是桩的空转，不是 hypervisor。

---

## 性能（实测）

基准：`tools/bench_core.cs` —— 固定工作量（12 线程 × 3 亿次 xorshift）、
编译后执行、**带正确性门**（12 条 lane 必须返回互不相同且非零的 sum）。
只报时间的基准分不清「快」和「错」。

| 配置 | 耗时 (ms) | 相对基线 |
|---|---|---|
| 基线（驱动 STOPPED） | 794 / 801 / 804 | **1.00×** |
| 桩模式 6 核（9.527–9.576 的旧默认） | 904 / 1070 | 1.13–1.34× |
| 桩模式 12 核 | 3211 / 3505 | 4.0–4.4× |
| 真 guest 6 核 | 815 / 825 / 875 | 1.03–1.09× |
| **真 guest 12 核（默认）** | **799 / 812 / 823 / 815 / 838** | **1.01–1.08×** |

**handler 单次成本 0.4 µs**（1,394 ticks），全核总 CPU 时间 0.012 s。
成本不在 hypervisor 里 —— 9.521 早就量过 handler 只占 0.7%，本轮数据与它一致。

CPU 占用：自旋版 100% → **阻塞版 21–29%**（tick 循环用 `KeDelayExecutionThread` 让出处理器）。

> **历史结论更正**：9.527/9.537 曾记录「6 核 1.2×（T7 FAIL）/ 11 核 ≈4×（T7 PASS）/
> 12 核 11.6×（T7 PASS）」，并据此把默认设为物理核数。**那个 11.6× 是桩模式的数字。**
> 真 guest 全核 1.05× 且 G25 17/17，旧默认在所有配置下都不是最优。

---

## 目录结构

```
YuanGuardHV/
├── build.bat                 构建（源级检查 → clang-cl → link → 签名）
├── hv/                       内核源码（48 个文件）
│   ├── svm_simplevm206.cpp   206 主线（vendored SimpleSvm + 4 处缝合）
│   ├── svm_simplevm206_x64.asm  VMRUN 循环 / 桩 / 退出 thunk
│   ├── npt_core.c            NPT 映射与权限
│   ├── protect.c             页写保护（同步守卫）
│   ├── mem_*.c               内存引擎（read/write/pte/bp/cache/frame/watch/...）
│   ├── yghv_guard.c          隐藏表完整性（SHA-256）
│   ├── yghv_sentry.c         攻击面清单与 escape 记录
│   └── common/               头文件（43 个）
├── tests/                    构建期检查器（19 个脚本 + 2 个运行器）
├── tools/
│   ├── yghv_ctl.ps1          PowerShell 客户端（84 条命令）
│   └── yghv_client/          Java 客户端（18 条命令）
└── docs/                     见 docs/INDEX.md
tools/                        独立工具
├── bench_core.cs             CPU 基准（固定工作量 + 正确性门）
└── bench_load.ps1            早期 PowerShell 版（慢 400 倍，留作对照）
```

---

## 功能里程碑

| 阶段 | 内容 | 状态 |
|---|---|---|
| Phase 1 | 骨架 + 核心头文件（VMCB/SVM/NPT/控制面） | ✅ |
| Phase 2a–2d | SVM init + VMRUN 单核 → 多核 DPC → 裸机回归 | ✅ |
| Phase 3 v1 | 内存写保护 / 终止保护 / 句柄保护 / 常驻（v27–v31） | ✅ |
| 真实目标 | Java/JNI 客户端、真实进程接入（`protect/unprotect/scan`） | ✅ |
| **206-A/B** | vendored 上游 SimpleSvm 逐字进场 + coexist 全核虚拟化 + 512GB identity NPT + 在线卸载 | ✅ |
| **206-C1–C5** | IOCTL 控制面可用 → 页写保护端到端 → 控制面加固 → 跨进程保护 → 实进程试点 | ✅ |
| **206-C6** | 假写路线：六条 NPT 冲刷路径全部证伪（Zen3 硬件事实）→ 换设计为周期同步守卫 | ✅ |
| **C 线（周期同步守卫）** | **不翻转 NPT 权限**，轮询快照比对 + 漂移回滚；延迟 92s → **2.3s**；64 页 @20 polls/s 仅 **0.38% 单核** | ✅ |
| **Tier 1.5 内存基准** | `mem-bench`：Engine A vs B，实测 B 快 **3.7–4.0×** | ✅ |
| **Tier 2.4 零拷贝窗口** | 4K NonPagedPool 共享页 + seqlock，guest/host 双向字节级验证 | ✅ |
| **Tier 7.1 自我隐藏** | `npt-view`（UNMAP/DECOY/大页三判据）+ `sections` 段报告 + `.pdata` 段隐藏实测 + guard 表页 + 符号链接自恢复 | ✅ |
| **性能：VMEXIT 削减** | 修复 `VMEXIT_VMMCALL` 不推进 RIP 的真 bug，退出率 **1418 万/秒 → 738/秒** | ✅ |
| **全仓审查（9.474–9.496）** | 逐文件审查 33 个 `.c`；修 **21 个缺陷**；新增 5 个构建期检查器（每个都做过注入验证） | ✅ |
| **★ 全核真 guest（9.573–9.574）** | 停止问题解决：**1.05× 吞吐 + 干净卸载**（`invmrun=0x0`） | ✅ |
| **★ 全核真 guest 成为默认（9.577）** | 注册表清空即生效；G25 **17/17** | ✅ |
| 旧 B 路线 | 整机进 guest 的 APIC 虚拟化实验 | ⛔ 已由 206 通路取代 |

### 全核真 guest 是怎么打通的（两个独立的根因）

七次蓝屏曾被归成一个原因（"结构上缺一个 host 执行流"）。**那个归因是错的。**

**缺陷一（`0xD1`）**：`RtlCaptureContext` 把 guest RIP 设在 `SvVirtualizeProcessor` 内部，
guest 走完 `return` 一路展开到 `Sv206CoreThread` 尾部执行 `terminateFn` ——
**全程在 guest 里**。承载 VMRUN 帧的那张内核栈被调度器回收，帧变野指针。

```
判据：桩模式 invmrun at unload = 0x0
      真 guest invmrun at unload = 0x6   <- 六个孤儿帧
```

`0x6` 从来不是"停止信号太弱"，是**六个核的栈之记录已经不存在了**。
修法：尾部改成 tick 循环，永不返回。

**缺陷二（`0x101 CLOCK_WATCHDOG`）**：第一版 tick 循环是 `for(;;) { spin; vmmcall; }`，
每个虚拟化核一个 100% 忙线程。6/12 时还剩 6 个逻辑核给 Windows 处理时钟；
12/12 时一个不剩，watchdog 就响。

```
FAULTING_PROCESSOR: 4
CLOCK_WATCHDOG_TIMEOUT_INTERRUPTS_DISABLED_nt!KiSystemCall64
```

修法：`KeDelayExecutionThread(KernelMode, FALSE, -1ms)` 让出处理器。**CPU 100% → 21–29%。**

---

## 已验证平台

**只有下面这一台是真机验证过的。** 列出它是为了让读者能判断"我的机器像不像"，
而不是暗示广泛兼容。

| 项 | 值 | 验证内容 |
|---|---|---|
| CPU | AMD Ryzen 5 5500（Zen3 Cezanne，Family 19h Model 50h，6 物理核 / 12 逻辑核） | 全核虚拟化、NPT、SVM、`INVLPGB` 缺失 |
| OS | Windows 10 Pro for Workstations 22H2（10.0.19045，x64） | 加载/卸载/功能全链 |
| 加载条件 | `testsigning=Yes` + `nointegritychecks=Yes` | 自签名驱动可加载 |
| 内存 | 16 GB | 512GB identity NPT 可用 |

**未验证**：Zen1/Zen2/Zen4、Intel（本项目的代码是 AMD SVM 专用，不支持 VT-x）、
Windows 11、Server 版本、嵌套虚拟化环境（VMware/Hyper-V 内的 SVM 暴露受限，
VM 内路线已判定不可用）。

## 环境要求

构建机需要：

| 工具 | 路径（本机示例） |
|---|---|
| clang-cl（LLVM） | `C:\Program Files\LLVM\bin\clang-cl.exe` |
| MSVC link.exe | Visual Studio 2022 BuildTools `…\MSVC\14.44.35207\bin\Hostx64\x64\link.exe` |
| Windows SDK / WDK | `C:\Program Files (x86)\Windows Kits\10`（Include 10.0.19041.0，signtool 10.0.26100） |
| signtool | WDK `…\bin\10.0.26100.0\x64\signtool.exe` |

运行（加载驱动）需要：

- 管理员权限，且内核已开启测试签名（`bcdedit /set testsigning on` 或使用已签名驱动）；
- 测试证书 `yuanguard_test.cer`。

### 本机开发/测试环境（实测）

| 项 | 值 |
|---|---|
| CPU | AMD Ryzen 5 5500（**Zen3 Cezanne，Family 19h Model 50h**，6 物理核 / 12 逻辑核） |
| 内存 | 16 GB |
| 宿主系统 | Windows 10 专业工作站版 22H2（10.0.19045，64 位） |
| 加载条件 | `testsigning=Yes` + `nointegritychecks=Yes` |
| 构建工具链 | clang-cl（LLVM）+ MSVC link 14.44.35207 + WDK 10.0.19041/10.0.26100 + signtool |

> - **CPU 型号是权威事实**：`INVLPGB`/`TLBSYNC` 是 Zen4+ 指令，**本机 Zen3 没有**（CPUID 实测确认）。
> - **单机交互式内核调试不可行**（内核断点挂起整机 + 本机 NIC 不支持 KDNET），
>   唯一路径是第二台物理机。详见 `docs/YUANMOD_HANDOFF_CURRENT.md` 9.296。

---

## 构建

```bat
cd YuanGuardHV
set YGHV_BAREMETAL_STEP=206
set YGHV_206B_COEXIST=1
set YGHV_206B_GNPT=1
build.bat
```

流程（`build.bat`）：

1. 先运行 `tests\run_source_checks.ps1`（**17 个脚本 / 49 条 PASS**，含 33 条自检），失败即中止；
2. clang-cl 编译 `hv\*.c` / `hv\*.cpp` + `svm_trampoline.S`；
3. MSVC link 链接为 `bin\yuanguard_hv.sys`，**同时产出 `bin\yuanguard_hv.map`**；
4. 运行 `tests\run_map_checks.ps1`（页布局检查，必须链接后跑）；
5. signtool 用 `yuanguard_test.cer` 签名；
6. 打印 SHA256 哈希。

> **`/MAP` 的用途**：崩溃取证时把 `模块+偏移` 一次映射到函数名。
> 前三次蓝屏都要手工反汇编才能定位（例：`0x12E13 → memmove`）。

### 运行时开关（注册表，`HKLM\SYSTEM\CurrentControlSet\Services\yghvpa\Parameters`）

**两个键都不存在 = 默认配置（真 guest + 全核）。**

| 值 | 类型 | 默认（不存在时） | 作用 |
|---|---|---|---|
| `RealGuestMode` | REG_DWORD | **开** | **非零 = 关闭真 guest，退回桩模式**（语义是"禁用"，9.577 起反转） |
| `CoreCount` | REG_DWORD | **全部处理器** | 限制虚拟化的核数；`0` 或不存在 = 全核 |

> ⚠️ **`RealGuestMode` 的语义在 9.577 反转了**：以前 `=1` 是打开，现在 `=1` 是关闭。
> 一台机器上如果留着旧约定写下的 `RealGuestMode=1`，现在会**被它自己的覆盖关掉**。
> 这个方向是有意选的 —— 事故应该倒向已验证的桩模式。

### 编译期门控（`set YGHV_XXX=1` 后构建）

| 变量 | 作用 |
|---|---|
| `YGHV_REAL_GUEST_PINNED` | 编译期钉死真 guest 为关（应急用） |
| `YGHV_LOADER_STEALTH` | 启用加载隐藏（模块摘链，**默认不做**，见限制 7） |
| `YGHV_UNLOAD_GUARD` | 反卸载守卫 |
| `YGHV_BAREMETAL_STEP` | 裸机 step 选择（`206` = OS-as-guest 主线） |
| `YGHV_206B_COEXIST` / `YGHV_206B_GNPT` | 206 路线：coexist 全核虚拟化 + 512GB identity NPT |
| `YGHV_SINGLECORE` | 单核 guest |
| `YGHV_CATCHALL` | 拦截 guest 全异常 + HLT（诊断） |
| `YGHV_R1_EXCLUDE_PRIVATE` / `YGHV_REAL_HOOK_TEST` | R1/真实 hook 实验 |

输出：`YuanGuardHV\bin\yuanguard_hv.sys`

---

## 加载与运行

以管理员身份运行（测试签名已开启）：

```powershell
# ① 构建（约 10 分钟）
$env:YGHV_BAREMETAL_STEP='206'; $env:YGHV_206B_COEXIST='1'; $env:YGHV_206B_GNPT='1'
cd YuanGuardHV; cmd /c build.bat
#   看这三行：ALL SOURCE CHECKS PASS / ALL MAP CHECKS PASS / Build SUCCESS

# ② 部署（必须先看哈希，再 start）
Copy-Item .\bin\yuanguard_hv.sys C:\yghv_pa.sys -Force
(Get-FileHash C:\yghv_pa.sys -Algorithm SHA256).Hash.Substring(0,16)

# ③ 加载 + 冒烟
sc.exe start yghvpa
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\yghv_ctl.ps1 sys-health
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\yghv_ctl.ps1 virt-counts
#   默认配置应看到：entries=12  early-returns=12  cores-marked=12

# ④ 卸载（这是最重要的验收）
sc.exe stop yghvpa
Get-Content C:\Windows\yghv_progress.log | Select-String "invmrun at unload"
#   必须看到：s206 invmrun at unload = 0x0000000000000000
```

- 服务名：**`yghvpa`**（不是 `yuanguard`）；控制设备：`\\.\YuanGuardHV`。
- **卸载判据 `invmrun=0x0` 是唯一可信的「没坏」证据**。卸载耗时约 0.07 秒。

---

## 客户端用法

### PowerShell 客户端

`tools\yghv_ctl.ps1` —— **84 条命令**，覆盖控制面、内存引擎、保护、隐藏、进程控制、注入、页表分析、诊断。

```powershell
$ctl = 'D:\yuanguard\YuanGuardHV\tools\yghv_ctl.ps1'
powershell -NoProfile -ExecutionPolicy Bypass -File $ctl sys-health
powershell -NoProfile -ExecutionPolicy Bypass -File $ctl mem-read <pid> <hex_va> u64
```

常用命令分组：

| 组 | 命令 |
|---|---|
| 诊断 | `sys-health` `virt-counts` `exit-codes` `handler-cost` `guest-start` `service-name` |
| 内存 | `mem-read` `mem-write` `mem-pte` `mem-bench` `mem-cache` `mem-frame` `mem-watch` |
| 保护 | `protect-page` `unprotect-page` `config-sync` `mem-bp` |
| 隐藏 | `sys-guard`（arm/verify/hidecore/list/uncovered）`npt-view` `npt-raw` `sections` `pa-section` `link-hide` `byovd-scan` |
| 观察 | `sys-sentry` `exit-mask` `cpuid-view` `mem-time` |
| 认证 | `sys-auth` `sys-crypto` |

### Java 客户端

`tools\yghv_client\`（18 条命令），用法见该目录 `README.md`。

---

## 测试

### 构建期检查器（`tests\`，19 个脚本 + 2 个运行器）

| 脚本 | 检查什么 |
|---|---|
| `ioctl_parity.ps1` | C ↔ PowerShell ↔ Java ↔ JNI 的 IOCTL 编号一致（当前 **60/60/60**）|
| `command_parity.ps1` | README ↔ 两个客户端的命令一致（当前 **PS=84 / Java=18**）|
| `safety_checks.ps1` | 安全红线（pause-under-lock 作用域、`STATUS_DEVICE_BUSY`、`0x5AA` 命名）|
| `irql_contract_check.ps1` | IRQL 可达性、岛内读区域、不许有「说谎的 `*_bare`」|
| `exit_rip_contract.ps1` | 38 个 `#VMEXIT` case 的 RIP 契约（ADVANCE / INJECT / HOLD 三类断言）|
| `ioctl_buffer_size.ps1` | 客户端缓冲覆盖它读的每个偏移 |
| `count_as_extent.ps1` | 未压缩的表不许用「活计数」当扫描上界 |
| `ioctl_output_bounds.ps1` | 每个写回复缓冲的 handler 先测 `out_len` |
| `service_name_check.ps1` | 每个 `sc` 调用必须用变量作服务名，不许写字面量 |
| `client_undefined_vars.ps1` | 客户端读到的每个变量名都被赋过值 |
| **`stub_stack_contract.ps1`** | **`Sv206GuestStub` 体内禁 push/pop/call/ret/lea rsp/[rsp]**，且必须仍有 `vmmcall` |
| **`docs_check.ps1`** | **文档门**：相对链接、索引行数、空记录、最新编号、归档份数、状态块自洽、镜像哈希、检查器数量、目录树份数 |
| **`win32_error_names.ps1`** | **错误码名门**：文档里「数字 + `ERROR_*`」的配对必须与 OS 表一致（`[HIST]` 引用除外）|
| **`eol_check.ps1`** | **行尾门**：同一文件不许混用 CRLF 与裸 LF，也不许有孤立 CR（混排会把 +37 行的真实改动撑成 1301 行 diff）|
| **`source_citation_check.ps1`** | **引用门**：文档里 `文件.c:123` 形式的引用必须指向真实存在的、非空行（行号越界会报，**漂移到空行也会报** —— 9.587 手工审计 18 条里有 7 条已漂移，且全部仍在文件内）|
| `page_layout_check.ps1` | 隐藏表不与 guard 状态共页、岛内读区域页对齐（链接后跑）|
| `string_encryption_check.ps1` | 生成表与 `tools/gen_strtab.py` 逐字节一致；镜像里没有任何导出名以 ASCII 或 UTF-16 形式残留（链接后跑，覆盖 import table 这种源码 grep 看不见的来源）|

> **每个检查器都必须能失败。** 三个曾报假 PASS（正则够不到目标文本）。
> 现在 **9 个变异 + 2 条反向对照**在每次构建时跑：往副本注入真缺陷要求抓到，
> 同时要求两条「本来就该通过」的输入保持沉默。
> `docs_check` 的反向对照有两条：代码里假装的链接必须**不被**报；
> INDEX 声称的镜像哈希若正好是真实值，必须**通过**（只测「能抓到」不算测过）。
>
> **文档里的数字不靠人抄。** `run_source_checks.ps1` 把实测的
> `scripts= / passes= / selftests=` 写进 `tests/check_stats.txt`，
> `docs_check` 拿它比对 README / QUICKSTART 里的「N 脚本 / M PASS」。
> 加一条检查 → 数字变了 → 文档没跟上就构建失败。9.583 加完当场被它拦了一次。

### G25 功能验证（17 项，实机）

```
T1  SHA-256 自检        T2  sys-guard arm      T2b 未篡改时 verify
T3  hidecore 藏自己页    T4  藏完仍能工作        T4b 隐藏页数上报
T5  uncovered 报告      T6  SHA-256 篡改检测    T7  诱饵页（guest 读零）
T8  exit 计时           T9  escape 记录         T10 攻击面清单
T11 sys-health          T12 全功能回归          T13 干净卸载
```

**当前默认配置：17/17 ALL PASS。**

### 性能基准

```powershell
csc /nologo /optimize+ /out:tools\bench_core.exe tools\bench_core.cs
.\tools\bench_core.exe -threads 12 -iters 300000000
```

输出 `elapsed_ms` + `CHECK OK`。**只报时间的基准分不清「快」和「错」** ——
正确性门要求 12 条 lane 返回互不相同且非零的 sum。

---

## 文档索引

**先读 [`docs/INDEX.md`](docs/INDEX.md)** —— 按问题索引，根目录 8 份，其余在 `archive/`。

| 文档 | 内容 |
|---|---|
| **`docs/INDEX.md`** | **★ 唯一入口** —— 按「你想知道什么」索引全部文档 |
| **`docs/QUICKSTART.md`** | **★★ 跨会话快速承接** —— 三分钟上手 / 当前状态 / 下一步 / 八个坑 |
| `docs/RESUME_PROMPT.md` | 深一层：铁律 / 架构约束 / 观测手段 / 方法论 |
| **`docs/TIER_LEDGER.md`** | **Tier 清单最终台账** —— 8 完成 / 9 不做（附理由）/ 1 需重启 |
| `docs/YUANMOD_HANDOFF_CURRENT.md` | 逐条变更记录 9.500 起（开头有索引，标题 + 真实行数）|
| `docs/WINDOWS_AS_GUEST_FINDINGS.md` | Windows-as-Guest 全部结论（唯一权威）|
| **`docs/KERNEL_API_CHECKLIST.md`** | **★ 写内核代码前必读** —— 三次蓝屏换来的 API 契约核对清单 |
| `docs/archive/` | 9.1–9.499 历史交接、2026-08 评审与研究、Tier 设计计划书 |
| **[`CHANGELOG.md`](CHANGELOG.md)** | **版本级变更摘要** —— 两个版本之间发生了什么（含 Breaking 标注）|
| **[`docs/ISSUES.md`](docs/ISSUES.md)** | **★ 唯一的问题清单** —— P0–P3 分级 + 版本规划 + 判据纪律 |

---

## 已知限制与路线图

1. **OS-as-guest 已打通，且现在是默认**：vendored 上游 SimpleSvm 逐字进场 + coexist 全核虚拟化，
   真实 Windows 以 guest 态运行，页写保护全链硬件实证。
   早期"平台级硬冻结"结论经 C0 原版 SimpleSvm 实机对照推翻（errata 1363/1235 属 Family 17h 编号系，本机 Zen3 不适用）。
2. **★ Zen3 NPT TLB 限制（硬件事实，已绕开）**：NPT（GPA→HPA）翻译**不响应任何已探索的软件冲刷机制** ——
   `TlbControl=1`（只冲 guest TLB）、`TlbControl=2`（FLUSH ALL，实测无效）、`nCR3` 变更
   （attempts == consulted == 29,697,622）、`INVLPGA`（打错层）、`INVLPGB`（Zen3 无此指令）全部证伪。
   **处置：换设计** —— 周期同步守卫**根本不翻转 NPT 权限**，硬件从不需要失效翻译。
3. **同步守卫无法区分写入来源（设计取舍）**：目标进程**自己**的合法写入同样被回滚 ——
   这是"不依赖 TLB"换来的代价。实测：目标持续自写仍被钉在单一值（70% 采样同值）。
4. **轮询间隔下限受系统定时器粒度约束**：实测 `NtQueryTimerResolution` 为 `min=15.62ms`，
   请求 1ms 实际得到 ~9.5–15.6ms，**实用下限约 16ms**。
5. **WPM 内核中介写旁路**：`WriteProcessMemory` 类内核 API 经 `MmCopyVirtualMemory`+`KeStackAttachProcess`
   以"目标 CR3 + cpl=0"完成写，现行裁决（is_target_cr3 ‖ cpl==0 → ALLOW）无法区分它与内核合法写。
   **此项不再是主线阻塞**（C 线走同步守卫路线）。
6. **DENY 的 AV 报告地址为 VA 0**：精确 GVA 重建在实测平台不可实现
   （`MmGetVirtualForPhysical` 选择性失效 + 直接映射基址不可无故障验证）。
   DENY 语义本身无损（确定性递 AV、零风暴）。
7. **隐形是尽力而为**：内核驱动在真实 Windows 中加载，绝对隐形不现实；
   `loader_stealth`（模块摘链）**默认不做** —— 它是 PatchGuard 巡视的表之一，
   且只摘 `InLoadOrderLinks` 属"部分 stealth"，风险付了收益没拿到。
   不承诺绕过任何具体反作弊产品。
   - **7.1 自我隐藏的边界（实测）**：`.pdata` 段可藏（guest 读零、host 可读）；
     `.text` 与 `.rdata` **不可藏**（前者是取指页、后者含 IAT，实测 `0x3B C0000005, rip=0`）。
8. **★ CPU 开销已大幅降低（此条 2026-11 更正）**：
   - **旧结论（已作废）**：「12 个核持续在 guest 里运行，基线 29% → 加载后 100%，无法在本机消除」。
     那句话描述的是**桩模式**，而桩的开销来自它自己的 `hlt/vmmcall` 空转（1,255,119 次退出）。
   - **现在（真 guest，默认）**：CPU **21–29%**，12 线程吞吐 **1.05×**，退出量 **27,914 次**。
   - 曾经以为的"唯一解法是换一台有 C-state 的机器"**不再成立** —— 换 guest 形态就够了。
9. **单机交互式内核调试不可行**：内核断点 = 挂起整个 OS（含调试器自身），且本机 NIC 不支持 KDNET。
   **唯一路径 = 第二台物理机**（详见 9.296）。
10. **`RealGuestMode` 语义已反转**：现在是"禁用开关"，不是"启用开关"。见[构建](#构建)一节。
11. **路线图**：真实产品目标（Minecraft/Forge）目标选择/页选取流程化 → R1 私有内存剔除 →
    MSR/IO/处理器层隐藏（需裸机/KVM）→ 产品化整合。

---

## 版本与变更记录

| 版本 | 主题 | 关键数据 |
|---|---|---|
| **v0.4.2**（当前）| P1-3 全部结案 + 错误码门 + 文档侧 5 处过期值 | 目标自写 10/10 存活；外来写 1/1 回滚 |
| v0.4.1 | 配置不一致可见化 + 真 guest 下重测内存基准 | B 比 A 快 **1.8–2.4×**；真 guest 下 B 慢 13%（NPT 代价）|
| v0.4.0 | 全核真 guest 成为默认 | 12 线程 **1.05×**、G25 **17/17**、卸载 0.07 s |
| v0.3.0 | 周期同步守卫（绕开 Zen3 NPT TLB 限制）| 延迟 92 s → **2.3 s**，64 页 @20 polls/s 0.38% 单核 |
| v0.2.0 | OS-as-guest 206 里程碑 | 页写保护全链 PASS |
| v0.1.0 | 首个可用版本 | SVM + 多核 + 三种保护 |

**完整摘要见 [`CHANGELOG.md`](CHANGELOG.md)**；逐条工程记录（编号 9.x）见
[`docs/YUANMOD_HANDOFF_CURRENT.md`](docs/YUANMOD_HANDOFF_CURRENT.md)。

> ⚠️ **v0.4.0 含破坏性变更**：`RealGuestMode` 的语义从"启用"反转为"禁用"。
> 详见[构建](#构建)一节的运行时开关表。

---

## 合规与法律声明

- 本项目为安全研究与学习用途，仅限在**你拥有或获授权**的机器上使用。
- 驱动加载需管理员权限与测试签名；内核级虚拟化若配置不当可能导致系统崩溃或数据丢失，风险自负。
- 与反作弊系统对抗可能违反游戏/平台服务条款。使用者须自行确认用途合规，作者不对滥用造成的封号、法律后果等负责。
- 本仓库采用 **MIT License**（见根目录 `LICENSE`）。
