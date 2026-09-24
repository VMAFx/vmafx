/**
 *  Copyright 2001-2012 Xiph.Org and contributors.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent AND BSD-2-Clause
 *
 *  psnr_hvs feature extractor on the SYCL backend
 *  (T7-23 / ADR-0188 / ADR-0191, GPU long-tail batch 2 part 3c).
 *  SYCL twin of psnr_hvs_vulkan (PR #143) and psnr_hvs_cuda
 *  (this PR's batch 2 part 3b).
 *
 *  Self-contained submit/collect — does NOT register with
 *  vmaf_sycl_graph_register because shared_frame is luma-only
 *  packed at uint width and psnr_hvs needs picture_copy-
 *  normalised float planes for all three planes (Y, Cb, Cr).
 *  Same pattern as ssim_sycl / ms_ssim_sycl.
 *
 *  Per-plane single-dispatch design — one work-group per output
 *  8×8 image block (step=7), 64 threads/WG. Cooperative load +
 *  thread-0-serial reductions matching CPU's exact i,j summation
 *  order (locks float bit-order to CPU's calc_psnrhvs).
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
#include "../picture_copy.h"
#include "sycl/common.h"

namespace
{

static constexpr int PSNR_HVS_BLOCK = 8;
static constexpr int PSNR_HVS_STEP = 7;
static constexpr int PSNR_HVS_NUM_PLANES = 3;
static constexpr size_t WG_DIM = 8;

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

struct PsnrHvsStateSycl {
    unsigned width[PSNR_HVS_NUM_PLANES];
    unsigned height[PSNR_HVS_NUM_PLANES];
    unsigned num_blocks_x[PSNR_HVS_NUM_PLANES];
    unsigned num_blocks_y[PSNR_HVS_NUM_PLANES];
    unsigned num_blocks[PSNR_HVS_NUM_PLANES];
    unsigned bpc;
    int32_t samplemax_sq;
    /* enable_chroma: when false, only the luma (Y) plane is dispatched.
     * Default true mirrors CPU integer_psnr_hvs — see ADR-0453. */
    bool enable_chroma;
    /* n_active_planes: 1 when enable_chroma=false or YUV400P, else 3. */
    unsigned n_active_planes;

    VmafSyclState *sycl_state;

    /* Host pinned float planes for picture_copy upload. */
    float *h_ref[PSNR_HVS_NUM_PLANES];
    float *h_dist[PSNR_HVS_NUM_PLANES];
    /* Device USM ref / dist / partials × 3 planes. */
    float *d_ref[PSNR_HVS_NUM_PLANES];
    float *d_dist[PSNR_HVS_NUM_PLANES];
    float *d_partials[PSNR_HVS_NUM_PLANES];
    float *h_partials[PSNR_HVS_NUM_PLANES];

    bool has_pending;
    unsigned pending_index;
    VmafDictionary *feature_name_dict;
};

struct PsnrHvsKernelArgs {
    const float *ref;
    const float *dist;
    float *partials;
    unsigned width;
    unsigned height;
    unsigned blocks_x;
    unsigned blocks_y;
    int plane;
    int bpc;
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

static void od_bin_fdct8x8(int blk[64])
{
    int z[64];
    for (int i = 0; i < 8; i++) {
        int y0;
        int y1;
        int y2;
        int y3;
        int y4;
        int y5;
        int y6;
        int y7;
        od_bin_fdct8(y0, y1, y2, y3, y4, y5, y6, y7, blk[0 * 8 + i], blk[1 * 8 + i], blk[2 * 8 + i],
                     blk[3 * 8 + i], blk[4 * 8 + i], blk[5 * 8 + i], blk[6 * 8 + i],
                     blk[7 * 8 + i]);
        z[i * 8 + 0] = y0;
        z[i * 8 + 1] = y1;
        z[i * 8 + 2] = y2;
        z[i * 8 + 3] = y3;
        z[i * 8 + 4] = y4;
        z[i * 8 + 5] = y5;
        z[i * 8 + 6] = y6;
        z[i * 8 + 7] = y7;
    }
    for (int i = 0; i < 8; i++) {
        int y0;
        int y1;
        int y2;
        int y3;
        int y4;
        int y5;
        int y6;
        int y7;
        od_bin_fdct8(y0, y1, y2, y3, y4, y5, y6, y7, z[0 * 8 + i], z[1 * 8 + i], z[2 * 8 + i],
                     z[3 * 8 + i], z[4 * 8 + i], z[5 * 8 + i], z[6 * 8 + i], z[7 * 8 + i]);
        blk[i * 8 + 0] = y0;
        blk[i * 8 + 1] = y1;
        blk[i * 8 + 2] = y2;
        blk[i * 8 + 3] = y3;
        blk[i * 8 + 4] = y4;
        blk[i * 8 + 5] = y5;
        blk[i * 8 + 6] = y6;
        blk[i * 8 + 7] = y7;
    }
}

} // namespace

