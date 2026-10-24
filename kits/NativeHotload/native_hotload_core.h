/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NATIVE_HOTLOAD_CORE_H
#define NATIVE_HOTLOAD_CORE_H
#include "native_hotload.h"

#define NLH_CORE_INTERFACE 0x434f0001u
#define NLH_CORE_STATE_ABI 1u
#define NLH_CORE_SCHED 1u
#define NLH_CORE_VM 2u
#define NLH_CORE_VFS 3u
#define NLH_SCHED_QUANTUM 1u
#define NLH_VM_SEQUENTIAL 1u
#define NLH_VFS_MAX_IO 1u
#define NLH_VFS_PREFETCH 2u
#define NLH_MODE_TIMESHARE 0u
#define NLH_MODE_REALTIME 1u
#define NLH_MODE_FIXED 2u
#define NLH_BEHAVIOR_DEFAULT 0u
#define NLH_BEHAVIOR_RANDOM 1u
#define NLH_BEHAVIOR_FORWARD 2u
#define NLH_BEHAVIOR_REVERSE 3u
#define NLH_IO_BOTH 0u
#define NLH_IO_READ 1u
#define NLH_IO_WRITE 2u
#define NLH_VM_UPDATE (1ULL << 32)

/* Pointer-free snapshots. Kernel thread/vm_object/mount/vnode ownership and
 * layout remain resident. The callback borrows this request for one call;
 * no pointers, locks, timers or I/O completion handlers leave the gateway.
 * No allocation, logging, platform calls, dependency calls or sleeping in
 * NLH_CORE_CALL. These calls occur under scheduler/VM/filesystem locks. */
struct nlh_core_request {
    uint32_t abi, bytes, domain, operation;
    union {
        struct {
            uint64_t initial, safety_limit, cpu_remaining;
            uint32_t mode, reserved;
        } sched;
        struct {
            uint64_t last_alloc, offset;
            int32_t sequential;
            uint32_t behavior, page_bytes, max_run;
        } vm;
        struct {
            uint32_t read_segments, write_segments, read_bytes, write_bytes;
            uint32_t page_bytes, max_upl_bytes, minimum_io, type;
            uint32_t max_io, scale, prefetch_limit, reserved;
        } vfs;
    } value;
};
_Static_assert(sizeof(struct nlh_core_request) == 64, "core request ABI");

#ifdef KERNEL
/* Returns the validated module decision or the resident baseline immediately.
 * No caller waits for an update, and no raw core request is exposed via USB. */
uint64_t nlh_core_select(const struct nlh_core_request *, uint64_t baseline);
void nlh_core_status(void);
#endif
#endif
