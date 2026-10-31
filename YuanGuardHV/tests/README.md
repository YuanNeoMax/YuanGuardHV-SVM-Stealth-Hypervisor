# YuanGuardHV 构建期检查

本目录只读源码与链接产物，**不加载驱动、不触碰任何实机路径**。

## 两个运行器（被 `build.bat` 调用）

| 脚本 | 时机 | 为什么分开 |
|---|---|---|
| `run_source_checks.ps1` | 编译**之前** | 这些检查只读源码 |
| `run_map_checks.ps1` | 链接**之后** | 页布局检查要读 `.map`，必须在链接完成后 |

这个拆分是 9.358 换来的：之前是一个运行器在编译前跑，于是 `page_layout_check.ps1`
读到的是**上一次构建**的 `.map` —— 改了四个 guard 全局的对齐，「没有任何变化」。

## 源级检查（`run_source_checks.ps1`）

| 脚本 | 检查什么 |
|---|---|
| `ioctl_parity.ps1` | C 头文件 ↔ PowerShell / Java / JNI 的 IOCTL 编号与常量一致（当前 **C=49 / PS=49 / Java=49**）|
| `command_parity.ps1` | README ↔ 两个客户端的命令一致（当前 **PS=70 / Java=18**）|
| `safety_checks.ps1` | 安全红线（pause-under-lock 作用域、`STATUS_DEVICE_BUSY`、`0x5AA` 命名）|
| `irql_contract_check.ps1` | IRQL 可达性、岛内读区域、不许有「说谎的 `*_bare`」|
| `exit_rip_contract.ps1` | 38 个 `#VMEXIT` case 的 RIP 契约（见下）|

## `exit_rip_contract.ps1` —— 为什么它存在

`VMEXIT_VMMCALL` 漏写 `Rip = NRip` 很久没人发现，因为它**没有失败模式**：

- 机器正常启动、运行、卸载干净
- 计数器在动，而且动得又快又稳
- 唯一症状是「改参数没效果」

而「改参数没效果」会被读成「这条指令在这个平台上不做事」——
它导致了三轮全错的实验（`hlt`、`pause`、`hlt+pause`），
每一轮都配了一篇看起来扎实的解释。

**没有枚举，就没有发现的机会。** 所以这个检查把 38 个 case 全部归类：

| 类别 | 断言 | 例子 |
|---|---|---|
| **ADVANCE** | 必须写 `StateSaveArea.Rip = NRip` | CPUID / MSR / RDTSC / 25 个指令型 / VMMCALL |
| **INJECT** | 必须注入事件（可直接或经 handler）| NPF / VMRUN / INTR / NMI |
| **HOLD** | **必须不写 RIP** | `#BP` / `#DB` / SHUTDOWN |

`HOLD` 是三类里唯一断言「不写」的第三种，也是最容易搞错的一类：

- `#BP` / `#DB` 是 **trap**，保存的 RIP 已指向下一条；APM 规定
  exception VMEXIT 的 NRIP = **RIP+1（一个字节）**，所以写 `Rip = NRip`
  会让 guest **从指令中间恢复**。这棵树在 9.254 实测过该后果。
- `SHUTDOWN` 根本不 resume（`S206RecordFatal` + `yghv_fail_degrade`），
  问它「注入吗」「推进吗」都是错的问题。

**新增 case 而未分类时，检查器报 `UNCLASSIFIED` 并失败** ——
这正是 `VMMCALL` 当年那个洞的形状。

### 它还有自检

每次构建都会：把 `VMEXIT_VMMCALL` 的 `Rip = NRip` 从**副本**里删掉 →
用本脚本自己跑那个副本 → **要求必须抓到** → 清理。

复用脚本本身（而非重写一份逻辑）是关键：否则自检测的是复制品，
复制品可以和本体一起漂移。

> 写这个检查器的三版都出过错，每次都是「检查器错了、代码是对的」：
> 第一版把「下一个 case」当 body 结尾 → 25 个假失败；
> 第二版只查 body 字面量 → `VMRUN` 假失败；
> 第三版窗口差 17 行 → `SvHandleMsrAccess` 假失败。
> **一个把好树报成坏的检查器比没有更糟**——它会训练人忽略它。

## 运行

```powershell
# 全部源级检查
powershell -NoProfile -ExecutionPolicy Bypass -File run_source_checks.ps1

# 单项
powershell -NoProfile -ExecutionPolicy Bypass -File exit_rip_contract.ps1
```

`build.bat` 会自动跑这两个运行器；任何一项失败都会中止构建。
