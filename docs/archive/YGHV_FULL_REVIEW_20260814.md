# YuanGuardHV 全面审查报告 + 任务修复清单（2026-08-14）

> 范围：仓库根 D:\yuanguard，HEAD `c37dd2e`（docs 9.135）。
> 方法：本人通读全部正式构建源码（main.c 4090 行 / protect.c 1298 行 /
> svm_core.c / svm_trampoline.S / vmexit.c / npt_core.c / vmmcall.c /
> control_device.c / multi_core.c / loader_stealth.c + 全部 common 头文件），
> 5 个独立模块子代理并行深审并逐条核实，构建验证 + 文档-代码核对。
> 只读审查，未修改任何代码；本机有硬冻结史，未实机加载驱动。

## 0. 结论

- 默认构建路径 **无 CRITICAL 级问题**；架构与关键机制（VMCB 布局、trampoline、
  NPT 数学、IOCTL 面、认证分层、结构体 ABI）经验证正确。
- 共记录 **9 项 MAJOR / 23 项 MINOR / 16 项 INFO-NIT**，逐条见第 3 节；
  任务修复清单见第 4 节。
- 2026-08-14 增补（protect 层独立子代理完成，经本人逐条对照源码核实）：
  新增 REV-036/037/038 三项 MAJOR 与 REV-039..045 七项 MINOR 与
  REV-046..048 三项 INFO-NIT。
- 构建验证：`cmd /c build.bat` → 静态校验 PASS → 编译/链接/签名 SUCCESS，
  产物 SHA256 `F411C929C1114773BEBB6B7C541B82607AA3CB0A5FB051D7C733200C3A0CC106`
  （79,232 字节，含关键标记字符串）。

## 1. 验证正确清单（非缺陷，审查确认无误）

| # | 项 | 结论 |
|---|---|---|
| OK-1 | VMCB 布局 | 与 AMD APM 完全一致；`vmcb.h` `_Static_assert` 覆盖控制区/状态区全部关键偏移（exitcode 0x70 / rip 0x178 / rsp 0x1D8 / rax 0x1F8 / g_pat 0x268 / cpl 0xCB 等） |
| OK-2 | trampoline 汇编 | push/pop 完全对称；VMEXIT 后 `[rsp+0x10]` 取 vcpu 正确（9.9 修复在案）；`stgi` 在 guest GPR 落盘后执行；VMLOAD/VMSAVE 顺序正确；vcpu 指针 64 位传递（`mov ecx,edi` 截断 bug 已修） |
| OK-3 | vcpu 结构偏移 | `svm_vcpu.h` static_assert 全覆盖（regs 0x78 / hsave_pa 0x28 / host_stack_top 0x38 / vmcb_pa 0x08）；汇编引用偏移全部一致 |
| OK-4 | NPT 数学 | `npt_translate` 4K pfn bit12..51 / 2MB pfn bit21..51 正确（历史 2MB PFN bug 已修）；`npt_split_2mb_to_4kb` 正确（common 掩码排除 PS 位）；NPF 错误码→#PF EC 解码正确；RIP 推进规则正确（NPF/#DB/INTR 不推进） |
| OK-5 | IOCTL 面 | 15 个 IOCTL（0x800..0x80E）函数号与完整编码在 C/PS/JNI/Java 四方一致；METHOD_BUFFERED + FILE_ANY_ACCESS 正确；长度校验在解引用前完成，内核侧无越界；未知码 fail-closed |
| OK-6 | owner 绑定 | CREATE 捕获 EPROCESS+CR3（DirectoryTableBase @0x028 正确）并 ObReference；DEVICE_CONTROL 校验请求者==owner + 当前 CR3==owner CR3 + SeDebug；CLEANUP 正确释放；无 pending-IRP 竞态 |
| OK-7 | VMMCALL 认证 | open/cookie/key 三级与文档一致；未知命令 fail-closed；SET_TARGET 自设目标校验 |
| OK-8 | 结构体 ABI | 12 个结构体内核↔Java 布局完全一致（含 1544B pages_info、144B install_hook、WCHAR name 63 字符上限一致） |
| OK-9 | 客户端 | SeDebug 启用正确（含 ERROR_NOT_ALL_ASSIGNED 检查）；JNI 缓冲有界；命令数 PS 20 / Java 18 与 README 精确一致 |
| OK-10 | 静态校验 | 真实有效且 fail-closed，已接入 build.bat（编译前失败即中止） |
| OK-11 | 默认宏 | R1_NPT_UNIT_TEST=1、RESIDENT_WORKLOAD_TEST=1；其余（BAREMETAL_STEP=0 / R1_EXCLUDE_PRIVATE=0 / REAL_HOOK_TEST=0 / HOOK_RENDEZVOUS_TEST=0 / LOADER_STEALTH=0 / UNLOAD_GUARD=0）全部门控默认关，与文档一致 |
| OK-12 | 文档状态 | HEAD c37dd2e = 9.135，工作区干净；多数文档声明与代码一致 |

