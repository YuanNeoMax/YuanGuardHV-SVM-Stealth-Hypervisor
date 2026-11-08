# 下一阶段目标：无痕内存读写引擎（Stealth Memory R/W）

> 设定日期：2026-10-11
> 目标等级：**只能超过，超越** —— 不是"能用"，而是"比现有方案更强"
> 前置状态：v0.3.0 已发布（周期同步守卫，206c25_mmcopy）

---

## 零、目标陈述（北极星）

**为【需要跨进程读写目标内存的操作者】在【内核驱动已加载且目标进程受保护】的处境下，
达成【不经过任何 NT 内存 API（`MmCopyVirtualMemory` / `WriteProcessMemory` /
`NtReadVirtualMemory`）即可完成全类型读写与特征码扫描】的可观测结果。**

**核心命题**：把内存访问路径从「**API 调用**」下沉到「**页表 + 物理帧**」，
使这条路径在**调用栈、API 追踪、ETW 事件**三个观测面上**都不出现**。

---

## 一、设计哲学：为什么"绕过 API"才是本质

### 1.1 现有引擎的暴露面（必须消除的）

```
当前路径（Engine A）：
  yghv_sync_write_page()
    → MmCopyVirtualMemory()          ← ① 可被 hook/追踪的内核 API
      → MiDoPoolCopy / memcpy        ← ② 调用栈可查
      → KeStackAttachProcess 类操作   ← ③ CR3 切换可观测（ETW/DPC）
```

**三个观测面**：
| 观测手段 | 能看到什么 |
|---|---|
| SSDT/inline hook | `MmCopyVirtualMemory` 被调用 |
| ETW / 内核回调 | `KeStackAttachProcess` 引起的 CR3 写 |
| 栈回溯 | `MiDoPoolCopy` ← `MmCopyVirtualMemory` ← `yghv_*` |

### 1.2 新引擎的路径（Engine B）

```
目标路径：
  yghv_mem_read(pid, va, buf, len)
    → 取 target->cr3（已在注册时缓存）      ← 无 CR3 切换
    → 手工四级遍历：PML4 → PDPT → PD → PT   ← 纯算术，无 API
    → 得到 PA
    → 按 PA 读物理帧                         ← 唯一的"访问"，且不经 NT 内存 API
```

**观测面对比**：

| 观测手段 | Engine A | **Engine B** |
|---|---|---|
| SSDT hook | 命中 `MmCopyVirtualMemory` | **无命中**（不调用任何内存 API） |
| inline hook | 同上 | **无命中** |
| 调用栈 | `MiDoPoolCopy` ← `MmCopyVirtualMemory` ← 我们 | **栈上无 NT 内存函数** |
| CR3 切换 | `KeStackAttachProcess` 可观测 | **零切换**（遍历用缓存的 cr3 值，不切地址空间） |
| ETW 内存事件 | 可能触发 | **不触发** |

> **关键洞察**：真正的"无痕"不是"擦除痕迹"，而是**根本不产生痕迹** ——
> 不调用被监视的 API，就不会在那些钩子上留下记录。

---

## 二、架构：两套引擎并存 + 按场景切换

### 2.1 引擎抽象层

```c
/* 统一的引擎接口 —— 上层（IOCTL/命令）不感知具体引擎 */
typedef enum {
    YGHV_ENGINE_AUTO = 0,   /* 自动选择（按启发式规则） */
    YGHV_ENGINE_API  = 1,   /* A: NT API 路径（MmCopyVirtualMemory） */
    YGHV_ENGINE_PTE  = 2,   /* B: 页表遍历 + 物理帧 */
} yghv_engine_t;

typedef struct {
    NTSTATUS (*read )(PEPROCESS proc, uint64_t va, void *buf, SIZE_T len, SIZE_T *done);
    NTSTATUS (*write)(PEPROCESS proc, uint64_t va, const void *buf, SIZE_T len, SIZE_T *done);
    const char *name;
} yghv_mem_engine_t;

extern const yghv_mem_engine_t g_engine_api;   /* A */
extern const yghv_mem_engine_t g_engine_pte;   /* B */
```

### 2.2 切换策略（AUTO 模式的启发式）

