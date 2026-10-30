# YuanGuardHV 新窗口恢复提示词（2026-10-30 重写，9.439 更新）

> **本文件是唯一入口。** 读完第 0、1、3、4 节就能接手。
> 完整历史在 `docs/YUANMOD_HANDOFF_CURRENT.md`（13858 行，按 9.3xx 编号），
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

**当前代码状态：源码已回滚到可加载的语义，构建 7/7 全绿，尚未实机验证。**
`9.439` 已回滚 9.436（guest 入口恢复 `ContextRecord->Rip`）、把
`case VMEXIT_VMMCALL` 改成 `ExitVm = TRUE`，并在反汇编层面复核过产物。
**待办：机器重启后 `sc start yghvpa` 验证加载不再挂死。**（见第 3 节）

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
HEAD = 9.439 那一笔（源码回滚，未推送）
       b3a4277    docs(9.438)  重构交接文档
       f7e1e8c    docs(9.437)
       3690dba    fix(9.435-9.436)  ← 已被 9.439 回滚
       48742f2    docs(9.434)
       e8629ca    fix(9.433)  VMMCALL bit1
       6152887    fix(9.432)  武装 VMMCALL
```

**`3690dba` 做了三件事，其中一件错 —— 9.439 已处理完：**

| 改动 | 判断 | 9.439 实际做了什么 |
|---|---|---|
| 9.432/9.433 武装 `SVM_INTERCEPT_MISC2_VMMCALL = (1UL << 1)` | **保留**（位号已按 bhyve 表核对）| 未动 |
| 9.435 加 `case VMEXIT_VMMCALL`（原来是 `break;`）| **保留但改** | 已改为 `InterlockedIncrement64; ExitVm = TRUE;` |
| 9.436 把 guest 入口设为 `svm_trampoline_test_guest` | **回滚** | 已恢复 `StateSaveArea.Rip = ContextRecord->Rip`，并删掉只为它存在的 `extern` 声明 |

**已构建验证：7 项静态检查全绿 + `Build SUCCESS`，并在 `dumpbin /disasm` 产物里
确认了 `[r13+0xF8]`（ContextRecord->Rip）写入 VMCB，以及 VMMCALL case 的 `mov dil,1`；
全二进制对 `svm_trampoline_test_guest` 的引用数为 0。**

**下一步（需要用户确认，因为会改机器状态）：重启**，清掉 `yghvpa` 的
`START_PENDING`（`sc stop` 返回 1052 "该服务此刻不可控制"），
重启后 `sc start yghvpa` 验证加载不再挂死。重启前确认工作树干净。

**在加载验证通过之前，不要动卸载路径的任何代码。**

---

## 4. 核心结论：卸载为什么崩（29 次崩溃的最终归因）

> **⚠ 9.440 先把「实测」和「推断」分开。** 下面 4.1 那条链引用的四个读数里，
> **只有 `g_inVmrunCount = 12` 是真正的测量**。另外三个的实际情况：
>
> | 读数 | 9.440 复核结果 |
> |---|---|
> | `g_S206VrLog` 为空 | **只说明「没有受保护页的写 NPF」**。全文件唯一写入点在 NPF 写裁决分支里（`svm_simplevm206.cpp:2651`），没 arm 保护页的机器即使 VMEXIT 跑一千万次，它也必然为空。**这是保护引擎仪表，不是 exit 计数器。** |
> | `snap_vmrun_stopped = 0` | **恒等式**。`g_VmrunStopped` 唯一递增点在 `Sv206VmrunShouldExit()`，而该函数**在产物里零引用**（9.419 删了 ASM 里的检查，C 侧没删）。`dumpbin` 查 `1400223340h` 命中 0 次。 |
> | `snap_resident_threads = 0` | **恒等式**。206-B 模式不启动 resident（`main.c:5521` 注释「Residents are not started」），所以它读的 `g_resident_thread_count` 从来没被写过。 |
>
> **推论**：`main.c:5047` 的判据 `stopped == 0 && resident_threads_at_entry == 0`
> **两个条件都是恒真的**，那个分支实际上无条件执行。9.405/9.406 基于它做的
> 「nothing was running」判断不成立。
>
> **详见 `YUANMOD_HANDOFF_CURRENT.md` 的 9.440 节。**

### 4.1 完整的因果链（每一环都有实测数据）

> **这一节的标题在 9.440 之后是不准确的**：链上有三环不是实测。保留原链是为了
> 让后来者看到推理是怎么走的，但**不要把它当已证结论**。

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

> **⚠ 9.439 修正这一句的适用范围。** 「没有任何被拦截指令」描述的是
> `SvVirtualizeProcessor` **剩下的那三行**，不是那三行 `return` 进去的内核代码。
> guest 是调用者续行，它下一件事就是执行真实内核指令，CPUID / CR3 写 / MSR / NPF 都会来。
> **所以下面这条后果链的起点（「永不产生 #VMEXIT」）从未被证明，只是被假设。**
> 9.439 已把 guest 恢复成续行，这一环现在是**待查**而非**已定**。
>
> **⚠ 9.440 补充：这一环不但没被证明，而且无法用现有仪表证明。**
> 这棵树上「有没有 VMEXIT」目前**没有任何计数器能回答** —— 见本节开头的表格。
> 要回答它，必须先加一个无条件 exit 计数器（见 8.2）。

**后果链**（`【实测】` / `【非测量】` / `【推断】` 逐行标注）：

```
guest 执行 SvVirtualizeProcessor 的续行代码
  → 不执行 CPUID、不执行 vmmcall、不碰 CR3                  【推断】
  → 永不产生 #VMEXIT                                        【推断 ← 无仪表可证】
  → vmrun 永不返回                                          【实测】g_inVmrunCount = 12
  → vmsave 不执行 → lock dec 不执行                          【实测】同上
  → SvHandleVmExit 一次不跑                                  【非测量】g_S206VrLog 是 NPF 仪表
  → stop flag 检查点永不执行                                  【非测量】恒等式，该检查已被删除
  → DriverUnload 走完 14 步但等不到计数归零                    【实测】step = 120 DONE
  → 返回后操作系统 unmap 驱动镜像
  → 卡在 vmrun 里的核执行已失效的代码页                        【实测】PAGED_IP 崩溃桶
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
| 本会话新引入 | 3 | `0x101`（无界等待）、`START_PENDING`（guest 入口）| **`START_PENDING` 已由 9.439 回滚** |
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

