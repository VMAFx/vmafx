/* Upstream-mirror filename: defines float_ssim symbol despite the integer_ prefix (matches Netflix upstream). See ADR-0549. */
/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
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
#include "log.h"
#include "picture.h"
#include "../picture_copy.h"
#include "sycl/common.h"

namespace
{

static constexpr size_t SSIM_WG_X = 16;
static constexpr size_t SSIM_WG_Y = 8;
static constexpr int SSIM_K = 11;

/* Same 11-tap normalised Gaussian as the Vulkan + CUDA twins —
 * matches g_gaussian_window_h in iqa/ssim_tools.h byte-for-byte. */
static constexpr float G[SSIM_K] = {
    0.001028f, 0.007599f, 0.036001f, 0.109361f, 0.213006f, 0.266012f,
    0.213006f, 0.109361f, 0.036001f, 0.007599f, 0.001028f,
};

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

/* Tile width for the horizontal SLM staging (SY-2, ADR-0458):
 * each WG of SSIM_WG_X columns needs SSIM_WG_X + (SSIM_K-1) input
 * floats per row to cover the 11-tap apron.  Two SLM arrays (ref,
 * cmp) of size SSIM_WG_Y * SSIM_TILE_W carry all needed input pixels;
 * the five output channels are computed from SLM with no extra arrays.
 * This eliminates 11 global-memory loads per output channel per pixel
 * (total 55 → 26 loads per pixel pair on Arc A380). */
static constexpr size_t SSIM_TILE_W = SSIM_WG_X + (size_t)(SSIM_K - 1); /* 26 */

static void launch_horiz(sycl::queue &q, const float *d_ref, const float *d_cmp, float *d_ref_mu,
                         float *d_cmp_mu, float *d_ref_sq, float *d_cmp_sq, float *d_refcmp,
                         unsigned width, unsigned w_horiz, unsigned h_horiz)
{
    /* nd_range with SSIM_WG_X × SSIM_WG_Y work-groups; work-item count
     * rounded up to WG multiples.  Guard threads write nothing. */
    const size_t global_x = ((w_horiz + SSIM_WG_X - 1) / SSIM_WG_X) * SSIM_WG_X;
    const size_t global_y = ((h_horiz + SSIM_WG_Y - 1) / SSIM_WG_Y) * SSIM_WG_Y;
    sycl::nd_range<2> ndr{sycl::range<2>{global_y, global_x}, sycl::range<2>{SSIM_WG_Y, SSIM_WG_X}};
    const unsigned e_w = width;
    const unsigned e_w_horiz = w_horiz;
    const unsigned e_h_horiz = h_horiz;
    const float *e_ref = d_ref;
    const float *e_cmp = d_cmp;
    float *e_ref_mu = d_ref_mu;
    float *e_cmp_mu = d_cmp_mu;
    float *e_ref_sq = d_ref_sq;
    float *e_cmp_sq = d_cmp_sq;
    float *e_refcmp = d_refcmp;

    q.submit([&](sycl::handler &cgh) {
        /* Two SLM tiles: one for ref, one for cmp.
         * Size = SSIM_WG_Y rows × SSIM_TILE_W cols = 8 × 26 floats each.
         * Total SLM per WG = 2 × 208 × 4 B = 1664 B — well within Arc A380
         * local-memory limits (64 KB per compute unit). */
        sycl::local_accessor<float, 1> s_ref(sycl::range<1>(SSIM_WG_Y * SSIM_TILE_W), cgh);
        sycl::local_accessor<float, 1> s_cmp(sycl::range<1>(SSIM_WG_Y * SSIM_TILE_W), cgh);

        cgh.parallel_for(ndr, [=](sycl::nd_item<2> it) {
            const size_t gx = it.get_global_id(1);
            const size_t gy = it.get_global_id(0);
            const size_t lx = it.get_local_id(1);
            const size_t ly = it.get_local_id(0);
            const size_t lid = ly * SSIM_WG_X + lx; /* linear local id */

            /* Phase 1: cooperative tile load.
             * The WG covers output columns [wg_ox, wg_ox + SSIM_WG_X).
             * Input columns needed: [wg_ox, wg_ox + SSIM_WG_X + SSIM_K - 1).
             * wg_ox is the WG's global x-origin mapped to input-space
             * (which equals output-space because input[x..x+SSIM_K-1]
             *  produces output[x]). */
            const size_t wg_ox = it.get_group(1) * SSIM_WG_X;
            const size_t wg_oy = it.get_group(0) * SSIM_WG_Y;
            const size_t tile_elems = SSIM_WG_Y * SSIM_TILE_W;
            const size_t wg_size = SSIM_WG_X * SSIM_WG_Y;

            for (size_t i = lid; i < tile_elems; i += wg_size) {
                const size_t tr = i / SSIM_TILE_W; /* row within tile */
                const size_t tc = i % SSIM_TILE_W; /* col within tile */
                const size_t gy_load = wg_oy + tr;
                const size_t gx_load = wg_ox + tc; /* no clamping needed:
                                                      * wg_ox + SSIM_TILE_W - 1
                                                      * == wg_ox + SSIM_WG_X + SSIM_K - 2
                                                      * < width  (w_horiz = width - SSIM_K + 1,
                                                      *   last valid wg_ox = w_horiz -
                                                      *   SSIM_WG_X → wg_ox + SSIM_TILE_W - 1
                                                      *   <= width - 1). */
                if (gy_load < (size_t)e_h_horiz && gx_load < (size_t)e_w) {
                    const size_t idx = gy_load * (size_t)e_w + gx_load;
                    s_ref[i] = e_ref[idx];
                    s_cmp[i] = e_cmp[idx];
                } else {
                    s_ref[i] = 0.0f;
                    s_cmp[i] = 0.0f;
                }
            }
            it.barrier(sycl::access::fence_space::local_space);

            /* Phase 2: compute 11-tap horizontal convolution from SLM. */
            if (gx < (size_t)e_w_horiz && gy < (size_t)e_h_horiz) {
                float ref_mu_h = 0.0f, cmp_mu_h = 0.0f;
                float ref_sq_h = 0.0f, cmp_sq_h = 0.0f, refcmp_h = 0.0f;
                for (int u = 0; u < SSIM_K; u++) {
                    /* SLM index: row ly, column lx + u. */
                    const size_t si = ly * SSIM_TILE_W + lx + (size_t)u;
                    const float r = s_ref[si];
                    const float c = s_cmp[si];
                    const float gw = G[u];
                    ref_mu_h += gw * r;
                    cmp_mu_h += gw * c;
                    ref_sq_h += gw * (r * r);
                    cmp_sq_h += gw * (c * c);
                    refcmp_h += gw * (r * c);
                }
                const size_t dst_idx = gy * (size_t)e_w_horiz + gx;
                e_ref_mu[dst_idx] = ref_mu_h;
                e_cmp_mu[dst_idx] = cmp_mu_h;
                e_ref_sq[dst_idx] = ref_sq_h;
                e_cmp_sq[dst_idx] = cmp_sq_h;
                e_refcmp[dst_idx] = refcmp_h;
            }
        });
    });
}

static void launch_vert_combine(sycl::queue &q, const float *d_ref_mu, const float *d_cmp_mu,
                                const float *d_ref_sq, const float *d_cmp_sq, const float *d_refcmp,
                                float *d_partials, unsigned w_horiz, unsigned w_final,
                                unsigned h_final, float c1, float c2)
{
    /* Round work-item count up to WG-multiples. Per-WG sum
     * via sycl::reduce_over_group; one float per WG written
     * to partials. fp64-free (Arc A380). */
    const size_t global_x = ((w_final + SSIM_WG_X - 1) / SSIM_WG_X) * SSIM_WG_X;
    const size_t global_y = ((h_final + SSIM_WG_Y - 1) / SSIM_WG_Y) * SSIM_WG_Y;
    const size_t wg_count_x = global_x / SSIM_WG_X;
    sycl::nd_range<2> ndr{sycl::range<2>{global_y, global_x}, sycl::range<2>{SSIM_WG_Y, SSIM_WG_X}};
    const unsigned e_w_horiz = w_horiz;
    const unsigned e_w_final = w_final;
    const unsigned e_h_final = h_final;
    const float e_c1 = c1;
    const float e_c2 = c2;
    const size_t e_wg_count_x = wg_count_x;
    const float *e_ref_mu = d_ref_mu;
    const float *e_cmp_mu = d_cmp_mu;
    const float *e_ref_sq = d_ref_sq;
    const float *e_cmp_sq = d_cmp_sq;
    const float *e_refcmp = d_refcmp;
    float *e_partials = d_partials;

    q.submit([=](sycl::handler &h) {
        h.parallel_for(ndr, [=](sycl::nd_item<2> it) {
            const size_t x = it.get_global_id(1);
            const size_t y = it.get_global_id(0);
            float my_ssim = 0.0f;
            if (x < (size_t)e_w_final && y < (size_t)e_h_final) {
                float ref_mu = 0.0f, cmp_mu = 0.0f;
                float ref_sq = 0.0f, cmp_sq = 0.0f, refcmp = 0.0f;
                for (int v = 0; v < SSIM_K; v++) {
                    const size_t src_idx = (y + (size_t)v) * (size_t)e_w_horiz + x;
                    const float w = G[v];
                    ref_mu += w * e_ref_mu[src_idx];
                    cmp_mu += w * e_cmp_mu[src_idx];
                    ref_sq += w * e_ref_sq[src_idx];
                    cmp_sq += w * e_cmp_sq[src_idx];
                    refcmp += w * e_refcmp[src_idx];
                }
                const float ref_var = ref_sq - ref_mu * ref_mu;
                const float cmp_var = cmp_sq - cmp_mu * cmp_mu;
                const float covar = refcmp - ref_mu * cmp_mu;
                const float mu_xy = ref_mu * cmp_mu;
                /* Wang et al. (2004) Eq.(13) combined SSIM formula.
                 * NOTE: this differs from the CPU scalar path (float_ssim.c /
                 * iqa_ssim), which uses the L×C×S decomposition with
                 * sigma_comb = sqrt(var_ref * var_cmp) for the contrast term.
                 * Using 2*covar here instead of 2*sqrt(var_ref*var_cmp) is an
                 * intentional design choice for GPU kernels (avoids a per-pixel
                 * sqrt). This produces a systematic divergence of ~2–3e-4 from
                 * the CPU path at 576×324 on Arc A380 (fp64-less hardware) and
                 * ~1e-5 on fp64-capable hardware. The CUDA and Vulkan twins use
                 * the same formula. See Research-0985 §3.2 and ADR-0188. */
                const float num = (2.0f * mu_xy + e_c1) * (2.0f * covar + e_c2);
                const float den =
                    (ref_mu * ref_mu + cmp_mu * cmp_mu + e_c1) * (ref_var + cmp_var + e_c2);
                my_ssim = num / den;
            }
            float wg_sum = sycl::reduce_over_group(it.get_group(), my_ssim, sycl::plus<float>{});
            if (it.get_local_id(0) == 0 && it.get_local_id(1) == 0) {
                const size_t wg_idx = it.get_group(0) * e_wg_count_x + it.get_group(1);
                e_partials[wg_idx] = wg_sum;
            }
        });
    });
}

} /* anonymous namespace */

