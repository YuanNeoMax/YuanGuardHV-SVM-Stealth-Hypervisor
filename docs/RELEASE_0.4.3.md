# YuanGuardHV v0.4.3 — Release Handoff

> **版本**：0.4.3（2026-11-14，tag `bc2b417`）  
> **镜像（v0.4.3 tag 构建）**：`f3b88948110f2ab9d6f8aba160e3af66ffe2f0a7e66ffd8dbe2615f626a4f3cd`  
> **当前 HEAD 构建**：`abeaad12e973a71a...`（HEAD 已在 v0.4.16，与 v0.4.3 tag 不是同一份代码）  
> **测试**：18 检查器 / 42 PASS / 19 自检（docs 两门只判不计），ALL PASS  
> **仓库**：https://github.com/YuanNeoMax/YuanGuardHV-SVM-Stealth-Hypervisor

---

## 一、项目概览

YuanGuardHV 是基于 AMD SVM/NPT 的裸机 hypervisor，Windows 自身作为 guest 运行在全部 12 个逻辑核上。核心目标是**真 guest + 全核**，不是桩模式。

**出货构建参数**：`YGHV_BAREMETAL_STEP=206 YGHV_206B_COEXIST=1 YGHV_206B_GNPT=1`

**真实处理 VMEXIT 的路径**：`hv/svm_simplevm206.cpp` 的 `SvHandleVmExit`（不是 `svm_core.c` 的 resident loop）。

---

## 二、能力面（v0.4.3 全部实现）

### 进程控制（IOCTL 0x838，15 op）

| op | 功能 | 状态 |
|---|---|---|
| 0 | 进程快照 | ✅ |
| 1 | 进程查询 | ✅ |
| 2 | PP/PPL 保护级别修改 | ✅ |
| 3 | 暂停/恢复进程 | ✅ |
| 4 | 强制结束进程 | ✅ |
| 5 | 偏移表诊断 | ✅ |
| 6 | 特征码诊断 | ✅ |
| 7 | 线程枚举 | ✅ |
| 8 | 令牌偷取 | ✅ |
| 9 | 进程隐藏 | ✅ |
| 10 | 句柄降权 | ✅ |
| 11 | 句柄降权诊断 | ✅ |
| 12 | 伪装进程映像名 | ✅ |
| 13 | 令牌提权 | ✅ |
| 14 | 线程隐藏 | ✅ |

### 注入原语（IOCTL 0x839，6 op）

| op | 功能 |
|---|---|
| 0 | 分配内存 |
| 1 | 写内存 |
| 2 | 释放内存 |
| 3 | 创建线程 |
| 4 | APC 排队 |
| 5 | 统计 |

### 页表分析（IOCTL 0x83A，5 op）

| op | 功能 |
|---|---|
| 0 | CR3 解码 |
| 1 | 页表树遍历 |
| 2 | 范围映射 |
| 3 | 页清单 |
| 4 | 统计 |

### 窗口防截图（IOCTL 0x83B，4 op）

| op | 功能 |
|---|---|
| 0 | 设置 DisplayAffinity |
| 1 | 查询 DisplayAffinity |
| 2 | 统计 |
| 3 | 窗口消息注入 |

### 无痕通信（CPUID 门铃，8 op）

PING / HEALTH / READ / STATS / MEM_READ / MEM_WRITE / PHYS_READ / PHYS_WRITE

### 其他能力

