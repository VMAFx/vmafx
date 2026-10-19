/**
 *  Copyright (c) the JPEG XL Project Authors.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent AND BSD-3-Clause
 *
 *  Device module of the HIP ssimulacra2 twin: the whole frame of
 *  ssimulacra2.c::extract on the device (the ADR-1363 chain of
 *  core/src/feature/sycl/ssimulacra2_sycl.cpp, ported to HIP).
 *
 *    ssimulacra2_yuv_to_linear   YUV -> linear RGB from the raw planes
 *    ssimulacra2_xyb             linear RGB -> XYB (MakePositiveXYB folded in)
 *    ssimulacra2_blur_rows/cols  3-pole recursive Gaussian, all three planes;
 *                                the row pass forms ref^2, dis^2, ref*dis
 *    ssimulacra2_chunk_sums      each chunk's six sums, in a tree (advice)
 *    ssimulacra2_chunk_plan      the binade every chunk starts in, from those
 *    ssimulacra2_chunk_units     each chunk's terms as integer increments of
 *                                that binade, composed in pixel order
 *    ssimulacra2_ordered_totals  one walk per sum over the chunks; a chunk
 *                                whose plan does not hold at the exact sum is
 *                                added term by term, in pixel order
 *    ssimulacra2_downsample      2x2 box downsample of the linear-RGB pyramid
 *
 *  Numerical contract: every stage is bit-identical to the CPU extractor
 *  (ADR-1445). That needs contraction off (the pragma below;
 *  core/src/meson.build also passes -ffp-contract=off for this module),
 *  products that feed adds in named temporaries, single-rounded fmaf() in the
 *  YUV matrix (ADR-0891 / ADR-1205), and the shared cube root and sRGB EOTF of
 *  ssimulacra2_math.h with a correctly rounded division (__fdiv_rn; HIP fp32
 *  `/` is correctly rounded by default,
 *  -fhip-fp32-correctly-rounded-divide-sqrt). The per-pixel terms that the CPU
 *  evaluates in fp64 are evaluated in fp64 with the CPU's expressions (the
 *  gfx1036's fp64 `+`, `-`, `*` and `/` return the host's bits: 16.8 million
 *  pixels of the six terms, ADR-1445), and their sums are the CPU's too:
 *  ssim_map() and edge_diff_map() add each term pixel after pixel into one
 *  double, and feature/ordered_sum.h returns the bits of that loop from pieces
 *  the device forms in parallel. The four kernels of the sums are the CUDA
 *  twin's (ADR-1433).
 */

#include <hip/hip_runtime.h>

#include <cstddef>
#include <cstdint>

#include "hip/ssimulacra2_hip.h"

/* Every add, subtract and multiply below rounds on its own, as in
 * ssimulacra2.c. */
#pragma clang fp contract(off)

/* The shared cube root and EOTF are plain C inline functions; compile them for
 * the device as well. The EOTF LUT is a namespace-scope const table, which HIP
 * places in device memory for the device copy of the function. */
#define VMAF_SS2_FDIV(a, b) __fdiv_rn((a), (b))
#pragma clang force_cuda_host_device begin
#include "feature/ssimulacra2_math.h"
#include "feature/ssimulacra2_score.h"
#pragma clang force_cuda_host_device end

/* The ordered-sum helpers run in device code as well; a double's bits come
 * from the device intrinsics. */
#define VMAF_ORDSUM_FUNC __device__ inline
#define VMAF_ORDSUM_BITS(v) ((uint64_t)__double_as_longlong(v))
#define VMAF_ORDSUM_FROM_BITS(bits) __longlong_as_double((long long)(bits))
#include "feature/ordered_sum.h"