| 场景 | 选择 | 理由 |
|---|---|---|
| 目标页在 **系统空间**（内核 VA） | A | B 的遍历对内核 VA 无优势；A 更稳 |
| 目标页 **跨页边界**且页数多 | A | B 需逐页遍历，A 一次拷贝 |
| 目标页 **≤4KB 且已注册** | **B** | 单页遍历最快、零 API |
| **需要无痕**（显式指定） | **B** | 唯一选择 |
| **目标进程已挂起/半死** | A | B 依赖页表有效；A 有内核兜底 |
| 页表项显示 **未 present**（换出） | A | B 无法处理缺页（见 §5 限制） |
| **大页（2MB/1GB）命中** | **B** | B 直接算 PA，**比 A 少一次遍历**（优势） |

**显式切换**：新增命令 `mem-engine <auto|api|pte>`，可强制指定。

### 2.3 关键设计：Engine B 的读原语

```c
/* 读一个物理页（PA 已知）—— 这是 Engine B 唯一的"访问"动作 */
static NTSTATUS yghv_pa_read(uint64_t pa, void *dst, SIZE_T len);
static NTSTATUS yghv_pa_write(uint64_t pa, const void *src, SIZE_T len);
```

**实现选择（三选一，需实测决定）**：

| 方案 | 机制 | 优点 | 缺点 |
|---|---|---|---|
| **B1** `MmCopyMemory(MM_COPY_MEMORY_PHYSICAL)` | 内核 API，按 PA 拷贝 | 已在本项目验证可用（206c25）；不切 CR3 | **仍是 API**（但比 `MmCopyVirtualMemory` 低一档：无进程语义） |
| **B2** `MmGetVirtualForPhysical` + 解引用 | 直接拿 VA 读写 | 最快，零 API | ⚠️ **只对系统页有效**（9.299 FIX-2 教训）；用户页会崩 |
| **B3** 自建物理内存映射 | `MmMapIoSpace` / 自管 PTE | 完全自主，可绕过一切 | 复杂、风险高、需处理 PAT/缓存属性 |

**决策**：**B1 为默认，B3 为终极目标（可选）**。
- B1 已在本项目**验证可用**（206c25 的同步守卫就用它读物理页）
- B2 **禁止**用于用户页（三次蓝屏的教训）
- B3 作为后续可选升级（"超越"的加分项）

> **注意**：B1 虽然仍是 API，但它**不携带进程语义** ——
> 监视"跨进程内存操作"的工具看不到它（`MmCopyMemory` 是通用的物理内存访问）。
> 这已经达成了"绕过 `MmCopyVirtualMemory` 调用路径"的核心命题。

---

## 三、全类型读写支持

### 3.1 类型层设计

```c
typedef enum {
    YGHV_T_U8, YGHV_T_U16, YGHV_T_U32, YGHV_T_U64,
    YGHV_T_I8, YGHV_T_I16, YGHV_T_I32, YGHV_T_I64,
    YGHV_T_FLOAT, YGHV_T_DOUBLE,
    YGHV_T_STR_ASCII, YGHV_T_STR_UTF16, YGHV_T_STR_UTF8,
    YGHV_T_BYTES,
    YGHV_T_VEC3,   /* float[3] —— 游戏常用（坐标） */
    YGHV_T_MAT4,   /* float[16] —— 游戏常用（变换矩阵） */
} yghv_mem_type_t;
```

### 3.2 统一 IOCTL 接口

```
IOCTL_YGHV_MEM_READ   (0x815)   /* 读：{pid, va, type, count} → {data} */
IOCTL_YGHV_MEM_WRITE  (0x816)   /* 写：{pid, va, type, count, data} */
IOCTL_YGHV_MEM_SCAN   (0x817)   /* 特征码扫描 */
IOCTL_YGHV_MEM_ENGINE (0x818)   /* 引擎切换 + 诊断 */
```

**输入结构（读）**：
```c
typedef struct {
    uint32_t pid;
    uint32_t type;        /* yghv_mem_type_t */
    uint64_t va;
    uint32_t count;       /* 元素个数（str 时为最大字节数） */
    uint32_t engine;      /* 0=auto 1=api 2=pte */
    uint32_t flags;       /* 预留 */
    uint32_t _pad;
} yghv_mem_read_req_t;   /* 32 字节 */
```

### 3.3 客户端命令

```powershell
# 类型化读写
yghv_ctl.ps1 mem-read  <pid> <hex_va> <type> [count]     # 输出值
yghv_ctl.ps1 mem-write <pid> <hex_va> <type> <value>     # 写值
yghv_ctl.ps1 mem-dump  <pid> <hex_va> <len>              # hex dump

# 引擎控制
yghv_ctl.ps1 mem-engine [auto|api|pte]                   # 读/设引擎
yghv_ctl.ps1 mem-bench  <pid> <hex_va> [iters]           # 两引擎性能对比

# 扫描
yghv_ctl.ps1 mem-scan <pid> <pattern> [--type bytes|str|float]
                       [--writable] [--executable] [--align N]
                       [--max N] [--engine auto|api|pte]
```

