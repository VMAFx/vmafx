/* Upstream-mirror filename: defines float_ssim symbol despite the integer_ prefix (matches Netflix upstream). See ADR-0549. */
/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright (c) 2011, Tom Distler (http://tdistler.com)
 *  Copyright 2001-2012 Xiph.Org and contributors.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent AND BSD-3-Clause AND BSD-2-Clause
 *
 *  float_ssim feature extractor on the SYCL backend
 *  (T7-23 / ADR-0188 / ADR-0189, GPU long-tail batch 2 part 1c).
 *  SYCL twin of ssim_vulkan (PR #139) and ssim_cuda (this PR's
 *  batch 2 part 1b).
 *
 *  Self-contained submit / collect — does *not* register with
 *  vmaf_sycl_graph_register because shared_frame is luma-only
 *  packed at uint width and SSIM needs float [0, 255]
 *  intermediates with picture_copy normalisation. Same approach
 *  as ciede_sycl (PR #137).
 *
*  Two-pass design mirrors ssim_vulkan / ssim_cuda:
 *    1. horizontal 11-tap separable Gaussian over ref / cmp /
 *       ref² / cmp² / ref·cmp into 5 device float buffers.
 *       SLM-staged (SY-2, ADR-0458): 26-float tile per WG row
 *       eliminates redundant global-memory reads across neighbours.
 *    2. nd_range vertical 11-tap + per-pixel SSIM combine +
 *       per-WG float partial sums via sycl::reduce_over_group.
 *
 *  Host accumulates partials in `double`, divides by
 *  (W-10)·(H-10) and emits `float_ssim`.
 *
 *  v1: scale=1 only — same constraint as ssim_vulkan/cuda.
 *  fp64-free (Intel Arc A380 lacks native fp64).
 */

#include <sycl/sycl.hpp>

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "config.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "feature/nonfinite_score.h"
#include "log.h"
#include "picture.h"
#include "../picture_copy.h"
#include "sycl/common.h"

namespace
{

constexpr size_t SSIM_WG_X = 16;
static constexpr size_t SSIM_WG_Y = 8;
static constexpr int SSIM_K = 11;

/* Same 11-tap normalised Gaussian as the Vulkan + CUDA twins —
 * matches g_gaussian_window_h in iqa/ssim_tools.h byte-for-byte. */
constexpr float G[SSIM_K] = {
    0.001028f, 0.007599f, 0.036001f, 0.109361f, 0.213006f, 0.266012f,
    0.213006f, 0.109361f, 0.036001f, 0.007599f, 0.001028f,
};

} // namespace

namespace
{

struct SsimStateSycl {
    /* Frame geometry. */
    unsigned width;
    unsigned height;
    unsigned bpc;
    int scale_override;

    unsigned w_horiz;
    unsigned h_horiz;
    unsigned w_final;
    unsigned h_final;
    unsigned wg_count_x;
    unsigned wg_count_y;
    unsigned wg_count;

    float c1;
    float c2;

    /* SYCL state back-pointer. */
    VmafSyclState *sycl_state;

    /* Host-pinned float ref / cmp staging (post picture_copy). */
    float *h_ref;
    float *h_cmp;
    /* Device USM ref / cmp + 5 intermediates + WG partials. */
    float *d_ref;
    float *d_cmp;
    float *d_ref_mu;
    float *d_cmp_mu;
    float *d_ref_sq;
    float *d_cmp_sq;
    float *d_refcmp;
    float *d_partials;
    /* Host-pinned partials for D2H. */
    float *h_partials;

    bool has_pending;
    unsigned pending_index;

    VmafDictionary *feature_name_dict;
};

} // namespace

/* Tile width for the horizontal SLM staging (SY-2, ADR-0458):
 * each WG of SSIM_WG_X columns needs SSIM_WG_X + (SSIM_K-1) input
 * floats per row to cover the 11-tap apron.  Two SLM arrays (ref,
 * cmp) of size SSIM_WG_Y * SSIM_TILE_W carry all needed input pixels;
 * the five output channels are computed from SLM with no extra arrays.
 * This eliminates 11 global-memory loads per output channel per pixel
 * (total 55 → 26 loads per pixel pair on Arc A380). */
namespace
{

constexpr size_t SSIM_TILE_W = SSIM_WG_X + (size_t)(SSIM_K - 1); /* 26 */

struct FloatHorizArgs {
    const float *reference;
    const float *comparison;
    float *reference_mean;
    float *comparison_mean;
    float *reference_square;
    float *comparison_square;
    float *cross_product;
    unsigned width;
    unsigned output_width;
    unsigned output_height;
};

struct FloatVertArgs {
    const float *reference_mean;
    const float *comparison_mean;
    const float *reference_square;
    const float *comparison_square;
    const float *cross_product;
    float *partials;
    unsigned horizontal_width;
    unsigned final_width;
    unsigned final_height;
    size_t group_columns;
    float c1;
    float c2;
};

struct SsimMoments {
    float reference_mean;
    float comparison_mean;
    float reference_square;
    float comparison_square;
    float cross_product;
};

} // namespace

namespace
{

static inline void load_float_tile(sycl::nd_item<2> item, const FloatHorizArgs &args,
                                   const sycl::local_accessor<float, 1> &reference,
                                   const sycl::local_accessor<float, 1> &comparison)
{
    const size_t local = item.get_local_id(0) * SSIM_WG_X + item.get_local_id(1);
    const size_t origin_x = item.get_group(1) * SSIM_WG_X;
    const size_t origin_y = item.get_group(0) * SSIM_WG_Y;
    const size_t group_size = SSIM_WG_X * SSIM_WG_Y;
    for (size_t offset = local; offset < SSIM_WG_Y * SSIM_TILE_W; offset += group_size) {
        const size_t y = origin_y + offset / SSIM_TILE_W;
        const size_t x = origin_x + offset % SSIM_TILE_W;
        if (y < args.output_height && x < args.width) {
            const size_t index = y * args.width + x;
            reference[offset] = args.reference[index];
            comparison[offset] = args.comparison[index];
        } else {
            reference[offset] = 0.0f;
            comparison[offset] = 0.0f;
        }
    }
}

} // namespace

namespace
{

static inline SsimMoments horizontal_moments(size_t local_x, size_t local_y,
                                             const sycl::local_accessor<float, 1> &reference,
                                             const sycl::local_accessor<float, 1> &comparison)
{
    SsimMoments result{};
    for (int tap = 0; tap < SSIM_K; ++tap) {
        const size_t index = local_y * SSIM_TILE_W + local_x + (size_t)tap;
        const float ref = reference[index];
        const float cmp = comparison[index];
        const float weight = G[tap];
        result.reference_mean += weight * ref;
        result.comparison_mean += weight * cmp;
        result.reference_square += weight * (ref * ref);
        result.comparison_square += weight * (cmp * cmp);
        result.cross_product += weight * (ref * cmp);
    }
    return result;
}

} // namespace

namespace
{

static inline void store_horizontal_moments(sycl::nd_item<2> item, const FloatHorizArgs &args,
                                            const sycl::local_accessor<float, 1> &reference,
                                            const sycl::local_accessor<float, 1> &comparison)
{
    const size_t x = item.get_global_id(1);
    const size_t y = item.get_global_id(0);
    if (x >= args.output_width || y >= args.output_height) {
        return;
    }
    const SsimMoments moments =
        horizontal_moments(item.get_local_id(1), item.get_local_id(0), reference, comparison);
    const size_t index = y * args.output_width + x;
    args.reference_mean[index] = moments.reference_mean;
    args.comparison_mean[index] = moments.comparison_mean;
    args.reference_square[index] = moments.reference_square;
    args.comparison_square[index] = moments.comparison_square;
    args.cross_product[index] = moments.cross_product;
}

} // namespace

namespace
{

static void launch_horiz(sycl::queue &queue, const FloatHorizArgs &args)
{
    const size_t global_x = ((args.output_width + SSIM_WG_X - 1) / SSIM_WG_X) * SSIM_WG_X;
    const size_t global_y = ((args.output_height + SSIM_WG_Y - 1) / SSIM_WG_Y) * SSIM_WG_Y;
    sycl::nd_range<2> const range{sycl::range<2>{global_y, global_x},
                                  sycl::range<2>{SSIM_WG_Y, SSIM_WG_X}};
    queue.submit([&](sycl::handler &handler) {
        sycl::local_accessor<float, 1> const reference(sycl::range<1>(SSIM_WG_Y * SSIM_TILE_W),
                                                       handler);
        sycl::local_accessor<float, 1> const comparison(sycl::range<1>(SSIM_WG_Y * SSIM_TILE_W),
                                                        handler);
        handler.parallel_for(range, [=](sycl::nd_item<2> item) {
            load_float_tile(item, args, reference, comparison);
            item.barrier(sycl::access::fence_space::local_space);
            store_horizontal_moments(item, args, reference, comparison);
        });
    });
}

} // namespace

namespace
{

static inline SsimMoments vertical_moments(const FloatVertArgs &args, size_t x, size_t y)
{
    SsimMoments result{};
    for (int tap = 0; tap < SSIM_K; ++tap) {
        const size_t index = (y + (size_t)tap) * args.horizontal_width + x;
        const float weight = G[tap];
        result.reference_mean += weight * args.reference_mean[index];
        result.comparison_mean += weight * args.comparison_mean[index];
        result.reference_square += weight * args.reference_square[index];
        result.comparison_square += weight * args.comparison_square[index];
        result.cross_product += weight * args.cross_product[index];
    }
    return result;
}

} // namespace

namespace
{

static inline float float_ssim_value(const FloatVertArgs &args, size_t x, size_t y)
{
    const SsimMoments moments = vertical_moments(args, x, y);
    const float reference_variance =
        moments.reference_square - moments.reference_mean * moments.reference_mean;
    const float comparison_variance =
        moments.comparison_square - moments.comparison_mean * moments.comparison_mean;
    const float covariance =
        moments.cross_product - moments.reference_mean * moments.comparison_mean;
    const float mean_product = moments.reference_mean * moments.comparison_mean;
    const float numerator = (2.0f * mean_product + args.c1) * (2.0f * covariance + args.c2);
    const float denominator = (moments.reference_mean * moments.reference_mean +
                               moments.comparison_mean * moments.comparison_mean + args.c1) *
                              (reference_variance + comparison_variance + args.c2);
    return numerator / denominator;
}

} // namespace

namespace
{

static inline void store_float_group(sycl::nd_item<2> item, const FloatVertArgs &args, float value)
{
    const float sum = sycl::reduce_over_group(item.get_group(), value, sycl::plus<float>{});
    if (item.get_local_id(0) == 0 && item.get_local_id(1) == 0) {
        const size_t index = item.get_group(0) * args.group_columns + item.get_group(1);
        args.partials[index] = sum;
    }
}

} // namespace

namespace
{

static void launch_vert_combine(sycl::queue &queue, const FloatVertArgs &args)
{
    const size_t global_x = ((args.final_width + SSIM_WG_X - 1) / SSIM_WG_X) * SSIM_WG_X;
    const size_t global_y = ((args.final_height + SSIM_WG_Y - 1) / SSIM_WG_Y) * SSIM_WG_Y;
    sycl::nd_range<2> const range{sycl::range<2>{global_y, global_x},
                                  sycl::range<2>{SSIM_WG_Y, SSIM_WG_X}};
    queue.submit([=](sycl::handler &handler) {
        handler.parallel_for(range, [=](sycl::nd_item<2> item) {
            const size_t x = item.get_global_id(1);
            const size_t y = item.get_global_id(0);
            const float value =
                x < args.final_width && y < args.final_height ? float_ssim_value(args, x, y) : 0.0f;
            store_float_group(item, args, value);
        });
    });
}

} // namespace

namespace
{

static int round_to_int(float x)
{
    return (int)(x + (x < 0.0f ? -0.5f : 0.5f));
}
static int min_int(int a, int b)
{
    return a < b ? a : b;
}
static int compute_scale(unsigned w, unsigned h, int override_)
{
    if (override_ > 0) {
        return override_;
    }
    int const scaled = round_to_int((float)min_int((int)w, (int)h) / 256.0f);
    return scaled < 1 ? 1 : scaled;
}

} // namespace

static const VmafOption options_ssim_sycl[] = {
    {
        .name = "scale",
        .help = "decimation scale factor (0=auto, 1=no downscaling). "
                "v1: GPU path requires scale=1; auto-detect rejects scale>1 with -EINVAL.",
        .offset = offsetof(SsimStateSycl, scale_override),
        .type = VMAF_OPT_TYPE_INT,
        .default_val = {.i = 0},
        .min = 0,
        .max = 10,
    },
    {.name = nullptr},
};

namespace
{

static int configure_float_ssim(SsimStateSycl *s, unsigned bpc, unsigned width, unsigned height)
{
    const int scale = compute_scale(width, height, s->scale_override);
    if (scale != 1) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "ssim_sycl: v1 supports scale=1 only (auto-detected scale=%d at %ux%u). "
                 "Pin --feature float_ssim_sycl:scale=1 if intended.\n",
                 scale, width, height);
        return -EINVAL;
    }
    if (width < (unsigned)SSIM_K || height < (unsigned)SSIM_K) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "ssim_sycl: input %ux%u smaller than 11x11 Gaussian footprint.\n", width, height);
        return -EINVAL;
    }
    s->width = width;
    s->height = height;
    s->bpc = bpc;
    s->w_horiz = width - (SSIM_K - 1);
    s->h_horiz = height;
    s->w_final = width - (SSIM_K - 1);
    s->h_final = height - (SSIM_K - 1);
    s->wg_count_x = (s->w_final + (unsigned)SSIM_WG_X - 1) / (unsigned)SSIM_WG_X;
    s->wg_count_y = (s->h_final + (unsigned)SSIM_WG_Y - 1) / (unsigned)SSIM_WG_Y;
    s->wg_count = s->wg_count_x * s->wg_count_y;
    const float range = 255.0f;
    const float k1 = 0.01f;
    const float k2 = 0.03f;
    s->c1 = (k1 * range) * (k1 * range);
    s->c2 = (k2 * range) * (k2 * range);
    return 0;
}

} // namespace