namespace
{

constexpr float kSs2C2 = 0.0009f; /* ssimulacra2.c kC2 */

/* Pixels of one reduction chunk and chunks of one staged batch, as size_t so
 * that the index arithmetic below is formed in 64 bits. */
constexpr size_t kChunkPixels = (size_t)SS2H_REDUCE_BLOCK * SS2H_CHUNK_RUN;
constexpr size_t kBatchChunks = (size_t)SS2H_REDUCE_BLOCK * SS2H_BATCH_RUN;

/* ------------------------------------------------------------------ */
/* YUV -> linear RGB, XYB, downsample                                  */
/* ------------------------------------------------------------------ */

/* ssimulacra2.c::read_plane coordinate mapping (nearest neighbour). */
__device__ inline unsigned ss2h_map(unsigned v, unsigned plane_dim, unsigned luma_dim)
{
    uint64_t s = v;
    if (plane_dim != luma_dim) {
        s = (plane_dim * 2u == luma_dim) ? (uint64_t)(v >> 1u) : (uint64_t)v * plane_dim / luma_dim;
    }
    return (s >= plane_dim) ? plane_dim - 1u : (unsigned)s;
}

__device__ inline float ss2h_sample(const Ss2hYuvArgs &a, unsigned img, unsigned p, unsigned x,
                                    unsigned y)
{
    const unsigned sx = ss2h_map(x, a.plane_w[p], a.width);
    const unsigned sy = ss2h_map(y, a.plane_h[p], a.height);
    const size_t idx = (size_t)sy * a.plane_w[p] + sx;
    if (a.wide) {
        return (float)static_cast<const uint16_t *>(a.plane[img][p])[idx];
    }
    return (float)static_cast<const uint8_t *>(a.plane[img][p])[idx];
}

__device__ inline float ss2h_clampf(float v)
{
    if (v < 0.0f) {
        return 0.0f;
    }
    if (v > 1.0f) {
        return 1.0f;
    }
    return v;
}

/* One pixel of ssimulacra2.c::picture_to_linear_rgb for one image. */
__device__ inline void ss2h_yuv_pixel(const Ss2hYuvArgs &a, unsigned img, unsigned x, unsigned y)
{
    const Ss2hYuvCoefficients &k = a.k;
    const float Y = ss2h_sample(a, img, 0u, x, y) * k.inv_peak;
    const float U = ss2h_sample(a, img, 1u, x, y) * k.inv_peak;
    const float V = ss2h_sample(a, img, 2u, x, y) * k.inv_peak;
    const float Yn = (Y - k.y_off) * k.y_scale;
    const float Un = (U - k.c_off) * k.c_scale;
    const float Vn = (V - k.c_off) * k.c_scale;
    /* Single-rounded FMAs in this order (ADR-0891 / ADR-1205). */
    const float R = fmaf(k.cr_r, Vn, Yn);
    const float G0 = fmaf(k.cb_g, Un, Yn);
    const float G = fmaf(k.cr_g, Vn, G0);
    const float B = fmaf(k.cb_b, Un, Yn);
    const size_t plane = (size_t)a.width * a.height;
    const size_t idx = (size_t)y * a.width + x;
    float *out = a.out[img];
    out[idx] = vmaf_ss2_srgb_eotf(ss2h_clampf(R));
    out[plane + idx] = vmaf_ss2_srgb_eotf(ss2h_clampf(G));
    out[2u * plane + idx] = vmaf_ss2_srgb_eotf(ss2h_clampf(B));
}

/* One pixel of ssimulacra2.c::linear_rgb_to_xyb (MakePositiveXYB folded in). */
__device__ inline void ss2h_xyb_pixel(const float *lin, float *xyb, size_t i, size_t plane)
{
    constexpr float kM00 = 0.30f;
    constexpr float kM02 = 0.078f;
    constexpr float kM10 = 0.23f;
    constexpr float kM12 = 0.078f;
    constexpr float kM20 = 0.24342268924547819f;
    constexpr float kM21 = 0.20476744424496821f;
    constexpr float kOpsinBias = 0.0037930732552754493f;
    constexpr float m01 = 1.0f - kM00 - kM02;
    constexpr float m11 = 1.0f - kM10 - kM12;
    constexpr float m22 = 1.0f - kM20 - kM21;
    const float cbrt_bias = vmaf_ss2_cbrtf(kOpsinBias);

    const float r = lin[i];
    const float g = lin[plane + i];
    const float b = lin[2u * plane + i];
    const float l_r = kM00 * r;
    const float l_g = m01 * g;
    const float l_b = kM02 * b;
    const float m_r = kM10 * r;
    const float m_g = m11 * g;
    const float m_b = kM12 * b;
    const float s_r = kM20 * r;
    const float s_g = kM21 * g;
    const float s_b = m22 * b;
    const float l = ((l_r + l_g) + l_b) + kOpsinBias;
    const float m = ((m_r + m_g) + m_b) + kOpsinBias;
    const float s = ((s_r + s_g) + s_b) + kOpsinBias;
    /* `if (l < 0.0f) l = 0.0f;` of the reference, NaN kept (not fmax). */
    const float L = vmaf_ss2_cbrtf((l < 0.0f) ? 0.0f : l) - cbrt_bias;
    const float M = vmaf_ss2_cbrtf((m < 0.0f) ? 0.0f : m) - cbrt_bias;
    const float S = vmaf_ss2_cbrtf((s < 0.0f) ? 0.0f : s) - cbrt_bias;
    const float X = 0.5f * (L - M);
    const float Yv = 0.5f * (L + M);
    const float X14 = X * 14.0f;
    xyb[i] = X14 + 0.42f;
    xyb[plane + i] = Yv + 0.01f;
    xyb[2u * plane + i] = (S - Yv) + 0.55f; /* B uses Y before its offset */
}

/* One output pixel of ssimulacra2.c::downsample_2x2, all three planes. */
__device__ inline void ss2h_down_pixel(const float *in, float *out, const Ss2hPlanesArgs &a,
                                       unsigned ox, unsigned oy)
{
    const size_t in_plane = (size_t)a.width * a.height;
    const size_t out_plane = (size_t)a.out_width * a.out_height;
    for (unsigned c = 0; c < SS2H_CHANNELS; c++) {
        const float *ip = in + c * in_plane;
        float sum = 0.0f;
        for (unsigned dy = 0; dy < 2u; dy++) {
            for (unsigned dx = 0; dx < 2u; dx++) {
                const unsigned ix = min(ox * 2u + dx, a.width - 1u);
                const unsigned iy = min(oy * 2u + dy, a.height - 1u);
                sum += ip[(size_t)iy * a.width + ix];
            }
        }
        out[c * out_plane + (size_t)oy * a.out_width + ox] = sum * 0.25f;
    }
}

/* ------------------------------------------------------------------ */
/* Separable recursive Gaussian                                        */
/* ------------------------------------------------------------------ */

struct Ss2hIirState {
    float prev1[3];
    float prev2[3];
};

/* One step of ssimulacra2.c::fast_gaussian_1d; returns o0 + o1 + o2. */
__device__ inline float ss2h_iir_step(float sum, Ss2hIirState &st, const Ss2hIir &k)
{
    float o[3];
#pragma unroll
    for (int p = 0; p < 3; p++) {
        const float ns = k.n2[p] * sum;
        const float dp = k.d1[p] * st.prev1[p];
        const float t = ns - dp;
        o[p] = t - st.prev2[p];
        st.prev2[p] = st.prev1[p];
        st.prev1[p] = o[p];
    }
    const float s01 = o[0] + o[1];
    return s01 + o[2];
}

/* ssimulacra2.c::fast_gaussian_1d along one row (step 1) or one column
 * (step = width) that starts at `base`. */
__device__ inline void ss2h_iir_line(const Ss2hBlurArgs &a, size_t base, size_t step, int xsize)
{
    const int radius = a.iir.radius;
    Ss2hIirState st = {{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}};
    for (int n = -radius + 1; n < xsize; ++n) {
        const int left = n - radius - 1;
        const int right = n + radius - 1;
        const float lv = (left >= 0) ? a.in[base + (size_t)left * step] : 0.0f;
        const float rv = (right < xsize) ? a.in[base + (size_t)right * step] : 0.0f;
        const float s = ss2h_iir_step(lv + rv, st, a.iir);
        if (n >= 0) {
            a.out[base + (size_t)n * step] = s;
        }
    }
}

/* ------------------------------------------------------------------ */
/* SSIM and edge-difference terms, the CPU's sums                      */
/* ------------------------------------------------------------------ */

__device__ inline double ss2h_quartic(double x)
{
    x *= x;
    return x * x;
}

/* The six terms of ssimulacra2.c::ssim_map and ::edge_diff_map for one pixel
 * of one channel, in the order of the CPU's sums: d, d^4, artifact,
 * artifact^4, detail, detail^4. Each is >= 0, or not finite. */
__device__ inline void ss2h_terms(const Ss2hCombineArgs &a, size_t idx, double terms[SS2H_SUMS])
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
    const float num_s = twice_cov + kSs2C2;
    const float denom_s = (a.s11[idx] - mu11) + (a.s22[idx] - mu22) + kSs2C2;
    double d = 1.0 - ((double)num_m * (double)num_s / (double)denom_s);
    if (d < 0.0) {
        d = 0.0;
    }

