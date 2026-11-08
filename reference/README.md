# reference/ —— 第三方参考实现的源码副本

> **这些不是本项目的代码，也不会被 `build.bat` 编译。**
> 放在这里只有一个用途：**比对实现细节**（尤其是 Intel VT-x 与本项目 AMD SVM 的对应关系）。

---

## 是什么

| 文件 | 说明 |
|---|---|
| `reference_ept.cpp` / `.h` | **Intel VT-x** 的 EPT 实现（92 KB）。与本项目的 NPT 是同一层的对应物 |
| `reference_ept_hook.cpp` / `.h` | EPT hook（32 KB） |
| `reference_vm_exit.asm` | VM-exit 分发（Intel 侧） |
| `reference_vm_context_asm.asm` / `.h` | VM context 切换 |
| `reference_exception_routines.asm` | 异常例程 |
| `reference_interrupt_handlers.asm` | 中断处理 |
| `reference_init_ntoskrnl.cpp` | ntoskrnl 初始化相关（19 KB） |
| `reference_log.cpp` / `.h` | 日志 |
| `reference_protect_drx.cpp` / `.h` | DR 寄存器保护 |

**来源**：Intel VT-x 内核 hypervisor 的公开示例实现，早期为研究
`mem-bp dr`（P1-1）与 EPT/NPT 对应关系而复制进来。原始出处见
`docs/archive/_ARCHIVE_HANDOFF_9.1-9.299.md` 里的引用记录。

## 为什么保留

- **跨厂商对照**：本项目是 AMD SVM/NPT，这份是 Intel VT-x/EPT。
  两者在同一抽象层（页表嵌套、VM-exit 分发、上下文切换）上的**差异**是最有用的部分 ——
  例如 `reference_protect_drx.*` 正是 `mem-bp dr` 那条路线的参考。
- 它们**不参与构建**，所以不会带来任何编译或体积成本。

## 已移出版本控制

`SymbolicAccessKM.lib`（1.1 MB）**已从仓库移除**：

- 它是一个**二进制静态库**，没有被 `build.bat` 或任何源码引用；
- 二进制库进 git 无法 review、无法 diff，且会永久留在历史里；
- 需要它时从原始出处重新获取，不要靠仓库保管。

`git rm --cached` 已执行，文件仍在本机磁盘上。

## 使用注意

- **不要**把这些文件加进 `build.bat` 的编译列表 —— 它们是 Intel 侧代码，
  与 `YuanGuardHV/hv/` 的 AMD SVM 实现**不能混编**。
- 引用其中任何逻辑到本项目时，**先确认厂商语义差异**（EPT vs NPT 的
  A/D 位、permission 编码、invept vs INVLPGA 都不同）。