- **原生结构体定义**：yghv_nt.h（偏移表 + 访问器）
- **编译期字符串加密**：yghv_str.h + gen_strtab.py
- **未导出函数特征码定位**：yghv_sig.c（含 yghv_sig_export_of 按名解析任意模块导出）
- **全局系统偏移初始化**：yghv_offsets.c（自动派生 + 验证）
- **物理地址映射读写**：mem_pa.c
- **影子页 IO 窗口**：mem_engine.c（A/B 双引擎）
- **页表重定向无痕**：mem_pte.c
- **异常拦截型无痕**：mem_bp.c（NPF/INT3/DR 三模式）
- **时间校验**：mem_time.c
- **动态设备名**：随机 8 位 hex 后缀
- **IOCTL 分发**：control_device.c（60 个 case，与 control_ioctl.h 的 60 个定义一一对应）
- **驱动卸载与痕迹清除**：yghv_unload_trace.c + yghv_proc_cleanup
- **驱动级读写**：按 PID 读写字节/整数/浮点/双浮点/字节集/文本
- **强制结束进程**：含受保护进程（先清 Protection 字节）
- **挂起进程**：PsSuspendProcess/PsResumeProcess
- **抢权**：令牌偷取 + 令牌提权
- **CR3 解密/页表分析**：yghv_pt.c
- **进程保护**：ObRegisterCallbacks 句柄降权
- **进程管控**：伪装/隐藏/提权/句柄降权/线程隐藏
- **无痕注入**：alloc+write+thread/APC
- **远程 DLL 注入**：alloc+write(path)+thread(LoadLibraryW,path)
- **APC 注入**：KeInitializeApc+KeInsertQueueApc（带属主校验）
- **窗口消息注入**：NtUserPostMessage
- **shellcode 注入**：alloc+write(bytes)+thread(bytes)
- **无痕通信**：CPUID 门铃 + 四内存原语
- **模块隐藏**：三链表摘除（InLoad/InMemory/InInitialization）
- **回调隐藏**：OB 回调从枚举链表摘除
- **回调去签名化**：altitude 327681（无已知样本）

---

## 三、架构要点

### 模块布局（hv/ 目录：45 个 .c/.cpp/.h/.hpp + 3 个 .S/.asm = 48 个文件）

| 模块 | 文件 | 职责 |
|---|---|---|
| 核心 | svm_simplevm206.cpp | VMEXIT 处理（真路径） |
| 偏移表 | yghv_offsets.c | 自动派生 EPROCESS/ETHREAD 偏移 |
| 字符串 | yghv_str.h + yghv_strtab.h | 编译期加密 |
| 特征码 | yghv_sig.c | 未导出函数定位 + 导出解析 |
| 进程控制 | yghv_proc.c | 15 op 分发 |
| 注入 | yghv_inject.c | 6 op 分发 |
| 页表 | yghv_pt.c | CR3/树/范围/页清单 |
| 窗口 | yghv_win.c | DisplayAffinity + 消息注入 |
| 句柄降权 | yghv_obguard.c | ObRegisterCallbacks |
| 无痕通信 | yghv_channel.c | CPUID 门铃 |
| 物理读写 | mem_pa.c | 物理地址映射 |
| 影子页 | mem_engine.c | A/B 双引擎 |
| 页表重定向 | mem_pte.c | 无痕映射 |
| 断点 | mem_bp.c | NPF/INT3/DR |
| 安全解引用 | yghv_safe.c | **唯一**允许解引用未验证指针的地方（9.608） |
| 卸载痕迹 | yghv_unload_trace.c | 痕迹清除 |

### 关键设计约束

1. **VIRTUAL_PROCESSOR_DATA 不许加成员**（9.577 硬约束）：HostStackLimit 带 DECLSPEC_ALIGN(PAGE_SIZE)，任何增长把 GuestVmcb/HostVmcb 后移一页，asm 用固定偏移寻址。
2. **METHOD_BUFFERED 单缓冲别名**：dispatch 必须先拷出请求再 memset out。
3. **偏移表 fail-closed**：off==0 返回 NULL，所有访问器安全失败。
4. **字符串不进镜像**：所有导出名走 strtab 解密。
5. **IRQL 全 PASSIVE**：所有 IOCTL op 在 PASSIVE_LEVEL 执行。
6. **未验证指针必须走 yghv_safe.c**（9.608）：`>= 0xFFFF800000000000` 只说明"属于内核高半区"，
   **不说明已映射**。两次 0x50 蓝屏都源于把前者当成了后者。禁止再写裸解引用。

---

## 三·五、关键代码位置

文档里的每一处 `file.ext:NNN` 都由 `tests/source_citation_check.ps1` 机械校验：
文件存在、行号在范围内、该行非空。改代码挪了行，这里会红。

