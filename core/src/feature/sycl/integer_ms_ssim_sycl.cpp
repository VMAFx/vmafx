/* Upstream-mirror filename: defines float_ms_ssim symbol despite the integer_ prefix (matches Netflix upstream). See ADR-0549. */
/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright (c) 2011, Tom Distler (http://tdistler.com)
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent AND BSD-3-Clause
 *
 *  float_ms_ssim feature extractor on the SYCL backend
 *  (T7-23 / ADR-0188 / ADR-0190, GPU long-tail batch 2 part 2c).
 *  SYCL twin of ms_ssim_vulkan (PR #141) and ms_ssim_cuda (this
 *  PR's batch 2 part 2b).
 *
 *  Self-contained submit / collect — does *not* register with
 *  vmaf_sycl_graph_register because shared_frame is luma-only
 *  packed at uint width and MS-SSIM needs picture_copy-normalised
 *  float planes + a 5-level pyramid. Same pattern as ssim_sycl
 *  (PR #140).
 *
 *  5-level pyramid + 3-output SSIM per scale + host-side Wang
 *  product combine. Three SYCL kernels:
 *    - decimate (9-tap 9/7 biorthogonal LPF + 2× downsample)
 *    - horiz (11-tap separable Gaussian over 5 stats)
 *    - vert+lcs (vertical 11-tap + per-pixel l/c/s + per-WG
 *      partials × 3 via reduce_over_group)
 *
 *  fp64-free (Intel Arc A380 lacks native fp64 — same constraint
 *  as ssim_sycl).
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
#include "log.h"
#include "picture.h"
#include "../picture_copy.h"
#include "sycl/common.h"

namespace
{

static constexpr int MS_SSIM_SCALES = 5;
/* Luma plus the two chroma planes. enable_chroma selects 1 or 3 of them; the
 * pyramid, the staging buffers and every derived dimension are per plane,
 * because a 4:2:0 chroma plane is a different size from luma and the kernels
 * take those dimensions as row pitches. */
static constexpr int MS_SSIM_MAX_PLANES = 3;
static constexpr int MS_SSIM_GAUSSIAN_LEN = 11;
static constexpr int MS_SSIM_K = 11;
static constexpr int LPF_LEN = 9;
static constexpr int LPF_HALF = 4;
static constexpr size_t WG_X = 16;
static constexpr size_t WG_Y = 8;

static constexpr float G[MS_SSIM_K] = {
    0.001028f, 0.007599f, 0.036001f, 0.109361f, 0.213006f, 0.266012f,
    0.213006f, 0.109361f, 0.036001f, 0.007599f, 0.001028f,
};

static constexpr float LPF[LPF_LEN] = {
    0.026727f, -0.016828f, -0.078201f, 0.266846f, 0.602914f,
    0.266846f, -0.078201f, -0.016828f, 0.026727f,
};

static constexpr float ALPHAS[MS_SSIM_SCALES] = {0.0f, 0.0f, 0.0f, 0.0f, 0.1333f};
static constexpr float BETAS[MS_SSIM_SCALES] = {0.0448f, 0.2856f, 0.3001f, 0.2363f, 0.1333f};
static constexpr float GAMMAS[MS_SSIM_SCALES] = {0.0448f, 0.2856f, 0.3001f, 0.2363f, 0.1333f};

} // namespace

namespace
{

/* Everything the kernels need that varies per plane. Held once per plane so a
 * chroma pass never reads a luma row pitch -- the failure that would produce is
 * a silent wrong number plus an out-of-bounds read, not a crash. */
struct MsSsimPlaneGeometry {
    unsigned width;
    unsigned height;
    unsigned scale_w[MS_SSIM_SCALES];
    unsigned scale_h[MS_SSIM_SCALES];
    unsigned scale_w_horiz[MS_SSIM_SCALES];
    unsigned scale_h_horiz[MS_SSIM_SCALES];
    unsigned scale_w_final[MS_SSIM_SCALES];
    unsigned scale_h_final[MS_SSIM_SCALES];
    unsigned scale_wg_count_x[MS_SSIM_SCALES];
    unsigned scale_wg_count_y[MS_SSIM_SCALES];
    unsigned scale_wg_count[MS_SSIM_SCALES];
    float *h_ref;
    float *h_cmp;
    float *d_pyramid_ref[MS_SSIM_SCALES];
    float *d_pyramid_cmp[MS_SSIM_SCALES];
};

struct MsSsimStateSycl {
    bool enable_chroma;
    unsigned n_planes;
    bool enable_lcs;
    bool enable_db;
    bool clip_db;
    double max_db;
    unsigned width;
    unsigned height;
    unsigned bpc;
    MsSsimPlaneGeometry geom[MS_SSIM_MAX_PLANES];
    float c1, c2, c3;
    VmafSyclState *sycl_state;
    /* The reduction workspace below is deliberately NOT per plane. It is already
     * reused across the five scales, and no chroma plane is larger than luma in
     * any supported pixel format, so the plane-0 sizing dominates. Planes run
     * sequentially for the same reason the scales do. */
    float *d_h_ref_mu;
    float *d_h_cmp_mu;
    float *d_h_ref_sq;
    float *d_h_cmp_sq;
    float *d_h_refcmp;
    float *d_l_partials;
    float *d_c_partials;
    float *d_s_partials;
    float *h_l_partials;
    float *h_c_partials;
    float *h_s_partials;
    bool has_pending;
    unsigned pending_index;
    VmafDictionary *feature_name_dict;
};

struct DecimateArgs {
    const float *source;
    float *destination;
    unsigned width;
    unsigned height;
    unsigned output_width;
    unsigned output_height;
};

struct LcsValues {
    float luminance;
    float contrast;
    float structure;
};

} // namespace

namespace
{

/* Period-2n mirror — matches ms_ssim_decimate.c::ms_ssim_decimate_mirror. */
static inline int mirror_idx(int idx, int n)
{
    int const period = 2 * n;
    int r = idx % period;
    if (r < 0) {
        r += period;
    }
    if (r >= n) {
        r = period - r - 1;
    }
    return r;
}

} // namespace

namespace
{

static inline float decimate_pixel(const DecimateArgs &args, size_t output_x, size_t output_y)
{
    int const source_x = (int)output_x * 2;
    int const source_y = (int)output_y * 2;
    float sum = 0.0f;
    for (int vertical = 0; vertical < LPF_LEN; ++vertical) {
        int const y = mirror_idx(source_y + vertical - LPF_HALF, (int)args.height);
        float row_sum = 0.0f;
        for (int horizontal = 0; horizontal < LPF_LEN; ++horizontal) {
            int const x = mirror_idx(source_x + horizontal - LPF_HALF, (int)args.width);
            row_sum += args.source[y * (int)args.width + x] * LPF[horizontal];
        }
        sum += row_sum * LPF[vertical];
    }
    return sum;
}

} // namespace

namespace
{

static void launch_decimate(sycl::queue &q, DecimateArgs args)
{
    sycl::range<2> const global{(size_t)args.output_height, (size_t)args.output_width};
    q.submit([=](sycl::handler &h_) {
        h_.parallel_for(global, [=](sycl::id<2> id) {
            const size_t y_out = id[0];
            const size_t x_out = id[1];
            if (x_out >= (size_t)args.output_width || y_out >= (size_t)args.output_height) {
                return;
            }
            args.destination[y_out * (size_t)args.output_width + x_out] =
                decimate_pixel(args, x_out, y_out);
        });
    });
}

} // namespace

namespace
{

static void launch_horiz(sycl::queue &q, const float *ref, const float *cmp, float *h_ref_mu,
                         float *h_cmp_mu, float *h_ref_sq, float *h_cmp_sq, float *h_refcmp,
                         unsigned width, unsigned w_horiz, unsigned h_horiz)
{
    sycl::range<2> const global{(size_t)h_horiz, (size_t)w_horiz};
    const unsigned e_w = width;
    const unsigned e_w_horiz = w_horiz;
    const unsigned e_h_horiz = h_horiz;
    const float *e_ref = ref;
    const float *e_cmp = cmp;
    float *e_h_ref_mu = h_ref_mu;
    float *e_h_cmp_mu = h_cmp_mu;
    float *e_h_ref_sq = h_ref_sq;
    float *e_h_cmp_sq = h_cmp_sq;
    float *e_h_refcmp = h_refcmp;

    q.submit([=](sycl::handler &h_) {
        h_.parallel_for(global, [=](sycl::id<2> id) {
            const size_t y = id[0];
            const size_t x = id[1];
            if (x >= (size_t)e_w_horiz || y >= (size_t)e_h_horiz)
                return;
            float ref_mu = 0.0f;
            float cmp_mu = 0.0f;
            float ref_sq = 0.0f;
            float cmp_sq = 0.0f;
            float refcmp = 0.0f;
            for (int u = 0; u < MS_SSIM_K; ++u) {
                const size_t src_idx = y * (size_t)e_w + (x + (size_t)u);
                const float r = e_ref[src_idx];
                const float c = e_cmp[src_idx];
                const float w = G[u];
                ref_mu += w * r;
                cmp_mu += w * c;
                ref_sq += w * (r * r);
                cmp_sq += w * (c * c);
                refcmp += w * (r * c);
            }
            const size_t dst_idx = y * (size_t)e_w_horiz + x;
            e_h_ref_mu[dst_idx] = ref_mu;
            e_h_cmp_mu[dst_idx] = cmp_mu;
            e_h_ref_sq[dst_idx] = ref_sq;
            e_h_cmp_sq[dst_idx] = cmp_sq;
            e_h_refcmp[dst_idx] = refcmp;
        });
    });
}

} // namespace

namespace
{

struct VertArgs {
    const float *ref_mu;
    const float *cmp_mu;
    const float *ref_sq;
    const float *cmp_sq;
    const float *refcmp;
    float *luminance;
    float *contrast;
    float *structure;
    unsigned horizontal_width;
    unsigned final_width;
    unsigned final_height;
    size_t group_columns;
    float c1;
    float c2;
    float c3;
};

} // namespace

namespace
{

static inline LcsValues vertical_lcs_pixel(const VertArgs &args, size_t x, size_t y)
{
    float ref_mu = 0.0f;
    float cmp_mu = 0.0f;
    float ref_sq = 0.0f;
    float cmp_sq = 0.0f;
    float refcmp = 0.0f;
    for (int tap = 0; tap < MS_SSIM_K; ++tap) {
        const size_t index = (y + (size_t)tap) * args.horizontal_width + x;
        const float weight = G[tap];
        ref_mu += weight * args.ref_mu[index];
        cmp_mu += weight * args.cmp_mu[index];
        ref_sq += weight * args.ref_sq[index];
        cmp_sq += weight * args.cmp_sq[index];
        refcmp += weight * args.refcmp[index];
    }
    const float ref_variance = sycl::fmax(ref_sq - ref_mu * ref_mu, 0.0f);
    const float cmp_variance = sycl::fmax(cmp_sq - cmp_mu * cmp_mu, 0.0f);
    const float covariance = refcmp - ref_mu * cmp_mu;
    const float geometric = sycl::sqrt(ref_variance * cmp_variance);
    const float clamped = (covariance < 0.0f && geometric <= 0.0f) ? 0.0f : covariance;
    return {
        .luminance =
            (2.0f * ref_mu * cmp_mu + args.c1) / (ref_mu * ref_mu + cmp_mu * cmp_mu + args.c1),
        .contrast = (2.0f * geometric + args.c2) / (ref_variance + cmp_variance + args.c2),
        .structure = (clamped + args.c3) / (geometric + args.c3),
    };
}

} // namespace

namespace
{

static inline void store_lcs_group(sycl::nd_item<2> item, const VertArgs &args, LcsValues values)
{
    float const luminance =
        sycl::reduce_over_group(item.get_group(), values.luminance, sycl::plus<float>{});
    float const contrast =
        sycl::reduce_over_group(item.get_group(), values.contrast, sycl::plus<float>{});
    float const structure =
        sycl::reduce_over_group(item.get_group(), values.structure, sycl::plus<float>{});
    if (item.get_local_id(0) == 0 && item.get_local_id(1) == 0) {
        const size_t index = item.get_group(0) * args.group_columns + item.get_group(1);
        args.luminance[index] = luminance;
        args.contrast[index] = contrast;
        args.structure[index] = structure;
    }
}

} // namespace

namespace
{

static void launch_vert_lcs(sycl::queue &q, const VertArgs &args)
{
    const size_t global_x = ((args.final_width + WG_X - 1) / WG_X) * WG_X;
    const size_t global_y = ((args.final_height + WG_Y - 1) / WG_Y) * WG_Y;
    sycl::nd_range<2> const ndr{sycl::range<2>{global_y, global_x}, sycl::range<2>{WG_Y, WG_X}};
    q.submit([=](sycl::handler &h_) {
        h_.parallel_for(ndr, [=](sycl::nd_item<2> it) {
            const size_t x = it.get_global_id(1);
            const size_t y = it.get_global_id(0);
            LcsValues values = {};
            if (x < (size_t)args.final_width && y < (size_t)args.final_height) {
                values = vertical_lcs_pixel(args, x, y);
            }
            store_lcs_group(it, args, values);
        });
    });
}

} // namespace

namespace
{

static const VmafOption options_ms_ssim_sycl[] = {
    {
        .name = "enable_lcs",
        .help = "enable luminance, contrast and structure intermediate output",
        .offset = offsetof(MsSsimStateSycl, enable_lcs),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val = {.b = false},
    },
    {
        .name = "enable_db",
        .help = "return dB-domain MS-SSIM score: -10*log10(1 - ms_ssim)",
        .offset = offsetof(MsSsimStateSycl, enable_db),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val = {.b = false},
    },
    {
        .name = "clip_db",
        .help = "clip linear ms_ssim to [0, 1] before dB conversion",
        .offset = offsetof(MsSsimStateSycl, clip_db),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val = {.b = false},
    },
    {
        .name = "enable_chroma",
        .help = "enable calculation for chroma channels (mirrors CPU PR #939 / "
                "ms_ssim_vulkan PR #957; v1 kernel defers multi-plane dispatch to v2)",
        .offset = offsetof(MsSsimStateSycl, enable_chroma),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val = {.b = false},
    },
    {.name = nullptr},
};

} // namespace

namespace
{

/* Mirrors float_ms_ssim.c::convert_to_db exactly. ADR-1221. */
static double ms_ssim_convert_to_db(double score, double max_db)
{
    /* score >= 1.0 makes log10(1-score) undefined (log10 of zero or negative)
     * yielding -Inf / NaN.  Return max_db directly for perfect similarity.  */
    if (score >= 1.0) {
        return max_db;
    }
    const double db = -10. * std::log10(1.0 - score);
    return db < max_db ? db : max_db;
}

} // namespace

namespace
{

static int configure_ms_ssim(MsSsimStateSycl *s, enum VmafPixelFormat format, unsigned bpc,
                             unsigned width, unsigned height)
{
    s->n_planes = (format == VMAF_PIX_FMT_YUV400P || !s->enable_chroma) ? 1U : 3U;
    const unsigned min_dimension = (unsigned)MS_SSIM_GAUSSIAN_LEN << (MS_SSIM_SCALES - 1);
    if (width < min_dimension || height < min_dimension) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "ms_ssim_sycl: input %ux%u too small; %d-level %d-tap MS-SSIM pyramid"
                 " needs >= %ux%u (Netflix#1414 / ADR-0153)\n",
                 width, height, MS_SSIM_SCALES, MS_SSIM_GAUSSIAN_LEN, min_dimension, min_dimension);
        return -EINVAL;
    }
    /* Chroma is walked by the same 5-level pyramid, so it must clear the same
     * minimum. Mirrors the check float_ms_ssim.c makes; without it a 4:2:0
     * input between min_dimension and 2*min_dimension passes the luma test and
     * then produces a degenerate chroma pyramid. Plane sizing follows
     * vmaf_picture_alloc (picture.c:146-149). */
    const unsigned ss_hor = format != VMAF_PIX_FMT_YUV444P ? 1u : 0u;
    const unsigned ss_ver = format == VMAF_PIX_FMT_YUV420P ? 1u : 0u;
    if (s->n_planes > 1U) {
        const unsigned chroma_w = (width + ss_hor) >> ss_hor;
        const unsigned chroma_h = (height + ss_ver) >> ss_ver;
        if (chroma_w < min_dimension || chroma_h < min_dimension) {
            vmaf_log(VMAF_LOG_LEVEL_ERROR,
                     "ms_ssim_sycl: enable_chroma needs every plane to clear the pyramid"
                     " minimum, but %ux%u luma gives %ux%u chroma and the %d-level %d-tap"
                     " pyramid requires at least %ux%u. Use at least %ux%u luma for this"
                     " pixel format, or leave enable_chroma off to score luma only.\n",
                     width, height, chroma_w, chroma_h, MS_SSIM_SCALES, MS_SSIM_GAUSSIAN_LEN,
                     min_dimension, min_dimension, min_dimension << ss_hor,
                     min_dimension << ss_ver);
            return -EINVAL;
        }
    }

    s->width = width;
    s->height = height;
    s->bpc = bpc;
    const unsigned peak = (1u << bpc) - 1u;
    if (s->clip_db) {
        const double mse = 0.5 / ((double)width * (double)height);
        s->max_db = std::ceil(10. * std::log10(peak * peak / mse));
    } else {
        s->max_db = INFINITY;
    }
    for (unsigned plane = 0; plane < s->n_planes; plane++) {
        MsSsimPlaneGeometry &geometry = s->geom[plane];
        geometry.width = plane == 0U ? width : (width + ss_hor) >> ss_hor;
        geometry.height = plane == 0U ? height : (height + ss_ver) >> ss_ver;
        geometry.scale_w[0] = geometry.width;
        geometry.scale_h[0] = geometry.height;
        for (int scale = 1; scale < MS_SSIM_SCALES; scale++) {
            geometry.scale_w[scale] =
                (geometry.scale_w[scale - 1] / 2) + (geometry.scale_w[scale - 1] & 1);
            geometry.scale_h[scale] =
                (geometry.scale_h[scale - 1] / 2) + (geometry.scale_h[scale - 1] & 1);
        }
    }
    return 0;
}

} // namespace

namespace
{

static void configure_ms_ssim_scales(MsSsimStateSycl *s)
{
    for (unsigned plane = 0; plane < s->n_planes; plane++) {
        MsSsimPlaneGeometry &geometry = s->geom[plane];
        for (int scale = 0; scale < MS_SSIM_SCALES; scale++) {
            geometry.scale_w_horiz[scale] = geometry.scale_w[scale] - (MS_SSIM_K - 1);
            geometry.scale_h_horiz[scale] = geometry.scale_h[scale];
            geometry.scale_w_final[scale] = geometry.scale_w[scale] - (MS_SSIM_K - 1);
            geometry.scale_h_final[scale] = geometry.scale_h[scale] - (MS_SSIM_K - 1);
            geometry.scale_wg_count_x[scale] =
                (geometry.scale_w_final[scale] + (unsigned)WG_X - 1) / (unsigned)WG_X;
            geometry.scale_wg_count_y[scale] =
                (geometry.scale_h_final[scale] + (unsigned)WG_Y - 1) / (unsigned)WG_Y;
            geometry.scale_wg_count[scale] =
                geometry.scale_wg_count_x[scale] * geometry.scale_wg_count_y[scale];
        }
    }
    const float range = 255.0f;
    const float k1 = 0.01f;
    const float k2 = 0.03f;
    s->c1 = (k1 * range) * (k1 * range);
    s->c2 = (k2 * range) * (k2 * range);
    s->c3 = s->c2 * 0.5f;
}

} // namespace

namespace
{

static void allocate_ms_ssim_buffers(MsSsimStateSycl *s)
{
    for (unsigned plane = 0; plane < s->n_planes; plane++) {
        MsSsimPlaneGeometry &geometry = s->geom[plane];
        const size_t input_bytes = (size_t)geometry.width * geometry.height * sizeof(float);
        geometry.h_ref = static_cast<float *>(vmaf_sycl_malloc_host(s->sycl_state, input_bytes));
        geometry.h_cmp = static_cast<float *>(vmaf_sycl_malloc_host(s->sycl_state, input_bytes));
        for (int scale = 0; scale < MS_SSIM_SCALES; scale++) {
            const size_t bytes =
                (size_t)geometry.scale_w[scale] * geometry.scale_h[scale] * sizeof(float);
            geometry.d_pyramid_ref[scale] =
                static_cast<float *>(vmaf_sycl_malloc_device(s->sycl_state, bytes));
            geometry.d_pyramid_cmp[scale] =
                static_cast<float *>(vmaf_sycl_malloc_device(s->sycl_state, bytes));
        }
    }
    /* Plane 0 dominates: chroma is never wider or taller than luma in any
     * supported pixel format, so these sizes cover every plane. */
    const size_t horizontal_bytes =
        (size_t)s->geom[0].scale_w_horiz[0] * s->geom[0].scale_h_horiz[0] * sizeof(float);
    s->d_h_ref_mu = static_cast<float *>(vmaf_sycl_malloc_device(s->sycl_state, horizontal_bytes));
    s->d_h_cmp_mu = static_cast<float *>(vmaf_sycl_malloc_device(s->sycl_state, horizontal_bytes));
    s->d_h_ref_sq = static_cast<float *>(vmaf_sycl_malloc_device(s->sycl_state, horizontal_bytes));
    s->d_h_cmp_sq = static_cast<float *>(vmaf_sycl_malloc_device(s->sycl_state, horizontal_bytes));
    s->d_h_refcmp = static_cast<float *>(vmaf_sycl_malloc_device(s->sycl_state, horizontal_bytes));
    const size_t partial_bytes = (size_t)s->geom[0].scale_wg_count[0] * sizeof(float);
    s->d_l_partials = static_cast<float *>(vmaf_sycl_malloc_device(s->sycl_state, partial_bytes));
    s->d_c_partials = static_cast<float *>(vmaf_sycl_malloc_device(s->sycl_state, partial_bytes));
    s->d_s_partials = static_cast<float *>(vmaf_sycl_malloc_device(s->sycl_state, partial_bytes));
    s->h_l_partials = static_cast<float *>(vmaf_sycl_malloc_host(s->sycl_state, partial_bytes));
    s->h_c_partials = static_cast<float *>(vmaf_sycl_malloc_host(s->sycl_state, partial_bytes));
    s->h_s_partials = static_cast<float *>(vmaf_sycl_malloc_host(s->sycl_state, partial_bytes));
}

} // namespace

namespace
{

static bool ms_ssim_allocations_complete(const MsSsimStateSycl *s)
{
    if (!s->d_h_ref_mu || !s->d_h_cmp_mu || !s->d_h_ref_sq || !s->d_h_cmp_sq || !s->d_h_refcmp ||
        !s->d_l_partials || !s->d_c_partials || !s->d_s_partials || !s->h_l_partials ||
        !s->h_c_partials || !s->h_s_partials) {
        return false;
    }
    for (unsigned plane = 0; plane < s->n_planes; plane++) {
        const MsSsimPlaneGeometry &geometry = s->geom[plane];
        if (!geometry.h_ref || !geometry.h_cmp) {
            return false;
        }
        for (int scale = 0; scale < MS_SSIM_SCALES; scale++) {
            if (!geometry.d_pyramid_ref[scale] || !geometry.d_pyramid_cmp[scale]) {
                return false;
            }
        }
    }
    return true;
}

} // namespace

namespace
{

static int init_fex_sycl(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                         unsigned w, unsigned h)
{
    auto *s = static_cast<MsSsimStateSycl *>(fex->priv);

    const int config_err = configure_ms_ssim(s, pix_fmt, bpc, w, h);
    if (config_err) {
        return config_err;
    }

    configure_ms_ssim_scales(s);
    if (!fex->sycl_state) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "ms_ssim_sycl: no SYCL state\n");
        return -EINVAL;
    }
    s->sycl_state = fex->sycl_state;

    allocate_ms_ssim_buffers(s);
    if (!ms_ssim_allocations_complete(s)) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "ms_ssim_sycl: USM allocation failed\n");
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
    auto *s = static_cast<MsSsimStateSycl *>(fex->priv);
    auto *qptr = static_cast<sycl::queue *>(vmaf_sycl_get_queue_ptr(s->sycl_state));
    if (!qptr) {
        return -EINVAL;
    }
    sycl::queue &q = *qptr;

    /* Every plane is staged here, not in collect: libvmaf's double-buffered GPU
     * dispatch (libvmaf.c, dispatch_gpu_double_buffer) releases the picture
     * after submit returns, so collect has no VmafPicture to read from. The
     * per-plane pyramids therefore all live until collect consumes them. */
    for (unsigned plane = 0; plane < s->n_planes; plane++) {
        MsSsimPlaneGeometry &geometry = s->geom[plane];
        const ptrdiff_t stride = (ptrdiff_t)((size_t)geometry.width * sizeof(float));
        picture_copy(geometry.h_ref, stride, ref_pic, 0, ref_pic->bpc, (int)plane);
        picture_copy(geometry.h_cmp, stride, dist_pic, 0, dist_pic->bpc, (int)plane);

        const size_t input_bytes = (size_t)geometry.width * geometry.height * sizeof(float);
        q.memcpy(geometry.d_pyramid_ref[0], geometry.h_ref, input_bytes);
        q.memcpy(geometry.d_pyramid_cmp[0], geometry.h_cmp, input_bytes);

        /* Build pyramid scales 1..4. */
        for (int i = 0; i < MS_SSIM_SCALES - 1; i++) {
            launch_decimate(q, {.source = geometry.d_pyramid_ref[i],
                                .destination = geometry.d_pyramid_ref[i + 1],
                                .width = geometry.scale_w[i],
                                .height = geometry.scale_h[i],
                                .output_width = geometry.scale_w[i + 1],
                                .output_height = geometry.scale_h[i + 1]});
            launch_decimate(q, {.source = geometry.d_pyramid_cmp[i],
                                .destination = geometry.d_pyramid_cmp[i + 1],
                                .width = geometry.scale_w[i],
                                .height = geometry.scale_h[i],
                                .output_width = geometry.scale_w[i + 1],
                                .output_height = geometry.scale_h[i + 1]});
        }
    }

    s->pending_index = index;
    s->has_pending = true;
    return 0;
}

} // namespace

