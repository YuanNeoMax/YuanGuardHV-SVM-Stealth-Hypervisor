# 9.546 阶段 C 诊断：桩模式读数正确，我的"矛盾"判断是错的

> 2026-11-08。**又一次：我没查证就下了结论，实测推翻了它。**

---

## 一、桩模式的实测读数

```
=== A: 桩模式（RealGuestMode 未设置）===
virt-counts: entries=6  early-returns=0  cores-marked=6  disagrees=0
guest-start: realGuestMode=0  writers=6  exits=185393
  guest Rip given     = 0xFFFFF80267E781BD
  guest Rsp given     = 0xFFFFD986D0F57B50
  first #VMEXIT Rip   = 0xFFFFF80267E781C5
```

---

## 二、我一度判定这是"矛盾"，并判断错了

我当时的推理：
> `else` 分支写的是 `&Sv206GuestStub`，但记录到的是 `0xFFFFF802...` 内核地址 ——
> **矛盾**，说明记录点被覆盖或记录到了别的 VMCB。

**这个推理错了。** 我用 map 核对：

```
Sv206GuestStub  RVA  = 0x271BD
记录的 guest Rip     = 0xFFFFF80267E781BD
                    低 16 位 0x81BD —— 完全一致
```

**`0xFFFFF80267E781BD` 就是 `Sv206GuestStub` 的真实内核虚拟地址。**

同样，`first #VMEXIT Rip = 0x...81C5` = 桩地址 **+ 8**，
对应 `hlt`(1 字节) + `mov rax,1`(2 字节起) 之后的位置 ——
**桩确实从给定地址开始执行，并执行到了 `vmmcall`。**

**这两个读数都是正确的，而且互相印证。**

---

## 三、这次错在哪

我假设「桩地址应该长得像 `0x1400281BD`（镜像内 VA）或某个我认得出的形态」，
看到 `0xFFFFF802...` 就判定"不是它"。

**内核驱动加载后的真实 VA 永远是 `0xFFFFF8xx...` 这种形态**，
`0x1400281BD` 只是 PE 里的 ImageBase+RVA，加载时会被重定位。

**我没有查证，只按印象判断 —— 这和之前几次失败是同一个病。**

---

## 四、这次实验真正证明了什么

| 问题 | 答案 |
|---|---|
| 桩模式下 guest 从哪开始？ | **`Sv206GuestStub`** ✅ 与代码一致 |
| 桩是否真的执行了？ | **是**，走了 8 字节到 `vmmcall` ✅ |
| `early-returns` 为什么是 0？ | **因为桩不回到 `SvVirtualizeProcessor`** ✅ 结论成立 |
| 记录机制是否可信？ | **是**，两个读数互相印证 ✅ |

**所以阶段 C 的核心问题仍然存在，而且现在有了直接证据**：
桩模式 `realGuestMode=0`，guest 从桩开始，不回来 → `early-returns=0`。
这是**预期行为**，不是缺陷。

---

## 五、RealGuestMode 的那次启动蓝屏了

```
11-08 07:38:51  0x000000D1  (0xfffff80267ee80fe, ...)
```

**崩溃地址 `0xfffff80267ee80fe`** —— 与桩地址 `0xfffff80267e781bd` **同一内核区域**（都是 `0xFFFFF80267E...`）。

**这提示崩溃发生在驱动自己的代码里**，很可能就是 guest 执行到了不该执行的位置。

但**那次运行没有留下 `guest-start` 读数**（蓝屏太快，IOCTL 没来得及跑）。

---

## 六、还发现一个真实缺陷

```c
extern "C" int yghv_s206_real_guest_mode(VOID)
{
    static volatile LONG cached = -1;    // ← 驱动生命周期内缓存
    ...
}
```

**`static` 缓存意味着：一旦读过一次，改注册表不会生效，必须重新加载驱动。**

这在测试时很容易误判 —— 我上一次测试里如果先跑桩模式再看注册表，会读到缓存值。

**这不是 bug（缓存是合理的设计），但测试时必须知道**：
**改 `RealGuestMode` 后必须 `sc stop` + `sc start`。**

---

## 七、当前状态

```
注册表   : Parameters 键已删除（RealGuestMode 不存在）
bin = 部署 = F194C08AF46D2220（9.546 诊断版，含 guest-start）
git      : 6 个文件未提交
机器     : 已重启（07:38:17），驱动 STOPPED
```

---

## 八、下一步

**阶段 C 要回答的问题现在很明确**：

> RealGuestMode 下，guest RIP 是 `ContextRecord->Rip`（`SvVirtualizeProcessor` 内部），
> 那么 guest 恢复后**是否会走到那条早退分支**？

**但这个问题的答案已经可以从结构上推断**：
`ContextRecord->Rip` 是 `RtlCaptureContext` 的返回点，**在 `SvVirtualizeProcessor` 内部**，
所以恢复后**会**走到 5646 行的判断 —— **前提是它能活到那里。**

而 9.539 那次 RealGuestMode 实测**稳定运行了 30 秒、快 38%**，说明它确实活着。
**那为什么当时没有早退计数？因为 9.543 之前根本没有这个计数器。**

**所以：`early-returns` 在 RealGuestMode 下是否增长，是一个从未被测过的量。**
这次的 `guest-start` 命令正是为了测它 —— 只需要一次不蓝屏的 RealGuestMode 启动。