    const double ed1 = fabs((double)a.img1[idx] - (double)mu1);
    const double ed2 = fabs((double)a.img2[idx] - (double)mu2);
    const double d1 = (1.0 + ed2) / (1.0 + ed1) - 1.0;
    double artifact;
    double detail;
    vmaf_ss2_split_edge_difference(d1, &artifact, &detail);

    terms[0] = d;
    terms[1] = ss2h_quartic(d);
    terms[2] = artifact;
    terms[3] = ss2h_quartic(artifact);
    terms[4] = detail;
    terms[5] = ss2h_quartic(detail);
}

/* Terms of the `j`-th pixel of `lane` in `chunk` of channel `c`; zeros past
 * the end of the plane, which leave every sum as it is. */
__device__ inline void ss2h_lane_terms(const Ss2hCombineArgs &a, unsigned c, unsigned chunk,
                                       unsigned lane, unsigned j, double terms[SS2H_SUMS])
{
    const size_t i = (size_t)chunk * kChunkPixels + (size_t)lane * SS2H_CHUNK_RUN + j;
    if (i < a.pixels) {
        ss2h_terms(a, (size_t)c * a.pixels + i, terms);
        return;
    }
    for (unsigned k = 0; k < SS2H_SUMS; k++) {
        terms[k] = 0.0;
    }
}

/* Fixed-shape tree over the SS2H_REDUCE_BLOCK lanes of a work-group in LDS;
 * lane 0 ends with the group's six sums in shared[0..5]. A shared-memory
 * tree over every lane, not wavefront shuffles, so wave32 and wave64 devices
 * add in the same order; the result only feeds the plan. */
__device__ inline void ss2h_block_tree(double *shared, unsigned lane, const double sums[SS2H_SUMS])
{
    for (unsigned k = 0; k < SS2H_SUMS; k++) {
        shared[lane * SS2H_SUMS + k] = sums[k];
    }
    __syncthreads();
    for (unsigned half = SS2H_REDUCE_BLOCK / 2u; half > 0u; half >>= 1u) {
        if (lane < half) {
            for (unsigned k = 0; k < SS2H_SUMS; k++) {
                shared[lane * SS2H_SUMS + k] += shared[(lane + half) * SS2H_SUMS + k];
            }
        }
        __syncthreads();
    }
}