extern "C" {

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
    if (override_ > 0)
        return override_;
    int scaled = round_to_int((float)min_int((int)w, (int)h) / 256.0f);
    return scaled < 1 ? 1 : scaled;
}

static const VmafOption options_ssim_sycl[] = {
    {
        .name = "scale",
        .help = "decimation scale factor (0=auto, 1=no downscaling). "
                "v1: GPU path requires scale=1; auto-detect rejects scale>1 with -EINVAL.",
        .offset = offsetof(SsimStateSycl, scale_override),
        .type = VMAF_OPT_TYPE_INT,
        .default_val.i = 0,
        .min = 0,
        .max = 10,
    },
    {0},
};

static int init_fex_sycl(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                         unsigned w, unsigned h)
{
    (void)pix_fmt;
    auto *s = static_cast<SsimStateSycl *>(fex->priv);

    int scale = compute_scale(w, h, s->scale_override);
    if (scale != 1) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "ssim_sycl: v1 supports scale=1 only (auto-detected scale=%d at %ux%u). "
                 "Pin --feature float_ssim_sycl:scale=1 if intended.\n",
                 scale, w, h);
        return -EINVAL;
    }
    if (w < (unsigned)SSIM_K || h < (unsigned)SSIM_K) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "ssim_sycl: input %ux%u smaller than 11x11 Gaussian footprint.\n", w, h);
        return -EINVAL;
    }

    s->width = w;
    s->height = h;
    s->bpc = bpc;
    s->w_horiz = w - (SSIM_K - 1);
    s->h_horiz = h;
    s->w_final = w - (SSIM_K - 1);
    s->h_final = h - (SSIM_K - 1);
    s->wg_count_x = (s->w_final + (unsigned)SSIM_WG_X - 1) / (unsigned)SSIM_WG_X;
    s->wg_count_y = (s->h_final + (unsigned)SSIM_WG_Y - 1) / (unsigned)SSIM_WG_Y;
    s->wg_count = s->wg_count_x * s->wg_count_y;
    const float L = 255.0f, K1 = 0.01f, K2 = 0.03f;
    s->c1 = (K1 * L) * (K1 * L);
    s->c2 = (K2 * L) * (K2 * L);

    if (!fex->sycl_state) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "ssim_sycl: no SYCL state\n");
        return -EINVAL;
    }
    s->sycl_state = fex->sycl_state;

    const size_t input_bytes = (size_t)w * h * sizeof(float);
    const size_t horiz_bytes = (size_t)s->w_horiz * s->h_horiz * sizeof(float);
    const size_t partials_bytes = (size_t)s->wg_count * sizeof(float);

    s->h_ref = (float *)vmaf_sycl_malloc_host(s->sycl_state, input_bytes);
    s->h_cmp = (float *)vmaf_sycl_malloc_host(s->sycl_state, input_bytes);
    s->d_ref = (float *)vmaf_sycl_malloc_device(s->sycl_state, input_bytes);
    s->d_cmp = (float *)vmaf_sycl_malloc_device(s->sycl_state, input_bytes);
    s->d_ref_mu = (float *)vmaf_sycl_malloc_device(s->sycl_state, horiz_bytes);
    s->d_cmp_mu = (float *)vmaf_sycl_malloc_device(s->sycl_state, horiz_bytes);
    s->d_ref_sq = (float *)vmaf_sycl_malloc_device(s->sycl_state, horiz_bytes);
    s->d_cmp_sq = (float *)vmaf_sycl_malloc_device(s->sycl_state, horiz_bytes);
    s->d_refcmp = (float *)vmaf_sycl_malloc_device(s->sycl_state, horiz_bytes);
    s->d_partials = (float *)vmaf_sycl_malloc_device(s->sycl_state, partials_bytes);
    s->h_partials = (float *)vmaf_sycl_malloc_host(s->sycl_state, partials_bytes);
    if (!s->h_ref || !s->h_cmp || !s->d_ref || !s->d_cmp || !s->d_ref_mu || !s->d_cmp_mu ||
        !s->d_ref_sq || !s->d_cmp_sq || !s->d_refcmp || !s->d_partials || !s->h_partials) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "ssim_sycl: USM allocation failed\n");
        return -ENOMEM;
    }

    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (!s->feature_name_dict)
        return -ENOMEM;

    s->has_pending = false;
    return 0;
}

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

    launch_horiz(q, s->d_ref, s->d_cmp, s->d_ref_mu, s->d_cmp_mu, s->d_ref_sq, s->d_cmp_sq,
                 s->d_refcmp, s->width, s->w_horiz, s->h_horiz);
    launch_vert_combine(q, s->d_ref_mu, s->d_cmp_mu, s->d_ref_sq, s->d_cmp_sq, s->d_refcmp,
                        s->d_partials, s->w_horiz, s->w_final, s->h_final, s->c1, s->c2);

    q.memcpy(s->h_partials, s->d_partials, (size_t)s->wg_count * sizeof(float));

    s->pending_index = index;
    s->has_pending = true;
    return 0;
}

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
    const double score = total / n_pixels;

    return vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "float_ssim", score, index);
}

