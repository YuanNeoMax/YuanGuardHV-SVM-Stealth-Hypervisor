# 快速承接 · YuanGuardHV

> **30 秒版**：AMD SVM 管理程序驱动，Windows 自己是 guest。代码在 `D:\yuanguard`，
> 分支 `main`，构建一条命令，加载一条命令。
> **当前最新编号 9.511**，G25 已在当前代码上重跑 **17 项 ALL PASS**。

---

## 1. 这是什么

一个运行在**真实 Windows** 上的 AMD SVM/NPT 管理程序。Windows 以 guest 态运行，
驱动是 host。提供三样东西：

| 能力 | 命令 | 实测数据 |
|---|---|---|
| 跨进程内存读写 | `mem-read` / `mem-write` / `mem-bench` | 实测比 API 快 **3.96×**（javaw，32MB 范围，9.449）|
| 页写保护 | `protect-page` / `unprotect-page` / `config-sync` | 不翻转 NPT 权限（周期比对 + 回滚）|
| 自我隐藏 | `section-hide` / `link-hide` / `sys-guard hidecore` | `.pdata` 段 DECOY 隐藏，guest 读零 |

---

## 2. 三分钟上手

```powershell
# ① 构建（约 10 分钟；判据不是 md5，重签名会变）
$env:YGHV_BAREMETAL_STEP='206'; $env:YGHV_206B_COEXIST='1'; $env:YGHV_206B_GNPT='1'
cd D:\yuanguard\YuanGuardHV; cmd /c build.bat
#   看这三行：ALL SOURCE CHECKS PASS / ALL MAP CHECKS PASS / Build SUCCESS

# ② 部署（必须先看哈希，再 start）
Copy-Item D:\yuanguard\YuanGuardHV\bin\yuanguard_hv.sys C:\yghv_pa.sys -Force
(Get-FileHash C:\yghv_pa.sys -Algorithm SHA256).Hash.Substring(0,16)

# ③ 加载 + 冒烟
sc.exe start yghvpa
powershell -NoProfile -ExecutionPolicy Bypass -File D:\yuanguard\YuanGuardHV\tools\yghv_ctl.ps1 sys-health

# ④ 卸载（这是最重要的验收）
sc.exe stop yghvpa
Get-Content C:\Windows\yghv_progress.log | Select-String "invmrun at unload"
#   必须看到：s206 invmrun at unload = 0x0     （12 个核全部干净离开）
```

**卸载判据 `invmrun=0x0` 是唯一可信的"没坏"证据。** 卸载耗时约 5 秒。

---

## 3. 当前状态（2026-11-01）

| 项 | 值 |
|---|---|
| 分支 / HEAD | `main` @ `a6f31dc`（9.497/a/b + 9.498），与 origin/main 同步后再确认 |
| 驱动 | `yghvpa` · **STOPPED** · 设备 `\\.\YuanGuardHV` |
| 最新编号 | **9.511** |
| 工作树 | 干净 |
| 静态检查 | 10 个运行器 / 14 条 PASS（含 3 个自检），两个运行器全绿；客户端 **75** 条命令 |
| 部署镜像 | `C:\yghv_pa.sys` = `90A964DA`（20:33:26），**与 `bin` 哈希一致** |
| **最近一次卸载** | `invmrun at unload = 0x0`，`sc stop` 6.76 秒，无蓝屏（9.511）|
| **G25** | **17 项 ALL PASS**（2026-11-01 12:16–12:38，跑在 9.497 的镜像上；9.498 只改了扫描上界）|

> **G25 的 target→T12 间隔 1056 秒**，`mmf-hold` 寿命 3600 秒 —— 余量 3.4×。
> 这个数必须一起记：9.447 就是把「镜像慢」误判成「驱动回归」的那一次。

---

## 4. 下一步（按优先级）

| # | 事项 | 为什么这个顺序 |
|---|---|---|
| **1** | **Tier 6.1 启动期控制验收** | 台账里唯一悬空项。**需重启，窗口不可回滚** |
| 2 | **那 47 页可以藏吗？——先让 DECOY 页的「写」也可见** | 9.511 已测：54 页里只有 7 页被 guest 触及、47 页从未。但藏了「会被写」的那页后写落空、30 秒没崩 —— **「丢了什么」还没有判据**，47 页的成败又会退回「跑得久没崩」|

> **G25 已完成（9.497）**，不再是待办。它跑在 9.465 的 RIP 推进与 9.448 的 35 个 VMEXIT
> case 之上 —— 那两条都在每个核的每轮热路径里，所以这一轮不能由「卸载通过」替代。