/* Slot of (lane, sum) in the shared increments of a work-group: even, odd. */
__device__ inline unsigned ss2h_units_slot(unsigned lane, unsigned k)
{
    return (lane * SS2H_SUMS + k) * 2u;
}

__device__ inline VmafOrdsumUnits ss2h_units_load(const long long *shared, unsigned lane,
                                                  unsigned k)
{
    const unsigned slot = ss2h_units_slot(lane, k);
    return vmaf_ordsum_units((int64_t)shared[slot], (int64_t)shared[slot + 1u]);
}

__device__ inline void ss2h_units_store(long long *shared, unsigned lane, unsigned k,
                                        VmafOrdsumUnits units)
{
    const unsigned slot = ss2h_units_slot(lane, k);
    shared[slot] = (long long)units.even;
    shared[slot + 1u] = (long long)units.odd;
}

/* The lanes of a work-group composed in lane order: at each step every lane
 * whose index is a multiple of 2 * step takes the lane `step` above it, which
 * has finished the step before. Lane 0 ends with the chunk's increment. The
 * composition is not commutative, so the pairing must be adjacent. */
__device__ inline void ss2h_ordered_tree(long long *shared, unsigned lane)
{
    for (unsigned step = 1u; step < SS2H_REDUCE_BLOCK; step <<= 1u) {
        if ((lane & (2u * step - 1u)) == 0u) {
            for (unsigned k = 0; k < SS2H_SUMS; k++) {
                const VmafOrdsumUnits left = ss2h_units_load(shared, lane, k);
                const VmafOrdsumUnits right = ss2h_units_load(shared, lane + step, k);
                ss2h_units_store(shared, lane, k, vmaf_ordsum_then(left, right));
            }
        }
        __syncthreads();
    }
}

/* What the lanes of ssimulacra2_ordered_totals do next. */
enum ss2h_walk_command : uint8_t {
    SS2H_WALK_LOAD = 0u,  /* stage batch `operand` of the plan and increments */
    SS2H_WALK_TERMS = 1u, /* compute the terms of chunk `operand` */
    SS2H_WALK_DONE = 2u,
};

/* A lane's share of one batch of chunk sums of one (channel, sum); zeros past
 * the last chunk, which plan as "all zero" and leave the prefix alone. */
__device__ inline void ss2h_stage_chunk_sums(const double *sums, unsigned chunks, unsigned batch,
                                             unsigned lane, double *staged)
{
    for (unsigned j = 0; j < SS2H_BATCH_RUN; j++) {
        const unsigned slot = lane * SS2H_BATCH_RUN + j;
        const size_t chunk = (size_t)batch * kBatchChunks + slot;
        staged[slot] = chunk < chunks ? sums[chunk * SS2H_SUMS] : 0.0;
    }
}

/* A lane's share of one batch of the plan and increments of one (channel,
 * sum). Slots past the last chunk are not read by the walk. */
__device__ inline void ss2h_stage_batch(const int16_t *plan, const int64_t *units, unsigned chunks,
                                        unsigned batch, unsigned lane, short *staged_plan,
                                        long long *staged_units)
{
    for (unsigned j = 0; j < SS2H_BATCH_RUN; j++) {
        const unsigned slot = lane * SS2H_BATCH_RUN + j;
        const size_t chunk = (size_t)batch * kBatchChunks + slot;
        if (chunk >= chunks) {
            continue;
        }
        staged_plan[slot] = (short)plan[chunk];
        const size_t pair = (size_t)slot * 2u;
        staged_units[pair] = (long long)units[chunk * 2u];
        staged_units[pair + 1u] = (long long)units[chunk * 2u + 1u];
    }
}

/* Lane 0 of ssimulacra2_ordered_totals: adds the chunks of the staged batch
 * from their increments, starting at `*chunk`, until one needs its terms or
 * the batch ends. Returns what the lanes must do next and its operand. */
__device__ inline unsigned ss2h_walk_batch(double *sum, unsigned *chunk, unsigned chunks,
                                           unsigned staged_batch, const short *plan,
                                           const long long *units, unsigned *operand)
{
    while (*chunk < chunks && *chunk / SS2H_BATCH == staged_batch) {
        const unsigned slot = *chunk % SS2H_BATCH;
        const size_t pair = (size_t)slot * 2u;
        const VmafOrdsumUnits u = vmaf_ordsum_units((int64_t)units[pair], (int64_t)units[pair + 1u]);
        if (!vmaf_ordsum_add_chunk(sum, (int)plan[slot], u)) {
            *operand = *chunk;
            return SS2H_WALK_TERMS;
        }
        (*chunk)++;
    }
    *operand = *chunk / SS2H_BATCH;
    return *chunk < chunks ? SS2H_WALK_LOAD : SS2H_WALK_DONE;
}

/* ------------------------------------------------------------------ */
/* Row pass through LDS                                                */
/* ------------------------------------------------------------------ */

