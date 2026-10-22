/**
 *  Copyright 2026 Lusoris
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Properties of an in-order SYCL queue on Level Zero immediate command lists
 * (ADR-3359): one definition for the engine's primary queue
 * (common.cpp::sycl_queue_props()) and the VMAFx library queue
 * (vmafx_sycl_rt.cpp::library_queue_props()).
 *
 * With batched command lists (UR_L0_USE_IMMEDIATE_COMMANDLISTS=0, the Arc
 * A-series setting the bundling page recommends) an Arc A380 on the i915
 * driver, compute-runtime 26.35, silently dropped the zero-copy VA import's
 * copies from a random frame on, once each frame's DMA-BUF import was mapped
 * at the GPU address the previous frame's import had just freed; the shared
 * slots froze, every extractor scored stale frames, and wait_and_throw()
 * reported nothing. The mode of the queue the imports are made against and
 * waited on is what matters. Keep this until a driver fix is re-tested with
 * scripts/test/zerocopy-e2e.sh --repeat 10 under the batched setting.
 *
 * Without the DPC++ extension (another SYCL implementation) the property is
 * left out and VMAF_SYCL_HAVE_IMMEDIATE_CMDLIST is 0; on a backend other than
 * Level Zero the property has no effect.
 */

#ifndef VMAF_SYCL_QUEUE_PROPS_H_
#define VMAF_SYCL_QUEUE_PROPS_H_

#include <sycl/sycl.hpp>

#ifdef SYCL_EXT_INTEL_QUEUE_IMMEDIATE_COMMAND_LIST
#define VMAF_SYCL_HAVE_IMMEDIATE_CMDLIST 1
#else
#define VMAF_SYCL_HAVE_IMMEDIATE_CMDLIST 0
#endif

/* In-order queue properties; `profiling` adds enable_profiling, `immediate`
 * adds immediate_command_list where the extension exists. */
static inline sycl::property_list vmaf_sycl_in_order_props(bool profiling, bool immediate)
{
#if VMAF_SYCL_HAVE_IMMEDIATE_CMDLIST
    using imm = sycl::ext::intel::property::queue::immediate_command_list;
    if (immediate && profiling) {
        return sycl::property_list{sycl::property::queue::in_order{},
                                   sycl::property::queue::enable_profiling{}, imm{}};
    }
    if (immediate) {
        return sycl::property_list{sycl::property::queue::in_order{}, imm{}};
    }
#else
    (void)immediate;
#endif
    if (profiling) {
        return sycl::property_list{sycl::property::queue::in_order{},
                                   sycl::property::queue::enable_profiling{}};
    }
    return sycl::property_list{sycl::property::queue::in_order{}};
}

#endif /* VMAF_SYCL_QUEUE_PROPS_H_ */
