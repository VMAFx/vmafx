/**
 *  Copyright (c) the JPEG XL Project Authors.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent AND BSD-3-Clause
 *
 *  Device stages of the ssimulacra2 CUDA twin that used to run on the host
 *  (ADR-1391, the CUDA port of the ADR-1363 chain): YUV -> linear RGB, linear
 *  RGB -> XYB, the per-pixel SSIM / edge-difference terms with their
 *  per-channel sums, and the 2x2 downsample. The products and blurs are in
 *  ssimulacra2_blur.cu.
 *
 *  Bit-exactness: built with `--fmad=false` (no contraction), the cube root
 *  and the sRGB EOTF are the shared helpers of ssimulacra2_math.h with a
 *  correctly rounded division (`__fdiv_rn`), and every product that feeds an
 *  add sits in its own expression, as in ssimulacra2.c. The per-pixel terms
 *  that the CPU evaluates in fp64 are evaluated in fp64 with the CPU's
 *  expressions, and their sums are the CPU's too (ADR-1433): ssim_map() and
 *  edge_diff_map() add each term pixel after pixel into one double, and
 *  feature/ordered_sum.h returns the bits of that loop from pieces the
 *  device forms in parallel. Four kernels per scale:
 *
 *    ssimulacra2_chunk_sums      each chunk's six sums, in a tree (advice);
 *    ssimulacra2_chunk_plan      the binade every chunk starts in, from those;
 *    ssimulacra2_chunk_units     each chunk's terms as integer increments of
 *                                that binade, composed in pixel order;
 *    ssimulacra2_ordered_totals  one walk per sum over the chunks. A chunk
 *                                whose plan does not hold at the exact sum (it
 *                                crosses a binade: a few dozen per sum) is
 *                                added term by term, in pixel order.
 */

#include <cstdint>

#include "cuda/ssimulacra2_cuda.h"

/* The shared helpers of ssimulacra2_math.h / ssimulacra2_score.h run in device
 * code here, the EOTF table is compiled into device memory with the same
 * values, and the cube root divides correctly rounded (ADR-1391). */
#define VMAF_SS2_FUNC static __device__ __forceinline__
#define VMAF_SS2_FDIV(a, b) __fdiv_rn((a), (b))
#define VMAF_SS2_EOTF_LUT_STORAGE static __device__ const
#include "feature/ssimulacra2_math.h"
#include "feature/ssimulacra2_score.h"

/* The ordered-sum helpers run in device code as well (ADR-1433); a double's
 * bits come from the device intrinsics. */
#define VMAF_ORDSUM_FUNC static __device__ __forceinline__
#define VMAF_ORDSUM_BITS(v) ((uint64_t)__double_as_longlong(v))
#define VMAF_ORDSUM_FROM_BITS(bits) __longlong_as_double((long long)(bits))
#include "feature/ordered_sum.h"

