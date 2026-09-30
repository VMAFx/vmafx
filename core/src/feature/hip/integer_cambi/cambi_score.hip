/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  HIP kernels of the device-resident CAMBI extractor (ADR-1378, the HIP
 *  port of the SYCL design of ADR-1357).
 *
 *  Every stage of cambi.c runs here, in the order integer_cambi_hip.c
 *  enqueues it on one stream per frame: input validation and preprocessing,
 *  the tiled 7x7 spatial mask, and per scale decimation, the horizontal and
 *  vertical mode filter (the vertical pass also writes the level map), the
 *  run / change bit masks, the c-values with top-K pass 0, the radix select
 *  passes 1 and 2 and the exact top-K sum. The host reads back one
 *  CambiHipResults block per frame.
 *
 *  The per-work-item arithmetic lives in cambi_hip_device.h, shared with the
 *  host unit test that replays these kernels; the kernels below add only the
 *  work decomposition, shared memory, barriers and atomics. Every reduction is
 *  over integers, so its order cannot change a result, and no kernel uses
 *  fp64. Every kernel takes (const CambiHipParams *, int scale, int pass).
 */

#include <hip/hip_runtime.h>
#include <stdint.h>

#include "cambi_hip_device.h"

/* The work-item's 2-D position inside a 16x16-block grid. */
__device__ static inline uint32_t cambi_gx(void)
{
    return blockIdx.x * blockDim.x + threadIdx.x;
}

__device__ static inline uint32_t cambi_gy(void)
{
    return blockIdx.y * blockDim.y + threadIdx.y;
}

/* Sum of one uint64 per work-item over the whole work-group; `lanes` is the
 * work-group size, a power of two. Integer addition, so the tree order does
 * not matter and the result is the same on wave32 and wave64 devices. */
__device__ static uint64_t cambi_group_sum(uint64_t value, uint64_t *shared, uint32_t lanes)
{
    const uint32_t lid = threadIdx.x;
    shared[lid] = value;
    __syncthreads();
    for (uint32_t span = lanes / 2u; span > 0u; span >>= 1u) {
        if (lid < span)
            shared[lid] += shared[lid + span];
        __syncthreads();
    }
    return shared[0];
}