| 要找什么 | 位置 |
|---|---|
| 偏移表结构与 magic | `common/yghv_nt.h:53`（`YGHV_OFF_MAGIC` 在 `common/yghv_nt.h:46`）|
| IOCTL 总派发 | `control_device.c:628` |
| 偏移派生（按导出名反汇编取 disp）| `yghv_offsets.c:89` |
| 线程链表探针（唯一性判据）| `yghv_proc.c:165` |
| 令牌探针（EX_FAST_REF 低 4 位）| `yghv_proc.c:255` |
| 注入：分配 | `yghv_inject.c:156` |
| 页表：逐层树 + 交叉校验 | `yghv_pt.c:82` |
| 窗口：初始化（解析 win32kfull 导出）| `yghv_win.c:70` |
| 句柄降权：注册 | `yghv_obguard.c:79` |
| 无痕通信：CPUID 门铃 | `yghv_channel.c:96` |

---
## 四、构建与部署

### 构建

```bash
cd YuanGuardHV
set YGHV_BAREMETAL_STEP=206
set YGHV_206B_COEXIST=1
set YGHV_206B_GNPT=1
build.bat
```

产物：`bin/yuanguard_hv.sys`（已签名）+ `bin/yuanguard_hv.map`

### 部署

```bash
copy bin\yuanguard_hv.sys C:\yghv_pa.sys
sc start yghvpa
```

### 测试

```bash
powershell -NoProfile -ExecutionPolicy Bypass -File tests/run_source_checks.ps1
```

18 个检查器，覆盖：IOCTL 一致性、命令一致性、结构体布局、字符串加密、EOL、源引用、IRQL 契约、缓冲区边界等。

---

## 五、客户端

### PowerShell（86 条命令）

```bash
YuanGuardHV/tools/yghv_ctl.ps1 proc snapshot
YuanGuardHV/tools/yghv_ctl.ps1 proc query <pid>
YuanGuardHV/tools/yghv_ctl.ps1 inject alloc <pid> <size>
YuanGuardHV/tools/yghv_ctl.ps1 pt <pid> tree <va>
YuanGuardHV/tools/yghv_ctl.ps1 win <pid> set <hwnd> [affinity]
```

### Java（18 条命令）

```bash
YuanGuardHV/tools/yghv_client/run.bat state
YuanGuardHV/tools/yghv_client/run.bat mem-read <pid> <va>
```

---

## 五·五、上机实测（2026-11-14，首次）

**v0.4.3 的镜像第一次在真机上跑通。** 本节记录的是实测数据，不是构建结论。

| 项 | 结果 |
|---|---|
| 加载 | 成功，服务 `Running` |
| 虚拟化 | `HypervisorPresent = True`（12 逻辑核） |
| 持续运行 | 3 小时以上，零蓝屏 |
| 卸载 | 6.94 s，**`invmrun at unload=0x0`**，卸载后 `HypervisorPresent=False` |

**实测中被降级为「明确拒绝」的两个 op**

`op9`（进程摘链隐藏）与 `op14`（线程摘链隐藏）**默认返回 `STATUS_NOT_IMPLEMENTED`**，
实现保留在 `YGHV_ALLOW_PROC_HIDE` / `YGHV_ALLOW_THREAD_HIDE` 之后供实验使用。

依据是 5 次蓝屏，每次都能定位到一个具体判据：

| BugCheck | 栈上的证据 | 原因 |
|---|---|---|
| `0x139` subcode 3 ×3 | `nt!KeTerminateThread` / `PspExitProcess` | 摘链时 `node->Flink = node` **自链接**；被隐藏的进程/线程退出时，内核自己的 teardown 走过该节点，判定 `CORRUPT_LIST_ENTRY` |
| `0x18` ×2 | `nt!ObfReferenceObjectWithTag` → `CcWriteBehindInternal` | 保存裸指针后**立即释放引用**，是 use-after-free；池内存被 `FILE_OBJECT` 复用后，几分钟后在缓存写回时才爆 |

两者是**同一条路径上的两个独立缺陷**：先修引用计数只是让自链接成为第一个引爆的。
**活着的 EPROCESS/ETHREAD 不能保持摘链状态** —— 退出路径要求它可达，这不是补丁能改的。

> 项目在 ISSUES.md 里已为 EPROCESS 记过同一结论（「摘链的代价大于它关掉的窗口」）。
> 9.507 对 `mem-bp dr` 用的也是这个模式：能报告「未实现」的功能是可见的，
> 在例行线程退出时崩机的功能不是。

**能力面实测（IOCTL，真实调用）**

