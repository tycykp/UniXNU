# 模块热加载与热替换

NativeHotload 为固定版本 `xnu-8796.141.3` / Darwin 22 提供显式启用的原生 ARM64 模块运行时，面向实现 NLH2 C 契约的驱动、服务和核心模块。目标范围包括 XNU 核心子系统。

补丁已将线程时间片、VM 顺序访问判定、VFS 聚簇 I/O 大小及预读计算接入可替换原生模块。[giulia Actions 40027302307](https://github.com/tycykp/UniXNU-OnePlus-Ace5/actions/runs/40027302307) 已完成完整 GUI 目标内核编译、真实链接入口／调用核验及 15 个策略模块构建。完整调度器、VM、VFS、IOKit 类和设备驱动的其余部分仍需迁移。

2026-10-24 首次 giulia 实机测试已完成 NLH2 上传，但 commit 在 PROBE 前返回 `EACCES`（13）。原生策略没有执行；调度器继续使用常驻原算法，记录到 617277 次回退、0 次策略调用。私有静态子映射修复与 `hl.memory` 诊断已通过 [Actions 40035230392](https://github.com/tycykp/UniXNU-OnePlus-Ace5/actions/runs/40035230392) 的完整目标内核及 15 个模块编译，并核对全部 20 个 kit 源文件和真实链接的 VM 启动调用。

修复镜像已于同日通过临时启动实机验收：三个策略均由真实内核入口执行，完成升级及私有状态迁移、START／IMPORT 失败回滚、正常卸载和 STOP 失败隔离。卸载、隔离后的后续快照均确认原生调用数冻结、常驻回退数增长，拒绝计数为 0。本地设备仓库报告为 `UniXNU-giulia/reports/hotload-device-memory-20261024.json`，记录 15 个模块场景、21 次生命周期变更、184 条 CLI 命令及 23 份内存快照，`error`、`kr`、`rollback_error` 均为 0。

调度与 VFS 已观察到改变结果计数增长；VM 改变结果分支本轮未覆盖。VFS 的 MAX_IO、PREFETCH 改变未分别归因，不宣称完整分支覆盖。实测范围为三个策略适配器及模块生命周期，不代表完整核心子系统或内建硬件驱动均可替换。故障注入后再次临时启动同一镜像，已确认注册表恢复为空（`token=0 busy=0 slots=0`），arena 就绪、阶段为 1、错误为 0。

## 运行时契约

最多同时管理 16 个命名模块，每个模块最多 8 项依赖、256 KiB 代码和 64 KiB 私有状态。卸载后的名称槽保留到重启，避免槽复用让常驻调用者意外转向其他模块。管理事务串行执行，不相关模块继续运行。NLH1 镜像和客户端不兼容。

镜像声明模块名、递增构建版本、CALL 接口 ABI、序列化状态 ABI、类别及依赖。依赖要求接口 ABI 完全匹配、构建版本满足下限。缺失依赖、重复／自依赖和循环依赖会在停止旧代码前拒绝。同名热替换保持接口 ABI、状态 ABI 和类别一致；有运行中或隔离模块依赖某个提供者时，禁止卸载该提供者。

常驻调用者通过 `nlh_dispatch(name, interface_abi, argument, result)` 进入模块；模块通过 `host->call` 调用已声明依赖，不保留其他模块的函数指针。入口使用原子计数固定活动镜像，不睡眠、不分配内存。CALL 可以并发，模块负责状态同步，并遵守调用者的执行上下文。

替换流程：

1. 校验完整镜像和依赖图，分配并封装候选映射，执行无副作用的 PROBE。
2. 关闭目标及其所有直接、间接依赖方的调用入口，最多等待五秒让在途调用退出；超时保留旧代码。
3. STOP 旧实例，EXPORT 序列化状态，IMPORT 到候选私有状态。
4. START 候选，成功后发布新实例并释放旧镜像。
5. 重新开放调用。候选 START 失败时，必须先成功 STOP 候选，才能重新 START 旧实例。

EXPORT 不得修改旧实例或转移资源所有权；IMPORT 只修改候选私有状态。临时传输缓冲最多 64 KiB，不得包含模块指针，也不能在回调结束后保留。START 需接受全零、导入后和停止后的状态；STOP 需清理正常运行及部分失败的 START。旧状态保留供回滚。

旧 STOP、候选清理或旧实例恢复失败时，目标进入隔离状态，其依赖方保持入口关闭。可能仍被引用的代码和状态保留到重启，不提供强制卸载。回调必须有界返回；排空时限不能抢占出错的原生代码。

切换期间入口返回 EBUSY，常驻适配器必须提供经过验证的重试／回退路径，不能持有 STOP 所需资源等待恢复。VM、调度和中断上下文尤其需要分别设计；通用入口本身不等于这些路径已可替换。

## 已接入的 XNU 核心算法

以下入口由 `core/emit_core_patch.py` 随 `0092-native-hotload.patch` 写入固定版本 XNU，使用 `native_hotload_core.h` 的 64 字节、无对象指针请求。常驻代码先按原算法计算结果，再通过 `nlh_core_select` 选择模块结果。模块缺失、入口关闭、竞争、接口错误或结果越界时立即使用原结果；调度、缺页和文件系统路径不等待更新工作线程。

| 模块 | 真实调用入口 | 模块决策与常驻状态 |
| --- | --- | --- |
| `xnu-sched` | `thread_quantum_init`，由线程派发及时间片更新路径调用 | 可缩短 timeshare 时间片；实时／固定模式、CPU ledger 上限和线程状态由原调度器维护 |
| `xnu-vm` | `vm_fault_is_sequential`，由缺页处理调用 | 可缩短同方向顺序访问长度；同页返回、对象锁、CAS 冲突处理与 `last_alloc` 提交留在原路径 |
| `xnu-vfs` | `cluster_max_io_size`、`cluster_max_prefetch`，由聚簇读写／预读调用 | 可降低 I/O 大小与预读预算；保留 UPL 旧下限、页对齐、设备能力、mount/vnode 及完成回调 |

这些核心模块仅接受保留名称、`kind=core`、接口 `0x434f0001`、状态 ABI 1、无依赖的镜像；主机和内核同时检查。核心回调不获得日志、时钟、平台或依赖回调，不能分配、睡眠、持有外部资源或保留请求。普通 USB `hl.call` 只能读取模块计数，不能传入核心请求指针。

替换关闭该模块入口并排空已固定镜像的调用；旧代码 STOP 后导出 `calls/changes/imports`，候选校验并导入，START 成功才发布。线程、VM 对象和文件系统对象始终由常驻内核持有，不序列化，也不转移所有权。IMPORT 或 START 失败时恢复旧实例；STOP 或清理失败时隔离并保持原算法回退。此边界不改运行队列或映射布局，因此通过在途调用排空保护代码生命周期，不需要停止其他 CPU；扩大到运行队列等可变对象时必须另行实现 CPU 协调协议。

上游函数、条件分支、提交顺序与许可证见 kit 的 `core/UPSTREAM.md`、`core/LICENSE`。原子入口为 lock-free，并非对所有 CPU 竞争情况都有严格步数上限。

## XNU 核心迁移边界

本轮源码审查对应 XNU 提交 `1b191cb58250d0705d8a51287127505aa4bc0789`，标签 `xnu-8796.141.3`。以下入口**尚未接入 NLH2**：

| 子系统与源码入口 | 必须完成的改造 |
| --- | --- |
| 调度：`osfmk/kern/sched_prim.c::thread_select`、`sched_prim.h::sched_dispatch_table` | 常驻分派和回退、各 CPU 安全汇合；不睡眠地迁移运行队列、优先级、定时器和当前线程状态 |
| VM/pmap：`osfmk/vm/vm_fault.c::vm_fault`、`vm_map.c`、`arm/pmap/pmap.c` | 保留映射／对象／页面所有权、缺页处理、TLB 更新及锁顺序；更新器的分配、映射与回收不能依赖正在停止的代码 |
| BSD 系统调用：`bsd/dev/arm/systemcalls.c`、`bsd/kern/syscalls.master` | 常驻 sysent 适配，保留 syscall ABI、阻塞 continuation 及进程状态 |
| VFS：`bsd/vfs/vfs_init.c::vfs_opv_init` 及文件系统操作表 | 稳定的 mount/vnode 适配，排空 I/O、日志及异步完成，迁移已挂载文件系统状态 |
| IOKit／中断：`iokit/Kernel`、`osfmk/arm64` | 常驻元类、虚表、异常／IRQ 入口和事件源；退出全部保留回调 |
| 网络与 IPC：`bsd/net*`、`osfmk/ipc` | 保留 socket、协议、port、消息所有权、定时器和在途任务 |

装载器及其分配／pmap 原语、工作线程调度、异常处理、USB 更新传输当前仍需常驻。替换这些基础设施需要独立常驻监督层及经过验证的切换协议。上述已接入算法不包含表中的其余入口。

硬件模块改造前，需将固定版本一加上游的探测、资源、供电、时钟、复位、互连、引脚、固件、DMA/IOMMU、中断、协议、运行、休眠、回滚与释放逐项映射到常驻适配器和模块回调。根存储和更新传输必须保持可用；先通过终端验证，再集成硬件行为到 UEFI。

## 内存与开发开关

仅 DEBUG/DEVELOPMENT、普通 ARM64、非 PPL、非 KTRR/CTRR、非 KASAN 构建可启用 `unixnu_hotload=1`。打包检查同时要求 NLH2 终端桥和受支持构建的内存能力标记。

代码使用私有静态虚拟地址子映射中的自有、固定驻留分配。`kmem_init` 阶段先检查原本未使用的静态地址 twig 及其上级页表，完整保留周围区域，并为代码 arena 留下封闭的中间占位。早期 VM map 完成重定位后，`ZALLOC/LAST` 将中间占位替换为永久子映射。子映射创建失败时，校验原占位并将其永久封闭；若保留区状态不一致则停止启动。此流程不放宽已有父级页表的执行权限，普通内核堆分配不能进入该区域。

装载器先核对 RW/NX 权限再复制代码，移除写权限后，通过私有同步凭据在原有 pmap/PVH 锁和 TLB 更新路径中授予执行权限。每个硬件叶页及其包含父级限制的有效权限均须通过核验，才能执行 PROBE 或其他原生回调。回收先撤销执行权限，再由正常 VM 路径解除 wired 状态、移除映射和释放分配。正常 pmap 建立／保护路径保持原行为。

首次实机镜像在普通内核堆中分配代码，源码审查确认该区域的上级 PXN 会阻止执行映射。旧镜像没有逐阶段内存报告，因此仅凭捕获的 `EACCES` 不能区分具体拒绝分支。新子映射和诊断均需重新构建内核；更新电脑客户端不会让旧镜像具备这些能力。

SHA-256 用于传输完整性，不提供签名授权。显式启用的开发设施执行可信原生代码；物理别名和主机函数表不构成沙箱。现有 AMFI／用户态信任策略保持不变。

## 构建与使用

在 giulia 仓库基于固定版本参考源码生成 profile：

```sh
python3 scripts/prepare-ios16.py --reference PINNED_REFERENCE \
  --work out/hotload-source --out out/hotload-profile --gui \
  --hotload-source ../UniXNU/kits/NativeHotload
```

随后完成 macOS/Xcode 内核构建、重定位及核验。本地 kit 会连同源码摘要和能力清单复制到 profile。打包启用 `--debug-terminal --debug-hotload`，整合脚本对应 `GIULIA_DEBUG_TERMINAL=1 GIULIA_DEBUG_HOTLOAD=1`。

giulia 的 `scripts/stage-hotload-ci.py` 可将公共 kit 生成纯源码快照并记录摘要，
实现归属仍在公共仓库。对已包含该快照的审查分支，`ios16-xnu.yml` 增加
`hotload=true` 后，在 Ubuntu 构建 15 个模块、macOS 编译内核，核对两者源码、
真实函数定义及核心调用并保留产物清单。热加载构建跳过 UEFI／启动镜像任务；
Actions 编译结果与终端实机验证分别记录。工作流不会自动取得未提交的本地源码。

嵌入本地固件后的完整内核可能需要大于 64 MiB 的 FAT。设备打包器可用
`--payload-mib 128` 显式选择容量，并核对真实 loader 的内核文件、虚拟跨度、
ADT 窗口及 16 KiB 对齐预算。最终镜像通过 `--max-image-bytes` 限制在实际确认
的目标分区容量内；新 payload 还需更新固件编译期大小／CRC 并重新链接。
这些检查不替代终端实测。

在 UniXNU 根目录使用 Linux／WSL Clang、LLD 构建核心模块及失败场景：

```sh
python3 kits/NativeHotload/build-core.py --out out/hotload-core
```

每个域产出 baseline v1、缩减策略 upgrade v2、fail-start、fail-import、fail-stop v3 和来源清单。路径为 `out/hotload-core/xnu-{sched,vm,vfs}/{baseline,upgrade,fail-start,fail-import,fail-stop}/xnu-*.nlh`。先装载 v1、v2，再依次执行 fail-start、fail-import，最后执行 fail-stop；前两项失败保持 v2 活动，其未发布的 v3 版本号可复用。fail-stop 镜像可以成功 START，后续停止才触发隔离，需重启后恢复。`core-suite.json` 记录各产物和验收步骤。镜像运行通过下面的终端工具装载，实际调用由正常系统工作负载触发。

也可构建独立的依赖／迁移示例：

```sh
python3 kits/NativeHotload/build-module.py --source kits/NativeHotload/examples/counter.c \
  --name counter --version 1 --out out/hotload/counter-v1
python3 kits/NativeHotload/build-module.py --source kits/NativeHotload/examples/counter.c \
  --name counter --version 2 --define COUNTER_STEP=10 --out out/hotload/counter-v2
python3 kits/NativeHotload/build-module.py --source kits/NativeHotload/examples/client.c \
  --name client --version 1 --requires counter:1:1 --out out/hotload/client
```

构建器保留 x18，禁用 SIMD/FP 和外置原子辅助函数；在静态链接隐藏重定位前检查目标文件，拒绝绝对重定位、未解析导入、可写全局、TLS 和构造函数。输出经过校验的镜像与来源 JSON。每个源文件声明 `const uint32_t nlh_state_bytes = sizeof(state)`；通过 `--interface-abi`、`--state-abi`、`--kind service|driver|core` 描述模块契约。

启动新内核后，退出投屏／PTY 模式、关闭其他串口终端，安装设备项目固定版本的 pyserial。在 Windows CMD 执行，COM7 换成实际端口，每条命令使用新日志路径：

```bat
python kits\NativeHotload\hotload.py --port COM7 --log out\hotload\01.log status
python kits\NativeHotload\hotload.py --port COM7 --log out\hotload\memory.log memory
python kits\NativeHotload\hotload.py --port COM7 --log out\hotload\core.log core
python kits\NativeHotload\hotload.py --port COM7 --log out\hotload\02.log load out\hotload\counter-v1\counter.nlh
python kits\NativeHotload\hotload.py --port COM7 --log out\hotload\03.log load out\hotload\client\client.nlh
python kits\NativeHotload\hotload.py --port COM7 --log out\hotload\04.log call client 0
python kits\NativeHotload\hotload.py --port COM7 --log out\hotload\05.log load out\hotload\counter-v2\counter.nlh
python kits\NativeHotload\hotload.py --port COM7 --log out\hotload\06.log call client 0
python kits\NativeHotload\hotload.py --port COM7 --log out\hotload\07.log list
python kits\NativeHotload\hotload.py --port COM7 --log out\hotload\08.log unload client
python kits\NativeHotload\hotload.py --port COM7 --log out\hotload\09.log unload counter
```

`memory` 发送带 nonce 关联的 `hl.memory NONCE` 查询，交互终端也可直接输入 `hl.memory`。该只读快照有界返回，更新事务忙碌时仍可读取。`snapshot=0` 表示未取得一致快照，应忽略本条诊断字段并重新查询。`arena=1` 仅表示启动时子映射已建立，不证明原生代码执行。

| `stage` | 最近报告的内存阶段 |
| --- | --- |
| 1 | 静态地址 arena 预检或子映射准备 |
| 2 | 分配固定驻留内存 |
| 3 | 复制前核验 RW/NX |
| 4 | 移除写权限并核验 RO/NX |
| 5 | 数据／指令缓存维护 |
| 6 | 通过私有 pmap 凭据授予执行权限 |
| 7 | 发布入口前核验有效 RX |
| 8 | 撤销执行权限 |
| 9 | 正常 VM 释放 |
| 10 | 内存操作完成 |

报告只包含数字字段：`error` 是本次操作的 errno，`kr` 是记录的 VM 返回码，`arena_kr` 是启动准备结果，`rollback_error` 单独记录清理核验失败。`bytes` 描述当前请求或分配大小，`arena_bytes` 保留启动 arena 大小；`checked_pages` 为已检查的硬件叶页数，`failed_page` 是从 0 起算的失败序号。`leaf`、`effective`、`parent_flags` 仅包含权限位，不包含虚拟／物理地址或带地址的原始描述符。创建失败的原阶段和权限证据在清理后保留；子映射不可用时保留启动失败证据。阶段 10 本身不证明模块 START 或核心调用成功。

两次预期结果为 **1、11**：计数状态保留，原生增量从 1 变成 10。核对 counter 的 generation／version 递增、client 版本不变，以及内核运行时间连续。先于 client 卸载 counter 应返回 EBUSY。这些是验收条件，**不是已取得的实机结果**。

核心验收先装载对应域 v1，运行调度／顺序文件读写负载，用 `core` 查看真实调用、回退和拒绝计数。`call xnu-sched 0`（或对应域）读取调用数，参数 1 读取改变结果数，参数 2 读取导入次数。升级 v2 后应看到版本和 generation 递增、调用计数保留、imports 增加。装入 fail-import／fail-start 后应保持旧版本和 generation；卸载后原算法继续工作，回退计数增加。隔离故障最后执行，核对槽保持阻断而系统仍走常驻算法。2026-10-24 实机已完成三个域的上述生命周期验收，决策分支覆盖范围见前文。

主机工具用随机 nonce 关联就绪查询，跳过旧控制台响应。传输每块 16 字节并逐块确认，满足 63 字节命令限制。断连丢弃未提交上传，已提交操作继续执行；结果不明确时查询 status／list，不重复提交。状态值：0 空、1 运行、2 已停止、3 隔离；被阻断的依赖方可以保持状态 1，但调用入口关闭。

本轮实测还遇到控制台文本与上传 ACK 无空白拼接，首次上传因此在 commit 前中止。主机解析器现已接受这类前缀后的完整协议记录，继续检查记录标记、数字字段、行尾及 token／nonce／received。修正仅用于主机客户端，已编译内核产物及其 staged 源码快照保持原样。

模块编译、完整目标内核链接和实机验证分别记录。已生成的调用入口补丁和模块产物不代表目标内核已成功运行，也不能证明完整核心子系统可替换。
