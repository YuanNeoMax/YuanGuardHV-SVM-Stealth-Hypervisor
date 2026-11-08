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
| `ioctl_parity.ps1` | C 头文件 ↔ PowerShell / Java / JNI 的 IOCTL 编号与常量一致（当前 **C=56 / PS=56 / Java=56**）|
| `command_parity.ps1` | README ↔ 两个客户端的命令一致（当前 **PS=79 / Java=18**）|
| `safety_checks.ps1` | 安全红线（pause-under-lock 作用域、`STATUS_DEVICE_BUSY`、`0x5AA` 命名）|
| `irql_contract_check.ps1` | IRQL 可达性、岛内读区域、不许有「说谎的 `*_bare`」|
| `exit_rip_contract.ps1` | 38 个 `#VMEXIT` case 的 RIP 契约（见下）|
| `ioctl_buffer_size.ps1` | 客户端缓冲覆盖它读的每个偏移 |
| `count_as_extent.ps1` | 未压缩的表不许用「活计数」当扫描上界（见下）|
| `ioctl_output_bounds.ps1` | 每个写回复缓冲的 handler 先测 `out_len` |
| `client_undefined_vars.ps1` | 客户端读到的每个变量名都被赋过值 |
| **`docs_check.ps1`** | **文档门**：相对链接、索引行数、空记录、最新编号、归档份数（见下）|
| **`stub_stack_contract.ps1`** | **桩不许动 RSP**：`Sv206GuestStub` 体内禁 push/pop/call/ret/lea rsp/[rsp]，且必须仍有 `vmmcall`（见下）|

## `docs_check.ps1` —— 文档是这棵树里唯一没有门的东西

十一个检查器全部读**源码**，没有一个读 `docs\`。于是文档成了唯一无人看管的产物，
而它漂移的方式与任何无门产物一样：

```
9.563 实测，INDEX.md 写下当天就有 5 处过期
  最新编号            9.561   vs  交接文档自称 9.562 / 正文末条 9.559
  archive6-11-08  24 份   vs  25 份
  docs\superpowers\  未登记  vs  2 份，且全树没有第二个入口
  9.559 记录          索引标 18 行  vs  只有标题、正文 0 行
  QUICKSTART          同一份文件里 9.537 与 9.558 两个答案
```

**检查的五件事**（只读，不加载驱动、不改变机器状态）：

| # | 检查 | 判据 |
|---|---|---|
| 1 | 链接完整性 | `docs\` 下每个 `](相对路径)` 必须解析得到 |
| 2 | 索引行数 | 每行的数字 == 该记录从标题到下一个标题的真实跨度 |
| 3 | 空记录 | `#### 9.x` 标题必须有正文（**0 行**才算缺陷，一句话记录是合法的）|
| 4 | 最新编号 | INDEX.md 的最大编号 == 交接文档里的最大编号 |
| 5 | 归档份数 | INDEX.md 印出的份数 == 磁盘上的份数 |

**为什么第 2 条最值钱**：索引的全部价值就是它的数字是真的。读者照「18 行」跳，
是在履行文档给出的一个承诺；如果那个承诺从未被测量，他就落在错误的记录上，
并**以为自己读错了文件**。9.559 就是证据 —— 空壳，而索引给它记了 18 行。

**为什么第 5 条不是学究**：9.500 是同一个形状 —— 四条客户端命令从未工作过，
因为每个检查器都读文件的不同部位，没有一个看那个操作数。
**文档里的数字就是那种值：量过是真的，手打的是小说。**

**自检在 `docs_check_selftest.ps1`，不读 `docs_check` 的代码，而是拿它的产物做实验**：
把 4 个真缺陷分别种进 `docs\` 的临时副本（索引行数改 999 / 掏空一条记录 /
断一条链接 / 把最新编号改回 9.500），**每个都必须被抓到**。未改动的副本必须 PASS，
否则下面四条什么都不能证明。

**还有一条反向对照**：检查器扫链接前会遮掉代码块与行内代码（讲链接语法的散文不是链接），
而遮罩是一条致盲的路 —— 所以往行内代码里塞一个指向不存在文件的链接，
**要求检查器仍然 PASS**。遮罩若吃掉真实区域，这条对照立刻失败。

> 自检第一次跑就暴露了自检自己的缺陷：父进程用 `*>&1 | Out-String` 收子进程输出，
> 但文件头是 `$ErrorActionPreference = 'Stop'`，**子进程的第一条 stderr 记录就终止了收集**，
> 于是四条正确的 mutation 全被报成「failed but not for the reason」。
> **检查器是对的，是取数的那一侧瞎了。** —— 这与 9.486 记录过的「猜出来的窗口，两个方向都会错」同族。

---

## `stub_stack_contract.ps1` —— 把最贵的一次蓝屏钉成规则

真 guest 唯一剩下的方向是「早退之后 `jmp` 进 `Sv206GuestStub`」。这条路上四次蓝屏，
而最贵的一次（9.444，`0x1E STACKPTR_ERROR`，#GP 在 `KiSwapContext` 里）
不是偏移算错，是**在外来栈上做了上下文切换**。

桩的注释一直写着 "the stub never touches the stack"——
**这句话此前没有任何东西在管。它是承诺，不是规则。**

检查方式：切出 `Sv206GuestStub proc ... endp`，在**去掉注释之后**的代码里扫：

```
禁   push / pop / call / ret / leave / enter
禁   add|sub rsp / mov rsp / lea rsp
禁   任何 [rsp ...] 或 [.. sp] 的栈相对访问
```

另外要求桩体内**仍然有 `vmmcall`** —— 一条停止握手看不见的桩，即使"不碰栈"也没有价值。

**判据边界，写在这里**：

```
证明     : 桩不会破坏它进来时的 RSP
不证明   : 跳到桩之后停止握手一定能应答   <- 这是实机问题，也是四次蓝屏那一段
```

绿色的勾**不能**被读成"真 guest 通了"。

**自检**：往桩里种一条 `push rbx`，要求报 `push moves RSP`——不是"抓到就行"。
第一版自检真的报了 "failed but not for the reason"，原因是用 `-match` 去比一个
`List` 的返回，`.Count` 拿到的是字符串长度 `1`。改成 `@(Test-Stub $tmp)` 后正确。

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
