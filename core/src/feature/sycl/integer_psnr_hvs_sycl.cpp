/**
 *  Copyright 2001-2012 Xiph.Org and contributors.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent AND BSD-2-Clause
 *
 *  psnr_hvs feature extractor on the SYCL backend
 *  (T7-23 / ADR-0188 / ADR-0191, GPU long-tail batch 2 part 3c).
 *  SYCL twin of psnr_hvs_cuda and psnr_hvs_hip.
 *
 *  Self-contained submit/collect on the primary queue. Reads the
 *  frame's samples where the SYCL state already holds them: luma from
 *  the shared frame, Cb / Cr from the opt-in shared chroma planes, both
 *  uploaded once per frame for every twin (ADR-1369). No host
 *  conversion and no private upload.
 *
 *  One dispatch for all active planes, two work-items per 8x8 block
 *  (step 7): one for the reference, one for the distorted image. Each
 *  stages its 64 samples in local memory, takes the variance ratio,
 *  runs the integer 8x8 DCT in place and sums its masking energy, all
 *  in calc_psnrhvs()'s i, j order; the reference work-item then scores
 *  the block from both coefficient sets. Per-block float arithmetic is
 *  the previous kernel's expression for expression, so block scores
 *  and the host's block-order plane sums are bit-identical to it.
 *  The DCT must stay in local memory, never in a work-item's private
 *  arrays: that footprint crashed the Xe2 GPU compiler at SIMD32
 *  (T-SYCL-PSNR-HVS-B580-SIGSEGV-2026-09-29).
 *
 *  fp64-free (Intel Arc A380 lacks native fp64 — same constraint
 *  as ssim_sycl / ms_ssim_sycl).
 */

#include <sycl/sycl.hpp>

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <utility>

#include "config.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "log.h"
#include "picture.h"
#include "sycl/common.h"

namespace
{

static constexpr int PSNR_HVS_BLOCK = 8;
static constexpr int PSNR_HVS_STEP = 7;
static constexpr int PSNR_HVS_NUM_PLANES = 3;
static constexpr size_t WG_DIM = 8;
static constexpr size_t BLOCK_AREA = WG_DIM * WG_DIM;

static constexpr float CSF_TABLES[3][64] = {
    /* Y */
    {1.6193873005f,   2.2901594831f,   2.08509755623f,  1.48366094411f,  1.00227514334f,
     0.678296995242f, 0.466224900598f, 0.3265091542f,   2.2901594831f,   1.94321815382f,
     2.04793073064f,  1.68731108984f,  1.2305666963f,   0.868920337363f, 0.61280991668f,
     0.436405793551f, 2.08509755623f,  2.04793073064f,  1.34329019223f,  1.09205635862f,
     0.875748795257f, 0.670882927016f, 0.501731932449f, 0.372504254596f, 1.48366094411f,
     1.68731108984f,  1.09205635862f,  0.772819797575f, 0.605636379554f, 0.48309405692f,
     0.380429446972f, 0.295774038565f, 1.00227514334f,  1.2305666963f,   0.875748795257f,
     0.605636379554f, 0.448996256676f, 0.352889268808f, 0.283006984131f, 0.226951348204f,
     0.678296995242f, 0.868920337363f, 0.670882927016f, 0.48309405692f,  0.352889268808f,
     0.27032073436f,  0.215017739696f, 0.17408067321f,  0.466224900598f, 0.61280991668f,
     0.501731932449f, 0.380429446972f, 0.283006984131f, 0.215017739696f, 0.168869545842f,
     0.136153931001f, 0.3265091542f,   0.436405793551f, 0.372504254596f, 0.295774038565f,
     0.226951348204f, 0.17408067321f,  0.136153931001f, 0.109083846276f},
    /* Cb */
    {1.91113096927f,  2.46074210438f,  1.18284184739f,  1.14982565193f,  1.05017074788f,
     0.898018824055f, 0.74725392039f,  0.615105596242f, 2.46074210438f,  1.58529308355f,
     1.21363250036f,  1.38190029285f,  1.33100189972f,  1.17428548929f,  0.996404342439f,
     0.830890433625f, 1.18284184739f,  1.21363250036f,  0.978712413627f, 1.02624506078f,
     1.03145147362f,  0.960060382087f, 0.849823426169f, 0.731221236837f, 1.14982565193f,
     1.38190029285f,  1.02624506078f,  0.861317501629f, 0.801821139099f, 0.751437590932f,
     0.685398513368f, 0.608694761374f, 1.05017074788f,  1.33100189972f,  1.03145147362f,
     0.801821139099f, 0.676555426187f, 0.605503172737f, 0.55002013668f,  0.495804539034f,
     0.898018824055f, 1.17428548929f,  0.960060382087f, 0.751437590932f, 0.605503172737f,
     0.514674450957f, 0.454353482512f, 0.407050308965f, 0.74725392039f,  0.996404342439f,
     0.849823426169f, 0.685398513368f, 0.55002013668f,  0.454353482512f, 0.389234902883f,
     0.342353999733f, 0.615105596242f, 0.830890433625f, 0.731221236837f, 0.608694761374f,
     0.495804539034f, 0.407050308965f, 0.342353999733f, 0.295530605237f},
    /* Cr */
    {2.03871978502f,  2.62502345193f,  1.26180942886f,  1.11019789803f,  1.01397751469f,
     0.867069376285f, 0.721500455585f, 0.593906509971f, 2.62502345193f,  1.69112867013f,
     1.17180569821f,  1.3342742857f,   1.28513006198f,  1.13381474809f,  0.962064122248f,
     0.802254508198f, 1.26180942886f,  1.17180569821f,  0.944981930573f, 0.990876405848f,
     0.995903384143f, 0.926972725286f, 0.820534991409f, 0.706020324706f, 1.11019789803f,
     1.3342742857f,   0.990876405848f, 0.831632933426f, 0.77418706195f,  0.725539939514f,
     0.661776842059f, 0.587716619023f, 1.01397751469f,  1.28513006198f,  0.995903384143f,
     0.77418706195f,  0.653238524286f, 0.584635025748f, 0.531064164893f, 0.478717061273f,
     0.867069376285f, 1.13381474809f,  0.926972725286f, 0.725539939514f, 0.584635025748f,
     0.496936637883f, 0.438694579826f, 0.393021669543f, 0.721500455585f, 0.962064122248f,
     0.820534991409f, 0.661776842059f, 0.531064164893f, 0.438694579826f, 0.375820256136f,
     0.330555063063f, 0.593906509971f, 0.802254508198f, 0.706020324706f, 0.587716619023f,
     0.478717061273f, 0.393021669543f, 0.330555063063f, 0.285345396658f}};

} // namespace

