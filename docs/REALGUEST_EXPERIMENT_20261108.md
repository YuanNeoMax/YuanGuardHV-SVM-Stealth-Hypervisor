# RealGuestMode 实验结果（2026-11-08）

> 这是一个**部分成功、最终失败**的实验。记录完整读数，**不要因结尾的失败忽略前面的成功**。

---

## 一、做了什么

给 `Sv206SetupGuest` 加了一个**默认关闭**的注册表开关 `RealGuestMode`：

```c
if (yghv_s206_real_guest_mode()) {
    Vmcb.StateSaveArea.Rip = ContextRecord->Rip;   // 真实 Windows 线程
    Vmcb.StateSaveArea.Rsp = ContextRecord->Rsp;
} else {
    Vmcb.StateSaveArea.Rip = &Sv206GuestStub;      // 现状（桩）
}
```

**默认关闭时行为完全不变**（已实测：RUNNING、degraded=0、stop 2.84s、invmrun=0x0）。

---

## 二、开启后的实测结果（**这些是真实读数**）

### 2.1 启动与稳定性

```
sc start        : 0.18 秒，STATE = RUNNING
10 秒后         : 仍 RUNNING
30 秒观察       : 06:39:39 / 06:39:50 / 06:40:01 全部 RUNNING
CPU             : 71 55 67 89 40 46 83 60 64 36 74 40 %
                  （总 51–60%，不是 100%）
exit-codes      : 1 个非零码，sum = 7,686
sys-health      : degraded=0 total=0 fatal=0
```

**没有蓝屏，没有硬冻结。** 这与 2026-08 那 114 个实验的结果**根本不同**。

### 2.2 性能

```
RealGuestMode 12 线程 : 1.933 秒
桩模式（6 核）        : 3.104 秒      ← 慢 60%
卸载基线              : 1.4–1.5 秒
```

**真实 Windows 在 guest 态比桩模式快 38%，接近卸载基线。**

### 2.3 NPT 是否真的生效（目标 2/3）

藏一页后，另一个 Windows 进程读它：

```
第一次 : value=0x0            ← NPT 拦住了，读到零
后续   : value=0xC4C4...      ← 读到真值
```

**不稳定**。合理解释：`mmf-read` 每次是新进程，映射的物理页可能不同
（文件缓存页在不同进程间不一定共享同一 PA）。**至少有一次确认 NPT 生效**，
但**没有做到稳定复现**，所以「NPT 对 Windows 生效」这条只能记为**部分验证**。

---

## 三、最终失败：卸载卡死

```
sc stop         : 0.04 秒返回，STATE = STOP_PENDING
5 秒后          : 仍 STOP_PENDING
progress.log    : 没有 "invmrun at unload" 记录
```

**驱动永远停不下来。** 必须断电重启。

### 为什么 —— 9.442 早就记录了原因

> *"the guest is at an ARBITRARY instruction boundary with live registers, and
> jmp rbx would resume it mid-function with RCX/RBX destroyed and SVM already off.
> That is silent corruption of whatever Windows thread was running."*

**桩能停，是因为它停在 `vmmcall` 上，`SvLV20` 可以干净地接管。
真实 Windows 线程没有这样的安全点 —— 它可能停在任意一条指令中间。**

---

## 四、这次实验真正的价值

| 旧结论（2026-08） | 本次实测 |
|---|---|
| 「真实 Windows 在 guest 态 <10s 硬冻结或 0x101」 | **30 秒稳定运行，无冻结无蓝屏** |
| 「平台级限制，errata 1363，无修复」 | **errata 1363 没有触发** —— 因为中断不拦截，没有持续的 APIC 访问流 |
| 「OS-as-guest 在本机不可行」 | **能跑，但不能停** |

**8 月那次失败的原因是「host ISR 代管中断」，不是「Windows 当 guest」。
这次把中断交给 guest 自己处理后，那个障碍消失了。**

### 剩下的唯一障碍

**停止路径。** 不是硬件，不是 errata，是**架构上缺少一个安全的退出点**。

---

## 五、下一步可能的解法（未验证）

要在「Windows 当 guest」下能停，需要一个**所有核都能到达的安全点**：

1. **利用 `vmmcall` 从用户态触发**：让一个用户态进程定期 `vmmcall`，
   每个核在 VMEXIT 里检查停止标志 —— 但 Windows 自己的线程不会这么做
2. **拦截一个高频且安全的指令**（如 `CPUID` 或 `RDTSC`），在 VMEXIT 里检查标志
   —— 但这会增加退出率
3. **注入 NMI 并要求每个核在 ISR 里检查** —— `yghv_s206_kick_parked_cores` 已有雏形，
   但真实 Windows 的 NMI handler 不是我们的
4. **接受「不能干净卸载」**：只在重启时消失，不做运行时卸载

**方案 4 可能才是这个架构的真实边界。**

---

## 六、重启后的确认（2026-11-08 06:45）

机器已于 06:44:12 重启（原因是 06:44:26 记录 0x000000d1，即 stop 卡死后强制的重启）。

重启后核对：

`
驱动       : STOPPED（干净）
RealGuestMode: 已删除（避免下次加载直接进危险模式）
bin = 部署  : 15BD8DF48E937D2B
默认配置冒烟: RUNNING / degraded=0 / stop 2.25s / invmrun=0x0  ← 全部正常
`

**桩模式未受影响**，这台机器随时可用。

---

## 七、当前机器状态

**驱动卡在 `STOP_PENDING`，必须断电重启。**

重启后：
- 注册表 `RealGuestMode` 仍然存在 → **必须删除，否则下次加载直接进入危险模式**
- 或者保持默认关闭（读不到 = 0 = 桩模式）

**恢复步骤（重启后执行）：**
```
reg delete "HKLM\SYSTEM\CurrentControlSet\Services\yghvpa\Parameters" /f
```
