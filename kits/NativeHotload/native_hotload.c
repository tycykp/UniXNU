/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Named native modules, declared dependencies and transactional replacement.
 * The loader, memory manager and transport remain resident. No symbol patching.
 */
#include <sys/systm.h>
#include <sys/errno.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <mach/thread_act.h>
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <machine/machine_routines.h>
#include <libkern/crypto/sha2.h>
#include "native_hotload.h"
#include "native_hotload_core.h"
#include "native_core_algorithms.h"

_Static_assert(sizeof(struct nlh_header) == 432, "NLH2 header ABI");
_Static_assert(sizeof(struct nlh_host) == 48, "NLH2 host ABI");
_Static_assert(sizeof(struct nlh_invocation) == 160, "resident platform ABI 1");
_Static_assert(__atomic_always_lock_free(sizeof(unsigned), 0), "nonblocking admission");
_Static_assert(__atomic_always_lock_free(sizeof(uint64_t), 0), "nonblocking core counters");
_Static_assert(sizeof(uintptr_t) == sizeof(uint64_t), "64-bit invocation tokens");
#define NLH_CLOSED 0x80000000u
#define NLH_COUNT  0x7fffffffu
static LCK_GRP_DECLARE(nlh_group, "native_hotload");
static LCK_MTX_DECLARE(nlh_lock, &nlh_group);
static LCK_SPIN_DECLARE(nlh_context_lock, &nlh_group);
enum { NLH_EMPTY, NLH_RUNNING, NLH_STOPPED, NLH_QUARANTINED };
enum { NLH_UPDATE = 1, NLH_UNLOAD, NLH_INVOKE };
struct nlh_image {
    struct nlh_header header;
    nlh_entry_fn entry;
    void *state;
    uint64_t cookie, activation_epoch;
    unsigned generation;
    unsigned owner, memory, dependencies[NLH_MAX_DEPENDENCIES];
};
static struct nlh_slot {
    /* Name/array index remain stable even after unload. Published once. */
    char name[32];
    unsigned admission, state, active, generation, last_generation;
    struct nlh_image images[2];
} nlh_slots[NLH_MAX_MODULES];
static unsigned nlh_count;
static unsigned nlh_core_indices[4]; /* stable slot index + 1, zero before first load */
static struct { uint64_t calls, fallbacks, rejected; } nlh_core_stats[4];
static const char *const nlh_core_names[4] = { "", "xnu-sched", "xnu-vm", "xnu-vfs" };
static struct {
    unsigned busy, uploading, total, received, token, operation, target;
    int error, recovery_error;
    uint64_t argument, result;
    unsigned char upload[sizeof(struct nlh_header) + NLH_MAX_CODE];
} nlh;
static int (*nlh_platform_call)(const struct nlh_invocation *, uint32_t,
                                uint64_t, uint64_t, uint64_t *);
static uint64_t nlh_image_serial, nlh_epoch_serial, nlh_invocation_serial;

/* The module receives a unique invocation ID as an opaque context, never an
 * address. Never dereference a context supplied to a host callback.
 * Entries use private stack frames, so concurrent/nested calls cannot overwrite
 * another invocation's phase, identity or resident request provenance. */
struct nlh_frame {
    struct nlh_frame *next;
    struct nlh_image *image;
    thread_t thread;
    struct nlh_invocation invocation;
};
static struct nlh_frame *nlh_frames;

static uint64_t nlh_now(void);
static void nlh_log(const char *);
static int nlh_platform(void *, uint32_t, uint64_t, uint64_t, uint64_t *);
static int nlh_dependency_call(void *, uint32_t, uint64_t, uint64_t *);

