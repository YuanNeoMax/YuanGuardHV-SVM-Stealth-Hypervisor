# YuanGuardHV 新窗口恢复提示词

> 更新时间: 2026-10-18 23:55（9.366 四项收口后）
> 仓库根: `D:\yuanguard` | 分支: `main`
> 用 `git rev-list --count @{u}..HEAD` 查当前领先提交数，`git log --oneline -3` 查 HEAD —— 不要写死。
> 本文件用于在新会话中无缝接管。**先读完本文件再动手。**

---

## 0. 一句话现状

AMD-V SVM/NPT 隐形 Hypervisor 已完成 **Tier 7 全功能（13/13 PASS）**，
`g_bp`/`g_frame`/`g_view_pages` 三张表**可以隐藏**（含热路径在隐藏下存活），
隐藏路径 10 分钟 soak **STABLE**。

9.366 之后：**Tier 7.4/7.5/7.6 的 wiring 已接通**（escape 与 attack-surface 不再恒为 0）、
**诱饵语义首次被真正验证**（`mmf-read`：guest cpl=3 读隐藏页得 `0x0`）、
**10 个模块状态区页精确化**（新增 `page_layout_check` 规则 4）、
**位置式指针表改为构建失败**。当前镜像 `206zB_pageexact`（md5 每次构建都变，**比 .map**）。

> **所有者的 1 分钟多核 soak 是 DECOY 路径的**（`probes=0` 是机制正确，不是没跑）。
> 详见 9.366.7 —— 不要把那条判词读成"ALLOW 循环在 4 核下存活"。

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

## 3. 当前验证矩阵（镜像 `206zB_pageexact`）

```
G25  Tier 7 全功能          13/13 ALL PASS   ← T7/T9/T10 已重写
G26  bp-after-hide           3/3  PASS
G29  arm→hide→trigger        PASS (hid 3, hide_fail=0, drain saw NPF)
G30  诱饵语义 + 7.4/7.6 接线 ALL PASS        ← 新增，决定性
G31  hidecore 归因           hid 3 / hide_fail=0    ← 新增，诊断
G32  多核隐藏 soak 1 分钟    STABLE (4 worker, 1209 写, hidden 11/11)
G28  隐藏路径 10 分钟 soak   STABLE（9.365 的结论，本轮未复跑）
静态检查                     7 个 PASS
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

必须用计数器交叉验证。`hide_fail` 位掩码：
```
0x10 早退（ALLOW 循环未武装，正确的拒绝）  1 split 失败  2 perm 失败
3 页是guard状态  4 无guard条目  5 已隐藏  0x20 循环未跑
```
`hash_fail` 承载 `state_why` 位掩码：
```
0x01 某个具名 guard 对象占这一页（9.366 起，不再是"在某两个地址之间"）
0x02 code_start  0x04 code_size  0x08 decoy_pa
0x10 decoy_va  0x20 npt  0x40 protect
```
**要单页归因，不要重建加载**：`sys-guard why <hex_pa>`（IOCTL 0x826 op 7）一次问清
三套判定，只读、不做隐藏。9.366 靠它一次定位"frame-table 页被拒"。

### 4.6 ★ 页归属判定必须是**对象清单**，不能是数值区间

`yghv_guard_state_page()` 曾经用 `[g_guard 的物理地址, g_guard_count 的物理地址]`
这个**数值区间**表示"guard 自己的状态"。链接器排 RVA，内存管理器排物理页 ——
`.map` 上只占两页的区间，运行时能覆盖一大片无关的活分配。

实测（9.366）：frame-table 的物理页恰好排进区间 → hidecore 只隐藏 2/3，
`hide_fail=3` + `hash_fail=1`，而 bp/view 两页在区间外、正常隐藏。

**现在的形式**：每个对象 `yghv_guard_state_add(&X, sizeof(X))` 登记一次，
判定是"有没有已登记的对象占这一页"。新增状态必须登记，否则它可被隐藏。

### 4.7 ★ 岛内读取的状态区必须页精确，且不与他模块共页

不是"隐藏目标"才需要页精确。VMEXIT 处理器每次故障**读**的那些模块状态
（watch 环、view 选择、fail 计数、sentry 计数、protect 表）同样需要：
一个页上有两套机制的热状态，隐藏它就会连带移除"决定怎么处理故障"的东西。

`page_layout_check` **规则 4** 检查这条（按 `.map` 的 obj 文件判定跨模块）。

### 4.8 ★ 两种隐藏机制产生**两种可观测结果**，测试必须按机制判定

外来页由 `yghv_guard_mode_for()` 按**归属**分类，只有 guard 状态与三张表是
"我们的"，其余一律 **DECOY**。

```
DECOY  叶子仍 present，指向零页  → guest 读 0x0，写入被丢弃，**不发 #NPF**
                                  → probes / escapes 都不会动（正确行为）