namespace
{

static void compute_scale_lcs(MsSsimStateSycl *s, sycl::queue &queue, unsigned plane, int scale,
                              double &luminance, double &contrast, double &structure)
{
    const MsSsimPlaneGeometry &geometry = s->geom[plane];
    launch_horiz(queue, geometry.d_pyramid_ref[scale], geometry.d_pyramid_cmp[scale], s->d_h_ref_mu,
                 s->d_h_cmp_mu, s->d_h_ref_sq, s->d_h_cmp_sq, s->d_h_refcmp,
                 geometry.scale_w[scale], geometry.scale_w_horiz[scale],
                 geometry.scale_h_horiz[scale]);
    launch_vert_lcs(queue, {.ref_mu = s->d_h_ref_mu,
                            .cmp_mu = s->d_h_cmp_mu,
                            .ref_sq = s->d_h_ref_sq,
                            .cmp_sq = s->d_h_cmp_sq,
                            .refcmp = s->d_h_refcmp,
                            .luminance = s->d_l_partials,
                            .contrast = s->d_c_partials,
                            .structure = s->d_s_partials,
                            .horizontal_width = geometry.scale_w_horiz[scale],
                            .final_width = geometry.scale_w_final[scale],
                            .final_height = geometry.scale_h_final[scale],
                            .group_columns = geometry.scale_wg_count_x[scale],
                            .c1 = s->c1,
                            .c2 = s->c2,
                            .c3 = s->c3});
    const size_t bytes = (size_t)geometry.scale_wg_count[scale] * sizeof(float);
    queue.memcpy(s->h_l_partials, s->d_l_partials, bytes);
    queue.memcpy(s->h_c_partials, s->d_c_partials, bytes);
    queue.memcpy(s->h_s_partials, s->d_s_partials, bytes);
    queue.wait();
    double total_l = 0.0;
    double total_c = 0.0;
    double total_s = 0.0;
    for (unsigned group = 0; group < geometry.scale_wg_count[scale]; group++) {
        total_l += (double)s->h_l_partials[group];
        total_c += (double)s->h_c_partials[group];
        total_s += (double)s->h_s_partials[group];
    }
    const double pixels =
        (double)geometry.scale_w_final[scale] * (double)geometry.scale_h_final[scale];
    luminance = total_l / pixels;
    contrast = total_c / pixels;
    structure = total_s / pixels;
}

} // namespace

