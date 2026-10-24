# Module hot loading and replacement

NativeHotload is an opt-in development runtime for native ARM64 modules on
`xnu-8796.141.3` / Darwin 22. It supports drivers, services and core modules
that implement the NLH2 C contract. The goal includes XNU core subsystems.

The patch connects thread quantum decisions, VM sequential-access tracking,
and VFS cluster I/O sizing and prefetch calculations to replaceable native
modules. The rest of the scheduler, VM, VFS, IOKit classes and hardware drivers
still requires conversion. The full GUI target kernel, linked gateway/core-call
checks and 15 policy module builds passed in
[giulia Actions run 40027302307](https://github.com/tycykp/UniXNU-OnePlus-Ace5/actions/runs/40027302307).
The first giulia device run on 2026-10-24 completed the NLH2 upload, but commit
returned `EACCES` (13) before PROBE. No native policy executed; the scheduler
continued through its resident fallback, reaching 617277 fallbacks with zero
policy calls. The private static-submap correction and `hl.memory` diagnostics
passed the full target-kernel and 15-module build in
[Actions run 40035230392](https://github.com/tycykp/UniXNU-OnePlus-Ace5/actions/runs/40035230392),
including agreement of all 20 kit source files and linked VM bootstrap calls.
The packaged image passed temporary-boot device acceptance on 2026-10-24:
all three policies executed through their real kernel entries, upgraded with
private-state migration, recovered from failed START/IMPORT, unloaded normally
and entered the intended failed-STOP quarantine. Follow-up snapshots confirmed
frozen native-call counters and growing resident fallbacks after both unload
and quarantine, with no rejected decisions. The local device-repository report
is `UniXNU-giulia/reports/hotload-device-memory-20261024.json`: 15 module
scenarios, 21 lifecycle changes, 184 CLI commands and 23 memory snapshots with
`error`, `kr` and `rollback_error` all zero.

Scheduler and VFS changed-result counters increased; the VM changed-result
branch was not exercised. VFS MAX_IO and PREFETCH changes were not separately
attributed, so full branch coverage is not claimed. These results cover the
three policy adapters and module lifecycle, not whole core subsystems or
built-in hardware drivers. After fault injection, another temporary boot of
the same image restored an empty registry (`token=0 busy=0 slots=0`) and a
ready, error-free arena at stage 1.

## Runtime contract

Up to 16 named modules can coexist. Names retain stable slots after unload
until reboot, preventing slot reuse from redirecting a resident caller.
Each module has at most eight declared dependencies, 256 KiB of code and
64 KiB of private state. One management transaction executes at a time;
unrelated modules continue running. NLH1 is incompatible.

An image declares its name, monotonically increasing build version, CALL
interface ABI, serialized-state ABI, kind and dependencies. Dependencies
require an exact interface ABI and minimum build version. Missing providers,
duplicate/self dependencies and cycles are rejected before stopping old code.
In-place replacements preserve the interface/state ABI and kind. Unload is
refused while any live or quarantined module depends on the provider.

Scalar service callers enter `nlh_dispatch(name, interface_abi, argument, result)`.
Modules call declared dependencies through `host->call`; they never retain
another module's function pointer. The gateway atomically pins the active
image without sleeping or allocating. CALL may run concurrently; each module
must synchronize its state and respect the caller's execution context.

Management performs the following sequence:

1. Validate the complete image and dependency graph; allocate and seal the
   candidate mapping, then run side-effect-free PROBE.
2. Close admission for the target and its transitive consumers. Wait up to
   five seconds for existing calls to return; timeout preserves old code.
3. STOP the old instance, EXPORT its serialized state, and IMPORT into the
   candidate's private state.
4. START the candidate; publish it and release the old image only on success.
5. Reopen admission. A failed candidate START first requires successful
   candidate STOP; only then can old START restore the previous instance.

EXPORT must not mutate the old instance or transfer resource ownership.
IMPORT may modify candidate private state only. The borrowed transfer buffer
holds at most 64 KiB and cannot contain module pointers. START accepts zero,
imported or stopped state. STOP must clean up partial START as well as normal
operation. Old state remains available for rollback.

A failed old STOP, candidate cleanup or old restart quarantines the affected
module and keeps its consumers blocked. Potentially referenced code/state is
retained until reboot; there is no force unload. Callbacks must return in a
bounded time: the drain deadline cannot preempt faulty native code.

Admission returns EBUSY during a transition. A resident adapter must implement
a validated retry/fallback path without holding resources needed by STOP.
This requirement is especially significant for VM, scheduler and interrupt
contexts; the generic gateway alone does not make those paths replaceable.

## Authenticated resident platform calls

The image header remains NLH2 (432 bytes), and `nlh_host` remains the same
48-byte ABI. The resident provider API has its own `NLH_PLATFORM_ABI=1`:
`nlh_register_platform_v2(abi, callback)` registers one resident callback before
the first upload. The old name-only `nlh_register_platform` symbol returns
ENOTSUP; providers must explicitly adopt the versioned interface. Modules
cannot install a provider through the host function table.

Each native entry receives a fresh stack-local host. Its context is an opaque,
never-reused invocation token, not an address. Before a platform or dependency
callback, the runtime matches that token to the current thread's innermost
active entry. Fabricated, expired, cross-thread and parent-entry tokens fail
with EACCES. Callback arguments cannot declare a different identity or phase.
The provider receives a borrowed const `nlh_invocation` snapshot containing the
validated name, version, interface/state ABI, kind, dependency count and digest;
image cookie, slot generation and activation epoch; invocation/root IDs;
phase, role, origin/depth and the original entry argument. Nothing in this
snapshot may be retained after the callback returns.

Only `nlh_dispatch_resident(name, interface_abi, argument, result)` marks a
direct call as RESIDENT. A resident adapter owns the bounded request identified
by `argument` and checks the response before committing its original hardware
path. Generic calls and USB `hl.call` carry separate GENERIC/USB origins;
declared dependencies always carry DEPENDENCY origin and increasing depth.
Root origin is diagnostic and confers no authority. Resident dispatch and
provider registration reject nested module/provider paths, preventing a
dependency from manufacturing a new resident root. USB scalar values are never
interpreted as kernel addresses.

PROBE, EXPORT and IMPORT reject both platform and dependency callbacks before
entering a provider. START/STOP/CALL retain their real phase and image role;
the provider must still enforce its own typed operations and resource rules.
For a pure protocol bridge, require RESIDENT origin, depth zero, ACTIVE role,
CALL phase, exact module metadata and the matching request ticket. Candidate
PROBE runs before old STOP and must never alter another invocation's staging.

Each candidate attempt reserves a fresh slot generation and image cookie;
only successful START publishes that generation. Every START attempt advances
the activation epoch, including rollback START of the old image. Candidate
cleanup STOP uses its failed START's epoch; old STOP/EXPORT retain the previous
epoch. A rollback therefore cannot reuse a request from the previous activation
even though its image version is unchanged. STOP/cleanup/restart failure keeps
the original quarantine and transitive admission closure, retaining uncertain
code/state until reboot. These are module generations, not evidence of hardware
reset, DMA drain or a valid hardware-owner lease.

`hl.status` advertises `platform_abi=1 resident_dispatch=1`; `hl.list` reports
active image cookies/epochs and any retained candidate identity. This requires
a rebuilt kernel with the resident bridge linked before upload. The previous
15-policy device validation predates this interface; it does not establish
hardware-driver replacement or validation of the new runtime.

## Connected XNU core algorithms

`core/emit_core_patch.py` adds these real entries to the pinned XNU tree through
`0092-native-hotload.patch`. They pass 64-byte, pointer-free requests defined in
`native_hotload_core.h`. Resident code computes the original result before
calling `nlh_core_select`. Missing modules, closed admission, contention,
interface errors and invalid decisions immediately use that original result;
the scheduler, fault and filesystem callers never wait for the update worker.

| Module | Actual call entry | Replaceable decision and resident state |
| --- | --- | --- |
| `xnu-sched` | `thread_quantum_init`, from dispatch and quantum refresh paths | Shorter timeshare quantum; real-time/fixed mode, CPU ledger limits and thread ownership remain resident |
| `xnu-vm` | `vm_fault_is_sequential`, from page faults | Shorter same-direction sequential run; same-page return, object locking, CAS conflict handling and `last_alloc` commit remain resident |
| `xnu-vfs` | `cluster_max_io_size` and `cluster_max_prefetch`, from clustered reads/writes and prefetch | Lower I/O sizes and prefetch budgets; retain the legacy UPL minimum, page alignment, device capabilities, mount/vnode ownership and completion callbacks |

Host and kernel both require the reserved names, `kind=core`, interface
`0x434f0001`, state ABI 1 and no dependencies. Core modules receive no logging,
clock, platform or dependency callbacks. Their decisions cannot allocate,
sleep, acquire external resources or retain requests. USB `hl.call` reads
telemetry only and cannot submit a core request pointer.

Replacement closes admission and drains pinned calls. After STOP, the old
module exports `calls/changes/imports`; the candidate validates and imports
them, then START must succeed before publication. Thread, VM and filesystem
objects remain resident and never enter serialization or ownership transfer.
IMPORT/START failure restores the old instance; STOP/cleanup failure quarantines
it while the resident algorithms continue. This boundary does not alter run
queues or mapping layouts: draining calls protects code lifetime without
stopping other CPUs. Converting those mutable structures requires a separate
CPU coordination protocol.

See the kit's `core/UPSTREAM.md` and `core/LICENSE` for original functions,
branches, commit order and licensing. Admission is lock-free, not strictly
wait-free under every form of CPU contention.

## XNU core migration boundary

The source audit uses XNU commit
`1b191cb58250d0705d8a51287127505aa4bc0789`
(tag `xnu-8796.141.3`). These paths are **not connected to NLH2 yet**:

| Subsystem and source entry | Required conversion |
| --- | --- |
| Scheduler: `osfmk/kern/sched_prim.c::thread_select`, `sched_prim.h::sched_dispatch_table` | Resident dispatch/fallback; per-CPU rendezvous; run-queue, priority, timer and current-thread state migration without sleeping |
| VM/pmap: `osfmk/vm/vm_fault.c::vm_fault`, `vm_map.c`, `arm/pmap/pmap.c` | Preserve map/object/page ownership, faults, shootdowns and lock order; the updater's allocation, mappings and reclamation cannot depend on code being stopped |
| BSD syscalls: `bsd/dev/arm/systemcalls.c`, `bsd/kern/syscalls.master` | Resident sysent adapters; preserve syscall ABI, blocked continuations and per-process state |
| VFS: `bsd/vfs/vfs_init.c::vfs_opv_init` and filesystem operation vectors | Stable mount/vnode adapters; drain I/O, journals and async completion; migrate live filesystem state |
| IOKit/interrupts: `iokit/Kernel`, `osfmk/arm64` | Resident metaclasses, vtables, exception/IRQ entries and event sources; retire all retained callbacks |
| Networking and IPC: `bsd/net*`, `osfmk/ipc` | Preserve sockets, protocols, ports, message ownership, timers and outstanding work |

The loader, its allocator/pmap primitives, worker scheduling, exception
handling and USB update transport currently remain resident. Replacing those
foundations requires a separate resident supervisor and a proven transition
protocol; making their pointers writable does not solve the lifetime cycle.
The connected algorithms above do not include the remaining entries in this table.

For hardware modules, first map every upstream probe/resource/power/clock/
reset/interconnect/pin/firmware/DMA/IOMMU/IRQ/protocol/runtime/suspend/rollback/
release step to the local resident adapter and module callbacks. Keep root
storage and the update transport available. Validate through the terminal
before integrating hardware behavior into UEFI.

## Memory and development mode

Only DEBUG/DEVELOPMENT, plain ARM64, non-PPL, non-KTRR/CTRR and non-KASAN
builds can enable `unixnu_hotload=1`. The payload checker requires both the
NLH2 bridge and the supported-build memory marker.

Code uses owned wired allocations in a private static-VA submap. During
`kmem_init`, the runtime checks an otherwise empty static-VA twig and its
ancestors, then reserves the complete surrounding region with a closed middle
placeholder for the code arena. After early VM maps have been relocated,
`ZALLOC/LAST` replaces that placeholder with a permanent submap. A failed
submap creation keeps the checked placeholder closed and permanently reserved;
an inconsistent reservation stops startup. No existing parent-table execute
permissions are relaxed, and ordinary kernel-heap allocations cannot use the
arena.

The loader checks RW/NX permissions before copying, removes write access, then
uses a private synchronous cookie to grant execute through the existing
pmap/PVH locks and TLB update path. Every hardware leaf and its effective
permissions, including ancestor overrides, must pass before PROBE or any other
native callback. Reclamation revokes execute access before the normal VM
unwire/removal path releases the allocation. The normal pmap entry/protection
paths retain their behavior.

The first device image allocated code in the ordinary heap. Source review
identified ancestor PXN as the obstacle to executable mappings there. That
image had no per-stage memory report, so the captured `EACCES` alone does not
identify its precise rejection branch. The new submap and diagnostics require
a newly built kernel; updating the host client does not add them to that image.

SHA-256 detects transfer corruption, not authorization. This explicitly enabled
development facility runs trusted native code; physical aliases and the host
table do not form a sandbox. Existing AMFI/userland trust policy is unchanged.

## Build and use

Generate a giulia profile from the pinned reference checkout:

```sh
python3 scripts/prepare-ios16.py --reference PINNED_REFERENCE \
  --work out/hotload-source --out out/hotload-profile --gui \
  --hotload-source ../UniXNU/kits/NativeHotload
```

Then complete the existing macOS/Xcode kernel build, rebasing and verification.
The local kit is copied into the profile with source hashes and capability
metadata. Package using `--debug-terminal --debug-hotload`
(or `GIULIA_DEBUG_TERMINAL=1 GIULIA_DEBUG_HOTLOAD=1` in the integrated script).
The device repository's `scripts/stage-hotload-ci.py` creates an auditable
source-only snapshot of the common kit; the canonical code stays in the common
repository. On a reviewed branch containing that snapshot, dispatch
`ios16-xnu.yml` with `hotload=true` to compile 15 modules on Ubuntu and the kernel
on macOS. The workflow binds their source hashes, checks real linked function
definitions and core calls, and retains provenance. Hotload builds skip
UEFI/boot-image tasks. Actions compilation and terminal device validation are
recorded separately. The workflow cannot obtain uncommitted local source.

The full kernel with local firmware may need more than the default 64 MiB FAT.
The device packer accepts an explicit `--payload-mib 128` and checks the real
loader's kernel-file, virtual-span, ADT-window and 16 KiB layout limits. Use
`--max-image-bytes` to constrain the complete image to independently confirmed
target-partition capacity. Relink the firmware with the new payload size/CRC;
these checks do not establish device execution.

In the UniXNU root, build the core modules and failure scenarios using
Linux/WSL Clang and LLD:

```sh
python3 kits/NativeHotload/build-core.py --out out/hotload-core
```

Each domain produces baseline v1, reduced-policy upgrade v2, and fail-start,
fail-import and fail-stop v3 images with provenance. After v1 and v2, run
fail-start and fail-import before fail-stop: the first two leave v2 active and
their unpublished v3 can be reused. A fail-stop image starts successfully and
quarantines on a subsequent stop; reboot is required to recover. Images use
`out/hotload-core/xnu-{sched,vm,vfs}/{baseline,upgrade,fail-start,fail-import,fail-stop}/xnu-*.nlh`.
`core-suite.json` records the
artifacts and acceptance sequence. Upload through the client below; normal
system workloads trigger actual kernel calls.

The standalone dependency/state-transfer examples are also available:

```sh
python3 kits/NativeHotload/build-module.py --source kits/NativeHotload/examples/counter.c \
  --name counter --version 1 --out out/hotload/counter-v1
python3 kits/NativeHotload/build-module.py --source kits/NativeHotload/examples/counter.c \
  --name counter --version 2 --define COUNTER_STEP=10 --out out/hotload/counter-v2
python3 kits/NativeHotload/build-module.py --source kits/NativeHotload/examples/client.c \
  --name client --version 1 --requires counter:1:1 --out out/hotload/client
```

The builder preserves x18, disables SIMD/FP and outline atomics, and rejects
absolute object relocations, undefined imports, writable globals, TLS and
constructors. It emits a checked image and provenance JSON. Declare
`const uint32_t nlh_state_bytes = sizeof(state)` in each source.

The builder first runs Clang `-MM` with the same target, defines and include
flags, then hashes the actual non-system dependency closure. Compilation's
`-MMD` dependency set and every input hash must match that preflight; the
builder rechecks inputs, kit tooling and compiler/linker identities before
publication. `module.json` binds those inputs and compile/link parameters to
the image and object/ELF hashes. A sibling staging directory is atomically
renamed to the new output only after image and manifest readback succeeds;
failed builds publish no module directory. An exclusive sibling `.build-lock`
prevents concurrent builders from targeting the same output. A crash leaves
that lock available for inspection before explicit removal.
Use `--interface-abi`, `--state-abi` and `--kind service|driver|core`
when defining a module's contract.

After booting the new kernel, close other serial terminals, exit screen/PTY
modes and install the device project's pinned pyserial requirement. In Windows
CMD, replace COM7 with the actual port and use a new log path for each command:

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

`memory` sends a nonce-correlated `hl.memory NONCE` query. Interactive terminals
also accept `hl.memory`. It is a bounded, read-only snapshot available while a
replacement is busy. `snapshot=0` means no consistent snapshot was obtained;
discard the accompanying diagnostic fields and query again. `arena=1` means
the boot-time submap was created, not that native code has executed.

| `stage` | Last reported memory phase |
| --- | --- |
| 1 | Static-VA arena preflight or submap setup |
| 2 | Wired allocation |
| 3 | RW/NX verification before copying |
| 4 | Remove write permission and verify RO/NX |
| 5 | Data/instruction cache maintenance |
| 6 | Grant execute access through the private pmap cookie |
| 7 | Effective RX verification before exposing the entry |
| 8 | Revoke execute access |
| 9 | Normal VM release |
| 10 | Memory operation completed |

The report contains numeric fields only: `error` is the operation's errno,
`kr` is its recorded VM return code, `arena_kr` is the boot setup result, and
`rollback_error` records cleanup verification failure separately. `bytes`
describes the current request or allocation, while `arena_bytes` preserves the
boot arena size. `checked_pages` counts inspected hardware leaves and
`failed_page` is their zero-based failure index. `leaf`, `effective` and
`parent_flags` contain permission bits only, with no virtual/physical addresses
or raw address-bearing descriptors. Failed creation retains its original
phase and permissions across cleanup; a missing submap retains the boot
failure evidence. Stage 10 alone does not establish module START or core calls.

Expected call results are **1 then 11**: the state survives while the increment
changes from 1 to 10. Check counter's generation/version, client's unchanged
version and continuous kernel uptime. Unloading counter before client should
return EBUSY. These are acceptance criteria, **not recorded device results**.

For each core domain, load v1, run scheduling or sequential file I/O workloads,
and use `core` to read actual calls, fallbacks and rejected decisions.
`call xnu-sched 0` (or the corresponding domain name) reads policy calls;
arguments 1 and 2 read changed decisions and imports. Upgrading to v2 must
increment version/generation, preserve call counters and increase imports.
Fail-import/fail-start must leave the old version/generation active. After
unload, the resident algorithm continues and fallback counters increase.
Run quarantine scenarios last and check that blocked slots still use resident
algorithms. The 2026-10-24 device run met these lifecycle criteria for all three
domains, with the decision-branch limits recorded above.

The client correlates its readiness query with a random nonce to skip old
console replies. Upload uses acknowledged 16-byte chunks within the 63-byte
command limit. Disconnect discards unfinished uploads; committed operations
continue. After an unknown result, query status/list instead of retrying a
commit. State codes: 0 empty, 1 running, 2 stopped, 3 quarantined; a blocked
consumer may remain in state 1 with admission closed.

During this device run, console text joined an upload ACK without whitespace
before `NLH`. The first upload was aborted before commit. The host parser now
accepts complete protocol records after such prefixes, while retaining the
record marker, numeric fields, line ending and token/nonce/received checks.
The correction was applied to the host client; the compiled kernel artifact
and its staged source snapshot were kept unchanged.

Module compilation, full target-kernel linking and physical-device validation
are separate milestones. Generated call-entry patches and module artifacts do
not establish a running target kernel or whole-subsystem replacement.