namespace
{

/* Work-items per work-group: 32 blocks, a reference and a distorted
 * work-item each. */
static constexpr size_t HVS_WG = 64;
/* Local-memory words per work-item: one 8x8 block plus one pad word, so
 * the work-items of a sub-group index distinct banks for the same
 * coefficient. */
static constexpr size_t HVS_LANE_STRIDE = BLOCK_AREA + 1;

struct PsnrHvsStateSycl {
    unsigned width[PSNR_HVS_NUM_PLANES];
    unsigned height[PSNR_HVS_NUM_PLANES];
    unsigned num_blocks_x[PSNR_HVS_NUM_PLANES];
    unsigned num_blocks_y[PSNR_HVS_NUM_PLANES];
    unsigned num_blocks[PSNR_HVS_NUM_PLANES];
    /* Offset of each plane's blocks in the one partials buffer. */
    unsigned first_block[PSNR_HVS_NUM_PLANES];
    unsigned total_blocks;
    unsigned bpc;
    int32_t samplemax_sq;
    /* enable_chroma: when false, only the luma (Y) plane is dispatched.
     * Default true mirrors CPU integer_psnr_hvs — see ADR-0453. */
    bool enable_chroma;
    /* n_active_planes: 1 when enable_chroma=false or YUV400P, else 3. */
    unsigned n_active_planes;

    VmafSyclState *sycl_state;

    /* Device block scores of every active plane, and their host copy. */
    float *d_partials;
    float *h_partials;

    bool has_pending;
    unsigned pending_index;
    VmafDictionary *feature_name_dict;
};

/* One plane as the kernel sees it: packed samples of both images. */
struct PsnrHvsPlaneArgs {
    const void *ref;
    const void *dist;
    unsigned width;
    unsigned blocks_x;
    unsigned first_block;
};

struct PsnrHvsKernelArgs {
    PsnrHvsPlaneArgs plane[PSNR_HVS_NUM_PLANES];
    float *partials;
    unsigned n_planes;
    unsigned total_blocks;
    bool wide; /* 16-bit samples (bpc > 8) */
};

} // namespace

namespace
{

/* OD_UNBIASED_RSHIFT32 — round-to-zero right shift. */
static inline int od_dct_rshift(int a, int b)
{
    return (int)(((unsigned int)a >> (32 - b)) + (unsigned int)a) >> b;
}

} // namespace

