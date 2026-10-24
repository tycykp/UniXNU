/* Modified 2026-10-24 for UniXNU NativeHotload.
 * Arithmetic/state decisions adapted from xnu-8796.141.3. See UPSTREAM.md.
 * The following original source notices apply to their respective adaptations.
 */
/* Original source notice: osfmk/kern/sched_prim.c */
/*
 * Copyright (c) 2000-2016 Apple Inc. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. The rights granted to you under the License
 * may not be used to create, or enable the creation or redistribution of,
 * unlawful or unlicensed copies of an Apple operating system, or to
 * circumvent, violate, or enable the circumvention or violation of, any
 * terms of an Apple operating system software license agreement.
 *
 * Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
/*
 * @OSF_FREE_COPYRIGHT@
 */
/*
 * Mach Operating System
 * Copyright (c) 1991,1990,1989,1988,1987 Carnegie Mellon University
 * All Rights Reserved.
 *
 * Permission to use, copy, modify and distribute this software and its
 * documentation is hereby granted, provided that both the copyright
 * notice and this permission notice appear in all copies of the
 * software, derivative works or modified versions, and any portions
 * thereof, and that both notices appear in supporting documentation.
 *
 * CARNEGIE MELLON ALLOWS FREE USE OF THIS SOFTWARE IN ITS "AS IS"
 * CONDITION.  CARNEGIE MELLON DISCLAIMS ANY LIABILITY OF ANY KIND FOR
 * ANY DAMAGES WHATSOEVER RESULTING FROM THE USE OF THIS SOFTWARE.
 *
 * Carnegie Mellon requests users of this software to return to
 *
 *  Software Distribution Coordinator  or  Software.Distribution@CS.CMU.EDU
 *  School of Computer Science
 *  Carnegie Mellon University
 *  Pittsburgh PA 15213-3890
 *
 * any improvements or extensions that they make and grant Carnegie Mellon
 * the rights to redistribute these changes.
 */
/*
 */
/*
 *	File:	sched_prim.c
 *	Author:	Avadis Tevanian, Jr.
 *	Date:	1986
 *
 *	Scheduling primitives
 *
 */

/* Original source notice: osfmk/vm/vm_fault.c */
/*
 * Copyright (c) 2000-2021 Apple Inc. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. The rights granted to you under the License
 * may not be used to create, or enable the creation or redistribution of,
 * unlawful or unlicensed copies of an Apple operating system, or to
 * circumvent, violate, or enable the circumvention or violation of, any
 * terms of an Apple operating system software license agreement.
 *
 * Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
/*
 * @OSF_COPYRIGHT@
 */
/*
 * Mach Operating System
 * Copyright (c) 1991,1990,1989,1988,1987 Carnegie Mellon University
 * All Rights Reserved.
 *
 * Permission to use, copy, modify and distribute this software and its
 * documentation is hereby granted, provided that both the copyright
 * notice and this permission notice appear in all copies of the
 * software, derivative works or modified versions, and any portions
 * thereof, and that both notices appear in supporting documentation.
 *
 * CARNEGIE MELLON ALLOWS FREE USE OF THIS SOFTWARE IN ITS "AS IS"
 * CONDITION.  CARNEGIE MELLON DISCLAIMS ANY LIABILITY OF ANY KIND FOR
 * ANY DAMAGES WHATSOEVER RESULTING FROM THE USE OF THIS SOFTWARE.
 *
 * Carnegie Mellon requests users of this software to return to
 *
 *  Software Distribution Coordinator  or  Software.Distribution@CS.CMU.EDU
 *  School of Computer Science
 *  Carnegie Mellon University
 *  Pittsburgh PA 15213-3890
 *
 * any improvements or extensions that they make and grant Carnegie Mellon
 * the rights to redistribute these changes.
 */
/*
 */
/*
 *	File:	vm_fault.c
 *	Author:	Avadis Tevanian, Jr., Michael Wayne Young
 *
 *	Page fault handling module.
 */

