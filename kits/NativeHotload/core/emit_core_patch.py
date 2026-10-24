"""Pinned xnu-8796.141.3 resident adapters for native core policies.

This emitter contains exact source anchors and original source positions. It
does not inspect or mutate a checkout. The normal kit emitter adds the shared
headers/runtime and appends emit_core_patch() to its opt-in patch.

The existing XNU calculation remains the fallback in every adapter. Only a
pointer-free decision crosses the gateway: thread accounting/timers, VM object
locking/CAS/last_alloc, and vnode/mount/UPL ownership remain with XNU.

Source excerpts below are from xnu-8796.141.3. Copyright (c) 2000-2021 Apple
Inc. All rights reserved. The upstream notices and APSL 2.0 are retained in
core/LICENSE and in every patched XNU source file; see core/UPSTREAM.md.
"""

import difflib
import re


def _changed(target, old, new, line):
    """Generate a context diff relative to a recorded pinned-source position."""
    patch = ''.join(difflib.unified_diff(
        old.splitlines(True), new.splitlines(True),
        fromfile='a/' + target, tofile='b/' + target))
    return re.sub(
        r'^@@ -(\d+)(,\d+)? \+(\d+)(,\d+)? @@',
        lambda m: (f'@@ -{int(m[1]) + line - 1}{m[2] or ""} '
                   f'+{int(m[3]) + line - 1}{m[4] or ""} @@'),
        patch, flags=re.M)


def _replace_once(text, old, new):
    if text.count(old) != 1:
        raise ValueError('Pinned core adapter anchor is not unique')
    return text.replace(old, new, 1)


_SCHED_OLD = '''void
thread_quantum_init(thread_t thread, uint64_t now)
{
\tuint64_t new_quantum = 0;

\tswitch (thread->sched_mode) {
\tcase TH_MODE_REALTIME:
\t\tnew_quantum = thread->realtime.computation;
\t\tnew_quantum = MIN(new_quantum, max_unsafe_rt_computation);
\t\tbreak;

\tcase TH_MODE_FIXED:
\t\tnew_quantum = SCHED(initial_quantum_size)(thread);
\t\tnew_quantum = MIN(new_quantum, max_unsafe_fixed_computation);
\t\tbreak;

\tdefault:
\t\tnew_quantum = SCHED(initial_quantum_size)(thread);
\t\tbreak;
\t}

\tif (cpulimit_affects_quantum) {
\t\tconst uint64_t cpulimit_remaining = thread_cpulimit_remaining(now);

\t\t/*
\t\t * If there's no remaining CPU time, the ledger system will
\t\t * notice and put the thread to sleep.
\t\t */
\t\tif (cpulimit_remaining > 0) {
\t\t\tnew_quantum = MIN(new_quantum, cpulimit_remaining);
\t\t}
\t}

\tassert3u(new_quantum, <, UINT32_MAX);
\tassert3u(new_quantum, >, 0);

\tthread->quantum_remaining = (uint32_t)new_quantum;
}
'''


def _sched_new():
    text = _replace_once(_SCHED_OLD, '\tuint64_t new_quantum = 0;\n', '''\tuint64_t new_quantum = 0;
\tstruct nlh_core_request request = {
\t\t.abi = NLH_CORE_INTERFACE,
\t\t.bytes = sizeof(request),
\t\t.domain = NLH_CORE_SCHED,
\t\t.operation = NLH_SCHED_QUANTUM,
\t\t.value.sched.mode = NLH_MODE_TIMESHARE,
\t};
''')
    text = _replace_once(text, '''\t\tnew_quantum = thread->realtime.computation;
\t\tnew_quantum = MIN(new_quantum, max_unsafe_rt_computation);
''', '''\t\tnew_quantum = thread->realtime.computation;
\t\trequest.value.sched.initial = new_quantum;
\t\trequest.value.sched.mode = NLH_MODE_REALTIME;
\t\trequest.value.sched.safety_limit = max_unsafe_rt_computation;
\t\tnew_quantum = MIN(new_quantum, request.value.sched.safety_limit);
''')
    text = _replace_once(text, '''\t\tnew_quantum = SCHED(initial_quantum_size)(thread);
\t\tnew_quantum = MIN(new_quantum, max_unsafe_fixed_computation);
''', '''\t\tnew_quantum = SCHED(initial_quantum_size)(thread);
\t\trequest.value.sched.initial = new_quantum;
\t\trequest.value.sched.mode = NLH_MODE_FIXED;
\t\trequest.value.sched.safety_limit = max_unsafe_fixed_computation;
\t\tnew_quantum = MIN(new_quantum, request.value.sched.safety_limit);
''')
    text = _replace_once(text, '''\tdefault:
\t\tnew_quantum = SCHED(initial_quantum_size)(thread);
\t\tbreak;
''', '''\tdefault:
\t\tnew_quantum = SCHED(initial_quantum_size)(thread);
\t\trequest.value.sched.initial = new_quantum;
\t\tbreak;
''')
    text = _replace_once(text,
        '\t\tconst uint64_t cpulimit_remaining = thread_cpulimit_remaining(now);\n',
        '\t\tconst uint64_t cpulimit_remaining = thread_cpulimit_remaining(now);\n'
        '\t\trequest.value.sched.cpu_remaining = cpulimit_remaining;\n')
    return _replace_once(text, '\tassert3u(new_quantum, <, UINT32_MAX);\n', '''\t/* The gateway cannot wait, acquire a lock, or transfer thread ownership. */
\tnew_quantum = nlh_core_select(&request, new_quantum);

\tassert3u(new_quantum, <, UINT32_MAX);
''')


