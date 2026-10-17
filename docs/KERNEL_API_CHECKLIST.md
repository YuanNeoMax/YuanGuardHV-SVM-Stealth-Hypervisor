# 内核 API 契约核对清单（写代码前必过）

> 来源：2026-10-11 本轮**三次蓝屏**的根因分析。
> 三次都是同一个模式：**我按"看起来对"的直觉写了内核代码，没有核对 API 契约**。
> 这份清单是给下一个窗口（和未来的我）的**写前检查表**。
> 位置：`D:\yuanguard\docs\` 同级参考，也见 `YUANMOD_HANDOFF_CURRENT.md` 9.304。

---

## 一、锁：`ExAcquireFastMutex` **不可递归**

**事故**：`yghv_protect_sync_arm` 在 `arm_page_locked` 里被调用，而后者已被
`yghv_protect_start_locked` 持锁 → 同线程二次获取 → 永久自旋 → guest 三重故障
→ `0xE2 MANUALLY_INITIATED_CRASH`（`BUGCHECK_P1=0x20601`）。

**规则**：
- `ExAcquireFastMutex` / `ExAcquireFastMutexUnsafe` **都不可递归**。
  同线程重复获取 = 死锁，**没有检测、没有报错**。
- **新增任何取锁函数前，先查调用链上是否已持锁。**
- 命名约定（本项目既有）：`xxx_locked()` = **假定调用者已持锁**；
  `xxx()` = 自己加锁后委托。
- 拆分范式：
  ```c
  static int foo_locked(...);              /* 调用者持锁 */
  int foo(...) {                           /* 公开入口 */
      ExAcquireFastMutex(&g_lock);
      int r = foo_locked(...);
      ExReleaseFastMutex(&g_lock);
      return r;
  }
  ```

**推论**：**从持锁函数里调用"会自己加锁"的函数**，是新代码最易犯的死锁。

---

## 二、物理地址：**跨进程不唯一**

**事故**：`yghv_sync_write_page` 用 slot 的 `gpa` 去 `g_protect.targets`
反查 `target_va`，再 attach 到那个 target 写回。**但 gpa 是物理地址，
多个进程可映射同一物理页** → 可能拿到 A 进程的 VA 却 attach 到 B 进程
→ `memmove` 目标无效 → `0x7E`。

**规则**：
- **物理地址不是全局唯一标识**。用它反查虚拟地址时，必须同时确认**属主进程**。
- 更根本：**（进程, VA）才是成对有效的标识**。能不用 gpa 就不用。

---

## 三、用户页访问：`MmGetVirtualForPhysical` **只管系统页**

**事故**：用它取目标**用户页**的 VA，然后解引用 → `movups` 访问违规
→ `0x3B SYSTEM_SERVICE_EXCEPTION`。

**规则**：
- `MmGetVirtualForPhysical` 只映射**系统已有直接映射的物理页**
  （自己的页表、非分页池…）。**用户进程的页没有这种映射。**
- 读任意物理内存 → `MmCopyMemory(..., MM_COPY_MEMORY_PHYSICAL, ...)`
  （**注意参数是 `MM_COPY_ADDRESS`，不是 `PHYSICAL_ADDRESS`**）
- 跨进程读写 → `MmCopyVirtualMemory(源进程, 源VA, 目标进程, 目标VA, 长度, KernelMode, &已拷)`
  —— **不需要 attach**，返回 NTSTATUS，不依赖 gpa 唯一性。
- **本项目既有正确写法**：`protect.c:yghv_protect_resolve_va_for` 用
  `MmCopyVirtualMemory`；`protect.c:yghv_pt_read` 用 `MmGetVirtualForPhysical`
  读**页表页**（系统页，正确）。

---

## 四、SEH：**`/EHs-c-` 下 `__try/__except` 完全不生成处理器**

**事故**：我给 `RtlCopyMemory` 包了 `__try/__except` 以为能接住访问违规，
实际 fault 直接变 bugcheck。

**证据**：
```
CFLAGS = /nologo /O2 /kernel /GR- /EHs-c- /Zl /GS-
二进制里搜不到 __C_specific_handler
```
`/EHs-c-` 关闭了 C++ 异常，MSVC 不再为 `__try` 生成 SEH 处理器。
**`__try/__except` 在这个构建下是装饰性的。**

**规则**：
- **不要指望 `__try/__except` 兜底**。要防 fault，用**返回状态的 API**
  （`MmCopyVirtualMemory` 而非裸 `RtlCopyMemory`）。
- 若确实需要 SEH，得改编译选项（会牵动全局，不轻动）。
- **验证方法**：`grep __C_specific_handler` 二进制；搜不到就说明 SEH 没编进去。

---

## 五、非对齐结构字段：**不要直接解引用**

**事故**：`KEY_VALUE_PARTIAL_INFORMATION.Data` 是**柔性数组**，位于结构偏移 20，
**未 16 字节对齐**。`*(ULONG*)Data` 被编译器向量化成 `movaps` → `#GP` → `0x7E`。