## 2. 审查方法说明

- 本人一手精读：全部正式构建源码 + 全部 common 头文件。
- 子代理交叉：SVM 核心 / NPT+vmexit / 控制面 / 客户端+测试 / protect 层 五路并行，
  报告逐条对照源码核实（修正 2 处误报：INTR 拦截"惰性"、exit-test 不可用——
  后者漏看 main.c:4075 `g_persistent_mode=TRUE`）。
- 本机实机加载驱动存在历史硬冻结风险（9.118/9.125），本次审查未加载。

## 3. 问题清单（挨个记录）

### 3.1 MAJOR（9 项）

#### YGHV-REV-001【MAJOR】持 g_protect_lock 跨全核 pause 死锁（已知）
- 位置：`hv/protect.c:1139`（install_hook_locked）、`protect.c:1254`（remove_hook_locked）、`hv/svm_core.c:760-790`（pause/resume）
- 问题：install/remove/clear 在持 `g_protect_lock` 时调用 `svm_core_pause_residents_for_patch`；常驻核的 HEARTBEAT（vmmcall.c:71→protect.c:550）与 NPF 写路径（protect.c:497）取同一把锁。核心若已在 dispatch 内等锁，永不回到循环置 `pause_ack` → AB-BA 死锁窗口 → 本机观测为整机硬冻结，`INSTALL_HOOK/REMOVE_HOOK/CLEAR` 在本机不可用。
- 影响：hook 路径实机硬冻结根因（文档 9.118/9.125 已记录）；本机已停止该族实验。
- 建议修复：pause 移出锁外（pause 是"无核心在 guest 上下文"的屏障），再取锁修改；独立 patch 互斥（owner+generation）；保留超时但超时必须完整 resume 并返回 STATUS_TIMEOUT。详细方案见 `docs/YGHV_HOOK_LOCK_AND_0x5AA_REDESIGN_20260813.md`。
- 验证：需换平台或硬件调试器；先单 hook 回归 → 并发 install+NPF workload → 4 目标槽满场景。
- 状态：待处理（换平台）

#### YGHV-REV-002【MAJOR】npt_set_page_perm_range 语义错误
- 位置：`hv/npt_core.c:143-152`
- 问题：以 2MB 步进遍历。对已拆分（4K）区域只改每槽**首个 4K 页**，漏掉其余 511 页；对未拆分大页把权限**过度应用到整 2MB**（超出请求范围）。
- 影响：当前仅 r1 单元测试使用（main.c:392-412），测试只断言首页，掩盖了错误；若未来用于真实保护会产生错误授权/误拦截。
- 建议修复：按 4K 页迭代（对齐起点、逐 4K 步进）。
- 验证：改后重跑 r1 单测（应仍 PASS 且范围断言更精确）；可加"拆分区域 range 只改范围内 4K 页"断言。
- 状态：待处理

#### YGHV-REV-003【MAJOR】svm_core_stop_all_residents 不唤醒暂停中的 resident
- 位置：`hv/svm_core.c:749-758`
- 问题：若某核正停在 pause 的 `KeWaitForSingleObject(&resume_event, ..., NULL)`（svm_core.c:642，无限等待），stop 只置 STOPPING、不 `KeSetEvent(resume_event)` → 该核永不醒来 → `svm_core_wait_all_stopped` 卸载死锁。
- 影响：与 REV-001 的 pause 路径重叠时卸载挂死；触发条件为卸载与 install/remove/clear 的 pause 重叠。
- 建议修复：stop 路径对 `pause_requested` 的 vcpu 补 `KeSetEvent(resume_event)`（一行级低风险）。
- 验证：本机非 hook 路径（卸载时无 pause 在途）可安全验证不回归。
- 状态：待处理（低风险可先修）

#### YGHV-REV-004【MAJOR】GET_PAGES returned 可超过实际写入条数 → 客户端越界读
- 位置：`hv/protect.c:603`（`info->returned = total` 跨目标累加，最多 4×64=256）；客户端 `tools/yghv_ctl.ps1:267-277`、`tools/yghv_client/YghvCtl.java:121-131`
- 问题：`get_pages_info` 只写 `min(count,64)` 条到 1544B 缓冲，但 `returned` 可到 256；客户端按 `for(i<returned)` 循环读取 24B 条目 → PS `ArgumentOutOfRangeException`、Java `BufferUnderflowException`。内核侧写入有界安全，是客户端堆越界读。
- 影响：多目标且页数>64 时客户端崩溃（selftest 只用 1 目标 1 页未暴露）。
- 建议修复：驱动 `returned = min(total, cap)`；客户端同步 `for(i<min(returned,count))`。
- 验证：构造 2 目标各 40 页后 `list-pages`。
- 状态：待处理（低风险可先修）

