/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * v1 -> v2 preserves the counter, then changes its native increment code. */
#include "native_hotload.h"
#ifndef COUNTER_STEP
#define COUNTER_STEP 1
#endif
struct counter { uint64_t value; uint32_t running; };
const uint32_t nlh_state_bytes = sizeof(struct counter);
int nlh_entry(const struct nlh_host *host, void *opaque, uint32_t op,
              uint64_t argument, uint64_t *result)
{
    struct counter *state = opaque;
    if (host->abi != NLH_ABI || host->bytes != sizeof(*host)) return 22;
    switch (op) {
    case NLH_PROBE: return 0;
    case NLH_START:
        state->running = 1;
        host->log("counter started"); return 0;
    case NLH_STOP:
        state->running = 0;
        host->log("counter stopped"); return 0;
    case NLH_EXPORT: {
        struct nlh_transfer *transfer = (void *)(uintptr_t)argument;
        if (transfer->abi != 1 || transfer->capacity < sizeof(uint64_t)) return 22;
        *(uint64_t *)transfer->data = state->value;
        transfer->bytes = sizeof(uint64_t);
        return 0;
    }
    case NLH_IMPORT: {
        const struct nlh_transfer *transfer = (const void *)(uintptr_t)argument;
        if (transfer->abi != 1 || transfer->bytes != sizeof(uint64_t)) return 22;
        state->value = *(const uint64_t *)transfer->data;
        return 0;
    }
    case NLH_CALL:
        if (!state->running) return 16;
        *result = __atomic_add_fetch(&state->value, argument + COUNTER_STEP, __ATOMIC_RELAXED);
        return 0;
    default: return 22;
    }
}