# The full original switch precedes this anchor. Same-page re-faults retain
# their original early return. We only select a numeric transition before the
# original compare-and-swap; a lost CAS still skips the last_alloc commit.
_VM_OLD = '''\t\tbreak;
\t}
\tif (sequential != orig_sequential) {
\t\tif (!OSCompareAndSwap(orig_sequential, sequential, (UInt32 *)&object->sequential)) {
'''

_VM_NEW = '''\t\tbreak;
\t}
\t/* Object state and its shared/exclusive lock never leave this owner. */
\tconst struct nlh_core_request request = {
\t\t.abi = NLH_CORE_INTERFACE,
\t\t.bytes = sizeof(request),
\t\t.domain = NLH_CORE_VM,
\t\t.operation = NLH_VM_SEQUENTIAL,
\t\t.value.vm = {
\t\t\t.last_alloc = last_alloc,
\t\t\t.offset = offset,
\t\t\t.sequential = orig_sequential,
\t\t\t.behavior = behavior == VM_BEHAVIOR_RANDOM ? NLH_BEHAVIOR_RANDOM :
\t\t\t    behavior == VM_BEHAVIOR_SEQUENTIAL ? NLH_BEHAVIOR_FORWARD :
\t\t\t    behavior == VM_BEHAVIOR_RSEQNTL ? NLH_BEHAVIOR_REVERSE :
\t\t\t    NLH_BEHAVIOR_DEFAULT,
\t\t\t.page_bytes = PAGE_SIZE,
\t\t\t.max_run = MAX_SEQUENTIAL_RUN,
\t\t},
\t};
\tsequential = (int32_t)(uint32_t)nlh_core_select(&request,
\t    NLH_VM_UPDATE | (uint32_t)sequential);
\tif (sequential != orig_sequential) {
\t\tif (!OSCompareAndSwap(orig_sequential, sequential, (UInt32 *)&object->sequential)) {
'''


_VFS_OLD = '''uint32_t
cluster_max_io_size(mount_t mp, int type)
{
\tuint32_t        max_io_size;
\tuint32_t        segcnt;
\tuint32_t        maxcnt;

\tswitch (type) {
\tcase CL_READ:
\t\tsegcnt = mp->mnt_segreadcnt;
\t\tmaxcnt = mp->mnt_maxreadcnt;
\t\tbreak;
\tcase CL_WRITE:
\t\tsegcnt = mp->mnt_segwritecnt;
\t\tmaxcnt = mp->mnt_maxwritecnt;
\t\tbreak;
\tdefault:
\t\tsegcnt = min(mp->mnt_segreadcnt, mp->mnt_segwritecnt);
\t\tmaxcnt = min(mp->mnt_maxreadcnt, mp->mnt_maxwritecnt);
\t\tbreak;
\t}
\tif (segcnt > (MAX_UPL_SIZE_BYTES >> PAGE_SHIFT)) {
\t\t/*
\t\t * don't allow a size beyond the max UPL size we can create
\t\t */
\t\tsegcnt = MAX_UPL_SIZE_BYTES >> PAGE_SHIFT;
\t}
\tmax_io_size = min((segcnt * PAGE_SIZE), maxcnt);

\tif (max_io_size < MAX_UPL_TRANSFER_BYTES) {
\t\t/*
\t\t * don't allow a size smaller than the old fixed limit
\t\t */
\t\tmax_io_size = MAX_UPL_TRANSFER_BYTES;
\t} else {
\t\t/*
\t\t * make sure the size specified is a multiple of PAGE_SIZE
\t\t */
\t\tmax_io_size &= ~PAGE_MASK;
\t}
\treturn max_io_size;
}

/*
 * Returns max prefetch value. If the value overflows or exceeds the specified
 * 'prefetch_limit', it will be capped at 'prefetch_limit' value.
 */
static inline uint32_t
cluster_max_prefetch(vnode_t vp, uint32_t max_io_size, uint32_t prefetch_limit)
{
\tbool is_ssd = disk_conditioner_mount_is_ssd(vp->v_mount);
\tuint32_t io_scale = IO_SCALE(vp, is_ssd ? PREFETCH_SSD : PREFETCH);
\tuint32_t prefetch = 0;

\tif (__improbable(os_mul_overflow(max_io_size, io_scale, &prefetch) ||
\t    (prefetch > prefetch_limit))) {
\t\tprefetch = prefetch_limit;
\t}

\treturn prefetch;
}
'''


