/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Executable core policies, entered by the real scheduler/VM/VFS adapters.
 * State is private; thread/vm_object/mount/vnode state remains resident.
 */
#include "native_core_algorithms.h"
#ifndef NLH_POLICY_DOMAIN
#error Specify NLH_POLICY_DOMAIN
#endif
#if NLH_POLICY_DOMAIN < NLH_CORE_SCHED || NLH_POLICY_DOMAIN > NLH_CORE_VFS
#error Policy domain must be scheduler, VM or VFS
#endif
#ifndef NLH_POLICY_DIVISOR
#define NLH_POLICY_DIVISOR 1
#endif
#if NLH_POLICY_DIVISOR < 1 || NLH_POLICY_DIVISOR > 16
#error Policy divisor must be 1..16
#endif
#ifndef NLH_FAIL_START
#define NLH_FAIL_START 0
#endif
#ifndef NLH_FAIL_IMPORT
#define NLH_FAIL_IMPORT 0
#endif
#ifndef NLH_FAIL_STOP
#define NLH_FAIL_STOP 0
#endif

struct policy_state {
    uint64_t calls, changes, imports;
    uint32_t running, reserved;
};
struct policy_snapshot {
    uint32_t magic, bytes, domain, reserved;
    uint64_t calls, changes, imports;
};
_Static_assert(sizeof(struct policy_state) == 32, "private policy state ABI");
_Static_assert(sizeof(struct policy_snapshot) == 40, "serialized policy state ABI");
_Static_assert(__atomic_always_lock_free(sizeof(uint64_t), 0), "lock-free counters required");
const uint32_t nlh_state_bytes = sizeof(struct policy_state);

int nlh_entry(const struct nlh_host *host, void *opaque, uint32_t operation,
              uint64_t argument, uint64_t *result)
{
    struct policy_state *state = opaque;
    if (!host || host->abi != NLH_ABI || host->bytes != sizeof(*host) || !state || !result)
        return 22;
    switch (operation) {
    case NLH_PROBE:
        return NLH_POLICY_DOMAIN >= NLH_CORE_SCHED && NLH_POLICY_DOMAIN <= NLH_CORE_VFS ? 0 : 22;
    case NLH_START:
        /* A deliberately failing build exercises partial-start rollback. */
        state->running = 1;
        return NLH_FAIL_START ? 5 : 0;
    case NLH_STOP:
        if (NLH_FAIL_STOP) return 5;
        state->running = 0;
        return 0;
    case NLH_EXPORT: {
        struct nlh_transfer *transfer = (void *)(uintptr_t)argument;
        if (state->running || !transfer || transfer->abi != NLH_CORE_STATE_ABI ||
            transfer->reserved || !transfer->data ||
            ((uintptr_t)transfer->data & 7u) ||
            transfer->capacity < sizeof(struct policy_snapshot)) return 22;
        struct policy_snapshot *snapshot = transfer->data;
        *snapshot = (struct policy_snapshot){0x3153434eu, sizeof(*snapshot), NLH_POLICY_DOMAIN,
            0, state->calls, state->changes, state->imports};
        transfer->bytes = sizeof(*snapshot);
        return 0;
    }
    case NLH_IMPORT: {
        const struct nlh_transfer *transfer = (const void *)(uintptr_t)argument;
        if (state->running || !transfer || transfer->abi != NLH_CORE_STATE_ABI ||
            transfer->reserved || !transfer->data ||
            ((uintptr_t)transfer->data & 7u) || transfer->bytes > transfer->capacity ||
            transfer->bytes != sizeof(struct policy_snapshot)) return 22;
        const struct policy_snapshot *snapshot = transfer->data;
        if (snapshot->magic != 0x3153434eu || snapshot->bytes != sizeof(*snapshot) ||
            snapshot->domain != NLH_POLICY_DOMAIN || snapshot->reserved) return 22;
        if (NLH_FAIL_IMPORT) return 5;
        state->calls = snapshot->calls;
        state->changes = snapshot->changes;
        state->imports = snapshot->imports + 1;
        return 0;
    }
    case NLH_CALL:
        /* The USB scalar call is telemetry only, never a request pointer. */
        if (!state->running) return 16;
        if (argument == 0) *result = __atomic_load_n(&state->calls, __ATOMIC_RELAXED);
        else if (argument == 1) *result = __atomic_load_n(&state->changes, __ATOMIC_RELAXED);
        else if (argument == 2) *result = state->imports;
        else return 22;
        return 0;
    case NLH_CORE_CALL: {
        const struct nlh_core_request *request = (const void *)(uintptr_t)argument;
        if (!state->running) return 16;
        if (!nlh_core_request_valid(request) || request->domain != NLH_POLICY_DOMAIN) return 22;
        uint64_t baseline = nlh_core_baseline(request), value = baseline;
        if (NLH_POLICY_DOMAIN == NLH_CORE_SCHED &&
            request->value.sched.mode == NLH_MODE_TIMESHARE) {
            value /= NLH_POLICY_DIVISOR;
            if (!value) value = 1;
        } else if (NLH_POLICY_DOMAIN == NLH_CORE_VM && (value & NLH_VM_UPDATE)) {
            int32_t sequence = (int32_t)(uint32_t)value;
            /* object->sequential counts whole resident pages. Divisors that
             * do not divide MAX_SEQUENTIAL_RUN must preserve that invariant. */
            int32_t limit = (int32_t)((request->value.vm.max_run / NLH_POLICY_DIVISOR) &
                                     ~(request->value.vm.page_bytes - 1));
            if (sequence > limit) sequence = limit;
            if (sequence < -limit) sequence = -limit;
            value = NLH_VM_UPDATE | (uint32_t)sequence;
        } else if (NLH_POLICY_DOMAIN == NLH_CORE_VFS) {
            value /= NLH_POLICY_DIVISOR;
            if (request->operation == NLH_VFS_MAX_IO) {
                value &= ~((uint64_t)request->value.vfs.page_bytes - 1);
                if (value < request->value.vfs.minimum_io) value = request->value.vfs.minimum_io;
            }
        }
        if (!nlh_core_result_valid(request, baseline, value)) return 22;
        __atomic_fetch_add(&state->calls, 1, __ATOMIC_RELAXED);
        if (value != baseline) __atomic_fetch_add(&state->changes, 1, __ATOMIC_RELAXED);
        *result = value;
        return 0;
    }
    default: return 22;
    }
}
