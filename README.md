# YuanGuardHV

> AMD-V (SVM/NPT) 隐形 Hypervisor —— 把进程保护逻辑下沉到虚拟化层。
> 前身是内核驱动版 YuanGuard（Minecraft/Forge 进程保护），本仓库将其保护能力迁移到 AMD SVM 虚拟化层实现。

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

**⚠️ 实验性项目。** 本项目涉及内核模式虚拟化、驱动加载与反侦查（stealth）技术，仅供拥有合法授权的安全研究/学习用途。驱动加载需要管理员权限与测试签名环境，运行不当可能导致系统崩溃（蓝屏）。详见文末[合规与法律声明](#合规与法律声明)。

---

## 目录

- [项目背景](#项目背景)
- [核心架构](#核心架构)
- [目录结构](#目录结构)
- [功能里程碑](#功能里程碑)
- [环境要求](#环境要求)
- [构建](#构建)
- [加载与运行](#加载与运行)
- [客户端用法](#客户端用法)
- [测试](#测试)
- [文档索引](#文档索引)
- [已知限制与路线图](#已知限制与路线图)
- [合规与法律声明](#合规与法律声明)

---

## 项目背景

- 早期版本是运行在 Ring 0 的内核驱动（YuanGuard），直接挂钩内核函数来保护 Minecraft/Forge 进程。
- 本仓库（YuanGuardHV）把同样的一套保护逻辑（内存写保护、进程终止保护、句柄保护、常驻）**搬进 AMD SVM 虚拟化层**，通过 NPT（Nested Page Table）+ VMMCALL + VMEXIT 拦截实现：
  - 保护语义与宿主 OS 解耦，宿主内核对保护逻辑“看不见”；
  - **OS-as-guest 路线已打通（2026-10，206 里程碑）**：vendored 上游 SimpleSvm 逐字进场，全核无缝虚拟化（coexist），真实 Windows 以 guest 态运行，页写保护全链（arm→NPF→岛内裸判→重武装→看门狗→在线卸载）硬件实证。
- 双路线并存：非驻留合成 guest 路线（驱动在真实 Windows 中运行，用合成 resident guest 承载 VMRUN 验证链）+ **206 主线**（OS-as-guest，全核虚拟化下的跨进程页保护，见[功能里程碑](#功能里程碑)）。

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

- **SVM 虚拟化**：`VMRUN`/`VMLOAD`/`VMSAVE`/`CLGI`，VMCB 字段对照 AMD APM 实现；合成 guest 通过 VMMCALL 心跳与宿主通信。
- **206 主线（OS-as-guest）**：`svm_simplevm206.cpp`（vendored 上游 SimpleSvm 逐字进场，仅 4 处缝合）承载全核虚拟化；NPF/CR3/CR4/#DB 在 VMEXIT 内由**岛内裸判**处理（GIF=0、无锁、禁一切 NT API），页写保护裁决与遥测环全部岛内完成。
- **NPT**：512GB identity-map（206 路线）/ 16GB（合成路线）+ NPF（Nested Page Fault）权限注入；支持 `perm / range / translate / split` API（v25 单测 PASS）。
- **VMEXIT/VMMCALL**：VMEXIT 分发、VMMCALL 认证分层（控制面要求 SeDebugPrivilege / 目标绑定）。
- **控制面**：内核驱动暴露 `\\.\YuanGuardHV` 设备，IOCTL 命令号统一（`0x5947` 文件标志），PowerShell/Java 客户端共用同一接口（ioctl_parity 门禁强制三端一致）。
- **多核**：206 路线全核无缝虚拟化（每核 VMCB + per-VCPU current-CR3 槽 + CR3/CR4 写拦截仿真）；合成路线为每核一个系统线程，双核 10000 轮心跳稳定。

---

## 目录结构

```
yuanguard/
├── README.md                     # 本文件
├── .gitignore                    # 构建产物/日志排除规则
├── YuanGuardHV/                  # 主体
│   ├── build.bat                 # 构建脚本（先跑静态检查，再编译+链接+签名）
│   ├── hv/                       # 内核驱动源码
│   │   ├── main.c                # DriverEntry、resident/OS-as-guest 实验框架
│   │   ├── svm_simplevm206.cpp/.hpp/.asm  # 206 主线：vendored 上游 SimpleSvm（OS-as-guest 核心）
│   │   ├── svm_core.c            # SVM 初始化、VMRUN、VMCB（合成路线）
│   │   ├── npt_core.c            # NPT identity map + NPF 权限注入
│   │   ├── vmexit.c              # VMEXIT 分发（合成路线）
│   │   ├── vmmcall.c             # VMMCALL 处理与认证
│   │   ├── multi_core.c          # 每核系统线程
│   │   ├── protect.c             # 保护逻辑 + 岛内裸判桥 + **周期同步守卫**（C 线）
│   │   ├── control_device.c      # IOCTL 控制设备
│   │   ├── loader_stealth.c      # 加载隐藏（门控，默认关）
│   │   ├── svm_trampoline.S      # 进入/退出 guest 的汇编蹦床
│   │   └── common/               # 头文件（vmcb/svm_defs/npt/msr/cpuid/protect/crx/…）
│   ├── tools/
│   │   ├── yghv_ctl.ps1          # PowerShell 客户端（34 命令，主客户端）
│   │   ├── yghv_stealth_check.ps1# 只读痕迹自查
│   │   ├── build_simplesvm.bat   # 构建 vendored SimpleSvm
│   │   └── yghv_client/          # Java/JNI 客户端（YghvCtl，18 命令）
│   ├── tests/                    # 静态校验（编译自动执行）
│   ├── unload_driver.ps1         # 无重启卸载驱动（NtUnloadDriver）
│   ├── start_kd.ps1 / run_kd.bat # VM+KD 调试通道
│   └── yuanguard_test.cer        # 测试签名证书（仅测试环境）
├── docs/                         # 全程实验/决策/审计记录
│   └── KERNEL_API_CHECKLIST.md   # ★ 写内核代码前必读（API 契约核对清单）
└── reference/                    # EPT 参考实现（研究用途，独立于正式构建）
```

> `bin/`、`*.sys`、`*.obj`、`*.map`、日志与 `svm_trampoline.asm` 均被 `.gitignore` 排除，不入库。
> **发布镜像通过 GitHub Release 附件分发**（见 [功能里程碑](#功能里程碑) 的 v0.3.0 链接）。

---

## 功能里程碑

| 阶段 | 内容 | 状态 |
|---|---|---|
| Phase 1 | 骨架 + 核心头文件（VMCB/SVM/NPT/控制面） | ✅ |
| Phase 2a | SVM init + VMRUN 单核，10000 轮 VMMCALL 心跳 | ✅ |
| Phase 2b | NPT identity-map + NPF 权限注入验证 | ✅ |
| Phase 2c | 多核 DPC（每核系统线程，双核心跳稳定） | ✅ |
| Phase 2d | 物理机（裸机）回归 | ✅ |
| Phase 3 v1 | 内存写保护 / 终止保护 / 句柄保护 / 常驻（v27–v31） | ✅ |
| 真实目标 | Java/JNI 客户端、真实进程接入（`protect/unprotect/scan`） | ✅ |
| R1 安全地基 | VMMCALL 认证分层、NPT 权限 API | ⏳ 部分（VMware 嵌套阻塞部分验证） |
| 隐形 | 加载隐藏门控、痕迹自查工具 | ⏳ 默认关闭，待 kd 复核 |
| **206-A/B** | vendored 上游 SimpleSvm 逐字进场 + coexist 全核无缝虚拟化 + 512GB identity NPT + 在线卸载（~50ms） | ✅ |
| **206-C1** | OS-as-guest 下 IOCTL 控制面可用（PASSIVE 创建 + guest 态 IRP） | ✅ |
| **206-C2** | 页写保护端到端：arm(split+PRESENT-only)→NPF→岛内裸判（ALLOW 重开+TF+#DB 重武装 / DENY 注入 #PF）→读回 | ✅ |
| **206-C3** | 控制面加固：目标退出看门狗（根治泄漏→风暴）、NONE 静默重开、last-hit 查询（fn 0x80F） | ✅ |
| **206-C4** | 跨进程保护：外部控制器按 pid 武装目标页（fn 0x810/0x811），目标写 ALLOW、外部进程写 DENY→确定性 AV | ✅ |
| **206-C5** | 实进程试点（notepad 真实目标 + scan-pid/wpm-write）+ WPM 内核中介旁路定性实证 + 遥测环扩容 | ✅ |
| **206-C6** | 假写路线：六条 NPT 冲刷路径全部证伪（Zen3 硬件事实）→ **换设计为周期同步守卫** | ✅ |
| **C 线（周期同步守卫）** | **不翻转 NPT 权限**，后台轮询快照比对 + 漂移回滚；延迟 92s → **2.3s**（40×）；64 页 @20 polls/s 仅 **0.38% 单核** | ✅ |
| **Tier 1.5 内存基准** | `mem-bench`：Engine A(`MmCopyVirtualMemory`) vs B(PTE walk + 直读)，实测 B 快 **3.7–4.0×** | ✅ |
| **Tier 2.4 零拷贝窗口** | 4K NonPagedPool 共享页 + seqlock，guest/host 双向字节级验证，**零拷贝** | ✅ |
| **Tier 7.1 自我隐藏** | 观测面 `npt-view`（UNMAP/DECOY/大页三判据）+ `sections` 段报告（含 IAT 定位）+ **`.pdata` 段隐藏实测通过** + guard 表页 + 符号链接自恢复 | ✅ |
| **诊断指标** | `vmmcall` 计数 + 单核 `Rflags/VIntr/shadow/EventInj` 可读，`hlt` 类现象的判定从推测变为可验证 | ✅ |
| **性能：VMEXIT 削减** | 修复 `VMEXIT_VMMCALL` 不推进 RIP 的真 bug，退出率 **1418 万/秒 → 738/秒（降 19,200 倍）** | ✅ |
| **构建期检查** | `exit_rip_contract.ps1`：38 个 VMEXIT case 的 RIP 契约（三类断言）+ **自检**（每次构建注入缺陷证明能失败）| ✅ |
| **缺陷 C** | `DriverUnload` 补 `yghv_trace_close()`（progress.log 句柄泄漏，A/B 因果证明） | ✅ |
| 旧 B 路线 | 整机进 guest 的 APIC 虚拟化实验 | ⛔ 已由 206 通路取代（历史见 docs） |

> **发布**：[v0.3.0](https://github.com/YuanNeoMax/YuanGuardHV-SVM-Stealth-Hypervisor/releases/tag/v0.3.0)
> 为当前版本（周期同步守卫，镜像 `206c25_mmcopy`）。

---

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

### 本机开发/测试环境（2026-10 实测）

| 项 | 值 |
|---|---|
| CPU | AMD Ryzen 5 5500（**Zen3 Cezanne，Family 19h Model 50h**） |
| 内存 | 16 GB |
| 宿主系统 | Windows 10 专业工作站版 22H2（10.0.19045，64 位） |
| 加载条件 | `testsigning=Yes` + `nointegritychecks=Yes`（内核不校验驱动签名） |
| 测试虚拟机 | Windows 10 Pro 19045.2965（VMware 17.6.4） |
| 构建工具链 | clang-cl（LLVM）+ MSVC link 14.44.35207 + WDK 10.0.19041/10.0.26100 + signtool |

> - **CPU 型号是权威事实**：`INVLPGB`/`TLBSYNC` 是 Zen4+ 指令，**本机 Zen3 没有**
>   （CPUID 实测确认，见 9.298）。
> - **VM 内 AMD SVM 指令暴露受限**（VMware `vhv.enable` 嵌套限制），VM+KD 通道
>   已判定不可用，实机验证走**裸机**路线。
> - **单机交互式内核调试不可行**（内核断点挂起整机 + 本机 NIC 不支持 KDNET），
>   唯一路径是第二台物理机。详见 `docs/YUANMOD_HANDOFF_CURRENT.md` 9.296。

---

## 构建

```bat
cd YuanGuardHV
build.bat
```

流程（`build.bat`）：

1. 先自动运行 `tests\run_static_checks.ps1`（接口一致性 + 命令一致性 + 安全红线），失败即中止；
2. clang-cl 编译 `hv\*.c` / `hv\*.cpp` + `svm_trampoline.S`；
3. MSVC link 链接为 `bin\yuanguard_hv.sys`（WDM kernel driver），**同时产出 `bin\yuanguard_hv.map`**；
4. signtool 用 `yuanguard_test.cer` 签名；
5. 打印 SHA256 哈希。

> **`/MAP` 的用途**：崩溃取证时把 `模块+偏移` 一次映射到函数名。前三次蓝屏都要手工反汇编
> 才能定位（例：`0x12E13 → memmove`）。用法见 `docs/KERNEL_API_CHECKLIST.md` 第九节。

可用环境变量门控（`set YGHV_XXX=1` 后构建）：

| 变量 | 作用 |
|---|---|
| `YGHV_LOADER_STEALTH` | 启用加载隐藏（模块摘链） |
| `YGHV_UNLOAD_GUARD` | 反卸载守卫 |
| `YGHV_BAREMETAL_NO_RESIDENT` | 裸机模式去 resident guest |
| `YGHV_BAREMETAL_STEP` | 裸机 step 选择（`206` = OS-as-guest 主线） |
| `YGHV_206B_COEXIST` / `YGHV_206B_GNPT` | 206 路线：coexist 全核虚拟化 + 512GB identity NPT（与 STEP=206 配合） |
| `YGHV_SINGLECORE` | 单核 guest |
| `YGHV_APIC_SHADOW` | APIC 影子（B-1full，裸机等价物） |
| `YGHV_APIC_IDENTITY` / `YGHV_APIC_PASSTHROUGH` | APIC identity / passthrough 实验 |
| `YGHV_NO_APIC_SHADOW` / `YGHV_NO_MSV_DEEP` / `YGHV_NO_CR3_INTERCEPT` | 关闭对应实验路径 |
| `YGHV_HOST_ISR` | guest 中断改由宿主 ISR 服务 |
| `YGHV_CATCHALL` | 拦截 guest 全异常 + HLT（诊断） |
| `YGHV_V101_4VEC` | 只拦截 4 向量（#DF/#NP/#SS/#GP） |
| `YGHV_R1_EXCLUDE_PRIVATE` / `YGHV_REAL_HOOK_TEST` | R1/真实 hook 实验 |

输出：`YuanGuardHV\bin\yuanguard_hv.sys`

---

## 加载与运行

以管理员身份在宿主/VM 中运行（测试签名已开启）：

```powershell
# 安装并启动内核服务（示例：驱动置于 C:\yuanguard_hv.sys）
sc.exe create yuanguard type= kernel binPath= C:\yuanguard_hv.sys start= demand
sc.exe start yuanguard

# 查询状态
sc.exe query yuanguard

# 无重启卸载（仓库自带工具，NtUnloadDriver）
powershell -NoProfile -ExecutionPolicy Bypass -File YuanGuardHV\unload_driver.ps1
```

- 服务名：`yuanguard`；控制设备：`\\.\YuanGuardHV`。
- 加载后可用 `tools\yghv_ctl.ps1 state` 验证驱动响应。
- 迭代流程（本仓库惯例）：构建 → 归档 `bin\yuanguard_hv.sys` → 拷贝到目标机 `C:\yuanguard_hv.sys` → `sc.exe start yuanguard` → 回归 → `unload_driver.ps1` 卸载（不重启可反复）。

### 206 主线（OS-as-guest）加载

206 镜像以 `YGHV_BAREMETAL_STEP=206` + `YGHV_206B_COEXIST/GNPT` 构建（DriverEntry 末尾 dispatch 进 vendored SimpleSvm，**全核进入虚拟化**）：

```powershell
sc.exe create yuang206c2 type= kernel binPath= C:\yuanguard_hv_s206c2.sys start= demand
sc.exe start yuang206c2      # entry + coexist dispatch，桌面无感
# ...控制面/保护实验...
sc.exe stop  yuang206c2      # 在线卸载（实测 ~50-130ms），无需重启
```

- 与合成路线的加载互斥（脚本自带残留 VMM guard，`yuang206*`/SimpleSvm 在 RUNNING 会拒绝启动）。
- 206 模式下控制面命令（`protect-page`/`mmf-*`/`lasthit` 等）在 guest 态 IRP 全可用。
- 完整实验脚本（run_c7…run_c16 系列）与判读记录见 `docs/YUANMOD_HANDOFF_CURRENT.md`。

---

## 客户端用法

### PowerShell 客户端

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File YuanGuardHV\tools\yghv_ctl.ps1 <command>
```

常用命令（**共 34 个**，完整列表见 `yghv_ctl.ps1` 或 `tools/yghv_client/README.md`）：

| 命令 | 说明 |
|---|---|
| `state` / `target` / `list-targets` / `list-pages` / `list-hooks` / `lasthit` | 查询状态 |
| `set-target <pid>` | 设置目标进程 |
| `add-page <hex_va>` / `remove-page <hex_va>` | 增/删受保护页（调用者自身地址空间） |
| `protect-page <pid> <hex_va>` / `unprotect-page <pid> <hex_va>` | **跨进程武装**（fn 0x810/0x811，206-C4：外部控制器对已注册目标按 pid 武装） |
| `start` / `stop` | 启动/停止保护 |
| `install-hook <name\|hex_va> [hook_id]` / `remove-hook <hook_id>` | 挂钩管理 |
| `config` | 读配置（含 `auto_disarm` / `deny_status` / `sync` / `sync_ms`） |
| `config auto-disarm <0\|1>` / `config deny-status <hex>` | 配置裁决行为 |
| **`config sync <0\|1> [interval_ms]`** | **配置周期同步守卫**（9.303 起并入 config，一次调用即可） |
| `config-fake [0\|1\|2]` / `config-fake-tlb [0..4]` | 假写模式 / TLB 冲刷策略选择器（诊断用） |
| **`config-sync [0\|1] [interval_ms]`** | 同步守卫开关 + 诊断读数（`mode/polls/hits/pages/refused/tsc_cycles`） |
| `clear` | 清空所有目标/页/钩子 |
| `set-auto-start` / `unset-auto-start` / `harden-service` / `unharden-service` | 服务加固 |
| `selftest` / `selftest-abort` / `exit-test` | 自检 / 看门狗测试（武装后脏死）/ 退出测试 |
| `scan-pid <pid> [count]` | 只读枚举目标私有页（VirtualQueryEx） |
| `wpm-write <pid> <hex_va> <hex_val>` | 内核中介写（WriteProcessMemory，C6 靶子） |
| **`wpm-read <pid> <hex_va>`** | **纯读**（无副作用；验证回滚必须用它，`wpm-write` 会扰动被测页） |
| `mmf-open <path>` | 打开文件映射并读回探针 |
| `mmf-loop <path> <sec> [info]` | 目标侧写循环（每 400ms 自写） |
| **`mmf-hold <path> <sec> [info] [size_bytes]`** | **不自写目标**（映射后只睡；多页测试用 `size_bytes`） |
| `mmf-write <path>` | 攻击者侧单写（输出 BLOCKED/LANDED） |

> **`protect <pid>` / `unprotect` / `scan <pid>` 已不存在** —— 早期版本有，现由
> `set-target` + `protect-page` 组合取代。Java 客户端的 `protect <pid>` 仍仅对调用
> 进程自身 PID 生效（ADD_PAGE 绑定调用者 CR3）；**跨进程武装用 PowerShell 的
> `protect-page <pid> <hex_va>`（fn 0x810）**。

### Java/JNI 客户端（`tools\yghv_client`）

```bat
cd YuanGuardHV\tools\yghv_client
build.bat          # 编译（含 JNI native）
run.bat state      # 用法与 PowerShell 客户端一致
```

> Java 客户端只覆盖部分命令（18 个），**新增功能一律以 PowerShell 客户端为准**；
> `command_parity.ps1` 会强制 PowerShell 命令与 README 一致。

---

## 测试

- **静态校验**（构建自动执行）：`tests\run_source_checks.ps1`（源级）+ `tests\run_map_checks.ps1`（链接后）
  - `ioctl_parity.ps1`：C 头文件 ↔ PowerShell/Java 客户端 IOCTL 编号一致（当前 **C=49 / PS=49 / Java=49**）；
  - `command_parity.ps1`：README ↔ 客户端命令一致（当前 **PS=70 / Java=18**）；
  - `safety_checks.ps1`：安全红线静态检查；
  - `irql_contract_check.ps1`：IRQL 可达性与岛内约束；
  - **`exit_rip_contract.ps1`**：38 个 VMEXIT case 的 RIP 契约 ——
    ADVANCE（必须推进）/ INJECT（必须注入）/ HOLD（必须**不**推进）三类断言，
    **附自检**：每次构建都往副本注入一次真缺陷，要求检查器必须抓到。
    > 为什么需要它：`VMEXIT_VMMCALL` 漏写 `Rip = NRip` 很久没人发现，
    > 因为它**没有失败模式** —— 机器正常启停，计数器在动，唯一症状是
    > 「改参数没效果」，而那看起来像「这条指令在这个平台上不做事」。
  - `page_layout_check.ps1`：隐藏表不与 guard 状态共页、岛内读区域页对齐。
- **VMEXIT 验证链**：VMMCALL 心跳、NPT translate/perm/range/split 单测、NPF 注入恢复。
- **实机回归**：物理机加载 → 双核心跳 → 保护测试 → 卸载（不重启）。
- **C 线四步验证**（2026-10，推荐流程）：最小面（不 arm）→ 单页 → 16 页 → 64 页上限。
  脚本样例见 `docs/YUANMOD_HANDOFF_CURRENT.md` 9.304。
- **痕迹自查**：`tools\yghv_stealth_check.ps1`（只读，输出 `[VISIBLE]`/`[CLEAN]` 清单）。
  > 要传 `-ServiceName`：实际部署的服务名是 `yghvpa`，不是 `yuanguard`。
  > 不传的话所有项都会报 `CLEAN` —— 因为它在查一个不存在的服务。
- **调试通道**：`start_kd.ps1` / `run_kd.bat`（VM 串口命名管道连 KD）。
  ⚠️ 单机交互式内核调试**原理上不可行**（内核断点挂起整机），本机 NIC 亦不支持 KDNET ——
  详见 `docs/YUANMOD_HANDOFF_CURRENT.md` 9.296。

---

## 文档索引

| 文档 | 内容 |
|---|---|
| `docs/YUANMOD_HANDOFF_CURRENT.md` | 全程交接与决策记录（最高优先级，**当前至 9.471**） |
| **`docs/RESUME_PROMPT.md`** | **★ 续接入口** —— 基线/雷区/方法论/加速查证 |
| **`docs/KERNEL_API_CHECKLIST.md`** | **★ 写内核代码前必读** —— 三次蓝屏换来的 API 契约核对清单 |
| `docs/YUANMOD_NEXT_WINDOW_PROMPT.md` | 新窗口接手提示词（基线/雷区/候选） |
| `docs/TASKS.md` | 任务清单与 P0 复核结论 |
| `docs/YGHV_OS_AS_GUEST_RESEARCH_20260814.md` | OS-as-guest 研究 |
| `docs/YGHV_OS_AS_GUEST_SUMMARY_20260815.md` | OS-as-guest 阶段性总结 |
| `docs/YGHV_SIMPLEVM_LEVERAGE_20260918.md` | SimpleSvm/HelloAmdHv 构型复审 + C0/C1 实机验证方案（含本机 CPU 权威记录） |
| `docs/YGHV_STEALTH_AUDIT_20260813.md` | 隐藏矩阵与反侦查审计 |
| `docs/YGHV_FULL_REVIEW_20260814.md` | 全面代码审查报告（YGHV-REV-001..043） |
| `docs/YGHV_HOOK_LOCK_AND_0x5AA_REDESIGN_20260813.md` | hook lock 与 0x5AA 重设计 |

---

## 已知限制与路线图

1. **OS-as-guest 已打通（206 线，2026-10）**：早期"平台级硬冻结"结论经 C0 原版 SimpleSvm 实机对照推翻（errata 1363/1235 属 Family 17h 编号系，本机 Zen3 不适用）。当前形态 = vendored 上游 SimpleSvm 逐字进场 + coexist 全核虚拟化，真实 Windows 以 guest 态运行，页写保护全链硬件实证（206-C1–C5 全 PASS，判读史见 `docs/YUANMOD_HANDOFF_CURRENT.md` 9.244–9.274）。历史 APIC 虚拟化 B 路线实验保留在 docs/ 作存档。
2. **★ Zen3 NPT TLB 限制（硬件事实，已绕开）**：NPT（GPA→HPA）翻译**不响应任何已探索的软件冲刷机制** —— `TlbControl=1`（只冲 guest TLB）、`TlbControl=2`（FLUSH ALL，实测无效）、`nCR3` 变更（attempts == consulted == 29,697,622）、`INVLPGA`（打错层，只管 GVA→GPA）、`INVLPGB`（Zen3 无此指令）全部证伪。**处置：换设计** —— 周期同步守卫**根本不翻转 NPT 权限**（页面始终 present+WRITABLE），硬件从不需要失效翻译，限制自然不适用。详见 9.297–9.304。
3. **同步守卫无法区分写入来源（设计取舍）**：目标进程**自己**的合法写入同样被回滚 —— 这是"不依赖 TLB"换来的代价。实测：目标持续自写仍被钉在单一值（70% 采样同值）。
4. **轮询间隔下限受系统定时器粒度约束**：实测 `NtQueryTimerResolution` 为 `min=15.62ms`，请求 1ms 实际得到 ~9.5–15.6ms，**实用下限约 16ms**。
5. **WPM 内核中介写旁路**：`WriteProcessMemory` 类内核 API 经 `MmCopyVirtualMemory`+`KeStackAttachProcess` 以"目标 CR3 + cpl=0"完成写，现行裁决（is_target_cr3 ‖ cpl==0 → ALLOW）无法区分它与内核合法写——已在实机定性实证（run_c17）。**此项不再是主线阻塞**（C 线走的是同步守卫路线）。
6. **DENY 的 AV 报告地址为 VA 0**：精确 GVA 重建在实测平台不可实现（`MmGetVirtualForPhysical` 选择性失效 + 直接映射基址不可无故障验证，206c5g/h 两轮蓝屏学费已记录并回退）。DENY 语义本身无损（确定性递 AV、零风暴），CR2=0/ec P=0 为最终行为。
7. **隐形是尽力而为**：内核驱动在真实 Windows 中加载，绝对隐形不现实；`loader_stealth` 默认关闭且未经 kd `!driver` 复核；不承诺绕过任何具体反作弊产品（ACE 仅尽力优化，不作验收标准）。
   - **7.1 自我隐藏的边界（实测）**：`.pdata` 段可藏（guest 读零、host 可读）；
     `.text` 与 `.rdata` **不可藏** —— 前者是取指页、后者含 IAT（实测崩溃
     `0x3B C0000005, rip=0`，指令本身正确但目标被自己藏掉了）。`.data` 未试。
8. **常驻虚拟化的 CPU 开销无法在本机消除（硬件约束）**：12 个核持续在 guest 里运行，
   实测 **基线 29% → 加载后 100%**。已把 VMEXIT 从 1418 万/秒压到 **738/秒**（降 19,200 倍），
   但**那降的是往返次数，不是核的工作量**。
   - `hlt` 在本机**不能停住核**：`powercfg /availablesleepstates` 显示固件不提供
     S1/S2（只有系统级 S3），无 C-state 可进，`hlt` 执行后立即返回。
   - 真正的解法只有两条：① 不做常驻虚拟化（架构改动，且 NPT 保护会从常驻变成窗口式）；
     ② 换一台有 C-state 的机器 —— 桩已是 `hlt + pause` 形态，到那时无需改代码。
9. **单机交互式内核调试不可行**：内核断点 = 挂起整个 OS（含调试器自身），且本机 NIC 不支持 KDNET。**唯一路径 = 第二台物理机**（详见 9.296）。
10. **仓库卫生**：`reference/` 参考实现、历史日志归档等清理项未完成。
11. **路线图**：真实产品目标（Minecraft/Forge）目标选择/页选取流程化 → R1 私有内存剔除 → `loader_stealth` 并入默认（先 kd 复核）→ MSR/IO/处理器层隐藏（需裸机/KVM）→ 产品化整合。
    - **7.1 收尾后**：段隐藏已完成，剩 `.data`（53 页，含活状态，风险最高）；
      建议先做「PA → 所属段」查询再动它，否则藏错子区间会把自己弄坏。

---

## 合规与法律声明

- 本项目为安全研究与学习用途，仅限在**你拥有或获授权**的机器上使用。
- 驱动加载需管理员权限与测试签名；内核级虚拟化若配置不当可能导致系统崩溃或数据丢失，风险自负。
- 与反作弊系统对抗可能违反游戏/平台服务条款。使用者须自行确认用途合规，作者不对滥用造成的封号、法律后果等负责。
- 本仓库采用 **MIT License**（见根目录 `LICENSE`）。
