# YuanGuardHV 新窗口恢复提示词（2026-10-30 重写）

> **本文件是唯一入口。** 读完第 0、1、3、4 节就能接手。
> 完整历史在 `docs/YUANMOD_HANDOFF_CURRENT.md`（13754 行，按 9.3xx 编号），
> 只在需要溯源某条结论时去查，**不要从头读**。

---

## 0. 一句话现状

**裸机 SVM/NPT hypervisor：能加载、能虚拟化、能隐藏页、能跑 13/13 回归；
但「卸载」这条路有 29 次崩溃，根因已定位到设计层面，尚未解决。**

| 能做什么 | 状态 |
|---|---|
| 加载驱动 | **可以**（最后一次改动前已验证多次）|
| 虚拟化 12 核 + NPT | **可以**（`g_npt.pml4_pa` 有效、12 个 VCPU 全建好）|
| 隐藏页 / decoy / 13 项回归 | **可以**（G25 13/13）|
| Tier 9.1 无设备 CPUID 通道 | **可以**（设备可移除，门铃可达）|
| Tier 9.2 控制通道认证 | **可以**（G41 13/13）|
| **卸载驱动** | **会崩或挂死**（见第 4 节）|

**当前代码状态：不可用于加载。** 最后一次改动（`3690dba`）把 guest 入口设为
trampoline，导致 `sc start` 就卡在 `START_PENDING`。**必须先回滚它**（见第 3 节）。

---

## 1. 用户铁律（违反即失败）

1. **会改变机器状态的命令必须先得到明确确认**：`sc create/start/stop/delete`、
   加载/卸载驱动、压力测试、重启、改启动链、推 GitHub。
   *只读操作可以直接执行*。
2. **只修用户报告过的问题**，不擅自扩大修复范围。
3. **每次改动/决策都写进 `docs/YUANMOD_HANDOFF_CURRENT.md`**，编号 9.3xx。
4. **构建和蓝屏取证不需要确认**。
5. **绝不推送 GitHub**（全部本地提交）。
6. **不要用 `python -c` 改 .h/.c**，用 edit 工具。
7. **蓝屏后自动取证 → 定位 → 修复 → 重跑，不中断**。
8. **优先级**：重启类最后、压测最后、按蓝屏风险从小到大做；
   高风险（会崩/会重启）先确认。

---

## 2. 环境

```
工作目录      D:\yuanguard
驱动源码      D:\yuanguard\YuanGuardHV\hv\
测试脚本      D:\aaaaaavm\
客户端        D:\yuanguard\YuanGuardHV\tools\yghv_ctl.ps1
构建          cd YuanGuardHV && set YGHV_BAREMETAL_STEP=206 && set YGHV_206B_COEXIST=1
              && set YGHV_206B_GNPT=1 && build.bat
带符号构建    ... && set YGHV_NO_DEVICE=1 && build.bat   （Tier 9.1 用）
调试器        "C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\kd.exe"
              必须用 -y "C:\symbols"（本地符号），srv* 会 Could not write to pipe
dump 配置     CrashDumpEnabled=2, CrashDumpType 已删除, AlwaysKeepMemoryDump=1
              → C:\Windows\MEMORY.DMP（2-3 GB，含物理内存）
机器          12 核 AMD，16 GB RAM，Win10 19045
```

**构建成功的判据不是 md5**（每次重签都变）。看 7 个静态检查全绿 + `Build SUCCESS`。

---

## 3. 当前代码状态与第一件事

```
HEAD = f7e1e8c   (docs only)
      3690dba    fix(9.435-9.436)  ← 这一笔是错的，需要回滚
      48742f2    docs(9.434)
      e8629ca    fix(9.433)  VMMCALL bit1
      6152887    fix(9.432)  武装 VMMCALL
```

**`3690dba` 做了三件事，其中一件错：**