#### YGHV-REV-005【MAJOR】yghv_trace_u64 栈缓冲区溢出（潜在）
- 位置：`hv/main.c:159-175`
- 问题：`char buf[64]`，label 最长拷到 63 字符，随后写 `=`+`0x`+16 位 hex+NUL → 最多 82 字节写入 64 字节栈缓冲。
- 影响：当前全部调用点 label 最长 23 字符（已统计），今日不可达；属潜在栈破坏。该函数跨模块导出（vmexit.c/control_device.c 均调用）。
- 建议修复：label 截断到 `sizeof(buf)-20`，或改用更大缓冲/堆。
- 验证：构造长 label 单测（或静态检查）。
- 状态：待处理（低风险可先修）

#### YGHV-REV-006【MAJOR】yghv_protect_check_target_exited 进程对象 UAF 竞态
- 位置：`hv/protect.c:712-731`
- 问题：锁内捕获裸 `EPROCESS` 指针（未 `ObReferenceObject`）后放锁，再对捕获指针 `KeWaitForSingleObject`。该函数在**每个常驻核** HEARTBEAT 每 10000 次退出调用；若核 B 的 `on_target_exit` 在核 A 捕获与等待之间对目标 `ObDereferenceObject`（最后一个引用）→ EPROCESS 释放 → 核 A 等待已释放对象 = UAF。
- 影响：多核常驻 + 目标进程在窗口内退出时低概率内存损坏。
- 建议修复：锁内 `ObReferenceObject` 每个捕获进程，等待循环后 `ObDereferenceObject`；
  另：若捕获后目标被重设（pid 复用），按旧 pid 调 `on_target_exit` 可能误清新目标——
  清除前应再校验槽的 pid/cr3 仍与捕获一致。
- 验证：多核持久模式下目标退出循环回归（exit-test 多轮）。
- 状态：待处理

#### YGHV-REV-036【MAJOR】yghv_hook_diag IRQL 违规（ZwCreateFile 在 APC_LEVEL）
- 位置：`hv/protect.c:20-47`；全部调用点在 `install_hook_locked`/`remove_hook_locked`
  （protect.c:1135/1142/1155/1161/1171/1178/1187/1198/1240/1249/1258）
- 问题：`yghv_hook_diag` 执行 `ZwCreateFile/Write/FlushBuffersFile`，而每个调用点
  都在持 `g_protect_lock`（FAST_MUTEX，取锁将 IRQL 提升到 APC_LEVEL）的锁内；
  `ZwCreateFile` 要求 PASSIVE_LEVEL → IRQL 违规，恰好在它要诊断的错误路径上可能蓝屏。
- 影响：hook 安装/移除失败路径（本机 9.125 已观察到该路径硬冻结）若先触发 IRQL 违规，
  诊断日志本身不可靠且可能叠加蓝屏。
- 建议修复：hook_diag 先放锁（或调用点改为收集 stage 后统一在锁外落盘），或在 PASSIVE 线程
  落盘。
- 验证：构造 install 失败场景确认 yghv_hook.log 正常且无 IRQL 蓝屏。
- 状态：待处理

#### YGHV-REV-037【MAJOR】x86-64 指令长度解码器误解码 → 错误补丁边界
- 位置：`hv/protect.c:900-903`（B8-BF 恒 imm64）、`protect.c:947-961`（0F 38/3A
  modrm 偏移差一）、`protect.c:981-992`（F6/F7 恒假设立即数）
- 问题：
  - `B8-BF`（mov r32,imm32，无 REX）被恒判为 imm64（9 字节而非 5 字节）——真实
    Zw*/Nt* `mov eax, imm32` 桩会命中；当前仅因 19045 上 Zw* 入口经
    `KiServiceLinkage`（非 B8 开头）而未被触发；
  - `0F 38/3A` 的 modrm 解码从 op3 字节开始（差一），且 RIP-relative 检测看错字节；
  - `F6/F7` 的立即数取决于 modrm.reg（`/0 test` 有立即数，`/2 not`、`/3 neg`、
    `/4 mul` 等无），代码恒假设有立即数 → 多算长度。
- 影响：对一般函数目标产生错误补丁边界 → 复制的原始字节不完整/跳回地址错位 →
  guest 崩溃或 hook 行为异常。dummy hook（NOP+ret）不受影响，故默认构建测试未暴露。
- 建议修复：按 REX.W 区分 B8-BF imm32/imm64；0F 38/3A 从 op3 后一字节起解 modrm；
  F6/F7 按 modrm.reg 判定立即数。
- 验证：对真实 Zw*/Nt* 与含 0F 38/3A、F6/F7 指令的目标跑 `validate_hook_target` 单测。
- 状态：待处理（建议换平台/kd 实机验证前先在 DriverEntry 内做负向/正向单测）

