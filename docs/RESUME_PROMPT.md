# YuanGuardHV 新窗口恢复提示词

> 生成时间: 2026-10-18 19:50
> 仓库根: `D:\yuanguard` | 分支: `main` | 代码 HEAD: `fb32cbf`（其上一条是本恢复提示词的 docs 提交）
> 本文件用于在新会话中无缝接管。**先读完本文件再动手。**

---

## 0. 一句话现状

AMD-V SVM/NPT 隐形 Hypervisor 已完成 **Tier 7 全功能（13/13 PASS）**，
`g_bp`/`g_frame`/`g_view_pages` 三张表**可以隐藏**（含热路径在隐藏下存活），
隐藏路径 10 分钟 soak **STABLE**（78 轮 / 0 hang / 0 失败 / probes 0→9）。
当前镜像 `206zA_why`（md5 `86DF8BBE`）。**30 个提交未推送。**

---

## 1. 用户铁律（必须遵守，违反即失败）

| # | 铁律 |
|---|---|
| 1 | **改变机器状态的命令必须先获明确确认**（驱动加载/停止、`sc`、压测、重启）。只读操作可直接执行。 |
| 2 | **只修用户报告过的问题**，不擅自扩大修复范围。 |
| 3 | **每个修改与决策写入 `docs/YUANMOD_HANDOFF_CURRENT.md`**，编号 9.3xx。 |
| 4 | 构建 + BSOD 取证可直接做，无需确认。 |
| 5 | **禁止 `python -c` 编辑 .h/.c** —— 用 edit 工具。 |
| 6 | **未经明确指示不得推 GitHub。** |

用户偏好：「自主规划解决」——能自主判断的可逆步骤直接做，别逐步问。

---

## 2. 环境

```
机器       Ryzen 5 5500 (Zen3 Cezanne, Family 19h Model 50h), 12 逻辑核
           Win10 Pro Workstation 19045, 16 GB, 裸机
           testsigning=Yes, nointegritychecks=Yes, debug=No, SecureBoot=off
           HypervisorPresent=False, IOMMU 不在 ACPI, 无 TPM
交互式 kd  不可用；本地符号 kd 可用
```

**构建**：
```bat
set YGHV_BAREMETAL_STEP=206
set YGHV_206B_COEXIST=1
set YGHV_206B_GNPT=1
cd /d D:\yuanguard\YuanGuardHV && build.bat
```

> **⚠ 每次构建 md5 都会变 —— 这不是回归。**
> `build.bat` 每次重新签名，PE 头里的 `TimeDateStamp`（0xE8）、`CheckSum`（0x138）、
> debug directory、证书表指针、PKCS#7 签名都会变，共约 323 字节，**全部在元数据区**。
> 实测：同一份代码两次构建，**代码段 0x400..0x24E54 差异字节 = 0**。
> 比对是否同一份代码要**比 .map** 或**比代码段**，不要比文件 md5。
> `206zA_why` 归档的 `86DF8BBE` 是那一次签名的产物，重建会得到不同的值。

编译器 `clang-cl`，`CFLAGS=/nologo /O2 /kernel /GR- /EHs-c- /Zl /GS-`
另有 `-Werror=return-type -Werror=implicit-function-declaration`（**不是全部警告**，内核内建产生 208 个）。

**build.bat 执行顺序（重要）**：
```
1. run_source_checks.ps1   编译前（只读源码）
2. 编译 + 链接
3. run_map_checks.ps1      链接后（读 .map）
4. 签名
```
> 9.358 修的结构性缺陷：map 级检查原来在编译**之前**跑，一直在验证**上一次构建**。

---

## 3. 当前验证矩阵（镜像 `206zA_why` / 86DF8BBE）

```
G25  Tier 7 全功能          13/13 ALL PASS
G26  bp-after-hide           3/3  PASS
G29  arm→hide→trigger        PASS (hid 3, hide_fail=0, drain saw NPF)
G28  隐藏路径 10 分钟 soak   STABLE (78 轮, 0 hang, 0 失败, probes 0→9)
静态检查                     6 个 PASS
```