UNMAP  清 present                → 每次访问 #NPF → 处理器运行 → probes 增长
```

**把"必须增长"写成断言，等于断言本次走了 UNMAP。** 9.366 在这上面栽了两次
（G30 的 probes、G25 的 T9）。修法：先判机制，再要求与该机制相符的后续。

---

## 5. 关键文件

| 文件 | 说明 |
|---|---|
| `YuanGuardHV\hv\yghv_guard.c` | Tier 7.1/7.3；`state_page`/`state_why`/`state_add`/`target_page`/`do_hide`/`hide_core`/`arm_core` |
| `YuanGuardHV\hv\mem_bp.c` | 断点；**热/冷分离**（`g_bp_region`/`g_hot`/`g_ring_region`）|
| `YuanGuardHV\hv\mem_frame.c` | **页精确** `g_frame_region` |
| `YuanGuardHV\hv\mem_view.c` | **页精确** `g_view_region` + `g_view_hot` |
| `YuanGuardHV\hv\mem_watch.c` | **页精确** `g_watch_region`/`g_watch_hot`/`g_watch_ring` |
| `YuanGuardHV\hv\yghv_sentry.c` | 7.4/7.5/7.6；**页精确** `g_sentry_hot`/`g_sentry_surface`；`surface_arm_intercepts` |
| `YuanGuardHV\hv\yghv_fail.c` | 7.2/7.7；**页精确** `g_fail`（环索引是 struct 字段，不是旁边的变量）|
| `YuanGuardHV\hv\mem_time.c` / `mem_cache.c` | **页精确** `g_time_region` / `g_cache_region` |
| `YuanGuardHV\hv\mem_engine.c` | **具名初始化器**（`.read = ` / `.write = `）|
| `YuanGuardHV\hv\npt_core.c` | `npt_split_2mb_to_4kb` / `npt_set_page_perm` / `npt_exclude_pa` |
| `YuanGuardHV\hv\protect.c` | `yghv_protect_page_is_npt()` (PA-correct)；2MB→4KB 分裂 |
| `YuanGuardHV\hv\svm_simplevm206.cpp` (3377 行) | **自有 include 方案**；guard 分支 L1389；#DB re-hide L1941/1945 |
| `YuanGuardHV\hv\common\control_ioctl.h` | IOCTL 0x800–0x828 |
| `YuanGuardHV\tools\yghv_ctl.ps1` | PS 命令 56（9.366 新增 `mmf-read`、`sys-guard why`）|
| `YuanGuardHV\tools\yghv_client\YghvCtl.java` | Java FN_ 18 |

**静态检查**（`YuanGuardHV\tests\`）：
```
run_source_checks.ps1 → ioctl_parity / command_parity / safety_checks / irql_contract_check
run_map_checks.ps1    → page_layout_check
```
- `irql_contract_check`：岛内可达集 + 指针表目标 + **位置式表 = 构建失败**（9.366）
- `page_layout_check`：隐藏表页独占 + 页对齐 + **规则 4：岛内读取区域页对齐且不跨模块共页**
  —— 解析到 0 个拒绝符号时**直接 throw**，否则空清单会让每一页都显得合法

**接口计数**（build 报告）：`C=41 PowerShell=41 Java=41`（IOCTL），`PowerShell=56 Java=18`（命令）

**9.366 新增的两个原语**：
- `sys-guard why <hex_pa>` —— 只读归因（IOCTL `0x826` op 7），一次问清 `state_why` /
  `target_page` / guard slot / hidden / hide_mode。定位"哪个表的页被拒"靠它，不靠重建加载。
- `mmf-read <path>` —— guest 侧读（`mmf-write` 的另一半）。判诱饵必须用它：`mem-read`
  走驱动、由 ALLOW 循环服务，两种机制下都返回一个 value 字符串。

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

### 6.6 ★ 测试断言了自己的假设（**七次记录在案**）
1. soak 偏移越界 → `STATUS_INVALID_PARAMETER`（把测试的错当成驱动缺陷）
2. 负向测试注入到 `hide_core` —— 但它**不在岛内**，正确的检查看起来失灵
3. T12 无条件要求 `add OK`，而正确行为是 `REFUSED`（注册表已隐藏）
4. soak 无条件要求 `bp add OK`，同上 —— 报 14 failures / FAILED，
   而实际 hidden 3/3、probes 前进、零 hang
5. G30 的诱饵检查调 `hidecore`（隐藏我们自己的三张表）然后读 guest 的 section ——
   **没有任何机制被要求隐藏那一页**，于是读到种子值、报 NOT HIDDEN（9.366）
6. G30 无条件要求 `probes` 增长，而 DECOY 模式**不产生 fault** —— 不增长是机制正确
7. G25 的 T9 无条件要求 `escapes>0`，而它在 T7 之前跑，那个位置的正确输出就是 0

**共同点**：断言写的是"我以为会发生的"，而不是"契约允许的"。

**修法模板**：接受两种合法结果，各自要求正确的后续。
```
已武装   → 触发必须落地 且 drain 必须看到 NPF
被拒绝   → 触发仍必须落地（没武装就没东西会空转）
```

> 写负向测试前先确认：**注入点确实在检查的作用域内**。
> 写正向断言前先问：**这个操作在当前状态下有几种合法结局**。
> **问机制前先确定本次走的是哪条机制** —— 一个只对 UNMAP 成立的断言，
> 在 DECOY 下报的是测试的错。
>
> ★ **别把机制名和 PASS/FAIL 存在同一个槽里**。PowerShell 里
> `$true -eq 'UNMAP (hole)'` 是 **TRUE**（字符串被转成布尔），
> 于是判词说 DECOY、下游逻辑无条件走 UNMAP。9.366 实测。

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
- ★ **.ps1 必须带 UTF-8 BOM 或纯 ASCII**。PowerShell 5.1 对无 BOM 的 .ps1 按 GBK
  解码，里面的 em dash 会让解析器在**完全无关的行**上报"缺少参数"。9.366 在这上面
  浪费两轮，症状（报错行号离真因很远）极具误导性
- ★ **每条命令都新开 powershell 进程的 soak 不是 soak**。g32 第一版 1 分钟只跑到
  2 轮 —— 6 次进程创建。把写动作放进 worker 自己的进程里，故障才真的并发
- 诊断脚本也要**卸载驱动**。g26/g29 跑完不卸载，调用方等一个永不退出的进程，
  下一个测试对着一个没人停的驱动跑（正是 runbook 警告的旧驱动污染）

---

## 8. 剩余未做项（诚实清单）

> 9.366 收口了原来的 1-5 项。9.367 又更正了三份 Tier 规划的事实前提，见下。

**★ 规划文件的前提错误（9.367 已更正并加注）**

`MEM_ENGINE_PLAN_TIER5/6/8_9.md` 开篇都写「现成 VMEXIT 已实现 35 种，扩展即可」。
实测：拦截位只开了 5 组（CPUID / MSR / CR3+CR4 / #DB / SHUTDOWN / VMRUN / NPF），
`SvHandleVmExit` 只有 **9 个 case**。35 是 AMD 的退出码数量，不是本实现的数量。
三份文件顶部已加 `9.367 CORRECTION` 注记 —— **按风险排序前先读 9.367.1**。

**★ 结构限制**：本项目的 VMCB 结构体在 `0x014` 处直接是
`Reserved1[0x03c-0x014]`，**没有 `InterceptMisc3`**。所以位 41/42/43/45
（WBINVD / MONITOR / MWAIT / XSETBV）做不了，除非先加字段。
位 14（RDTSC）/ 23-25（PAUSE/HLT/INVLPG）在 Misc1，位 32/33/39 在 Misc2，都可以。

**★ INT3 模式没有热路径证据**：`InterceptException` 只设了 bit 1（#DB），
#BP 是 bit 3 **没设**。`case VMEXIT_EXCEPTION_BP` 那条链
（#BP → 恢复原字节 → TF → #DB → 重写 0xCC）只会在 guest 自己执行 INT3 时到达，
那时 find 返回 0、走反射。g18 的 B1-B3 验证的是**写字节/恢复字节**，
靶页注释自己写着「never executed」—— 0xCC 从未触发。**热路径至今零证据。**
（这是第七次「断言测的是 A，被读成 B」。）

1. **没有多核 UNMAP soak** —— `g32` 只覆盖 DECOY。ALLOW 循环的并发行为仍只有
   G29/G26 的单次决定性验证，缺 30 分钟尺度。
2. **INT3 热路径未验证** —— 见上。补证需要开 `InterceptException bit 3`，
   并先确认 #BP 反射路径不误伤 guest 自己的 INT3（历史：#GP 拦截曾导致 0xE2）。
3. **`g_protect` 只做了页对齐，没做页精确** —— union 化会改变 `extern` 声明形状。
4. **`surface_arm_intercepts` 的清单是手写的** —— 9 个名字需手工同步；
   从掩码反推要 sentry 认识 VMCB 形状，这个耦合是有意保留的。
5. **位置式的宏构造形式绕得过检查** —— `#define TABLE(x) { ... }` 检查器看不见。
6. **`escapes` 只在真正拒绝时才非零** —— 判断接线是否活着看 `sys-sentry surface`
   的条目，不是 escapes。
