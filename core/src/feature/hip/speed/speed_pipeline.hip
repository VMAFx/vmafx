/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  HIP kernels of the device-resident SpEED chain shared by speed_chroma_hip
 *  and speed_temporal_hip (ADR-1384, the HIP port of the SYCL design of
 *  ADR-1358).
 *
 *  speed_hip_pipeline.c enqueues them on one stream per frame, after one
 *  upload of the raw planes: optional prescale, the anti-alias filter at the
 *  16x-decimated sample points, local mean subtraction with the independent
 *  term, the 25 submatrix means, the 25x25 covariance matrices, the
 *  eigenvalue problem, regularity and QR factorisation, the per-block solve,
 *  variance and entropy, and the frame score. The host reads back one
 *  SpeedGpuFrameResult at collect time.
 *
 *  The arithmetic of every work-item is speed_hip_device.h, shared with the
 *  host unit test that replays these kernels; the kernels add only the work
 *  decomposition, shared memory and barriers. This TU is compiled with
 *  -ffp-contract=off and -fhip-fp32-correctly-rounded-divide-sqrt
 *  (hip_cu_extra_flags in core/src/meson.build) and uses no fp64. Every
 *  kernel takes (const SpeedHipParams *, uint32_t binding_set).
 */

#include <hip/hip_runtime.h>
#include <stdint.h>

#include "speed_hip_device.h"

/* The work-item's pixel position and channel in a 2-D-tile x channel grid. */
__device__ static inline uint32_t speed_gx(void)
{
    return blockIdx.x * blockDim.x + threadIdx.x;
}

__device__ static inline uint32_t speed_gy(void)
{
    return blockIdx.y * blockDim.y + threadIdx.y;
}

extern "C" {

/* vif_scale_frame_s() of the raw planes, when the frame is resampled. */
__global__ void speed_hip_scale(const SpeedHipParams *p, uint32_t set)
{
    const SpeedGpuGeometry *g = &p->geometry;
    const uint32_t x = speed_gx();
    const uint32_t y = speed_gy();
    const uint32_t ch = blockIdx.z;
    if (x < g->scaled_w && y < g->scaled_h)
        p->scaled[((size_t)ch * g->scaled_h + y) * g->scaled_w + x] =
            speed_hd_scale_sample(p, set, ch, y, x);
}

/* The anti-alias filter evaluated only at the 16x-decimated points. */
__global__ void speed_hip_decimate(const SpeedHipParams *p, uint32_t set)
{
    const SpeedGpuGeometry *g = &p->geometry;
    const uint32_t j = speed_gx();
    const uint32_t i = speed_gy();
    const uint32_t ch = blockIdx.z;
    if (j < g->down_w && i < g->down_h)
        p->down[((size_t)ch * g->down_h + i) * g->down_w + j] =
            speed_hd_antialias_at(p, set, ch, i, j);
}

/* Local mean subtraction and the independent term. */
__global__ void speed_hip_centre(const SpeedHipParams *p, uint32_t set)
{
    (void)set;
    const SpeedGpuGeometry *g = &p->geometry;
    const uint32_t j = speed_gx();
    const uint32_t i = speed_gy();
    if (j < g->trunc_w && i < g->trunc_h)
        speed_hd_centre_item(p, blockIdx.z, i, j);
}

/* The 25 submatrix means of every channel, one work-item each. */
__global__ void speed_hip_means(const SpeedHipParams *p, uint32_t set)
{
    (void)set;
    const uint32_t item = blockIdx.x * blockDim.x + threadIdx.x;
    if (item >= p->channels * SPEED_HIP_N)
        return;
    const uint32_t ch = item / SPEED_HIP_N;
    const uint32_t element = item % SPEED_HIP_N;
    p->means[ch * SPEED_HIP_N + element] = speed_hd_submatrix_mean(p, ch, element);
}

/* One work-group per (channel, lower-triangle entry): exact fp32-pair
 * partial sums, a pair-arithmetic tree over the group, one rounding. */
__global__ void speed_hip_covariance(const SpeedHipParams *p, uint32_t set)
{
    (void)set;
    __shared__ float hi[SPEED_HIP_GROUP];
    __shared__ float lo[SPEED_HIP_GROUP];
    const uint32_t group = p->cov_group;
    const uint32_t lid = threadIdx.x;
    const uint32_t ch = blockIdx.x / SPEED_HIP_TRIANGLE;
    uint32_t x = 0u;
    uint32_t y = 0u;
    speed_hd_triangle_entry(blockIdx.x % SPEED_HIP_TRIANGLE, &x, &y);
    const SpeedHdFf part = speed_hd_covariance_partial(p, ch, x, y, lid, group);
    hi[lid] = part.hi;
    lo[lid] = part.lo;
    __syncthreads();
    for (uint32_t span = group / 2u; span > 0u; span >>= 1u) {
        if (lid < span) {
            const SpeedHdFf merged = speed_hd_ff_add(speed_hd_ff(hi[lid], lo[lid]),
                                                     speed_hd_ff(hi[lid + span], lo[lid + span]));
            hi[lid] = merged.hi;
            lo[lid] = merged.lo;
        }
        __syncthreads();
    }
    if (lid == 0u)
        speed_hd_covariance_store(p, ch, x, y, speed_hd_ff(hi[0], lo[0]));
}

/* One work-group of SPEED_HIP_LINALG_GROUP per channel: eigenvalues,
 * regularity and QR factorisation in shared memory. */
__global__ void speed_hip_linalg(const SpeedHipParams *p, uint32_t set)
{
    (void)set;
    __shared__ float slm[SPEED_HIP_SLM_FLOATS];
    SpeedHdLanes lanes;
    lanes.lid = threadIdx.x;
    lanes.count = SPEED_HIP_LINALG_GROUP;
    speed_hd_linalg_group(&lanes, p, blockIdx.x, slm);
}

/* Per (channel, block): Q^T b, back substitution, variance and entropy. */
__global__ void speed_hip_solve(const SpeedHipParams *p, uint32_t set)
{
    (void)set;
    const uint32_t block = blockIdx.x * blockDim.x + threadIdx.x;
    if (block < p->geometry.blocks)
        speed_hd_block_statistics(p, blockIdx.y, block);
}

/* One work-group per score pair: the per-block contributions in parallel,
 * then lane 0's sequential sum in the reference order. */
__global__ void speed_hip_score(const SpeedHipParams *p, uint32_t set)
{
    (void)set;
    const uint32_t pair = blockIdx.x;
    const uint32_t blocks = p->geometry.blocks;
    for (uint32_t b = threadIdx.x; b < blocks; b += SPEED_HIP_GROUP)
        p->contrib[(size_t)pair * blocks + b] = speed_hd_block_score(p, pair, b);
    __syncthreads();
    if (threadIdx.x == 0u)
        speed_hd_score_finish(p, pair);
}

} /* extern "C" */