namespace
{

/* ssimulacra2.c constants. */
constexpr float kC2 = 0.0009f;
constexpr float kM00 = 0.30f;
constexpr float kM02 = 0.078f;
constexpr float kM10 = 0.23f;
constexpr float kM12 = 0.078f;
constexpr float kM20 = 0.24342268924547819f;
constexpr float kM21 = 0.20476744424496821f;
constexpr float kOpsinBias = 0.0037930732552754493f;

/* ssimulacra2.c::read_plane coordinate mapping (nearest neighbour). */
__device__ __forceinline__ unsigned ss2c_map(unsigned v, unsigned plane_dim, unsigned luma_dim)
{
    uint64_t s = v;
    if (plane_dim != luma_dim) {
        s = (plane_dim * 2u == luma_dim) ? (uint64_t)(v >> 1) : (uint64_t)v * plane_dim / luma_dim;
    }
    return (s >= plane_dim) ? plane_dim - 1u : (unsigned)s;
}

__device__ __forceinline__ float ss2c_sample(const Ss2cYuvArgs &a, unsigned img, unsigned p,
                                             unsigned x, unsigned y)
{
    const unsigned sx = ss2c_map(x, a.plane_w[p], a.width);
    const unsigned sy = ss2c_map(y, a.plane_h[p], a.height);
    // SAFETY: sx < plane_w[p] and sy < plane_h[p] (ss2c_map clamps), and the
    // picture holds plane_h[p] rows of pitch[img][p] bytes.
    const char *row = (const char *)a.plane[img][p] + (size_t)sy * a.pitch[img][p];
    if (a.wide)
        return (float)((const uint16_t *)row)[sx];
    return (float)((const uint8_t *)row)[sx];
}

__device__ __forceinline__ float ss2c_clampf(float v)
{
    if (v < 0.0f)
        return 0.0f;
    if (v > 1.0f)
        return 1.0f;
    return v;
}

__device__ __forceinline__ double ss2c_quartic(double x)
{
    x *= x;
    return x * x;
}

/* The six terms of ssimulacra2.c::ssim_map and ::edge_diff_map for one pixel
 * of one channel, in the order of the CPU's sums: d, d^4, artifact,
 * artifact^4, detail, detail^4. Each is >= 0, or not finite. */
__device__ __forceinline__ void ss2c_terms(const Ss2cCombineArgs &a, size_t idx,
                                           double terms[SS2C_SUMS])
{
    const float mu1 = a.mu1[idx];
    const float mu2 = a.mu2[idx];
    const float mu11 = mu1 * mu1;
    const float mu22 = mu2 * mu2;
    const float mu12 = mu1 * mu2;
    const float diff = mu1 - mu2;
    const float diff_sq = diff * diff;
    const float num_m = 1.0f - diff_sq;
    const float cov = a.s12[idx] - mu12;
    const float twice_cov = 2.0f * cov;
    const float num_s = twice_cov + kC2;
    const float denom_s = (a.s11[idx] - mu11) + (a.s22[idx] - mu22) + kC2;
    double d = 1.0 - ((double)num_m * (double)num_s / (double)denom_s);
    if (d < 0.0)
        d = 0.0;

    const double ed1 = fabs((double)a.img1[idx] - (double)mu1);
    const double ed2 = fabs((double)a.img2[idx] - (double)mu2);
    const double d1 = (1.0 + ed2) / (1.0 + ed1) - 1.0;
    double artifact;
    double detail;
    vmaf_ss2_split_edge_difference(d1, &artifact, &detail);

    terms[0] = d;
    terms[1] = ss2c_quartic(d);
    terms[2] = artifact;
    terms[3] = ss2c_quartic(artifact);
    terms[4] = detail;
    terms[5] = ss2c_quartic(detail);
}

/* Terms of the `j`-th pixel of `lane` in `chunk` of channel `c`; zeros past
 * the end of the plane, which leave every sum as it is. */
__device__ __forceinline__ void ss2c_lane_terms(const Ss2cCombineArgs &a, unsigned c,
                                                unsigned chunk, unsigned lane, unsigned j,
                                                double terms[SS2C_SUMS])
{
    const size_t i = (size_t)chunk * (size_t)SS2C_CHUNK_PIXELS + (size_t)lane * SS2C_CHUNK_RUN + j;
    if (i < a.pixels) {
        ss2c_terms(a, (size_t)c * a.pixels + i, terms);
        return;
    }
    for (unsigned k = 0; k < SS2C_SUMS; k++)
        terms[k] = 0.0;
}

/* Fixed-shape tree over the SS2C_REDUCE_BLOCK threads of a block; thread 0
 * ends with the block's six sums in shared[0..5]. */
__device__ __forceinline__ void ss2c_block_tree(double *shared, unsigned lane,
                                                const double sums[SS2C_SUMS])
{
    for (unsigned k = 0; k < SS2C_SUMS; k++)
        shared[lane * SS2C_SUMS + k] = sums[k];
    __syncthreads();
    for (unsigned half = SS2C_REDUCE_BLOCK / 2u; half > 0u; half >>= 1) {
        if (lane < half) {
            for (unsigned k = 0; k < SS2C_SUMS; k++)
                shared[lane * SS2C_SUMS + k] += shared[(lane + half) * SS2C_SUMS + k];
        }
        __syncthreads();
    }
}

/* Slot of (lane, sum) in the shared increments of a block: even, odd. */
__device__ __forceinline__ unsigned ss2c_units_slot(unsigned lane, unsigned k)
{
    return (lane * SS2C_SUMS + k) * 2u;
}

__device__ __forceinline__ VmafOrdsumUnits ss2c_units_load(const long long *shared, unsigned lane,
                                                           unsigned k)
{
    const unsigned slot = ss2c_units_slot(lane, k);
    return vmaf_ordsum_units((int64_t)shared[slot], (int64_t)shared[slot + 1u]);
}

__device__ __forceinline__ void ss2c_units_store(long long *shared, unsigned lane, unsigned k,
                                                 VmafOrdsumUnits units)
{
    const unsigned slot = ss2c_units_slot(lane, k);
    shared[slot] = (long long)units.even;
    shared[slot + 1u] = (long long)units.odd;
}

/* The lanes of a block composed in lane order: at each step every lane whose
 * index is a multiple of 2 * step takes the lane `step` above it, which has
 * finished the step before. Lane 0 ends with the chunk's increment. The
 * composition is not commutative, so the pairing must be adjacent. */
__device__ __forceinline__ void ss2c_ordered_tree(long long *shared, unsigned lane)
{
    for (unsigned step = 1u; step < SS2C_REDUCE_BLOCK; step <<= 1) {
        if ((lane & (2u * step - 1u)) == 0u) {
            for (unsigned k = 0; k < SS2C_SUMS; k++) {
                const VmafOrdsumUnits left = ss2c_units_load(shared, lane, k);
                const VmafOrdsumUnits right = ss2c_units_load(shared, lane + step, k);
                ss2c_units_store(shared, lane, k, vmaf_ordsum_then(left, right));
            }
        }
        __syncthreads();
    }
}

/* What the lanes of ssimulacra2_ordered_totals do next. */
enum ss2c_walk_command : uint8_t {
    SS2C_WALK_LOAD = 0u,  /* stage batch `operand` of the plan and increments */
    SS2C_WALK_TERMS = 1u, /* compute the terms of chunk `operand` */
    SS2C_WALK_DONE = 2u,
};

/* A lane's share of one batch of chunk sums of one (channel, sum); zeros past
 * the last chunk, which plan as "all zero" and leave the prefix alone. */
__device__ __forceinline__ void ss2c_stage_chunk_sums(const double *sums, unsigned chunks,
                                                      unsigned batch, unsigned lane, double *staged)
{
    for (unsigned j = 0; j < SS2C_BATCH_RUN; j++) {
        const unsigned slot = lane * SS2C_BATCH_RUN + j;
        const size_t chunk = (size_t)batch * (size_t)SS2C_BATCH + slot;
        staged[slot] = chunk < chunks ? sums[chunk * SS2C_SUMS] : 0.0;
    }
}

/* A lane's share of one batch of the plan and increments of one (channel,
 * sum). Slots past the last chunk are not read by the walk. */
__device__ __forceinline__ void ss2c_stage_batch(const int16_t *plan, const int64_t *units,
                                                 unsigned chunks, unsigned batch, unsigned lane,
                                                 short *staged_plan, long long *staged_units)
{
    for (unsigned j = 0; j < SS2C_BATCH_RUN; j++) {
        const unsigned slot = lane * SS2C_BATCH_RUN + j;
        const size_t chunk = (size_t)batch * (size_t)SS2C_BATCH + slot;
        if (chunk >= chunks)
            continue;
        staged_plan[slot] = (short)plan[chunk];
        staged_units[(size_t)slot * 2u] = (long long)units[chunk * 2u];
        staged_units[(size_t)slot * 2u + 1u] = (long long)units[chunk * 2u + 1u];
    }
}

/* Lane 0 of ssimulacra2_ordered_totals: adds the chunks of the staged batch
 * from their increments, starting at `*chunk`, until one needs its terms or
 * the batch ends. Returns what the lanes must do next and its operand. */
__device__ __forceinline__ unsigned ss2c_walk_batch(double *sum, unsigned *chunk, unsigned chunks,
                                                    unsigned staged_batch, const short *plan,
                                                    const long long *units, unsigned *operand)
{
    while (*chunk < chunks && *chunk / SS2C_BATCH == staged_batch) {
        const size_t slot = *chunk % SS2C_BATCH;
        const VmafOrdsumUnits u =
            vmaf_ordsum_units((int64_t)units[slot * 2u], (int64_t)units[slot * 2u + 1u]);
        if (!vmaf_ordsum_add_chunk(sum, (int)plan[slot], u)) {
            *operand = *chunk;
            return SS2C_WALK_TERMS;
        }
        (*chunk)++;
    }
    *operand = *chunk / SS2C_BATCH;
    return *chunk < chunks ? SS2C_WALK_LOAD : SS2C_WALK_DONE;
}

} // namespace