7. **Tier 5（9 项）/ Tier 6（5 项）/ Tier 8（5 项）/ Tier 9（5 项）** —— 全部未开始；
   但 Tier 5.1 的 TSC 平移部分**零拦截成本可做**（`VMCB.TscOffset` 字段已在）。
8. **本地提交未推送** —— 9.366 = `3d6a784`；用 `git rev-list --count @{u}..HEAD` 查领先数。

---

## 9. 下一步建议（按价值排序）

> 排序原则：**先纠正前提 → 再补验证尺度 → 再补"已被误认为做过"的 → 最后才是新能力**。
> 一个错了的验证会在后续每一项上重复付代价；新能力建在错的前提上会连着错。

| 优先级 | 任务 | 理由 | 风险 |
|---|---|---|---|
| **已完成** | 更正三份 Tier 规划的前提 | 9.367；纯文档 | 无 |
| **高** | 多核 UNMAP soak（30 分钟）| 唯一还缺的**尺度** | 低 |
| **高** | INT3 热路径补证（开 #BP 位 + 可执行靶页）| 现在的验证只覆盖写字节 | **中** |
| 中 | Tier 5.1 TSC 平移（`TscOffset`）| 零拦截成本、字段已在 | 低 |
| 中 | Tier 5.1 非线性伪造（RDTSC + RDTSCP 拦截）| 两个拦截位都在已声明字段里 | 中 |
| 中 | Tier 5.4 硬件视图伪造（CPUID 扩展）| CPUID 已拦截且在用 | 低 |
| 中 | Tier 5.6 执行轨迹导出 | 记录侧已有，缺导出与容量 | 低中 |
| 低 | Tier 6.5 反-反虚拟化闭环 | 依赖 TSC + CPUID 的结果，必须在其后 | 中 |
| 低 | 技术债（`g_protect` union / 清单派生 / 推送）| 可随时插队；推送需明确指示 | 低 |