/* Walking a row in global memory, one lane per row, makes every load of a
 * wavefront touch as many cache lines as it has lanes; on the gfx1036 that
 * pass took 42x the time of the lane-per-column pass (322 vs 7.7 ms per 4K
 * launch, ADR-1390). The row pass instead runs one wavefront per
 * SS2H_ROW_TILE rows: it loads SS2H_ROW_TILE-column tiles of its rows with
 * consecutive lanes on consecutive columns, each lane walks its own row
 * through the tile, and the lanes store the tile's outputs the same way. */

/* One plane of a row pass: its rows row0 .. row0 + SS2H_ROW_TILE - 1. */
struct Ss2hRowPlane {
    const float *a;
    const float *b; /* second factor of a product, or nullptr */
    float *out;
    unsigned row0;
};

__device__ inline Ss2hRowPlane ss2h_row_plane(const Ss2hBlurArgs &args, unsigned c, unsigned row0)
{
    const size_t plane = (size_t)c * args.width * args.height;
    Ss2hRowPlane rows;
    rows.a = args.in + plane;
    rows.b = (args.in2 != nullptr) ? args.in2 + plane : nullptr;
    rows.out = args.out + plane;
    rows.row0 = row0;
    return rows;
}

/* Tile geometry of the row pass: SS2H_ROW_TILE rows (one per lane) by
 * SS2H_ROW_COLS columns. A load instruction covers kRowsPerLoad rows of
 * SS2H_ROW_COLS consecutive columns, so every lane issues kTileLoads loads
 * per tile (twice that for a product), within the 63 loads a gfx10 wavefront
 * can keep in flight. */
constexpr unsigned kRowsPerLoad = SS2H_ROW_TILE / SS2H_ROW_COLS;
constexpr unsigned kTileLoads = SS2H_ROW_TILE / kRowsPerLoad;

/* This lane's share of tile `chunk` (columns chunk * SS2H_ROW_COLS ..) into
 * registers, as fast_gaussian_1d reads it: zero right of the image (and on
 * rows past its end), the product formed here for kProduct (the single fp32
 * multiply of ssimulacra2.c::multiply_3plane). Every lane loads
 * unconditionally, out-of-range lanes from element 0, so the loads of a tile
 * issue back to back instead of one branch and one wait per row. */
template <bool kProduct>
__device__ inline void ss2h_row_tile_load(const Ss2hRowPlane &rows, const Ss2hBlurArgs &args,
                                          int chunk, unsigned lane, float vals[kTileLoads])
{
    const unsigned col = ((unsigned)chunk * SS2H_ROW_COLS) + (lane % SS2H_ROW_COLS);
    const unsigned sub = lane / SS2H_ROW_COLS;
    const bool col_ok = col < args.width;
#pragma unroll
    for (unsigned j = 0; j < kTileLoads; j++) {
        const unsigned row = rows.row0 + (j * kRowsPerLoad) + sub;
        const bool inside = col_ok && row < args.height;
        const size_t idx = inside ? ((size_t)row * args.width) + col : 0u;
        const float pa = rows.a[idx];
        const float v = kProduct ? pa * rows.b[idx] : pa;
        vals[j] = inside ? v : 0.0f;
    }
}

/* Parity of a tile index: the two tile slots alternate. */
__device__ inline unsigned ss2h_tile_slot(int chunk)
{
    return (unsigned)chunk & 1u;
}

/* One chunk's walk of the IIR over this lane's row: the steps n = first + k.
 * The left input lies in tile c - 1 or c, the right one is column k of tile
 * c; steps left of column 0 store nothing. */
__device__ inline void ss2h_row_walk(const Ss2hBlurArgs &args,
                                     float (*tile)[SS2H_ROW_TILE][SS2H_ROW_COLS + 1],
                                     float (*outs)[SS2H_ROW_COLS + 1], int chunk, int first,
                                     Ss2hIirState &st)
{
    constexpr int kCols = (int)SS2H_ROW_COLS;
    const unsigned lane = threadIdx.x;
    const int width = (int)args.width;
    const int radius = args.iir.radius;
    const float *right = tile[ss2h_tile_slot(chunk)][lane];
    for (int k = 0; k < kCols && first + k < width; k++) {
        const int n = first + k;
        const int left = n - radius - 1;
        const float lv =
            (left >= 0) ? tile[ss2h_tile_slot(left / kCols)][lane][left % kCols] : 0.0f;
        const float s = ss2h_iir_step(lv + right[k], st, args.iir);
        if (n >= 0) {
            outs[lane][k] = s;
        }
    }
}

/* This lane's share of the chunk's outputs, from LDS to the plane. */
__device__ inline void ss2h_row_store(const Ss2hRowPlane &rows, const Ss2hBlurArgs &args,
                                      const float (*outs)[SS2H_ROW_COLS + 1], int col)
{
    const unsigned lane = threadIdx.x;
    const unsigned my_col = lane % SS2H_ROW_COLS;
    const unsigned sub = lane / SS2H_ROW_COLS;
    if (col < 0 || col >= (int)args.width) {
        return;
    }
#pragma unroll
    for (unsigned j = 0; j < kTileLoads; j++) {
        const unsigned r = (j * kRowsPerLoad) + sub;
        if (rows.row0 + r < args.height) {
            rows.out[((size_t)(rows.row0 + r) * args.width) + (size_t)col] = outs[r][my_col];
        }
    }
}

