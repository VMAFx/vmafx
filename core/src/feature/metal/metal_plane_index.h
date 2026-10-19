/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  The largest plane a Metal twin may index with a 32-bit `uint`
 *  (T-METAL-UINT-PLANE-INDEX-2026-10-05).
 *
 *  The Metal kernels of float_vif, integer SSIM, float_ssim and float_ms_ssim
 *  keep five moment planes of N samples in one buffer and index sample `at`
 *  of plane k as `k * N + at`, in `uint` (float_vif.metal, integer_ssim.metal,
 *  float_ssim.metal, float_ms_ssim.metal). The largest index, 5N - 1, passes
 *  2^32 - 1 from N = 858,993,460: past 16K for the three SSIM twins, and at
 *  16K for float_vif with a vif_prescale above 2.55. The planes past the wrap
 *  would alias the start of the buffer. Each host calls
 *  vmaf_mtl_plane_index_check() from init(), before it creates a Metal
 *  context or a buffer, and refuses such a frame. A buffer of that size is
 *  beyond the device memory of the Apple GPUs the twins run on in any case.
 *
 *  Host-only C (included by the .mm hosts and by core/test on every
 *  platform); the kernels keep their `uint` indices.
 */

#ifndef VMAF_FEATURE_METAL_METAL_PLANE_INDEX_H_
#define VMAF_FEATURE_METAL_METAL_PLANE_INDEX_H_

#include <errno.h>
#include <stdint.h>

#include "log.h"

/* Moment planes per buffer in the four twins above. */
#define VMAF_MTL_MOMENT_PLANES (5u)

/* Indices a 32-bit `uint` can address: 2^32. */
#define VMAF_MTL_UINT_INDEX_COUNT (UINT64_C(4294967296))

/**
 * 0 when `planes` planes of `samples` samples have their largest index,
 * planes * samples - 1, inside a 32-bit `uint`; otherwise -EINVAL after
 * logging the extractor, the plane size and the limit.
 */
static inline int vmaf_mtl_plane_index_check(const char *extractor, uint64_t samples,
                                             unsigned planes)
{
    if (planes == 0u || samples <= VMAF_MTL_UINT_INDEX_COUNT / planes) {
        return 0;
    }
    vmaf_log(VMAF_LOG_LEVEL_ERROR,
             "%s: %u planes of %llu samples pass the 32-bit index of the Metal kernels "
             "(at most %llu samples per plane)\n",
             extractor, planes, (unsigned long long)samples,
             (unsigned long long)(VMAF_MTL_UINT_INDEX_COUNT / planes));
    return -EINVAL;
}

#endif /* VMAF_FEATURE_METAL_METAL_PLANE_INDEX_H_ */