namespace
{

template <typename T> static T *allocate_host(VmafSyclState *state, size_t bytes)
{
    return static_cast<T *>(vmaf_sycl_malloc_host(state, bytes));
}

template <typename T> static T *allocate_device(VmafSyclState *state, size_t bytes)
{
    return static_cast<T *>(vmaf_sycl_malloc_device(state, bytes));
}

} // namespace

namespace
{

static void allocate_float_ssim(SsimStateSycl *s)
{
    const size_t input_bytes = (size_t)s->width * s->height * sizeof(float);
    const size_t horiz_bytes = (size_t)s->w_horiz * s->h_horiz * sizeof(float);
    const size_t partials_bytes = (size_t)s->wg_count * sizeof(float);
    s->h_ref = allocate_host<float>(s->sycl_state, input_bytes);
    s->h_cmp = allocate_host<float>(s->sycl_state, input_bytes);
    s->d_ref = allocate_device<float>(s->sycl_state, input_bytes);
    s->d_cmp = allocate_device<float>(s->sycl_state, input_bytes);
    s->d_ref_mu = allocate_device<float>(s->sycl_state, horiz_bytes);
    s->d_cmp_mu = allocate_device<float>(s->sycl_state, horiz_bytes);
    s->d_ref_sq = allocate_device<float>(s->sycl_state, horiz_bytes);
    s->d_cmp_sq = allocate_device<float>(s->sycl_state, horiz_bytes);
    s->d_refcmp = allocate_device<float>(s->sycl_state, horiz_bytes);
    s->d_partials = allocate_device<float>(s->sycl_state, partials_bytes);
    s->h_partials = allocate_host<float>(s->sycl_state, partials_bytes);
}

} // namespace