namespace
{

static void od_bin_fdct8(int &y0, int &y1, int &y2, int &y3, int &y4, int &y5, int &y6, int &y7,
                         int x0, int x1, int x2, int x3, int x4, int x5, int x6, int x7)
{
    int t0 = x0;
    int t4 = x1;
    int t2 = x2;
    int t6 = x3;
    int t7 = x4;
    int t3 = x5;
    int t5 = x6;
    int t1 = x7;
    t1 = t0 - t1;
    const int t1h = od_dct_rshift(t1, 1);
    t0 -= t1h;
    t4 += t5;
    const int t4h = od_dct_rshift(t4, 1);
    t5 -= t4h;
    t3 = t2 - t3;
    t2 -= od_dct_rshift(t3, 1);
    t6 += t7;
    const int t6h = od_dct_rshift(t6, 1);
    t7 = t6h - t7;
    t0 += t6h;
    t6 = t0 - t6;
    t2 = t4h - t2;
    t4 = t2 - t4;
    t0 -= (t4 * 13573 + 16384) >> 15;
    t4 += (t0 * 11585 + 8192) >> 14;
    t0 -= (t4 * 13573 + 16384) >> 15;
    t6 -= (t2 * 21895 + 16384) >> 15;
    t2 += (t6 * 15137 + 8192) >> 14;
    t6 -= (t2 * 21895 + 16384) >> 15;
    t3 += (t5 * 19195 + 16384) >> 15;
    t5 += (t3 * 11585 + 8192) >> 14;
    t3 -= (t5 * 7489 + 4096) >> 13;
    t7 = od_dct_rshift(t5, 1) - t7;
    t5 -= t7;
    t3 = t1h - t3;
    t1 -= t3;
    t7 += (t1 * 3227 + 16384) >> 15;
    t1 -= (t7 * 6393 + 16384) >> 15;
    t7 += (t1 * 3227 + 16384) >> 15;
    t5 += (t3 * 2485 + 4096) >> 13;
    t3 -= (t5 * 18205 + 16384) >> 15;
    t5 += (t3 * 2485 + 4096) >> 13;
    y0 = t0;
    y1 = t1;
    y2 = t2;
    y3 = t3;
    y4 = t4;
    y5 = t5;
    y6 = t6;
    y7 = t7;
}

} // namespace

namespace
{

using HvsLocal = sycl::local_accessor<int, 1>;

/* Pass 1 of the CPU's od_bin_fdct8x8() for column `i`, written back over
 * column i. The CPU stores that transform as row i of its scratch z, so
 * after the eight columns the block holds z transposed. */
static void hvs_fdct8_column(const HvsLocal &slm, size_t base, size_t i)
{
    int y0;
    int y1;
    int y2;
    int y3;
    int y4;
    int y5;
    int y6;
    int y7;
    od_bin_fdct8(y0, y1, y2, y3, y4, y5, y6, y7, slm[base + i], slm[base + WG_DIM + i],
                 slm[base + (2 * WG_DIM) + i], slm[base + (3 * WG_DIM) + i],
                 slm[base + (4 * WG_DIM) + i], slm[base + (5 * WG_DIM) + i],
                 slm[base + (6 * WG_DIM) + i], slm[base + (7 * WG_DIM) + i]);
    slm[base + i] = y0;
    slm[base + WG_DIM + i] = y1;
    slm[base + (2 * WG_DIM) + i] = y2;
    slm[base + (3 * WG_DIM) + i] = y3;
    slm[base + (4 * WG_DIM) + i] = y4;
    slm[base + (5 * WG_DIM) + i] = y5;
    slm[base + (6 * WG_DIM) + i] = y6;
    slm[base + (7 * WG_DIM) + i] = y7;
}

/* Pass 2 for index `i`: the CPU transforms column i of z into row i of the
 * output. Column i of z is row i of the transposed block, so the transform
 * reads row i and writes it back in place. Integer arithmetic, so neither
 * the in-place layout nor the order changes a coefficient. */
static void hvs_fdct8_row(const HvsLocal &slm, size_t base, size_t i)
{
    const size_t row = base + (i * WG_DIM);
    int y0;
    int y1;
    int y2;
    int y3;
    int y4;
    int y5;
    int y6;
    int y7;
    od_bin_fdct8(y0, y1, y2, y3, y4, y5, y6, y7, slm[row], slm[row + 1], slm[row + 2], slm[row + 3],
                 slm[row + 4], slm[row + 5], slm[row + 6], slm[row + 7]);
    slm[row + 0] = y0;
    slm[row + 1] = y1;
    slm[row + 2] = y2;
    slm[row + 3] = y3;
    slm[row + 4] = y4;
    slm[row + 5] = y5;
    slm[row + 6] = y6;
    slm[row + 7] = y7;
}

} // namespace