namespace
{

static inline int sample_to_int(float v, int bpc)
{
    if (bpc == 8) {
        return (int)sycl::floor(v + 0.5f);
    }
    if (bpc == 10) {
        return (int)sycl::floor(v * 4.0f + 0.5f);
    }
    return (int)sycl::floor(v * 16.0f + 0.5f);
}

} // namespace

namespace
{

static inline bool load_hvs_block(sycl::nd_item<2> item,
                                  const sycl::local_accessor<int, 1> &local_ref,
                                  const sycl::local_accessor<int, 1> &local_dist,
                                  const PsnrHvsKernelArgs &args)
{
    const size_t block_y = item.get_group(0);
    const size_t block_x = item.get_group(1);
    const size_t local_y = item.get_local_id(0);
    const size_t local_x = item.get_local_id(1);
    const size_t local_index = local_y * 8u + local_x;
    const size_t origin_x = block_x * 7u;
    const size_t origin_y = block_y * 7u;
    const bool valid = block_x < (size_t)args.blocks_x && block_y < (size_t)args.blocks_y &&
                       origin_x + 7u < (size_t)args.width && origin_y + 7u < (size_t)args.height;
    int ref = 0;
    int dist = 0;
    if (valid) {
        const size_t source_index = (origin_y + local_y) * (size_t)args.width + origin_x + local_x;
        ref = sample_to_int(args.ref[source_index], args.bpc);
        dist = sample_to_int(args.dist[source_index], args.bpc);
    }
    local_ref[local_index] = ref;
    local_dist[local_index] = dist;
    return valid;
}

} // namespace

namespace
{

static inline void copy_hvs_block(const sycl::local_accessor<int, 1> &source, int output[64])
{
    for (int index = 0; index < 64; index++) {
        output[index] = source[index];
    }
}

static inline float hvs_variance_ratio(const int block[64])
{
    float means[4] = {0.f, 0.f, 0.f, 0.f};
    float global_mean = 0.f;
    for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 8; col++) {
            const int subgroup = ((row & 12) >> 2) + ((col & 12) >> 1);
            global_mean += (float)block[row * 8 + col];
            means[subgroup] += (float)block[row * 8 + col];
        }
    }
    global_mean /= 64.f;
    means[0] /= 16.f;
    means[1] /= 16.f;
    means[2] /= 16.f;
    means[3] /= 16.f;
    float variances[4] = {0.f, 0.f, 0.f, 0.f};
    float global_variance = 0.f;
    for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 8; col++) {
            const int subgroup = ((row & 12) >> 2) + ((col & 12) >> 1);
            const float global_delta = (float)block[row * 8 + col] - global_mean;
            const float subgroup_delta = (float)block[row * 8 + col] - means[subgroup];
            global_variance += global_delta * global_delta;
            variances[subgroup] += subgroup_delta * subgroup_delta;
        }
    }
    global_variance *= 1.f / 63.f * 64.f;
    variances[0] *= 1.f / 15.f * 16.f;
    variances[1] *= 1.f / 15.f * 16.f;
    variances[2] *= 1.f / 15.f * 16.f;
    variances[3] *= 1.f / 15.f * 16.f;
    if (global_variance > 0.f) {
        global_variance =
            (variances[0] + variances[1] + variances[2] + variances[3]) / global_variance;
    }
    return global_variance;
}

} // namespace

namespace
{

static inline void hvs_mask(float output[64], int plane)
{
    for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 8; col++) {
            const float csf = CSF_TABLES[plane][row * 8 + col];
            const float scaled = csf * 0.3885746225901003f;
            output[row * 8 + col] = scaled * scaled;
        }
    }
}

static inline float hvs_mask_energy(const int block[64], const float mask[64])
{
    float energy = 0.f;
    for (int row = 0; row < 8; row++) {
        const int first_col = (row == 0) ? 1 : 0;
        for (int col = first_col; col < 8; col++) {
            const int coefficient = block[row * 8 + col];
            energy += (float)(coefficient * coefficient) * mask[row * 8 + col];
        }
    }
    return energy;
}

} // namespace