namespace
{

static double combine_ms_ssim(const double luminance[MS_SSIM_SCALES],
                              const double contrast[MS_SSIM_SCALES],
                              const double structure[MS_SSIM_SCALES])
{
    double score = 1.0;
    for (int scale = 0; scale < MS_SSIM_SCALES; scale++) {
        score *= std::pow(luminance[scale], (double)ALPHAS[scale]) *
                 std::pow(contrast[scale], (double)BETAS[scale]) *
                 std::pow(std::fabs(structure[scale]), (double)GAMMAS[scale]);
    }
    return score;
}

static const char *const l_names[MS_SSIM_SCALES] = {
    "float_ms_ssim_l_scale0", "float_ms_ssim_l_scale1", "float_ms_ssim_l_scale2",
    "float_ms_ssim_l_scale3", "float_ms_ssim_l_scale4",
};
static const char *const c_names[MS_SSIM_SCALES] = {
    "float_ms_ssim_c_scale0", "float_ms_ssim_c_scale1", "float_ms_ssim_c_scale2",
    "float_ms_ssim_c_scale3", "float_ms_ssim_c_scale4",
};
static const char *const s_names[MS_SSIM_SCALES] = {
    "float_ms_ssim_s_scale0", "float_ms_ssim_s_scale1", "float_ms_ssim_s_scale2",
    "float_ms_ssim_s_scale3", "float_ms_ssim_s_scale4",
};

} // namespace