/* ssimulacra2.c::fast_gaussian_1d along SS2H_ROW_TILE rows of one plane, one
 * lane per row (see ssimulacra2_blur_rows). */
template <bool kProduct>
__device__ inline void ss2h_blur_rows(const Ss2hBlurArgs &args,
                                      float (*tile)[SS2H_ROW_TILE][SS2H_ROW_COLS + 1],
                                      float (*outs)[SS2H_ROW_COLS + 1])
{
    constexpr int kCols = (int)SS2H_ROW_COLS;
    const unsigned lane = threadIdx.x;
    const unsigned my_col = lane % SS2H_ROW_COLS;
    const unsigned sub = lane / SS2H_ROW_COLS;
    const Ss2hRowPlane rows = ss2h_row_plane(args, blockIdx.y, blockIdx.x * SS2H_ROW_TILE);
    const int width = (int)args.width;
    const int lead = args.iir.radius - 1;

    float vals[kTileLoads];
    ss2h_row_tile_load<kProduct>(rows, args, 0, lane, vals);
#pragma unroll
    for (unsigned j = 0; j < kTileLoads; j++) {
        tile[0][(j * kRowsPerLoad) + sub][my_col] = vals[j];
        tile[1][(j * kRowsPerLoad) + sub][my_col] = 0.0f; /* tile -1: left of column 0 */
    }
    __syncwarp();

    Ss2hIirState st = {{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}};
    for (int chunk = 0; (chunk * kCols) - lead < width; chunk++) {
        const int first = (chunk * kCols) - lead;
        ss2h_row_tile_load<kProduct>(rows, args, chunk + 1, lane, vals);
        ss2h_row_walk(args, tile, outs, chunk, first, st);
        __syncwarp();
        ss2h_row_store(rows, args, outs, first + (int)my_col);
#pragma unroll
        for (unsigned j = 0; j < kTileLoads; j++) {
            tile[ss2h_tile_slot(chunk + 1)][(j * kRowsPerLoad) + sub][my_col] = vals[j];
        }
        __syncwarp();
    }
}

/* What every lane of ssimulacra2_ordered_totals does in one round: stage the
 * next batch of the plan and increments, or compute its run of terms of
 * chunk `what` into LDS (`k` selects the sum). */
__device__ inline void ss2h_totals_round(const Ss2hCombineArgs &a, unsigned todo, unsigned what,
                                         unsigned c, unsigned k, size_t first, unsigned lane,
                                         double *terms_of_chunk, short *staged_plan,
                                         long long *staged_units)
{
    if (todo == SS2H_WALK_LOAD) {
        ss2h_stage_batch(a.plan + first, a.units + first * 2u, a.chunks, what, lane, staged_plan,
                         staged_units);
        return;
    }
    for (unsigned j = 0; j < SS2H_CHUNK_RUN; j++) {
        double terms[SS2H_SUMS];
        ss2h_lane_terms(a, c, what, lane, j, terms);
        terms_of_chunk[lane * SS2H_CHUNK_RUN + j] = terms[k];
    }
}

/* The walk's state in lane 0 of ssimulacra2_ordered_totals, kept in LDS
 * words shared with the other lanes (command, operand) and in registers. */
struct Ss2hTotalsState {
    double sum;
    unsigned chunk;
    unsigned staged_batch;
    unsigned *command;
    unsigned *operand;
};

/* Lane 0, after a round: adds the computed chunk's terms in pixel order (the
 * CPU's loop) or notes the staged batch, then walks on. */
__device__ inline void ss2h_totals_walk(const Ss2hCombineArgs &a, unsigned todo, unsigned what,
                                        const double *terms_of_chunk, const short *staged_plan,
                                        const long long *staged_units, Ss2hTotalsState *state)
{
    if (todo == SS2H_WALK_TERMS) {
        for (size_t i = 0; i < kChunkPixels; i++) {
            state->sum += terms_of_chunk[i];
        }
        state->chunk = what + 1u;
    } else {
        state->staged_batch = what;
    }
    unsigned next = 0u;
    *state->command = ss2h_walk_batch(&state->sum, &state->chunk, a.chunks, state->staged_batch,
                                      staged_plan, staged_units, &next);
    *state->operand = next;
}

} // namespace

extern "C" {

/* 2D grid of SS2H_PIX_BX x SS2H_PIX_BY blocks over the full frame. */
__global__
__launch_bounds__(SS2H_PIX_BX *SS2H_PIX_BY) void ssimulacra2_yuv_to_linear(Ss2hYuvArgs args)
{
    const unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= args.width || y >= args.height) {
        return;
    }
    for (unsigned img = 0; img < SS2H_IMAGES; img++) {
        ss2h_yuv_pixel(args, img, x, y);
    }
}