namespace
{

static bool float_ssim_allocations_complete(const SsimStateSycl *s)
{
    return s->h_ref && s->h_cmp && s->d_ref && s->d_cmp && s->d_ref_mu && s->d_cmp_mu &&
           s->d_ref_sq && s->d_cmp_sq && s->d_refcmp && s->d_partials && s->h_partials;
}

} // namespace

namespace
{

static int init_fex_sycl(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                         unsigned width, unsigned height)
{
    (void)pix_fmt;
    auto *s = static_cast<SsimStateSycl *>(fex->priv);
    const int config_error = configure_float_ssim(s, bpc, width, height);
    if (config_error) {
        return config_error;
    }
    if (!fex->sycl_state) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "ssim_sycl: no SYCL state\n");
        return -EINVAL;
    }
    s->sycl_state = fex->sycl_state;
    allocate_float_ssim(s);
    if (!float_ssim_allocations_complete(s)) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "ssim_sycl: USM allocation failed\n");
        return -ENOMEM;
    }
    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (!s->feature_name_dict) {
        return -ENOMEM;
    }
    s->has_pending = false;
    return 0;
}

} // namespace

namespace
{

static int submit_fex_sycl(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                           VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic_90;
    (void)dist_pic_90;
    auto *s = static_cast<SsimStateSycl *>(fex->priv);
    auto *qptr = static_cast<sycl::queue *>(vmaf_sycl_get_queue_ptr(s->sycl_state));
    if (!qptr)
        return -EINVAL;
    sycl::queue &q = *qptr;

    /* Host-side picture_copy → uint sample → float [0, 255]
     * (matches CPU float_ssim.c::extract). The destination is
     * tightly packed at width*sizeof(float). */
    picture_copy(s->h_ref, (ptrdiff_t)((size_t)s->width * sizeof(float)), ref_pic, /*offset=*/0,
                 ref_pic->bpc, 0);
    picture_copy(s->h_cmp, (ptrdiff_t)((size_t)s->width * sizeof(float)), dist_pic, 0,
                 dist_pic->bpc, 0);

    const size_t input_bytes = (size_t)s->width * s->height * sizeof(float);
    q.memcpy(s->d_ref, s->h_ref, input_bytes);
    q.memcpy(s->d_cmp, s->h_cmp, input_bytes);

    launch_horiz(q, {.reference = s->d_ref,
                     .comparison = s->d_cmp,
                     .reference_mean = s->d_ref_mu,
                     .comparison_mean = s->d_cmp_mu,
                     .reference_square = s->d_ref_sq,
                     .comparison_square = s->d_cmp_sq,
                     .cross_product = s->d_refcmp,
                     .width = s->width,
                     .output_width = s->w_horiz,
                     .output_height = s->h_horiz});
    launch_vert_combine(q, {.reference_mean = s->d_ref_mu,
                            .comparison_mean = s->d_cmp_mu,
                            .reference_square = s->d_ref_sq,
                            .comparison_square = s->d_cmp_sq,
                            .cross_product = s->d_refcmp,
                            .partials = s->d_partials,
                            .horizontal_width = s->w_horiz,
                            .final_width = s->w_final,
                            .final_height = s->h_final,
                            .group_columns = s->wg_count_x,
                            .c1 = s->c1,
                            .c2 = s->c2});

    q.memcpy(s->h_partials, s->d_partials, (size_t)s->wg_count * sizeof(float));

    s->pending_index = index;
    s->has_pending = true;
    return 0;
}

} // namespace