**类型别名**（便于手输）：
```
u8 i8 u16 i16 u32 i32 u64 i64 f32 f64 str wstr utf8 bytes vec3 mat4
```

---

## 四、跨进程特征码扫描（Engine B 的杀手级应用）

### 4.1 为什么 Engine B 在此碾压 Engine A

**Engine A 的扫描**：对每个候选地址调一次 `MmCopyVirtualMemory`
→ 1GB 区域 = 262144 次 API 调用 → **慢且噪声大**（每次都是可观测的 API 调用）

**Engine B 的扫描**：
```
1. 用目标 cr3 遍历页表，枚举所有 present 的用户页（一次遍历，纯算术）
2. 对每个页：算 PA → 读物理帧（一次 MmCopyMemory，4KB）
3. 在本地缓冲里做模式匹配（SIMD 可用）
4. 命中才记录 VA
```

**优势**：
- **遍历一次页表**就知道哪些页有效（跳过未映射区，不浪费 API 调用）
- **按页读**而非按候选地址读 → 调用次数 = 页数（而非候选数），**降低 2–3 个数量级**
- **页表项自带属性**（可写/可执行/大页）→ 可按属性过滤，**不读无用的页**

### 4.2 扫描请求结构

```c
typedef struct {
    uint32_t pid;
    uint32_t pattern_len;      /* 1..64 */
    uint8_t  pattern[64];
    uint8_t  mask[64];         /* 通配符掩码（0x00 = 通配） */
    uint32_t max_results;
    uint32_t flags;            /* bit0=writable bit1=executable bit2=include_large */
    uint64_t align;            /* 结果对齐（0=任意） */
    uint64_t range_start;      /* 0 = 全用户空间 */
    uint64_t range_end;
} yghv_mem_scan_req_t;
```

**返回**：命中地址数组（VA 列表）+ 总数。

### 4.3 性能预估

| 场景 | Engine A | **Engine B** | 提升 |
|---|---|---|---|
| 扫描 100MB 可写区域 | ~25600 次 API | **~25600 页读**（同量级，但每次更轻） | 1–3× |
| 扫描 4GB 全空间 | ~1M 次 API | **遍历 + 仅读 present 页**（通常 <500MB） | **5–20×** |
| 大页命中（2MB） | 每页单独读 | **一页覆盖 512 个候选** | **~100×** |

> **"超越"的具体体现**：大页感知。Engine A 对 2MB 大页区域仍需逐地址访问，
> 而 Engine B 一次遍历就知道这是大页 → 一次读 2MB → 内部扫描。

---

## 五、风险与限制（必须诚实前置）

### 5.1 已知的硬限制

| 限制 | 影响 | 处置 |
|---|---|---|
| **缺页（未 present）无法处理** | B 遍历到 `!present` 就返回失败 | **回退 A**（A 有内核缺页处理） |
| **页换出到页面文件** | 同上 | 回退 A |
| **COW 页** | B 读到的是**共享的原始页**（非进程私有副本） | ⚠️ **语义差异，必须文档化** |
| **页表本身被换出** | 遍历失败 | 回退 A（`MmCopyVirtualMemory` 会处理） |
| **目标进程退出中** | 页表可能失效 | 加进程引用 + 存活检查 |

### 5.2 ⚠️ COW 语义差异（**最需要警惕的**）

```
场景：目标进程 fork/COW 后，某页是"写时复制"
  Engine A（MmCopyVirtualMemory）：走内核路径，可能触发 COW 分裂 → 读进程私有副本
  Engine B（页表遍历）：读到 PTE 指向的物理帧 → 可能是共享的原始页
```

**处置**：
- 文档化这个差异
- AUTO 模式对 **COW 可能性高**的场景（只读映射、共享内存）**倾向 A**
- 提供 `--strict` 标志强制 A

### 5.3 三次蓝屏的教训必须应用

| 教训 | 本设计的应对 |
|---|---|
| `ExAcquireFastMutex` 不可递归 | 引擎实现里**不取 `g_protect_lock`**（用独立锁或原子） |
| `MmGetVirtualForPhysical` 只管系统页 | **Engine B 禁用 B2 方案**（只用 `MmCopyMemory` 按 PA） |
| `/EHs-c-` 下 SEH 失效 | **不用 `__try`**，全部用返回状态码 |
| 物理地址跨进程不唯一 | **B 用 (进程, VA) 定页，PA 只在单次操作内有效** |