extern "C" {

/* One pixel of ssimulacra2.c::picture_to_linear_rgb per thread; blockIdx.z
 * selects the image. */
__global__ void __launch_bounds__(SS2C_PIX_BX *SS2C_PIX_BY)
    ssimulacra2_yuv_to_linear(const Ss2cYuvArgs a)
{
    const unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned y = blockIdx.y * blockDim.y + threadIdx.y;
    const unsigned img = blockIdx.z;
    if (x >= a.width || y >= a.height)
        return;
    const Ss2cYuvCoefficients &k = a.k;
    const float Y = ss2c_sample(a, img, 0, x, y) * k.inv_peak;
    const float U = ss2c_sample(a, img, 1, x, y) * k.inv_peak;
    const float V = ss2c_sample(a, img, 2, x, y) * k.inv_peak;
    const float Yn = (Y - k.y_off) * k.y_scale;
    const float Un = (U - k.c_off) * k.c_scale;
    const float Vn = (V - k.c_off) * k.c_scale;
    /* Single-rounded FMAs in this order (ADR-0891 / ADR-1205). */
    const float R = __fmaf_rn(k.cr_r, Vn, Yn);
    const float G0 = __fmaf_rn(k.cb_g, Un, Yn);
    const float G = __fmaf_rn(k.cr_g, Vn, G0);
    const float B = __fmaf_rn(k.cb_b, Un, Yn);
    const size_t plane = (size_t)a.width * a.height;
    const size_t idx = (size_t)y * a.width + x;
    float *out = a.out[img];
    out[idx] = vmaf_ss2_srgb_eotf(ss2c_clampf(R));
    out[plane + idx] = vmaf_ss2_srgb_eotf(ss2c_clampf(G));
    out[2u * plane + idx] = vmaf_ss2_srgb_eotf(ss2c_clampf(B));
}

/* One pixel of ssimulacra2.c::linear_rgb_to_xyb per thread; blockIdx.y
 * selects the image. `pixels` = width x height (compact planes). */
__global__ void __launch_bounds__(256)
    ssimulacra2_xyb(const float *__restrict__ lin_ref, const float *__restrict__ lin_dis,
                    float *__restrict__ xyb_ref, float *__restrict__ xyb_dis, unsigned pixels)
{
    const unsigned i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= pixels)
        return;
    const float *lin = (blockIdx.y == 0u) ? lin_ref : lin_dis;
    float *xyb = (blockIdx.y == 0u) ? xyb_ref : xyb_dis;
    const float r = lin[i];
    const float g = lin[pixels + i];
    const float b = lin[2u * pixels + i];
    const float m01 = 1.0f - kM00 - kM02;
    const float m11 = 1.0f - kM10 - kM12;
    const float m22 = 1.0f - kM20 - kM21;
    const float cbrt_bias = vmaf_ss2_cbrtf(kOpsinBias);
    /* Products in named temporaries: `kM00 * r + m01 * g + ...` with
     * contraction off, left to right. */
    const float l_r = kM00 * r;
    const float l_g = m01 * g;
    const float l_b = kM02 * b;
    const float m_r = kM10 * r;
    const float m_g = m11 * g;
    const float m_b = kM12 * b;
    const float s_r = kM20 * r;
    const float s_g = kM21 * g;
    const float s_b = m22 * b;
    float l = ((l_r + l_g) + l_b) + kOpsinBias;
    float m = ((m_r + m_g) + m_b) + kOpsinBias;
    float s = ((s_r + s_g) + s_b) + kOpsinBias;
    if (l < 0.0f)
        l = 0.0f;
    if (m < 0.0f)
        m = 0.0f;
    if (s < 0.0f)
        s = 0.0f;
    const float L = vmaf_ss2_cbrtf(l) - cbrt_bias;
    const float M = vmaf_ss2_cbrtf(m) - cbrt_bias;
    const float S = vmaf_ss2_cbrtf(s) - cbrt_bias;
    const float X = 0.5f * (L - M);
    const float Yv = 0.5f * (L + M);
    /* MakePositiveXYB, libjxl order (B uses Y before Y is offset). */
    const float X14 = X * 14.0f;
    xyb[i] = X14 + 0.42f;
    xyb[pixels + i] = Yv + 0.01f;
    xyb[2u * pixels + i] = (S - Yv) + 0.55f;
}