### G25 的四个参数（被实测打过脸，别改回去）

```
各命令 -TimeoutSec  : 150 / 180
HangTimeoutSec      : 240
mmf-hold 寿命       : 3600 秒
脚本                : D:\aaaaaavm\g25_tier7.ps1
```

> `mmf-hold` 目标到期退出后，`mem-read` 返回 `FAILED status=0x00000000`、
> `mem-pte` 返回 `NOT MAPPED` —— **看起来完全像驱动回归，实际是测试工具超时**。9.446 误判过一次。

---

## 5. 会浪费你时间的八个坑

| 坑 | 症状 | 处置 |
|---|---|---|
| **每条命令 25–60 秒** | 看着像挂死 | **不是挂死**：12 核在 guest 里跑，每个 IOCTL 排队。超时设 **≥300 秒** |
| `Copy-Item` 部署 | `sc start` 成功但在跑旧镜像 | **先看哈希再 start** |
| `[IO.File]::ReadAllText/WriteAllText` 反复整文件替换源码 | 文件变成二进制 | **用 `edit` 原子替换**；坏了就 `git checkout` |
| pwsh 的 `cd` 不持久 | 文件写到错误目录 | **用绝对路径** |
| `yghv_stealth_check.ps1` 不传 `-ServiceName` | 所有项报 `CLEAN` | 服务名是 `yghvpa` 不是 `yuanguard` |
| `npt-view` 的 `frame` | 大页上不是该页的帧，误判「可见」 | **先排除 `large=1`**，再判 UNMAP/DECOY |
| PowerShell 双引号里写 Markdown | 反引号被当成转义符，文件内容静默损坏（且肉眼难辨）| **改写含反引号的文档走 Python**，不要走 PS 的双引号字符串 |
| `git push` 连不上 | 代理或直连失效 | **先探测两边**（`Test-NetConnection 127.0.0.1 -Port 7890` 和 `github.com -Port 443`），谁通用谁 |

---

## 6. 三条方法论（比任何单个修复都值钱）

### 一、判据的局限 ≠ 被测对象的结论

`npt-view` 只认 `present=0`，于是把 DECOY 模式的**成功隐藏**报成「没隐藏」，
据此写下了「NPT 条目没真正落地」——**那句结论是错的**。同形态已复发五次。

> **写结论前问一句「我的判据能观察到这个事件吗」。**

### 二、参数怎么调读数都不变时，先确认它有没有被执行

9.463/9.464 两轮都在解释「为什么 `pause`/`hlt` 不起作用」，真相是
`VMEXIT_VMMCALL` 不推进 RIP，**那些指令根本没走到**。修一行后退出率降 **19200 倍**。

### 三、检查器必须自证能失败

三个检查器都曾报假失败，有两版正则够不到要检查的文本、**注入 bug 后仍 PASS**。

> **每个检查器都要注入一次真缺陷、确认能抓到、再恢复。永不失败的检查器等于没有。**

---

## 7. 操作纪律

- **改变机器状态的命令必须先得到明确确认**：`sc create/start/stop/delete`、
  驱动加载/卸载、压测、重启、启动链修改、GitHub 推送。
- **只读可直接执行**：读源码、`dumpbin`、`kd` 分析 dump、读日志。
- **构建和蓝屏取证不需要确认。**
- 蓝屏后：**自动取证 → 定位 → 修复 → 重跑**，不中断问人。
- 优先级：**重启类最后、压测最后、小爆炸半径优先**。
- 每个改动记进 `YUANMOD_HANDOFF_CURRENT.md`，编号接着上一个，**不跳号**。

---

## 8. 文档地图

| 想知道什么 | 读哪个 | 大小 |
|---|---|---|
| **新会话第一步** | `docs/RESUME_PROMPT.md` | 49 KB |
| 现在在哪、这一步做了什么 | `docs/YUANMOD_HANDOFF_CURRENT.md`（9.300–9.511） | 552 KB |
| 哪些功能做了 / 哪些不做 / 为什么 | `docs/TIER_LEDGER.md` | 7.6 KB |
| **写内核代码前** | `docs/KERNEL_API_CHECKLIST.md` | 36 KB |
| 目录总览 | `docs/INDEX.md` | — |
| 考古（9.1–9.299） | `docs/_ARCHIVE_HANDOFF_9.1-9.299.md` | 523 KB |

---

## 9. 一句话

**这个项目的难点不是写代码，是「在没有观测能力的路径上，任何改动都不可验证」。**

所以：先建观测，再改代码；改完先确认它被执行到。