**9.439 追加第 9 条证伪**：

| 推断 | 证伪依据 |
|---|---|
| 换成测试 guest + 武装 VMMCALL，vmrun 就会正常返回（9.436）| **加载就挂死**（9.437 的 `START_PENDING`）。vmmcall 是心跳不是退出请求，`break` 让循环无限重进 vmrun。|

**9.440 追加第 10 条 —— 而且它让上面表格里的第 3 条失去依据：**

| 推断 | 9.440 复核 |
|---|---|
| 「有第二套 VMRUN 循环在跑」（9.395 证伪）| **证伪所依据的三个读数全部失效**：`g_S206VrLog=0` 是 NPF 仪表不是 exit 计数、`resident=0` 在 206-B 下是恒等式、`s203=0` 同样因为 resident 没启动。**这条证伪不成立**，「第二套循环」需要重新排除。|

**另注（9.440）**：表格第 3 条「ack 早已在 ASM 桩 `SvLV20` 里」是对的 ——
`SvLV20` 确实有 `mov ecx,'SSVM'`。**但这恰恰说明 9.383 那行
`VpRegs->Rcx = 'SSVM'` 是多余的**（`SvLV20` 自己会写 ECX），
而它把 `mov rsp, rcx` 的输入变成了垃圾。见 8.2。

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
1. ~~**回滚 9.436**（guest 入口改动）→ 恢复 `Rip = ContextRecord->Rip`~~ **已完成（9.439）**
2. ~~**把 `case VMEXIT_VMMCALL` 改成 `ExitVm = TRUE`**（不是 `break`）~~ **已完成（9.439）**
3. **重启机器**清 `START_PENDING` —— **待做，需用户确认**
4. **重启后 `sc start yghvpa`** 验证加载不再挂死 —— **这是回滚生效的唯一证据**