/* Advice for the plan: the six sums of each chunk, added in a tree.
 * blockIdx.y = channel, blockIdx.x = chunk, one block per chunk. */
__global__ void __launch_bounds__(SS2C_REDUCE_BLOCK) ssimulacra2_chunk_sums(const Ss2cCombineArgs a)
{
    __shared__ double shared[SS2C_REDUCE_BLOCK * SS2C_SUMS];
    const unsigned c = blockIdx.y;
    const unsigned chunk = blockIdx.x;
    const unsigned lane = threadIdx.x;
    double sums[SS2C_SUMS] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    for (unsigned j = 0; j < SS2C_CHUNK_RUN; j++) {
        double terms[SS2C_SUMS];
        ss2c_lane_terms(a, c, chunk, lane, j, terms);
        for (unsigned k = 0; k < SS2C_SUMS; k++)
            sums[k] += terms[k];
    }
    ss2c_block_tree(shared, lane, sums);
    if (lane == 0u) {
        double *out = a.chunk_sums + ((size_t)c * a.chunks + chunk) * SS2C_SUMS;
        for (unsigned k = 0; k < SS2C_SUMS; k++)
            out[k] = shared[k];
    }
}

/* The plan of every chunk of one (channel, sum): blockIdx.y = channel,
 * blockIdx.x = sum. The lanes stage a batch of chunk sums, lane 0 follows
 * their prefix, the lanes store the batch's plan. */
