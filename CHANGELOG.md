# Changelog

本项目的版本级变更摘要。逐条工程记录（编号 `9.x`，条数以交接文档开头索引为准）在
[`docs/YUANMOD_HANDOFF_CURRENT.md`](docs/YUANMOD_HANDOFF_CURRENT.md)；
本文只回答「两个版本之间发生了什么」。

格式参考 [Common Changelog](https://common-changelog.org)，版本号遵循
[语义化版本](https://semver.org/lang/zh-CN/)。

> **注意本项目的版本号语义**：`0.x` 阶段，MINOR 版本可能包含**破坏性变更**
> （运行时开关语义、默认配置）。每次有此类变更都会在下面明确标注 **Breaking**。

---

## v0.4.6 — 窗口防截图接入（能力面第四块扩张）

### Added
- `yghv_sig_export_of()`：按名称解析任意模块的导出表（三表解析：AddressOfNames / AddressOfNameOrdinals / AddressOfFunctions；NameOrdinals WORD 值索引；Forwarder 拒绝）
- `yghv_win.h` / `yghv_win.c`：窗口显示亲和性模块（`IOCTL_YGHV_WIN` = 0x83B，op 0 SET / 1 GET / 2 STATS）
- 验证回读：Set 后立刻 Get，值不等报 `YGHV_WIN_E_VERIFY`（6）
- NO_GUI 检测：调用线程无 Win32Thread 时报 `YGHV_WIN_E_NO_GUI`（3）而非瞎猜
- PS 客户端 `win` 命令（set / get / 计数器）
- Java 客户端 `FN_WIN = 0x83B` 常量

### Changed
- `yghv_sig_stats()` 从 8 qword 扩到 10（新增 exports / forwarders 计数）
- `yghv_sig.h` 的调用者数组从 8 扩到 10

### Known limitations
- 防录屏的 `NtUserSetWindowCompositionAttribute` 路径未实现
- 遍历目标进程所有窗口自动设置未实现（需要 `NtUserBuildHwndList` + PID 过滤）
- `IOCTL_YGHV_WIN` 的三个 op 全部没上机

### Verification
- 源检查 EXIT=0：17 脚本 / 49 PASS / 33 自检
- 构建 EXIT=0，镜像 SHA256 = 412e30761e177c0a0953c7cd94d285bb1f18008db2baf5e3299d47022cf2b2a2
- `yghv_win` 符号在 map 中已确认

## v0.4.5 — 页表分析成模块（第 0.4.x 条能力面的第三块）

**日期**：2026-11-14
**范围**：v0.4.4..v0.4.5（工程记录 9.593）

> v0.4.3 接进程控制，v0.4.4 接注入，这一轮把「页表」从 IOCTL case 里的手写循环
> 抬成一个能单独调用的模块。**没有新能力面，是同一个能力的第二种问法。**

### Added

- `hv/common/yghv_pt.h` + `hv/yghv_pt.c`：`yghv_pt_cr3_of()` / `yghv_pt_tree()` /
  `yghv_pt_map()` / `yghv_pt_pages()` / `yghv_pt_stats()`。
- `IOCTL_YGHV_PT`（函数号 **0x83A**，设备类型 0x5947，METHOD_BUFFERED）与五个 op：
  `CR3` / `TREE` / `MAP` / `PAGES` / `STATS`。
- `tools/yghv_ctl.ps1` 新增 `pt` 命令，五个子形态。**客户端命令 82 → 83 条。**

### Changed

- `CR3` 不再以一个裸数出现。裸 CR3 在 PCID 机器上低 12 位是 PCID、bit 63 是 no-flush，
  页表基址只是 `CR3 & 0x000FFFFFFFFFF000`：把它当基址打印最多差 4095，
  用它比较两个进程则只要 PCID 不同就永远「不等」。现在 `raw` / `dtb` / `pcid` / `noflush` 分开回。
- `TREE` 保留每一级的**条目与其所在表的物理地址**。原来的 `PTE_PROBE` 只回一个 PA，
  分不清「这一层 absent」与「这张表读不到」。
- 范围扫描的上限由 `out_len` 反算，不取常量：常量会和客户端的请求大小各自漂移。

### Fixed

- `control_ioctl.h` 插入时行尾被写成 `\r\r\n`（39 行）。编译器报的是
  `error: expected identifier or '('` —— 语法错的样子，行尾的病因。
  **改 CRLF 文件必须「先压平到 LF、再整体重建为 CRLF」**；`eol_check.ps1` 查的是
  「同一文件不许混 CRLF 与裸 LF」，`\r\r\n` 在它眼里仍是 CRLF，抓不到。

### Known limitations

- 五个 op **全部没上机**（见 `docs/ISSUES.md` P1-6）。
- `TREE` 的交叉校验是「两条路径一致就算对」，而两条路径共用同一个表读取函数：
  若表读取本身错了，两条会一起错、校验一起沉默。
- 不写页表。重定向已在 `mem_selfmap.c` 与 `npt_remap` 里，有自己关于 flush 与
  「当前 CPU 用哪个 CR3」的不变量；再加一个写者会把那些不变量劈成两半。

### Verification

```
build.bat（YGHV_BAREMETAL_STEP=206 YGHV_206B_COEXIST=1 YGHV_206B_GNPT=1）  EXIT=0
ALL MAP CHECKS PASS
bin/yuanguard_hv.sys SHA256 = 1059462fd02d8401a5de39f228a449520d0e7845dd6fbd444f7e9a93fed4c9d2
dumpbin /imports：PsLookupProcessByProcessId、MmCopyMemory 均已在 IAT
map：yghv_pt 相关符号 23 个
tests/run_source_checks.ps1  EXIT=0  scripts=17 passes=49 selftests=33
```

## v0.4.4 — 注入原语接入（能力面第二块扩张）

**日期**：2026-11-14
**范围**：v0.4.3..v0.4.4（工程记录 9.592）

> v0.4.3 把「进程控制」接上了 IOCTL，这一轮把「注入」接上。
> 落的是五个原语而不是十五条命令 —— DLL 注入与 shellcode 注入都是它们的组合，
> 组合在客户端是一行事，在内核是两个独立演化的分配 bug。

### Added

- **`IOCTL_YGHV_INJECT`（函数号 0x839）与 `hv/yghv_inject.c`**（9.592）：五个原语。
  `ALLOC`（`ZwAllocateVirtualMemory` 提交留驻，上限 1 MB）、`WRITE`（走 `yghv_mem_write`
  的 A/B 双引擎，单块上限 64 KB）、`FREE`、`THREAD`（`RtlCreateUserThread`，可选择挂起创建）、
  `APC`（`KeInitializeApc` + `KeInsertQueueApc`）、`STATS`。
- **`hv/common/yghv_inject.h`**（新）：`yghv_inj_alloc` / `_write` / `_free` / `_thread` /
  `_apc` / `_stats` / `_cleanup` 与十个错误码（`YGHV_INJ_E_NO_TARGET` …
  `YGHV_INJ_E_OFFSETS`）。
- **PowerShell `inject` 命令**（9.592）：`inject [alloc <pid> <size> [prot]|write <pid> <va> <hex>|
  free <pid> <va>|thread <pid> <start> [arg] [suspended]|apc <pid> <tid> <routine> [arg]]`，
  无参数时读八个计数器。

### Changed

- **函数地址由客户端传入，内核不解析**（9.592）：`thread` 的 `start_va` 与 `apc` 的
  `routine_va` 都是调用者给的。走 `PEB.Ldr` 找 `kernel32!LoadLibraryW` 需要三个额外结构偏移
  （`PEB.Ldr` / `LDR_DATA_TABLE_ENTRY.DllBase` / `IMAGE_EXPORT_DIRECTORY`），
  每一个错了都会静默返回一个看起来合理的地址 —— 9.591 的 `PsGetProcessSignatureLevel`
  解出 0x879（真值 0x87A）就是同形状的坑。客户端本来就有精确的 `GetProcAddress`。
- **每个原语只开它需要的权限**（9.592）：`PROCESS_VM_OPERATION` / `PROCESS_CREATE_THREAD` 分开申请，
  `PROCESS_ALL_ACCESS` 一次都不用（它在受保护进程上必然失败，失败形状与「注入被拦」一样，
  会把人引到错方向）。`STATUS_ACCESS_DENIED` 与「没这个进程」分开映射成两个错误码。
- `hv/common/control_ioctl.h` 追加 INJECT 块：`YGHV_INJECT_HDR_QWORDS 8`、
  `YGHV_INJECT_OUT_QWORDS 512`、`YGHV_INJECT_MAX_PAYLOAD 4000`、
  `yghv_ioctl_inject_req_t`（40 字节，payload 紧随其后）。
- `hv/main.c` `DriverUnload` 的清理顺序：`yghv_inj_cleanup()` → `yghv_proc_cleanup()` →
  `yghv_ut_snapshot_pre_unload(`。APC 的 `KernelRoutine` 指向**本镜像**，先卸载就是跳进已释放内存。
- `YghvCtl.java` 加 `FN_INJECT = 0x839`（只加常量不加 `case`）。

### Fixed

- **`WRITE` 的 payload 跨过回复头**（9.592）：METHOD_BUFFERED 单缓冲里请求头 40 字节、
  回复头 64 字节，payload 从 `buf + 40` 开始，**整个落在回复头覆盖范围内**。
  原地读 payload 再写目标进程，写进去的是回复头此刻的内容（零）——
  写成功、返回成功、目标里一段零。修法不是调整顺序（顺序救不了），
  而是先 `ExAllocatePoolWithTag` 拷到池里再写。分发签名因此改成
  `yghv_inject_dispatch(in, payload, payload_len, out, out_len, info)`，
  payload 偏移由 `control_device.c` 算一次传进来，不在驱动里重算。
- **APC 必须校验线程属于目标进程**（9.592）：`PsLookupThreadByThreadId` 只按 tid 找，
  而 **tid 是系统级唯一的** —— 传别的进程的 tid 依然能成功入队，用户态例程地址落进
  那个进程执行不了的地址空间，APC 触发一次就死，发起方看到的是「投递成功」。
  现在队列前强制 `yghv_thread_process(thread) == proc`。
- **README 树计数量错了一次，而且是量法错**（9.592）：`ls hv/ | grep -v / | wc -l` 得到 46，
  写进 README 后被 `docs_check.ps1` 报 `claims 46, disk has 45`。
  `ls` 输出到管道时不给目录加尾斜杠，`grep -v /` 一个目录也过滤不掉 ——
  数出来的是「45 个文件 + `common` 这个目录名」。`docs_check` 用的是 `Get-ChildItem -File`，它是对的。

### Known limitations

- **五个原语一次都没有在真机上跑过**：`C:\yghv_pa.sys` 仍是 9.588 那一版，本轮镜像未部署。
  `dumpbin /imports` 已确认 `ZwAllocateVirtualMemory` / `ZwFreeVirtualMemory` /
  `RtlCreateUserThread` / `PsLookupThreadByThreadId` / `KeInitializeApc` /
  `KeInsertQueueApc` 六条新符号进了 IAT，但「进了 IAT」与「行为正确」是两件事。
- **窗口消息注入仍未接**：它走 win32k 的 `NtUserPostMessage` 那一侧，与这套原语不是同一机制。
- **线程级挂起不存在**：ntoskrnl 没有 `KeSuspendThread` / `ZwSuspendThread`，
  只有进程级 `PsSuspendProcess`（9.591 已用）。单挂一个线程只能走 APC 冻结。
- 能力面清单里剩下的：CR3 解密 / 页表分析、PP/PPL 与窗口防截图、无痕通信扩展。
- P1-4（`IOCTL_YGHV_PROC` 十个 op 未上机）与 P1-5（本轮五个原语未上机）见 `docs/ISSUES.md`。

### Verification

```
YGHV_BAREMETAL_STEP=206 YGHV_206B_COEXIST=1 YGHV_206B_GNPT=1 build.bat
-> ALL MAP CHECKS PASS / Build SUCCESS
镜像 SHA256 e31defa5214af6c9dd9c2987cea876c8f6cba6fc8da62e04ee541e3dced0cc7e
run_source_checks.ps1 -> scripts=17 passes=49 selftests=33 / ALL SOURCE CHECKS PASS
docs_check -> 57 md / 32 links / 78 records / 9 项
```

## v0.4.3 — 进程/线程控制面接入（P1-1 之外的第一次能力面扩张）

**日期**：2026-11-14
**范围**：v0.4.2..v0.4.3（工程记录 9.588–9.591）

> v0.4.2 之后的一轮工作分两半：9.588 把 P1-1 的 `mem-bp dr` 真正接上硬件（代码完成，
> 上机未验证），9.589–9.590 做了原生结构/偏移/特征码/字符串加密四块底层，
> 9.591 把前两块接到 IOCTL 上——这是它们第一次能被外部问到。

### Added

- **`IOCTL_YGHV_PROC`（函数号 0x838）与 `hv/yghv_proc.c`**（9.591）：十个 op。
  `SNAPSHOT` 进程快照（最多 120 条）、`QUERY` 单进程、`PROT` 写 PP/PPL protection 字节、
  `SUSPEND`/`RESUME`、`TERM` 结束进程（含 `YGHV_PROC_TERM_CLEAR_PROT` 先清保护字节）、
  `OFFSETS`/`SIG` 诊断、`THREADS` 线程枚举（含 freeze count）、`TOKEN` 主令牌偷取、`HIDE` 摘链隐藏。
- **偏移表 v2**（9.591）：`YGHV_OFF_VERSION` 1→2，append-only 追加 `inherited_pid` / `exit_status` /
  `thread_list_entry` / `token`，位 `YGHV_OFF_PPID 0x400` / `XSTAT 0x800` / `TLIST 0x1000` / `TOKEN 0x2000`，
  新访问器 `yghv_proc_inherited_pid` / `yghv_proc_exit_status`。
- **两个运行时探针**（9.591）：ETHREAD.`ThreadListEntry` 与 EPROCESS token（EX_FAST_REF）扫
  `0x100..0x900` 步长 8，**命中数必须恰为 1**，否则字段保持 NOT DERIVED 并打日志拒绝。
- **PowerShell `proc` 命令**（9.591）：`proc [<pid>|offsets|sig|threads <pid> [max]|prot <pid> <hex>|
  suspend <pid>|resume <pid>|term <pid> [clearp] [code]|token <src> <dst>|hide <pid>|unhide <pid>]`。

### Changed

- `hv/common/yghv_nt.h` 补 `PROCESS_TERMINATE` / `PROCESS_VM_READ` /
  `PROCESS_QUERY_LIMITED_INFORMATION` / `PROCESS_SUSPEND_RESUME` 四个 `#ifndef` 兜底——
  **这四个常量在 WDK km 头里根本不存在**（只在用户态 `winnt.h`），
  而 `YGHV_PROC_READ_RIGHTS` 从 9.590 之前就引用其中两个，只是从来没有调用点所以没炸。
- `hv/common/yghv_sig.h` 的四个访问器形状宏包 `#ifndef`：它们与 `yghv_accessor.h` 重复定义，
  同时 include 两者的 .c 会拿到 8 条 `-Wmacro-redefined`。
- `tools/gen_strtab.py` 的 NAMES 11 → 13 条（append-only：`key_for(i)` 按索引算，插入会重编整个表）。
- `hv/main.c` `DriverEntry` 在 sig 自检之后调 `yghv_proc_init()`；`DriverUnload` 在
  `yghv_ut_snapshot_pre_unload(` 之前调 `yghv_proc_cleanup()`。

### Fixed

- **`hv/yghv_proc.c` 的 METHOD_BUFFERED 单缓冲别名**（9.591）：`in` 与 `out` 同一个地址，
  第一版先 `RtlZeroMemory(out, out_len)` 再读请求 → `r.op` 读回 0，
  **每一次调用都静默退化成 SNAPSHOT**，客户端收到进程列表而不是它要的结果。
  正确顺序是先 `RtlCopyMemory(&r, in, sizeof(r))` 再清零 out。
- **`YGHV_PROC_TERM_CLEAR_PROT` 的顺序**（9.591）：受保护进程的
  `ZwOpenProcess(PROCESS_TERMINATE)` 一开始就失败，所以清保护字节必须发生在**第一次
  ZwOpenProcess 之前**；失败回滚要用 `PsLookupProcessByProcessId` 重新拿引用，不能用可能已回收的旧引用。
- **README / QUICKSTART 的三处过期计数**（9.591）：`hv/` 44、`common/` 39、
  PowerShell 81 条命令、IOCTL 57 个。`docs_check.ps1` 会核对这些数字，但**它读的
  `tests/check_stats.txt` 是上一轮的**——单独跑 `docs_check` 会被它误导，必须先跑 runner。

### Known limitations

- **`IOCTL_YGHV_PROC` 的十个 op 全部没有在真机上跑过**：`C:\yghv_pa.sys` 仍是 9.588 那一版，
  v0.4.3 的镜像没有部署。`dumpbin /imports` 已确认
  `PsSuspendProcess` / `PsResumeProcess` / `PsGetThreadFreezeCount` / `ZwOpenProcess` /
  `ZwTerminateProcess` / `PsReferencePrimaryToken` 六条新符号确实进了 IAT（导入总数 99），
  但「进了 IAT」与「行为正确」是两件事。
- **三个看着能派生、实际会派生错的导出**不会被加进偏移表，理由记在代码注释里：
  `PsGetProcessSignatureLevel` 会解出 0x879（真值 0x87A，差一个字节）、
  `PsGetThreadFreezeCount` 是二级解引用、`PsIsProtectedProcess` 是 `test` 形状不是访问器。
- **摘链隐藏没有拿 `EPROCESS.ProcessLock`**：实测代价大于它关掉的窗口，是记录在案的取舍。
  对应地 `yghv_proc_cleanup()` 必须挂在卸载路径上——被摘出去的 EPROCESS 比驱动活得久。
- P1-1 `mem-bp dr`（9.588）代码完成且编译通过，**未上机验证**。
- P0-1 Tier 6.1 启动期验收：需重启，不进 0.4.x。

### Verification

```
ALL SOURCE CHECKS PASS     17 脚本 / 49 条 PASS（含 33 条自检）
ALL MAP CHECKS PASS
Build SUCCESS              bin/yuanguard_hv.sys
镜像 SHA256                f3b88948110f2ab9d6f8aba160e3af66ffe2f0a7e66ffd8dbe2615f626a4f3cd
导入符号                   99（全部 ntoskrnl.exe），新增 6 条已验证在列
```

## v0.4.2 — P1-3 全部结案 + 错误码门 + 文档侧修复

**日期**：2026-11-08
**范围**：v0.4.1..v0.4.2（工程记录 9.579–9.584）

> 这一节补写得晚：v0.4.2 打了 tag、发了 notes（`.release/notes-v0.4.2.md`），
> 却**从未进入 CHANGELOG**，于是「完整摘要见 CHANGELOG.md」这句话在 v0.4.2 上不成立。
> 版本级摘要是给人回答「两个版本之间发生了什么」的，它缺一节就等于该版本没发生过。

### Added

- **`sync-stats` 命令**（9.582）：同步守卫的 8 个计数器可读，含 `hits`（回滚次数）。
  此前该守卫**不可观测** —— 没有计数器，「它在工作」和「它没被调用」是同一幅画面。
- **`win32_error_names.ps1` + 5 条自检**（9.583）：文档里「数字 + `ERROR_*`」的配对
  必须与 OS 现场提供的名字表一致；`[HIST]` 标记允许引用自己纠正过的错误；
  `checked==0` 直接判 FAIL（跑起来什么都没比 == 没查）。
- **`docs_check` 第 6–9 项**（9.583）：状态块自洽、镜像哈希、检查器数量、目录树份数。
  反向变异自检 4 → 10 个，正向对照 1 → 2 条。
- **`docs/archive/README.md`** 与 **`reference/README.md`**（9.583）。

### Changed

- `protect-page` 的错误信息：`0x490` 现在翻译成
  "pid X is not a registered target. Run 'set-target X' first."
- `docs/ISSUES.md` 的 P1-3 一节补齐 9.582→9.584 的三步进展。

### Fixed

- **P1-3b 根因**（9.584）：`protect-page` 对任何页返回 `0x490` 的真实原因是
  **缺 `set-target <pid>`**，不是 mmf 页限制。补上后同页同 pid 立即 `OK`。
- **P1-3c 的疑问被实测收窄**（9.584）：目标自写 10/10 存活（`hits` 不变），
  外来写 1/1 被回滚（`hits` 0→1）。守卫实际能区分写入来源，
  但**代码不检查写入者 CR3**，机制未完全解释（可能是 COW 或缓存一致性）。

### Known limitations

- **P1-3c 的机制不明** —— 「能区分」是实测，不是设计。对产品是好消息
  （保护页免受外来写入，不破坏目标自身状态），但它仍是未解释现象。
- P0-1 Tier 6.1 启动期验收：需重启，不进 0.4.x。
- P1-1 `mem-bp dr`：功能不存在（`yghv_bp_add_ex` 对 DR 模式返回 `-12`）。
  9.585 已查明三次失败打在**206-B 不走的 resident 路径**上。

### Verification

```
ALL SOURCE CHECKS PASS      15 脚本 / 39 条 PASS（含 25 条自检）
ALL MAP CHECKS PASS         1 条
docs_check PASS             57 md / 32 链接 / 69 记录 / 9 项
win32_error_names PASS      10 组配对与 OS 表一致
G25                         17/17 ALL PASS（全核真 guest）
性能                        12 线程 1.05-1.08×
卸载                        0.06-0.23 s，invmrun at unload = 0x0
```

> 上面这组数字是 **v0.4.2 发版当时**的实测值，刻意保持原样而不是换成今天的最新值 ——
> 版本级摘要记录的是那个版本。今天的数字在 `docs/YUANMOD_HANDOFF_CURRENT.md` 末尾状态块。

### SHA256

```
acbe2dc6ffab08ab8db917607cc0e37d87567b86ffd4fa5b72d82578619c2963
```

---

## v0.4.1 — 配置不一致可见化 + 真 guest 下重测内存基准

**日期**：2026-11-08
**范围**：v0.4.0..v0.4.1

### Added

- **`RealGuestMode` 覆盖决定现在写进 `progress.log`**（每轮加载一次，含原始注册表值）。
  9.577 反转该值语义后，带着旧约定 `RealGuestMode=1` 的机器会**静默**退回桩模式，
  而可观察症状 `early-returns=0` 在桩模式下也是正常读数 —— 用户分不清
  「你把它关了」和「它默认就是关的」。三种情形实测：

  | 注册表 | `early-returns` | 日志 |
  |---|---|---|
  | 键不存在 | 12 | 无（默认开着） |
  | `=1` | **0** | **两行**（含 `raw=0x1`）|
  | `=0` | 12 | 无（与默认一致）|

### Changed

- `docs/ISSUES.md` 成为**唯一问题清单**（P0–P3 分级 + 版本规划 + 判据纪律）。
  此前清单散在 `TIER_LEDGER` / `RESUME_PROMPT` §8 / `README` 三处且互相冲突。

### Fixed

- **`docs/WINDOWS_AS_GUEST_FINDINGS.md` 自相矛盾**：9.573–9.577 之后，
  同一份文档同时写着「编译期封禁」和「已打通」，而且第七节教人
  「删除那个 `return 0`」——**代码里已经没有那行了**。下一个会话会照它操作然后对不上。
  处置：编号归一（5.1–5.8，原本有两个 5.1/5.2/5.3/5.4），旧内容降为「历史」并保留教训，
  重写一句话结论与第七节。

### Performance — `mem-bench` 在真 guest 下重测

9.449 的 3.96× 是**桩模式**读数。本轮在真 guest 下用同一批字节重测
（判据：`bytes` 相等 + `hits` 相等，见 9.449.4）。

| 模式 | B (pte) 中位数 | faster (A/B) |
|---|---|---|
| 桩 | 18,702,972 cycles | 2.16–2.47×（一个 5.12× 离群）|
| **真 guest** | **21,146,805 cycles** | **1.81–2.38×** |

**B 比 A 快 1.8–2.4×；真 guest 下 B 比桩模式慢约 13%** —— 方向合理：
B 走页表遍历 + 物理读，真 guest 下每次访问过 NPT 翻译；A 走 `MmCopyVirtualMemory`。

> **判据纪律**：A 的方差很大（44–89M cycles），**比值不可靠**，要看 B 的绝对值。
> 9.449 的 3.96× 与这里的 2.4× **不能直接对比** —— 目标进程（javaw 32MB vs
> explorer 6MB）、范围、页密度都不同。

### Verification

```
ALL SOURCE CHECKS PASS   （24 PASS / 0 FAIL）
docs_check PASS          （64 条记录，链接全通）
默认配置                 entries=12 / early-returns=12
卸载                     0.406 s，invmrun at unload = 0x0
镜像                     82BA3E70645B74B8...
```

### Not done (deliberately)

- **P2-1 全核真 guest 挂机 ≥1 小时** —— operator 明确跳过。
  目前最长连续运行仍是约 30 秒 + G25 的 3 分钟。

---

## v0.4.0 — 全核真 guest（默认），性能 1.05×，G25 17/17

**日期**：2026-11-08
**范围**：v0.3.0..v0.4.0（222 个提交，工程记录 9.305–9.577）

### Added

- **全核真 guest 成为默认配置**。真实 Windows 以 guest 态运行在全部 12 个逻辑核上。
- `tools/bench_core.cs`：固定工作量、**带正确性门**的 CPU 基准
  （12 条 lane 必须返回互不相同且非零的 sum）。
- `tests/docs_check.ps1`：**文档门** —— 相对链接、索引行数、空记录、最新编号、归档份数。
- `tests/docs_check_selftest.ps1`：4 个真缺陷的变异自检 + 1 条反向对照。
- `tests/stub_stack_contract.ps1`：`Sv206GuestStub` 体内禁移动 RSP，且必须仍有 `vmmcall`。
- `docs/INDEX.md`、`docs/QUICKSTART.md`、`docs/WINDOWS_AS_GUEST_FINDINGS.md`：
  文档结构重整为「按问题索引」。

### Changed

- **Breaking — `RealGuestMode` 语义反转**：注册表值现在是**禁用开关**。
  旧：`=1` 打开 / 键不存在 = 关。新：`=1` **关闭** / 键不存在 = **开**。
  > 一台机器上如果留着旧约定写下的 `RealGuestMode=1`，升级后会**被它自己的覆盖关掉**。
  > 这个方向是有意选的 —— 事故应该倒向已验证的桩模式。
- **Breaking — `CoreCount` 默认值**：从「物理核数」改为「全部处理器」。
  `0` 或键不存在 = 全核。
- `virt-counts` 在 `early-returns=0` 时的提示文案：不再断言「C frame is never unwound」
  （那句话把健康的桩模式渲染成缺陷），改为按模式分别说明。

### Fixed

- **`0xD1` @ `Sv206LaunchVm+0x1E`（真 guest 卸载必崩，七次蓝屏中的五、六次）**：
  guest 把承载 VMRUN 帧的内核栈销毁了 —— `RtlCaptureContext` 把 guest RIP 设在
  `SvVirtualizeProcessor` 内部，guest 走完 `return` 一路展开到 `Sv206CoreThread` 尾部
  执行 `terminateFn`，**全程在 guest 里**。栈被调度器回收，帧变野指针。
  判据：桩模式 `invmrun at unload = 0x0` / 真 guest `= 0x6`（六个孤儿帧）。
  修法：尾部改为 tick 循环，永不返回。
- **`0x101 CLOCK_WATCHDOG_TIMEOUT`（全核首次运行）**：tick 循环是 `for(;;) { spin; vmmcall; }`，
  每个虚拟化核一个 100% 忙线程；12/12 时 Windows 没有逻辑核处理时钟中断。
  修法：`KeDelayExecutionThread(KernelMode, FALSE, -1ms)`。**CPU 100% → 21–29%。**
- **卸载 4 秒 → 0.046 秒**：等待条件拿 `KeQueryActiveProcessorCountEx()`（12）当上界，
  而 206 只虚拟化 6 个核，**条件不可满足**，每次卸载都空转等超时。
- `safety_checks.ps1` 读的三个文档在重构中被移入 `archive/`，导致这个安全检查
  在 `Get-Content` 上抛出、**一行都没检查**，而且是在 `build.bat` 里抛的。
- 删除两个死变量 `g_S206CoresExited` / `g_VmrunStopped`。后者不只是死，
  它还在污染诊断 —— `snap_vmrun_stopped` 每次都是 0，「没有核停过」与「字段没接线」
  在 dump 里长得一样。

### Removed

- 8 处确认零引用的死代码：`yghv_emit_u8`、`yghv_protect_resolve_va`、
  `yghv_exclude_driver`、`yghv_avic_load_idtr`、`yg_svm_vmload`、`ch_wr32`、
  `yghv_time_readcr3`、`yghv_sentry_find_name`、`Sv206IretSlot`+`S206_IRET_SLOT`。
- **保留**（不是漏掉）：18 个 `*_pad` 变量是故意的页隔离；两个
  `*_unsafe_do_not_call` 是 Phase 2b 的设计记录；`yghv_s206_physical_cores()`
  现在零调用但保留为拓扑查询入口。

### Performance

基准：`tools/bench_core.cs`，12 线程 × 3 亿次 xorshift，固定工作量。

| 配置 | 耗时 (ms) | 相对基线 |
|---|---|---|
| 基线（驱动 STOPPED） | 794 / 801 / 804 | 1.00× |
| 桩模式 6 核（v0.3.0 的默认） | 904 / 1070 | 1.13–1.34× |
| 桩模式 12 核 | 3211 / 3505 | 4.0–4.4× |
| 真 guest 6 核 | 815 / 825 / 875 | 1.03–1.09× |
| **真 guest 12 核（默认）** | **799 / 812 / 823 / 815 / 838** | **1.01–1.08×** |

**成本不在 hypervisor 里**：handler 单次 0.4 µs，全核总 CPU 时间 0.012 s。
桩模式退出量 1,255,119 次 vs 真 guest 27,914 次（45 倍）——
桩的开销来自它自己的 `hlt/vmmcall` 空转。

> **更正 v0.3.0 的一项结论**：v0.3.0 的已知限制写着「12 核持续在 guest 里运行，
> 基线 29% → 加载后 100%，无法在本机消除；唯一解法是换一台有 C-state 的机器」。
> 那句话描述的是**桩模式**。真 guest 下 CPU 21–29%，"换硬件"不再是解法。

### Verification

- **G25 功能验证：17/17 ALL PASS**（全核真 guest，注册表清空走默认）。
  对比 v0.3.0 的 6 核桩模式：15/17（T7/T9 FAIL）。
- T7 判据：hide 之后 **guest 侧读到 `value=0x0`**，不是 seed `0xC4C40000000000` —— DECOY 真实生效。
- 卸载判据：`invmrun at unload = 0x0`，耗时 0.07 s。
- 构建期检查：13 脚本 / 24 条 PASS（含 9 条自检），0 FAIL。

---

## v0.3.0 — 周期同步守卫（绕开 Zen3 NPT TLB 限制的换设计）

**日期**：2026-10-11

### Added

- **周期同步守卫**：既然 NPT 权限翻转无法即时生效，那就不翻转 ——
  页面在 NPT 里**始终 present+WRITABLE**，硬件从不需要失效翻译，限制自然不适用。
  改为后台线程**轮询快照比对**，发现漂移即回滚。**延迟 92 秒 → 2.3 秒（40 倍）。**
- `docs/KERNEL_API_CHECKLIST.md`：三次蓝屏换来的写前检查表。
- `build.bat` 加 `/MAP` —— 崩溃取证时把「模块+偏移」一次映射到函数名。

### Fixed

| Bug | BugCheck | 根因 |
|---|---|---|
| 1 | `0xE2` | `ExAcquireFastMutex` **递归死锁**（不可递归） |
| 2 | `0x3B` | `MmGetVirtualForPhysical` 用于**用户页**（它只映射系统页） |
| 3 | `0x7E` | 注册表 `KEY_VALUE_PARTIAL_INFORMATION.Data` **未对齐** + 无长度校验 |
| 4 | `0x7E` | `KeStackAttachProcess` + `memmove` **野指针**（物理地址跨进程不唯一） |

- 缺陷 C：`DriverUnload` 补 `yghv_trace_close()`（`progress.log` 句柄泄漏）。
- 发现本项目构建用 `/EHs-c-`，**`__try/__except` 完全不生成处理器** —— 此前写的 SEH 是装饰性的。

### Changed

- `config sync <0|1> [interval_ms]` 并入 `config`，不再需要单独的 `config-sync` 往返。
- `YGHV_SYNC_MAX` 16 → 64，新增 `refused` 计数（arm 因表满被拒不再静默）。

### Verification

四步挨个来，全程零蓝屏：最小面 6/6 → 单页 5/5 → 16 页 5/5 → **64 页 5/5**。
性能：每页每轮 ≈2.97 μs；64 页 @20 polls/s ≈ **0.38% 单核**。

---

## v0.2.0 — OS-as-guest 206 里程碑

**日期**：2026-10-05

- vendored 上游 SimpleSvm 逐字进场 + coexist 全核无缝虚拟化 + 512GB identity NPT。
- 206-C1–C5：IOCTL 控制面可用 → 页写保护端到端 → 控制面加固 → 跨进程保护 → 实进程试点。
- 206-C6：假写路线六条 NPT 冲刷路径全部证伪（Zen3 硬件事实）。

---

## v0.1.0 — 首个可用版本

**日期**：2026-08-29

- SVM 初始化 + VMRUN 单核 → 多核 DPC → 裸机回归。
- 内存写保护 / 终止保护 / 句柄保护 / 常驻。
- Java/JNI 客户端、真实进程接入（`protect/unprotect/scan`）。