static int close_fex_sycl(VmafFeatureExtractor *fex)
{
    auto *s = static_cast<SsimStateSycl *>(fex->priv);
    if (s->sycl_state) {
        if (s->h_ref)
            vmaf_sycl_free(s->sycl_state, s->h_ref);
        if (s->h_cmp)
            vmaf_sycl_free(s->sycl_state, s->h_cmp);
        if (s->d_ref)
            vmaf_sycl_free(s->sycl_state, s->d_ref);
        if (s->d_cmp)
            vmaf_sycl_free(s->sycl_state, s->d_cmp);
        if (s->d_ref_mu)
            vmaf_sycl_free(s->sycl_state, s->d_ref_mu);
        if (s->d_cmp_mu)
            vmaf_sycl_free(s->sycl_state, s->d_cmp_mu);
        if (s->d_ref_sq)
            vmaf_sycl_free(s->sycl_state, s->d_ref_sq);
        if (s->d_cmp_sq)
            vmaf_sycl_free(s->sycl_state, s->d_cmp_sq);
        if (s->d_refcmp)
            vmaf_sycl_free(s->sycl_state, s->d_refcmp);
        if (s->d_partials)
            vmaf_sycl_free(s->sycl_state, s->d_partials);
        if (s->h_partials)
            vmaf_sycl_free(s->sycl_state, s->h_partials);
    }
    if (s->feature_name_dict)
        vmaf_dictionary_free(&s->feature_name_dict);
    return 0;
}

