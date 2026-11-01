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
| `ioctl_buffer_size.ps1` | 客户端缓冲覆盖它读的每个偏移 |
| `count_as_extent.ps1` | 未压缩的表不许用「活计数」当扫描上界（见下）|
| `ioctl_output_bounds.ps1` | 每个写回复缓冲的 handler 先测 `out_len` |
| `client_undefined_vars.ps1` | 客户端读到的每个变量名都被赋过值 |

## `count_as_extent.ps1` —— 为什么它存在，以及它为什么被重写

一个表的计数是**在册数量**，不是**数组范围**。数组从不压缩，所以删中间槽会留下洞：

```
装 3 个:  active = [1,1,1]  count = 3
删中间:   active = [1,0,1]  count = 2
扫描 i<n: 只看槽 0、1 -> 槽 2 活着，但永远找不到
```

后果是静默的：第三个断点停止触发，而 `count=2` 与所有列表自洽。

**9.486 第一版是空过的**：它只在计数读取之后 **5 行内**找循环，而真实距离是 18–19 行，
所以一条都没匹配到，却仍打印 `[WARN] ... may be stale` 并 `[PASS]`。
这与 `exit_rip_contract` 第一版「窗口太窄 → 假失败」是同一类错误的两面：
**猜出来的窗口，两个方向都会错。**

9.498 重写后：
- 用**花括号配对**导出函数体，在函数体内任意位置找循环 —— 不再有窗口；
- 计数从不递减的表（append-only）按构造放行（`g_state_obj_count` 就是这种）；
- 匹配不到任何计数读取时**硬失败**，而不是 WARN + PASS；
- **带自检**：把原始缺陷（19 行的原始距离，不是贴着循环的 1 行）种进 `hv\` 的临时副本，
  **要求必须抓到**，然后清理。贴着循环种缺陷会连坏窗口都能抓到，那等于什么都没证明。

**自检第一次真正跑起来就抓到了第四个扫描**：`yghv_bp_on_dr` 仍用 `g_bp_count` 当上界，
9.485 只修了它当时看的三处。已在 9.498 一并修掉。


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
