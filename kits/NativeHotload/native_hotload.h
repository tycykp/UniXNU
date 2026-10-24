/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NATIVE_HOTLOAD_H
#define NATIVE_HOTLOAD_H
#include <stdint.h>

#define NLH_ABI 2u
#define NLH_MAGIC 0x32484c4eu /* NLH2, little endian */
#define NLH_MAX_MODULES 16u
#define NLH_MAX_DEPENDENCIES 8u
#define NLH_MAX_CODE (256u * 1024u)
#define NLH_MAX_STATE (64u * 1024u)
#define NLH_PROBE 1u
#define NLH_START 2u
#define NLH_STOP 3u
#define NLH_CALL 4u
#define NLH_EXPORT 5u
#define NLH_IMPORT 6u
#define NLH_CORE_CALL 7u /* resident typed gateway only; never a USB pointer */
#define NLH_SERVICE 0u
#define NLH_DRIVER 1u
#define NLH_CORE 2u

struct nlh_dependency {
    char name[32];
    uint32_t interface_abi, minimum_version;
};

/* Digest covers every preceding header byte followed by all code.
 * No absolute code/data pointers, imports, TLS or writable globals in images.
 * interface_abi describes CALL; state_abi describes EXPORT/IMPORT data.
 * kind classifies a module; it does not convert a static kernel subsystem. */
struct nlh_header {
    uint32_t magic, abi, header_bytes, code_bytes;
    uint32_t state_bytes, entry_offset, version, interface_abi;
    uint32_t state_abi, dependency_count, kind, reserved;
    char name[32];
    struct nlh_dependency dependencies[NLH_MAX_DEPENDENCIES];
    unsigned char sha256[32];
};

/* EXPORT/IMPORT run after STOP in sleepable worker context.
 * EXPORT writes bytes <= capacity; IMPORT reads that exact serialized format.
 * Never serialize module pointers or transfer ownership during EXPORT/IMPORT.
 * The transfer buffer is borrowed for this callback only. */
struct nlh_transfer {
    uint32_t abi, bytes, capacity, reserved;
    void *data;
};

/* External function pointers always target resident gateways. CALL may execute
 * concurrently and inherits the caller's context; modules synchronize state.
 * Admission never sleeps. On EBUSY the resident caller must retry or use a
 * proven fallback, without holding resources needed by STOP.
 * Interrupt/scheduler/VM use requires its own audited resident adapter.
 * STOP=0 retires DMA, IRQs, threads and external references, including partial
 * START. PROBE and EXPORT have no side effects; IMPORT touches private state
 * only. START works on zero, imported, or stopped state. Callbacks are bounded. */
struct nlh_host {
    uint32_t abi, bytes;
    void *context;
    void (*log)(const char *message);
    uint64_t (*now_ns)(void);
    int (*platform)(void *, uint32_t operation, uint64_t a, uint64_t b, uint64_t *result);
    int (*call)(void *, uint32_t dependency, uint64_t argument, uint64_t *result);
};
typedef int (*nlh_entry_fn)(const struct nlh_host *, void *state,
                          uint32_t operation, uint64_t argument, uint64_t *result);

#ifdef KERNEL
int nlh_command(const char *command);
void nlh_disconnect(void);
int nlh_dispatch(const char *name, uint32_t interface_abi, uint64_t argument,
                 uint64_t *result);
/* Register once before the first upload; provider remains resident. */
int nlh_register_platform(uint32_t abi,
    int (*call)(const char *, uint32_t, uint64_t, uint64_t, uint64_t *));
int nlh_memory_enabled(void);
void nlh_memory_status(uint32_t nonce);
extern const char nlh_memory_build[];
int nlh_memory_create(unsigned slot, const void *code, unsigned length);
void *nlh_memory_address(unsigned slot);
void nlh_memory_destroy(unsigned slot);
#endif
#endif