| 改动 | 判断 |
|---|---|
| 9.432/9.433 武装 `SVM_INTERCEPT_MISC2_VMMCALL = (1UL << 1)` | **保留**（位号已按 bhyve 表核对）|
| 9.435 加 `case VMEXIT_VMMCALL`（当前是 `break;`）| **保留但改**：改成 `guestContext.ExitVm = TRUE;` |
| 9.436 把 guest 入口设为 `svm_trampoline_test_guest` | **回滚** —— 它把「透明虚拟化」换成了「测试 guest」，见第 4 节 |

**新窗口第一件事：回滚 9.436 的 guest 入口改动，保留 VMMCALL 位与 case。**

回滚后 RIP 恢复为 `ContextRecord->Rip` —— 那是上游 SimpleSvm 的语义（透明虚拟化）。

**第二件事：机器需要重启**，因为 `yghvpa` 服务卡在 `START_PENDING`（`sc stop` 返回 1052
"该服务此刻不可控制"）。重启前确认工作树干净。

---

## 4. 核心结论：卸载为什么崩（29 次崩溃的最终归因）

### 4.1 完整的因果链（每一环都有实测数据）

```
DriverEntry
  └─ svm_core_init()                     12 个 VCPU + VMCB + hsave 全部建好（已证：dump 逐字段）
  └─ Sv206Entry()
       └─ SvVirtualizeAllProcessors()
            └─ 每核 SvVirtualizeProcessor
                 └─ SvPrepareForVirtualization
                      └─ StateSaveArea.Rip = ContextRecord->Rip   ★ 关键
                 └─ Sv206LaunchVm()
                      └─ vmrun  ← guest 从此执行「SvVirtualizeProcessor 自己的续行代码」
```

**★ 核心事实（9.434）：guest 的入口不是任何测试 guest，而是
`SvVirtualizeProcessor` 自己捕获的 RIP。** 那行代码就在
`SvPrepareForVirtualization` 里：

```c
VpData->GuestVmcb.StateSaveArea.Rip = ContextRecord->Rip;
```

而 `ContextRecord` 是几行之前 `RtlCaptureContext` 抓的 —— 抓的是那个函数当下的位置。
它之后是 `Sv206LaunchVm()` / `SV_DEBUG_BREAK()` / `KeBugCheck()` —— **没有任何被拦截指令**。

**后果链**：

```
guest 执行 SvVirtualizeProcessor 的续行代码
  → 不执行 CPUID、不执行 vmmcall、不碰 CR3
  → 永不产生 #VMEXIT
  → vmrun 永不返回
  → vmsave 不执行 → lock dec 不执行 → g_inVmrunCount = 12（永远，已实测）
  → SvHandleVmExit 一次不跑（g_S206VrLog 整场为空，已实测）
  → stop flag 检查点（在循环回边）永不执行（snap_vmrun_stopped=0，已实测）
  → DriverUnload 走完 14 步（step=120 DONE，已实测）但等不到计数归零
  → 返回后操作系统 unmap 驱动镜像
  → 卡在 vmrun 里的核执行已失效的代码页
  → FAILURE_BUCKET = AV_CODE_AV_STACKPTR_ERROR_PAGED_IP
```

### 4.2 崩溃点的三次修正（前两次都错了）

| 版本 | 我说崩溃在 | 实际 |
|---|---|---|
| 9.389/9.395/9.407 | `mov rax,[rsp]`（读 `VpData->HostStackLayout`）| ✗ |
| 9.421 | `vmsave rax`（写已释放的 VMCB）| ✗ |
| **9.427** | — | **`vmrun rax` 本身，且 `PAGED_IP`（代码页已失效）** |

按 ASM 桩布局算偏移（含 `lock inc/dec`）：

```
+0x00 mov rsp, rcx      +0x03 mov rax,[rsp]   +0x07 vmload rax
+0x0A lock inc           +0x12 vmrun rax  ← 崩溃在 +0x14（操作数 D8）
+0x15 vmsave rax         +0x18 lock dec
```