/* Original source notice: bsd/vfs/vfs_cluster.c */
/*
 * Copyright (c) 2000-2020 Apple Inc. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. The rights granted to you under the License
 * may not be used to create, or enable the creation or redistribution of,
 * unlawful or unlicensed copies of an Apple operating system, or to
 * circumvent, violate, or enable the circumvention or violation of, any
 * terms of an Apple operating system software license agreement.
 *
 * Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
/* Copyright (c) 1995 NeXT Computer, Inc. All Rights Reserved */
/*
 * Copyright (c) 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)vfs_cluster.c	8.10 (Berkeley) 3/28/95
 */

#ifndef NATIVE_CORE_ALGORITHMS_H
#define NATIVE_CORE_ALGORITHMS_H
#include "native_hotload_core.h"

static inline uint64_t nlh_core_min(uint64_t a, uint64_t b) { return a < b ? a : b; }

/* Complete quantum decision after resident SCHED()/ledger resource reads.
 * A zero remaining CPU budget is handled by the resident ledger, as upstream. */
static inline uint64_t nlh_sched_baseline(const struct nlh_core_request *r)
{
    uint64_t quantum = r->value.sched.initial;
    if (r->value.sched.mode != NLH_MODE_TIMESHARE)
        quantum = nlh_core_min(quantum, r->value.sched.safety_limit);
    if (r->value.sched.cpu_remaining)
        quantum = nlh_core_min(quantum, r->value.sched.cpu_remaining);
    return quantum;
}

/* Complete sequential-access transition. The resident owner performs the
 * original compare-and-swap and last_alloc commit under its existing lock. */
static inline uint64_t nlh_vm_baseline(const struct nlh_core_request *r)
{
    uint64_t page = r->value.vm.page_bytes;
    uint64_t offset = r->value.vm.offset & ~(page - 1);
    uint64_t last = r->value.vm.last_alloc;
    int32_t sequential = r->value.vm.sequential, limit = (int32_t)r->value.vm.max_run;
    unsigned behavior = r->value.vm.behavior;
    if (offset == last && behavior != NLH_BEHAVIOR_RANDOM)
        return (uint32_t)sequential;
    switch (behavior) {
    case NLH_BEHAVIOR_RANDOM: sequential = 0; break;
    case NLH_BEHAVIOR_FORWARD:
        if (offset && last == offset - page) {
            if (sequential < limit) sequential += (int32_t)page;
        } else sequential = 0;
        break;
    case NLH_BEHAVIOR_REVERSE:
        if (last && last == offset + page) {
            if (sequential > -limit) sequential -= (int32_t)page;
        } else sequential = 0;
        break;
    default:
        if (offset && last == offset - page) {
            if (sequential < 0) sequential = 0;
            if (sequential < limit) sequential += (int32_t)page;
        } else if (last && last == offset + page) {
            if (sequential > 0) sequential = 0;
            if (sequential > -limit) sequential -= (int32_t)page;
        } else sequential = 0;
        break;
    }
    return NLH_VM_UPDATE | (uint32_t)sequential;
}

/* Complete cluster_max_io_size()/cluster_max_prefetch() arithmetic; disk
 * capability reads and IO_SCALE() remain with the resident vnode owner. */
static inline uint64_t nlh_vfs_baseline(const struct nlh_core_request *r)
{
    if (r->operation == NLH_VFS_PREFETCH) {
        uint64_t bytes = (uint64_t)r->value.vfs.max_io * r->value.vfs.scale;
        /* Upstream's os_mul_overflow() stores a uint32_t and caps on either
         * overflow or exceeding the limit. Widen only for overflow detection. */
        return bytes > UINT32_MAX || bytes > r->value.vfs.prefetch_limit ?
            r->value.vfs.prefetch_limit : (uint32_t)bytes;
    }
    uint64_t segments, maximum;
    if (r->value.vfs.type == NLH_IO_READ) {
        segments = r->value.vfs.read_segments; maximum = r->value.vfs.read_bytes;
    } else if (r->value.vfs.type == NLH_IO_WRITE) {
        segments = r->value.vfs.write_segments; maximum = r->value.vfs.write_bytes;
    } else {
        segments = nlh_core_min(r->value.vfs.read_segments, r->value.vfs.write_segments);
        maximum = nlh_core_min(r->value.vfs.read_bytes, r->value.vfs.write_bytes);
    }
    segments = nlh_core_min(segments, r->value.vfs.max_upl_bytes / r->value.vfs.page_bytes);
    uint64_t bytes = nlh_core_min(segments * r->value.vfs.page_bytes, maximum);
    if (bytes < r->value.vfs.minimum_io) return r->value.vfs.minimum_io;
    return bytes & ~((uint64_t)r->value.vfs.page_bytes - 1);
}