static const char *provided_features_ssim_sycl[] = {"float_ssim", NULL};

extern "C" VmafFeatureExtractor vmaf_fex_float_ssim_sycl = {
    .name = "float_ssim_sycl",
    .init = init_fex_sycl,
    .extract = NULL,
    .flush = NULL,
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

} /* extern "C" */

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

static constexpr size_t ISSIM_WG_X = 16;
static constexpr size_t ISSIM_WG_Y = 8;
/* 9-tap integer Gaussian kernel matching gaussian_filter_init(sigma=1.5, max_len=5):
 * [2, 9, 28, 55, 68, 55, 28, 9, 2], sum=256, kernel_len=4. */
static constexpr int ISSIM_HALF_K = 4;
static constexpr int ISSIM_K_SZ = 9;
static constexpr int32_t ISSIM_KERNEL[ISSIM_K_SZ] = {2, 9, 28, 55, 68, 55, 28, 9, 2};

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

/* Pass 1 (8bpc): horizontal 9-tap int64 moment accumulation. */
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
                int64_t mux = 0LL, muy = 0LL, x2 = 0LL, xy = 0LL, y2 = 0LL, w = 0LL;
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

/* Pass 1 (>8bpc): same as above but reads uint16_t. */
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
                int64_t mux = 0LL, muy = 0LL, x2 = 0LL, xy = 0LL, y2 = 0LL, w = 0LL;
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

