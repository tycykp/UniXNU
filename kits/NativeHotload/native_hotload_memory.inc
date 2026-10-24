/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Included ONLY in Darwin 22 arm/pmap/pmap.c by the opt-in patch generator.
 * A reserved, otherwise unmapped static-VA twig supplies the code submap.
 * No existing parent-table permissions or ordinary pmap policies are changed.
 */
#include <pexpert/pexpert.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include "../../vm/native_hotload_vm.h"
#include "native_hotload.h"

#if (DEVELOPMENT || DEBUG) && !XNU_MONITOR && !defined(KERNEL_INTEGRITY_KTRR) && !defined(KERNEL_INTEGRITY_CTRR) && !__arm64e__ && !KASAN
#define NLH_MEMORY_SUPPORTED 1
const char nlh_memory_build[] = "NLH memory abi=2 executable=1";
#else
#define NLH_MEMORY_SUPPORTED 0
const char nlh_memory_build[] = "NLH memory abi=2 executable=0";
#endif

enum {
    NLH_MEM_ARENA = 1,
    NLH_MEM_ALLOCATE,
    NLH_MEM_WRITE_VERIFY,
    NLH_MEM_PROTECT,
    NLH_MEM_CACHE,
    NLH_MEM_SEAL,
    NLH_MEM_EXECUTE_VERIFY,
    NLH_MEM_REVOKE,
    NLH_MEM_FREE,
    NLH_MEM_COMPLETE
};

struct nlh_memory_diagnostic {
    uint32_t stage, error, kr, slot, bytes, checked_pages, failed_page;
    uint32_t rollback_error, arena_bytes;
    uint64_t leaf, effective, parent_flags;
};

static LCK_GRP_DECLARE(nlh_memory_group, "native_hotload_memory");
static LCK_MTX_DECLARE(nlh_memory_lock, &nlh_memory_group);
static struct {
    vm_offset_t address;
    vm_size_t size;
    unsigned sealed;
} nlh_memory[NLH_MAX_MODULES * 2];
static struct nlh_memory_request {
    vm_offset_t start, end;
    thread_t owner;
    unsigned slot;
} nlh_execute_request, nlh_revoke_request;

/* The boot writer runs before workers exist; subsequent writers own the
 * memory mutex. Each field is atomic so bounded readers also avoid C races. */
static struct nlh_memory_diagnostic nlh_memory_report;
static struct nlh_memory_diagnostic nlh_memory_boot_report;
static uint32_t nlh_memory_report_sequence;
static uint32_t nlh_memory_arena_ready;
static uint32_t nlh_memory_arena_kr = KERN_NOT_SUPPORTED;

int nlh_memory_enabled(void);
int nlh_memory_create(unsigned, const void *, unsigned);
void *nlh_memory_address(unsigned);
void nlh_memory_destroy(unsigned);
void nlh_memory_status(uint32_t);

static void nlh_memory_publish(const struct nlh_memory_diagnostic *report)
{
    __atomic_add_fetch(&nlh_memory_report_sequence, 1u, __ATOMIC_ACQ_REL);
#define NLH_MEMORY_STORE(field) \
    __atomic_store_n(&nlh_memory_report.field, report->field, __ATOMIC_RELAXED)
    NLH_MEMORY_STORE(stage);
    NLH_MEMORY_STORE(error);
    NLH_MEMORY_STORE(kr);
    NLH_MEMORY_STORE(slot);
    NLH_MEMORY_STORE(bytes);
    NLH_MEMORY_STORE(checked_pages);
    NLH_MEMORY_STORE(failed_page);
    NLH_MEMORY_STORE(rollback_error);
    NLH_MEMORY_STORE(arena_bytes);
    NLH_MEMORY_STORE(leaf);
    NLH_MEMORY_STORE(effective);
    NLH_MEMORY_STORE(parent_flags);
#undef NLH_MEMORY_STORE
    __atomic_add_fetch(&nlh_memory_report_sequence, 1u, __ATOMIC_RELEASE);
}

int nlh_memory_enabled(void)
{
#if NLH_MEMORY_SUPPORTED
    int enabled = 0;
    PE_parse_boot_argn("unixnu_hotload", &enabled, sizeof(enabled));
    return enabled == 1;
#else
    return 0;
#endif
}

/* kmem_init calls this before allocating any entry in the reserved static
 * region, after pmap_bootstrap and before concurrent VM activity. Do not use
 * late-initialized mutexes or modify a descriptor during this preflight. */