| 家族 | op | 实测结果 |
|---|---|---|
| **proc 0x838** | 0 快照 | ✅ **120 个进程**，pid/ppid/prot/type/signer/base 全部合理 |
| | 1 查询 | ✅ pid 与映像名正确（`powershell.exe`）|
| | 2 保护级别 | ✅ |
| | 3 挂起/恢复 | ✅ `q0=1`，返回线程 id |
| | 4 结束进程 | ✅ 对存活目标 |
| | 5 偏移诊断 | ✅ `derived_mask=0x2FBF`（14/14 派生）|
| | 6 特征码诊断 | ✅ |
| | 7 线程枚举 | ✅ **29 个线程**（`TLIST` 修复后，见下）|
| | 8 令牌偷取 | ✅ 返回两个内核指针 |
| | 9 进程隐藏 | ⛔ **默认拒绝**（见上节）|
| | 10 句柄降权 | XX 派发正确，但注册被系统拒绝 0xC0000022（见下）|
| | 11 句柄降权诊断 | ✅ |
| | 12 伪装映像名 | ✅ |
| | 13 令牌提权 | ✅ 返回启用特权数 `0x39E846` |
| | 14 线程隐藏 | ⛔ **默认拒绝**（见上节）|
| **inject 0x839** | 0 分配 / 1 写 / 2 释放 / 5 统计 | ✅ 全链路：va=0x1BCC31E0000 → write → free |
| **pt 0x83A** | 0 CR3 / 2 范围映射 / 3 页清单 / 4 统计 | ✅ 统计返回 6 次查询 |
| **win 0x83B** | 0–3 全部 | ✅ 派发正确；`win32kfull` 无交互会话时按设计拒绝 |
| **sys-health 0x825** | — | ✅ `degraded=0 total=0 fatal=0` |


### 新增检查器：`canonical_deref_check.ps1`

**"canonical" 不是 "mapped"。** 同一类缺陷在本轮造成两次 `0x50`，第三次
（`yghv_obguard.c` 摘除 OB 回调）是靠**人读源码**发现的 —— 这正是本项目反复
吃亏的"没有任何检查观察到它"的形状。现在它是检查器。

判据：一次 `0xFFFF800000000000` 比较，其后 6 行内有**裸解引用**，且窗口内没有
`__try` / `MmIsAddressValid` / `yghv_safe_*` / `yghv_p_read64` / `yghv_p_write64`。

豁免三类，都是真的不该报：
- 只用来**选路径**不解引用（`mem_engine.c` 选引擎）
- 比较本身在 `__try` 内
- 比较守的是受保护的读取

单个位置可写 `/* canonical-ok: <理由> */` 就地豁免，理由写在发生处，
而不是为所有人放宽规则。

**已验证它能失败**：往 `yghv_obguard.c` 注入一个"比较后裸解引用"的函数，
检查器返回 exit 1；恢复后回到 exit 0。

审计结果：16 处比较，**真正裸露的只有 `obguard` 一处**（本轮已修），
其余有 `__try` 或走 `yghv_safe` 层。


### 剩余 op 的补测结果（本轮完成）

| op | 结果 | 说明 |
|---|---|---|
| `pt op4` 统计 | ✅ `q0=0x6 q1=0x1` | 6 次 CR3 查询。**之前报 `ok=False` 是测试脚本传参错误**，不是缺陷 |
| `inject op0` 分配 | ✅ `va=0x1BCC31E0000` | 在 notepad 里真实分配 |
| `inject op1` 写内存 | ✅ `err=0` | 写入 8 字节 |
| `inject op2` 释放 | ✅ `err=0` | |
| `inject op5` 统计 | ✅ `allocs=1 writes=1` | 计数器与实际操作一致 |

**`inject` 的 alloc → write → free 全链路已验证**，这是 9.592 接入后第一次端到端跑通。

仍未端到端验证的：`inject op3`（创建线程）与 `op4`（APC 排队）——两者都需要一个
可控的靶程序来观察执行效果，本轮没有准备。它们在派发层是可达的（同一 switch），
但没有"写入的代码确实被执行"这种证据。

### P1-1 `mem-bp dr`：**机制上不工作**，且现在可测

这是本轮最重要的发现。之前 P1-1 的记录是「代码完成、未上机」；实测结论是
**代码完成，但机制在本机不可能工作**。