/* Pass 2: vertical 9-tap int64 accumulation + float SSIM formula
 * (fp64-free: ADR-0220) + per-WG float/int64 partial sums.
 * Host accumulates: ssim = sum(partials * wgt) / sum(wgt). */
static void launch_issim_vert_combine(sycl::queue &q, const int64_t *d_mux_h,
                                      const int64_t *d_muy_h, const int64_t *d_x2_h,
                                      const int64_t *d_xy_h, const int64_t *d_y2_h,
                                      const int64_t *d_w_h, float *d_partials, int64_t *d_wgt,
                                      unsigned width, unsigned height, int64_t samplemax,
                                      size_t wg_count_x)
{
    const size_t gx = ((width + ISSIM_WG_X - 1) / ISSIM_WG_X) * ISSIM_WG_X;
    const size_t gy = ((height + ISSIM_WG_Y - 1) / ISSIM_WG_Y) * ISSIM_WG_Y;
    const unsigned e_width = width;
    const unsigned e_height = height;
    const size_t e_wg_count_x = wg_count_x;
    /* Convert samplemax to float for the SSIM formula (fp64-free). */
    const float sm_f = (float)samplemax;
    q.submit([=](sycl::handler &h) {
        h.parallel_for(
            sycl::nd_range<2>{sycl::range<2>{gy, gx}, sycl::range<2>{ISSIM_WG_Y, ISSIM_WG_X}},
            [=](sycl::nd_item<2> it) {
                const unsigned x = (unsigned)it.get_global_id(1);
                const unsigned y = (unsigned)it.get_global_id(0);
                float my_ssim = 0.0f;
                /* int64 weight accumulated as int64_t, then cast to float
                 * for reduce_over_group (no int64 reduction in SYCL fp64-free
                 * tier). Host re-reads the int64 column separately. */
                float my_wgt_f = 0.0f;

                if (x < e_width && y < e_height) {
                    const int k_min = (int)y < ISSIM_HALF_K ? ISSIM_HALF_K - (int)y : 0;
                    const int k_max = ((int)y + ISSIM_HALF_K >= (int)e_height) ?
                                          ISSIM_K_SZ - ((int)y + ISSIM_HALF_K - (int)e_height + 1) :
                                          ISSIM_K_SZ;
                    int64_t mux = 0LL, muy = 0LL, x2 = 0LL, xy = 0LL, y2 = 0LL, w = 0LL;
                    for (int k = k_min; k < k_max; k++) {
                        const unsigned src_y = (unsigned)((int)y - ISSIM_HALF_K + k);
                        const size_t hidx = (size_t)src_y * e_width + x;
                        const int64_t vk = (int64_t)ISSIM_KERNEL[k];
                        mux += vk * d_mux_h[hidx];
                        muy += vk * d_muy_h[hidx];
                        x2 += vk * d_x2_h[hidx];
                        xy += vk * d_xy_h[hidx];
                        y2 += vk * d_y2_h[hidx];
                        w += vk * d_w_h[hidx];
                    }
                    /* SSIM formula in float32 (fp64-free constraint ADR-0220).
                     * Places=4-5 vs CPU double formula — documented ADR-0564. */
                    const float w_f = (float)w;
                    const float c1 = sm_f * sm_f * 0.0001f * w_f * w_f;
                    const float c2 = sm_f * sm_f * 0.0009f * w_f * w_f;
                    const float dmux = (float)mux;
                    const float dmuy = (float)muy;
                    const float dx2 = (float)x2;
                    const float dxy = (float)xy;
                    const float dy2 = (float)y2;
                    const float mxy = dmux * dmuy;
                    const float num = (2.0f * mxy + c1) * (2.0f * (dxy * w_f - mxy) + c2);
                    const float den = (dmux * dmux + dmuy * dmuy + c1) *
                                      (dx2 * w_f - dmux * dmux + dy2 * w_f - dmuy * dmuy + c2);
                    if (den != 0.0f && w > 0LL) {
                        my_ssim = w_f * (num / den);
                        my_wgt_f = w_f;
                    }
                }
                const float wg_ssim =
                    sycl::reduce_over_group(it.get_group(), my_ssim, sycl::plus<float>{});
                const float wg_wgt_f =
                    sycl::reduce_over_group(it.get_group(), my_wgt_f, sycl::plus<float>{});
                if (it.get_local_id(0) == 0 && it.get_local_id(1) == 0) {
                    const size_t wg_idx = it.get_group(0) * e_wg_count_x + it.get_group(1);
                    d_partials[wg_idx] = wg_ssim;
                    /* Store weight as int64 for host reduction. Cast from float
                     * is safe: weights are bounded by (kernel_weight^2 * pixels_per_block)
                     * well within float32's 24-bit mantissa for 128-thread blocks. */
                    d_wgt[wg_idx] = (int64_t)wg_wgt_f;
                }
            });
    });
}

} /* anonymous namespace */