### 8.2 卸载问题的下一步（9.440 重写：先补仪表，再谈根因）

9.434 留下的问题被 9.436 用「换成测试 guest」回答了，9.437 证明那个答案会让加载挂死。
9.439 已回到透明虚拟化。**而 9.440 复核发现：这个问题目前连「问清楚」都做不到 ——
因为没有任何仪表能回答「有没有 VMEXIT」。**

**所以下一步不是改代码，是加一个无条件计数器。**

最小仪表（三行，`SvHandleVmExit` 开头即可）：

```c
extern "C" { volatile LONG64 g_S206Exits = 0; }
/* SvHandleVmExit 的第一条语句 */
InterlockedIncrement64(&g_S206Exits);
```

加完要能回答三个问题，**每个答案对应完全不同的修法**：

| 读数 | 含义 | 下一步 |
|---|---|---|
| `g_S206Exits == 0` | 真的没有 VMEXIT（9.434 成立）| 查 VMCB 拦截位是否真进了硬件；查 guest 到底在执行什么 |
| `g_S206Exits > 0` 且 `g_inVmrunCount == 12` | 有 VMEXIT 但核回不到宿主 | 查 `SvLV20` / ExitVm 出口（**`rsp = 'SSVM'` 就在这条路上**）|
| `g_S206Exits` 增长极快 | VMEXIT 风暴 | 查 guest 是否在自陷循环 |

**顺带要修的三处死代码/假判据（都不改行为，只让读数有意义）**：

1. `g_VmrunStopped` / `Sv206VmrunShouldExit` —— 无调用者，删掉或接回 ASM。
   只要它还在，`stopped == 0` 就会被误读成「一个都没停」。
2. `main.c:5047` 的 `stopped == 0 && resident_threads_at_entry == 0` ——
   两个条件恒真，该分支无条件执行。要么补真实判据，要么明说它是兜底。
3. `g_S206VrLog` 的注释 —— 它不是 exit 日志，是 NPF 写裁决日志。

**候选根因（9.440 记入，未修，按铁律等用户点名）**：9.383 把
`guestContext.VpRegs->Rcx` 从 guest RSP 改成 `'SSVM'`，而 ASM `SvLV20` 第一句是
`mov rsp, rcx` —— 栈指针被设成 `0x4D565353`（非规范地址，任何栈操作立刻 #GP）。
反汇编已确认该赋值进了产物（`14002 40AD: mov qword ptr [r14+70h],5353564Dh` /
`14002 4C30: mov rsp,rcx`）。

**它还是冗余的**：`'SSVM'` 的用途是让 `SvDevirtualizeProcessor` 的
`registers[2] != 'SSVM'` 通过，而那个检查已由 CPUID leaf 自己的
`registers[2] = 'SSVM'` 满足（`svm_simplevm206.cpp:1649`，9.378 修的）。
**所以这一行既是栈损坏，又没起任何作用。**

**为什么它必须优先排除**：在一个 rsp 是垃圾的上下文里，
**所有关于「卸载顺序」「时序」「竞态」的推理都不成立**。9.413/9.414/9.416
那三次 `0x139`/`0x7F` 也可能与它有关（栈已坏，再加任何栈操作都放大）。

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
# 1. 确认状态（这一步永远是只读的，可以直接跑）
cd D:\yuanguard && git log --oneline -3 && git status --porcelain
sc.exe query yghvpa      # 若 START_PENDING → 需重启（9.439 后仍是这个状态）

# 2. 第 2、3、4 步已在 9.439 做完并构建验证过（见第 3 节），
#    不要再改一遍 —— 改之前先 git log 看有没有 9.439 那一笔。
#    复核方式（只读）：
git log --oneline -3 -- YuanGuardHV/hv/svm_simplevm206.cpp
grep -n "StateSaveArea.Rip = ContextRecord->Rip" YuanGuardHV/hv/svm_simplevm206.cpp
grep -n -A2 "case VMEXIT_VMMCALL" YuanGuardHV/hv/svm_simplevm206.cpp   # 应见 ExitVm = TRUE