/* 1D, one work-item per pixel of the current scale. */
__global__ __launch_bounds__(SS2H_ELEM_BLOCK) void ssimulacra2_xyb(Ss2hPlanesArgs args)
{
    const size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x;
    const size_t plane = (size_t)args.width * args.height;
    if (i >= plane) {
        return;
    }
    for (unsigned img = 0; img < SS2H_IMAGES; img++) {
        ss2h_xyb_pixel(args.in[img], args.out[img], i, plane);
    }
}

/* One wavefront per SS2H_ROW_TILE rows of one plane (grid: row tiles x 3
 * planes, SS2H_ROW_TILE lanes), one lane per row: ssimulacra2.c::
 * fast_gaussian_1d along every row. Chunk c walks the steps
 * n = c * SS2H_ROW_COLS - (radius - 1) + k, k < SS2H_ROW_COLS. Their right
 * input n + radius - 1 is column k of tile c; their left input
 * n - radius - 1 lies in tile c - 1 or c because the host keeps
 * 2 * radius <= SS2H_ROW_COLS, so two tile slots suffice. Tile c + 1 is
 * loaded into registers while tile c is walked. ssimulacra2_blur_rows blurs
 * `in`, ssimulacra2_blur_rows_product the product in * in2. */
__global__ __launch_bounds__(SS2H_ROW_TILE) void ssimulacra2_blur_rows(Ss2hBlurArgs args)
{
    __shared__ float tile[2][SS2H_ROW_TILE][SS2H_ROW_COLS + 1];
    __shared__ float outs[SS2H_ROW_TILE][SS2H_ROW_COLS + 1];
    ss2h_blur_rows<false>(args, tile, outs);
}

__global__ __launch_bounds__(SS2H_ROW_TILE) void ssimulacra2_blur_rows_product(Ss2hBlurArgs args)
{
    __shared__ float tile[2][SS2H_ROW_TILE][SS2H_ROW_COLS + 1];
    __shared__ float outs[SS2H_ROW_TILE][SS2H_ROW_COLS + 1];
    ss2h_blur_rows<true>(args, tile, outs);
}

/* One work-item per column of the three planes (3 * width lines). */
__global__ __launch_bounds__(SS2H_BLUR_BLOCK) void ssimulacra2_blur_cols(Ss2hBlurArgs args)
{
    const unsigned line = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned lines = args.width * SS2H_CHANNELS;
    if (line >= lines) {
        return;
    }
    const unsigned c = line / args.width;
    const unsigned l = line - c * args.width;
    const size_t base = (size_t)c * args.width * args.height + (size_t)l;
    ss2h_iir_line(args, base, (size_t)args.width, (int)args.height);
}

/* Advice for the plan: the six sums of each chunk, added in a tree.
 * blockIdx.y = channel, blockIdx.x = chunk, one work-group per chunk. */
__global__ __launch_bounds__(SS2H_REDUCE_BLOCK) void ssimulacra2_chunk_sums(Ss2hCombineArgs a)
{
    __shared__ double shared[SS2H_REDUCE_BLOCK * SS2H_SUMS];
    const unsigned c = blockIdx.y;
    const unsigned chunk = blockIdx.x;
    const unsigned lane = threadIdx.x;
    double sums[SS2H_SUMS] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    for (unsigned j = 0; j < SS2H_CHUNK_RUN; j++) {
        double terms[SS2H_SUMS];
        ss2h_lane_terms(a, c, chunk, lane, j, terms);
        for (unsigned k = 0; k < SS2H_SUMS; k++) {
            sums[k] += terms[k];
        }
    }
    ss2h_block_tree(shared, lane, sums);
    if (lane == 0u) {
        double *out = a.chunk_sums + ((size_t)c * a.chunks + chunk) * SS2H_SUMS;
        for (unsigned k = 0; k < SS2H_SUMS; k++) {
            out[k] = shared[k];
        }
    }
}

/* The plan of every chunk of one (channel, sum): blockIdx.y = channel,
 * blockIdx.x = sum. The lanes stage a batch of chunk sums, lane 0 follows
 * their prefix, the lanes store the batch's plan. */
__global__ __launch_bounds__(SS2H_REDUCE_BLOCK) void ssimulacra2_chunk_plan(Ss2hCombineArgs a)
{
    __shared__ double staged_sums[SS2H_BATCH];
    __shared__ short staged_plan[SS2H_BATCH];
    const unsigned c = blockIdx.y;
    const unsigned k = blockIdx.x;
    const unsigned lane = threadIdx.x;
    const double *sums = a.chunk_sums + (size_t)c * a.chunks * SS2H_SUMS + k;
    int16_t *plan = a.plan + ((size_t)c * SS2H_SUMS + k) * a.chunks;
    const unsigned batches = (a.chunks + SS2H_BATCH - 1u) / SS2H_BATCH;
    double prefix = 0.0;
    for (unsigned batch = 0; batch < batches; batch++) {
        ss2h_stage_chunk_sums(sums, a.chunks, batch, lane, staged_sums);
        __syncthreads();
        if (lane == 0u) {
            for (unsigned i = 0; i < SS2H_BATCH; i++) {
                staged_plan[i] = (short)vmaf_ordsum_plan(&prefix, staged_sums[i]);
            }
        }
        __syncthreads();
        for (unsigned j = 0; j < SS2H_BATCH_RUN; j++) {
            const unsigned slot = lane * SS2H_BATCH_RUN + j;
            const size_t chunk = (size_t)batch * kBatchChunks + slot;
            if (chunk < a.chunks) {
                plan[chunk] = (int16_t)staged_plan[slot];
            }
        }
    }
}