#### YGHV-REV-038【MAJOR】remove_hook pause 超时后 hook 永久残留
- 位置：`hv/protect.c:1245-1261`
- 问题：remove 先 `remove_page_for_locked`（把函数页从 NPF 策略移除、恢复可写），
  再 `pause_residents_for_patch`；若 pause 超时返回，函数**不恢复原始字节**、不清
  `h->installed`、页已移出策略 → 函数仍被补丁跳到 stub；重试时
  `remove_page_for_locked` 返回 NOT_FOUND 直接退出，永远不会恢复字节 → hook 残留 +
  stub 泄漏 + 后续 cleanup 释放 stub 后函数指向已释放内存（悬垂）。
- 影响：hook 路径错误场景下系统函数被永久改写（崩溃/语义错误）。
- 建议修复：pause 失败也必须恢复原始字节并清 `h->installed`；或先 pause 成功再移页。
- 验证：换平台后构造 pause 超时场景。
- 状态：待处理（与 REV-001 同属 hook 路径，换平台后一并验证）

### 3.2 MINOR（23 项）

| ID | 位置 | 问题 | 建议 |
|---|---|---|---|
| YGHV-REV-007 | `vmmcall.c:36-39,100-120` + `main.c:3529-3536` | STOP_INTERNAL/SHUTDOWN 仅 cookie 认证；cookie 预置进 guest RCX 且由 `KeQuerySystemTime^KeQueryTickCount` 派生，任何 ring-0 guest 可推导 → 可整体停掉 hypervisor。当前无不可信 guest（合成 guest 均为驱动自写），实际暴露为零 | SHUTDOWN 改走 `r8==auth_key` 分支；cookie 派生加强 |
| YGHV-REV-008 | `vmexit.c:615-617` | `svm_handle_msr` 对非 VM_CR MSR 把宿主机 MSR 值写入 guest RAX/RDX（语义错误）。潜在：默认 MSRPM 仅拦截 VM_CR | 非 VM_CR 分支按 read/write 正确模拟或 fail-closed |
| YGHV-REV-009 | `vmexit.c:622-626` | `svm_handle_cr` 空操作（仅计数）。潜在：CR 拦截当前未启用 | 启用前先实现模拟或 `return 1` 停机 |
| YGHV-REV-010 | `npt_core.c:42` + `main.c:3639-3647` | `npt_init` pml4 分配失败时 `g_cache` 释放未置 NULL（后续若调 npt_cleanup 则 UAF——当前失败路径不调，不可达）；npt_init/map_ram 失败路径未调 npt_cleanup，泄漏 g_cache+pml4 | 失败路径 `g_cache=NULL`；统一调用 npt_cleanup |
| YGHV-REV-011 | `npt_core.c:160-162` | `npt_translate` 先查 `large_page` 后查 `present`，对 P 位清零的未拆分大页仍返回翻译。潜在：当前清 P 前均先 split | 先查 present |
| YGHV-REV-012 | `control_device.c:116` | `SeSinglePrivilegeCheck(SE_DEBUG_PRIVILEGE, UserMode)` 无 SEH；缺权限时可能 raise 而非返回 FALSE（LOG 行成死代码） | `__try/__except` 包裹或改用 token 显式检查 |
| YGHV-REV-013 | `vmmcall.c:27` vs `58-60` | VMMCALL auth 用全值 `cr3==g_control_cr3`，target_matches_cr3 用 `~0xFFF` 掩码，不一致 | 统一掩码 |
| YGHV-REV-014 | `vmmcall.c` 各控制命令 | 成功返回原生 NTSTATUS、认证失败返回 YGHV_STATUS_DENIED，状态协议不统一 | 统一映射 YGHV_STATUS_* |
| YGHV-REV-015 | `vmmcall.c:157/169` | VMMCALL ADD_PAGE/REMOVE_PAGE 硬编码 targets[0]，与 IOCTL 按 CR3 选槽不一致（VMMCALL 仅驱动自用，风险低） | 与 IOCTL 对齐按 CR3 解析 |
| YGHV-REV-016 | `svm_core.c:471-475` | `svm_core_init` 分配失败路径：vcpu 分配失败时 `g_vcpus[core]=NULL`，cleanup IPI 无法恢复该核 `EFER.SVME` | 失败路径按核恢复 EFER |
| YGHV-REV-017 | `loader_stealth.c:40-57` | 无 LoaderLock 摘链（与并发加载/卸载竞态）；仅摘 `InLoadOrderLinks`、名字仅清指针未清内容（部分隐身）。门控默认关 | 持 LoaderLock；评估完整性；并入默认前需 kd `!driver` 复核 |
| YGHV-REV-018 | `control_device.c:19-50` | 每 IOCTL 同步开/写/刷/关 `yghv_ioctl.log`（每次 2 次），性能开销 + 永久取证痕迹 | 内存缓冲/限频或删除 |
| YGHV-REV-019 | `YghvCtl.java:282-325` + `README.md:28` | `protect <pid>` 仅当 pid==自身才生效（ADD_PAGE 按调用者 CR3 解析），README 未说明 | README 注明"仅能保护自身"或失败提前提示 |
| YGHV-REV-020 | `yghv_ctl.ps1:359/365/444` | PS hex 参数不接受 `0x` 前缀（Java 接受），行为不一致 | PS 剥 `0x` 前缀 |
| YGHV-REV-021 | `yghv_ctl.ps1:466-467` | selftest baseline 读而未断言；中途失败不清理驱动状态（target/页/保护残留） | 断言 baseline+1；finally 恢复 |
| YGHV-REV-022 | `yghv_ctl_jni.c:86/90` | `GetByteArrayElements` 返回值未检查 NULL | 检查后处理 |
| YGHV-REV-039 | `protect.c:1135/1178/1240` | `yghv_hook_diag(..., st)` 中 `st` 未初始化即传入（install:map / install:missing / remove:map），诊断日志记录垃圾 NTSTATUS | 传入前初始化或传 stage 常量 |
| YGHV-REV-040 | `protect.c:1199-1204` | install 失败路径恢复原始字节后未触发 NPT TLB flush（`npt_set_page_perm` 后无 `npt_flush_pending`），且恢复与 resume 存在竞态 | 补 flush；按 REV-001 锁序重构后消除竞态 |
| YGHV-REV-041 | `protect.c:1148` vs `1206-1209` | install 失败路径：先 `svm_core_resume_residents()` 再释放 stub 页，resume 后常驻核可能正执行该 stub → 小窗口 UAF | 先释放 stub 再 resume，或归入统一 lock/barrier |
| YGHV-REV-042 | `protect.c:443-444` | `start_locked` 对 0 页返回 `STATUS_INVALID_PARAMETER`，与 baremetal step 9"0 页 keepalive"用法冲突（门控实验，非默认路径） | 明确 keepalive 语义或修正门控测试 |
| YGHV-REV-043 | `protect.c:1102-1108` + `g_protect.config.deny_status` | 默认 `R1_EXCLUDE_PRIVATE=0` 下 NPT 全 RAM RW，stub 页与 `deny_status` 对 guest **可写**——受陷 guest 可把 deny_status 清零或改 stub 绕过保护（当前无不可信 guest，实际暴露为零，与 REV-007 同威胁模型） | 并入 R1 私有页剔除门控；或对 deny_status/stub 单独 NPT 只读 |
| YGHV-REV-044 | `protect.c:252-266` | `set_target` 切换目标时若中途某页 disarm 失败，返回 `STATUS_UNSUCCESSFUL` 但已 disarm 部分页且未重置 `page_count`，槽保留旧 pid + 混合 armed/disarmed 状态 | disarm 失败回滚或保持槽原状 |
| YGHV-REV-045 | `protect.c:497-527` | `on_npf_write` 不检查 `g_protect.active`：若 `stop()` 失败后仍有页 armed，一次写入即永久 disarm（rearm 才查 active）→ 状态不一致 | NPF 决策前检查 active |