/* Bounded even for the scheduler/VM gateways. Identifiers never wrap or reuse. */
static int nlh_identity(uint64_t *serial, uint64_t *identity)
{
    uint64_t value = __atomic_load_n(serial, __ATOMIC_RELAXED);
    for (unsigned tries = 0; tries < 8; tries++) {
        if (value == UINT64_MAX) return EOVERFLOW;
        if (__atomic_compare_exchange_n(serial, &value, value + 1, 0,
                __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
            *identity = value + 1;
            return 0;
        }
    }
    return EBUSY;
}

static void nlh_context_enter(struct nlh_frame *frame)
{
    boolean_t interrupts = ml_set_interrupts_enabled(FALSE);
    lck_spin_lock(&nlh_context_lock);
    frame->next = nlh_frames;
    nlh_frames = frame;
    lck_spin_unlock(&nlh_context_lock);
    ml_set_interrupts_enabled(interrupts);
}

static void nlh_context_leave(struct nlh_frame *frame)
{
    boolean_t interrupts = ml_set_interrupts_enabled(FALSE);
    lck_spin_lock(&nlh_context_lock);
    struct nlh_frame **cursor = &nlh_frames;
    while (*cursor && *cursor != frame) cursor = &(*cursor)->next;
    if (*cursor) *cursor = frame->next;
    lck_spin_unlock(&nlh_context_lock);
    ml_set_interrupts_enabled(interrupts);
}

static struct nlh_frame *nlh_context_current(void)
{
    struct nlh_frame *match = NULL;
    thread_t thread = current_thread();
    boolean_t interrupts = ml_set_interrupts_enabled(FALSE);
    lck_spin_lock(&nlh_context_lock);
    for (struct nlh_frame *frame = nlh_frames; frame; frame = frame->next) {
        if (frame->thread != thread) continue;
        /* Only the innermost invocation can use callbacks. A dependency cannot
         * reuse its parent's resident context, even on the same thread. */
        match = frame;
        break;
    }
    lck_spin_unlock(&nlh_context_lock);
    ml_set_interrupts_enabled(interrupts);
    return match; /* this thread owns the matching stack until entry returns */
}

static struct nlh_frame *nlh_context(void *context)
{
    struct nlh_frame *frame = nlh_context_current();
    return frame && frame->invocation.invocation_id == (uint64_t)(uintptr_t)context ? frame : NULL;
}

static int nlh_execute(struct nlh_image *image, unsigned op, uint64_t argument,
                        uint64_t *result, unsigned role, unsigned origin,
                        const struct nlh_invocation *parent)
{
    struct nlh_host host = { NLH_ABI, sizeof(struct nlh_host), NULL,
        NULL, NULL, NULL, NULL };
    struct nlh_frame frame = { .image = image,
        .thread = current_thread(), .invocation = {
            .abi = NLH_PLATFORM_ABI, .bytes = sizeof(struct nlh_invocation),
            .image_cookie = image->cookie, .activation_epoch = image->activation_epoch,
            .argument = argument, .slot_generation = image->generation,
            .phase = op, .role = role, .origin = origin,
            .root_origin = parent ? parent->root_origin : origin,
            .depth = parent ? parent->depth + 1 : 0 } };
    int error = nlh_identity(&nlh_invocation_serial, &frame.invocation.invocation_id);
    if (error) return error;
    frame.invocation.root_invocation_id = parent ? parent->root_invocation_id :
        frame.invocation.invocation_id;
    memcpy(frame.invocation.metadata.name, image->header.name, sizeof(image->header.name));
    frame.invocation.metadata.version = image->header.version;
    frame.invocation.metadata.interface_abi = image->header.interface_abi;
    frame.invocation.metadata.state_abi = image->header.state_abi;
    frame.invocation.metadata.kind = image->header.kind;
    frame.invocation.metadata.dependency_count = image->header.dependency_count;
    memcpy(frame.invocation.metadata.sha256, image->header.sha256, sizeof(image->header.sha256));
    if (image->header.kind == NLH_CORE) {
        /* Audited non-sleeping core policies receive no external callbacks. */
        return image->entry(&host, image->state, op, argument, result);
    }
    /* An address of a stack local could be reused by a later invocation. An
     * opaque never-reused ID also rejects a retained/stale host context. */
    host.context = (void *)(uintptr_t)frame.invocation.invocation_id;
    host.log = nlh_log;
    host.now_ns = nlh_now;
    host.platform = nlh_platform;
    host.call = nlh_dependency_call;
    nlh_context_enter(&frame);
    error = image->entry(&host, image->state, op, argument, result);
    nlh_context_leave(&frame);
    bzero(&host, sizeof(host));
    return error;
}

static uint64_t nlh_now(void)
{
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns;
}

static void nlh_log(const char *message)
{
    if (message) printf("NLH module: %.160s\n", message);
}

static int nlh_name(const char *name)
{
    unsigned i = 0;
    for (; i < 32 && name[i]; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_'))
            return 0;
    }
    return i && i < 32;
}

static int nlh_find(const char *name)
{
    unsigned count = __atomic_load_n(&nlh_count, __ATOMIC_ACQUIRE);
    for (unsigned i = 0; i < count; i++)
        if (!strcmp(name, nlh_slots[i].name)) return (int)i;
    return -1;
}

/* A single atomic word couples admission with the number of executing calls.
 * No lock, allocation, sleeping or callback before the image is pinned.
 * Active image/header/state are immutable while any reader owns a pin. */
static int nlh_call_slot_op(unsigned index, uint32_t abi, uint32_t minimum,
                            unsigned operation, uint64_t argument, uint64_t *result,
                            unsigned origin, const struct nlh_invocation *parent)
{
    struct nlh_slot *slot = &nlh_slots[index];
    unsigned value = __atomic_load_n(&slot->admission, __ATOMIC_ACQUIRE);
    unsigned retries = 0;
    for (;;) {
        if ((value & NLH_CLOSED) || (value & NLH_COUNT) == NLH_COUNT) return EBUSY;
        if (__atomic_compare_exchange_n(&slot->admission, &value, value + 1,
                0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) break;
        /* Contended core calls use the resident algorithm, never spin waiting
         * for another CPU's policy call or a module replacement. */
        if (operation == NLH_CORE_CALL && ++retries == 8) return EBUSY;
    }
    struct nlh_image *image = &slot->images[slot->active];
    uint64_t output = 0;
    int error = EPROGMISMATCH;
    if (image->header.interface_abi == abi && image->header.version >= minimum &&
        (operation != NLH_CORE_CALL || image->header.kind == NLH_CORE))
        error = nlh_execute(image, operation, argument, &output,
                            NLH_ROLE_ACTIVE, origin, parent);
    __atomic_fetch_sub(&slot->admission, 1, __ATOMIC_RELEASE);
    if (result) *result = output;
    return error;
}

static int nlh_call_slot(unsigned index, uint32_t abi, uint32_t minimum,
                         uint64_t argument, uint64_t *result, unsigned origin,
                         const struct nlh_invocation *parent)
{
    return nlh_call_slot_op(index, abi, minimum, NLH_CALL, argument, result, origin, parent);
}

uint64_t nlh_core_select(const struct nlh_core_request *request, uint64_t baseline)
{
    if (!request) return baseline;
    unsigned domain = request->domain;
    if (domain < NLH_CORE_SCHED || domain > NLH_CORE_VFS) return baseline;
    unsigned index = __atomic_load_n(&nlh_core_indices[domain], __ATOMIC_ACQUIRE);
    if (!index) return baseline; /* includes boot before the loader is usable */
    if (!nlh_core_request_valid(request) || nlh_core_baseline(request) != baseline) {
        __atomic_fetch_add(&nlh_core_stats[domain].rejected, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&nlh_core_stats[domain].fallbacks, 1, __ATOMIC_RELAXED);
        return baseline;
    }
    uint64_t result = 0;
    int error = nlh_call_slot_op(index - 1, NLH_CORE_INTERFACE, 1, NLH_CORE_CALL,
                                (uint64_t)(uintptr_t)request, &result, NLH_ORIGIN_CORE, NULL);
    if (!error && nlh_core_result_valid(request, baseline, result)) {
        __atomic_fetch_add(&nlh_core_stats[domain].calls, 1, __ATOMIC_RELAXED);
        return result;
    }
    if (!error || (error != EBUSY && error != ENOENT))
        __atomic_fetch_add(&nlh_core_stats[domain].rejected, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&nlh_core_stats[domain].fallbacks, 1, __ATOMIC_RELAXED);
    return baseline;
}

void nlh_core_status(void)
{
    printf("NLH core abi=1 domains=sched,vm,vfs resident_objects=1\n");
    for (unsigned i = NLH_CORE_SCHED; i <= NLH_CORE_VFS; i++)
        printf("NLH core domain=%u name=%s slot_plus_one=%u calls=%llu fallbacks=%llu rejected=%llu\n",
            i, nlh_core_names[i], __atomic_load_n(&nlh_core_indices[i], __ATOMIC_ACQUIRE),
            __atomic_load_n(&nlh_core_stats[i].calls, __ATOMIC_RELAXED),
            __atomic_load_n(&nlh_core_stats[i].fallbacks, __ATOMIC_RELAXED),
            __atomic_load_n(&nlh_core_stats[i].rejected, __ATOMIC_RELAXED));
}

int nlh_dispatch(const char *name, uint32_t abi, uint64_t argument, uint64_t *result)
{
    if (!name || !nlh_name(name) || !abi) return EINVAL;
    int index = nlh_find(name);
    return index < 0 ? ENOENT : nlh_call_slot((unsigned)index, abi, 1, argument,
                                            result, NLH_ORIGIN_GENERIC, NULL);
}

int nlh_dispatch_resident(const char *name, uint32_t abi, uint64_t argument, uint64_t *result)
{
    if (!name || !nlh_name(name) || !abi) return EINVAL;
    /* A nested module/provider/dependency path must never mint a new resident
     * root. The adapter submits a fresh request after the outer entry returns. */
    if (nlh_context_current()) return EACCES;
    int index = nlh_find(name);
    return index < 0 ? ENOENT : nlh_call_slot((unsigned)index, abi, 1, argument,
                                            result, NLH_ORIGIN_RESIDENT, NULL);
}

static int nlh_dependency_call(void *context, uint32_t dependency,
                               uint64_t argument, uint64_t *result)
{
    struct nlh_frame *frame = nlh_context(context);
    if (!frame) return EACCES;
    if (frame->invocation.phase != NLH_START && frame->invocation.phase != NLH_STOP &&
        frame->invocation.phase != NLH_CALL) return EACCES;
    if (frame->invocation.depth >= NLH_MAX_MODULES) return ELOOP;
    struct nlh_image *image = frame->image;
    if (image->header.kind == NLH_CORE) return ENOTSUP;
    if (dependency >= image->header.dependency_count) return EINVAL;
    const struct nlh_dependency *dep = &image->header.dependencies[dependency];
    return nlh_call_slot(image->dependencies[dependency], dep->interface_abi,
                         dep->minimum_version, argument, result,
                         NLH_ORIGIN_DEPENDENCY, &frame->invocation);
}

static int nlh_platform(void *context, uint32_t op, uint64_t a, uint64_t b, uint64_t *out)
{
    struct nlh_frame *frame = nlh_context(context);
    if (!frame) return EACCES;
    if (frame->invocation.phase != NLH_START && frame->invocation.phase != NLH_STOP &&
        frame->invocation.phase != NLH_CALL) return EACCES;
    if (frame->image->header.kind == NLH_CORE) return ENOTSUP;
    if (!nlh_platform_call) return ENOTSUP;
    /* The provider receives a borrowed immutable copy, not module-owned data. */
    const struct nlh_invocation invocation = frame->invocation;
    return nlh_platform_call(&invocation, op, a, b, out);
}

int nlh_register_platform(uint32_t abi,
    int (*call)(const char *, uint32_t, uint64_t, uint64_t, uint64_t *))
{
    (void)abi;
    (void)call;
    return ENOTSUP;
}

int nlh_register_platform_v2(uint32_t abi,
    int (*call)(const struct nlh_invocation *, uint32_t, uint64_t, uint64_t, uint64_t *))
{
    if (abi != NLH_PLATFORM_ABI || !call) return EINVAL;
    if (nlh_context_current()) return EACCES;
    lck_mtx_lock(&nlh_lock);
    int error = (nlh_platform_call || nlh.token || nlh.busy) ? EBUSY : 0;
    if (!error) nlh_platform_call = call;
    lck_mtx_unlock(&nlh_lock);
    return error;
}

static int nlh_invoke(struct nlh_image *image, unsigned op, uint64_t argument,
                      uint64_t *result, unsigned role)
{
    if (op == NLH_START) {
        int error = nlh_identity(&nlh_epoch_serial, &image->activation_epoch);
        if (error) return error;
    }
    return nlh_execute(image, op, argument, result, role, NLH_ORIGIN_MANAGEMENT, NULL);
}

static void nlh_release(struct nlh_image *image)
{
    if (image->entry) nlh_memory_destroy(image->memory);
    if (image->state) kheap_free(KHEAP_DEFAULT, image->state, image->header.state_bytes);
    bzero(image, sizeof(*image));
}

static int nlh_padded_name(const char *name)
{
    if (!nlh_name(name)) return 0;
    unsigned i = (unsigned)strlen(name);
    for (; i < 32; i++) if (name[i]) return 0;
    return 1;
}

static int nlh_validate(struct nlh_header *h)
{
    memcpy(h, nlh.upload, sizeof(*h));
    if (h->magic != NLH_MAGIC || h->abi != NLH_ABI ||
        h->header_bytes != sizeof(*h) || h->reserved ||
        h->code_bytes < 4 || h->code_bytes > NLH_MAX_CODE ||
        h->code_bytes != nlh.total - sizeof(*h) ||
        !h->state_bytes || h->state_bytes > NLH_MAX_STATE ||
        h->entry_offset > h->code_bytes - 4 || (h->entry_offset & 3) ||
        !h->version || !h->interface_abi || !h->state_abi || h->kind > NLH_CORE ||
        h->dependency_count > NLH_MAX_DEPENDENCIES || !nlh_padded_name(h->name))
        return EINVAL;
    for (unsigned i = 0; i < NLH_MAX_DEPENDENCIES; i++) {
        struct nlh_dependency *d = &h->dependencies[i];
        if (i >= h->dependency_count) {
            const unsigned char *p = (const unsigned char *)d;
            for (unsigned j = 0; j < sizeof(*d); j++) if (p[j]) return EINVAL;
        } else {
            if (!nlh_padded_name(d->name) || !d->interface_abi || !d->minimum_version ||
                !strcmp(d->name, h->name)) return EINVAL;
            for (unsigned j = 0; j < i; j++)
                if (!strcmp(d->name, h->dependencies[j].name)) return EINVAL;
        }
    }
    unsigned domain = 0;
    for (unsigned i = NLH_CORE_SCHED; i <= NLH_CORE_VFS; i++)
        if (!strcmp(h->name, nlh_core_names[i])) domain = i;
    /* Reserved core names cannot be occupied by ordinary modules. These
     * non-sleeping policies may not acquire resources or call dependencies. */
    if (h->kind == NLH_CORE || domain) {
        if (!domain || h->kind != NLH_CORE || h->interface_abi != NLH_CORE_INTERFACE ||
            h->state_abi != NLH_CORE_STATE_ABI || h->dependency_count) return EINVAL;
    }
    SHA256_CTX context;
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256_Init(&context);
    SHA256_Update(&context, h, sizeof(*h) - sizeof(h->sha256));
    SHA256_Update(&context, nlh.upload + sizeof(*h), h->code_bytes);
    SHA256_Final(hash, &context);
    return memcmp(hash, h->sha256, sizeof(hash)) ? EIO : 0;
}

/* One mutating worker owns graph changes. Readers only use pinned image edges.
 * The reverse closure remains closed if STOP/cleanup/rollback is uncertain. */
static unsigned nlh_dependants(unsigned index)
{
    unsigned mask = 1u << index, previous;
    do {
        previous = mask;
        for (unsigned i = 0; i < nlh_count; i++) {
            struct nlh_slot *s = &nlh_slots[i];
            if (s->state != NLH_RUNNING && s->state != NLH_QUARANTINED) continue;
            struct nlh_image *image = &s->images[s->active];
            for (unsigned j = 0; j < image->header.dependency_count; j++)
                if (mask & (1u << image->dependencies[j])) mask |= 1u << i;
            /* Quarantined candidates can also retain provider resources. */
            if (s->state == NLH_QUARANTINED) {
                image = &s->images[s->active ^ 1u];
                for (unsigned j = 0; j < image->header.dependency_count; j++)
                    if (mask & (1u << image->dependencies[j])) mask |= 1u << i;
            }
        }
    } while (mask != previous);
    return mask;
}

static int nlh_resolve(struct nlh_image *image, unsigned target)
{
    unsigned dependants = nlh_dependants(target);
    for (unsigned i = 0; i < image->header.dependency_count; i++) {
        const struct nlh_dependency *dep = &image->header.dependencies[i];
        int found = nlh_find(dep->name);
        if (found < 0) return ENOENT;
        if (dependants & (1u << found)) return ELOOP;
        struct nlh_slot *s = &nlh_slots[found];
        struct nlh_header *h = &s->images[s->active].header;
        if (s->state != NLH_RUNNING || (__atomic_load_n(&s->admission, __ATOMIC_ACQUIRE) & NLH_CLOSED))
            return EBUSY;
        if (h->interface_abi != dep->interface_abi || h->version < dep->minimum_version)
            return EPROGMISMATCH;
        image->dependencies[i] = (unsigned)found;
    }
    return 0;
}

static int nlh_drain(unsigned mask)
{
    uint64_t deadline = nlh_now() + 5000000000ULL;
    for (unsigned i = 0; i < nlh_count; i++)
        if (mask & (1u << i))
            __atomic_fetch_or(&nlh_slots[i].admission, NLH_CLOSED, __ATOMIC_ACQ_REL);
    for (;;) {
        unsigned count = 0;
        for (unsigned i = 0; i < nlh_count; i++)
            if (mask & (1u << i))
                count |= __atomic_load_n(&nlh_slots[i].admission, __ATOMIC_ACQUIRE) & NLH_COUNT;
        if (!count) return 0;
        if (nlh_now() >= deadline) return EBUSY;
        struct timespec pause = {0, 10000000};
        (void)msleep(&nlh, NULL, PZERO, "nlhdrain", &pause);
    }
}

static void nlh_reopen(unsigned mask)
{
    for (unsigned i = 0; i < nlh_count; i++)
        if ((mask & (1u << i)) && nlh_slots[i].state == NLH_RUNNING)
            __atomic_fetch_and(&nlh_slots[i].admission, NLH_COUNT, __ATOMIC_RELEASE);
}

static void nlh_worker(void *unused, wait_result_t wait)
{
    (void)unused; (void)wait;
    uint64_t result = 0;
    int error = 0, recovery_error = 0;
    unsigned target = nlh.target, closed = 0, keep_closed = 0;
    struct nlh_slot *slot = NULL;
    struct nlh_image *old = NULL, *next = NULL;
    void *transfer_data = NULL;
    struct nlh_header header;
    if (nlh.operation == NLH_UPDATE) {
        error = nlh_validate(&header);
        if (error) goto done;
        int found = nlh_find(header.name);
        if (found < 0) {
            if (nlh_count == NLH_MAX_MODULES) { error = ENOSPC; goto done; }
            target = nlh_count;
            slot = &nlh_slots[target];
            memcpy(slot->name, header.name, sizeof(slot->name));
            slot->admission = NLH_CLOSED;
            __atomic_store_n(&nlh_count, target + 1, __ATOMIC_RELEASE);
            for (unsigned i = NLH_CORE_SCHED; i <= NLH_CORE_VFS; i++)
                if (!strcmp(header.name, nlh_core_names[i]))
                    __atomic_store_n(&nlh_core_indices[i], target + 1, __ATOMIC_RELEASE);
        } else target = (unsigned)found;
    }
    slot = &nlh_slots[target];
    old = &slot->images[slot->active];
    next = &slot->images[slot->active ^ 1u];
    if (slot->state == NLH_QUARANTINED ||
        (slot->state == NLH_RUNNING &&
         (__atomic_load_n(&slot->admission, __ATOMIC_ACQUIRE) & NLH_CLOSED))) {
        error = EBUSY; goto done;
    }
    if (nlh.operation == NLH_INVOKE) {
        error = nlh_call_slot(target, old->header.interface_abi, 1, nlh.argument,
                              &result, NLH_ORIGIN_USB, NULL);
        goto done;
    }
    unsigned closure = nlh_dependants(target);
    /* Never detach a provider that live/quarantined consumers may still own. */
    if (nlh.operation == NLH_UNLOAD && closure != (1u << target)) {
        error = EBUSY; goto done;
    }
    for (unsigned i = 0; i < nlh_count; i++) {
        if (!(closure & (1u << i)) || i == target) continue;
        if (nlh_slots[i].state != NLH_RUNNING ||
            (__atomic_load_n(&nlh_slots[i].admission, __ATOMIC_ACQUIRE) & NLH_CLOSED)) {
            error = EBUSY; goto done;
        }
    }
    if (nlh.operation == NLH_UPDATE) {
        if (slot->state != NLH_EMPTY) {
            if (header.version <= old->header.version) { error = EALREADY; goto done; }
            if (header.interface_abi != old->header.interface_abi ||
                header.state_abi != old->header.state_abi || header.kind != old->header.kind) {
                error = EPROGMISMATCH; goto done;
            }
        }
        next->header = header;
        next->owner = target;
        next->memory = target * 2 + (slot->active ^ 1u);
        if (slot->last_generation == UINT32_MAX) { error = EOVERFLOW; goto reject; }
        next->generation = ++slot->last_generation;
        error = nlh_identity(&nlh_image_serial, &next->cookie);
        if (error) goto reject;
        error = nlh_resolve(next, target);
        if (error) goto reject;
        /* Private state may contain resident resource pointers. Keep it out
         * of KHEAP_DATA_BUFFERS; only serialized transfers are pure data. */
        next->state = kheap_alloc(KHEAP_DEFAULT, header.state_bytes, Z_WAITOK | Z_ZERO);
        if (!next->state) { error = ENOMEM; goto reject; }
        error = nlh_memory_create(next->memory, nlh.upload + sizeof(header), header.code_bytes);
        if (error) goto reject;
        next->entry = (nlh_entry_fn)((uintptr_t)nlh_memory_address(next->memory) + header.entry_offset);
        error = nlh_invoke(next, NLH_PROBE, 0, &result, NLH_ROLE_CANDIDATE);
        if (error) goto reject;
        if (slot->state == NLH_RUNNING) {
            transfer_data = kalloc_data(NLH_MAX_STATE, Z_WAITOK | Z_ZERO);
            if (!transfer_data) { error = ENOMEM; goto reject; }
        }
    }
    closed = closure;
    error = nlh_drain(closed);
    if (error) goto reject;
    if (slot->state == NLH_RUNNING) {
        error = nlh_invoke(old, NLH_STOP, 0, &result, NLH_ROLE_ACTIVE);
        if (error) { slot->state = NLH_QUARANTINED; keep_closed = 1; goto reject; }
        slot->state = NLH_STOPPED;
    }
    if (nlh.operation == NLH_UNLOAD) {
        nlh_release(old); slot->state = NLH_EMPTY; goto done;
    }
    if (slot->state == NLH_STOPPED) {
        struct nlh_transfer transfer = {header.state_abi, 0, NLH_MAX_STATE, 0, transfer_data};
        error = nlh_invoke(old, NLH_EXPORT, (uint64_t)(uintptr_t)&transfer,
                            &result, NLH_ROLE_ACTIVE);
        if (!error && (transfer.data != transfer_data || transfer.capacity != NLH_MAX_STATE ||
            transfer.bytes > NLH_MAX_STATE || transfer.abi != header.state_abi || transfer.reserved))
            error = EINVAL;
        if (!error) error = nlh_invoke(next, NLH_IMPORT, (uint64_t)(uintptr_t)&transfer,
                                      &result, NLH_ROLE_CANDIDATE);
        if (error) goto rollback; /* IMPORT has no external side effects. */
    }
    error = nlh_invoke(next, NLH_START, 0, &result, NLH_ROLE_CANDIDATE);
    if (error) {
        recovery_error = nlh_invoke(next, NLH_STOP, 0, &result, NLH_ROLE_CANDIDATE);
        if (recovery_error) {
            slot->state = NLH_QUARANTINED; keep_closed = 1; goto done;
        }
        goto rollback;
    }
    if (slot->state == NLH_STOPPED) nlh_release(old);
    slot->active ^= 1u;
    slot->state = NLH_RUNNING;
    slot->generation = next->generation;
    goto done;
rollback:
    nlh_release(next);
    if (slot->state == NLH_STOPPED) {
        recovery_error = nlh_invoke(old, NLH_START, 0, &result, NLH_ROLE_ROLLBACK);
        slot->state = recovery_error ? NLH_QUARANTINED : NLH_RUNNING;
        keep_closed = recovery_error != 0;
    }
    goto done;
reject:
    if (next) nlh_release(next);
done:
    if (transfer_data) kfree_data(transfer_data, NLH_MAX_STATE);
    if (!keep_closed) nlh_reopen(closed);
    lck_mtx_lock(&nlh_lock);
    nlh.error = error; nlh.recovery_error = recovery_error; nlh.result = result;
    nlh.target = target; nlh.uploading = 0; nlh.total = nlh.received = 0;
    printf("NLH done token=%u error=%d recovery=%d slot=%u generation=%u active=%u state=%u result=%llu\n",
        nlh.token, error, recovery_error, target,
        slot ? slot->generation : 0, slot ? slot->images[slot->active].header.version : 0,
        slot ? slot->state : NLH_EMPTY, result);
    nlh.busy = 0;
    lck_mtx_unlock(&nlh_lock);
    thread_terminate(current_thread());
}

static int nlh_number(const char **cursor, uint64_t *value)
{
    const char *p = *cursor;
    uint64_t n = 0;
    if (*p < '0' || *p > '9') return 0;
    do {
        unsigned digit = (unsigned)(*p++ - '0');
        if (n > (UINT64_MAX - digit) / 10) return 0;
        n = n * 10 + digit;
    } while (*p >= '0' && *p <= '9');
    *value = n; *cursor = p;
    return 1;
}

static int nlh_hex(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

void nlh_disconnect(void)
{
    lck_mtx_lock(&nlh_lock);
    if (!nlh.busy) { nlh.uploading = 0; nlh.total = nlh.received = 0; }
    lck_mtx_unlock(&nlh_lock);
}

int nlh_command(const char *cmd)
{
    if (strncmp(cmd, "hl.", 3)) return 0;
    if (!nlh_memory_enabled()) { printf("NLH disabled unixnu_hotload=1 development-arm64-required\n"); return 1; }
    const char *p = cmd;
    uint64_t token = 0, number = 0;
    unsigned operation = 0, target = NLH_MAX_MODULES;
    int error = 0;
    lck_mtx_lock(&nlh_lock);
    if (!strcmp(cmd, "hl.status") || !strncmp(cmd, "hl.status ", 10)) {
        if (cmd[9]) {
            p = cmd + 10;
            if (!nlh_number(&p, &number) || *p || !number || number > UINT32_MAX) {
                error = EINVAL; goto reply;
            }
        }
        printf("%s\n", nlh_memory_build);
        printf("NLH status abi=2 nonce=%u token=%u busy=%u slots=%u received=%u total=%u error=%d recovery=%d result=%llu provider=%u platform_abi=1 resident_dispatch=1 uptime_ns=%llu\n",
            (unsigned)number, nlh.token, nlh.busy, __atomic_load_n(&nlh_count, __ATOMIC_ACQUIRE),
            nlh.received, nlh.total, nlh.error, nlh.recovery_error, nlh.result,
            nlh_platform_call != NULL, nlh_now());
        goto out;
    }
    if (!strcmp(cmd, "hl.core")) { nlh_core_status(); goto reply; }
    if (!strcmp(cmd, "hl.memory") || !strncmp(cmd, "hl.memory ", 10)) {
        if (cmd[9]) {
            p = cmd + 10;
            if (!nlh_number(&p, &number) || *p || !number || number > UINT32_MAX) {
                error = EINVAL; goto reply;
            }
        }
        nlh_memory_status((uint32_t)number);
        goto out;
    }
    /* Worker owns image/graph writes while busy. Global/core status remain usable. */
    if (nlh.busy) { error = EBUSY; goto reply; }
    if (!strcmp(cmd, "hl.list")) {
        for (unsigned i = 0; i < nlh_count; i++) {
            struct nlh_slot *s = &nlh_slots[i];
            struct nlh_header *h = &s->images[s->active].header;
            unsigned admission = __atomic_load_n(&s->admission, __ATOMIC_ACQUIRE);
            printf("NLH module slot=%u name=%s state=%u active=%u generation=%u kind=%u interface=%u state_abi=%u deps=%u blocked=%u calls=%u image_cookie=%llu activation_epoch=%llu candidate=%u candidate_cookie=%llu candidate_epoch=%llu candidate_generation=%u\n",
                i, s->name, s->state, h->version, s->generation, h->kind,
                h->interface_abi, h->state_abi, h->dependency_count,
                !!(admission & NLH_CLOSED), admission & NLH_COUNT,
                s->images[s->active].cookie, s->images[s->active].activation_epoch,
                s->images[s->active ^ 1u].header.version,
                s->images[s->active ^ 1u].cookie, s->images[s->active ^ 1u].activation_epoch,
                s->images[s->active ^ 1u].generation);
        }
        goto reply;
    }
    if (!strncmp(cmd, "hl.begin ", 9)) {
        p = cmd + 9;
        if (!nlh_number(&p, &number) || *p || number < sizeof(struct nlh_header) + 4 || number > sizeof(nlh.upload)) {
            error = EINVAL; goto reply;
        }
        if (nlh.uploading) { error = EBUSY; goto reply; }
        if (nlh.token == UINT32_MAX) { error = EOVERFLOW; goto reply; }
        nlh.token++; nlh.total = (unsigned)number; nlh.received = 0; nlh.uploading = 1;
        goto reply;
    }
    if (!strncmp(cmd, "hl.data ", 8)) {
        p = cmd + 8;
        if (!nlh_number(&p, &token) || *p++ != ' ' || !nlh_number(&p, &number) || *p++ != ' ') {
            error = EINVAL; goto reply;
        }
        size_t hex = strlen(p);
        if (!nlh.uploading || token != nlh.token || !hex || hex > 32 || (hex & 1) ||
            number != nlh.received || hex / 2 > nlh.total - nlh.received) { error = EINVAL; goto reply; }
        unsigned char bytes[16];
        for (unsigned i = 0; i < hex / 2; i++) {
            int a = nlh_hex(p[2*i]), b = nlh_hex(p[2*i+1]);
            if (a < 0 || b < 0) { error = EINVAL; goto reply; }
            bytes[i] = (unsigned char)((a << 4) | b);
        }
        memcpy(nlh.upload + nlh.received, bytes, hex / 2);
        nlh.received += (unsigned)hex / 2;
        goto reply;
    }
    if (!strncmp(cmd, "hl.commit ", 10) || !strncmp(cmd, "hl.abort ", 9)) {
        bool commit = cmd[3] == 'c';
        p = cmd + (commit ? 10 : 9);
        if (!nlh_number(&p, &token) || *p || token != nlh.token || !nlh.uploading) {
            error = EINVAL; goto reply;
        }
        if (!commit) { nlh.uploading = 0; nlh.total = nlh.received = 0; goto reply; }
        if (nlh.received != nlh.total) { error = EINVAL; goto reply; }
        operation = NLH_UPDATE;
    } else if (!strncmp(cmd, "hl.unload ", 10) || !strncmp(cmd, "hl.call ", 8)) {
        operation = cmd[3] == 'u' ? NLH_UNLOAD : NLH_INVOKE;
        p = cmd + (operation == NLH_UNLOAD ? 10 : 8);
        char name[32] = {0};
        unsigned i = 0;
        while (*p && *p != ' ' && i < 31) name[i++] = *p++;
        if (!nlh_name(name) || (operation == NLH_UNLOAD ? *p != 0 :
            (*p++ != ' ' || !nlh_number(&p, &number) || *p))) { error = EINVAL; goto reply; }
        int found = nlh_find(name);
        if (found < 0) { error = ENOENT; goto reply; }
        target = (unsigned)found;
    } else { error = EINVAL; goto reply; }
    if (operation != NLH_UPDATE &&
        (nlh.uploading || nlh_slots[target].state != NLH_RUNNING)) { error = EBUSY; goto reply; }
    if (operation != NLH_UPDATE) {
        if (nlh.token == UINT32_MAX) { error = EOVERFLOW; goto reply; }
        nlh.token++;
    }
    nlh.operation = operation; nlh.target = target; nlh.argument = number; nlh.busy = 1;
    thread_t thread = THREAD_NULL;
    if (kernel_thread_start(nlh_worker, NULL, &thread) != KERN_SUCCESS) {
        nlh.busy = 0; error = ENOMEM;
    } else thread_deallocate(thread);
reply:
    printf("NLH ack token=%u error=%d busy=%u received=%u\n", nlh.token, error, nlh.busy, nlh.received);
out:
    lck_mtx_unlock(&nlh_lock);
    return 1;
}