__global__ void __launch_bounds__(SS2C_REDUCE_BLOCK) ssimulacra2_chunk_plan(const Ss2cCombineArgs a)
{
    __shared__ double staged_sums[SS2C_BATCH];
    __shared__ short staged_plan[SS2C_BATCH];
    const unsigned c = blockIdx.y;
    const unsigned k = blockIdx.x;
    const unsigned lane = threadIdx.x;
    const double *sums = a.chunk_sums + (size_t)c * a.chunks * SS2C_SUMS + k;
    int16_t *plan = a.plan + ((size_t)c * SS2C_SUMS + k) * a.chunks;
    const unsigned batches = (a.chunks + SS2C_BATCH - 1u) / SS2C_BATCH;
    double prefix = 0.0;
    for (unsigned batch = 0; batch < batches; batch++) {
        ss2c_stage_chunk_sums(sums, a.chunks, batch, lane, staged_sums);
        __syncthreads();
        if (lane == 0u) {
            for (unsigned i = 0; i < SS2C_BATCH; i++)
                staged_plan[i] = (short)vmaf_ordsum_plan(&prefix, staged_sums[i]);
        }
        __syncthreads();
        for (unsigned j = 0; j < SS2C_BATCH_RUN; j++) {
            const unsigned slot = lane * SS2C_BATCH_RUN + j;
            const size_t chunk = (size_t)batch * (size_t)SS2C_BATCH + slot;
            if (chunk < a.chunks)
                plan[chunk] = (int16_t)staged_plan[slot];
        }
    }
}

/* Each chunk's increment under its plan, composed in pixel order: a lane
 * composes its run, then the lanes are composed in order.
 * blockIdx.y = channel, blockIdx.x = chunk. */
__global__ void __launch_bounds__(SS2C_REDUCE_BLOCK)
    ssimulacra2_chunk_units(const Ss2cCombineArgs a)
{
    __shared__ long long shared[SS2C_REDUCE_BLOCK * SS2C_SUMS * 2];
    const unsigned c = blockIdx.y;
    const unsigned chunk = blockIdx.x;
    const unsigned lane = threadIdx.x;
    const size_t first = ((size_t)c * SS2C_SUMS) * a.chunks + chunk;
    VmafOrdsumUnits units[SS2C_SUMS];
    for (VmafOrdsumUnits &unit : units)
        unit = vmaf_ordsum_units(0, 0);
    for (unsigned j = 0; j < SS2C_CHUNK_RUN; j++) {
        double terms[SS2C_SUMS];
        ss2c_lane_terms(a, c, chunk, lane, j, terms);
        for (unsigned k = 0; k < SS2C_SUMS; k++) {
            const int plan = (int)a.plan[first + (size_t)k * a.chunks];
            units[k] = vmaf_ordsum_then(units[k], vmaf_ordsum_planned_term(terms[k], plan));
        }
    }
    for (unsigned k = 0; k < SS2C_SUMS; k++)
        ss2c_units_store(shared, lane, k, units[k]);
    __syncthreads();
    ss2c_ordered_tree(shared, lane);
    if (lane == 0u) {
        for (unsigned k = 0; k < SS2C_SUMS; k++) {
            int64_t *out = a.units + (first + (size_t)k * a.chunks) * 2u;
            out[0] = (int64_t)shared[ss2c_units_slot(0u, k)];
            out[1] = (int64_t)shared[ss2c_units_slot(0u, k) + 1u];
        }
    }
}