namespace
{

static int append_lcs_scores(VmafFeatureCollector *collector, const double luminance[],
                             const double contrast[], const double structure[], unsigned index)
{
    int err = 0;
    for (int scale = 0; scale < MS_SSIM_SCALES; scale++) {
        err |= vmaf_feature_collector_append(collector, l_names[scale], luminance[scale], index);
        err |= vmaf_feature_collector_append(collector, c_names[scale], contrast[scale], index);
        err |= vmaf_feature_collector_append(collector, s_names[scale], structure[scale], index);
    }
    return err;
}

} // namespace

namespace
{

static int collect_fex_sycl(VmafFeatureExtractor *fex, unsigned index,
                            VmafFeatureCollector *feature_collector)
{
    auto *s = static_cast<MsSsimStateSycl *>(fex->priv);
    auto *qptr = static_cast<sycl::queue *>(vmaf_sycl_get_queue_ptr(s->sycl_state));
    if (!qptr) {
        return -EINVAL;
    }
    sycl::queue &q = *qptr;

    /* Feature names per plane, matching float_ms_ssim.c's ms_ssim_feature_names
     * exactly -- a GPU twin that emitted different names would be scored as a
     * different feature rather than as an accelerated one. */
    static const char *const plane_feature_names[MS_SSIM_MAX_PLANES] = {
        "float_ms_ssim",
        "float_ms_ssim_cb",
        "float_ms_ssim_cr",
    };

    int err = 0;
    for (unsigned plane = 0; plane < s->n_planes; plane++) {
        double l_means[MS_SSIM_SCALES] = {0};
        double c_means[MS_SSIM_SCALES] = {0};
        double s_means[MS_SSIM_SCALES] = {0};
        /* Planes run sequentially for the same reason the scales do: the
         * horizontal intermediates and the partials are one shared workspace,
         * and compute_scale_lcs waits on its readback before returning. */
        for (int scale = 0; scale < MS_SSIM_SCALES; scale++) {
            compute_scale_lcs(s, q, plane, scale, l_means[scale], c_means[scale], s_means[scale]);
        }
        double score = combine_ms_ssim(l_means, c_means, s_means);
        if (s->enable_db) {
            score = ms_ssim_convert_to_db(score, s->max_db);
        }
        err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                       plane_feature_names[plane], score, index);
        /* The l/c/s per-scale breakdown is luma-only, as on the CPU twin:
         * float_ms_ssim.c guards it with `p == 0`. */
        if (plane == 0U && s->enable_lcs) {
            err |= append_lcs_scores(feature_collector, l_means, c_means, s_means, index);
        }
    }
    return err;
}

} // namespace