观测链（每个数字现在都由驱动打印）：

```
mem-bp dr <va> w     -> OK          请求被接受
count=1 dr=1                       槽位已占用
hits=0                             靶进程写了那个页约 200 次，一次没命中
dr7: cores=0 unresolved=12         没有任何核的 VMCB 收到 Dr7
walk: sawProcessors=12 indexFailures=0
```

**最后两行同时成立，才说明问题**：走核机制跑遍了 12 个处理器且没中途退出，
所以回调确实执行了 12 次；但每一次都走了「CPUID 握手没应答」分支。

根因：`SvDrApplyOnCore` 用 `CPUID_FAKE_VPD` 取自己的 VPD 指针，而这个叶只在
`SvHandleVmExit` 里应答，**也就是只在 guest 态**。`SvExecuteOnEachProcessor`
的回调跑在 **host 态**，CPUID 直接由硬件执行，ECX 不是 `'SSV1'`，调用方正确拒绝。

这不是重试能修的竞态 —— **回调所在的上下文结构上就无法问这个问题**。

> 同一模式还有两处：`SvTscApplyOnCore`、`SvFakeNcr3Resync`，用同一个叶、同样写法。
> VPD 探针（op 8）在加走核计数器之前返回 `hits=0 misses=0`（12 个处理器），
> 这个组合在回调跑过的情况下不可能出现 —— 就是它把我引到这里的。

**正确做法**（本轮未做）：每个核的 VPD 指针改由「虚拟化时填好的按核表」提供，
回调里用 `KeGetCurrentProcessorNumber()` 索引，不走 CPUID。这是对 vendored 入口路径
的真实改动，且需要一台能承受蓝屏的机器来验证 —— **不该在无法验证时猜着改**。

在修好之前，`dr OK` 不再会被误读成「已武装」：诊断会明确显示 `cores=0`。
### op7 / op14（线程枚举与线程隐藏）：`TLIST` 探针已修

**这是从 9.591 起就从未工作过的功能。** 探针有两个未知量互相依赖，而代码试图
按顺序解：先用 ETHREAD 定位 `ThreadListEntry`，再用它走进程线程链。

三个独立缺陷，每一个都足以让它永远失败：

| 缺陷 | 证据 | 后果 |
|---|---|---|
| 第一阶段的唯一性要求 | `hits=15`（实测）| `hits != 1` 直接拒绝，**第二阶段从未执行过** |
| 15 个候选里保留的是最后一个 | `entry = off` 每次覆盖 | 留下的是 `0x640`，不是真值 |
| 保留的值从未被验证 | 第二阶段用错的 entry 走链 | `probe1 ok=0 seen=0`，第一个节点就失败 |

**15 个候选是正常的，不是危险信号**：一个 ETHREAD 有很多 `LIST_ENTRY` 字段
（线程链表、两个 APC 队列、互斥与 IRP 链、rundown 链），每一个都内部自洽，
都能通过双向链表检查。要求恰好一个，等于要求这个结构只有一条链。

修法：把两个未知量当**联合搜索**——收集全部候选，然后对每个
`(entryCand, headOff)` 组合走一遍完整链，接受唯一通过的那个。

结果：`derived_mask` 从 `0x2FBF` → **`0x3FBF`**，`op7` 返回 **29 个线程**。

> 与 `ActiveProcessLinks` 完全同型：**唯一性要求加在了错误的阶段上**，
> 于是一个正确的答案被否决了几个月。
### op10 句柄降权：`ObRegisterCallbacks` 被系统拒绝（已穷尽排查）

`op10` 派发正确，`ObRegisterCallbacks` 返回 **`0xC0000022 STATUS_ACCESS_DENIED`**。
现在该 op 会把这个状态码回给客户端（`o[1]`），不再只是一个无法解读的 0。

逐项排除，每项都有实测数据：

| 项 | 实测值 | 结论 |
|---|---|---|
| ObjectType | `fffff803364fc410` | 与 `PsProcessType` 的值**逐位相同** → 解引用正确 |
| altitude | 试过 `321000` 与 `327681` | 状态**完全相同** → 不是字符串问题 |
| 调用上下文 | `pid=4`（DriverEntry）与 `pid=8500`（IOCTL 调用者）| **完全相同** → 不是调用者问题 |
| 结构尺寸 | `regbuf 0x28`、`opbuf 0x20` | 与结构定义精确匹配 |
| Version / Operations / PreOperation | `0x0100` / `0x0003` / 非空 | 全部正确 |

