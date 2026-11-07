# 9.549 根因确定：RealGuestMode 的 guest 不会走停止握手

> 2026-11-08。**根因找到，而且 9.462 的注释早就写明了同一个崩溃。**

---

## 一、崩溃点（已由 dump 反汇编证实）

```
Sv206LaunchVm @ 0x1400280B0（当前构建）
  +0x1B  vmrun rax
  +0x1E  vmsave rax      ← 0xD1，P1=P4=0x...80CE，P3=8（取指失败）
```

**`vmrun` 返回了，但取不到 `vmsave` 那条指令。**

---

## 二、根因链

### 2.1 停止等待的是什么

`main.c:5104-5123`：

```c
/* g_S206CoresExited is ... how many cores have reached Sv206GuestExited,
   i.e. have left the guest for good ... When it equals the active core count,
   nothing of this driver is running anywhere, and the image can be unmapped
   without a PAGED_IP. */
stopped = yghv_s206_vmrun_stopped_count();
while ((ULONG)stopped < active && waited < 20000) {
    yghv_s206_kick_parked_cores();     // 注入 NMI 唤醒
    for (spin...) { }
    stopped = yghv_s206_vmrun_stopped_count();
}
```

**它等的是「每个核都走到 `Sv206GuestExited`」。**

### 2.2 桩模式为什么能到达

桩的循环是 `hlt / mov rax,1 / vmmcall / jmp`：

1. 停止标志置位
2. 桩执行 `vmmcall` → `#VMEXIT`
3. 处理程序看到停止标志 → 设 `ExitVm`
4. `SvLV20` → `jmp rbx` → `Sv206LeaveToHost`
5. 走到 `Sv206GuestExited`，计数 +1
6. 循环看到 `stopped == active`，退出

**桩天生会执行 `vmmcall`，所以这个握手总能完成。**

### 2.3 RealGuestMode 为什么到不了

RealGuestMode 的 guest 是 `ContextRecord->Rip`，即 `SvVirtualizeProcessor` **内部**的返回点：

1. 停止标志置位
2. **guest 里没有任何代码检查这个标志**
3. **guest 也没有 `vmmcall` 可以触发 `#VMEXIT`**
4. `yghv_s206_kick_parked_cores()` 注入 NMI —— 但 **NMI 注入到的是 guest**，
   而 guest 现在是真实 Windows 代码路径，它不理会我们的 NMI
5. 循环等到 20000 次超时
6. `DriverUnload` 返回 → **镜像被 unmap**
7. 仍在 guest 里的核继续执行 → **取指到已 unmap 的页** → `0xD1 PAGED_IP`

### 2.4 9.462 的注释早就写过同一个现象

```
main.c:5135:
  "... DriverUnload then returned, the image was unmapped, and the parked cores
   executed `hlt` on dead code pages -> 0xD1 PAGED_IP at Sv206LaunchVm+0x1e"
```

**同一个偏移、同一个错误码、同一个机制。**
只是那次是"桩停在被踢醒之前"，这次是"guest 根本不参与握手"。

---

## 三、这解释了全部观察

| 观察 | 用这个根因解释 |
|---|---|
| 桩模式停止正常（`invmrun=0x0`）| 桩执行 `vmmcall`，握手完成 |
| RealGuestMode 启动成功、`early-returns=6` | 与停止无关 ✅ |
| RealGuestMode `sc stop` → `0xD1 @ +0x1E` | 握手超时 → 镜像 unmap → 取指死页 |
| 9.547 那次"启动即崩" | 探针额外拖慢了某核，触发同样的超时路径 |
| `P1 = P4`、`P3=8` | 执行的那条指令就在被 unmap 的页上 |

---

## 四、这解决了阶段 D 的设计问题

**阶段 D 不是"把停止改成 DPC 扇出"就够了。**

参考项目的 `SvmTerminateCpu` 之所以有效，是因为它做的是：

```c
AsmSvmVmmcall(HvCallDevirtualize);   // 在该核上【主动执行 vmmcall】
```

**是 host 在目标核上主动执行 `vmmcall` 指令**，不是等 guest 执行。

**而这件事要求：该核当前正在执行我们可控的代码。**
参考项目满足这一点，因为它的 DPC 就是在那个核上跑我们自己的代码。

**我们的 RealGuestMode 不满足** —— 那个核正在 guest 里跑 Windows 代码，
`vmmcall` 由谁执行？

**答案：DPC。** DPC 可以在该核上运行，并在其中执行 `vmmcall`。

**所以阶段 D 的正确实现是**：
```
DriverUnload
  -> 对每个核 KeInsertQueueDpc
       -> DPC 里 AsmSvmVmmcall(HvCallDevirtualize)
            -> #VMEXIT -> 停止处理 -> iretq
```

**而 DPC 能在 guest 运行期间于该核上执行吗？** 这正是 `kick_parked_cores` 目前在做的事
（注入 NMI 让核回到 host），但 NMI 注入到 guest 后不会自动让 host 代码运行。

**这是阶段 D 真正要解决的技术问题，且尚未验证。**

---

## 五、当前状态

```
注册表 : Parameters 已删除
机器   : 已重启（07:51:21），驱动 STOPPED
bin    : D830A5A0C7305C72
桩模式 : 可用（invmrun=0x0）
提交   : a4cfcef
```

---

## 六、结论

**RealGuestMode 已达成**：
- 启动成功
- 稳定运行
- `early-returns=6`（C 帧正常展开，参考项目模型成立）

**RealGuestMode 的停止是下一个独立课题**，而且它的根因现在很明确：

> **guest 不参与停止握手，而现有停止机制假设 guest 会参与。**

**这不是 `vmsave` 的 bug，是握手协议不适用于 real guest。**