namespace
{

static int collect_fex_sycl(VmafFeatureExtractor *fex, unsigned index,
                            VmafFeatureCollector *feature_collector)
{
    auto *s = static_cast<SsimStateSycl *>(fex->priv);
    auto *qptr = static_cast<sycl::queue *>(vmaf_sycl_get_queue_ptr(s->sycl_state));
    if (!qptr)
        return -EINVAL;
    qptr->wait();

    /* Per-WG float partials → host double sum → mean SSIM
     * over (W-10)·(H-10) pixels. Same precision pattern as
     * ssim_vulkan / ssim_cuda. */
    double total = 0.0;
    for (unsigned i = 0; i < s->wg_count; i++)
        total += (double)s->h_partials[i];
    const double n_pixels = (double)s->w_final * (double)s->h_final;
    return vmaf_ssim_emit_ratio_score_named(feature_collector, s->feature_name_dict,
                                            "float_ssim_sycl", "float_ssim", total, n_pixels, 0,
                                            0.0, index);
}

} // namespace

namespace
{

template <typename T> static void release_buffer(VmafSyclState *state, T *pointer)
{
    if (pointer) {
        vmaf_sycl_free(state, pointer);
    }
}

} // namespace

namespace
{

static int close_fex_sycl(VmafFeatureExtractor *fex)
{
    auto *s = static_cast<SsimStateSycl *>(fex->priv);
    if (s->sycl_state) {
        release_buffer(s->sycl_state, s->h_ref);
        release_buffer(s->sycl_state, s->h_cmp);
        release_buffer(s->sycl_state, s->d_ref);
        release_buffer(s->sycl_state, s->d_cmp);
        release_buffer(s->sycl_state, s->d_ref_mu);
        release_buffer(s->sycl_state, s->d_cmp_mu);
        release_buffer(s->sycl_state, s->d_ref_sq);
        release_buffer(s->sycl_state, s->d_cmp_sq);
        release_buffer(s->sycl_state, s->d_refcmp);
        release_buffer(s->sycl_state, s->d_partials);
        release_buffer(s->sycl_state, s->h_partials);
    }
    if (s->feature_name_dict) {
        vmaf_dictionary_free(&s->feature_name_dict);
    }
    return 0;
}

static const char *provided_features_ssim_sycl[] = {"float_ssim", nullptr};

} // namespace

extern "C" VmafFeatureExtractor vmaf_fex_float_ssim_sycl = {
    .name = "float_ssim_sycl",
    .init = init_fex_sycl,
    .extract = nullptr,
    .flush = nullptr,
    .close = close_fex_sycl,
    .submit = submit_fex_sycl,
    .collect = collect_fex_sycl,
    .options = options_ssim_sycl,
    .priv_size = sizeof(SsimStateSycl),
    .flags = VMAF_FEATURE_EXTRACTOR_SYCL,
    .provided_features = provided_features_ssim_sycl,
    .chars =
        {
            .n_dispatches_per_frame = 2,
            .is_reduction_only = false,
            .min_useful_frame_area = 1920U * 1080U,
            .dispatch_hint = VMAF_FEATURE_DISPATCH_AUTO,
        },
};

/* ============================================================
 * Real integer_ssim SYCL extractor (ADR-0564)
 *
 * Bit-exact int64 moment accumulation matching the CPU integer_ssim.c.
 * Uses the same 9-tap integer Gaussian [2,9,28,55,68,55,28,9,2] and
 * boundary-truncation as the CPU.
 *
 * fp64 constraint (ADR-0220 / AGENTS.md): no double inside kernel
 * lambdas. The SSIM formula is computed in float32 per-pixel, then
 * float partials are accumulated on the host in double.  This gives
 * places=4-5 vs CPU (not places=6), documented in ADR-0564.
 *
 * Two-pass design:
 *   Pass 1 (launch_issim_horiz): 9-tap int64 horizontal moment
 *     accumulation. Writes 6 x (W x H) int64 USM arrays.
 *   Pass 2 (launch_issim_vert_combine): 9-tap int64 vertical
 *     accumulation from horiz arrays, then float SSIM formula,
 *     then float per-WG partial sum + int64 per-WG weight sum.
 * Host: ssim = sum(float_partials * wgt_partials) / sum(wgt_partials).
 * ============================================================ */

