/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Included only in osfmk/vm/vm_kern.c before kmem_init().
 * Reserve the pmap-validated empty static VA twig before its surrounding
 * region becomes permanent, then create its submap after early VM maps have
 * been relocated by zone_init. Ordinary kernel heap allocations cannot use
 * this VA. No existing translation-table permissions are changed here.
 */
#include "native_hotload_vm.h"

#if defined(__arm64__)
static SECURITY_READ_ONLY_LATE(vm_map_t) nlh_code_map;
static SECURITY_READ_ONLY_LATE(vm_map_offset_t) nlh_code_arena;
static SECURITY_READ_ONLY_LATE(vm_map_size_t) nlh_code_arena_size;

vm_map_t
nlh_memory_code_map(void)
{
    return nlh_code_map;
}

/* Follow kmem_init's existing fatal reservation policy. Failure here must
 * never leave a hole in the boot-time static VA reservation. Only the middle
 * placeholder is initially replaceable; all surrounding VA stays permanent.
 */
__startup_func
static void
nlh_boot_reserve_none(vm_map_offset_t start, vm_map_size_t size,
                      bool permanent)
{
    if (size == 0) return;
    vm_map_offset_t address = start;
    vm_map_kernel_flags_t flags = VM_MAP_KERNEL_FLAGS_FIXED(
        .vmkf_no_pmap_check = true);
    flags.vmf_permanent = permanent;
    kern_return_t kr = vm_map_enter(kernel_map, &address, size, 0, flags,
        VM_OBJECT_NULL, 0, FALSE, VM_PROT_NONE, VM_PROT_NONE,
        VM_INHERIT_DEFAULT);
    if (kr != KERN_SUCCESS || address != start) {
        panic("kmem_init: native hotload static VA reservation failed (%d)", kr);
    }
}

__startup_func
__attribute__((noinline))
bool
nlh_memory_boot_reserve(unsigned region, vm_map_offset_t start,
                        vm_map_size_t size)
{
    if (region != 0) return false;

    vm_map_offset_t arena = 0, arena_end, region_end;
    vm_map_size_t arena_size = 0;
    kern_return_t kr = nlh_memory_boot_range(&arena, &arena_size);
    if (kr != KERN_SUCCESS) {
        nlh_memory_boot_result(kr);
        return false;
    }

    const vm_map_offset_t page_mask = VM_MAP_PAGE_MASK(kernel_map);
    vm_map_size_t rounded_size;
    /* Validate the complete split before changing any VM entry. Preserve the
     * original kmem_init rounding for the containing static reservation.
     */
    if (size == 0 || arena_size == 0 ||
        ((start | arena | arena_size) & page_mask) != 0 ||
        os_add_overflow(size, page_mask, &rounded_size) ||
        os_add_overflow(start, rounded_size & ~page_mask, &region_end) ||
        os_add_overflow(arena, arena_size, &arena_end) ||
        arena < start || arena_end > region_end) {
        nlh_memory_boot_result(KERN_INVALID_ADDRESS);
        return false;
    }

    nlh_boot_reserve_none(start, arena - start, true);
    nlh_boot_reserve_none(arena, arena_size, false);
    nlh_boot_reserve_none(arena_end, region_end - arena_end, true);

    /* Save numeric VA bounds only. Creating an early map would require a
     * relocation owner, which cannot survive optional submap failure with
     * a NULL map. The closed placeholder remains until the late initializer.
     */
    nlh_code_arena = arena;
    nlh_code_arena_size = arena_size;
    return true;
}

/* vm_map_enter restores its saved placeholder after an overwrite failure.
 * Confirm that exact ownership under the map's exclusive lock before making
 * it permanent, as upstream does for immutable entries. No VA, object, map
 * accounting or pmap entry changes. A missing or changed placeholder means
 * the static reservation itself is no longer provably intact.
 *
 * Do not rebuild this entry with a late fixed vm_map_enter: its ordinary
 * kernel_map bounds check rejects static VA even without OVERWRITE.
 */
__startup_func
static void
nlh_boot_close_placeholder(void)
{
    vm_map_entry_t entry;
    vm_map_lock(kernel_map);
    bool owned = vm_map_lookup_entry(kernel_map, nlh_code_arena, &entry) &&
        entry->vme_start == nlh_code_arena &&
        entry->vme_end == nlh_code_arena + nlh_code_arena_size &&
        !entry->is_sub_map && VME_OBJECT(entry) == VM_OBJECT_NULL &&
        entry->protection == VM_PROT_NONE &&
        entry->max_protection == VM_PROT_NONE &&
        !entry->wired_count && !entry->user_wired_count &&
        !entry->vme_atomic && !entry->in_transition && !entry->needs_wakeup &&
        !entry->is_shared && !entry->needs_copy;
    if (!owned) {
        vm_map_unlock(kernel_map);
        panic("native hotload: failed submap lost its static VA placeholder");
    }
    entry->vme_permanent = true;
    vm_map_unlock(kernel_map);
}

/* zone_init (ZALLOC/FIRST) relocates early maps, vm_kernel_boostraped
 * (SECOND) enables VM entry caches, and kalloc_init runs THIRD. At LAST,
 * normal VM zones can create and release maps without early-map ownership.
 * SECURITY_READ_ONLY_LATE data is still writable before machine lockdown.
 */
__startup_func
static void
nlh_memory_map_init(void)
{
    if (nlh_code_arena_size == 0) return;

    vm_map_offset_t address = nlh_code_arena;
    kmem_return_t result = kmem_suballoc(kernel_map, &address,
        nlh_code_arena_size,
        VM_MAP_CREATE_DEFAULT, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE,
        KMS_PERMANENT, VM_KERN_MEMORY_OSKEXT);
    if (result.kmr_return != KERN_SUCCESS) {
        nlh_boot_close_placeholder();
        nlh_memory_boot_result(result.kmr_return);
        return;
    }
    if (address != nlh_code_arena || result.kmr_submap == VM_MAP_NULL) {
        panic("kmem_init: native hotload submap has invalid fixed bounds");
    }

    nlh_code_map = result.kmr_submap;
    nlh_memory_boot_result(KERN_SUCCESS);
}
STARTUP(ZALLOC, STARTUP_RANK_LAST, nlh_memory_map_init);
#endif /* defined(__arm64__) */