| 测试 | 脚本 | 用途 |
|---|---|---|
| G25 | `D:\aaaaaavm\g25_tier7.ps1` | Tier 7 全功能 13 项 |
| G26 | `D:\aaaaaavm\g26_bphang.ps1` | bp 在 hidecore 前后 |
| G28 | `D:\aaaaaavm\g28_hidesoak.ps1` | 隐藏路径 10 分钟 soak（`-Minutes N`）|
| G29 | `D:\aaaaaavm\g29_border.ps1` | **决定性**：arm→hide→trigger |

**跑测试前必做**（否则会被旧驱动污染，曾导致假失败）：
```powershell
Get-Process powershell,pwsh | Where-Object { $_.Id -ne $PID } | ForEach-Object {
  $c=(Get-CimInstance Win32_Process -Filter "ProcessId=$($_.Id)" -EA SilentlyContinue).CommandLine
  if($c -match 'mmf-hold|g2[0-9]_|yghvg'){ Stop-Process -Id $_.Id -Force -EA SilentlyContinue }
}
foreach($n in @('yghvg28','yghvg29','yghvg26','yghvg25','yghvgt')){
  & sc.exe stop $n 2>&1 | Out-Null; Start-Sleep -Seconds 2
  & sc.exe delete $n 2>&1 | Out-Null
}
Get-ChildItem 'C:\' -Filter 'yghv*' | ForEach-Object { Remove-Item $_.FullName -Force -EA SilentlyContinue }
```

---

## 4. 核心架构约束（违反会崩溃或挂起）

### 4.1 OS-as-guest 决定什么能隐藏

只有跑在 **VMRUN 循环 / VMEXIT 处理器**里的代码在 **HOST 模式**（不查 NPT）。
普通驱动线程（IOCTL、系统 worker）在 **GUEST 模式** → guest VA → guest 页表 → GPA → NPT → SPA。

> **隐藏一页 = 从 NPT 移除 = 从 guest 模式的驱动线程也移除。**

### 4.2 两种隐藏机制，不可互换

| 机制 | 值 | 行为 | 适用 |
|---|---|---|---|
| `YGHV_HIDE_UNMAP` | 0 | 清 PRESENT；我方下次访问 fault，ALLOW 循环服务**真实帧** | **我们自己的表** |
| `YGHV_HIDE_DECOY` | 1 | 叶子指向诱饵帧；页仍 present，读返回**零** | **外来页** |
| `YGHV_HIDE_AUTO` | 2 | 由 `yghv_guard_mode_for()` 决定 | 内部调用唯一该传的值 |

**机制不匹配 ⇒ 页变成零页 ⇒ 表读成空、写被丢弃、功能静默失效**（9.350）。

### 4.3 地址类型纪律（三次栽在同一处）

- `mem-write` 收 **VA**
- `pa-io` 收 **PA**
- **来自 `MmGetPhysicalAddress` 的是物理的；`&something` 永远不是。**
- 两侧都是 `uint64_t`，**类型系统救不了** —— 这是命名规则。
- 每个比较点必须**显式翻译一次并命名哪一侧是哪一侧**。
- per-VMEXIT 路径上 VA 侧常量**解析一次并缓存**。

### 4.4 岛内约束（`protect.c` 9.249）

`IRQL = DISPATCH`、`GIF=0`、**禁止阻塞**：
- 无锁、无 LOG/trace、无分配
- 门控函数（`KeAreAllApcsDisabled()`）在此返回 **看似合理的 0**，无法与"无事可做"区分
- 岛内安全版本命名 `*_bare`

### 4.5 返回 0 无法区分"拒绝"与"无事可做"

必须用计数器交叉验证。`hide_fail` 位掩码（9.365）：
```
0x10 早退（ALLOW 循环未武装，正确的拒绝）  1 split 失败  2 perm 失败
3 页是guard状态  4 无guard条目  5 已隐藏  0x20 循环未跑
```
`hash_fail` 现在承载 `state_why` 位掩码：
```
0x01 跨度  0x02 code_start  0x04 code_size  0x08 decoy_pa
0x10 decoy_va  0x20 npt  0x40 protect
```