### 3.3 INFO / NIT（16 项）

| ID | 位置 | 说明 |
|---|---|---|
| YGHV-REV-023 | `main.c:197` / `main.c:3522` / `pool/*` / `test_*.c` / `min_drv.c` / `stub.c` / `protect.c:51,97-103,124,172,374` | 死代码：`yghv_prepare_guest_code`、`yghv_make_guest_code_executable`（后者若被调会解引用 NULL `g_guest_code_page`）；pool/test/min_drv/stub 不进入正式构建；`g_protect_cr3_list` 只写不读（hook stub 已改 unrolled 遍历，不再依赖）、`yghv_protect_guest_va_to_pa`/`yghv_protect_find_page`/`yghv_protect_resolve_va` 无调用者（find_page 公共包装还返回锁外指针，若将来有调用者属不安全模式） |
| YGHV-REV-024 | `main.c:3548-3549` | 重复 `if (g_guest_code_page)`（无害） |
| YGHV-REV-025 | `svm_core.c:6` | 硬编码 `#define YGHV_DEBUG_LOG`（与 build.bat `/DYGHV_DEBUG_LOG` 重复，已知警告） |
| YGHV-REV-026 | `debug.h:6` + `loader_stealth.c` + `svm_core.c:571` | `YGHV_TAG 'vhGY'` 注释"change to common pool tag before release"；`ponytail:` 代号残留 |
| YGHV-REV-027 | `docs/YUANMOD_HANDOFF_CURRENT.md` 9.125 + `tests/safety_checks.ps1` | 交接文档曾把 0x5AA 误标为 ERROR_BUSY（2026-08-14 已改正）；safety_checks 检查清单原未含该文档（已纳入） |
| YGHV-REV-028 | `control_plane.h:16-21` | 6 个未实现 VMMCALL 命令 fail-closed 到 YGHV_STATUS_ERROR（正确行为，记录） |
| YGHV-REV-029 | `control_device.c:353-354` | 设备对象无显式安全描述符（依赖驱动内 owner/SeDebug/CR3 检查，鲁棒） |
| YGHV-REV-030 | C:\yuanguard_hv.sys | C 盘稳定默认版 `70888311...`（9.112 构建）与 HEAD 源码构建 `F411C929...` 不同源；"当前版本"指 HEAD 时需重新部署 |
| YGHV-REV-031 | `protect.c:620` | GET_TARGETS 每槽 `hook_count` 恒 0（hooks 全局表设计语义，文档已注明） |
| YGHV-REV-032 | `tests/ioctl_parity.ps1:14` | 仅比较函数号，不比较完整 CTL_CODE（METHOD/ACCESS 位）；结构体布局在 protect.h 未与用户态共享 | 建议把 *_info 结构体移入共享头并扩展校验 |
| YGHV-REV-033 | `tools/yghv_stealth_check.ps1` | 只读正确；两处 nit：发现痕迹时无非零退出码、0-access CreateFile 探测可能误报 CLEAN |
| YGHV-REV-034 | `tools/yghv_client/build_jni.bat` + `build.bat` | 硬编码 `D:\DevTools\zulu21` 与 clang 路径；build.bat 把 Sleepy.class 输出到客户端根目录（开发机无害） |
| YGHV-REV-035 | `vmexit.c:490-493` | INTR/NMI 处理器仅计数不 EOI/重注入；经核实 trampoline VMEXIT 后先 stgi、pending 中断由宿主 ISR 消费（step11 实测 15 次 INTR 退出正常完成），无 livelock；留待 kd 复核语义 |
| YGHV-REV-046 | `npt_core.c`（`NPT_LARGE_PAGE_FLAGS`/`NPT_4K_PAGE_FLAGS` 省略 bit 63）+ `protect.c:137-139` | identity NPT 全 RAM 无 NX，guest 可执行任意 RAM 页；arm/disarm 重写 NX 位时只清不设。属 9.103 路线"默认 NX/RX 收紧"待办 | 显式 NX 策略（并入 R1/REV-043） |
| YGHV-REV-047 | `vmexit.c:409-418, 431-437` | rearm-pending 时无条件消费下一次 #DB：若 guest 本有 TF（调试单步）或硬件断点 #DB 恰在该窗口触发，guest 的 #DB 丢失 | 区分 guest #DB 来源或仅在 TF 由本驱动设置时消费 |
| YGHV-REV-048 | `protect.c` 解码器 + `1145-1148` + `yghv_hook_diag` | NIT 组：0x27/0x2F/0x9A/0xEA 在 x64 非法却被分错类；resume 与页写保护之间（1148→1151-1191）补丁页对 guest 仍可写的小窗口；yghv_hook.log 无限追加无大小上限 | 逐一修正/加大小上限 |