**`FAILURE_BUCKET: ..._PAGED_IP_`** 是这个结论的判词：不是「访问了错误的**数据**」，
而是「执行了已失效的**代码**」。

### 4.3 29 次崩溃的分类

| 类 | 次数 | 崩溃码 | 已修？ |
|---|---|---|---|
| 值/位置类 | 7 | hang / `0x101` / `0x3B` / `0x139`×3 / 6008 | **是**（各有提交与测试）|
| EFER 类 | 3 | `0x1E` — IPI 里清 `EFER.SVME`（APM 明文 undefined）| **是**（9.380，已证生效）|
| 卸载顺序类 | 5 | `0xD1` / `0x7F` | 否 |
| 硬复位 | 4 | 无 dump 无 bugcheck | 否 |
| 本会话新引入 | 3 | `0x101`（无界等待）、`START_PENDING`（guest 入口）| 需回滚 |
| 第三方 | 1 | `usbwifi` | 无关 |

### 4.4 已证伪的 8 个推断（别再重走）

| 推断 | 证伪依据 |
|---|---|
| `npt_cleanup` 提前释放活表（9.381）| `npt-range` 工具：NPT 在 `0xC7B...`，故障地址全不在 |
| VPD 页对齐是根因（9.386）| 改真页对齐后崩溃不变 |
| AVIC timer 是元凶（9.388）| 加了 cancel+drain 后崩溃不变 |
| ack 从未被写是根因（9.378/9.383）| ack 早在 ASM 桩 `SvLV20` 里就有（`mov ecx,'SSVM'`）|
| 有第二套 VMRUN 循环在跑（9.395）| `g_S206VrLog=0`、`resident=0`、`s203=0` |
| stop check 位置错（9.413/9.414/9.416，三次）| 零 call 的内联版照样 `0x7F/8`，崩溃点不动 |
| `vmsave` 写已释放 VMCB（9.421）| `PAGED_IP` —— 是代码页失效不是数据 |
| guest 是 trampoline、缺 VMMCALL 位（9.431）| 加位加 case 后反而**加载挂死**（9.437）|

**注意最后一行**：9.431 的「guest 是 trampoline」是**从 `svm_trampoline.asm` 的存在
推断的**，而 9.434 读了赋值语句本身才发现不是。**「有个文件存在」不等于「它被用了」。**

---

## 5. 架构约束（违反会崩或挂）

来自 9.3xx 的实测，**每条都是踩出来的**：

### 5.1 VMCB 位是 per-core，且跨驱动重载存活
`prepare` 每核只跑一次。运行中改某核的拦截位必须用
`SvExecuteOnEachProcessor`（已在 `SvTscApplyOnCore` 里验证）。
**且 `prepare` 必须显式清零任何我们可能设过的位** —— 「这次不设置」≠「清除」。

### 5.2 `SvExecuteOnEachProcessor` 不是通用原语
它只保证当前线程绑到目标核，**不保证那个核不在 VMRUN 里、不保证没有 guest 正在碰
要改的状态**。TSC 位用它成功了（没有东西在用）；`#BP` 位用它两次 `0x139`。

### 5.3 VM-exit 帧里不要调用 C 函数
`SvLV10` / `SvLV20` 这类 ASM 桩的栈布局是按「不碰 guest 状态」设计的。
在里面 `call` 一个 C 函数会因序言（`__chkstk` / SEH frame）失败：
- 9.413 → `0x139 MISSING_GSFRAME`
- 9.414 → `0x7F` 双故障
- 9.416 → **零 call 的内联版也 `0x7F`** ⇒ 连纯内联都不能随便加

### 5.4 `KeIpiGenericCall` 只保证到达，不保证上下文
回调里 `wrmsr` 在 GIF=0 的核上会被拒（`0x1E`）。
合法清 `EFER.SVME` 只有一个窗口：`ExitVm` 路径里
`vmload` → `stgi` → `wrmsr`。