---

## 5. 关键文件

| 文件 | 说明 |
|---|---|
| `YuanGuardHV\hv\yghv_guard.c` (1477 行) | Tier 7.1/7.3；`state_page`/`target_page`/`do_hide`/`hide_core`/`arm_core` |
| `YuanGuardHV\hv\mem_bp.c` | 断点；**热/冷分离**（`g_bp_region`/`g_hot`/`g_ring_region`）|
| `YuanGuardHV\hv\mem_frame.c` | **页精确** `g_frame_region` |
| `YuanGuardHV\hv\mem_view.c` | **页精确** `g_view_region` |
| `YuanGuardHV\hv\mem_engine.c` | **具名初始化器**（`.read = ` / `.write = `）|
| `YuanGuardHV\hv\npt_core.c` | `npt_split_2mb_to_4kb` / `npt_set_page_perm` / `npt_exclude_pa` |
| `YuanGuardHV\hv\protect.c` | `yghv_protect_page_is_npt()` (PA-correct)；2MB→4KB 分裂 |
| `YuanGuardHV\hv\svm_simplevm206.cpp` (3377 行) | **自有 include 方案**；guard 分支 L1389；#DB re-hide L1941/1945 |
| `YuanGuardHV\hv\common\control_ioctl.h` | IOCTL 0x800–0x828 |
| `YuanGuardHV\tools\yghv_ctl.ps1` | PS 命令 55 |
| `YuanGuardHV\tools\yghv_client\YghvCtl.java` | Java FN_ 18 |