namespace
{

static inline float hvs_error(const int ref[64], const int dist[64], const float mask[64],
                              float threshold, int plane)
{
    float error_sum = 0.f;
    for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 8; col++) {
            const int index = row * 8 + col;
            const float csf = CSF_TABLES[plane][index];
            float error = sycl::fabs((float)ref[index] - (float)dist[index]);
            if (row != 0 || col != 0) {
                const float masking = threshold / mask[index];
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

static inline float score_hvs_block(const sycl::local_accessor<int, 1> &local_ref,
                                    const sycl::local_accessor<int, 1> &local_dist, int plane)
{
    int ref[64];
    int dist[64];
    copy_hvs_block(local_ref, ref);
    copy_hvs_block(local_dist, dist);
    const float ref_ratio = hvs_variance_ratio(ref);
    const float dist_ratio = hvs_variance_ratio(dist);
    od_bin_fdct8x8(ref);
    od_bin_fdct8x8(dist);
    float mask[64];
    hvs_mask(mask, plane);
    const float ref_energy = hvs_mask_energy(ref, mask);
    const float dist_energy = hvs_mask_energy(dist, mask);
    float threshold = sycl::sqrt(ref_energy * ref_ratio) / 32.f;
    const float dist_threshold = sycl::sqrt(dist_energy * dist_ratio) / 32.f;
    if (dist_threshold > threshold) {
        threshold = dist_threshold;
    }
    return hvs_error(ref, dist, mask, threshold, plane);
}

} // namespace

namespace
{

static void launch_psnr_hvs(sycl::queue &q, PsnrHvsKernelArgs args)
{
    sycl::nd_range<2> const ndr{
        sycl::range<2>{(size_t)args.blocks_y * WG_DIM, (size_t)args.blocks_x * WG_DIM},
        sycl::range<2>{WG_DIM, WG_DIM}};
    q.submit([=](sycl::handler &h_) {
        sycl::local_accessor<int, 1> const s_ref(sycl::range<1>(64), h_);
        sycl::local_accessor<int, 1> const s_dist(sycl::range<1>(64), h_);
        h_.parallel_for(ndr, [=](sycl::nd_item<2> it) {
            const bool valid = load_hvs_block(it, s_ref, s_dist, args);
            it.barrier(sycl::access::fence_space::local_space);
            if (it.get_local_linear_id() != 0u) {
                return;
            }
            const float score = valid ? score_hvs_block(s_ref, s_dist, args.plane) : 0.f;
            const size_t slot = it.get_group(0) * (size_t)args.blocks_x + it.get_group(1);
            args.partials[slot] = score;
        });
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
    }
    return 0;
}

} // namespace

namespace
{

static int allocate_hvs_buffers(PsnrHvsStateSycl *s)
{
    for (int plane = 0; std::cmp_less(plane, s->n_active_planes); plane++) {
        const size_t plane_bytes = (size_t)s->width[plane] * s->height[plane] * sizeof(float);
        const size_t partials_bytes = (size_t)s->num_blocks[plane] * sizeof(float);
        s->h_ref[plane] = static_cast<float *>(vmaf_sycl_malloc_host(s->sycl_state, plane_bytes));
        s->h_dist[plane] = static_cast<float *>(vmaf_sycl_malloc_host(s->sycl_state, plane_bytes));
        s->d_ref[plane] = static_cast<float *>(vmaf_sycl_malloc_device(s->sycl_state, plane_bytes));
        s->d_dist[plane] =
            static_cast<float *>(vmaf_sycl_malloc_device(s->sycl_state, plane_bytes));
        s->d_partials[plane] =
            static_cast<float *>(vmaf_sycl_malloc_device(s->sycl_state, partials_bytes));
        s->h_partials[plane] =
            static_cast<float *>(vmaf_sycl_malloc_host(s->sycl_state, partials_bytes));
        if (!s->h_ref[plane] || !s->h_dist[plane] || !s->d_ref[plane] || !s->d_dist[plane] ||
            !s->d_partials[plane] || !s->h_partials[plane]) {
            vmaf_log(VMAF_LOG_LEVEL_ERROR, "psnr_hvs_sycl: USM allocation failed\n");
            return -ENOMEM;
        }
    }
    return 0;
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

    const int alloc_err = allocate_hvs_buffers(s);
    if (alloc_err) {
        (void)close_fex_sycl(fex);
        return alloc_err;
    }

    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (!s->feature_name_dict) {
        (void)close_fex_sycl(fex);
        return -ENOMEM;
    }
    s->has_pending = false;
    return 0;
}

} // namespace

namespace
{

/* Per-plane picture_copy clone (since libvmaf's picture_copy
 * hardcodes plane 0). */
template <typename Picture>
static void picture_copy_plane(float *dst, Picture *pic, int plane, unsigned width, unsigned height)
{
    if (pic->bpc <= 8) {
        const auto *src = static_cast<const uint8_t *>(pic->data[plane]);
        const size_t src_stride = (size_t)pic->stride[plane];
        for (unsigned y = 0; y < height; y++) {
            for (unsigned x = 0; x < width; x++) {
                dst[y * width + x] = (float)src[y * src_stride + x];
            }
        }
    } else {
        const float scaler = (pic->bpc == 10) ? 4.0f :
                             (pic->bpc == 12) ? 16.0f :
                             (pic->bpc == 16) ? 256.0f :
                                                1.0f;
        const auto *src = static_cast<const uint16_t *>(pic->data[plane]);
        const size_t src_stride_words = (size_t)pic->stride[plane] / sizeof(uint16_t);
        for (unsigned y = 0; y < height; y++) {
            for (unsigned x = 0; x < width; x++) {
                dst[y * width + x] = (float)src[y * src_stride_words + x] / scaler;
            }
        }
    }
}

} // namespace

namespace
{

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
    sycl::queue &q = *qptr;

    for (int p = 0; p < PSNR_HVS_NUM_PLANES; ++p) {
        if (std::cmp_greater_equal(p, s->n_active_planes)) {
            break;
        }
        picture_copy_plane(s->h_ref[p], ref_pic, p, s->width[p], s->height[p]);
        picture_copy_plane(s->h_dist[p], dist_pic, p, s->width[p], s->height[p]);
        const size_t plane_bytes = (size_t)s->width[p] * s->height[p] * sizeof(float);
        q.memcpy(s->d_ref[p], s->h_ref[p], plane_bytes);
        q.memcpy(s->d_dist[p], s->h_dist[p], plane_bytes);
    }

    for (int p = 0; p < PSNR_HVS_NUM_PLANES; ++p) {
        if (std::cmp_greater_equal(p, s->n_active_planes)) {
            break;
        }
        launch_psnr_hvs(q, {.ref = s->d_ref[p],
                            .dist = s->d_dist[p],
                            .partials = s->d_partials[p],
                            .width = s->width[p],
                            .height = s->height[p],
                            .blocks_x = s->num_blocks_x[p],
                            .blocks_y = s->num_blocks_y[p],
                            .plane = p,
                            .bpc = (int)s->bpc});
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

static void copy_hvs_partials(PsnrHvsStateSycl *s, sycl::queue &queue)
{
    for (int plane = 0; plane < PSNR_HVS_NUM_PLANES; ++plane) {
        if (std::cmp_greater_equal(plane, s->n_active_planes)) {
            break;
        }
        const size_t bytes = (size_t)s->num_blocks[plane] * sizeof(float);
        queue.memcpy(s->h_partials[plane], s->d_partials[plane], bytes);
    }
    queue.wait();
}

static void reduce_hvs_planes(const PsnrHvsStateSycl *s, double scores[PSNR_HVS_NUM_PLANES])
{
    for (int plane = 0; plane < PSNR_HVS_NUM_PLANES; ++plane) {
        if (std::cmp_greater_equal(plane, s->n_active_planes)) {
            break;
        }
        float sum = 0.0f;
        for (unsigned block = 0; block < s->num_blocks[plane]; block++) {
            sum += s->h_partials[plane][block];
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
    sycl::queue &q = *qptr;
    copy_hvs_partials(s, q);
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
        for (int p = 0; p < PSNR_HVS_NUM_PLANES; p++) {
            if (s->h_ref[p])
                vmaf_sycl_free(s->sycl_state, s->h_ref[p]);
            if (s->h_dist[p])
                vmaf_sycl_free(s->sycl_state, s->h_dist[p]);
            if (s->d_ref[p])
                vmaf_sycl_free(s->sycl_state, s->d_ref[p]);
            if (s->d_dist[p])
                vmaf_sycl_free(s->sycl_state, s->d_dist[p]);
            if (s->d_partials[p])
                vmaf_sycl_free(s->sycl_state, s->d_partials[p]);
            if (s->h_partials[p])
                vmaf_sycl_free(s->sycl_state, s->h_partials[p]);
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
            .n_dispatches_per_frame = 3,
            .is_reduction_only = false,
            .min_useful_frame_area = 1920U * 1080U,
            .dispatch_hint = VMAF_FEATURE_DISPATCH_AUTO,
        },
};