namespace
{

/* od_bin_fdct8x8() of the block at `base`, in place in local memory. */
static void hvs_fdct8x8(const HvsLocal &slm, size_t base)
{
    for (size_t i = 0; i < WG_DIM; i++) {
        hvs_fdct8_column(slm, base, i);
    }
    for (size_t i = 0; i < WG_DIM; i++) {
        hvs_fdct8_row(slm, base, i);
    }
}

/* This work-item's block: its plane, the image it reads, and where. */
struct HvsLaneBlock {
    const void *src;
    size_t origin;
    unsigned width;
    unsigned block;
    int plane;
};

/* One value of the plane's geometry, picked among values the caller read with
 * constant indices. Never index args.plane[] with the run-time plane: the
 * compiler then keeps the whole kernel-argument struct in private memory,
 * which is scratch (152 bytes per work-item on DG2), and scratch returns
 * wrong values on the xe driver (T-SYCL-PSNR-HVS-XE-SCRATCH-2026-09-30). */
template <typename T> static inline T hvs_pick(int plane, T luma, T cb, T cr)
{
    if (plane == 2) {
        return cr;
    }
    return (plane == 1) ? cb : luma;
}

static inline HvsLaneBlock hvs_locate(const PsnrHvsKernelArgs &args, unsigned block, bool is_dist)
{
    const PsnrHvsPlaneArgs &y = args.plane[0];
    const PsnrHvsPlaneArgs &cb = args.plane[1];
    const PsnrHvsPlaneArgs &cr = args.plane[2];
    int plane = 0;
    if (1U < args.n_planes && block >= cb.first_block) {
        plane = 1;
    }
    if (2U < args.n_planes && block >= cr.first_block) {
        plane = 2;
    }
    const unsigned first_block = hvs_pick(plane, y.first_block, cb.first_block, cr.first_block);
    const unsigned blocks_x = hvs_pick(plane, y.blocks_x, cb.blocks_x, cr.blocks_x);
    const unsigned width = hvs_pick(plane, y.width, cb.width, cr.width);
    const void *src = is_dist ? hvs_pick(plane, y.dist, cb.dist, cr.dist) :
                                hvs_pick(plane, y.ref, cb.ref, cr.ref);
    const unsigned in_plane = block - first_block;
    const size_t origin_x = (size_t)(in_plane % blocks_x) * PSNR_HVS_STEP;
    const size_t origin_y = (size_t)(in_plane / blocks_x) * PSNR_HVS_STEP;
    return {.src = src,
            .origin = (origin_y * width) + origin_x,
            .width = width,
            .block = block,
            .plane = plane};
}

/* The CPU reads the raw integer samples (calc_psnrhvs()), so do we. */
static inline void hvs_load_block(const HvsLocal &slm, size_t base, const HvsLaneBlock &lane,
                                  bool wide)
{
    for (size_t row = 0; row < WG_DIM; row++) {
        const size_t source = lane.origin + (row * lane.width);
        for (size_t col = 0; col < WG_DIM; col++) {
            slm[base + (row * WG_DIM) + col] =
                wide ? (int)static_cast<const uint16_t *>(lane.src)[source + col] :
                       (int)static_cast<const uint8_t *>(lane.src)[source + col];
        }
    }
}

} // namespace

namespace
{

/* One accumulator per 4x4 quadrant of the block, numbered as calc_psnrhvs()
 * numbers its sub-blocks: ((i & 12) >> 2) + ((j & 12) >> 1), so 0 = rows
 * 0-3 x columns 0-3, 1 = rows 4-7 x columns 0-3, 2 = rows 0-3 x columns 4-7,
 * 3 = rows 4-7 x columns 4-7. Named members, not a float[4]: an array indexed
 * by a run-time quadrant number lives in private memory, which is scratch. */
struct HvsQuadrants {
    float q0;
    float q1;
    float q2;
    float q3;
};

/* Adds row `row`'s samples, in column order, to the global sum and to the
 * sums of the quadrants holding its left and right half. */
static inline void hvs_row_sums(const HvsLocal &block, size_t base, size_t row, float &global,
                                float &left, float &right)
{
    const size_t first = base + (row * WG_DIM);
    for (size_t col = 0; col < WG_DIM / 2U; col++) {
        const float sample = (float)block[first + col];
        global += sample;
        left += sample;
    }
    for (size_t col = WG_DIM / 2U; col < WG_DIM; col++) {
        const float sample = (float)block[first + col];
        global += sample;
        right += sample;
    }
}

/* The same walk for the squared deviations from the global mean and from the
 * means of the row's two quadrants. */
static inline void hvs_row_squares(const HvsLocal &block, size_t base, size_t row,
                                   float global_mean, float left_mean, float right_mean,
                                   float &global, float &left, float &right)
{
    const size_t first = base + (row * WG_DIM);
    for (size_t col = 0; col < WG_DIM / 2U; col++) {
        const float global_delta = (float)block[first + col] - global_mean;
        const float quadrant_delta = (float)block[first + col] - left_mean;
        global += global_delta * global_delta;
        left += quadrant_delta * quadrant_delta;
    }
    for (size_t col = WG_DIM / 2U; col < WG_DIM; col++) {
        const float global_delta = (float)block[first + col] - global_mean;
        const float quadrant_delta = (float)block[first + col] - right_mean;
        global += global_delta * global_delta;
        right += quadrant_delta * quadrant_delta;
    }
}

/* calc_psnrhvs()'s i, j walk over the block: each accumulator receives its
 * samples in the CPU's order. */
static inline void hvs_sample_sums(const HvsLocal &block, size_t base, float &global,
                                   HvsQuadrants &sums)
{
    for (size_t row = 0; row < WG_DIM / 2U; row++) {
        hvs_row_sums(block, base, row, global, sums.q0, sums.q2);
    }
    for (size_t row = WG_DIM / 2U; row < WG_DIM; row++) {
        hvs_row_sums(block, base, row, global, sums.q1, sums.q3);
    }
}

static inline void hvs_square_sums(const HvsLocal &block, size_t base, float global_mean,
                                   const HvsQuadrants &means, float &global, HvsQuadrants &squares)
{
    for (size_t row = 0; row < WG_DIM / 2U; row++) {
        hvs_row_squares(block, base, row, global_mean, means.q0, means.q2, global, squares.q0,
                        squares.q2);
    }
    for (size_t row = WG_DIM / 2U; row < WG_DIM; row++) {
        hvs_row_squares(block, base, row, global_mean, means.q1, means.q3, global, squares.q1,
                        squares.q3);
    }
}

} // namespace