### 5.4 安全边界

- 读写**只对已注册的 target**（`set-target` 后的进程），不做任意 PID 注入
- 扫描**默认只扫用户空间**（不碰内核地址）
- 所有命令**需要管理员**（设备对象已有 ACL）

---

## 六、实施阶段（分五步，每步可独立验证）

### Phase 1：页表遍历库（**基础，无风险**）

**产出**：`hv/mem_pte.c` + `hv/common/mem_pte.h`

```c
/* 遍历上下文 —— 复用已注册的 target cr3 */
typedef struct {
    uint64_t cr3;
    PEPROCESS process;      /* 引用已持有 */
    /* 统计（诊断用） */
    uint64_t walks;
    uint64_t large_1gb;
    uint64_t large_2mb;
    uint64_t small_4k;
    uint64_t not_present;
} yghv_pte_ctx_t;

int  yghv_pte_init(yghv_pte_ctx_t *c, PEPROCESS proc);
uint64_t yghv_pte_va_to_pa(yghv_pte_ctx_t *c, uint64_t va);
int  yghv_pte_query(yghv_pte_ctx_t *c, uint64_t va, yghv_pte_info_t *out);
```

**复用**：`yghv_protect_guest_va_to_pa` 已有正确的四级遍历逻辑（含 1GB/2MB），
把它**提取成独立模块**并扩展（加属性查询、统计）。

**验证**：`mem-bench` 对比 A/B 得到的 PA 是否一致（**用 A 当基准**）。

### Phase 2：物理帧读原语（**低风险**）

**产出**：`yghv_pa_read` / `yghv_pa_write`（基于 `MmCopyMemory`）

**验证**：对**已知内容**的页（`mmf-hold` 的种子页）读，比对值。

### Phase 3：类型化读写接口（**中风险**）

**产出**：`IOCTL_YGHV_MEM_READ/WRITE` + PS 命令 + 引擎切换

**验证**：
- 每种类型读写往返一致（u8/i32/f64/str/bytes…）
- A/B 两引擎结果一致
- 跨页边界读写正确

### Phase 4：特征码扫描（**中高风险，最复杂**）

**产出**：`IOCTL_YGHV_MEM_SCAN` + `mem-scan` 命令

**验证**：
- 在已知进程里搜已知字符串，命中地址正确
- 通配符/掩码生效
- 属性过滤（writable/executable）生效
- 大页区域不漏扫

### Phase 5：性能基准 + 超越证明（**收官**）

**产出**：`mem-bench` 报告

**必须证明的"超越"**：
1. **功能超越**：全类型 + 扫描 + 双引擎（A 引擎没有扫描）
2. **性能超越**：B 引擎在大页/大范围场景**快于 A**
3. **无痕超越**：调用栈上**不出现 NT 内存 API**（用 kd 或栈抓取证明）

---

## 七、验收标准（可测量断言）

| # | 断言 | 测量方式 |
|---|---|---|
| 1 | A/B 两引擎对同一 (pid, va) 读出的值**逐字节一致** | 随机 1000 个地址比对 |
| 2 | 全部 16 种类型读写往返一致 | 每类型 100 次往返 |
| 3 | 跨页边界读写（4KB 边界两侧）正确 | 边界测试用例 |
| 4 | `mem-scan` 在已知进程找到已知字符串 | 命中地址 == 预期 |
| 5 | **B 引擎调用栈上无 `MmCopyVirtualMemory`** | 栈抓取证据 |
| 6 | **B 引擎零 CR3 切换** | 统计 `KeStackAttachProcess` 调用数 == 0 |
| 7 | 大页区域扫描**不漏**且**快于 A** | bench 对比 |
| 8 | 引擎切换命令生效且可观测 | `mem-engine` 读回 |
| 9 | 缺页/换出场景**回退 A 不崩** | 构造缺页页测试 |
| 10 | 加载→读写→扫描→卸载**全程零蓝屏** | 四步验证流程 |

---

## 八、文件规划