## 4. 任务修复清单（按优先级）

> 状态：`[ ]` 待处理 / `[x]` 已完成。原则：每次代码改动先出方案经用户确认，
> 记录到 `docs/YUANMOD_HANDOFF_CURRENT.md`；本机勿实机执行 hook 路径（REV-001）。

### P0 —— 低风险可本机验证
- [x] **YGHV-REV-003** 修复 `svm_core_stop_all_residents` 对 `pause_requested` vcpu 补 `KeSetEvent(resume_event)`（svm_core.c）——2026-08-14 已修
- [x] **YGHV-REV-004** 驱动 `get_pages_info` 改 `returned = min(total, cap)`；客户端循环收敛 `min(returned,count)`（protect.c + yghv_ctl.ps1 + YghvCtl.java）——2026-08-14 已修
- [x] **YGHV-REV-005** 修复 `yghv_trace_u64` label 截断，杜绝栈溢出（main.c）——2026-08-14 已修
- [x] **YGHV-REV-006** `check_target_exited` 捕获进程指针加 `ObReferenceObject/ObDereferenceObject`（protect.c）——2026-08-14 已修
- [ ] **YGHV-REV-020** PS 客户端 hex 参数兼容 `0x` 前缀（yghv_ctl.ps1）
- [ ] **YGHV-REV-021** selftest baseline 断言 + 失败清理驱动状态（yghv_ctl.ps1）
- [ ] **YGHV-REV-022** JNI `GetByteArrayElements` NULL 检查（yghv_ctl_jni.c）
- [x] **YGHV-REV-039** `yghv_hook_diag` 未初始化 `st` 修正（protect.c）——2026-08-14 已修（声明即初始化）