namespace
{

constexpr size_t ISSIM_WG_X = 16;
static constexpr size_t ISSIM_WG_Y = 8;
/* 9-tap integer Gaussian kernel matching gaussian_filter_init(sigma=1.5, max_len=5):
 * [2, 9, 28, 55, 68, 55, 28, 9, 2], sum=256, kernel_len=4. */
static constexpr int ISSIM_HALF_K = 4;
static constexpr int ISSIM_K_SZ = 9;
constexpr int32_t ISSIM_KERNEL[ISSIM_K_SZ] = {2, 9, 28, 55, 68, 55, 28, 9, 2};

} // namespace

namespace
{

struct IssimStateSycl {
    unsigned width;
    unsigned height;
    unsigned bpc;

    unsigned wg_count_x;
    unsigned wg_count_y;
    unsigned wg_count;

    VmafSyclState *sycl_state;

    /* Staging buffers: host-pinned input (packed, no stride). */
    uint8_t *h_ref_u8;
    uint8_t *h_cmp_u8;
    uint16_t *h_ref_u16;
    uint16_t *h_cmp_u16;

    /* Device USM input planes. */
    uint8_t *d_ref_u8;
    uint8_t *d_cmp_u8;
    uint16_t *d_ref_u16;
    uint16_t *d_cmp_u16;

    /* Six int64 intermediate device arrays for horizontal pass. */
    int64_t *d_mux;
    int64_t *d_muy;
    int64_t *d_x2;
    int64_t *d_xy;
    int64_t *d_y2;
    int64_t *d_w;

    /* float per-WG ssim partial sum. */
    float *d_partials;
    float *h_partials;
    /* int64 per-WG weight partial sum. */
    int64_t *d_wgt;
    int64_t *h_wgt;

    bool has_pending;
    unsigned pending_index;

    VmafDictionary *feature_name_dict;
};

} // namespace

/* Pass 1 (8bpc): horizontal 9-tap int64 moment accumulation. */
namespace
{

static void launch_issim_horiz_8bpc(sycl::queue &q, const uint8_t *d_ref, const uint8_t *d_cmp,
                                    int64_t *d_mux, int64_t *d_muy, int64_t *d_x2, int64_t *d_xy,
                                    int64_t *d_y2, int64_t *d_w, unsigned width, unsigned height)
{
    const size_t gx = ((width + ISSIM_WG_X - 1) / ISSIM_WG_X) * ISSIM_WG_X;
    const size_t gy = ((height + ISSIM_WG_Y - 1) / ISSIM_WG_Y) * ISSIM_WG_Y;
    const unsigned e_width = width;
    const unsigned e_height = height;
    q.submit([&](sycl::handler &cgh) {
        cgh.parallel_for(
            sycl::nd_range<2>{sycl::range<2>{gy, gx}, sycl::range<2>{ISSIM_WG_Y, ISSIM_WG_X}},
            [=](sycl::nd_item<2> it) {
                const unsigned x = (unsigned)it.get_global_id(1);
                const unsigned y = (unsigned)it.get_global_id(0);
                if (x >= e_width || y >= e_height)
                    return;
                const int k_min = (int)x < ISSIM_HALF_K ? ISSIM_HALF_K - (int)x : 0;
                const int k_max = ((int)x + ISSIM_HALF_K >= (int)e_width) ?
                                      ISSIM_K_SZ - ((int)x + ISSIM_HALF_K - (int)e_width + 1) :
                                      ISSIM_K_SZ;
                int64_t mux = 0LL;
                int64_t muy = 0LL;
                int64_t x2 = 0LL;
                int64_t xy = 0LL;
                int64_t y2 = 0LL;
                int64_t w = 0LL;
                for (int k = k_min; k < k_max; k++) {
                    const int src_x = (int)x - ISSIM_HALF_K + k;
                    const int64_t s = (int64_t)d_ref[(size_t)y * e_width + (unsigned)src_x];
                    const int64_t d = (int64_t)d_cmp[(size_t)y * e_width + (unsigned)src_x];
                    const int64_t wk = (int64_t)ISSIM_KERNEL[k];
                    mux += wk * s;
                    muy += wk * d;
                    x2 += wk * s * s;
                    xy += wk * s * d;
                    y2 += wk * d * d;
                    w += wk;
                }
                const size_t idx = (size_t)y * e_width + x;
                d_mux[idx] = mux;
                d_muy[idx] = muy;
                d_x2[idx] = x2;
                d_xy[idx] = xy;
                d_y2[idx] = y2;
                d_w[idx] = w;
            });
    });
}

} // namespace

/* Pass 1 (>8bpc): same as above but reads uint16_t. */
namespace
{

static void launch_issim_horiz_16bpc(sycl::queue &q, const uint16_t *d_ref, const uint16_t *d_cmp,
                                     int64_t *d_mux, int64_t *d_muy, int64_t *d_x2, int64_t *d_xy,
                                     int64_t *d_y2, int64_t *d_w, unsigned width, unsigned height)
{
    const size_t gx = ((width + ISSIM_WG_X - 1) / ISSIM_WG_X) * ISSIM_WG_X;
    const size_t gy = ((height + ISSIM_WG_Y - 1) / ISSIM_WG_Y) * ISSIM_WG_Y;
    const unsigned e_width = width;
    const unsigned e_height = height;
    q.submit([&](sycl::handler &cgh) {
        cgh.parallel_for(
            sycl::nd_range<2>{sycl::range<2>{gy, gx}, sycl::range<2>{ISSIM_WG_Y, ISSIM_WG_X}},
            [=](sycl::nd_item<2> it) {
                const unsigned x = (unsigned)it.get_global_id(1);
                const unsigned y = (unsigned)it.get_global_id(0);
                if (x >= e_width || y >= e_height)
                    return;
                const int k_min = (int)x < ISSIM_HALF_K ? ISSIM_HALF_K - (int)x : 0;
                const int k_max = ((int)x + ISSIM_HALF_K >= (int)e_width) ?
                                      ISSIM_K_SZ - ((int)x + ISSIM_HALF_K - (int)e_width + 1) :
                                      ISSIM_K_SZ;
                int64_t mux = 0LL;
                int64_t muy = 0LL;
                int64_t x2 = 0LL;
                int64_t xy = 0LL;
                int64_t y2 = 0LL;
                int64_t w = 0LL;
                for (int k = k_min; k < k_max; k++) {
                    const int src_x = (int)x - ISSIM_HALF_K + k;
                    const int64_t s = (int64_t)d_ref[(size_t)y * e_width + (unsigned)src_x];
                    const int64_t d = (int64_t)d_cmp[(size_t)y * e_width + (unsigned)src_x];
                    const int64_t wk = (int64_t)ISSIM_KERNEL[k];
                    mux += wk * s;
                    muy += wk * d;
                    x2 += wk * s * s;
                    xy += wk * s * d;
                    y2 += wk * d * d;
                    w += wk;
                }
                const size_t idx = (size_t)y * e_width + x;
                d_mux[idx] = mux;
                d_muy[idx] = muy;
                d_x2[idx] = x2;
                d_xy[idx] = xy;
                d_y2[idx] = y2;
                d_w[idx] = w;
            });
    });
}

} // namespace