kern_return_t nlh_memory_boot_range(vm_map_offset_t *start, vm_map_size_t *size)
{
    struct nlh_memory_diagnostic *report = &nlh_memory_boot_report;
    bzero(report, sizeof(*report));
    report->stage = NLH_MEM_ARENA;
    if (start) *start = 0;
    if (size) *size = 0;
    kern_return_t kr = KERN_NOT_SUPPORTED;
    if (!start || !size) { kr = KERN_INVALID_ARGUMENT; goto fail; }
    if (!nlh_memory_enabled()) goto fail;
    if (!kernel_pmap || !nx_enabled || !kernel_pmap->nx_enabled) goto fail;

    const pt_attr_t *pt_attr = pmap_get_pt_attr(kernel_pmap);
    if (pt_attr_root_level(pt_attr) >= PMAP_TT_L2_LEVEL ||
        pt_attr_twig_level(pt_attr) != PMAP_TT_L2_LEVEL ||
        pt_attr_leaf_level(pt_attr) != PMAP_TT_L3_LEVEL ||
        pt_attr_twig_size(pt_attr) != ARM_TT_L2_SIZE ||
        pt_attr_page_size(pt_attr) > PAGE_SIZE) goto fail;

    const vm_map_offset_t base = LOW_GLOBAL_BASE_ADDRESS + 2 * ARM_TT_L2_SIZE;
    const vm_map_size_t needed = (vm_map_size_t)NLH_MAX_CODE * 2 * NLH_MAX_MODULES;
    const vm_map_size_t extent = (needed + ARM_TT_L2_OFFMASK) & ~ARM_TT_L2_OFFMASK;
    vm_map_offset_t end;
    report->bytes = (uint32_t)extent;
    report->arena_bytes = (uint32_t)extent;
    if (!extent || os_add_overflow(base, extent, &end) || end > gVirtBase ||
        base < kernel_pmap->min || end > kernel_pmap->max) {
        kr = KERN_INVALID_ADDRESS;
        goto fail;
    }
    for (vm_map_offset_t va = base; va < end; va += ARM_TT_L2_SIZE) {
        for (unsigned level = pt_attr_root_level(pt_attr);
             level <= PMAP_TT_L2_LEVEL; ++level) {
            tt_entry_t *ttep = pmap_ttne(kernel_pmap, level, va);
            if (!ttep) { kr = KERN_INVALID_ADDRESS; goto fail; }
            tt_entry_t tte = *ttep;
            if (level == PMAP_TT_L2_LEVEL) {
                if (tte != ARM_TTE_EMPTY) { kr = KERN_NO_SPACE; goto fail; }
                continue;
            }
            report->parent_flags |= tte & (ARM_TTE_TABLE_PXN |
                ARM_TTE_TABLE_XN | ARM_TTE_TABLE_AP(AP_MASK));
            /* Check this descriptor before asking pmap_ttne for the next
             * level: walking through an intermediate block may panic. */
            if ((tte & (ARM_TTE_TYPE_MASK | ARM_TTE_VALID)) !=
                (ARM_TTE_TYPE_TABLE | ARM_TTE_VALID)) {
                kr = KERN_INVALID_ADDRESS;
                goto fail;
            }
            if (tte & (ARM_TTE_TABLE_PXN | ARM_TTE_TABLE_AP(ARM_TTE_TABLE_AP_RO))) {
                kr = KERN_PROTECTION_FAILURE;
                goto fail;
            }
        }
    }
    *start = base;
    *size = extent;
    nlh_memory_publish(report);
    return KERN_SUCCESS;
fail:
    report->error = kr == KERN_NOT_SUPPORTED ? ENOTSUP : EACCES;
    report->kr = (uint32_t)kr;
    nlh_memory_publish(report);
    return kr;
}

void nlh_memory_boot_result(kern_return_t kr)
{
    struct nlh_memory_diagnostic *report = &nlh_memory_boot_report;
    report->stage = NLH_MEM_ARENA;
    if (!report->error && kr != KERN_SUCCESS) {
        report->error = EACCES;
        report->kr = (uint32_t)kr;
    }
    __atomic_store_n(&nlh_memory_arena_kr, (uint32_t)kr, __ATOMIC_RELEASE);
    __atomic_store_n(&nlh_memory_arena_ready, kr == KERN_SUCCESS, __ATOMIC_RELEASE);
    nlh_memory_publish(report);
}