namespace
{

/* Variance ratio of the untransformed samples, as calc_psnrhvs() takes it
 * before its DCT. Summation order is the CPU's, unchanged. */
static inline float hvs_variance_ratio(const HvsLocal &block, size_t base)
{
    HvsQuadrants means = {.q0 = 0.f, .q1 = 0.f, .q2 = 0.f, .q3 = 0.f};
    float global_mean = 0.f;
    hvs_sample_sums(block, base, global_mean, means);
    global_mean /= 64.f;
    means.q0 /= 16.f;
    means.q1 /= 16.f;
    means.q2 /= 16.f;
    means.q3 /= 16.f;
    HvsQuadrants variances = {.q0 = 0.f, .q1 = 0.f, .q2 = 0.f, .q3 = 0.f};
    float global_variance = 0.f;
    hvs_square_sums(block, base, global_mean, means, global_variance, variances);
    global_variance *= 1.f / 63.f * 64.f;
    variances.q0 *= 1.f / 15.f * 16.f;
    variances.q1 *= 1.f / 15.f * 16.f;
    variances.q2 *= 1.f / 15.f * 16.f;
    variances.q3 *= 1.f / 15.f * 16.f;
    if (global_variance > 0.f) {
        global_variance =
            (variances.q0 + variances.q1 + variances.q2 + variances.q3) / global_variance;
    }
    return global_variance;
}

} // namespace

namespace
{

/* mask[i][j] of calc_psnrhvs(), recomputed per coefficient instead of held in
 * a private float[64]; the same two float multiplies, so the same value. */
static inline float hvs_mask_at(int plane, int index)
{
    const float scaled = CSF_TABLES[plane][index] * 0.3885746225901003f;
    return scaled * scaled;
}

static inline float hvs_mask_energy(const HvsLocal &block, size_t base, int plane)
{
    float energy = 0.f;
    for (int row = 0; row < 8; row++) {
        const int first_col = (row == 0) ? 1 : 0;
        for (int col = first_col; col < 8; col++) {
            const int index = (row * 8) + col;
            const int coefficient = block[base + index];
            energy += (float)(coefficient * coefficient) * hvs_mask_at(plane, index);
        }
    }
    return energy;
}

static inline float hvs_error(const HvsLocal &block, size_t ref_base, size_t dist_base,
                              float threshold, int plane)
{
    float error_sum = 0.f;
    for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 8; col++) {
            const int index = (row * 8) + col;
            const float csf = CSF_TABLES[plane][index];
            float error =
                sycl::fabs((float)block[ref_base + index] - (float)block[dist_base + index]);
            if (row != 0 || col != 0) {
                const float masking = threshold / hvs_mask_at(plane, index);
                error = error < masking ? 0.f : error - masking;
            }
            error_sum += (error * csf) * (error * csf);
        }
    }
    return error_sum;
}

} // namespace

namespace
{

/* Variance ratio and masking energy of one image of a block. */
struct HvsImageStats {
    float ratio;
    float energy;
};

/* Runs on the reference work-item; the distorted coefficients sit one
 * work-item stride further on. */
static inline float score_hvs_block(const HvsLocal &block, size_t ref_base, HvsImageStats ref,
                                    HvsImageStats dist, int plane)
{
    float threshold = sycl::sqrt(ref.energy * ref.ratio) / 32.f;
    const float dist_threshold = sycl::sqrt(dist.energy * dist.ratio) / 32.f;
    if (dist_threshold > threshold) {
        threshold = dist_threshold;
    }
    return hvs_error(block, ref_base, ref_base + HVS_LANE_STRIDE, threshold, plane);
}

/* Work-item 2k takes block k's reference image, 2k + 1 its distorted one;
 * both sit in one sub-group, which exchanges their statistics. Work-items
 * past the last block run on block 0 so the sub-group stays converged,
 * and never store. */
static void psnr_hvs_item(sycl::nd_item<1> item, const HvsLocal &block,
                          const PsnrHvsKernelArgs &args)
{
    const size_t id = item.get_global_id(0);
    const bool active = id < 2U * (size_t)args.total_blocks;
    const bool is_dist = (id & 1U) != 0U;
    const HvsLaneBlock lane = hvs_locate(args, active ? (unsigned)(id >> 1) : 0U, is_dist);
    const size_t base = item.get_local_id(0) * HVS_LANE_STRIDE;
    hvs_load_block(block, base, lane, args.wide);
    const float ratio = hvs_variance_ratio(block, base);
    hvs_fdct8x8(block, base);
    const HvsImageStats mine = {.ratio = ratio, .energy = hvs_mask_energy(block, base, lane.plane)};
    const sycl::sub_group group = item.get_sub_group();
    const HvsImageStats partner = {.ratio = sycl::permute_group_by_xor(group, mine.ratio, 1U),
                                   .energy = sycl::permute_group_by_xor(group, mine.energy, 1U)};
    sycl::group_barrier(group);
    if (active && !is_dist) {
        args.partials[lane.block] = score_hvs_block(block, base, mine, partner, lane.plane);
    }
}

} // namespace