namespace
{

static void free_ms_ssim_pointer(VmafSyclState *state, float *&pointer)
{
    if (pointer) {
        vmaf_sycl_free(state, pointer);
        pointer = nullptr;
    }
}

static void free_ms_ssim_pyramid(MsSsimStateSycl *s)
{
    /* MS_SSIM_MAX_PLANES, not n_planes: close must free whatever init managed to
     * allocate, and an init that failed part-way through plane 2 still leaves
     * plane 0 and 1 live. free_ms_ssim_pointer null-checks, so the unused tail
     * of a luma-only run costs nothing. */
    for (unsigned plane = 0; plane < MS_SSIM_MAX_PLANES; plane++) {
        MsSsimPlaneGeometry &geometry = s->geom[plane];
        free_ms_ssim_pointer(s->sycl_state, geometry.h_ref);
        free_ms_ssim_pointer(s->sycl_state, geometry.h_cmp);
        for (int scale = 0; scale < MS_SSIM_SCALES; scale++) {
            free_ms_ssim_pointer(s->sycl_state, geometry.d_pyramid_ref[scale]);
            free_ms_ssim_pointer(s->sycl_state, geometry.d_pyramid_cmp[scale]);
        }
    }
}

} // namespace

namespace
{

static void free_ms_ssim_workspace(MsSsimStateSycl *s)
{
    free_ms_ssim_pointer(s->sycl_state, s->d_h_ref_mu);
    free_ms_ssim_pointer(s->sycl_state, s->d_h_cmp_mu);
    free_ms_ssim_pointer(s->sycl_state, s->d_h_ref_sq);
    free_ms_ssim_pointer(s->sycl_state, s->d_h_cmp_sq);
    free_ms_ssim_pointer(s->sycl_state, s->d_h_refcmp);
    free_ms_ssim_pointer(s->sycl_state, s->d_l_partials);
    free_ms_ssim_pointer(s->sycl_state, s->d_c_partials);
    free_ms_ssim_pointer(s->sycl_state, s->d_s_partials);
    free_ms_ssim_pointer(s->sycl_state, s->h_l_partials);
    free_ms_ssim_pointer(s->sycl_state, s->h_c_partials);
    free_ms_ssim_pointer(s->sycl_state, s->h_s_partials);
}

} // namespace