### 5.5 `NT_ASSERT` 在 free build 里是空的
任何「只靠 `NT_ASSERT` 支撑」的前置条件（对齐、范围、互斥）在 release 驱动里
**等于不存在**。判据：去掉 assert 后代码会算错地址 ⇒ 它必须是运行时检查。

### 5.6 页表遍历用 `mem_pte.c`，不是 `protect.c`
`protect.c` 的 `yghv_pt_read` 用 `MmGetVirtualForPhysical`（本项目自己标记为
*not reliable for arbitrary physical pages*）。`mem_pte.c` 用 `MmCopyMemory(PHYSICAL)`，
注释明确 *safe at DISPATCH_LEVEL and takes no locks*。**两套并存，选错一次赔一台机器。**

### 5.7 观测点必须满足三个条件
1. 只有那个过程会写（不被被观测的过程改写）
2. 单调（写进去不会被清零）
3. 在要观测的事件之前已有值

不满足就会出现「双义值」：卸载过程中的读数天然会被卸载本身改写
（`g_PowerCallbackRegistration` 被清成 NULL、`g_resident_thread_count` 被清成 0）。

### 5.8 跨崩溃存活的存储不能属于会被卸载的模块
静态变量、模块 `.bss`、模块内分配的页 —— 驱动 unmap 后**都不再属于你**。
本树为此踩了两次（9.390 的静态页、9.391）。

---

## 6. 可用的观测手段（都验证过）

### 6.1 卸载进度页（`yghv_ut_mark`）
`hv/yghv_unload_trace.c` —— 加载时 `MmAllocateContiguousMemory` 分配，
**故意永不释放**（4 KB/次加载的泄漏，换一份能活过卸载的记录）。
120 槽不取模历史环 + 独立快照区。

埋点：`1 START / 10 NOTIFY_OFF / 20 COOP_UNLOAD / 21 skip-walk / 25 COOP+5 /
30 STOP_REQ / 40 JOIN_GUEST / 45 stopped / 46 active / 47 cond / 50 JOIN_203 /
51 in-vmrun / 52 waited / 53 skipped / 55-57 最终等待 / 80 CTRL / 90 PROTECT /
100 NPT / 110 CORE / 120 DONE`

**读法**（PA 必须**卸载前**记录，见 6.2）：

```
kd -z C:\Windows\MEMORY.DMP -y "C:\symbols" -c "!dd <PA> L40; q"
```

**.bss 不可达时怎么办**：kernel dump 只保存内核可达的页，驱动 `.bss` 时有时无。
用 6.2 的 PA 定位进度页（它在池里，通常可达）。

### 6.2 PA 记录流程（`t_paflow.ps1`）
```powershell
pwsh -File D:\aaaaaavm\t_paflow.ps1 -Image <sys> -Label <tag>
```
它做三件事：加载 → **用 `unload-trace` 读出 PA 写进 `pa_<tag>_<stamp>.txt`** →
`sc stop`。崩溃后拿那个 PA 去 dump 里查。

**必须这么做**：PA 每次加载都变，事后算不出来。

### 6.3 `npt-range`
```
yghv_ctl.ps1 npt-range            → 页数 + 最低/最高 PA
yghv_ctl.ps1 npt-range <hexpa>    → 该地址是不是我们的 NPT 页
```
**用途**：把「这地址像不像 NPT」从推断变成算术。它已经证伪了一个错了三次的假设。

### 6.4 `unload-trace`
```
yghv_ctl.ps1 unload-trace   → page PA / last step / value
```

### 6.5 dump 分析的三条硬规矩
1. **必须 `-y "C:\symbols"`**（本地）。`srv*` 会 `Could not write to pipe`，
   `!analyze` 完全失效（实测 248631 行错误、0 个结果字段）。