def _vfs_new():
    text = _replace_once(_VFS_OLD, '\tuint32_t        maxcnt;\n', '''\tuint32_t        maxcnt;
\tstruct nlh_core_request request = {
\t\t.abi = NLH_CORE_INTERFACE,
\t\t.bytes = sizeof(request),
\t\t.domain = NLH_CORE_VFS,
\t\t.operation = NLH_VFS_MAX_IO,
\t\t.value.vfs = {
\t\t\t.page_bytes = PAGE_SIZE,
\t\t\t.max_upl_bytes = MAX_UPL_SIZE_BYTES,
\t\t\t.minimum_io = MAX_UPL_TRANSFER_BYTES,
\t\t\t.type = NLH_IO_BOTH,
\t\t},
\t};
''')
    text = _replace_once(text, '''\t\tsegcnt = mp->mnt_segreadcnt;
\t\tmaxcnt = mp->mnt_maxreadcnt;
''', '''\t\tsegcnt = mp->mnt_segreadcnt;
\t\tmaxcnt = mp->mnt_maxreadcnt;
\t\trequest.value.vfs.read_segments = segcnt;
\t\trequest.value.vfs.read_bytes = maxcnt;
\t\trequest.value.vfs.type = NLH_IO_READ;
''')
    text = _replace_once(text, '''\t\tsegcnt = mp->mnt_segwritecnt;
\t\tmaxcnt = mp->mnt_maxwritecnt;
''', '''\t\tsegcnt = mp->mnt_segwritecnt;
\t\tmaxcnt = mp->mnt_maxwritecnt;
\t\trequest.value.vfs.write_segments = segcnt;
\t\trequest.value.vfs.write_bytes = maxcnt;
\t\trequest.value.vfs.type = NLH_IO_WRITE;
''')
    text = _replace_once(text, '''\t\tsegcnt = min(mp->mnt_segreadcnt, mp->mnt_segwritecnt);
\t\tmaxcnt = min(mp->mnt_maxreadcnt, mp->mnt_maxwritecnt);
''', '''\t\trequest.value.vfs.read_segments = mp->mnt_segreadcnt;
\t\trequest.value.vfs.write_segments = mp->mnt_segwritecnt;
\t\tsegcnt = min(request.value.vfs.read_segments, request.value.vfs.write_segments);
\t\trequest.value.vfs.read_bytes = mp->mnt_maxreadcnt;
\t\trequest.value.vfs.write_bytes = mp->mnt_maxwritecnt;
\t\tmaxcnt = min(request.value.vfs.read_bytes, request.value.vfs.write_bytes);
''')
    text = _replace_once(text, '\treturn max_io_size;\n',
        '\treturn (uint32_t)nlh_core_select(&request, max_io_size);\n')
    return _replace_once(text, '\treturn prefetch;\n', '''\t/* Capability detection and IO_SCALE retain their original resident order. */
\tconst struct nlh_core_request request = {
\t\t.abi = NLH_CORE_INTERFACE,
\t\t.bytes = sizeof(request),
\t\t.domain = NLH_CORE_VFS,
\t\t.operation = NLH_VFS_PREFETCH,
\t\t.value.vfs = {
\t\t\t.page_bytes = PAGE_SIZE,
\t\t\t.max_upl_bytes = MAX_UPL_SIZE_BYTES,
\t\t\t.minimum_io = MAX_UPL_TRANSFER_BYTES,
\t\t\t.type = NLH_IO_READ,
\t\t\t.max_io = max_io_size,
\t\t\t.scale = io_scale,
\t\t\t.prefetch_limit = prefetch_limit,
\t\t},
\t};
\treturn (uint32_t)nlh_core_select(&request, prefetch);
''')


def emit_core_patch():
    """Return resident core call-site changes as a unified diff.

    Source-relative includes share bsd/dev/native_hotload_core.h with the
    runtime. osfmk imports exported BSD headers, so a new dev/ header would
    otherwise require an additional Makefile export. The outer emitter adds
    that header and its adjacent native_hotload.h; no source lookup is needed.
    """
    patch = ''
    for target, line, anchor, header in (
        ('osfmk/kern/sched_prim.c', 129,
         '#include <os/atomic_private.h>\n\n#ifdef KDBG_MACOS_RELEASE\n',
         '../../bsd/dev/native_hotload_core.h'),
        ('osfmk/vm/vm_fault.c', 112,
         '#include <libkern/coreanalytics/coreanalytics.h>\n\n'
         '#define VM_FAULT_CLASSIFY       0\n',
         '../../bsd/dev/native_hotload_core.h'),
        ('bsd/vfs/vfs_cluster.c', 102,
         '#include <vfs/vfs_disk_conditioner.h>\n\n#if 0\n',
         '../dev/native_hotload_core.h'),
    ):
        first, rest = anchor.split('\n', 1)
        patch += _changed(target, anchor, first + '\n'
            f'#include "{header}"\n' + rest, line)
    patch += _changed('osfmk/kern/sched_prim.c', _SCHED_OLD, _sched_new(), 3968)
    patch += _changed('osfmk/vm/vm_fault.c', _VM_OLD, _VM_NEW, 486)
    patch += _changed('bsd/vfs/vfs_cluster.c', _VFS_OLD, _vfs_new(), 330)
    return patch