**规则**：
- 结构里的柔性数组（`Data[1]`、`Buffer[1]`）**不要直接解引用**。
- 用 `RtlCopyMemory` 拷到**对齐的局部变量**再读。
- **同时校验 `Type` 和 `DataLength`** —— 注册表值可能类型不符或长度不足。
- 通用：`#pragma pack` 的结构、协议解析、驱动间通信结构，都要检查对齐。

---

## 六、注册表：`ZwCreateKey` **不创建中间路径**

**事故**：硬编码 `...\Services\yuanguard\Parameters`，但测试服务名是 `yghva6`
→ 父键不存在 → 创建失败 → 持久化静默失效。

**规则**：
- 驱动应该用 `DriverEntry` 传入的 `RegistryPath`（`PUNICODE_STRING`），
  **不要硬编码服务名**。
- 本项目 `main.c` 原本把它丢了（`(void)r`），9.304 起改为
  `yghv_protect_set_registry_path(r)`。
- **`ZwCreateKey` 失败是静默的** —— 要检查返回值，别 best-effort 到底。

---

## 七、IRQL 与 attach

**规则**：
- `KeStackAttachProcess` **要求 PASSIVE_LEVEL**。
- `ExAcquireFastMutex` 把 IRQL 抬到 **APC_LEVEL**。
- → **attach 必须在锁外**。范式：锁内取 (进程, VA) 并 `ObReferenceObject`，
  放锁，再 attach/操作，最后 `ObDereferenceObject`。
- 更好：直接用 `MmCopyVirtualMemory`，**不需要 attach**。

---

## 八、进程生命周期

**事故（静态审查发现，未崩）**：slot 存 `PEPROCESS` 裸拷贝，
而 watchdog 会 `ObDereferenceObject` + `RtlZeroMemory` → 悬垂指针。

**规则**：
- **任何跨调用保存的 `PEPROCESS` 都必须自己 `ObReferenceObject`**，
  并在用完后 `ObDereferenceObject`。
- **更简单**：不保存指针，每次用时在锁内取 + 取引用。
- `cleanup` 里的释放顺序：**先停线程（join），再拆结构**。
- **清理函数即使"看起来没事"也要走完**（不要 `if (!x) return;` 早退导致泄漏）。

---

## 九、编译与调试基础设施

**已加**（9.304）：`build.bat` 链接 `/MAP`。
```bat
set "LINKS=%LINKS% /MAP:"%BIN_DIR%\yuanguard_hv.map""
```
**用处**：崩溃时 `模块+偏移` 一次查表就能定位函数。
```powershell
# 用 map 把 RVA 映射到函数
$m = Get-Content bin\yuanguard_hv.map
$prev=$null
foreach($line in $m){
  if($line -match '^\s+0001:([0-9a-f]{8})\s+(\S+)\s+([0-9a-f]{16})\s+f'){
    $rva = [Convert]::ToInt64($Matches[1],16) + 0x1000
    if($rva -gt $TARGET_RVA){ "落在: $($prev.Sym)"; break }
    $prev = [pscustomobject]@{ Sym=$Matches[2]; Rva=$rva }
  }
}
```
**本轮前三次崩溃都没有 map**，全靠手工反汇编 + 读 `.pdata` 定位，极其费时。

---

## 十、测试纪律（**测试 bug 会伪装成驱动故障**）

**本轮 5 个测试 bug**，全都曾让我误判：
1. **用 `scan-pid` 的任意进程页做被测对象** → 那些页目标自己在改 →
   触发回滚路径 → 那正是崩溃点。**必须用受控页**（mmf 映射）。
2. **目标寿命 < arm 耗时** → 目标先死，后续全空读 → 误判 FAIL。
   （64 页 arm 要 140 秒，每次 `protect-page` 约 2.19 秒 = PS 进程启动开销）