namespace
{

struct IntegerVertArgs {
    const int64_t *reference_mean;
    const int64_t *comparison_mean;
    const int64_t *reference_square;
    const int64_t *cross_product;
    const int64_t *comparison_square;
    const int64_t *weight;
    float *partials;
    int64_t *weight_partials;
    unsigned width;
    unsigned height;
    float sample_max;
    size_t group_columns;
};

struct IntegerMoments {
    int64_t reference_mean;
    int64_t comparison_mean;
    int64_t reference_square;
    int64_t cross_product;
    int64_t comparison_square;
    int64_t weight;
};

struct IssimContribution {
    float weighted_score;
    float weight;
};

} // namespace

namespace
{

static inline IntegerMoments vertical_integer_moments(const IntegerVertArgs &args, unsigned x,
                                                      unsigned y)
{
    const int first = (int)y < ISSIM_HALF_K ? ISSIM_HALF_K - (int)y : 0;
    const int last = ((int)y + ISSIM_HALF_K >= (int)args.height) ?
                         ISSIM_K_SZ - ((int)y + ISSIM_HALF_K - (int)args.height + 1) :
                         ISSIM_K_SZ;
    IntegerMoments result{};
    for (int tap = first; tap < last; ++tap) {
        const unsigned source_y = (unsigned)((int)y - ISSIM_HALF_K + tap);
        const size_t index = (size_t)source_y * args.width + x;
        const int64_t coefficient = (int64_t)ISSIM_KERNEL[tap];
        result.reference_mean += coefficient * args.reference_mean[index];
        result.comparison_mean += coefficient * args.comparison_mean[index];
        result.reference_square += coefficient * args.reference_square[index];
        result.cross_product += coefficient * args.cross_product[index];
        result.comparison_square += coefficient * args.comparison_square[index];
        result.weight += coefficient * args.weight[index];
    }
    return result;
}

} // namespace

namespace
{

static inline IssimContribution integer_ssim_contribution(const IntegerVertArgs &args, unsigned x,
                                                          unsigned y)
{
    const IntegerMoments moments = vertical_integer_moments(args, x, y);
    const float weight = (float)moments.weight;
    const float c1 = args.sample_max * args.sample_max * 0.0001f * weight * weight;
    const float c2 = args.sample_max * args.sample_max * 0.0009f * weight * weight;
    const float reference_mean = (float)moments.reference_mean;
    const float comparison_mean = (float)moments.comparison_mean;
    const float reference_square = (float)moments.reference_square;
    const float cross_product = (float)moments.cross_product;
    const float comparison_square = (float)moments.comparison_square;
    const float mean_product = reference_mean * comparison_mean;
    const float numerator =
        (2.0f * mean_product + c1) * (2.0f * (cross_product * weight - mean_product) + c2);
    const float denominator =
        (reference_mean * reference_mean + comparison_mean * comparison_mean + c1) *
        (reference_square * weight - reference_mean * reference_mean + comparison_square * weight -
         comparison_mean * comparison_mean + c2);
    if (denominator == 0.0f || moments.weight <= 0LL) {
        return {};
    }
    return {.weighted_score = weight * (numerator / denominator), .weight = weight};
}

} // namespace

namespace
{

static inline void store_integer_group(sycl::nd_item<2> item, const IntegerVertArgs &args,
                                       IssimContribution contribution)
{
    const float score =
        sycl::reduce_over_group(item.get_group(), contribution.weighted_score, sycl::plus<float>{});
    const float weight =
        sycl::reduce_over_group(item.get_group(), contribution.weight, sycl::plus<float>{});
    if (item.get_local_id(0) == 0 && item.get_local_id(1) == 0) {
        const size_t index = item.get_group(0) * args.group_columns + item.get_group(1);
        args.partials[index] = score;
        args.weight_partials[index] = (int64_t)weight;
    }
}

} // namespace

namespace
{

static void launch_issim_vert_combine(sycl::queue &queue, const IntegerVertArgs &args)
{
    const size_t global_x = ((args.width + ISSIM_WG_X - 1) / ISSIM_WG_X) * ISSIM_WG_X;
    const size_t global_y = ((args.height + ISSIM_WG_Y - 1) / ISSIM_WG_Y) * ISSIM_WG_Y;
    sycl::nd_range<2> const range{sycl::range<2>{global_y, global_x},
                                  sycl::range<2>{ISSIM_WG_Y, ISSIM_WG_X}};
    queue.submit([=](sycl::handler &handler) {
        handler.parallel_for(range, [=](sycl::nd_item<2> item) {
            const unsigned x = (unsigned)item.get_global_id(1);
            const unsigned y = (unsigned)item.get_global_id(0);
            IssimContribution contribution{};
            if (x < args.width && y < args.height) {
                contribution = integer_ssim_contribution(args, x, y);
            }
            store_integer_group(item, args, contribution);
        });
    });
}

} // namespace

