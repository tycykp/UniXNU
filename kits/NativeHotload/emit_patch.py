"""Emit an opt-in Darwin 22 patch; never change an existing source tree.

The pmap anchor is deliberately exact. Unsupported source revisions fail closed.
The device composition root includes this patch only when explicitly selected.
"""
import difflib
import re
import runpy
from pathlib import Path

ROOT = Path(__file__).resolve().parent


def added(target, text):
    return ''.join(difflib.unified_diff([], text.splitlines(True),
                                      fromfile='/dev/null', tofile='b/' + target))


def changed(target, old, new, line):
    patch = ''.join(difflib.unified_diff(old.splitlines(True), new.splitlines(True),
                                       fromfile='a/' + target, tofile='b/' + target))
    # Real pinned-source positions avoid accidental start/end-of-file anchors.
    # Existing device patches may shift these positions; context remains exact.
    return re.sub(r'^@@ -(\d+)(,\d+)? \+(\d+)(,\d+)? @@',
                  lambda m: f'@@ -{int(m[1])+line-1}{m[2] or ""} +{int(m[3])+line-1}{m[4] or ""} @@',
                  patch, flags=re.M)


def emit(out):
    out = Path(out)
    patch = ''
    for name in ('native_hotload.h', 'native_hotload.c'):
        patch += added('bsd/dev/' + name, (ROOT/name).read_text(encoding='utf-8'))
    patch += added('bsd/dev/native_hotload_core.h',
                   (ROOT/'native_hotload_core.h').read_text(encoding='utf-8'))
    patch += added('bsd/dev/native_core_algorithms.h',
                   (ROOT/'core/native_core_algorithms.h').read_text(encoding='utf-8'))
    patch += added('osfmk/arm/pmap/native_hotload.h',
                   (ROOT/'native_hotload.h').read_text(encoding='utf-8'))
    patch += added('osfmk/arm/pmap/native_hotload_memory.inc',
                   (ROOT/'native_hotload_memory.inc').read_text(encoding='utf-8'))
    for name in ('native_hotload_vm.h', 'native_hotload_boot.inc'):
        patch += added('osfmk/vm/' + name, (ROOT/name).read_text(encoding='utf-8'))
    registration = ('OPTIONS/config_macf\t\t\toptional config_macf\n'
                    'OPTIONS/mach_assert\t\t\toptional mach_assert\n'
                    'OPTIONS/mach_ldebug\t\t\toptional mach_ldebug\n'
                    'OPTIONS/mach_vm_debug\t\t\toptional mach_vm_debug\n')
    first, rest = registration.split('\n', 1)
    patch += changed('bsd/conf/files', registration,
                     first + '\nbsd/dev/native_hotload.c standard\n' + rest, 8)
    anchor = ('void\npmap_protect(\n\tpmap_t pmap,\n')
    patch += changed('osfmk/arm/pmap/pmap.c', anchor,
                     '#include "native_hotload_memory.inc"\n\n' + anchor, 5141)
    old = ('\t\t\tif (set_NX) {\n'
           '\t\t\t\ttmplate |= pt_attr_leaf_xn(pt_attr);\n'
           '\t\t\t} else {\n'
           '\t\t\t\tif (pmap == kernel_pmap) {\n'
           '\t\t\t\t\t/* do NOT clear "PNX"! */\n'
           '\t\t\t\t\ttmplate |= ARM_PTE_NX;\n')
    new = ('\t\t\tif (set_NX || nlh_revoke_allowed(pmap, start, end, prot, args)) {\n'
           '\t\t\t\ttmplate |= pt_attr_leaf_xn(pt_attr);\n'
           '\t\t\t} else {\n'
           '\t\t\t\tif (pmap == kernel_pmap) {\n'
           '\t\t\t\t\t/* Only the opt-in loader may seal its owned code. */\n'
           '\t\t\t\t\tif (nlh_execute_allowed(pmap, start, end, prot, args))\n'
           '\t\t\t\t\t\ttmplate &= ~ARM_PTE_PNX;\n'
           '\t\t\t\t\ttmplate |= ARM_PTE_NX;\n')
    # Separate file sections allow applying these exact anchors with offsets.
    change = changed('osfmk/arm/pmap/pmap.c', old, new, 5319)
    patch += change
    patch += runpy.run_path(str(ROOT/'core/emit_core_patch.py'))['emit_core_patch']()
    patch += runpy.run_path(str(ROOT/'core/emit_vm_patch.py'))['emit_vm_patch']()
    target = out / '0092-native-hotload.patch'
    if target.exists(): raise ValueError('Hotload patch already exists')
    target.write_text(patch, encoding='utf-8', newline='\n')
    return target