3. **`$R` / `$r` 变量名冲突** —— **PowerShell 变量名不区分大小写**！
4. **`$Pages-1`** 被解析成数组减法 → 要写 `($Pages - 1)`。
5. **`param()` 未声明 `$Arg4`** → 第 4 个位置参数被拒。

**规则**：
- **判据异常时，先怀疑测试脚本，再怀疑驱动。**
- 被测对象必须**行为可控**（不自写、寿命足够、地址明确）。
- 长时间操作前**先估算耗时**并据此设置目标寿命。
- **PowerShell 变量名大小写敏感度 = 不敏感**，避免单字母大小写配对。

---

## 十一、加载驱动前的检查顺序（实操流程）

1. **静态**：锁纪律 → IRQL → 生命周期 → 内存/对齐 → 编译（看有无 implicit）
2. **最小面**：加载 + 线程 + IOCTL + 卸载（**不 arm 任何页**）
3. **单页**：受控页，跑够时间
4. **多页**：受控页 ×N
5. **上限**：容量上限 ×N
6. 每步都用**带挂起保护的脚本**（`Invoke-Guarded` + 有界 `sc stop`）

**本轮的教训**：跳过 1–4 直接上多页 = 连续三次蓝屏。

---

## 十二、崩溃取证流程（已验证有效）

```powershell
# 1. 读 BugCheck 事件
Get-WinEvent -FilterHashtable @{LogName='System'; Id=1001} -MaxEvents 3

# 2. 分析 dump（落盘，避免输出被截断）
kd -z C:\Windows\Minidump\<newest>.dmp -y srv*C:\symbols*https://msdl.microsoft.com/download/symbols -c "!analyze -v; q"

# 3. 关键字段：BUGCHECK_CODE / P1..P4 / SYMBOL_NAME / MODULE_NAME / PROCESS_NAME
# 4. 用 map 把 MODULE+offset 映射到函数
# 5. 用 dumpbin /disasm 看崩溃镜像的机器码
# 6. 用 IAT 解析确认 call qword ptr [xxx] 调的是谁（见 iat_map.ps1）
```

**`BUGCHECK_P1` 往往直接编码了原因**：
- `0xE2 P1=0x20601` → `svm_simplevm206.cpp` 的 SHUTDOWN 路径（guest 三重故障）
- `0x3B/0x7E P1=0xc0000005` → 访问违规
- `0xD1 P1=0` → 空指针

---

**本文件位置建议**：`D:\yuanguard\docs\KERNEL_API_CHECKLIST.md`
（或在 `YUANMOD_NEXT_WINDOW_PROMPT.md` 里引用）

---

## 十三、API **配套关系**必须查文档（第七次教训：0x1A / PFN 数据库损坏）

**事故**：`yghv_selfmap_init` 连续三次蓝屏 `0x1A` / `0x1240`（PFN 数据库损坏）。

**错误的模式**：把两个"看起来相关"的 API 拼在一起，没核对各自的契约。

```c
// 我写的（错）
va  = MmAllocateMappingAddress(size, tag);   // 保留地址：无物理页
MmInitializeMdl(mdl, va, size);
MmBuildMdlForNonPagedPool(mdl);              // ← 契约违反：要求非分页池地址
locked = MmMapLockedPagesSpecifyCache(mdl, ...);  // 用垃圾 PFN 建 PTE
// 然后还手工改 PTE 的 PFN 字段 ← 绕过引用计数
```

**三个 API 的真实契约**：

| API | 契约 | 正确配套 |
|---|---|---|
| `MmAllocateMappingAddress` | 保留系统 VA 范围，**无物理页** | `MmProbeAndLockPages` + `MmMapLockedPagesWithReservedMapping` |
| `MmBuildMdlForNonPagedPool` | 输入 VA 必须**在非分页池** | 用 `ExAllocatePoolWithTag(NonPagedPool...)` 分配的内存 |
| `MmMapLockedPagesSpecifyCache` | 会**正确增减 PFN 引用计数**，但 `map`/`unmap` 必须**成对** | 每次用完立即 `MmUnmapLockedPages` |

**为什么 `mem_pa.c` 同样"手工填 PFN"却安全**：
```c
MmInitializeMdl(假VA) → 手工填PFN → MmMapLockedPagesSpecifyCache
  → 写 → **立即 MmUnmapLockedPages**      ← map/unmap 平衡
```
而 selfmap 是"**保持映射 + 手工改 PTE**" —— **两头都破坏契约**。