2. **`.map` 的 RVA 用 map 里 `00000001400xxxxx` 那一列**，不是 `0003:000xxxxx`
   段内偏移。用错读到的全是 0。
3. **映射前先看相邻符号间隔**：6-16 字节的是 import thunk 表，不是函数体。

---

## 7. PowerShell / 构建陷阱

1. **脚本必须 UTF-8 with BOM 或纯 ASCII**。没有 BOM 的中文会让 PS 5.1 报
   「缺少 `}`」这种远离真因的错误。
2. **单引号字符串里不要放 em dash（`—`）**，会破坏引号配对。
3. **`throw '...<hex>...'` 里的 `<` 会被当成重定向**。
4. **`[uint32]$negativeInt` 会 InvalidCast**，用
   `[BitConverter]::ToUInt32([BitConverter]::GetBytes($v),0)`。
5. **`0x...u` 后缀是 C 语法**，PowerShell 没有。
6. **空串参数不会以空串到达**：`f('')` 收到的是 `$null`。需要显式 token（如 `key clear`）。
7. **`0xDEADBEEF` 在 PS 里是负数**，用 `[uint32]3735928559`。
8. **`git refs/heads/main` 会被重启中断写成全 NUL**。修复：
   `tail -1 .git/logs/HEAD | awk '{print $2}'` → 写回 `.git/refs/heads/main`。
9. **测试脚本自己会造假象**：断言必须能区分它声称要测的机制。
   本树有 7 个「测试断言了自己的假设」的案例。

---

## 8. 剩余工作清单（诚实版）

### 8.1 必须马上做（不做则代码不可用）
1. **回滚 9.436**（guest 入口改动）→ 恢复 `Rip = ContextRecord->Rip`
2. **把 `case VMEXIT_VMMCALL` 改成 `ExitVm = TRUE`**（不是 `break`）
3. **重启机器**清 `START_PENDING`

### 8.2 卸载问题的下一步（需要设计决定）
9.434 留下一个必须回答的问题：

> **206 主线想让 guest 做什么？**
>
> **(a) 透明虚拟化**（上游语义）：guest = 调用者自己的续行。
>   则 `vmrun` 返回是正常路径 —— 需要查「返回后为什么没继续」。
> **(b) 真实 guest**：必须显式设 `Rip = svm_trampoline_test_guest`。
>   但那样「虚拟化建好就撤」，206 主线名不副实。

**这个决定需要用户拍板，不是代码能推出来的。**

### 8.3 原清单（Tier 5/6/8/9，共 30 项）
| Tier | 项 | 状态 |
|---|---|---|
| 5.1 TSC | offset 平移 / freeze | offset **成立**；freeze **已禁用**（无法证明释放）|
| 5.4 CPUID 视图 | 三档 | **完成**（`016aa31`）|
| 5.6 轨迹导出 | | **完成**（`9e1b2bc`）|
| 9.1 无设备通道 | | **完成**（设备可移除，门铃可达）|
| 9.2 通道认证 | HMAC + 序号 | **完成**（G41 13/13）|
| 9.3/9.4/9.5 | 冗余/远程/隐蔽 | 未做（9.4 建议跳过并写原因）|
| 6.4 | 跨 VM 操纵 | **跳过**（需写原因）|
| 6.1 | 启动期控制 | **需要重启，最后做且要确认** |
| 其余 5.x / 6.x / 8.x | | 未做 |

**注意**：`MEM_ENGINE_PLAN_TIER5/6/8_9.md` 开头都写「现成 VMEXIT 已实现 35 种」——
**那是错的**。实际只有 5 组拦截武装、9 个 case。文档已在 9.367 标注修正。

---

## 9. 关键文件