---

## 10. 新窗口第一件事

```powershell
cd D:\yuanguard
git log --oneline -3            # HEAD 应含 9.366 的 feat 提交
git status --porcelain          # 应为空
cd D:\yuanguard\YuanGuardHV
$env:YGHV_BAREMETAL_STEP=206; $env:YGHV_206B_COEXIST=1; $env:YGHV_206B_GNPT=1
.\build.bat                      # 应 7 个 PASS + Build SUCCESS
```
确认构建成功（**7 个** PASS + Build SUCCESS）。
**不要用 md5 校验** —— 每次构建都会因重新签名而变（见第 2 节的说明）。

然后读：
1. `docs/YUANMOD_HANDOFF_CURRENT.md` **9.366**（上一轮四项收口）与 **9.367**（下一步规划 + 三处事实更正）
2. `docs/KERNEL_API_CHECKLIST.md` §27（VA/PA）、§28（页精确/具名）—— **最易重犯**
3. 本文件第 4 节（架构约束，4.6-4.8 是 9.366 新增的）

**动手前先问用户要做什么。不要自动开始剩余项。**

---

## 11. 累积已验证历史（全部在实机跑过，零崩溃）

```
Phase 1                 6/6
Phase 2-5 W1-W8        10/10
Phase 7  C1-C6          7/7
Phase 4b D1-D6          6/6
Phase 6  E1-E6          6/6
Tier 4.1 / 4.3          9/9 / 9/9
Tier 4.2 / 4.4 / 4.5    8/8 / 7/7 / 7/7
Tier 4.6                4/4
Tier 7.2 / 7.7          8/8
Tier 7.1 / 7.3          9/9
Tier 7 全功能          13/13   ★ 9.366 复跑仍全绿
G30 诱饵语义 + 接线     ALL PASS  ★ 9.366
G32 多核隐藏 soak 1min  STABLE    ★ 9.366（DECOY 路径，判词已限定）
```

> **INT3 热路径不在这份清单里** —— B1-B3 验的是写/恢复字节，不是命中（9.367.3）。

**近期镜像归档**（`D:\aaaaaavm\`）：

| 镜像 | 说明 |
|---|---|
| `206z9_pageexact` | frame/view 页精确 |
| `206zA_why` | 含 state_why 诊断 |
| **`206zB_pageexact`** | **当前**：7.4/7.5/7.6 接线 + 诱饵重写 + 10 处页精确 + 位置式强制具名 |

> md5 不再是镜像身份。**比 `.map` 或代码段 0x400..0x24E54**。

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