**静态检查**（`YuanGuardHV\tests\`）：
```
run_source_checks.ps1 → ioctl_parity / command_parity / safety_checks / irql_contract_check
run_map_checks.ps1    → page_layout_check
```

**接口计数**（build 报告）：`C=41 PowerShell=41 Java=41`（IOCTL），`PowerShell=55 Java=18`（命令）

---

## 6. 已完成的方法论（这些是资产，别重犯）

### 6.1 一个正确的修复会暴露出寄生缺陷
9.361 收紧 `state_page` 跨度（本身正确）→ 撤掉意外覆盖 → 露出 `target_page`/`state_page`
的 VA/PA 混淆。**当改动让东西不再工作，先问：是改动弄坏了它，还是揭开了它。**

### 6.2 页精确 ≠ 页对齐
`DECLSPEC_ALIGN(4096)` **只对齐起点不规定尺寸**。4 字节计数器留 4092 字节空隙，
链接器拿冷符号填满。修法唯一可靠：**union + 页大小字节数组 + 编译期断言**
```c
typedef union { T entries[N]; uint8_t page[4096]; } region_t;
DECLSPEC_ALIGN(4096) static region_t g_region;
typedef char assert_exact[(sizeof(region_t) == 4096) ? 1 : -1];
```

### 6.3 断言最有用的时候，是它抓出"用来修设计错误的代码"里的错误
第一次写 hot/cold 断言：把标量和命中环放一个 struct → 构建失败（环 6144 字节 = 一页半）。

### 6.4 检查必须读它判定的那个产物
- 静态检查曾跑在编译**之前**，验证上一次构建（9.358）
- 检查的**提取正则**也曾与代码格式脱节（报 `none parsed` 而代码在拒绝七页）

### 6.5 位置式 ≠ 具名
位置式初始化器把成员语义和位置绑死，重排字段静默交换 `read`/`write`。
**改具名，验证方式是"字段对调后仍精确指名"。**

### 6.6 ★ 测试断言了自己的假设（**四次记录在案**）
1. soak 偏移越界 → `STATUS_INVALID_PARAMETER`（把测试的错当成驱动缺陷）
2. 负向测试注入到 `hide_core` —— 但它**不在岛内**，正确的检查看起来失灵
3. T12 无条件要求 `add OK`，而正确行为是 `REFUSED`（注册表已隐藏）
4. soak 无条件要求 `bp add OK`，同上 —— 报 14 failures / FAILED，
   而实际 hidden 3/3、probes 前进、零 hang

**共同点**：断言写的是"我以为会发生的"，而不是"契约允许的"。

**修法模板**：接受两种合法结果，各自要求正确的后续。
```
已武装   → 触发必须落地 且 drain 必须看到 NPF
被拒绝   → 触发仍必须落地（没武装就没东西会空转）
```

> 写负向测试前先确认：**注入点确实在检查的作用域内**。
> 写正向断言前先问：**这个操作在当前状态下有几种合法结局**。

### 6.7 让"什么都没做"说出是哪扇门
`hid 0` 有三种成因，第一版报成一样，排查两次奔向 NPT 才想到早退。
**一个能说出"走了哪扇门"的计数器，价值高于三次重建加载。**

### 6.8 写错的机制比不写更糟
9.355 断言"热路径付嵌套 VMEXIT"，被 `probes=0` 推翻。
**当注释里的机制被计数器反驳，改注释为实测事实并标注未确认假设。**

---

## 7. PowerShell 陷阱（我踩过，把文件截断过两次）

- **`@()` 是固定大小数组，`.Add()` 会失败** → 用 `New-Object 'System.Collections.Generic.List[string]'` + `AddRange`，或**直接用 edit 工具**
- `Write-Host ("...{0}..." -f x)` 里含 `->` 或 `{}` 会被当脚本块解析 → 用变量拼接
- 单引号字符串里的 `'`（如 `table's`）会提前终止字符串 → 改双引号 + `''`
- `-match 'KeAreAllApcsDisabled\(\)'` 在特定上下文会被当泛型 token → 用 `[(]`
- 批处理 `.bat` **必须纯 ASCII**（em dash 在 CJK 代码页下吞字符，曾让 `%WDK_ROOT%` 变空）
- 每个 `powershell -File` 是**新进程** → 扫描+遍历必须在同一次调用内完成
- `Start-Job` 子进程随父进程死；用**有界 `Start-Job` + `Wait-Job -Timeout`** 检测挂起

---

## 8. 剩余未做项（诚实清单）

1. **页精确未覆盖所有模块** —— 只有 3 张被隐藏表 + `mem_bp` 两个热区域。
   其余"小对象留大空隙"靠 `page_layout_check` 事后从 `.map` 发现，不是编译期。
2. **规则 7 的位置式回退路径仍在** —— 位置式表被显式报告，但未强制改具名。
3. **soak 是单核重心** —— 未覆盖多核并发隐藏访问，未覆盖 30 分钟以上。
4. **Tier 7.4/7.5/7.6 wiring 仍 inert** —— `yghv_sentry_escape`/`yghv_sentry_surface_add`
   从未被调用 ⇒ `escapes=0`、`surface: 0 entry(s)`。需在 MSR/IOIO/CR 拒绝路径、
   `yghv_guard_is_hidden` 命中处、加载时与 IOCTL 分发器里加调用点。
5. **T7 decoy 测试弱** —— 只有 `(t7 -match 'value:')`，非守卫路径也通过。
   需要一个真正验证"guest 读隐藏的外来页返回零"的测试。
6. **Tier 5（9 项）、Tier 6（5 项）、Tier 8-9** —— 已规划，未开始。
7. **30 个提交未推送**（最后一次推送是 `535f74d`）。
8. **未做长时 soak**（30 分钟以上、多核并发）。

---

## 9. 下一步建议（按价值排序）

| 优先级 | 任务 | 理由 |
|---|---|---|
| **高** | Tier 7.4/7.5/7.6 wiring | 功能存在但从不被调用，`escapes=0` 是**假指标** |
| **高** | 强化 T7 decoy 测试 | 当前测试在非守卫路径也通过，无法证明功能 |
| 中 | 页精确推广到其余热点模块 | 防止下一次"隐藏连带隐藏邻居" |
| 中 | 多核并发隐藏 soak | 当前 soak 单核重心 |
| 低 | 位置式表强制改具名 | 已有显式报告，风险可控 |

---

## 10. 新窗口第一件事

```powershell
cd D:\yuanguard
git log --oneline -3            # 最新两条应含 docs: 恢复提示词
git status --porcelain          # 应为空
cd D:\yuanguard\YuanGuardHV
$env:YGHV_BAREMETAL_STEP=206; $env:YGHV_206B_COEXIST=1; $env:YGHV_206B_GNPT=1
.\build.bat                      # 应 6 个 PASS + Build SUCCESS
```
确认构建成功（6 个 PASS + Build SUCCESS）。
**不要用 md5 校验** —— 每次构建都会因重新签名而变（见第 2 节的说明）。
若要确认代码未变，比 `.map` 或比代码段 0x400..0x24E54。

然后读：
1. `docs/KERNEL_API_CHECKLIST.md` §27（VA/PA 三处）与 §28（页精确/具名）—— **最易重犯**
2. `docs/YUANMOD_HANDOFF_CURRENT.md` 9.364 / 9.365（最近两章）
3. 本文件第 4 节（架构约束）

**动手前先问用户要做什么。不要自动开始剩余项。**

---

## 11. 累积已验证历史（全部在实机跑过，零崩溃）

```
Phase 1                 6/6
Phase 2-5 W1-W8        10/10
Phase 7  C1-C6          7/7
Phase 4b D1-D6          6/6
Phase 6  E1-E6          6/6
Tier 4.1                9/9
Tier 4.3                9/9
Tier 4.2                8/8
Tier 4.4                7/7    （三机制：NPF/INT3/DR）
Tier 4.5                7/7    （见 9.2xx）
Tier 4.6                4/4    （见 9.2xx）
Tier 4 合计            24/24   （4.4 三机制 + 4.5 + 4.6）
Tier 7.2 / 7.7          8/8    （故障隔离 + 优雅降级）
Tier 7.1 / 7.3          9/9    （自隐藏 + 完整性自检）
Tier 7 全功能          13/13   ★ 当前
SOAK(旧)               10.2 min STABLE
```

**近期镜像归档**（`D:\aaaaaavm\`，按时间）：

| 镜像 | md5 | 说明 |
|---|---|---|
| `206y6_nohot` | 585A71AE | Tier 7 13/13 首通（g_bp 排除） |
| `206y9_guarded` | F797728B | g_bp 护栏 + 断言 |
| `206z1_final` | F93496F2 | 三项收口 |
| `206z3_bphidden` | 8424FB4D | g_bp 可隐藏 |
| `206z4_assert` | 6C408DD2 | hot/cold 页精确 |
| `206z6_vafix` | FB8DD42E | VA/PA 首次修复 |
| `206z8_vafull` | CEF35E62 | 三处 VA/PA 全修 |
| `206z9_pageexact` | AA3F774F | frame/view 页精确 |
| **`206zA_why`** | **86DF8BBE** | **当前 HEAD，含 state_why 诊断** |

共 130+ 个历史镜像可回溯（`206a` … `206zA`）。

---

## 12. 目录结构速查

```
D:\yuanguard\
├─ docs\
│  ├─ YUANMOD_HANDOFF_CURRENT.md   9337 行，章节到 9.365  ← 权威交接记录
│  ├─ KERNEL_API_CHECKLIST.md       873 行，28 章         ← 内核 API 红线
│  └─ RESUME_PROMPT.md                                    ← 本文件
├─ YuanGuardHV\
│  ├─ hv\           驱动源码（.c/.cpp/.h）
│  ├─ hv\common\    共享头（control_ioctl.h / mem_bp.h / yghv_guard.h ...）
│  ├─ tests\        6 个静态检查 .ps1
│  ├─ tools\        yghv_ctl.ps1 (55 命令) + yghv_client (Java 18)
│  ├─ bin\          构建产物 yuanguard_hv.sys + .map
│  └─ build.bat     clang-cl + link
└─

D:\aaaaaavm\                       测试脚本与镜像归档
├─ g20..g29_*.ps1                测试脚本（g25/g26/g28/g29 是当前主力）
├─ yuanguard_hv_step206*.sys     130+ 历史镜像
└─ doc_93xx.md                   章节草稿（已并入 HANDOFF）
```