namespace
{

static void launch_psnr_hvs(sycl::queue &q, const PsnrHvsKernelArgs &args)
{
    const size_t items = 2U * (size_t)args.total_blocks;
    const size_t global = (items + HVS_WG - 1U) / HVS_WG * HVS_WG;
    const sycl::nd_range<1> ndr{sycl::range<1>{global}, sycl::range<1>{HVS_WG}};
    q.submit([&](sycl::handler &h) {
        const HvsLocal s_block(sycl::range<1>(HVS_WG * HVS_LANE_STRIDE), h);
        const PsnrHvsKernelArgs kernel_args = args;
        h.parallel_for(ndr,
                       [=](sycl::nd_item<1> item) { psnr_hvs_item(item, s_block, kernel_args); });
    });
}

} // namespace

namespace
{

static const VmafOption options_psnr_hvs_sycl[] = {
    {
        .name = "enable_chroma",
        .help = "enable psnr_hvs calculation for chroma channels (Cb and Cr); "
                "when false only the luma plane is scored and psnr_hvs equals "
                "psnr_hvs_y (mirrors CPU PR #946 / ADR-0453)",
        .offset = offsetof(PsnrHvsStateSycl, enable_chroma),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val = {.b = true},
    },
    {.name = nullptr},
};

} // namespace

namespace
{

static int validate_hvs_input(enum VmafPixelFormat format, unsigned bpc, unsigned width,
                              unsigned height)
{
    if (bpc > 12) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "psnr_hvs_sycl: invalid bitdepth (%u); bpc must be ≤ 12\n",
                 bpc);
        return -EINVAL;
    }
    if (format == VMAF_PIX_FMT_YUV400P) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "psnr_hvs_sycl: YUV400P unsupported (psnr_hvs needs all 3 planes)\n");
        return -EINVAL;
    }
    if (width < (unsigned)PSNR_HVS_BLOCK || height < (unsigned)PSNR_HVS_BLOCK) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "psnr_hvs_sycl: input %ux%u smaller than 8×8 block\n", width,
                 height);
        return -EINVAL;
    }
    return 0;
}

} // namespace

namespace
{

static int configure_hvs_geometry(PsnrHvsStateSycl *s, enum VmafPixelFormat format, unsigned width,
                                  unsigned height)
{
    s->width[0] = width;
    s->height[0] = height;
    switch (format) {
    case VMAF_PIX_FMT_YUV420P:
        s->width[1] = s->width[2] = (width + 1u) >> 1;
        s->height[1] = s->height[2] = (height + 1u) >> 1;
        break;
    case VMAF_PIX_FMT_YUV422P:
        s->width[1] = s->width[2] = (width + 1u) >> 1;
        s->height[1] = s->height[2] = height;
        break;
    case VMAF_PIX_FMT_YUV444P:
        s->width[1] = s->width[2] = width;
        s->height[1] = s->height[2] = height;
        break;
    default:
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "psnr_hvs_sycl: unsupported pix_fmt\n");
        return -EINVAL;
    }
    return 0;
}

} // namespace

namespace
{

static int configure_hvs_blocks(PsnrHvsStateSycl *s)
{
    s->n_active_planes = s->enable_chroma ? (unsigned)PSNR_HVS_NUM_PLANES : 1U;
    s->total_blocks = 0U;
    for (int plane = 0; std::cmp_less(plane, s->n_active_planes); plane++) {
        if (s->width[plane] < (unsigned)PSNR_HVS_BLOCK ||
            s->height[plane] < (unsigned)PSNR_HVS_BLOCK) {
            vmaf_log(VMAF_LOG_LEVEL_ERROR,
                     "psnr_hvs_sycl: plane %d dims %ux%u smaller than 8×8 block\n", plane,
                     s->width[plane], s->height[plane]);
            return -EINVAL;
        }
        s->num_blocks_x[plane] = (s->width[plane] - PSNR_HVS_BLOCK) / PSNR_HVS_STEP + 1;
        s->num_blocks_y[plane] = (s->height[plane] - PSNR_HVS_BLOCK) / PSNR_HVS_STEP + 1;
        s->num_blocks[plane] = s->num_blocks_x[plane] * s->num_blocks_y[plane];
        s->first_block[plane] = s->total_blocks;
        s->total_blocks += s->num_blocks[plane];
    }
    return 0;
}

} // namespace

