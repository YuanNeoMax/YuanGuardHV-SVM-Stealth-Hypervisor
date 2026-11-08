# YuanGuardHV hook 锁序与 0x5AA 静态分析（2026-08-13）

> 只读分析，未改驱动代码；修复方案供换平台/硬件调试器后验证。

## 1. 结论

- `0x5AA` 是 Win32 `ERROR_NO_SYSTEM_RESOURCES`（1450），不是
  `ERROR_BUSY`（170）。用 `RtlNtStatusToDosError` 实测：
  `STATUS_INSUFFICIENT_RESOURCES (0xC000009A) -> 0x5AA`。
- 全仓库没有 `STATUS_DEVICE_BUSY` 或直接 0x5AA 返回点；最可能的
  真实来源是 `STATUS_INSUFFICIENT_RESOURCES`。
- 候选返回点：
  - `YuanGuardHV/hv/protect.c:249`：目标表满（槽 0 System + 3 客户端
    槽），`install-hook` 对非目标调用者自动 `set_target` 时最容易触发；
  - `YuanGuardHV/hv/protect.c:1071`：hook stub 非分页池分配失败；
  - `YuanGuardHV/hv/npt_core.c:91`：2MB→4KB split 的页表分配失败；
  - `YuanGuardHV/hv/control_device.c:82`：CreateFile 上下文分配失败；
  - `YuanGuardHV/hv/protect.c:314`：目标页表满。
- 9.124/9.125 的“ERROR_BUSY”记录应改为“ERROR_NO_SYSTEM_RESOURCES”；
  历史编号不重排，以本文件为准。

## 2. 死锁/冻结路径

当前 install/remove：

1. `yghv_protect_install_hook` / `remove_hook` 先取 `g_protect_lock`
   （protect.c:1033-1035 / 1216-1218）。
2. locked 函数内调用 `svm_core_pause_residents_for_patch`
   （protect.c:1139 / 1254）。
3. 常驻核心的 NPF 写路径 `yghv_protect_on_npf_write` 也要取同一把锁
   （protect.c:497-526）。
4. 若核心正在 NPF handler 等待锁，它不会回到 resident 循环置
   `pause_ack`；IOCTL 线程持锁等 pause，形成 AB-BA 死锁窗口。
5. pause 有 5 秒超时并会 `svm_core_resume_residents`，理论可恢复；
   但实机观测是整机硬冻结且无 dump，说明该窗口在本平台被放大或
   还存在 VMRUN 内不退出等其他原因。9.118 的“pause 移出锁内”尝试仍
   冻结，不能只靠调换顺序解决，需要 kd 确认实际停点。

`clear` 同一问题放大：`yghv_protect_clear` 持锁后逐个
`remove_hook_locked`（protect.c:649-672），每次都会 pause；remove
pause 失败时不清 `h->installed`，所以 `hook_count` 可残留 1。

## 3. 修复方案（供换平台验证，本机不实机执行）

### 3.1 锁序

- install/remove/clear 统一改为：patch 互斥 -> 资源准备 -> pause ->
  取 `g_protect_lock` -> 修改 -> 释放 -> resume -> patch 互斥释放。
- pause 阶段不持有 `g_protect_lock`，NPF handler 可完成当前临界区并
  回到 resident 循环 ack。
- 增加独立 patch 互斥（owner + generation，或专用 FAST_MUTEX），
  防止 install/remove/clear 并发；互斥只保护 patch 流程，NPF 不取它。
- 保留 pause 超时，但超时必须完整 resume 并返回 `STATUS_TIMEOUT`，
  调用侧记录 `patch:pause`。

### 3.2 clear

- 先停止保护并 disarm 全部页；然后 pause 一次；持锁清理 hooks 与
  targets；最后 resume。
- remove_hook 失败不再让 clear 保留 `installed=1`：clear 的语义是清空，
  应在尝试移除后无条件清 `g_protect_hooks[]` 对应槽（stub 按需释放）。

### 3.3 0x5AA 处理

- 目标表满应返回明确语义并打印“target table full”，不要笼统落到
  `STATUS_INSUFFICIENT_RESOURCES`；客户端据此提示先清槽或等目标退出。
- 预分配 hook stub 页（例如 `YGHV_PROTECT_MAX_HOOKS` 个 4KB 页）在
  DriverEntry 初始化，install 不再运行时分配，消除最常见的 pool 失败。
- `npt_split_2mb_to_4kb` 的页表分配失败保留，但记录 `install:split`
  与 NTSTATUS；可在首次 install 前预热 split 或对分配失败重试一次。
- 客户端把 `0x5AA` 显示为 `ERROR_NO_SYSTEM_RESOURCES`，不再叫
  ERROR_BUSY；IOCTL 失败时打印 NTSTATUS hex 与阶段。
- 扩展 `yghv_hook_diag`：`install:targetfull`、`install:stub`、
  `install:split`、`install:addpage` 都写入 hook id + NTSTATUS；
  CreateFile 失败单独写 `yghv_ioctl.log` 的 `create` 阶段。

### 3.4 实机验证顺序

- 新平台/kd 环境先做 install/remove/clear 单 hook 回归，确认无冻结；
- 再并发 install + NPF workload，观察是否出现 pause timeout；
- 再构造 4 目标槽满场景，验证 0x5AA 根因与客户端提示；
- 全程保留 `yghv_hook.log`、`yghv_ioctl.log` 与 kd 日志。

## 4. 相关文件

- `YuanGuardHV/hv/protect.c`：hook 状态、g_protect_lock、install/remove/clear
- `YuanGuardHV/hv/svm_core.c`：`svm_core_pause_residents_for_patch`
- `YuanGuardHV/hv/control_device.c`：IOCTL 分发、CreateFile 上下文
- `YuanGuardHV/hv/npt_core.c`：2MB→4KB split 与页表分配
- `YuanGuardHV/tools/yghv_ctl.ps1`：客户端错误呈现
