# YuanGuardHV OS-as-Guest 常驻可行性研究（2026-08-14，代码级）

> 关联：`docs/YUANMOD_HANDOFF_CURRENT.md` 9.84-9.102（历史冻结）、9.149-9.152
> （冻结隔离实验 + 独立 CR3 修复）、9.157-9.158（本窗口有界试点重开 + step20 0xCE）。
> 本文档回答：**OS-as-guest 常驻在本机（Ryzen 5 5500）为何必然冻结、能否用
> 驱动代码修复、换平台后怎么重开。**

---

## 1. 结论速览

| 问题 | 结论 |
|---|---|
| OS-as-guest **有界试点**（step12/14/16）本机能否跑？ | ✅ 能（本窗口已全 PASS，机制完好） |
| OS-as-guest **常驻**（step17+/99/100）本机能否跑？ | ❌ 不能——guest 必须共享宿主 CR3，触发与 9.152 相同的 TLB 别名 CPU 锁死 |
| 9.152 的独立 guest CR3 修复能否套用到 OS-as-guest？ | ❌ 不能——OS-as-guest 的 guest 是 Windows 本身，必须用宿主完整地址空间 |
| 有没有**未测试**的驱动级杠杆？ | ✅ 有一个：**ASID/TLB 卫生**（每核唯一 ASID + 每次 VMRUN FLUSHBYASID）。当前代码 `guest_asid=1`、`tlb_control=0`、全程零 INVLPGA，从未隔离测试过 |
| 最终路线 | 本机只做有界验证；常驻需换平台 + ASID/TLB 卫生验证 |

---

## 2. 冻结根因（重述 + 代码定位）

### 2.1 9.152 已证实：共享宿主 CR3 → TLB 别名 → CPU 锁死

- 9.149-9.151 四组隔离实验排除：NPF/#DB/TLB 风暴、核数（1 与 12）、VMEXIT 率
  （~1M/s 与 ~120k/s）、C 状态。所有共享宿主 CR3 配置均在 **15-60s 内**全核锁死。
- 9.152：给合成 guest 构建**独立最小页表**（仅映射 guest 代码页）后 12 核稳定
  5+ 分钟。根因 = guest 以 guest ASID 翻译宿主内核地址空间，与宿主 TLB 条目
  （同一 VA→PA）别名，触发本机 CPU 全核锁死（无 bugcheck/转储/MCE，纯硅片级）。

### 2.2 OS-as-guest 路径的代码事实（本窗口核实）

- 入口：`yghv_baremetal_step_test(YGHV_BAREMETAL_STEP)` → `yghv_os_guest_*_thread`
  → `svm_prepare_vcpu` + `svm_trampoline_os_enter`。
- `svm_prepare_vcpu`（svm_core.c:281-419）关键设置：
  - `ctrl->np_enable = 0`（初始关闭，随后线程各自 `svm_core_set_npt` 开启）
  - `state->cr3 = yg_read_cr3()`（**宿主当前 CR3**）
  - `ctrl->guest_asid = 1`（**所有核固定 1**）
  - `ctrl->tlb_control = 0`（VMRUN 不刷 TLB）
- 各 `yghv_os_guest_*_thread` 仅调用 `svm_core_set_npt(core, g_npt.pml4_pa)`
  （NPT identity，GPA=SPA），**从不覆写 CR3、从不改 ASID、从不 INVLPGA**。
- 结果：guest 页表 = 宿主页表，guest TLB（ASID=1）与宿主 TLB（ASID=0）对
  同一 VA→PA 并存 → **与 9.152 完全相同的别名场景**。

### 2.3 为什么 9.152 的独立 CR3 修复不能用于 OS-as-guest

- 独立 CR3 只映射少量合成代码页（main.c 4221-4269），guest 只能在映射页内跑。
- OS-as-guest 的 guest 是 **Windows 调度器/内核本身**，需要访问整个 Windows
  地址空间（进程切换 → 全内核 VA 活动）。不可能用最小页表；给 Windows 造一个
  完整独立页表 = 影子页表，等价重写整个虚拟化层，且仍无法消除 TLB 别名
  （同一 VA→PA 在 ASID 0/1 下并存是映射内容决定的，与页表根指针无关）。

### 2.4 有界试点 PASS vs 常驻冻结的机理差

- 有界试点（12/14/16）**同样共享宿主 CR3**，但 guest 只做 5000 轮 cpuid/rdtsc +
  计数，数秒内触发 exit 上限 → host_done → 线程结束。**未积累到触发 CPU bug 的
  TLB 压力即退出** → PASS。
- 常驻线：guest 持续跑 Windows 调度器/上下文切换，TLB 别名持续积累 15-60s →
  触发 CPU 锁死。

---

## 3. 未测试的驱动级杠杆：ASID/TLB 卫生（换平台必测）