**不可行性的结论**：
```
要"零 API 访问任意物理帧"  → 必须手工改 PTE
要"不破坏 PFN 引用计数"    → 必须经过内存管理器的 map API
两者不可兼得（除非完整实现 PFN 引用计数管理 = 内存管理器内部契约）
```

**规则**：
1. **任何 `Mm*` API 组合前，先查两个 API 的文档**，确认"谁产出什么、谁消费什么"
2. **MDL 的 `map` 与 `unmap` 必须成对**；需要长期映射时用 `MmMapLockedPagesWithReservedMapping`（不是 SpecifyCache）
3. **永远不要手工改写 PTE** —— PFN 引用计数由内存管理器拥有
4. 崩溃码 `0x1A` + `P1=0x1240` = **PFN 数据库损坏**（比访问违规更严重，有数据损坏风险）
---

## 十四、NPT 表项的 flags 必须完整（第九次教训：0x0A / IRQL_NOT_LESS_OR_EQUAL）

**事故**：`npt-remap` 实验导致 `0x0A`（P1=页对齐用户地址，P2=IRQL 2）。

**根因**：`npt_map_page(m, g, spa, flags)` 内部：
```c
pt[p0].all = (spa & ~0xFFF) | (flags & 0xFFF) | (flags & (1ULL<<63));
```
**不在 `flags` 里的位全部被丢弃。** 传 `NPT_PERM_PRESENT | NPT_PERM_WRITABLE`
（0x3）会丢掉 bit2(user) 和 bit5(accessed) → NPT 层触发 #NPF →
岛内 handler 用 `MmGetVirtualForPhysical`（对用户页返回野指针）→ 0x0A。

**正确**：用 `NPT_4K_PAGE_FLAGS`（0x27 = present|writable|user|accessed），
与 `npt_split_2mb_to_4kb` 内部的 `common` 掩码同理。

**规则**：
1. 调用任何 `npt_map_page` 前，确认 flags 含完整属性集，**不要只传权限位**
2. `npt_get_pt` / `npt_get_pd` 用 `MmGetVirtualForPhysical`，实质 **PASSIVE-only**
   → 加 `KeAreAllApcsDisabled()` 检查
3. **`0x0A` 的 P2 直接就是 IRQL**，不需要 kd 分析
4. **kd 在本机不可用**（两次事故：管道损坏、挂起）→ 只用 dump 参数 + 代码审查
---

## 十五、★ kd 用**本地符号**完全可用（第十次教训：误判导致 2 次无谓崩溃）

**误判**：以为 kd 在这台机器上不可用（两次挂起后放弃）。
**真相**：挂起的是**符号服务器联网**，不是 kd。

```bash
# 可用（本地 38 个内核符号，90 秒内稳定返回）
kd -z <dmp> -y C:\symbols -c "k; q"        # 栈回溯
kd -z <dmp> -y C:\symbols -c "r; q"        # 寄存器
kd -z <dmp> -y C:\symbols;C:\ -c "u yghv_g16+0x19d17; q"   # 反汇编
kd -z <dmp> -y C:\symbols -c "dt nt!_CONTEXT 0xffffab8c...; q"   # 异常上下文

# 会挂起（符号服务器超时）
kd -z <dmp> -y "srv*C:\symbols*https://msdl.microsoft.com/download/symbols" -c "!analyze -v; q"
```

**关键技巧**：
1. **`-y C:\symbols`**（本地）而不是 `srv*...`
2. **短命令**：`k` / `r` / `u` / `dt`，不要 `!analyze -v`
3. **超时保护**：`Start-Job` + `Wait-Job -Timeout 90`
4. **反汇编自己的驱动**：把镜像放回 `C:\<svcname>.sys`，
   然后 `-y C:\symbols;C:\` + `u <drvname>+0x<RVA>`
5. **拿异常时的寄存器**：0x3B 的 P3 是 CONTEXT 地址，
   `dt nt!_CONTEXT <P3>` 能看到故障现场的 Rcx/Rdx/R11 等

**代价**：这个误判让我白崩了 2 次（9.311 之后纯靠猜）。

**规则**：崩溃后**第一件事**是用本地符号 kd 取证，
不要猜，也不要因为一次工具失败就放弃工具。
---

## 十六、★ direct map **不覆盖全部物理内存**（第十一次教训）

**Phase 6（9.310）曾断言"直接映射路径打通"。这个结论只在低地址成立。**

```
kd 证据（2026-10-17 22:59, 0x50 PAGE_FAULT_IN_NONPAGED_AREA）
  base = 0xFFFFF77FFA8A1000   （由 KUSER_SHARED_DATA VA-PA 推出）
  P1   = 0xFFFFF783504B8000 = base + 0x355C17000   (13.34 GB)
  yghv_pa_write+0x6A -> memcpy+0x17