extern "C" {

static int init_fex_issim_sycl(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt,
                               unsigned bpc, unsigned w, unsigned h)
{
    (void)pix_fmt;
    auto *s = static_cast<IssimStateSycl *>(fex->priv);

    if (w < 1u || h < 1u) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "integer_ssim_sycl: zero-dimension input %ux%u\n", w, h);
        return -EINVAL;
    }

    s->width = w;
    s->height = h;
    s->bpc = bpc;
    s->wg_count_x = (unsigned)((w + ISSIM_WG_X - 1) / ISSIM_WG_X);
    s->wg_count_y = (unsigned)((h + ISSIM_WG_Y - 1) / ISSIM_WG_Y);
    s->wg_count = s->wg_count_x * s->wg_count_y;

    if (!fex->sycl_state) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "integer_ssim_sycl: no SYCL state\n");
        return -EINVAL;
    }
    s->sycl_state = fex->sycl_state;

    const size_t pix8_bytes = (size_t)w * h * sizeof(uint8_t);
    const size_t pix16_bytes = (size_t)w * h * sizeof(uint16_t);
    const size_t int64_plane_bytes = (size_t)w * h * sizeof(int64_t);
    const size_t partials_bytes = (size_t)s->wg_count * sizeof(float);
    const size_t wgt_bytes = (size_t)s->wg_count * sizeof(int64_t);

    s->h_ref_u8 = (uint8_t *)vmaf_sycl_malloc_host(s->sycl_state, pix8_bytes);
    s->h_cmp_u8 = (uint8_t *)vmaf_sycl_malloc_host(s->sycl_state, pix8_bytes);
    s->h_ref_u16 = (uint16_t *)vmaf_sycl_malloc_host(s->sycl_state, pix16_bytes);
    s->h_cmp_u16 = (uint16_t *)vmaf_sycl_malloc_host(s->sycl_state, pix16_bytes);
    s->d_ref_u8 = (uint8_t *)vmaf_sycl_malloc_device(s->sycl_state, pix8_bytes);
    s->d_cmp_u8 = (uint8_t *)vmaf_sycl_malloc_device(s->sycl_state, pix8_bytes);
    s->d_ref_u16 = (uint16_t *)vmaf_sycl_malloc_device(s->sycl_state, pix16_bytes);
    s->d_cmp_u16 = (uint16_t *)vmaf_sycl_malloc_device(s->sycl_state, pix16_bytes);
    s->d_mux = (int64_t *)vmaf_sycl_malloc_device(s->sycl_state, int64_plane_bytes);
    s->d_muy = (int64_t *)vmaf_sycl_malloc_device(s->sycl_state, int64_plane_bytes);
    s->d_x2 = (int64_t *)vmaf_sycl_malloc_device(s->sycl_state, int64_plane_bytes);
    s->d_xy = (int64_t *)vmaf_sycl_malloc_device(s->sycl_state, int64_plane_bytes);
    s->d_y2 = (int64_t *)vmaf_sycl_malloc_device(s->sycl_state, int64_plane_bytes);
    s->d_w = (int64_t *)vmaf_sycl_malloc_device(s->sycl_state, int64_plane_bytes);
    s->d_partials = (float *)vmaf_sycl_malloc_device(s->sycl_state, partials_bytes);
    s->h_partials = (float *)vmaf_sycl_malloc_host(s->sycl_state, partials_bytes);
    s->d_wgt = (int64_t *)vmaf_sycl_malloc_device(s->sycl_state, wgt_bytes);
    s->h_wgt = (int64_t *)vmaf_sycl_malloc_host(s->sycl_state, wgt_bytes);

    if (!s->h_ref_u8 || !s->h_cmp_u8 || !s->h_ref_u16 || !s->h_cmp_u16 || !s->d_ref_u8 ||
        !s->d_cmp_u8 || !s->d_ref_u16 || !s->d_cmp_u16 || !s->d_mux || !s->d_muy || !s->d_x2 ||
        !s->d_xy || !s->d_y2 || !s->d_w || !s->d_partials || !s->h_partials || !s->d_wgt ||
        !s->h_wgt) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "integer_ssim_sycl: USM allocation failed\n");
        return -ENOMEM;
    }

    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (!s->feature_name_dict)
        return -ENOMEM;

    s->has_pending = false;
    return 0;
}

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

    const size_t npix = (size_t)s->width * s->height;
    if (s->bpc == 8u) {
        /* Pack 8bpc pixels (strided) into tight uint8 staging. */
        const uint8_t *rp = (const uint8_t *)ref_pic->data[0];
        const uint8_t *dp = (const uint8_t *)dist_pic->data[0];
        for (unsigned y = 0; y < s->height; y++) {
            __builtin_memcpy(s->h_ref_u8 + y * s->width, rp + y * ref_pic->stride[0], s->width);
            __builtin_memcpy(s->h_cmp_u8 + y * s->width, dp + y * dist_pic->stride[0], s->width);
        }
        q.memcpy(s->d_ref_u8, s->h_ref_u8, npix * sizeof(uint8_t));
        q.memcpy(s->d_cmp_u8, s->h_cmp_u8, npix * sizeof(uint8_t));
        launch_issim_horiz_8bpc(q, s->d_ref_u8, s->d_cmp_u8, s->d_mux, s->d_muy, s->d_x2, s->d_xy,
                                s->d_y2, s->d_w, s->width, s->height);
    } else {
        /* Pack >8bpc pixels (uint16, strided) into tight staging. */
        const uint8_t *rp = (const uint8_t *)ref_pic->data[0];
        const uint8_t *dp = (const uint8_t *)dist_pic->data[0];
        for (unsigned y = 0; y < s->height; y++) {
            __builtin_memcpy(s->h_ref_u16 + y * s->width, rp + y * ref_pic->stride[0],
                             s->width * 2u);
            __builtin_memcpy(s->h_cmp_u16 + y * s->width, dp + y * dist_pic->stride[0],
                             s->width * 2u);
        }
        q.memcpy(s->d_ref_u16, s->h_ref_u16, npix * sizeof(uint16_t));
        q.memcpy(s->d_cmp_u16, s->h_cmp_u16, npix * sizeof(uint16_t));
        launch_issim_horiz_16bpc(q, s->d_ref_u16, s->d_cmp_u16, s->d_mux, s->d_muy, s->d_x2,
                                 s->d_xy, s->d_y2, s->d_w, s->width, s->height);
    }

    const int64_t samplemax = (int64_t)((1u << s->bpc) - 1u);
    launch_issim_vert_combine(q, s->d_mux, s->d_muy, s->d_x2, s->d_xy, s->d_y2, s->d_w,
                              s->d_partials, s->d_wgt, s->width, s->height, samplemax,
                              s->wg_count_x);

    q.memcpy(s->h_partials, s->d_partials, (size_t)s->wg_count * sizeof(float));
    q.memcpy(s->h_wgt, s->d_wgt, (size_t)s->wg_count * sizeof(int64_t));

    s->pending_index = index;
    s->has_pending = true;
    return 0;
}

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
    if (total_wgt == 0LL)
        return -EINVAL;
    const double score = total_ssim / (double)total_wgt;

    return vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict, "ssim",
                                                   score, index);
}