/* These predicates run inside the existing pmap/PVH locks. Only a synchronous
 * operation on a registered allocation may change its execution permission. */
static bool nlh_memory_request_allowed(const struct nlh_memory_request *request,
                                     pmap_t pmap, vm_map_address_t start,
                                     vm_map_address_t end, void *args)
{
    return args == request && pmap == kernel_pmap &&
        request->owner == current_thread() && request->slot < NLH_MAX_MODULES * 2 &&
        request->start == nlh_memory[request->slot].address &&
        request->end - request->start == nlh_memory[request->slot].size &&
        start >= request->start && end <= request->end && start < end;
}

static bool nlh_execute_allowed(pmap_t pmap, vm_map_address_t start,
                               vm_map_address_t end, vm_prot_t prot, void *args)
{
    return prot == (VM_PROT_READ | VM_PROT_EXECUTE) &&
        nlh_memory_request_allowed(&nlh_execute_request, pmap, start, end, args);
}

static bool nlh_revoke_allowed(pmap_t pmap, vm_map_address_t start,
                              vm_map_address_t end, vm_prot_t prot, void *args)
{
    return prot == VM_PROT_READ &&
        nlh_memory_request_allowed(&nlh_revoke_request, pmap, start, end, args);
}

/* Inspect every hardware leaf, including PAGE_RATIO > 1. All reported bits
 * are permissions only; diagnostic snapshots contain no kernel addresses. */
static bool nlh_memory_check(vm_offset_t address, vm_size_t size,
                             uint64_t expected, struct nlh_memory_diagnostic *report)
{
    bool valid = true;
    const pt_attr_t *pt_attr = pmap_get_pt_attr(kernel_pmap);
    report->checked_pages = 0;
    report->failed_page = 0;
    pmap_lock(kernel_pmap, PMAP_LOCK_SHARED);
    for (vm_offset_t va = address; va < address + size;
         va += pt_attr_page_size(pt_attr)) {
        uint64_t parent_flags = 0;
        pt_entry_t leaf = 0;
        bool mapped = true;
        for (unsigned level = pt_attr_root_level(pt_attr);
             level <= pt_attr_leaf_level(pt_attr); ++level) {
            tt_entry_t *ttep = pmap_ttne(kernel_pmap, level, va);
            if (!ttep) { mapped = false; break; }
            tt_entry_t tte = *ttep;
            if (level == pt_attr_leaf_level(pt_attr)) {
                leaf = tte;
                mapped = (tte & ARM_PTE_TYPE_VALID) == ARM_PTE_TYPE;
                break;
            }
            if ((tte & (ARM_TTE_TYPE_MASK | ARM_TTE_VALID)) !=
                (ARM_TTE_TYPE_TABLE | ARM_TTE_VALID)) {
                mapped = false;
                break;
            }
            parent_flags |= tte & (ARM_TTE_TABLE_PXN | ARM_TTE_TABLE_XN |
                ARM_TTE_TABLE_AP(AP_MASK));
        }
        report->leaf = leaf & (ARM_PTE_APMASK | ARM_PTE_NX | ARM_PTE_PNX);
        report->parent_flags = parent_flags;
        report->effective = mapped ? pmap_get_arm64_prot(kernel_pmap, va) : 0;
        ++report->checked_pages;
        if (!mapped || report->leaf != expected || report->effective != expected ||
            (parent_flags & ARM_TTE_TABLE_AP(ARM_TTE_TABLE_AP_RO))) {
            report->failed_page = report->checked_pages - 1;
            valid = false;
            break;
        }
    }
    pmap_unlock(kernel_pmap, PMAP_LOCK_SHARED);
    return valid;
}

/* The slot is still owned, and has never been published or has been drained.
 * Revoke through pmap's PVH/PTE/TLB path, then let kmem_free perform the normal
 * unwire/remove/object teardown. Pre-removing PTEs would break wire accounting. */
static int nlh_memory_revoke(vm_map_t map, unsigned slot,
                              struct nlh_memory_diagnostic *report)
{
    vm_offset_t address = nlh_memory[slot].address;
    vm_size_t size = nlh_memory[slot].size;
    kern_return_t kr = vm_map_protect(map, address, address + size, VM_PROT_READ, FALSE);
    nlh_revoke_request = (struct nlh_memory_request){ address, address + size,
        current_thread(), slot };
    pmap_protect_options(kernel_pmap, address, address + size, VM_PROT_READ,
        0, &nlh_revoke_request);
    bzero(&nlh_revoke_request, sizeof(nlh_revoke_request));
    __asm__ volatile("dsb ish\n\tisb" ::: "memory");
    bool valid = nlh_memory_check(address, size,
        ARM_PTE_AP(AP_RONA) | ARM_PTE_NX | ARM_PTE_PNX, report);
    report->kr = (uint32_t)kr;
    return kr == KERN_SUCCESS && valid ? 0 : EACCES;
}