/* Each chunk's increment under its plan, composed in pixel order: a lane
 * composes its run, then the lanes are composed in order.
 * blockIdx.y = channel, blockIdx.x = chunk. */
__global__ __launch_bounds__(SS2H_REDUCE_BLOCK) void ssimulacra2_chunk_units(Ss2hCombineArgs a)
{
    __shared__ long long shared[SS2H_REDUCE_BLOCK * SS2H_SUMS * 2];
    const unsigned c = blockIdx.y;
    const unsigned chunk = blockIdx.x;
    const unsigned lane = threadIdx.x;
    const size_t first = ((size_t)c * SS2H_SUMS) * a.chunks + chunk;
    VmafOrdsumUnits units[SS2H_SUMS];
    for (VmafOrdsumUnits &unit : units) {
        unit = vmaf_ordsum_units(0, 0);
    }
    for (unsigned j = 0; j < SS2H_CHUNK_RUN; j++) {
        double terms[SS2H_SUMS];
        ss2h_lane_terms(a, c, chunk, lane, j, terms);
        for (unsigned k = 0; k < SS2H_SUMS; k++) {
            const int plan = (int)a.plan[first + (size_t)k * a.chunks];
            units[k] = vmaf_ordsum_then(units[k], vmaf_ordsum_planned_term(terms[k], plan));
        }
    }
    for (unsigned k = 0; k < SS2H_SUMS; k++) {
        ss2h_units_store(shared, lane, k, units[k]);
    }
    __syncthreads();
    ss2h_ordered_tree(shared, lane);
    if (lane == 0u) {
        for (unsigned k = 0; k < SS2H_SUMS; k++) {
            int64_t *out = a.units + (first + (size_t)k * a.chunks) * 2u;
            out[0] = (int64_t)shared[ss2h_units_slot(0u, k)];
            out[1] = (int64_t)shared[ss2h_units_slot(0u, k) + 1u];
        }
    }
}

/* The CPU's sum of one term over one channel: blockIdx.y = channel,
 * blockIdx.x = sum. Lane 0 walks the chunks, a staged batch at a time. When a
 * chunk needs its terms, every lane computes its run of them into LDS and
 * lane 0 adds them in pixel order, as the CPU's loop does. Every round stages
 * a batch or consumes a chunk, which bounds the loop. */
__global__ __launch_bounds__(SS2H_REDUCE_BLOCK) void ssimulacra2_ordered_totals(Ss2hCombineArgs a)
{
    __shared__ double terms_of_chunk[SS2H_CHUNK_PIXELS];
    __shared__ long long staged_units[SS2H_BATCH * 2];
    __shared__ short staged_plan[SS2H_BATCH];
    __shared__ unsigned command;
    __shared__ unsigned operand;
    const unsigned c = blockIdx.y;
    const unsigned k = blockIdx.x;
    const unsigned lane = threadIdx.x;
    const size_t first = ((size_t)c * SS2H_SUMS + k) * a.chunks;
    const unsigned rounds = a.chunks + a.chunks / SS2H_BATCH + 2u;
    Ss2hTotalsState state = {0.0, 0u, 0u, &command, &operand};
    if (lane == 0u) {
        command = SS2H_WALK_LOAD;
        operand = 0u;
    }
    for (unsigned round = 0; round < rounds; round++) {
        __syncthreads();
        const unsigned todo = command;
        const unsigned what = operand;
        if (todo == SS2H_WALK_DONE) {
            break;
        }
        ss2h_totals_round(a, todo, what, c, k, first, lane, terms_of_chunk, staged_plan,
                          staged_units);
        __syncthreads();
        if (lane == 0u) {
            ss2h_totals_walk(a, todo, what, terms_of_chunk, staged_plan, staged_units, &state);
        }
    }
    if (lane == 0u) {
        a.totals[(size_t)c * SS2H_SUMS + k] = state.sum;
    }
}

/* 2D grid over the next scale; both images, three planes each. */
__global__
__launch_bounds__(SS2H_PIX_BX *SS2H_PIX_BY) void ssimulacra2_downsample(Ss2hPlanesArgs args)
{
    const unsigned ox = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned oy = blockIdx.y * blockDim.y + threadIdx.y;
    if (ox >= args.out_width || oy >= args.out_height) {
        return;
    }
    for (unsigned img = 0; img < SS2H_IMAGES; img++) {
        ss2h_down_pixel(args.in[img], args.out[img], args, ox, oy);
    }
}

} /* extern "C" */