namespace
{

static int close_fex_sycl(VmafFeatureExtractor *fex)
{
    auto *s = static_cast<MsSsimStateSycl *>(fex->priv);
    if (s->sycl_state) {
        free_ms_ssim_pyramid(s);
        free_ms_ssim_workspace(s);
    }
    if (s->feature_name_dict) {
        vmaf_dictionary_free(&s->feature_name_dict);
    }
    return 0;
}

/* All three plane features are advertised. Without _cb / _cr here the
 * ADR-0530 name-based fallback routes them to the CPU twin, which is what made
 * enable_chroma look harmless on this backend: the numbers still appeared, from
 * the CPU, while the option silently did nothing on the GPU. */
static const char *provided_features_ms_ssim_sycl[] = {"float_ms_ssim", "float_ms_ssim_cb",
                                                       "float_ms_ssim_cr", nullptr};

} // namespace

extern "C" VmafFeatureExtractor vmaf_fex_float_ms_ssim_sycl = {
    .name = "float_ms_ssim_sycl",
    .init = init_fex_sycl,
    .extract = nullptr,
    .flush = nullptr,
    .close = close_fex_sycl,
    .submit = submit_fex_sycl,
    .collect = collect_fex_sycl,
    .options = options_ms_ssim_sycl,
    .priv_size = sizeof(MsSsimStateSycl),
    .flags = VMAF_FEATURE_EXTRACTOR_SYCL,
    .provided_features = provided_features_ms_ssim_sycl,
    .chars =
        {
            .n_dispatches_per_frame = 18,
            .is_reduction_only = false,
            .min_useful_frame_area = 1920U * 1080U,
            .dispatch_hint = VMAF_FEATURE_DISPATCH_AUTO,
        },
};