# 3. 构建（改动后才需要；构建和蓝屏取证不需要确认）
cd YuanGuardHV && set YGHV_BAREMETAL_STEP=206 && set YGHV_206B_COEXIST=1 `
  && set YGHV_206B_GNPT=1 && build.bat    # 必须 7/7 全绿

# 4. 重启（清 START_PENDING）—— 会改机器状态，需要用户明确确认
#    重启后第一件事：sc start yghvpa，看它是否真的进入 RUNNING
```

**在「`sc start` 能进 RUNNING」这条被实测确认之前，不要动卸载路径的任何代码。**
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

# 9.439 新增：不信源码就信产物 —— 反汇编复核关键赋值
dumpbin /disasm:nobytes YuanGuardHV\bin\yuanguard_hv.sys > dis.txt
# 找 RIP 是否来自 ContextRecord（应见 [r13+0xF8]，且无 trampoline 引用）
findstr /C:"000000F8h" dis.txt
# 找 VMMCALL case 是否真的设了 ExitVm（应见 lock inc g_S206Vmmcalls 后紧跟 mov dil,1）
findstr /C:"14005FFE0h" dis.txt

# 9.440 新增：查某个函数在产物里到底有没有调用者（判死代码）
# 先从 map 取 RVA，再数产物里的引用次数；0 = 死代码，它的计数器是恒等式
findstr /C:"Sv206VmrunShouldExit" YuanGuardHV\bin\yuanguard_hv.map
dumpbin /disasm:nobytes YuanGuardHV\bin\yuanguard_hv.sys | findstr /C:"1400223340h"
# 同理可查 g_VmrunStopped 的唯一递增点是否可达
```

**构建判据**：不是 md5（每次重签都变）。看 `ALL SOURCE CHECKS PASS` +
`ALL MAP CHECKS PASS` + `Build SUCCESS` 三行。

**读数判据（9.440 新增）**：在把这棵树上的任何计数器写进结论之前，先问三个问题：
1. 它**只有**那个过程会写吗？（`g_S206VrLog` 只有 NPF 分支写 ⇒ 它不测 exit）
2. 它的写入点**在产物里可达**吗？（`g_VmrunStopped` 唯一递增点零引用 ⇒ 恒等式）
3. 在这个 build 模式下**那个机制启动了吗**？（206-B 不启 resident ⇒ 该计数器恒为 0）

三个问题里有一个答不上，那个读数就不能当证据。**本树已有 3 个读数栽在这里。**

---

## 13. 一句话给新会话

**这个项目的难点不是写代码，是「在没有观测能力的路径上，任何改动都不可验证」。**
今晚 29 次重启里，真正修好的根因有 8 个，而**证伪自己的推断有 8 个** ——
后者花的代价和前者一样多。

**所以：先建观测，再改代码。**

**9.439 追加一条更狠的**：**读源码得出的结论，也要用产物复核一遍。**
9.434 靠读一行赋值推翻了 9.431，那是对的；但 9.436 又靠读同一行推出了「换成测试 guest」，
结果加载挂死。同一个动作，一次纠正了错误，一次制造了错误 —— 差别不在读没读源码，
在**有没有问「改完之后谁会不满意」**。`SvVirtualizeAllProcessors` 是同步的，
一个永不返回的 guest 必然让 `sc start` 挂住 —— 这一条在动手前就能推出来。

**9.440 追加一条最狠的，因为它打的是「实测」这两个字本身**：

**这个项目最大的风险不是改错代码，是把「恒等式」当成「测量结果」。**
今晚的因果链上有三个这样的读数：一个只测了别的东西（`g_S206VrLog` 测 NPF 不测 exit）、
一个的写入点根本没有调用者（`g_VmrunStopped`）、一个测的机制在本 build 下没启动
（`g_resident_thread_count`）。它们都以「已实测」的身份进了文档，
并支撑了至少一条「已证伪」结论（4.4 第 3 条）。

**判据只有一句：一个计数器能当证据，前提是它在被测事件没发生时也会动。**
做不到这一点的计数器，为空和为 0 都不说明任何事 —— 它只会让一个未经验证的假设
看起来像一次观测。