### P1 —— 语义修复（改后需回归 r1/非 hook 路径）
- [x] **YGHV-REV-002** `npt_set_page_perm_range` 改为 4K 迭代（npt_core.c）——2026-08-14 已修
- [x] **YGHV-REV-010** `npt_init` 失败路径 `g_cache=NULL`；npt_init/map_ram 失败统一清理（npt_core.c + main.c）——2026-08-14 已修
- [x] **YGHV-REV-011** `npt_translate` 先查 present（npt_core.c）——2026-08-14 已修
- [x] **YGHV-REV-013** VMMCALL CR3 比较统一掩码（vmmcall.c）——2026-08-14 已修
- [x] **YGHV-REV-015** VMMCALL ADD/REMOVE_PAGE 按 CR3 解析目标槽（vmmcall.c）——2026-08-14 已修
- [x] **YGHV-REV-036** `yghv_hook_diag` IRQL 违规：放锁后落盘或在 PASSIVE 线程落盘（protect.c）——2026-08-14 已修（mark+flush）
- [x] **YGHV-REV-037** 指令解码器修正（B8-BF 按 REX.W 分 imm32/imm64、0F 38/3A modrm 差一、F6/F7 按 modrm.reg 判立即数）+ 增补真实目标正向/负向单测（protect.c）——2026-08-14 已修 + boundary 5 用例
- [x] **YGHV-REV-040** install 失败路径恢复后补 NPT TLB flush（protect.c）——2026-08-14 已修
- [x] **YGHV-REV-041** install 失败路径先释放 stub 再 resume，消除 UAF 窗口（protect.c）——2026-08-14 已修（re-pause 后恢复+释放）
- [x] **YGHV-REV-044** `set_target` disarm 失败回滚或保持槽原状（protect.c）——2026-08-14 已修（snapshot+rollback）
- [x] **YGHV-REV-045** `on_npf_write` 决策前检查 `g_protect.active`（protect.c）——2026-08-14 已修

### P2 —— 安全/健壮性加固
- [x] **YGHV-REV-007** SHUTDOWN（含 STOP_INTERNAL）走 `r8==auth_key` 分支；cookie 派生加强（vmmcall.c）——2026-08-14 部分：SHUTDOWN 已改走 cookie+auth_key(r8)；STOP_INTERNAL 保持 cookie-only（cpuid 测试 guest 覆写 r8 且断言 r8==0，无法安全加 key；两者破坏性相同，纵深防御，真正门槛为 R1 私有页剔除，默认关）
- [x] **YGHV-REV-008/009** MSR/CR 处理器实现正确模拟或 fail-closed（vmexit.c）——2026-08-14 已修（MSR 写不覆写寄存器；CR fail-closed return 1）
- [x] **YGHV-REV-012** `SeSinglePrivilegeCheck` 加 `__try/__except` 或改 token 显式检查（control_device.c）——2026-08-14 已修（改 PreviousMode=KernelMode，返回 FALSE 不 raise）
- [x] **YGHV-REV-014** VMMCALL 状态协议统一（vmmcall.c）——2026-08-14 已修（控制命令经 `yghv_vmmcall_status` 映射 YGHV_STATUS_OK/ERROR）
- [x] **YGHV-REV-016** `svm_core_init` 失败路径恢复 EFER.SVME（svm_core.c）——2026-08-14 已修（svme_was_set 捕获 + 分配失败时恢复）
- [x] **YGHV-REV-017** loader_stealth 持 LoaderLock、完整性评估、并入默认前 kd `!driver` 复核——2026-08-14 部分：注释说明 DriverEntry 持 loader lock（无竞态）；完整性/启用仍待 kd（门控保持关闭）
- [x] **YGHV-REV-018** IOCTL 日志内存缓冲/限频或删除（control_device.c）——2026-08-14 已修（仅错误时落盘，删 in 行）
- [x] **YGHV-REV-019** README 注明 `protect` 仅能保护自身（或客户端提前提示）——2026-08-14 已修（README 增注 + list-java 指引）
- [ ] **YGHV-REV-043** `deny_status`/hook stub 对 guest 置 NPT 只读（并入 REV-007/R1 私有页剔除方案）——**延后**：R1 设计族
- [ ] **YGHV-REV-046** identity NPT 显式 NX/RX 收紧（并入 R1 默认权限路线）——**延后**：R1 设计族（须选择性 NX，避免 guest 代码页不可执行）
- [x] **YGHV-REV-047** rearm-#DB 消费逻辑区分 guest 来源 #DB（vmexit.c）——2026-08-14 已修（仅 TF 置位时消费 rearm，否则重注入）
- [ ] **YGHV-REV-029** 设备对象加显式安全描述符（评估）——**延后**：DACL 配置风险，需评估客户端打开权限后实施