namespace
{

/* Block scores only: the samples come from the state's shared planes. */
static int allocate_hvs_buffers(PsnrHvsStateSycl *s)
{
    const size_t partials_bytes = (size_t)s->total_blocks * sizeof(float);
    s->d_partials = static_cast<float *>(vmaf_sycl_malloc_device(s->sycl_state, partials_bytes));
    s->h_partials = static_cast<float *>(vmaf_sycl_malloc_host(s->sycl_state, partials_bytes));
    if (!s->d_partials || !s->h_partials) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "psnr_hvs_sycl: USM allocation failed\n");
        return -ENOMEM;
    }
    return 0;
}

/* Luma from the shared frame, Cb / Cr from the shared chroma planes. Both
 * calls are idempotent: every twin that asks shares one upload. */
static int attach_shared_planes(PsnrHvsStateSycl *s)
{
    int err = vmaf_sycl_shared_frame_init(s->sycl_state, s->width[0], s->height[0], s->bpc);
    if (!err && s->n_active_planes > 1U) {
        err = vmaf_sycl_shared_chroma_init(s->sycl_state, s->width[1], s->height[1]);
    }
    if (err) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "psnr_hvs_sycl: shared planes unavailable (%d)\n", err);
    }
    return err;
}

} // namespace

namespace
{

static int close_fex_sycl(VmafFeatureExtractor *fex);

static int init_fex_sycl(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                         unsigned w, unsigned h)
{
    auto *s = static_cast<PsnrHvsStateSycl *>(fex->priv);

    const int input_err = validate_hvs_input(pix_fmt, bpc, w, h);
    if (input_err) {
        return input_err;
    }

    s->bpc = bpc;
    const int32_t samplemax = (1 << bpc) - 1;
    s->samplemax_sq = samplemax * samplemax;

    const int geometry_err = configure_hvs_geometry(s, pix_fmt, w, h);
    if (geometry_err) {
        return geometry_err;
    }
    const int block_err = configure_hvs_blocks(s);
    if (block_err) {
        return block_err;
    }

    if (!fex->sycl_state) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "psnr_hvs_sycl: no SYCL state\n");
        return -EINVAL;
    }
    s->sycl_state = fex->sycl_state;

    int err = attach_shared_planes(s);
    if (!err) {
        err = allocate_hvs_buffers(s);
    }
    if (!err) {
        s->feature_name_dict =
            vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
        err = s->feature_name_dict ? 0 : -ENOMEM;
    }
    if (err) {
        (void)close_fex_sycl(fex);
        return err;
    }
    s->has_pending = false;
    return 0;
}

} // namespace

namespace
{

static PsnrHvsKernelArgs hvs_kernel_args(const PsnrHvsStateSycl *s)
{
    PsnrHvsKernelArgs args = {};
    for (unsigned p = 0; p < s->n_active_planes; p++) {
        args.plane[p] = {.ref = vmaf_sycl_get_shared_plane(s->sycl_state, 1, p),
                         .dist = vmaf_sycl_get_shared_plane(s->sycl_state, 0, p),
                         .width = s->width[p],
                         .blocks_x = s->num_blocks_x[p],
                         .first_block = s->first_block[p]};
    }
    args.partials = s->d_partials;
    args.n_planes = s->n_active_planes;
    args.total_blocks = s->total_blocks;
    args.wide = s->bpc > 8U;
    return args;
}

} // namespace

namespace
{

/* The luma of this frame is already on the device (the host read path
 * uploads it before any extractor submits); the chroma goes up once for all
 * twins here. The kernel waits for both on the device, the block scores
 * are read back in the same in-order stream, and collect() only waits. */
static int submit_fex_sycl(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                           VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic_90;
    (void)dist_pic_90;
    auto *s = static_cast<PsnrHvsStateSycl *>(fex->priv);
    auto *qptr = static_cast<sycl::queue *>(vmaf_sycl_get_queue_ptr(s->sycl_state));
    if (!qptr) {
        return -EINVAL;
    }
    int err = 0;
    if (s->n_active_planes > 1U) {
        /* The zero-copy import path hands no host pictures and imports luma only. */
        err = (ref_pic && dist_pic) ?
                  vmaf_sycl_shared_chroma_upload(s->sycl_state, ref_pic, dist_pic) :
                  -EINVAL;
    }
    if (!err) {
        err = vmaf_sycl_queue_after_upload(s->sycl_state, qptr);
    }
    if (err) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "psnr_hvs_sycl: frame %u planes not on the device (%d)\n",
                 index, err);
        return err;
    }
    try {
        launch_psnr_hvs(*qptr, hvs_kernel_args(s));
        qptr->memcpy(s->h_partials, s->d_partials, (size_t)s->total_blocks * sizeof(float));
    } catch (const sycl::exception &e) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "psnr_hvs_sycl: submitting frame %u: %s\n", index, e.what());
        return -EIO;
    }
    s->pending_index = index;
    s->has_pending = true;
    return 0;
}

} // namespace