static int close_fex_issim_sycl(VmafFeatureExtractor *fex)
{
    auto *s = static_cast<IssimStateSycl *>(fex->priv);
    if (s->sycl_state) {
        if (s->h_ref_u8)
            vmaf_sycl_free(s->sycl_state, s->h_ref_u8);
        if (s->h_cmp_u8)
            vmaf_sycl_free(s->sycl_state, s->h_cmp_u8);
        if (s->h_ref_u16)
            vmaf_sycl_free(s->sycl_state, s->h_ref_u16);
        if (s->h_cmp_u16)
            vmaf_sycl_free(s->sycl_state, s->h_cmp_u16);
        if (s->d_ref_u8)
            vmaf_sycl_free(s->sycl_state, s->d_ref_u8);
        if (s->d_cmp_u8)
            vmaf_sycl_free(s->sycl_state, s->d_cmp_u8);
        if (s->d_ref_u16)
            vmaf_sycl_free(s->sycl_state, s->d_ref_u16);
        if (s->d_cmp_u16)
            vmaf_sycl_free(s->sycl_state, s->d_cmp_u16);
        if (s->d_mux)
            vmaf_sycl_free(s->sycl_state, s->d_mux);
        if (s->d_muy)
            vmaf_sycl_free(s->sycl_state, s->d_muy);
        if (s->d_x2)
            vmaf_sycl_free(s->sycl_state, s->d_x2);
        if (s->d_xy)
            vmaf_sycl_free(s->sycl_state, s->d_xy);
        if (s->d_y2)
            vmaf_sycl_free(s->sycl_state, s->d_y2);
        if (s->d_w)
            vmaf_sycl_free(s->sycl_state, s->d_w);
        if (s->d_partials)
            vmaf_sycl_free(s->sycl_state, s->d_partials);
        if (s->h_partials)
            vmaf_sycl_free(s->sycl_state, s->h_partials);
        if (s->d_wgt)
            vmaf_sycl_free(s->sycl_state, s->d_wgt);
        if (s->h_wgt)
            vmaf_sycl_free(s->sycl_state, s->h_wgt);
    }
    if (s->feature_name_dict)
        (void)vmaf_dictionary_free(&s->feature_name_dict);
    return 0;
}

} /* anonymous namespace */

extern "C" {

static const VmafOption options_issim_sycl[] = {
    {0},
};

static const char *provided_features_issim_sycl[] = {"ssim", NULL};

/* Real integer_ssim SYCL extractor (ADR-0564). Uses 9-tap int64 moments
 * matching the CPU algorithm. The SSIM formula is computed in float32
 * (fp64-free constraint, ADR-0220); expected precision is places=4-5 vs
 * CPU. Load-bearing: declared via extern in feature_extractor.c. */
VmafFeatureExtractor vmaf_fex_integer_ssim_sycl = {
    .name = "integer_ssim_sycl",
    .init = init_fex_issim_sycl,
    .extract = NULL,
    .flush = NULL,
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

} /* extern "C" */