剩余解释只有镜像完整性：签名者 `CN=YuanGuardHV Test` **不在本机受信任根存储中**
（本机只有 `CN=YuanGuardTest`），`AuthenticodeStatus = UnknownError`。
Windows 对签名链不通过的驱动拒绝对象管理器注册路径。

> **这是环境限制，不是代码缺陷** —— 但必须如实报告。
> 与 9.507（`mem-bp dr` 返回 REFUSED）、两个摘链 op 同一条纪律：
> **做不到的能力必须说出来，不能返回一个看不出原因的成功。**

**实测中修掉的五个真缺陷**（每个都有 dump 或计数证据，见 CHANGELOG 9.608）：

1. 两次 `0x50` 蓝屏 —— "canonical" 被当成 "mapped" → 新增 `hv/yghv_safe.c`
2. `ActiveProcessLinks` 从未派生成功 —— 闭环判据 `prev == head` **永远为假**
3. `proc` 快照返回 0 进程 —— 我用宽松验收时**丢了 `good >= 16` 下限**，空链表抢先命中 `0x158`
4. 快照只回 header —— METHOD_BUFFERED 的 `IoStatus.Information` 报 32 字节，120 条 entry 全被丢弃
5. `win` 全部返回 error 1 —— `case IOCTL_YGHV_WIN` 被写进了 **CACHE 的内层 switch**

> **这一节的教训**：前四个都不是"代码写错了"，而是**判据观察不到它要观察的事件**。
> 项目里 9.466 那条纪律在这里第四次生效：先建观测，再改代码。

---

## 六、已知限制

| 项目 | 状态 | 说明 |
|---|---|---|
| P1-1 mem-bp dr | 未上机 | 9.588 已接通硬件（DR0–3 + 每核 VMCB Dr7 + #DB 回调），代码完成，需实测 |
| P1-4 进程控制 15 op | 未上机 | 全部编译通过，需实测 |
| P1-5 注入原语 6 op | 未上机 | 全部编译通过，需实测 |
| P1-7 窗口防截图 | 未上机 | win32kfull 导出解析就绪，需实测 |
| P1-8 伪装/线程隐藏/令牌提权 | 未上机 | 全部编译通过，需实测 |
| TOKEN 偏移 | 推断 | 0x38/0x40 基于 Win10 x64 实测推断 |
| 线程隐藏 | 部分 | 只摘进程线程链表，不摘 session/全局 |
| 1h 挂机 | 未测 | P2-1，需重启 |

---

## 七、版本历史（0.4.x 系列）

| 版本 | 提交 | 主要变更 |
|---|---|---|
| v0.4.0 | `64b8499` | 全核真 guest 成为默认 |
| v0.4.1 | `41fed59` | RealGuestMode 覆盖可见化 + 真 guest 下重测 mem-bench |
| v0.4.2 | `38b7410` | P1-3 全部结案 + 错误码门 + 文档侧修复 |
| v0.4.3 | `f747091` | **本版本**：完整能力面（16 项 + 9 超越）；含 9.588 的 P1-1 `mem-bp dr` 真修 |

> **提交号取各 tag 实际指向的 commit**（`git rev-parse <tag>^{commit}`）。
> P1-1 `mem-bp dr` 的硬件接通是 9.588，落在 v0.4.3 的范围里，不是 v0.4.1。

---

## 八、下一步

1. **上机验证**：部署 v0.4.3 镜像，逐项验证 15 个 proc op、6 个 inject op、5 个 pt op、4 个 win op。
2. **P1-1 mem-bp dr 上机**：验证 DR 断点路径。
3. **全核挂机 1h**：验证长期稳定性。
4. **性能回归**：G25 功能验证 17/17。

---

## 九、文档索引

| 文档 | 用途 |
|---|---|
| **本文档** | 0.4.3 交接文档（主文档） |
| ISSUES.md | 问题清单 |
| QUICKSTART.md | 快速开始 |
| archive/ | 历史文档归档 |