| 文件 | 作用 |
|---|---|
| `hv/svm_simplevm206.cpp` | 206 主线：VMCB 准备、`SvHandleVmExit`、CPUID |
| `hv/svm_simplevm206_x64.asm` | VMRUN 桩（`SvLV10` 循环 / `SvLV20` 出口）。**改这里极度危险** |
| `hv/svm_simplevm206.hpp` | VMCB 结构 + 拦截位定义 |
| `hv/svm_core.c` | 旧 resident 机制（`g_resident_thread_count=0` ⇒ 未启用）|
| `hv/svm_trampoline.S` / `svm_trampoline.asm` | 测试 guest（三条指令）。**当前未被用作 guest 入口** |
| `hv/npt_core.c` | NPT。`npt_cleanup` 的 `pt_reg` 已修失效复用（9.381）|
| `hv/mem_pte.c` vs `hv/protect.c` | **两套页表遍历**，用前者 |
| `hv/yghv_unload_trace.c` | 卸载进度页（第 6.1 节）|
| `tools/yghv_ctl.ps1` | 客户端，63 条命令。有 `unload-trace` / `npt-range` / `sys-auth` |
| `tests/` | 7 个静态检查脚本，`build.bat` 自动跑 |

---

## 10. 新窗口的第一个动作（照着做）

```powershell
# 1. 确认状态
cd D:\yuanguard && git log --oneline -3 && git status --porcelain
sc.exe query yghvpa      # 若 START_PENDING → 需重启

# 2. 回滚 9.436 的 guest 入口（只改那一个 #if 块）
#    恢复 VpData->GuestVmcb.StateSaveArea.Rip = ContextRecord->Rip

# 3. 把 case VMEXIT_VMMCALL 的 break 改成 guestContext.ExitVm = TRUE;

# 4. 构建验证
cd YuanGuardHV && set YGHV_BAREMETAL_STEP=206 && set YGHV_206B_COEXIST=1 `
  && set YGHV_206B_GNPT=1 && build.bat    # 必须 7/7 全绿

# 5. 重启（清 START_PENDING），然后才谈下一步
```

**在第 10 节完成之前，不要动卸载路径的任何代码。**
29 次崩溃里 8 次是因为「在不可观测的路径上改代码」。

---

## 11. 累积已验证历史（实机跑过）

| 测试 | 内容 | 结果 |
|---|---|---|
| G25 | 13 项回归（arm/hide/decoy/sha/health/unload） | **13/13** |
| G33 | snapshot 非破坏 + drain 消费 | ALL PASS |
| G39 | TSC 逐核武装与释放 | ALL PASS |
| G40 | 逐核 walk 可达性（12/12 核） | ALL PASS |
| G41 | 控制通道认证 | 13/13 |
| G43 | 无设备 CPUID 通道 | 部分（设备可移除已验证）|
| G18/G28/G29/G30/G31 | hide/decoy/border/why | 见各 9.3xx |

**回归基线镜像**：`206zR_va`（`db7547ab`）—— 那是最后一次 G25 13/13 全绿的版本。

---

## 12. 加速查证（别重复劳动）

```powershell
# 当前所有 yghv 服务
sc.exe query yghvpa; sc.exe query yghvfin

# 蓝屏事件
Get-WinEvent -FilterHashtable @{LogName='System';Id=6008,1001} -MaxEvents 3

# dump 是否存在及时间
ls C:\Windows\MEMORY.DMP

# 从 dump 读进度页（PA 来自 pa_*.txt）
kd -z C:\Windows\MEMORY.DMP -y "C:\symbols" -c "!dd 0xC7BD5000 L40; q"

# 从 dump 读某个全局变量（RVA 用 map 里 14000xxxxx 那列 + 模块基址）
kd -z C:\Windows\MEMORY.DMP -y "C:\symbols" -c "dq <addr> L1; q"
```

---

## 13. 一句话给新会话

**这个项目的难点不是写代码，是「在没有观测能力的路径上，任何改动都不可验证」。**
今晚 29 次重启里，真正修好的根因有 8 个，而**证伪自己的推断有 8 个** ——
后者花的代价和前者一样多。

**所以：先建观测，再改代码。**
