/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * A second service invokes counter through its declared dependency gateway.
 * No direct function pointer into counter survives a call or an update. */
#include "native_hotload.h"
struct client { uint32_t running; };
const uint32_t nlh_state_bytes = sizeof(struct client);
int nlh_entry(const struct nlh_host *host, void *opaque, uint32_t op,
              uint64_t argument, uint64_t *result)
{
    struct client *state = opaque;
    if (host->abi != NLH_ABI || host->bytes != sizeof(*host)) return 22;
    switch (op) {
    case NLH_PROBE: return 0;
    case NLH_START: state->running = 1; return 0;
    case NLH_STOP: state->running = 0; return 0;
    case NLH_EXPORT: {
        struct nlh_transfer *transfer = (void *)(uintptr_t)argument;
        if (transfer->abi != 1) return 22;
        transfer->bytes = 0; return 0;
    }
    case NLH_IMPORT: {
        const struct nlh_transfer *transfer = (const void *)(uintptr_t)argument;
        return transfer->abi == 1 && transfer->bytes == 0 ? 0 : 22;
    }
    case NLH_CALL:
        return state->running ? host->call(host->context, 0, argument, result) : 16;
    default: return 22;
    }
}