namespace
{

struct IntegerBufferSizes {
    size_t pixels8;
    size_t pixels16;
    size_t moments;
    size_t partials;
    size_t weights;
};

static int configure_integer_ssim(IssimStateSycl *s, unsigned bpc, unsigned width, unsigned height)
{
    if (width < 1u || height < 1u) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "integer_ssim_sycl: zero-dimension input %ux%u\n", width,
                 height);
        return -EINVAL;
    }
    s->width = width;
    s->height = height;
    s->bpc = bpc;
    s->wg_count_x = (unsigned)((width + ISSIM_WG_X - 1) / ISSIM_WG_X);
    s->wg_count_y = (unsigned)((height + ISSIM_WG_Y - 1) / ISSIM_WG_Y);
    s->wg_count = s->wg_count_x * s->wg_count_y;
    return 0;
}

} // namespace

namespace
{

static IntegerBufferSizes integer_buffer_sizes(const IssimStateSycl *s)
{
    const size_t pixels = (size_t)s->width * s->height;
    return {
        .pixels8 = pixels * sizeof(uint8_t),
        .pixels16 = pixels * sizeof(uint16_t),
        .moments = pixels * sizeof(int64_t),
        .partials = (size_t)s->wg_count * sizeof(float),
        .weights = (size_t)s->wg_count * sizeof(int64_t),
    };
}

} // namespace

namespace
{

static void allocate_integer_ssim(IssimStateSycl *s, const IntegerBufferSizes &bytes)
{
    s->h_ref_u8 = allocate_host<uint8_t>(s->sycl_state, bytes.pixels8);
    s->h_cmp_u8 = allocate_host<uint8_t>(s->sycl_state, bytes.pixels8);
    s->h_ref_u16 = allocate_host<uint16_t>(s->sycl_state, bytes.pixels16);
    s->h_cmp_u16 = allocate_host<uint16_t>(s->sycl_state, bytes.pixels16);
    s->d_ref_u8 = allocate_device<uint8_t>(s->sycl_state, bytes.pixels8);
    s->d_cmp_u8 = allocate_device<uint8_t>(s->sycl_state, bytes.pixels8);
    s->d_ref_u16 = allocate_device<uint16_t>(s->sycl_state, bytes.pixels16);
    s->d_cmp_u16 = allocate_device<uint16_t>(s->sycl_state, bytes.pixels16);
    s->d_mux = allocate_device<int64_t>(s->sycl_state, bytes.moments);
    s->d_muy = allocate_device<int64_t>(s->sycl_state, bytes.moments);
    s->d_x2 = allocate_device<int64_t>(s->sycl_state, bytes.moments);
    s->d_xy = allocate_device<int64_t>(s->sycl_state, bytes.moments);
    s->d_y2 = allocate_device<int64_t>(s->sycl_state, bytes.moments);
    s->d_w = allocate_device<int64_t>(s->sycl_state, bytes.moments);
    s->d_partials = allocate_device<float>(s->sycl_state, bytes.partials);
    s->h_partials = allocate_host<float>(s->sycl_state, bytes.partials);
    s->d_wgt = allocate_device<int64_t>(s->sycl_state, bytes.weights);
    s->h_wgt = allocate_host<int64_t>(s->sycl_state, bytes.weights);
}

} // namespace

namespace
{

static bool integer_ssim_allocations_complete(const IssimStateSycl *s)
{
    return s->h_ref_u8 && s->h_cmp_u8 && s->h_ref_u16 && s->h_cmp_u16 && s->d_ref_u8 &&
           s->d_cmp_u8 && s->d_ref_u16 && s->d_cmp_u16 && s->d_mux && s->d_muy && s->d_x2 &&
           s->d_xy && s->d_y2 && s->d_w && s->d_partials && s->h_partials && s->d_wgt && s->h_wgt;
}

} // namespace

namespace
{

static int init_fex_issim_sycl(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt,
                               unsigned bpc, unsigned width, unsigned height)
{
    (void)pix_fmt;
    auto *s = static_cast<IssimStateSycl *>(fex->priv);
    const int config_error = configure_integer_ssim(s, bpc, width, height);
    if (config_error) {
        return config_error;
    }
    if (!fex->sycl_state) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "integer_ssim_sycl: no SYCL state\n");
        return -EINVAL;
    }
    s->sycl_state = fex->sycl_state;
    allocate_integer_ssim(s, integer_buffer_sizes(s));
    if (!integer_ssim_allocations_complete(s)) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "integer_ssim_sycl: USM allocation failed\n");
        return -ENOMEM;
    }
    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (!s->feature_name_dict) {
        return -ENOMEM;
    }
    s->has_pending = false;
    return 0;
}

} // namespace

namespace
{

template <typename T>
static void pack_integer_plane(T *destination, const VmafPicture *picture, unsigned width,
                               unsigned height)
{
    const auto *source = static_cast<const uint8_t *>(picture->data[0]);
    for (unsigned y = 0; y < height; ++y) {
        __builtin_memcpy(destination + (size_t)y * width, source + (size_t)y * picture->stride[0],
                         (size_t)width * sizeof(T));
    }
}

} // namespace

