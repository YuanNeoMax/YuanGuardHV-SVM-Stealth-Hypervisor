# 9.548 ★★★★ 突破：`early-returns` 0 → 6，参考项目的模型在本机成立

> 2026-11-08。**整个重构的关键里程碑达成。**

---

## 一、读数

**桩模式（探针关闭后，行为未变）：**
```
entries=6  early-returns=0  cores-marked=6  disagrees=0
```

**RealGuestMode=1：**
```
entries=6  early-returns=6  cores-marked=6  disagrees=0
guest-start: realGuestMode=1  writers=6  exits=0
  guest Rip given = 0xFFFFF801BDB2531F
  guest Rsp given = 0xFFFFE283826D2B50
sys-health: degraded=0 total=0 fatal=0
```

| 模式 | entries | early-returns |
|---|---|---|
| 桩 | 6 | **0** ← 桩不回来（预期）|
| **RealGuestMode** | 6 | **6** ← **全部回来** |

---

## 二、这意味着什么

`early-returns` 只在**真正走早退分支**时递增：

```c
if (secondPass != 0 && installed)
{
    InterlockedIncrement64(&g_S206EarlyReturns);
    status = STATUS_SUCCESS;
    goto Exit;          // ← 正常展开 C 帧
}
```

**6 个核全部走到了这里。**

**结论：guest 恢复后确实回到了 `SvVirtualizeProcessor`，并沿着正常返回路径展开了 C 帧。**

这正是参考项目 `iretq` 能安全工作的前提 —— **现在这个前提在我们这里成立了。**

---

## 三、同时推翻了 9.547 的一个判断

9.547 写的是「RealGuestMode 现在连启动都崩了，障碍从"停不下来"变成"一跑就崩"」。

**那个判断是错的 —— 崩溃是我自己的探针造成的。**

9.546 在 `SvHandleVmExit` **第一行**加了：

```c
if (InterlockedIncrement(&g_S206GuestFirstExitTaken) == 1) { ... }
```

**每个 VMEXIT 都执行，在 DISPATCH/HIGH_LEVEL、GIF 已清的状态下。**

关掉它之后（`if (0 && ...)`），**RealGuestMode 立刻正常运行，`early-returns=6`。**

> **我的诊断代码本身成为了被测系统的故障源。**
> 这是本树第 9 次同一个形状：观测行为改变观测对象。
> 前 8 次记在 8.4 与第 13 节；这一的新形态是「在热路径上加 interlocked」。

---

## 四、`exits=0` 是什么

`guest-start` 报 `exits=0`、`no exit recorded yet` —— 因为记录它的代码正是被 `if (0 && ...)` 关掉的那段。

**这不是故障**，是预期的：探针关了，自然没有读数。

**而 `early-returns=6` 用的是另一条路径（`g_S206EarlyReturns`，在早退分支里递增），它没有被关。**

---

## 五、现在的位置

**重构方案的阶段 A / A2 / C 已全部达成：**

| 阶段 | 状态 |
|---|---|
| A: 显式 secondPass 标志 | ✅ |
| A2: per-core 标志（disagrees 4→0）| ✅ |
| **C: guest 接管 C 帧（early-returns 6）** | ✅ **本轮达成** |
| D: DPC 扇出停止 | 未做 |
| E: iretq 出口 | 未做 |

**而 D/E 现在第一次有了成立的前提。**

---

## 六、下一步必须先做的事

**RealGuestMode 现在能启动、能跑、C 帧能展开，但还没有验证"能不能停"。**

而按已知信息：**guest RIP = `0xFFFFF801BDB2531F`** —— 这不是桩地址，是
`ContextRecord->Rip`，即 `SvVirtualizeProcessor` 内部。

**这意味着停止时 `SvLV20` 的 `jmp rbx` 会跳到…… 哪里？**
`rbx` 在 206 分支里被设成 `Sv206LeaveToHost`（一个 `ret`），
而那个 `ret` 会从 `HostExitRsp` 指向的栈弹出返回地址 —— **而那个栈此刻是谁的？**

**必须测 `sc stop`。** 这是下一个决定性实验。