extern "C" {

/* cambi.c::validate_image, for bit depths other than 8 and 16. */
__global__ void cambi_hip_validate(const CambiHipParams *p, int scale, int pass)
{
    (void)scale;
    (void)pass;
    const uint32_t x = cambi_gx();
    const uint32_t y = cambi_gy();
    if (x >= p->src_width || y >= p->src_height)
        return;
    if (cambi_hd_sample_invalid(p, x, y))
        atomicOr(&p->frame->results.status, CAMBI_HIP_STATUS_INVALID_INPUT);
}

/* cambi.c::cambi_preprocessing: 10-bit conversion, resize, anti-dither. */
__global__ void cambi_hip_preprocess(const CambiHipParams *p, int scale, int pass)
{
    (void)scale;
    (void)pass;
    const uint32_t x = cambi_gx();
    const uint32_t y = cambi_gy();
    if (x < p->proc_width && y < p->proc_height)
        p->preproc[y * p->proc_width + x] = cambi_hd_preproc_pixel(p, y, x);
}

/* cambi.c::get_spatial_mask on one CAMBI_HIP_TILE^2 tile. */
__global__ void cambi_hip_spatial_mask(const CambiHipParams *p, int scale, int pass)
{
    (void)scale;
    (void)pass;
    __shared__ uint32_t pixels[CAMBI_HIP_MASK_PIXELS];
    __shared__ uint8_t flags[CAMBI_HIP_MASK_FLAGS];
    __shared__ uint8_t row_sums[CAMBI_HIP_MASK_ROW_SUMS];
    const uint32_t lid = threadIdx.y * CAMBI_HIP_TILE + threadIdx.x;
    const uint32_t lanes = CAMBI_HIP_TILE * CAMBI_HIP_TILE;
    const int y0 = (int)(blockIdx.y * CAMBI_HIP_TILE) - (int)CAMBI_HIP_MASK_HALO;
    const int x0 = (int)(blockIdx.x * CAMBI_HIP_TILE) - (int)CAMBI_HIP_MASK_HALO;
    for (uint32_t i = lid; i < CAMBI_HIP_MASK_PIXELS; i += lanes)
        pixels[i] = cambi_hd_mask_tile_pixel(p, y0, x0, i);
    __syncthreads();
    for (uint32_t i = lid; i < CAMBI_HIP_MASK_FLAGS; i += lanes)
        flags[i] = cambi_hd_mask_tile_flag(pixels, i);
    __syncthreads();
    for (uint32_t i = lid; i < CAMBI_HIP_MASK_ROW_SUMS; i += lanes)
        row_sums[i] = cambi_hd_mask_tile_row_sum(flags, i);
    __syncthreads();
    const uint32_t x = cambi_gx();
    const uint32_t y = cambi_gy();
    if (x < p->proc_width && y < p->proc_height)
        p->mask[y * p->proc_width + x] =
            cambi_hd_mask_tile_out(row_sums, threadIdx.y, threadIdx.x, p->mask_index);
}

/* cambi.c::decimate of the previous scale's filtered image. */
__global__ void cambi_hip_decimate(const CambiHipParams *p, int scale, int pass)
{
    (void)pass;
    const CambiHipScale *sc = &p->scale[scale];
    const uint32_t x = cambi_gx();
    const uint32_t y = cambi_gy();
    if (x < sc->width && y < sc->height)
        cambi_hd_decimate_pixel(sc, x, y);
}

/* cambi.c::filter_mode, horizontal pass. */
__global__ void cambi_hip_filter_h(const CambiHipParams *p, int scale, int pass)
{
    (void)pass;
    const CambiHipScale *sc = &p->scale[scale];
    const uint32_t x = cambi_gx();
    const uint32_t y = cambi_gy();
    if (x < sc->width && y < sc->height)
        cambi_hd_filter_h_pixel(sc, x, y);
}

/* cambi.c::filter_mode, vertical pass, plus the level map. */
__global__ void cambi_hip_filter_v(const CambiHipParams *p, int scale, int pass)
{
    (void)pass;
    const CambiHipScale *sc = &p->scale[scale];
    const uint32_t x = cambi_gx();
    const uint32_t y = cambi_gy();
    if (x < sc->width && y < sc->height)
        cambi_hd_filter_v_pixel(p, sc, x, y);
}

/* Run / change bit masks: a 32 x CAMBI_HIP_ROWMASK_ROWS work-group, one
 * column per work-item; the first item of each row ORs the row's 32 bits. */
__global__ void cambi_hip_row_masks(const CambiHipParams *p, int scale, int pass)
{
    (void)pass;
    __shared__ uint32_t run_bits[CAMBI_HIP_ROWMASK_ROWS * 32u];
    __shared__ uint32_t change_bits[CAMBI_HIP_ROWMASK_ROWS * 32u];
    const CambiHipScale *sc = &p->scale[scale];
    const uint32_t x = cambi_gx();
    const uint32_t y = cambi_gy();
    const uint32_t slot = threadIdx.y * 32u + threadIdx.x;
    uint32_t run = 0u;
    uint32_t change = 0u;
    if (x < sc->width && y < sc->height)
        cambi_hd_row_mask_bits(p->q, sc->width, sc->height, p->pad, x, y, &run, &change);
    run_bits[slot] = run << threadIdx.x;
    change_bits[slot] = change << threadIdx.x;
    __syncthreads();
    if (threadIdx.x != 0u || y >= sc->height)
        return;
    uint32_t run_word = 0u;
    uint32_t change_word = 0u;
    for (uint32_t b = 0u; b < 32u; ++b) {
        run_word |= run_bits[slot + b];
        change_word |= change_bits[slot + b];
    }
    p->runs[y * sc->words + blockIdx.x] = run_word;
    p->change[y * sc->words + blockIdx.x] = change_word;
}

/* c-values of one histogram column per work-item, radix pass 0 and the sum
 * of every c-value (the whole top-K sum when the threshold resolves to 0). */
__global__ void cambi_hip_cvals(const CambiHipParams *p, int scale, int pass)
{
    (void)pass;
    __shared__ uint64_t sums[CAMBI_HIP_CVALS_BLOCK];
    const CambiHdCvals a = cambi_hd_cvals_args(p, scale);
    CambiHdTally tally = {0u, 0u, 0u};
    cambi_hd_cvals_column(&a, blockIdx.y, cambi_gx(), &tally);
    cambi_hd_tally_flush(a.radix_hist, &tally);
    const uint64_t sum = cambi_group_sum(tally.sum, sums, CAMBI_HIP_CVALS_BLOCK);
    CambiHipSelect *sel = &p->frame->select[scale];
    if (threadIdx.x == 0u)
        cambi_hd_add_split(&sel->all_lo, &sel->all_hi, sum);
}

/* Radix histogram of passes 1 and 2 over the bucket the earlier passes
 * chose: a local histogram per work-group, then one atomic per non-empty
 * bin. The resolved flag was written by an earlier kernel, so either every
 * item of the group returns here or none does. */
__global__ void cambi_hip_radix_hist(const CambiHipParams *p, int scale, int pass)
{
    __shared__ uint32_t local_hist[CAMBI_HIP_RADIX_BINS];
    const CambiHipScale *sc = &p->scale[scale];
    CambiHipSelect *sel = &p->frame->select[scale];
    if (sel->resolved != 0u)
        return;
    for (uint32_t b = threadIdx.x; b < CAMBI_HIP_RADIX_BINS; b += CAMBI_HIP_POOL_BLOCK)
        local_hist[b] = 0u;
    __syncthreads();
    uint32_t begin = 0u;
    uint32_t end = 0u;
    cambi_hd_pool_block(sc->width * sc->height, sc->pool_groups, blockIdx.x, &begin, &end);
    for (uint32_t i = begin + threadIdx.x; i < end; i += CAMBI_HIP_POOL_BLOCK) {
        uint32_t bin = 0u;
        if (cambi_hd_radix_match(cambi_hd_float_bits(p->cvals[i]), sel->prefix, pass, &bin))
            atomicAdd(&local_hist[bin], 1u);
    }
    __syncthreads();
    for (uint32_t b = threadIdx.x; b < CAMBI_HIP_RADIX_BINS; b += CAMBI_HIP_POOL_BLOCK) {
        if (local_hist[b] != 0u)
            atomicAdd(&sel->hist[b], local_hist[b]);
    }
}

/* Bucket scan of pass `pass` in one work-group: lane l owns bins
 * [2047 - 8l - 7, 2047 - 8l]; the lane holding the rank-th largest element
 * records its bin, then every lane clears its own bins for the next pass. */
__global__ void cambi_hip_radix_scan(const CambiHipParams *p, int scale, int pass)
{
    __shared__ uint32_t counts[CAMBI_HIP_POOL_BLOCK];
    CambiHipSelect *sel = &p->frame->select[scale];
    if (sel->resolved != 0u)
        return;
    const uint32_t lane = threadIdx.x;
    const uint32_t top = CAMBI_HIP_RADIX_BINS - 1u - lane * CAMBI_HIP_BINS_PER_LANE;
    const uint32_t k = cambi_hd_pass_rank(&p->scale[scale], sel, pass);
    uint32_t mine = 0u;
    for (uint32_t j = 0u; j < CAMBI_HIP_BINS_PER_LANE; ++j)
        mine += sel->hist[top - j];
    counts[lane] = mine;
    __syncthreads();
    if (lane == 0u) {
        uint32_t running = 0u;
        for (uint32_t l = 0u; l < CAMBI_HIP_POOL_BLOCK; ++l) {
            const uint32_t count = counts[l];
            counts[l] = running;
            running += count;
        }
    }
    __syncthreads();
    const uint32_t before = counts[lane];
    if (before < k && k <= before + mine)
        cambi_hd_scan_pick(sel, top, CAMBI_HIP_BINS_PER_LANE, k, before, pass);
    for (uint32_t j = 0u; j < CAMBI_HIP_BINS_PER_LANE; ++j)
        sel->hist[top - j] = 0u;
}

/* Fixed-point sum of every c-value strictly above the threshold. Not needed
 * when the threshold resolved to 0: the c-values kernel summed everything. */
__global__ void cambi_hip_topk_sum(const CambiHipParams *p, int scale, int pass)
{
    (void)pass;
    __shared__ uint64_t sums[CAMBI_HIP_POOL_BLOCK];
    const CambiHipScale *sc = &p->scale[scale];
    CambiHipSelect *sel = &p->frame->select[scale];
    if (sel->resolved != 0u)
        return;
    uint32_t begin = 0u;
    uint32_t end = 0u;
    cambi_hd_pool_block(sc->width * sc->height, sc->pool_groups, blockIdx.x, &begin, &end);
    const uint32_t threshold = sel->prefix;
    uint64_t sum = 0u;
    for (uint32_t i = begin + threadIdx.x; i < end; i += CAMBI_HIP_POOL_BLOCK) {
        const float value = p->cvals[i];
        sum += cambi_hd_float_bits(value) > threshold ? cambi_hd_fixed(value) : 0u;
    }
    const uint64_t total = cambi_group_sum(sum, sums, CAMBI_HIP_POOL_BLOCK);
    if (threadIdx.x == 0u)
        cambi_hd_add_split(&sel->gt_lo, &sel->gt_hi, total);
}

/* The exact 128-bit top-K sum of the scale into the readback block. */
__global__ void cambi_hip_topk_final(const CambiHipParams *p, int scale, int pass)
{
    (void)pass;
    if (threadIdx.x != 0u)
        return;
    const CambiHdU128 total = cambi_hd_topk_total(&p->frame->select[scale]);
    p->frame->results.sum_lo[scale] = total.lo;
    p->frame->results.sum_hi[scale] = total.hi;
}

} /* extern "C" */
