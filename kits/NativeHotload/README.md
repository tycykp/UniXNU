# NativeHotload

Opt-in native ARM64 module loading and replacement for pinned
`xnu-8796.141.3` / Darwin 22. NLH2 manages named modules, dependency calls,
version checks, explicit state transfer, rollback and quarantine over the
existing USB diagnostic transport.

Resident adapters connect actual scheduler quantum, VM sequential-access and
VFS cluster sizing/prefetch decisions to three replaceable core policies. Their
private counters migrate and failures restore the previous policy or retain
the resident baseline. Kernel objects remain resident. Whole core subsystems
and hardware drivers still require conversion. The full GUI target build and
linked-call checks passed in giulia Actions run 40027302307. In the first
device run on 2026-10-24, the full upload completed but commit returned
`EACCES` (13) before PROBE: no native module executed. The resident scheduler
continued, with 617277 fallbacks and zero policy calls. The private static-VA
submap correction and memory diagnostics below passed the full target-kernel
and 15-module build in
[Actions run 40035230392](https://github.com/tycykp/UniXNU-OnePlus-Ace5/actions/runs/40035230392),
including all 20 kit source hashes and linked VM bootstrap calls. The packaged
image then passed temporary-boot device acceptance for real calls in all three
domains, state-preserving upgrade, failed START/IMPORT rollback, normal unload
and failed-STOP quarantine. Follow-up snapshots confirmed frozen native calls
and increasing resident fallbacks after unload and quarantine. The local
report is `UniXNU-giulia/reports/hotload-device-memory-20261024.json`: 15 module
scenarios, 21 lifecycle changes, 184 CLI commands and 23 error-free memory
snapshots. A later temporary boot of the same image confirmed an empty registry
and ready stage-1 arena.

Scheduler and VFS changed-result counters increased. The VM changed-result
branch remains untested; VFS MAX_IO/PREFETCH changes were not separately
attributed. Whole-subsystem and built-in hardware-driver replacement are
outside this validation. This is a C module ABI, not a Mach-O/kext loader.

See the [English guide](../../docs/drivers/hotload.md) and
[中文说明](../../docs/zh/drivers/hotload.md) for the lifecycle contract,
commands, core migration boundaries and validation limits. Booting a kernel
that contains the loader is required once. Compatible converted modules can
then be replaced independently. NLH1 images/clients are incompatible.

| File | Responsibility |
| --- | --- |
| `native_hotload.h` | Image, dependency, state transfer and resident gateway ABI |
| `native_hotload.c` | Registry, atomic admission, dependency graph, lifecycle and quarantine |
| `native_hotload_memory.inc` | Static-arena preflight, owned wired allocations, RX transition, permission verification and numeric diagnostics |
| `native_hotload_vm.h`, `native_hotload_boot.inc`, `core/emit_vm_patch.py` | Closed static-VA reservation in kmem_init and private submap creation at ZALLOC/LAST |
| `emit_patch.py` | Opt-in pinned XNU patch generation |
| `nlh_format.py` | Shared host-side NLH2 image validation |
| `build-module.py`, `module.ld` | AArch64 compilation, relocation checks and packaging |
| `hotload.py` | Acknowledged USB uploads and nonce-correlated status/memory queries |
| `native_hotload_core.h`, `core/emit_core_patch.py` | Typed resident scheduler/VM/VFS call adapters |
| `core/policy.c`, `core/native_core_algorithms.h` | Executable core decisions with state migration |
| `build-core.py` | Three-domain baseline, upgrade and failure image packaging |
| `core/UPSTREAM.md`, `core/LICENSE` | Pinned upstream mapping and original license notices |
| `examples/counter.c`, `examples/client.c` | State-preserving upgrade and dependency-call examples |

The runtime follows the pinned XNU VM/pmap ownership and locking paths.
The arena uses an otherwise empty static-VA twig without relaxing existing
ancestor PXN. It is reserved before heap-range setup, then made a permanent
private submap after early maps have been relocated. RW/NX, RO/NX and effective
RX are verified before native entry; execute revocation precedes normal VM
release. The original device image used ordinary heap allocations and lacks
this correction and its per-stage diagnostics.

Query the new kernel from the kit directory, using the actual port and a new
capture path:

```bat
python hotload.py --port COM7 --log out\memory.log memory
```

The read-only `hl.memory NONCE` response reports numeric `stage`, `error`,
`kr`, `arena_kr`, request `bytes`, `arena_bytes`, inspected/failed hardware
leaf counts, permission-only `leaf/effective/parent_flags` and
`rollback_error`. It exposes no addresses. Stages are 1 arena, 2 allocation,
3 RW/NX check, 4 RO/NX protection/check, 5 cache maintenance, 6 execute seal,
7 RX check, 8 revoke, 9 VM release and 10 complete. `snapshot=0` invalidates the
snapshot; `arena=1` and stage 10 alone do not prove native execution. See the
bilingual guides for the complete field interpretation and acceptance sequence.

The host client also handles complete ACK/done/status/memory records joined to a
console prefix without whitespace. The first affected upload was aborted
before commit; token, nonce and received-offset checks remain in place. This
host correction leaves the compiled kernel artifact and its staged source
snapshot unchanged.

Hardware conversion must separately preserve and document the complete pinned
OnePlus upstream lifecycle. The examples do not access hardware.

License: BSD-2-Clause-Patent for original runtime/tooling and policy code.
Adapted XNU algorithms and generated modifications to Apple source retain
their original notices and license; see `core/LICENSE`.
