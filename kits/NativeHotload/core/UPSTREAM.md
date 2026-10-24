# Pinned core decision policies / 固定版本核心决策策略

These modules replace three small policy decisions reached by real XNU call
sites. They do **not** replace the scheduler, virtual memory manager, VFS,
their object layouts, or their complete resource lifecycles. All kernel
objects and all ownership remain resident.

这些模块接入真实 XNU 调用点，替换三类局部策略决策；不代表调度器、虚拟内存、
VFS 整个子系统或其完整资源生命周期已被热替换。内核对象、布局和所有权始终常驻。

## Source identity / 上游来源

- Repository / 仓库: [apple-oss-distributions/xnu](https://github.com/apple-oss-distributions/xnu).
- Tag / 标签: `xnu-8796.141.3`.
- Commit / 提交: `1b191cb58250d0705d8a51287127505aa4bc0789`.
- Audited local checkout / 本次核对目录:
  `UniXNU-giulia/.cache/modem-xnu-base` (clean at review / 核对时无修改).
- Adaptation date / 适配日期: `2026-10-24`.

SHA-256 below is computed over UTF-8 source with LF line endings, so Windows
checkout newline conversion does not change the identity. Line numbers refer
to the unmodified pinned sources.

下表 SHA-256 基于 UTF-8、LF 换行的完整源文件，避免 Windows 检出换行差异。
行号对应未修改的固定版本源文件。

| Source / 源文件 | SHA-256 (LF) |
| --- | --- |
| `osfmk/kern/sched_prim.c` | `6e3aa6911a36e416f169fa2a121a80b9c653220e273d641c6fbad9a335844a7f` |
| `osfmk/vm/vm_fault.c` | `990814178e80da7c3d7ddff855d497f9b863fdb4a45415e71534cf4e2c75b934` |
| `bsd/vfs/vfs_cluster.c` | `3d62ca2049e1f55f8da9afa5970842dfc256f69b6522222b6e3efb08fbca8156` |
| `APPLE_LICENSE` | `e5881019d8766c1e88a5fe1dbca4ba40c78011d41fcb18f6e9f50df60182685b` |

## Correspondence and boundaries / 对应关系与边界

The original function bodies still calculate a resident fallback before the
typed gateway selects a module result. `native_core_algorithms.h` independently
reproduces those calculations from borrowed, pointer-free snapshots.

原函数体先计算常驻回退值，再通过类型化入口选择模块结果。
`native_core_algorithms.h` 使用临时、无指针的快照独立重现这些计算。

| Upstream step / 上游步骤 | Module calculation / 模块计算 | Resident work / 常驻操作 |
| --- | --- | --- |
| `sched_prim.c:3969`, `thread_quantum_init()` mode switch | `nlh_sched_baseline()` retains the realtime/fixed safety cap and uncapped timeshare branch / 保留实时、固定模式安全上限与分时分支 | Read `realtime.computation` for realtime; call `SCHED(initial_quantum_size)` for fixed/default; read the matching safety limit in the original branch / 按原分支读取初始量子和安全上限 |
| CPU ledger clamp and zero-budget case | Clamp only for nonzero `cpu_remaining` / 仅非零剩余预算参与限幅 | Call `thread_cpulimit_remaining(now)` only when `cpulimit_affects_quantum`; retain ledger sleep behavior / 保留有条件的预算读取及 ledger 休眠行为 |
| Quantum assertion and assignment | Results must be positive, below `UINT32_MAX`, and no larger than baseline; realtime/fixed must equal baseline / 返回值为正、低于 UINT32_MAX、不增大基准；实时与固定模式保持原值 | Retain original assertions and `thread->quantum_remaining` assignment / 保留原断言及线程字段提交 |
| `vm_fault.c:399`, `vm_fault_is_sequential()` object reads and truncation | `nlh_vm_baseline()` truncates offset to page boundary, handles same-page refault, and retains every random/forward/reverse/default transition / 保留页对齐、同页重入及随机、正向、反向、默认全部分支 | Snapshot `last_alloc` and `orig_sequential` with at least the original shared object lock; keep the same-page early return / 沿用原对象共享锁、字段读取与同页提前返回 |
| Sequential bounds and commit | Byte count remains a whole number of pages in `[-MAX_SEQUENTIAL_RUN, MAX_SEQUENTIAL_RUN]`; `NLH_VM_UPDATE` expresses whether the commit path runs / 字节计数保持整页及正负最大范围，标志位区分是否进入提交路径 | Preserve compare-and-swap against the original snapshot, immediate return on CAS failure, and `last_alloc` write only on the original success path / 保留原值 CAS、失败即返回及原有 last_alloc 提交顺序 |
| `vfs_cluster.c:331`, `cluster_max_io_size()` | `nlh_vfs_baseline()` keeps read/write/default capability selection, segment clamp, byte minimum, old transfer floor and page rounding / 保留读、写、默认能力选择、段数上限、字节取小、旧传输下限及页对齐 | Read mount capabilities in the matching original branch; retain callers, UPL ownership, mapping and I/O submission / 按原分支读取 mount 能力；UPL 所有权、映射及 I/O 提交仍由原调用链处理 |
| `vfs_cluster.c:378`, `cluster_max_prefetch()` | Preserve unsigned 32-bit multiply overflow or over-limit cap; the widened product only detects overflow / 保留 32 位无符号乘法溢出或越界即限幅；加宽乘积仅用于检测溢出 | Retain disk-conditioner SSD decision and 32-bit `IO_SCALE()` evaluation before snapshot / 保留 SSD 判断及快照之前的 32 位 IO_SCALE 计算 |

The VM default case also covers upstream behavior values other than the three
explicit random/forward/reverse cases. The adapter may normalize these values
to `NLH_BEHAVIOR_DEFAULT`. Offset addition/subtraction uses unsigned 64-bit
arithmetic, retaining upstream wrap behavior. Forward/reverse explicit behavior
retains the opposite-sign count when advancing; only the default branch resets
the old direction before starting a run in the other direction.

VM 的默认分支覆盖除随机、显式正向和显式反向以外的 behavior；适配器可将它们
统一映射到 `NLH_BEHAVIOR_DEFAULT`。偏移加减沿用 64 位无符号运算及其回绕。
显式正向、反向分支不会主动清除反方向计数；只有默认分支在改变方向时先归零。

Request validation supports resident 4 KiB or 16 KiB pages. A VM snapshot with
an out-of-range or non-page-multiple sequential count is declined, leaving the
original resident decision intact. `MAX_SEQUENTIAL_RUN` is pinned to 1 GiB.
The transfer floor and UPL size originate in
`osfmk/mach/memory_object_types.h`; mount fields are defined as `uint32_t` in
`bsd/sys/mount_internal.h`. This code does not change any of those definitions.

快照校验支持常驻 4 KiB、16 KiB 页。VM 计数超界或不是整页时拒绝调用模块，
使用原常驻决策。`MAX_SEQUENTIAL_RUN` 固定为 1 GiB。传输下限和 UPL 大小来自
`osfmk/mach/memory_object_types.h`，mount 能力字段在
`bsd/sys/mount_internal.h` 中定义为 `uint32_t`；本适配不修改这些定义。

## Policy variation / 策略变化

`policy.c` uses `NLH_POLICY_DIVISOR` from 1 through 16. Divisor 1 reproduces
the resident baseline for accepted snapshots. Other divisors shorten only
timeshare quanta, clamp sequential-run magnitude to a smaller page-aligned
limit, and reduce VFS I/O/prefetch sizes while retaining the original I/O floor.
VM policies cannot reverse a run, invent one after random access, change the
commit flag, or return a fractional-page count. Scheduler realtime/fixed
quanta remain unchanged. The gateway independently validates results before
the original caller commits them.

`policy.c` 的 `NLH_POLICY_DIVISOR` 范围为 1 至 16。1 对有效快照返回常驻基准；
其他值只缩短分时量子、按整页降低连续访问计数上限，以及在保留 I/O 下限的前提下
降低 VFS I/O 或预取大小。VM 策略不得反转访问方向、为随机访问制造连续计数、
改变提交标志或返回不足整页的计数。实时、固定模式量子保持原值。常驻入口在原
调用者提交结果前独立校验返回值。

These are policy experiments, not a claim of scheduling fairness, VM
performance, filesystem throughput, or hardware validation for each divisor.
Reducing a timeshare quantum changes scheduling behavior; reducing sequential
counts or I/O limits changes VM read-ahead and VFS batching decisions.

这些是策略实验；并不表示每个除数均已通过调度公平性、VM 性能、文件系统吞吐或
硬件验证。缩短分时量子会改变调度行为；降低连续计数或 I/O 上限会影响 VM 预读
及 VFS 批量处理。

## Concurrency and migration / 并发与迁移

`NLH_CORE_CALL` runs in its original scheduler/VM/VFS context, sometimes while
locks are held. It borrows one immutable snapshot, uses stack/private state,
and performs no allocation, logging, host calls, dependency calls, I/O or
sleep. Per-module `calls` and `changes` use lock-free 64-bit atomics. This is
not a wait-free guarantee: an ARM64 compiler may lower atomics to LL/SC retry
sequences. Scalar telemetry reads are independent observations, not an atomic
snapshot of all counters.

`NLH_CORE_CALL` 沿用调度、VM、VFS 的原始上下文，可能持锁。只临时借用不可变
快照，使用栈和私有状态，不分配、打印、调用 host 或依赖、执行 I/O 或休眠。
`calls`、`changes` 使用无锁 64 位原子操作；这不构成 wait-free 保证，ARM64
编译器可能生成 LL/SC 重试。标量遥测分别读取各计数，不是全体计数的原子快照。

The resident loader must close admission and drain every accepted call before
`STOP`, `EXPORT` or `IMPORT`; lifecycle callbacks are serialized. Therefore
`running`, `imports` and drained export/import accesses need no concurrent
writers. A successful `STOP` changes only the private running flag. `START`
accepts zeroed, imported or stopped private state. The failing-start build
sets the flag before reporting failure, so the loader must execute partial
start cleanup. A failing stop leaves the state running and requires the
resident loader's quarantine path.

常驻加载器必须先关闭准入并等待所有已接纳调用退出，再执行 `STOP`、`EXPORT`、
`IMPORT`；生命周期回调串行执行。因此 `running`、`imports` 和排空后的导入导出
访问没有并发写入。成功 `STOP` 只清理私有运行标志，`START` 接受零值、已导入或
已停止的私有状态。故意失败的 START 先设置运行标志，再返回错误，用于验证
部分启动清理；失败 STOP 保留运行状态，要求常驻加载器隔离对应镜像。

State ABI 1 exports a 40-byte snapshot: magic `0x3153434e`, byte size, domain,
zero reserved word, then three `uint64_t` counters (`calls`, `changes`,
`imports`). It contains no pointers or XNU objects. Transfer data must be
nonnull, eight-byte aligned, and fit its capacity; ABI, size, domain and
reserved fields are checked before import. Import copies counters into the
stopped candidate and increments `imports`. All counters use unsigned modulo
2^64 arithmetic. Failed import changes nothing; failed candidate start leaves
the stopped old module's counters available for rollback. `running` is never
serialized. Unload destroys private counters; resident core objects survive
replacement and unload.

状态 ABI 1 导出 40 字节快照：magic `0x3153434e`、字节数、domain、零保留字段，
随后是 `calls`、`changes`、`imports` 三个 `uint64_t` 计数。快照不含指针或 XNU
对象。传输区须非空、8 字节对齐且容量足够，导入前核对 ABI、大小、domain 和
保留字段。导入只写已停止的新模块私有状态，并将 `imports` 加一；全部计数按
无符号 2^64 回绕。导入失败不修改状态；新模块启动失败时，旧模块停止时的计数
仍可回滚使用。`running` 不序列化。卸载销毁私有计数，常驻核心对象不随替换或
卸载销毁。

## License and validation / 许可与验证

`native_core_algorithms.h` retains notices from the three pinned Apple files,
including the APSL 2.0 header and their Carnegie Mellon, NeXT and Berkeley
notices. [LICENSE](LICENSE) contains those original notices, the pinned
`APPLE_LICENSE` text, and the repository's BSD-2-Clause-Patent text applicable
to the independent lifecycle/policy wrapper in `policy.c`. It does not
relicense the XNU adaptations under BSD.

`native_core_algorithms.h` 保留三份固定 Apple 源文件的通知，包括 APSL 2.0
头部及 Carnegie Mellon、NeXT、Berkeley 通知。[LICENSE](LICENSE) 收录这些
原始通知、固定版本 `APPLE_LICENSE` 原文，以及适用于独立生命周期/策略封装
`policy.c` 的仓库 BSD-2-Clause-Patent 文本；不将 XNU 适配代码改许可为 BSD。

Source review establishes the correspondence above. Compilation, packaged
image inspection, rollback/quarantine behavior and execution through the real
call sites require the unified validation recorded with the surrounding
change. A source review alone does not establish device execution.

以上对应关系来自源码核对。编译、最终镜像检查、回滚/隔离及真实调用点执行，
需随本轮完整改动统一验证；源码核对不代表已在设备执行。