当前代码：`guest_asid=1`（全核共享）、`tlb_control=0`、无 INVLPGA/FLUSHBYASID。
AMD SVM 规范下，重复使用 ASID 必须先按 ASID 刷 TLB（tlb_control=1 或 INVLPGA），
否则陈旧 guest TLB 条目与宿主条目长期并存。**9.149-9.151 与 9.152 都未隔离
"ASID/TLB 卫生"这个变量**——独立 CR3 修复改变了映射内容，但 ASID 一直是 1、
从不按 ASID 刷。

可测试假设（本机亦可试，但冻结风险 + 需重启预案）：
1. **每核唯一 ASID**（`guest_asid = core+1`，宿主 0 保留）：消除跨核 ASID 复用
   带来的陈旧条目。
2. **每次 VMRUN `tlb_control = FLUSH_BY_ASID`（=1）**：进 guest 前按 ASID 刷，
   避免 guest/host 同 VA→PA 长期并存。
3. 宿主侧 VMEXIT 后如需，INVLPGA 刷 guest ASID。
4. NPT 改为**非 identity**（guest-physical 重映射到另一段 SPA）——但 OS-as-guest
   guest 页表内是宿主 PA，非 identity 会破坏 guest 内存访问，**仅对合成 guest
   有意义，OS-as-guest 不可行**（除非同时改写 guest 页表=影子页表）。

> 注意：这些杠杆是否真的能让 OS-as-guest 常驻在本机存活，**无理论保证**——
> 若 CPU bug 是"ASID 0/1 下同 VA→PA 并存即锁死"（与刷不刷无关），则 ASID 卫生
> 无效，只能换平台。因此建议**在换来的平台上先测**，避免在本机反复冻结。

---

## 4. 本机已确认的边界（9.157-9.158）

| 验证项 | 结果 |
|---|---|
| step12 单核有界 | PASS（counter=5000） |
| step14 全核有界 | PASS（12 核×10000） |
| step16 全核无缝有界 | PASS（12 核×10000） |
| step20 spin 常驻运行 | PASS（resident alive 35s+） |
| step20 `sc stop` 卸载 | ❌ 0xCE（OS-as-guest 线程未注册 join 表 + spin 不查停止标志） |

---

## 5. 换平台重开 OS-as-guest：准备清单 + 风险分析

### 5.1 前置条件
- [ ] 目标机 AMD-V（或 VT-x）可用，裸机（非 VMware 嵌套；嵌套下 MSRPM/IOPM/
      CR0 受限，见 YUANMOD_NEXT_WINDOW_PROMPT 雷区）。
- [ ] 构建环境：WDK 10.0.19041.0 + clang-cl + MSVC link（build.bat 已备好）。
- [ ] 测试签名（testsigning on）或已签名驱动。
- [ ] **重启预案**：OS-as-guest 常驻不可安全卸载（历史 + 0xCE 实证），每次常驻
      实验后需重启清除。

### 5.2 有界试点先行（零/低风险，确认机制完好）
1. `$env:YGHV_BAREMETAL_STEP='12'; cmd /c build.bat` → 部署 → `sc start` →
   核对 `os guest counter=0x1388` → `sc stop` → 恢复稳定版。
2. step14 → step16，核对每核 exits=0x2710、counter=0xea60。

### 5.3 常驻线（每次需重启预案）
1. **先测 ASID/TLB 卫生假设**：在 step17/20 基础上实现
   `guest_asid=core+1` + 每 VMRUN `tlb_control=1`（FLUSH_BY_ASID），加载 step20
   spin 常驻，观察 `resident alive` 能否超过 60s（旧基线必冻）→ 若能，说明 ASID
   卫生有效，再推进阻塞常驻（step17）；若仍冻，判定为硅片级，放弃本机/该 CPU。
2. **修复 0xCE 卸载**（无论平台）：把 OS-as-guest 线程注册进 multi_core 的
   `g_resident_threads` join 表，并给 spin guest 加停止标志（host_done 退出），
   使 `sc stop` 干净卸载。
3. 全核阻塞常驻（step99/100）最后测，预期即便 ASID 卫生有效也需逐核验证。

### 5.4 风险清单
| 风险 | 等级 | 缓解 |
|---|---|---|
| CPU 全核锁死（无转储） | 高（本机已证） | 换平台 + 重启预案 + 看门狗就位 |
| 卸载 0xCE | 中（已证，可修） | 注册 join 表 + 停止标志 |
| START_PENDING 挂起（9.145） | 中 | 逐核/有界先行 |
| VM 嵌套功能受限 | 高（若误用 VM） | 裸机部署 |

---

## 6. 结论

1. **OS-as-guest 常驻在本机必然冻结**：guest 必须共享宿主 CR3 → TLB 别名 →
   CPU 锁死；9.152 的独立 CR3 修复对 OS-as-guest 不可用。
2. **有界试点（12-16）是本机可安全验证的上限**，已全 PASS。
3. **换平台重开的正确顺序**：有界试点 → ASID/TLB 卫生实验（唯一未测变量）→
   修复 0xCE 卸载 → 阻塞常驻。
4. 若 ASID/TLB 卫生在换来的平台上仍冻结，则 OS-as-guest 常驻需要真正的
   影子页表/NPT 非 identity 方案（大幅重写），或改用非透明 OS-as-guest 架构。
