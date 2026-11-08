# YuanGuardHV 全链路隐藏与反侦查审计（2026-08-13）

> 目标确认：以“抗人工取证”优先，ACE 类反作弊尽力而为但不保证。
> 合规说明：反作弊对抗可能违反游戏/平台服务条款，实施前需自行确认
> 用途合规；本文只描述本项目的隐藏能力与验证方法，不承诺绕过任何
> 具体反作弊产品。

## 1. 现状约束

- 当前主线是非驻留保护路线：驱动加载在真实 Windows 里运行。真实系统
  内核态安全软件原则上总能发现一个已加载的内核驱动，绝对隐形不现实。
- OS-as-guest 整机隐藏已停线（平台级硬冻结），所以“整机都看不见”
  的形态当前不可用。
- 因此本阶段目标是：把明显痕迹降到最低，提高人工取证门槛；ACE 只做
  尽力优化，不作为验收标准。

## 2. 六层隐藏矩阵

| 层 | 已有能力 | 缺口 | 验证方式 | 风险 |
|---|---|---|---|---|
| 加载层 | 服务防删除、demand/auto 切换 | 驱动文件、服务、注册表、签名均可见；无文件/注册表隐藏 | 注册表/服务查询、文件扫描 | 低 |
| 系统运行层 | `loader_stealth` 门控可从模块列表摘链 | 默认关闭、未 kd 复核；设备对象、日志、句柄可见 | kd `!driver`、设备查询、日志检查 | 中 |
| 处理器层 | CPUID 隐藏只覆盖合成 guest；R1 私有内存剔除代码已写 | 门控关闭，本机硬冻结；MSR/IO 隐藏未做 | 裸机/KVM + kd/硬件调试器 | 高 |
| 交互层 | CLI/Java 客户端可配置保护 | 设备名 `YuanGuardHV`、进程关联、Minecraft mod 均可见/未做 | 客户端自查、进程扫描 | 中 |
| 反卸载/反篡改 | 服务 SDDL 防删、`YGHV_UNLOAD_GUARD` 门控 | 文件完整性、防篡改未做 | sc sdset、文件校验 | 中 |
| 自查层 | 无 | 需要用户态痕迹扫描工具 | 本文件配套 `yghv_stealth_check.ps1` | 低 |

## 3. 当前可见痕迹清单

- 服务：`yuanguard`
- 注册表：`HKLM\SYSTEM\CurrentControlSet\Services\yuanguard`
- 驱动文件：`C:\yuanguard_hv.sys`
- 控制设备：`\\.\YuanGuardHV` / `\Device\YuanGuardHV`
- 日志：`C:\Windows\yghv_progress.log`、`yghv_ioctl.log`、
  `yghv_hook.log`
- 源码/构建目录：`D:\yuanguard`、`YuanGuardHV\bin` 等
- 内核模块列表：`loader_stealth` 开启前可见

## 4. 推荐实施顺序

### 阶段 1：本机安全（当前可做）
- 运行 `tools\yghv_stealth_check.ps1`，量化可见痕迹。
- 删除/收敛调试日志与测试文件，压缩发布包。
- 服务默认 demand、SDDL 防删；客户端不落盘敏感参数。

### 阶段 2：需要内核调试会话
- `loader_stealth` 并入默认前，先 kd `!driver` 复核模块列表确实不可见。
- 评估设备对象/符号链接改名或隐藏；评估 `YGHV_UNLOAD_GUARD`。

### 阶段 3：需要裸机/KVM
- R1 私有内存剔除与 NPT 自剔除（当前门控关闭）。
- MSR/IO 隐藏、处理器层剩余痕迹。
- 真实系统下 CPUID/时序侧信道优化。

### 阶段 4：产品化整合
- Minecraft mod 骨架与客户端通信收敛。
- 发布包构建、签名策略、启动/卸载流程。

## 5. 自查工具

`YuanGuardHV\tools\yghv_stealth_check.ps1` 只读检查当前机器上的可见
痕迹，不加载驱动、不写日志、不修改系统。输出每项 `[VISIBLE]` /
`[CLEAN]` 和总可见痕迹数。

运行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File YuanGuardHV\tools\yghv_stealth_check.ps1
```