```

| PA 范围 | direct map 可用 |
|---|---|
| 低地址（如 KUSER_SHARED_DATA 87 MB） | ✅ 9.310 验证通过 |
| 13+ GB | ❌ 0x50 |

**原因**：Windows x64 的 direct map 不保证覆盖 `[0, RAM_SIZE)` 全区间，
中间存在未映射/MMIO 区域。

**规则**：
1. **不要用 direct map 写任意 PA** —— 只对**已知在低地址**的帧可用
2. 写 PA 用 **MDL 路径**（`MmMapLockedPagesSpecifyCache`），Phase 2-7 全验证
3. 若要用 direct map，必须先做**范围检查**（但我们没有可靠的上界）
4. **Phase 6 的岛内读也应视为"仅低地址可用"**

**为什么 MDL 更可靠**：MDL 让内存管理器按 PFN 建映射，
不依赖 direct map 的覆盖范围。

---

## 十七、★ 五次崩溃后仍未解决的问题（Tier 4.4）

**现象**：目标页在进程正常运行时是可写的私有页；bp 撤销其 NPT 写权限后，
无论走 MDL 还是 direct map，写同一物理帧都失败（0x3B / 0x50 各一次以上）。

**四个假设全部证伪**（每个一次崩溃）：
1. `GuestPaOfGhcb` 未设 → 设了还崩
2. Engine A 走 guest NPT → 拒绝非 PTE 还崩
3. `npt_set_page_perm` 打到 2MB PDE → 先 split 还崩
4. 假 VA 的 MDL 不可写 → 换 direct map 崩得不一样

**未建模的交互**：NPT 权限撤销 ↔ 宿主对该帧的写能力。

**结论**：在建立这个模型之前，**不要重新启用 Tier 4.4**。
试错的代价是每次一个重启。
---

## 十八、★ 分阶段返回码：一次运行定位失败原因（第十二次教训）

**问题**：`bp_add` 返回 0，但不知道是哪个守卫拒绝。只能靠猜 → 每次猜错崩一次。

**做法**：让失败路径返回**不同的负值**，每个值对应一个守卫：

```c
if (KeAreAllApcsDisabled())              return -1;  /* IRQL */
if (yghv_mem_engine_get() != PTE)        return -2;  /* engine */
if (!yghv_protect_npt_split_4k(page))    return -3;  /* split */
if (!yghv_protect_npt_set_perm(page,..)) return -4;  /* perm */
/* table full */                          return -5;
```

IOCTL 层用 `out[0] = (UINT64)(INT64)ok;` 原样传出，PS 层写成 switch 打印。

**效果**：第一次运行就打印出
`mem-bp: add FAILED (stage -2: engine is not PTE)`，
直接指向 bug，而不是靠又崩一次来缩小范围。

**规则**：**任何有多重前置检查的操作，失败时必须报告"哪一步"**，
不要只返回 bool。诊断成本 >> 代码成本。

---

## 十九、★ 引擎选择被每次读写重置（9.326，潜伏极久的 bug）

```c
/* control_device.c，注释写得很清楚： */
/* "Engine 0 means use the current selection — do NOT overwrite it with AUTO,
 *  or a mem-engine <sel> set earlier would be undone by the very next
 *  read/write." */
if (req_engine <= YGHV_ENGINE_PTE_SELFMAP)        /* ← 0 也满足！ */
    yghv_mem_engine_set((yghv_engine_t)req_engine);
```

`req_engine` 默认 0，`0 <= 3` 恒真 → **每次 mem-read/write 都 set(AUTO)**，
把先前 `mem-engine pte` 的选择清掉。

**潜伏原因**：所有旧测试都在**每次 write 前重新设一次引擎**，
所以从未暴露。只有"设一次 → 中间夹一次读 → 再检查选择"的路径才会踩到。

**修复**：`if (req_engine != YGHV_ENGINE_AUTO && req_engine <= SELFMAP)`

**教训**：**注释和代码不一致时，信注释的意图，检查代码的条件**。
这个 bug 的注释完全正确，代码却与它相反，而注释被写在那里恰恰说明
作者知道这个陷阱 —— 只是条件写错了。