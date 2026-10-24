"""Pinned xnu-8796.141.3 VM reservation hook for native hotload code.

The kit's main emitter copies native_hotload_vm.h and native_hotload_boot.inc
into osfmk/vm, then appends emit_vm_patch(). Exact upstream source anchors and
original line positions keep this opt-in adapter closed on other revisions.
This emitter neither reads nor mutates an XNU checkout.

kmem_init reserves numeric arena bounds and a closed placeholder. The included
boot adapter registers its map initializer at ZALLOC/LAST, after early maps
have been relocated and normal VM zones support failure cleanup.

The excerpts retain the upstream source's original notices when applied.
Copyright (c) Apple Inc. All rights reserved; see core/LICENSE for APSL 2.0.
"""

import difflib
import re


def _changed(old, new, line):
    patch = ''.join(difflib.unified_diff(
        old.splitlines(True), new.splitlines(True),
        fromfile='a/osfmk/vm/vm_kern.c', tofile='b/osfmk/vm/vm_kern.c'))
    return re.sub(
        r'^@@ -(\d+)(,\d+)? \+(\d+)(,\d+)? @@',
        lambda m: (f'@@ -{int(m[1]) + line - 1}{m[2] or ""} '
                   f'+{int(m[3]) + line - 1}{m[4] or ""} @@'),
        patch, flags=re.M)


_INIT_OLD = '''/*
 *\tkmem_init:
 *
 *\tInitialize the kernel's virtual memory map, taking
 *\tinto account all memory allocated up to this time.
 */
__startup_func
void
kmem_init(
'''

_INIT_NEW = _INIT_OLD.replace(
    '__startup_func\n',
    '#include "native_hotload_boot.inc"\n\n__startup_func\n', 1)

_RESERVE_OLD = '''\t\twhile (pmap_virtual_region(region_select, &region_start, &region_size)) {
\t\t\tmap_addr = region_start;
\t\t\tkr = vm_map_enter(kernel_map, &map_addr,
\t\t\t    vm_map_round_page(region_size,
\t\t\t    VM_MAP_PAGE_MASK(kernel_map)),
\t\t\t    (vm_map_offset_t) 0,
\t\t\t    VM_MAP_KERNEL_FLAGS_FIXED_PERMANENT(.vmkf_no_pmap_check = true),
\t\t\t    VM_OBJECT_NULL,
\t\t\t    (vm_object_offset_t) 0, FALSE, VM_PROT_NONE, VM_PROT_NONE,
\t\t\t    VM_INHERIT_DEFAULT);
'''

_RESERVE_NEW = _RESERVE_OLD.replace(
    '\t\t\tmap_addr = region_start;\n',
    '''\t\t\tif (nlh_memory_boot_reserve(region_select, region_start, region_size)) {
\t\t\t\tregion_select++;
\t\t\t\tcontinue;
\t\t\t}
\t\t\tmap_addr = region_start;
''', 1)


def emit_vm_patch():
    """Return the VM hooks against the pinned original source positions."""
    return (_changed(_INIT_OLD, _INIT_NEW, 3759) +
            _changed(_RESERVE_OLD, _RESERVE_NEW, 3795))
