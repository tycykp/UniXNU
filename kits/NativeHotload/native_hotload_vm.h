/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Private contract between the pinned XNU VM and pmap implementations.
 * This header is not part of the module or diagnostic transport ABI.
 */
#ifndef NATIVE_HOTLOAD_VM_H
#define NATIVE_HOTLOAD_VM_H

#include <vm/vm_map.h>

kern_return_t nlh_memory_boot_range(vm_map_offset_t *, vm_map_size_t *);
void nlh_memory_boot_result(kern_return_t);
/* kmem_init reserves VA; the ZALLOC/LAST initializer publishes the map. */
bool nlh_memory_boot_reserve(unsigned, vm_map_offset_t, vm_map_size_t);
vm_map_t nlh_memory_code_map(void);

#endif /* NATIVE_HOTLOAD_VM_H */
