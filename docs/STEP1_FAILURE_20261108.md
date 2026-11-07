# 9.541 第 1 步失败：结构体加字段 = 蓝屏 0x1E/0xC000001D

> 2026-11-08。**这一步我以为「只改布局、不改行为」，结果蓝屏。**
> 结论：`VIRTUAL_PROCESSOR_DATA` 不能随便加字段。

---

## 一、意图

给 `HostStackLayout` 末尾加一个 5 qword 的 `IretFrame`，让 `iretq` 能从一个
**栈上固定偏移**的位置取帧（参考项目就是这样做的），而不是像 9.540 那样用全局变量。

**我以为这是安全的**，理由写在了代码注释里：
> "NOTHING WRITES OR CONSUMES THIS YET. Step 1 of the refactor is layout only;
> the exit still `mov rsp,rcx ; jmp rbx`."

---

## 二、结果：`0x1E` KMODE_EXCEPTION_NOT_HANDLED

```
时间  2026-11-08 07:10:59
代码  0x0000001E  KMODE_EXCEPTION_NOT_HANDLED
参数1 0xFFFFFFFFC000001D  = STATUS_ILLEGAL_INSTRUCTION
```

**非法指令 —— 代码执行到了不该执行的地方，说明有东西被写坏了。**

### 编译期就已经提醒过我

`static_assert(sizeof(VIRTUAL_PROCESSOR_DATA) == KERNEL_STACK_SIZE + PAGE_SIZE * 3)` 失败，
我把它改成 `+ PAGE_SIZE * 4` 让它通过了。

**那次 assert 失败是唯一的警告，而我选择了"修 assert"而不是"查为什么变大"。**

更准确地说：`HostStackLimit` 带 `DECLSPEC_ALIGN(PAGE_SIZE)`，加 0x28 字节会让
整个 union 向上对齐到**整整一页**。所以结构体不是"大了一点"，是**大了一页**。

---

## 三、为什么一页就够炸

`VIRTUAL_PROCESSOR_DATA` 是 per-core 的，**它的地址被硬编码进汇编**：

```asm
; svm_simplevm206_x64.asm
mov qword ptr [rcx - 5FD0h + 6110h], rax   ; HostExitRsp
mov rcx, [rsp + 8 * 18 + KTRAP_FRAME_SIZE] ; VpData
```

`0x5FD0` = `HostStackLayout` 在 VPD 里的偏移，`0x6110` = `HostExitRsp` 在 VMCB 里的偏移。
**这些常量是照着旧布局算的。**

同时 `GuestVmcb` / `HostVmcb` / `HostStateArea` 三个页紧跟 union：

```c
union { HostStackLimit[KERNEL_STACK_SIZE]; struct{...} HostStackLayout; };
DECLSPEC_ALIGN(PAGE_SIZE) VMCB GuestVmcb;
DECLSPEC_ALIGN(PAGE_SIZE) VMCB HostVmcb;
DECLSPEC_ALIGN(PAGE_SIZE) UINT8 HostStateArea[PAGE_SIZE];
```

union 大一页 → **后面三个页全部后移一页** → 所有按旧偏移访问 `GuestVmcb` 的
汇编代码读写到了错误的位置 → 写坏执行流 → 非法指令。

---

## 四、正确的做法（如果还要做）

**不能靠"加字段"来扩栈，要靠在既有空间里挖。**

`HostStackLayout` 目前是：

```
StackContents   [KERNEL_STACK_SIZE - 6*8 - KTRAP_FRAME]
TrapFrame       KTRAP_FRAME          ← 已用于调试栈重建（3365/3366 行在填）
GuestVmcbPa     ← RSP 指向这里
HostVmcbPa
Self
SharedVpData
Padding1
Reserved1
```

**`StackContents` 是一大块（约 12KB 减去 TrapFrame 和 6 个 qword）**，而 RSP 指向它的**尾部**。
`SvHandleVmExit` 用 `sub rsp, 80h` 在 RSP 下方建自己的帧 —— 也就是说
**RSP 到 `StackContents` 底部之间有大量未使用的空间**。

**正确的做法**：在 `StackContents` 里预留一段（例如从底部往上 0x30 字节），
用「RSP + 已知负偏移」访问，**完全不动结构体大小**。

---

## 五、这一轮的教训

> **"只改布局不改行为"这句话，只在没人依赖那个布局时成立。**
>
> 我们的布局被**汇编硬编码**依赖着（0x5FD0 / 0x6110 / KTRAP_FRAME_SIZE）。
> 结构体多一页，这些常量全部失效，而它们不会编译报错 —— 只会蓝屏。

**`static_assert` 当时失败了，那不是在碍事，那是在报告真相。**

---

## 六、当前状态

```
源码    : 已 git checkout 回滚
重建    : bin = 1B467656C866299C
部署    : C:\yghv_pa.sys = 1B467656C866299C
冒烟    : RUNNING / degraded=0 / stop 2.07s / invmrun=0x0  ← 全部正常
提交    : 未提交（改动已丢弃）
```

**机器可用，桩模式（1.2×）未受影响。**

---

## 七、如果继续第 1 步，必须改方向

| 原方案 | 问题 | 新方案 |
|---|---|---|
| 在结构体末尾加字段 | 撑大一页，破坏三个页的偏移 | **在 `StackContents` 内部挖空间**，结构体大小不变 |

**新方案的具体做法**：
```c
// 不加字段，改为在既有数组里划一段
UINT8 StackContents[KERNEL_STACK_SIZE - (sizeof(PVOID)*6) - sizeof(KTRAP_FRAME)];
//                                          ^ 从这里往上预留 0x30
```
用 `reinterpret_cast<S206_IRET_FRAME*>(&HostStackLayout.StackContents[0])` 访问 ——
**0 大小变化，0 偏移变化**。

**但这需要先确认 `StackContents[0]` 在运行时没被别的东西用**（目前看没被用过，
它就是那段"RSP 下方的空闲栈"）。