```
新增：
  YuanGuardHV/hv/mem_pte.c              # 页表遍历（Engine B 核心）
  YuanGuardHV/hv/mem_engine.c           # 双引擎抽象 + AUTO 选择
  YuanGuardHV/hv/mem_type.c             # 类型化读写
  YuanGuardHV/hv/mem_scan.c             # 特征码扫描
  YuanGuardHV/hv/common/mem_engine.h    # 接口定义
  docs/MEM_ENGINE_DESIGN.md             # 本设计（细化版）

修改：
  hv/common/control_ioctl.h             # +0x815..0x818
  hv/control_device.c                   # 4 个新 IOCTL 分支
  hv/protect.c                          # Engine A 适配到统一接口
  tools/yghv_ctl.ps1                    # +6 命令
  tools/yghv_client/{README.md,YghvCtl.java}  # parity
  tests/{ioctl_parity,command_parity}.ps1      # 门禁同步

测试脚本（D:\aaaaaavm\）：
  f1_pte_walk.ps1      # Phase 1-2：遍历 + PA 读
  f2_types.ps1         # Phase 3：全类型
  f3_scan.ps1          # Phase 4：扫描
  f4_bench.ps1         # Phase 5：性能
  f5_stealth.ps1       # Phase 5：无痕证明
```

---

## 九、与现有成果的关系

| 现有 | 本阶段如何用 |
|---|---|
| `yghv_protect_guest_va_to_pa`（四级遍历） | **直接提取复用**，扩展属性查询 |
| `MmCopyMemory` 物理读（206c25 已验证） | **成为 Engine B 的读原语** |
| `mmf-hold`（不自写目标） | Phase 1–3 的**受控测试介质** |
| `wpm-read`（纯读探针） | **Engine A 基准**，用于 A/B 比对 |
| 周期同步守卫 | 与本阶段**正交**，可并存（守卫管"保护"，引擎管"访问"） |
| `KERNEL_API_CHECKLIST.md` | **写代码前必过**（三次蓝屏的教训） |
| `/MAP` 构建产物 | 崩溃时一次定位函数 |

---

## 十、风险控制（吸取三次蓝屏）

**开发纪律（硬性）**：
1. **写代码前**：过一遍 `KERNEL_API_CHECKLIST.md`
2. **每个 Phase**：静态审查 → 最小面 → 单页 → 多页（**不跳步**）
3. **每次加载**：带挂起保护的脚本（`Invoke-Guarded` + 有界 `sc stop`）
4. **测试对象**：只用**受控页**（`mmf-hold`），**绝不用 `scan-pid` 的任意进程页**
5. **每个 Phase 独立可回滚**：不通过就不进下一 Phase

**预计风险点**：
- Phase 1：低（纯算术，复用已验证代码）
- Phase 2：低（复用已验证的 `MmCopyMemory`）
- Phase 3：中（新增 IOCTL 面，注意 parity 门禁）
- Phase 4：**高**（扫描要遍历大量页，需注意 IRQL/超时）
- Phase 5：低（测量）

---

## 十一、"超越"的具体承诺

相对**现有 Engine A（`MmCopyVirtualMemory`）**：

| 维度 | Engine A | **Engine B（本设计）** |
|---|---|---|
| 调用 NT 内存 API | 是（可被 hook/ETW 观测） | **否**（栈上无 NT 内存函数） |
| CR3 切换 | 有（attach） | **零** |
| 大页处理 | 透明（内核做） | **显式**（可 1GB/2MB 一次读） |
| 类型化读写 | 无（裸字节） | **16 种类型** |
| 特征码扫描 | 无 | **有**（页表驱动，属性过滤） |
| 引擎切换 | 无 | **AUTO/API/PTE 三态** |
| 性能（大范围） | 逐地址 API | **页级 + 大页感知** |

**不是"能读写"，而是"用一条没人监视的路径读写，并且更快"。**

---

## 十二、待你确认的决策点

1. **读原语选型**：默认 `MmCopyMemory`（B1，已验证）？还是要我尝试 B3（自建物理映射，更彻底但风险高）？
2. **COW 语义**：接受 B 读到共享页（文档化 + `--strict` 强制 A），还是要求 B 也处理 COW？
3. **扫描范围**：默认只扫用户空间（安全），还是要支持内核空间扫描？
4. **实施节奏**：一次做完五个 Phase，还是每个 Phase 交付后等你确认？
5. **版本目标**：这轮完成后打 **v0.4.0**？

---

**我的建议**：
- 读原语用 **B1**（已验证、零风险），B3 作为后续可选
- COW **文档化 + 可选 `--strict`**（不强行处理，避免引入新的崩溃面）
- 扫描**只扫用户空间**（内核扫描风险不成比例）
- **分 Phase 交付**（每步可验证，避免"一次写完发现崩"）
- 目标 **v0.4.0**

**理由**：三次蓝屏的教训是"**步子迈大了**"。这次每步都小、都可验证、都可回滚。