int nlh_memory_create(unsigned slot, const void *code, unsigned length)
{
    if (!nlh_memory_enabled()) return ENOTSUP;
    if (slot >= NLH_MAX_MODULES * 2 || !code || !length || length > NLH_MAX_CODE) return EINVAL;
    lck_mtx_lock(&nlh_memory_lock);
    if (nlh_memory[slot].address) { lck_mtx_unlock(&nlh_memory_lock); return EBUSY; }
    struct nlh_memory_diagnostic report = { .stage = NLH_MEM_ARENA,
        .slot = slot, .bytes = length, .arena_bytes = nlh_memory_boot_report.arena_bytes };
    vm_map_t map = nlh_memory_code_map();
    int error = 0;
    if (!map) {
        /* Boot has finished before module operations can arrive. Retain its
         * first failed permission/range check when reporting this request. */
        report = nlh_memory_boot_report;
        report.slot = slot;
        report.bytes = length;
        if (!report.kr)
            report.kr = __atomic_load_n(&nlh_memory_arena_kr, __ATOMIC_ACQUIRE);
        error = ENXIO;
        goto done;
    }
    report.stage = NLH_MEM_ALLOCATE;
    nlh_memory_publish(&report);
    vm_size_t size = round_page(length);
    vm_offset_t address = 0;
    /* An owned wired object in the dedicated submap, initially RW/NX.
     * Never use KMA_DATA or a shared kernel_object for executable code. */
    kern_return_t kr = kmem_alloc(map, &address, size, KMA_ZERO, VM_KERN_MEMORY_OSKEXT);
    if (kr != KERN_SUCCESS) { report.kr = (uint32_t)kr; error = ENOMEM; goto done; }
    nlh_memory[slot].address = address;
    nlh_memory[slot].size = size;
    report.stage = NLH_MEM_WRITE_VERIFY;
    nlh_memory_publish(&report);
    if (!nlh_memory_check(address, size,
        ARM_PTE_AP(AP_RWNA) | ARM_PTE_NX | ARM_PTE_PNX, &report)) {
        error = EACCES;
        goto rollback;
    }
    memcpy((void *)address, code, length);
    report.stage = NLH_MEM_PROTECT;
    nlh_memory_publish(&report);
    kr = vm_map_protect(map, address, address + size, VM_PROT_READ | VM_PROT_EXECUTE, FALSE);
    if (kr != KERN_SUCCESS) { report.kr = (uint32_t)kr; error = EACCES; goto rollback; }
    /* Check that VM actually removed write before any execute grant. */
    if (!nlh_memory_check(address, size,
        ARM_PTE_AP(AP_RONA) | ARM_PTE_NX | ARM_PTE_PNX, &report)) {
        error = EACCES;
        goto rollback;
    }
    report.stage = NLH_MEM_CACHE;
    nlh_memory_publish(&report);
    flush_dcache64(address, (unsigned)size, FALSE);
    invalidate_icache64(address, (unsigned)size, FALSE);
    report.stage = NLH_MEM_SEAL;
    nlh_memory_publish(&report);
    nlh_execute_request = (struct nlh_memory_request){ address, address + size,
        current_thread(), slot };
    pmap_protect_options(kernel_pmap, address, address + size,
        VM_PROT_READ | VM_PROT_EXECUTE, 0, &nlh_execute_request);
    bzero(&nlh_execute_request, sizeof(nlh_execute_request));
    __asm__ volatile("dsb ish\n\tisb" ::: "memory");
    report.stage = NLH_MEM_EXECUTE_VERIFY;
    nlh_memory_publish(&report);
    if (!nlh_memory_check(address, size, ARM_PTE_AP(AP_RONA) | ARM_PTE_NX, &report)) {
        error = EACCES;
        goto rollback;
    }
    /* The runtime obtains an entry only after every leaf passes verification. */
    __atomic_store_n(&nlh_memory[slot].sealed, 1u, __ATOMIC_RELEASE);
    report.stage = NLH_MEM_COMPLETE;
    goto done;
rollback:
    {
        struct nlh_memory_diagnostic cleanup = { .stage = NLH_MEM_REVOKE };
        report.error = (uint32_t)error;
        nlh_memory_publish(&report);
        report.rollback_error = (uint32_t)nlh_memory_revoke(map, slot, &cleanup);
        /* Preserve the original failure's stage, kr and permissions. */
        kmem_free(map, address, size);
        bzero(&nlh_memory[slot], sizeof(nlh_memory[slot]));
    }
done:
    report.error = (uint32_t)error;
    nlh_memory_publish(&report);
    lck_mtx_unlock(&nlh_memory_lock);
    return error;
}