namespace
{

template <typename Picture>
static void submit_integer_horizontal(IssimStateSycl *s, sycl::queue &queue, Picture *reference,
                                      Picture *comparison)
{
    const size_t pixels = (size_t)s->width * s->height;
    if (s->bpc == 8u) {
        pack_integer_plane(s->h_ref_u8, reference, s->width, s->height);
        pack_integer_plane(s->h_cmp_u8, comparison, s->width, s->height);
        queue.memcpy(s->d_ref_u8, s->h_ref_u8, pixels * sizeof(uint8_t));
        queue.memcpy(s->d_cmp_u8, s->h_cmp_u8, pixels * sizeof(uint8_t));
        launch_issim_horiz_8bpc(queue, s->d_ref_u8, s->d_cmp_u8, s->d_mux, s->d_muy, s->d_x2,
                                s->d_xy, s->d_y2, s->d_w, s->width, s->height);
        return;
    }
    pack_integer_plane(s->h_ref_u16, reference, s->width, s->height);
    pack_integer_plane(s->h_cmp_u16, comparison, s->width, s->height);
    queue.memcpy(s->d_ref_u16, s->h_ref_u16, pixels * sizeof(uint16_t));
    queue.memcpy(s->d_cmp_u16, s->h_cmp_u16, pixels * sizeof(uint16_t));
    launch_issim_horiz_16bpc(queue, s->d_ref_u16, s->d_cmp_u16, s->d_mux, s->d_muy, s->d_x2,
                             s->d_xy, s->d_y2, s->d_w, s->width, s->height);
}

} // namespace

namespace
{

static int submit_fex_issim_sycl(VmafFeatureExtractor *fex, VmafPicture *ref_pic,
                                 VmafPicture *ref_pic_90, VmafPicture *dist_pic,
                                 VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic_90;
    (void)dist_pic_90;
    auto *s = static_cast<IssimStateSycl *>(fex->priv);
    auto *qptr = static_cast<sycl::queue *>(vmaf_sycl_get_queue_ptr(s->sycl_state));
    if (!qptr)
        return -EINVAL;
    sycl::queue &q = *qptr;

    submit_integer_horizontal(s, q, ref_pic, dist_pic);
    launch_issim_vert_combine(q, {.reference_mean = s->d_mux,
                                  .comparison_mean = s->d_muy,
                                  .reference_square = s->d_x2,
                                  .cross_product = s->d_xy,
                                  .comparison_square = s->d_y2,
                                  .weight = s->d_w,
                                  .partials = s->d_partials,
                                  .weight_partials = s->d_wgt,
                                  .width = s->width,
                                  .height = s->height,
                                  .sample_max = (float)((1u << s->bpc) - 1u),
                                  .group_columns = s->wg_count_x});

    q.memcpy(s->h_partials, s->d_partials, (size_t)s->wg_count * sizeof(float));
    q.memcpy(s->h_wgt, s->d_wgt, (size_t)s->wg_count * sizeof(int64_t));

    s->pending_index = index;
    s->has_pending = true;
    return 0;
}

} // namespace

namespace
{

static int collect_fex_issim_sycl(VmafFeatureExtractor *fex, unsigned index,
                                  VmafFeatureCollector *feature_collector)
{
    auto *s = static_cast<IssimStateSycl *>(fex->priv);
    auto *qptr = static_cast<sycl::queue *>(vmaf_sycl_get_queue_ptr(s->sycl_state));
    if (!qptr)
        return -EINVAL;
    qptr->wait();

    double total_ssim = 0.0;
    int64_t total_wgt = 0LL;
    for (unsigned i = 0; i < s->wg_count; i++) {
        total_ssim += (double)s->h_partials[i];
        total_wgt += s->h_wgt[i];
    }
    return vmaf_ssim_emit_ratio_score_named(feature_collector, s->feature_name_dict,
                                            "integer_ssim_sycl", "ssim", total_ssim,
                                            (double)total_wgt, 0, 0.0, index);
}

} // namespace

namespace
{

static int close_fex_issim_sycl(VmafFeatureExtractor *fex)
{
    auto *s = static_cast<IssimStateSycl *>(fex->priv);
    if (s->sycl_state) {
        release_buffer(s->sycl_state, s->h_ref_u8);
        release_buffer(s->sycl_state, s->h_cmp_u8);
        release_buffer(s->sycl_state, s->h_ref_u16);
        release_buffer(s->sycl_state, s->h_cmp_u16);
        release_buffer(s->sycl_state, s->d_ref_u8);
        release_buffer(s->sycl_state, s->d_cmp_u8);
        release_buffer(s->sycl_state, s->d_ref_u16);
        release_buffer(s->sycl_state, s->d_cmp_u16);
        release_buffer(s->sycl_state, s->d_mux);
        release_buffer(s->sycl_state, s->d_muy);
        release_buffer(s->sycl_state, s->d_x2);
        release_buffer(s->sycl_state, s->d_xy);
        release_buffer(s->sycl_state, s->d_y2);
        release_buffer(s->sycl_state, s->d_w);
        release_buffer(s->sycl_state, s->d_partials);
        release_buffer(s->sycl_state, s->h_partials);
        release_buffer(s->sycl_state, s->d_wgt);
        release_buffer(s->sycl_state, s->h_wgt);
    }
    if (s->feature_name_dict) {
        (void)vmaf_dictionary_free(&s->feature_name_dict);
    }
    return 0;
}

static const VmafOption options_issim_sycl[] = {
    {.name = nullptr},
};

static const char *provided_features_issim_sycl[] = {"ssim", nullptr};

} // namespace

/* Real integer_ssim SYCL extractor (ADR-0564). Uses 9-tap int64 moments
 * matching the CPU algorithm. The SSIM formula is computed in float32
 * (fp64-free constraint, ADR-0220); expected precision is places=4-5 vs
 * CPU. Load-bearing: declared via extern in feature_extractor.c. */
extern "C" VmafFeatureExtractor vmaf_fex_integer_ssim_sycl = {
    .name = "integer_ssim_sycl",
    .init = init_fex_issim_sycl,
    .extract = nullptr,
    .flush = nullptr,
    .close = close_fex_issim_sycl,
    .submit = submit_fex_issim_sycl,
    .collect = collect_fex_issim_sycl,
    .options = options_issim_sycl,
    .priv_size = sizeof(IssimStateSycl),
    .flags = VMAF_FEATURE_EXTRACTOR_SYCL,
    .provided_features = provided_features_issim_sycl,
    .chars =
        {
            .n_dispatches_per_frame = 2,
            .is_reduction_only = false,
            .min_useful_frame_area = 1920U * 1080U,
            .dispatch_hint = VMAF_FEATURE_DISPATCH_AUTO,
        },
};
