# 9.542 第 1 步成功：iretq 帧槽位（结构体未变，行为未变）

> 2026-11-08。重构四步中的第 1 步完成。**这是三轮失败后第一次成功。**

---

## 一、和前一次（9.541）的差别

| | 9.541（失败）| **9.542（成功）** |
|---|---|---|
| 做法 | 在 `HostStackLayout` **末尾加字段** | **只定义类型 + accessor**，不加成员 |
| union 大小 | **大一页**（因 `DECLSPEC_ALIGN(PAGE_SIZE)` 向上取整）| **完全不变** |
| `static_assert` | 失败 → 我改成了 `+PAGE*4` 让它过 | **保持原值 `+PAGE*3` 就通过** |
| GuestVmcb 偏移 | 后移一页 → 汇编常量全错 | 不变 |
| 结果 | `0x1E` / `C000001D` 非法指令 | **正常运行** |

**关键区别：`static_assert` 没有被我改。**

它保持原值通过，这本身就是"结构体没变"的编译期证明 ——
而 9.541 里我改 assert 让它通过，那等于**关掉了唯一的检查**。

---

## 二、帧放在哪

`StackContents` 是 `VIRTUAL_PROCESSOR_DATA` 联合体里一个 **11.6 KB 的字节数组**，
**全树没有任何代码引用它**（只有定义，零读零写，已 grep 验证）。

RSP 指向 `GuestVmcbPa`（即 `TrapFrame` 的顶部），所以 **`StackContents` 的最后几个字节
就是 RSP 正下方的空闲栈**：

```
StackContents[ ..................... ][ Rip Cs Rflags Rsp Ss ]
                                     ^ sizeof(StackContents) - sizeof(S206_IRET_SLOT)
```

**地址是算出来的，不是手写的**：

```c
static PS206_IRET_SLOT Sv206IretSlot(PVIRTUAL_PROCESSOR_DATA vp)
{
    return reinterpret_cast<PS206_IRET_SLOT>(
                &vp->HostStackLayout.StackContents[
                    sizeof(vp->HostStackLayout.StackContents) - sizeof(S206_IRET_SLOT)]);
}
```

如果 `StackContents` 将来改大小，这个 accessor 自动跟着变。
**写死偏移会在别的地方正中间 —— 那正是 9.541 失败模式的缩小版。**

---

## 三、验证（全部通过）

```
构建     : ALL SOURCE PASS / ALL MAP PASS / Build SUCCESS
struct   : static_assert 原值通过 → 大小确实未变
启动     : RUNNING
健康     : sys-health degraded=0 total=0 fatal=0
退出计数 : 2 个非零码，sum=159,400（与之前一致）
性能     : 12 线程 2.112 s（基线 3.104 s）
卸载     : stop 1.93 s，WIN32_EXIT_CODE=0
判据     : s206 invmrun at unload = 0x0   ← 干净
bin      : A64D7C00006E06F2
```

**桩模式完全未受影响。**

---

## 四、这一步证明了什么，没证明什么

**证明了**：
- iretq 帧可以在宿主栈上有一个**固定可算**的位置，而不改变任何布局
- 这条路径不破坏现有卸载（`invmrun=0` 依然干净）

**没证明**：
- `iretq` 本身能不能工作 —— **帧还没被任何人用**
- 那个位置在真实 guest 运行时会不会被别的栈使用踩掉

**第二步（真正的 iretq）仍然有蓝屏风险**，而且按修正后的顺序，
应该先做第 3 步（`Virtualized` 早退），最后才换退出方式。

---

## 五、下一步

按 `docs/REFACTOR_PLAN_RG.md` 的修正顺序：

```
1. ✅ MachineFrame 进栈（本轮完成，零回归）
3. Virtualized 早退        ← 下一个
4. DPC 扇出停止
2. 最后才换成 iretq
```

**第 3 步的风险比第 2 步低**，而且它是 iretq 能成立的前提 ——
参考项目的 `iretq` 之所以不炸，正是因为 `RtlCaptureContext` + `Virtualized` 早退
让那个 C 帧**被 guest 接管而不是被瞬移绕过**。