void *nlh_memory_address(unsigned slot)
{
    if (slot >= NLH_MAX_MODULES * 2 ||
        !__atomic_load_n(&nlh_memory[slot].sealed, __ATOMIC_ACQUIRE)) return NULL;
    return (void *)nlh_memory[slot].address;
}

void nlh_memory_destroy(unsigned slot)
{
    if (slot >= NLH_MAX_MODULES * 2) return;
    lck_mtx_lock(&nlh_memory_lock);
    if (nlh_memory[slot].address) {
        struct nlh_memory_diagnostic report = { .stage = NLH_MEM_REVOKE,
            .slot = slot, .bytes = (uint32_t)nlh_memory[slot].size,
            .arena_bytes = nlh_memory_boot_report.arena_bytes };
        vm_map_t map = nlh_memory_code_map();
        /* Admission is closed and all invocations have already drained. */
        __atomic_store_n(&nlh_memory[slot].sealed, 0u, __ATOMIC_RELEASE);
        nlh_memory_publish(&report);
        report.error = (uint32_t)nlh_memory_revoke(map, slot, &report);
        if (!report.error) report.stage = NLH_MEM_FREE;
        nlh_memory_publish(&report);
        kmem_free(map, nlh_memory[slot].address, nlh_memory[slot].size);
        bzero(&nlh_memory[slot], sizeof(nlh_memory[slot]));
        if (!report.error) report.stage = NLH_MEM_COMPLETE;
        nlh_memory_publish(&report);
    }
    lck_mtx_unlock(&nlh_memory_lock);
}

void nlh_memory_status(uint32_t nonce)
{
    struct nlh_memory_diagnostic report = { 0 };
    bool snapshot = false;
    for (unsigned attempt = 0; attempt != 8; ++attempt) {
        uint32_t before = __atomic_load_n(&nlh_memory_report_sequence, __ATOMIC_ACQUIRE);
        if (before & 1u) continue;
#define NLH_MEMORY_LOAD(field) \
        report.field = __atomic_load_n(&nlh_memory_report.field, __ATOMIC_RELAXED)
        NLH_MEMORY_LOAD(stage);
        NLH_MEMORY_LOAD(error);
        NLH_MEMORY_LOAD(kr);
        NLH_MEMORY_LOAD(slot);
        NLH_MEMORY_LOAD(bytes);
        NLH_MEMORY_LOAD(checked_pages);
        NLH_MEMORY_LOAD(failed_page);
        NLH_MEMORY_LOAD(rollback_error);
        NLH_MEMORY_LOAD(arena_bytes);
        NLH_MEMORY_LOAD(leaf);
        NLH_MEMORY_LOAD(effective);
        NLH_MEMORY_LOAD(parent_flags);
#undef NLH_MEMORY_LOAD
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (before == __atomic_load_n(&nlh_memory_report_sequence, __ATOMIC_ACQUIRE)) {
            snapshot = true;
            break;
        }
    }
    if (!snapshot) bzero(&report, sizeof(report));
    printf("NLH memory abi=2 nonce=%u snapshot=%u arena=%u arena_kr=%u stage=%u error=%u kr=%u slot=%u bytes=%u arena_bytes=%u checked_pages=%u failed_page=%u leaf=%llu effective=%llu parent_flags=%llu rollback_error=%u\n",
        nonce, (unsigned)snapshot, __atomic_load_n(&nlh_memory_arena_ready, __ATOMIC_ACQUIRE),
        __atomic_load_n(&nlh_memory_arena_kr, __ATOMIC_ACQUIRE), report.stage,
        report.error, report.kr, report.slot, report.bytes, report.arena_bytes, report.checked_pages,
        report.failed_page, report.leaf, report.effective, report.parent_flags,
        report.rollback_error);
}