### P3 —— 需换平台 / 内核调试会话
- [ ] **YGHV-REV-001** 锁序重构（pause 移出锁外 + patch 互斥 + clear 单次 pause 后无条件清 hook 槽）；按 9.129 设计文档，换平台/kd 后实机验证——仍延后（大改+hook 路径本机不可测）
- [x] **YGHV-REV-038** remove_hook pause 失败也必须恢复原始字节并清 `h->installed`——2026-08-14 已修（构建验证，本机未加载）
- [x] **YGHV-REV-042** baremetal step 9"0 页 keepalive"与 `start_locked` 语义对齐（门控实验）——2026-08-14 已注释说明（构建验证）
- [ ] **YGHV-REV-035** INTR/NMI 拦截语义 kd 复核

### P4 —— 清理 / 文档
- [x] **YGHV-REV-023** 清理死代码（yghv_prepare_guest_code / yghv_make_guest_code_executable；pool/test/min_drv/stub 归档说明）——2026-08-14 已移除三处死代码（main.c）；**构建验证通过，本机未加载**（P4 冻结已证实为平台间歇性，清理代码无因果，见 9.141/9.144/9.145）
- [x] **YGHV-REV-024** 去重复 if（main.c）——2026-08-14 已修；构建验证，本机未加载
- [x] **YGHV-REV-025** svm_core.c 去掉硬编码 YGHV_DEBUG_LOG——2026-08-14 已修（svm_core.c + npt_core.c，build.bat 全局传 /DYGHV_DEBUG_LOG）；构建验证，本机未加载
- [x] **YGHV-REV-026** 更换 pool tag / 去 ponytail 代号——2026-08-14 已清理（debug.h tag 注释 + loader_stealth/svm_core 三处）；构建验证，本机未加载
- [x] **YGHV-REV-027** 交接文档 ERROR_BUSY 标注改 0x5AA 正确命名；safety_checks 纳入交接文档——2026-08-14 已修（9.125 改 ERROR_NO_SYSTEM_RESOURCES；safety_checks 纳入并验证通过；纯文档/脚本）
- [x] **YGHV-REV-032** *_info 结构体移入共享头；ioctl_parity 扩展完整编码/布局校验——2026-08-14 部分：ioctl_parity 已扩展完整编码常量校验（脚本）；*_info 移入共享头待定
- [x] **YGHV-REV-033** stealth_check 加非零退出码——2026-08-14 已修（脚本）
- [x] **YGHV-REV-034** build_jni.bat 去掉硬编码路径——2026-08-14 已修（脚本）
- [x] **YGHV-REV-048** 解码器非法 opcode 分类、补丁页写窗口、yghv_hook.log 大小上限（NIT 组）——2026-08-14 已修非法 opcode（0x27/2F/9A/EA 拒绝）+ hook.log 4096 行上限；补丁页写窗口并入 REV-001；构建验证，本机未加载
- [x] **YGHV-REV-030** 明确"C 盘稳定版 vs HEAD"部署口径（记录即可，非代码问题）——2026-08-14 已记录（TASKS.md 部署基线）

> **2026-08-14 P4 冻结事件（更新）**：P4 版（C014E8BB...）实机回归时硬冻结（10:44:31 意外关机，无蓝屏/转储）。后续阶段 3（9.144）用无 P4 改动的构建复跑同一 selftest 序列未冻结 → 证实 P4 冻结为平台间歇性（1/5），与 P4 清理代码无因果；P4 驱动清理于 2026-08-14 重新应用（构建验证通过，本机未加载——本机 hypervisor 负载敏感，见 9.145）。蓝屏（11:03）根因为看门狗 join 句柄误用（9.142，已修）。

## 5. 文档-代码核对结果

- HEAD `c37dd2e` = 9.135，工作区干净（构建产物 gitignore）。
- 9.115-9.135 驱动/客户端/测试声明与代码一致；宏默认值、门控状态、IOCTL 编号、
  认证分层、多目标 4 槽、hooks 全局表均核实无误。
- 发现 2 处文档/工具间隙：交接文档 ERROR_BUSY 残留（REV-027）；C 盘稳定版与
  HEAD 不同源（REV-030）。

## 6. 附录：本次审查记录

- 审查动作待记入 `docs/YUANMOD_HANDOFF_CURRENT.md`（9.136）。
- 子代理：SVM 核心 / NPT+vmexit / 控制面 / 客户端+测试 / protect 层 五路并行，
  报告全部完成并逐条核实并入；protect 层独立确认了 REV-001/004/006，
  并新增 REV-036/037/038/039/040/041/042/043/044/045/046/047/048
  （已全部并入本报告，其中 REV-037 解码器细化为 3 类误解码 + RIP-rel 检测）。
- 独立复核修正：exit-test 不可用（误报，默认构建 `g_persistent_mode=TRUE`）、
  INTR 拦截"惰性"（与 step11 实测矛盾，留 REV-035 待 kd）。
