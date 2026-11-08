# 9.552 安全封堵：RealGuestMode 编译期固定为关闭

> 2026-11-08。**这是本会话的收尾动作，不是新实验。**

---

## 一、为什么这样做

本会话 07:38–08:09 期间产生了 **4 次 0xD1 蓝屏**，每一次都重启机器，
最后机器启动时出现 BIOS 提示 `Remove disks or other media. Press any key to restart`。

**这些蓝屏全部来自同一个模式：`RealGuestMode`。**
而它是由**一个注册表 DWORD** 点开的。

**一个 `reg add` 就能武装的崩溃路径，不该继续存在。**

---

## 二、做了什么

`yghv_s206_real_guest_mode()` 的第一行改为无条件 `return 0`：

```c
extern "C" int yghv_s206_real_guest_mode(VOID)
{
    static volatile LONG cached = -1;
    ...
    /*
     * 9.552: PINNED OFF AT COMPILE TIME.
     *
     * The registry below is still read (the code path and its comments stay honest),
     * but the answer is discarded. ...
     */
    return 0;

    hit = InterlockedCompareExchange(&cached, -1, -1);   // 不可达
    ...
}
```

**注册表读取代码保留**（这样注释和代码路径仍然诚实），但返回值被丢弃。

---

## 三、验证：编译器把它整个优化掉了

反汇编产物（`yghv_s206_real_guest_mode @ 0x140027870`）：

```
0000000140027870: xor         eax,eax
0000000140027872: ret
0000000140027873: int         3
```

**整个函数变成两条指令。注册表读、字符串比较、缓存全部消失。**

**这不是"运行时判断为 0"，是"这个逻辑在二进制里不存在了"。**

---

## 四、冗余验证：故意把注册表设成 1

```
注册表写入 : RealGuestMode = 1
驱动实测   : realGuestMode=0   early-returns=0
```

**注册表里明确写着 1，驱动报告 0，早退计数仍为 0（桩模式特征）。**

**pin 确实挡住了。**

---

## 五、保留了什么

| 项 | 状态 |
|---|---|
| RealGuestMode 的**实测结果** | ✅ 保留在文档：`early-returns` 0→6 |
| RealGuestMode 的**代码** | ✅ 保留（`return 0` 之前）|
| 9.551 的 `vmmcall` tick | ✅ 保留（沿同一路径，现在不可达）|
| 桩模式 | ✅ 完全可用 |

**测得的事实没有被抹掉，只是那条路被封了。**

---

## 六、验证结果

```
构建     : ALL SOURCE PASS / ALL MAP PASS / Build SUCCESS
pin      : xor eax,eax ; ret（编译器确认）
冗余测试 : 注册表=1 -> 驱动报 0
启动     : RUNNING
健康     : degraded=0 total=0 fatal=0
停止     : 3.19 s，STATE=STOPPED
判据     : s206 invmrun at unload = 0x0   ← 干净
注册表   : 已删除
bin      : 25B1263D918F9DE2
```

---

## 七、关于 BIOS 提示

`Remove disks or other media. Press any key to restart` 是 **BIOS/UEFI 找不到可引导设备**的提示，
不是驱动造成的。

**但它很可能与连续 4 次蓝屏有关**：异常重启会让某些主板（尤其开了 Fast Boot 的）
改变启动顺序，或把残留的 USB 设备排到首位。

**建议（需人工操作）**：
1. 拔掉所有 USB 存储设备
2. 进 BIOS 确认系统盘是第一启动项
3. 暂时关闭 Fast Boot，确认稳定后再开

**按任意键能进系统，说明硬盘和系统完好。**

---

## 八、会话收尾状态

```
机器     : 正常，驱动 STOPPED
注册表   : Parameters 已删除
bin = 部署 = 25B1263D918F9DE2（桩模式，RealGuestMode 编译期关闭）
桩模式   : 可用（1.2×，invmrun=0x0）
```

**后续若要继续研究 RealGuestMode，必须先在代码里把 `return 0` 改回去，
并且只在明确知道这是在重新打开一条已验证会产生蓝屏的路径时才这么做。**