/* The CPU's sum of one term over one channel: blockIdx.y = channel,
 * blockIdx.x = sum. Lane 0 walks the chunks, a staged batch at a time. When a
 * chunk needs its terms, every lane computes its run of them into shared
 * memory and lane 0 adds them in pixel order, as the CPU's loop does. Every
 * round stages a batch or consumes a chunk, which bounds the loop. */
__global__ void __launch_bounds__(SS2C_REDUCE_BLOCK)
    ssimulacra2_ordered_totals(const Ss2cCombineArgs a)
{
    __shared__ double terms_of_chunk[SS2C_CHUNK_PIXELS];
    __shared__ long long staged_units[SS2C_BATCH * 2];
    __shared__ short staged_plan[SS2C_BATCH];
    __shared__ unsigned command;
    __shared__ unsigned operand;
    const unsigned c = blockIdx.y;
    const unsigned k = blockIdx.x;
    const unsigned lane = threadIdx.x;
    const size_t first = ((size_t)c * SS2C_SUMS + k) * a.chunks;
    const unsigned rounds = a.chunks + a.chunks / SS2C_BATCH + 2u;
    double sum = 0.0;
    unsigned chunk = 0u;
    unsigned staged_batch = 0u;
    if (lane == 0u) {
        command = SS2C_WALK_LOAD;
        operand = 0u;
    }
    for (unsigned round = 0; round < rounds; round++) {
        __syncthreads();
        const unsigned todo = command;
        const unsigned what = operand;
        if (todo == SS2C_WALK_DONE)
            break;
        if (todo == SS2C_WALK_LOAD) {
            ss2c_stage_batch(a.plan + first, a.units + first * 2u, a.chunks, what, lane,
                             staged_plan, staged_units);
        } else {
            for (unsigned j = 0; j < SS2C_CHUNK_RUN; j++) {
                double terms[SS2C_SUMS];
                ss2c_lane_terms(a, c, what, lane, j, terms);
                terms_of_chunk[lane * SS2C_CHUNK_RUN + j] = terms[k];
            }
        }
        __syncthreads();
        if (lane == 0u) {
            if (todo == SS2C_WALK_TERMS) {
                for (const double term : terms_of_chunk)
                    sum += term;
                chunk = what + 1u;
            } else {
                staged_batch = what;
            }
            unsigned next = 0u;
            command = ss2c_walk_batch(&sum, &chunk, a.chunks, staged_batch, staged_plan,
                                      staged_units, &next);
            operand = next;
        }
    }
    if (lane == 0u)
        a.totals[(size_t)c * SS2C_SUMS + k] = sum;
}

/* One output pixel of ssimulacra2.c::downsample_2x2 per thread, all three
 * channels; blockIdx.z selects the image. Compact planes on both sides. */
__global__ void __launch_bounds__(SS2C_PIX_BX *SS2C_PIX_BY)
    ssimulacra2_downsample(const float *__restrict__ in_ref, const float *__restrict__ in_dis,
                           float *__restrict__ out_ref, float *__restrict__ out_dis, unsigned iw,
                           unsigned ih, unsigned ow, unsigned oh)
{
    const unsigned ox = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned oy = blockIdx.y * blockDim.y + threadIdx.y;
    if (ox >= ow || oy >= oh)
        return;
    const float *in = (blockIdx.z == 0u) ? in_ref : in_dis;
    float *out = (blockIdx.z == 0u) ? out_ref : out_dis;
    const size_t in_plane = (size_t)iw * ih;
    const size_t out_plane = (size_t)ow * oh;
    for (unsigned c = 0; c < SS2C_CHANNELS; c++) {
        const float *ip = in + (size_t)c * in_plane;
        float sum = 0.0f;
        for (unsigned dy = 0; dy < 2u; dy++) {
            for (unsigned dx = 0; dx < 2u; dx++) {
                unsigned ix = ox * 2u + dx;
                unsigned iy = oy * 2u + dy;
                if (ix >= iw)
                    ix = iw - 1u;
                if (iy >= ih)
                    iy = ih - 1u;
                sum += ip[(size_t)iy * iw + ix];
            }
        }
        out[(size_t)c * out_plane + (size_t)oy * ow + ox] = sum * 0.25f;
    }
}

} /* extern "C" */