static inline uint64_t nlh_core_baseline(const struct nlh_core_request *r)
{
    if (r->domain == NLH_CORE_SCHED) return nlh_sched_baseline(r);
    if (r->domain == NLH_CORE_VM) return nlh_vm_baseline(r);
    return nlh_vfs_baseline(r);
}

static inline int nlh_core_request_valid(const struct nlh_core_request *r)
{
    if (!r || r->abi != NLH_CORE_INTERFACE || r->bytes != sizeof(*r)) return 0;
    if (r->domain == NLH_CORE_SCHED)
        return r->operation == NLH_SCHED_QUANTUM && r->value.sched.mode <= NLH_MODE_FIXED &&
            !r->value.sched.reserved && r->value.sched.initial > 0 &&
            nlh_sched_baseline(r) > 0 && nlh_sched_baseline(r) < UINT32_MAX;
    if (r->domain == NLH_CORE_VM) {
        uint32_t page = r->value.vm.page_bytes;
        return r->operation == NLH_VM_SEQUENTIAL && (page == 4096 || page == 16384) &&
            r->value.vm.max_run == 1024u * 1024u * 1024u &&
            r->value.vm.sequential >= -(int32_t)r->value.vm.max_run &&
            r->value.vm.sequential <= (int32_t)r->value.vm.max_run &&
            !((uint32_t)r->value.vm.sequential & (page - 1));
    }
    if (r->domain == NLH_CORE_VFS) {
        uint32_t page = r->value.vfs.page_bytes;
        return (r->operation == NLH_VFS_MAX_IO || r->operation == NLH_VFS_PREFETCH) &&
            (page == 4096 || page == 16384) && !r->value.vfs.reserved &&
            r->value.vfs.max_upl_bytes >= r->value.vfs.minimum_io &&
            r->value.vfs.minimum_io >= page &&
            !(r->value.vfs.minimum_io & (page - 1)) &&
            r->value.vfs.type <= NLH_IO_WRITE;
    }
    return 0;
}

static inline int nlh_core_result_valid(const struct nlh_core_request *r,
                                       uint64_t baseline, uint64_t result)
{
    if (r->domain == NLH_CORE_SCHED)
        return result && result <= baseline &&
            (r->value.sched.mode == NLH_MODE_TIMESHARE || result == baseline);
    if (r->domain == NLH_CORE_VM) {
        int32_t sequence = (int32_t)(uint32_t)result;
        int32_t expected = (int32_t)(uint32_t)baseline;
        if (result >> 33 || ((result ^ baseline) & NLH_VM_UPDATE) ||
            ((uint32_t)sequence & (r->value.vm.page_bytes - 1))) return 0;
        if (!(baseline & NLH_VM_UPDATE)) return result == baseline;
        /* Policies can shorten a sequential run, but cannot reverse its sign
         * or invent a run after the baseline detected random access. */
        return (expected >= 0 ? sequence >= 0 && sequence <= expected :
                               sequence <= 0 && sequence >= expected);
    }
    if (r->operation == NLH_VFS_PREFETCH) return result <= baseline;
    return result >= r->value.vfs.minimum_io && result <= baseline &&
        !(result & (r->value.vfs.page_bytes - 1));
}
#endif
