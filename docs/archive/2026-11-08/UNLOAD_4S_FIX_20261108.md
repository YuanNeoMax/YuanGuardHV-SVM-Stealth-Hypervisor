# 9.556 卸载 4 秒 → 0.04 秒：一个从 9.443 起就存在的不可满足条件

> 2026-11-08。**实测 0.046 / 0.046 / 0.037 秒（基线 1.88–4.12 秒）。**

---

## 一、结果

```
基线（9.555 之前）: 1.88 / 1.93 / 2.07 / 2.19 / 2.84 / 3.19 / 4.12 秒
9.555 之后        : 2.844 / 2.137 / 4.118 秒      ← 没变
9.556 之后        : 0.046 / 0.046 / 0.037 秒      ← 快 40–100 倍
```

`invmrun at unload = 0x0` 依然干净。

---

## 二、问题是什么

`main.c:5121`：

```c
stopped = yghv_s206_vmrun_stopped_count();
while ((ULONG)stopped < active && waited < 20000) {
    yghv_s206_kick_parked_cores();
    for (spin = 0; spin < 200000UL; spin++) { }
    waited += 1;
    stopped = yghv_s206_vmrun_stopped_count();
}
```

`main.c:4993`：

```c
ULONG active = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);
```

**`active` = 12（逻辑处理器数）。而 206 只虚拟化 6 个核。**

**`stopped` 最多只能到 6，永远小于 12 —— 条件不可满足。**

每次卸载都要跑完 `20000 × 200000 = 4×10⁹` 次空循环，约 2–4 秒。

---

## 三、为什么两次修复都需要

| 版本 | 改动 | 为什么不够 |
|---|---|---|
| **9.555** | ASM 在 `Sv206LeaveToHost` 里 `lock inc g_S206CoresExited` | 计数器变诚实了，但**上界是 12**，仍然不可满足 |
| **9.556** | 上界改为 `yghv_s206_vpd_count()`（实际 VPD 数 = 6）| **这才让条件可满足** |

**9.555 是必要的（没有它计数器永远是 0），但 9.556 才是让等待真正结束的那一步。**

**这个 bug 从 9.443 起就存在**，一直被"超时后继续执行"掩盖 ——
功能上没出问题，但每次卸载都在空转 4 秒。

---

## 四、怎么定位到的

**关键的一步是：9.555 改完后【时间没变】。**

如果时间变快了，我会认为 9.555 解决了问题，`active` 的错误就不会被发现。

> **"修了但没有效果"这个结果本身，是找到真正原因的唯一线索。**

随后逐行核对 `main.c` 的等待条件，才发现 `active` 来自
`KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS)` —— 那是**逻辑 CPU 数**，
和"被虚拟化的核数"是两个不同的量。

---

## 五、改动内容

### 9.555（asm）

```asm
; Sv206LeaveToHost，sub rsp,28h 之前
lock inc dword ptr [g_S206CoresExitedAsm]
```

`lock inc` 是单指令、内存操作数、**不碰任何寄存器** —— 所以 `rax`（`PsTerminateSystemThread`）
和 `rcx`（`STATUS_SUCCESS`）原样保留，9.445 证明正确的调用序列一字未动。

### 9.556（C）

```c
{
    extern int yghv_s206_vpd_count(void);
    int vpds = yghv_s206_vpd_count();
    ULONG waitFor = (vpds > 0) ? (ULONG)vpds : 0;
    yghv_ut_mark_val(58, (uint64_t)waitFor);

    while ((ULONG)stopped < waitFor && waited < 20000) {
        ...
    }
}
```

**VPD 数为 0 时等待直接跳过** —— 那是"没有核被虚拟化"，9.442 的注释已称之为正常结果。

---

## 六、验证

```
构建     : ALL SOURCE PASS / ALL MAP PASS / Build SUCCESS
启动     : RUNNING
健康     : sys-health degraded=0 total=0 fatal=0
计数     : entries=6  early-returns=0  cores-marked=6  disagrees=0
卸载     : 0.046 / 0.046 / 0.037 s（三次）
判据     : s206 invmrun at unload = 0x0
bin      : 8F3D546665D951A7
```

---

## 七、这个发现的意义

**这是本会话唯一一个真正落地的性能改进，而且它不是优化的结果，是纠错的结果。**

驱动一直在做一件不可能成功的事，然后靠超时兜底。**兜底让它看起来能用，但代价是每次卸载 4 秒。**

**"靠超时兜底"是一种隐蔽的缺陷** —— 它不会崩，不会报错，只是慢，而且慢得足够稳定，
会被当作"这功能就这样"。