namespace
{

static const char *const plane_features[PSNR_HVS_NUM_PLANES] = {"psnr_hvs_y", "psnr_hvs_cb",
                                                                "psnr_hvs_cr"};

/* A device fault surfaces here as a sycl::exception; it must not cross the C
 * collect callback, and the partials it leaves behind are stale. */
static int wait_hvs_partials(sycl::queue &queue)
{
    try {
        queue.wait_and_throw();
    } catch (const sycl::exception &e) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "psnr_hvs_sycl: reading back the block errors: %s\n",
                 e.what());
        return -EIO;
    }
    return 0;
}

/* Block order, one float accumulator per plane: the sum the gate was
 * calibrated on (ADR-1361). */
static void reduce_hvs_planes(const PsnrHvsStateSycl *s, double scores[PSNR_HVS_NUM_PLANES])
{
    for (int plane = 0; plane < PSNR_HVS_NUM_PLANES; ++plane) {
        if (std::cmp_greater_equal(plane, s->n_active_planes)) {
            break;
        }
        const float *partials = s->h_partials + s->first_block[plane];
        float sum = 0.0f;
        for (unsigned block = 0; block < s->num_blocks[plane]; block++) {
            sum += partials[block];
        }
        const int pixels = (int)(s->num_blocks[plane] * 64u);
        sum /= (float)pixels;
        sum /= (float)s->samplemax_sq;
        scores[plane] = (double)sum;
    }
}

} // namespace

namespace
{

static int append_hvs_scores(VmafFeatureCollector *collector, const PsnrHvsStateSycl *s,
                             const double scores[PSNR_HVS_NUM_PLANES], unsigned index)
{
    int err = 0;
    for (int plane = 0; plane < PSNR_HVS_NUM_PLANES; ++plane) {
        if (std::cmp_greater_equal(plane, s->n_active_planes)) {
            break;
        }
        const double db = 10.0 * (-1.0 * std::log10(scores[plane]));
        err |= vmaf_feature_collector_append(collector, plane_features[plane], db, index);
    }
    const double combined =
        (s->n_active_planes == 1U) ? scores[0] : 0.8 * scores[0] + 0.1 * (scores[1] + scores[2]);
    const double db = 10.0 * (-1.0 * std::log10(combined));
    err |= vmaf_feature_collector_append(collector, "psnr_hvs", db, index);
    return err;
}

} // namespace

namespace
{

static int collect_fex_sycl(VmafFeatureExtractor *fex, unsigned index,
                            VmafFeatureCollector *feature_collector)
{
    auto *s = static_cast<PsnrHvsStateSycl *>(fex->priv);
    auto *qptr = static_cast<sycl::queue *>(vmaf_sycl_get_queue_ptr(s->sycl_state));
    if (!qptr) {
        return -EINVAL;
    }
    const int wait_err = wait_hvs_partials(*qptr);
    if (wait_err) {
        return wait_err;
    }
    double plane_score[PSNR_HVS_NUM_PLANES] = {};
    reduce_hvs_planes(s, plane_score);
    return append_hvs_scores(feature_collector, s, plane_score, index);
}

} // namespace

namespace
{

static int close_fex_sycl(VmafFeatureExtractor *fex)
{
    auto *s = static_cast<PsnrHvsStateSycl *>(fex->priv);
    if (s->sycl_state) {
        if (s->d_partials) {
            vmaf_sycl_free(s->sycl_state, s->d_partials);
        }
        if (s->h_partials) {
            vmaf_sycl_free(s->sycl_state, s->h_partials);
        }
    }
    if (s->feature_name_dict) {
        vmaf_dictionary_free(&s->feature_name_dict);
    }
    return 0;
}

static const char *provided_features_psnr_hvs_sycl[] = {"psnr_hvs_y", "psnr_hvs_cb", "psnr_hvs_cr",
                                                        "psnr_hvs", nullptr};

} // namespace

extern "C" VmafFeatureExtractor vmaf_fex_psnr_hvs_sycl = {
    .name = "psnr_hvs_sycl",
    .init = init_fex_sycl,
    .extract = nullptr,
    .flush = nullptr,
    .close = close_fex_sycl,
    .submit = submit_fex_sycl,
    .collect = collect_fex_sycl,
    .options = options_psnr_hvs_sycl,
    .priv_size = sizeof(PsnrHvsStateSycl),
    .flags = VMAF_FEATURE_EXTRACTOR_SYCL,
    .provided_features = provided_features_psnr_hvs_sycl,
    .chars =
        {
            .n_dispatches_per_frame = 1,
            .is_reduction_only = false,
            .min_useful_frame_area = 1920U * 1080U,
            .dispatch_hint = VMAF_FEATURE_DISPATCH_AUTO,
        },
};
