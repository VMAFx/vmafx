/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  float_psnr feature kernel on the SYCL backend (T7-23 / batch 3
 *  part 3c — ADR-0192 / ADR-0195). SYCL twin of float_psnr_vulkan +
 *  float_psnr_cuda.
 */

#include <sycl/sycl.hpp>

#include "sycl_compat.h"

#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
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

struct FloatPsnrStateSycl {
    unsigned width;
    unsigned height;
    unsigned bpc;
    double peak;
    double psnr_max;
    /* `uncapped` option: mirrors CPU float_psnr.c. When true, psnr_max
     * keeps only its zero-noise infinity-sentinel role and stops
     * truncating genuinely computed values. Default false keeps every
     * shipped score unchanged. See ADR-1193 / T-UPSTREAM-1109. */
    bool uncapped;
    size_t plane_bytes;

    VmafSyclState *sycl_state;

    void *h_ref;
    void *h_dis;
    void *d_ref;
    void *d_dis;

    float *d_partials;
    float *h_partials;
    unsigned wg_count_x;
    unsigned wg_count_y;
    unsigned wg_count;

    bool has_pending;
    unsigned pending_index;

    VmafDictionary *feature_name_dict;
};

} // namespace

namespace
{

static constexpr int FPSNR_WG_X = 16;
static constexpr int FPSNR_WG_Y = 16;

struct FpsnrOutput {
    float *partials;
};

} // namespace

namespace
{

static inline float fpsnr_inv_scaler(unsigned bpc)
{
    if (bpc == 10) {
        return 0.25f;
    }
    if (bpc == 12) {
        return 0.0625f;
    }
    if (bpc == 16) {
        return 0.00390625f;
    }
    return 1.0f;
}

} // namespace

namespace
{

static inline float fpsnr_pixel_noise(const void *ref, const void *dis, size_t offset, unsigned bpc)
{
    float r;
    float d;
    if (bpc <= 8) {
        r = (float)static_cast<const uint8_t *>(ref)[offset];
        d = (float)static_cast<const uint8_t *>(dis)[offset];
    } else {
        const float inv_scaler = fpsnr_inv_scaler(bpc);
        r = (float)static_cast<const uint16_t *>(ref)[offset] * inv_scaler;
        d = (float)static_cast<const uint16_t *>(dis)[offset] * inv_scaler;
    }
    const float diff = r - d;
    return diff * diff;
}

} // namespace

namespace
{

static inline void fpsnr_store_workgroup_sum(sycl::nd_item<2> item,
                                             const sycl::local_accessor<float, 1> &scratch,
                                             float noise, float *partials, unsigned workgroups_x)
{
    sycl::sub_group const subgroup = item.get_sub_group();
    const float subgroup_sum = sycl::reduce_over_group(subgroup, noise, sycl::plus<float>{});
    const uint32_t subgroup_id = subgroup.get_group_linear_id();
    const uint32_t subgroup_lane = subgroup.get_local_linear_id();
    const uint32_t subgroup_count = subgroup.get_group_linear_range();
    if (subgroup_lane == 0) {
        scratch[subgroup_id] = subgroup_sum;
    }
    item.barrier(sycl::access::fence_space::local_space);

    if (item.get_local_linear_id() == 0) {
        float total = 0.0f;
        for (uint32_t subgroup_index = 0; subgroup_index < subgroup_count; subgroup_index++) {
            total += scratch[subgroup_index];
        }
        const size_t workgroup_index = item.get_group(0) * workgroups_x + item.get_group(1);
        partials[workgroup_index] = total;
    }
}

} // namespace

namespace
{

static sycl::event launch_float_psnr(sycl::queue &q, const void *ref, const void *dis,
                                     FpsnrOutput output, unsigned width, unsigned height,
                                     unsigned bpc, unsigned wg_count_x)
{
    const size_t global_x =
        ((static_cast<size_t>(width) + FPSNR_WG_X - 1) / FPSNR_WG_X) * FPSNR_WG_X;
    const size_t global_y =
        ((static_cast<size_t>(height) + FPSNR_WG_Y - 1) / FPSNR_WG_Y) * FPSNR_WG_Y;
    return q.submit([&](sycl::handler &cgh) {
        constexpr int MAX_SUBGROUPS = FPSNR_WG_X * FPSNR_WG_Y;
        sycl::local_accessor<float, 1> const s_partials(sycl::range<1>(MAX_SUBGROUPS), cgh);

        cgh.parallel_for(
            sycl::nd_range<2>(sycl::range<2>(global_y, global_x),
                              sycl::range<2>(FPSNR_WG_Y, FPSNR_WG_X)),
            [=](sycl::nd_item<2> item) VMAF_SYCL_REQD_SG_SIZE(32) {
                const int gx = (int)item.get_global_id(1);
                const int gy = (int)item.get_global_id(0);
                float my_noise = 0.0f;
                if (std::cmp_less(gx, width) && std::cmp_less(gy, height)) {
                    my_noise = fpsnr_pixel_noise(ref, dis, (size_t)gy * width + (size_t)gx, bpc);
                }
                fpsnr_store_workgroup_sum(item, s_partials, my_noise, output.partials, wg_count_x);
            });
    });
}

} // namespace

namespace
{

template <typename T> static void copy_y_plane(VmafPicture *pic, void *dst, unsigned w, unsigned h)
{
    const T *src = static_cast<const T *>(pic->data[0]);
    T *out = static_cast<T *>(dst);
    const ptrdiff_t src_stride_t = pic->stride[0] / static_cast<ptrdiff_t>(sizeof(T));
    for (unsigned i = 0; i < h; i++) {
        for (unsigned j = 0; j < w; j++)
            out[j] = src[j];
        src += src_stride_t;
        out += w;
    }
}

} // namespace

namespace
{

static const VmafOption options_float_psnr_sycl[] = {
    {
        .name = "uncapped",
        .help = "report the true PSNR instead of truncating at the psnr_max ceiling "
                "(a zero-noise pair still reports psnr_max)",
        .offset = offsetof(FloatPsnrStateSycl, uncapped),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val = {.b = false},
    },
    {.name = nullptr}};

} // namespace

namespace
{

static int configure_peak(FloatPsnrStateSycl *s, unsigned bpc)
{
    if (bpc == 8) {
        s->peak = 255.0;
        s->psnr_max = 60.0;
    } else if (bpc == 10) {
        s->peak = 255.75;
        s->psnr_max = 72.0;
    } else if (bpc == 12) {
        s->peak = 255.9375;
        s->psnr_max = 84.0;
    } else if (bpc == 16) {
        s->peak = 255.99609375;
        s->psnr_max = 108.0;
    } else {
        return -EINVAL;
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
    (void)pix_fmt;
    auto *s = static_cast<FloatPsnrStateSycl *>(fex->priv);
    s->width = w;
    s->height = h;
    s->bpc = bpc;
    s->has_pending = false;

    const int config_err = configure_peak(s, bpc);
    if (config_err) {
        return config_err;
    }

    if (!fex->sycl_state) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "float_psnr_sycl: no SYCL state\n");
        return -EINVAL;
    }
    VmafSyclState *state = fex->sycl_state;
    s->sycl_state = state;

    s->plane_bytes = (size_t)w * h * (bpc <= 8 ? 1u : 2u);
    s->h_ref = vmaf_sycl_malloc_host(state, s->plane_bytes);
    s->h_dis = vmaf_sycl_malloc_host(state, s->plane_bytes);
    s->d_ref = vmaf_sycl_malloc_device(state, s->plane_bytes);
    s->d_dis = vmaf_sycl_malloc_device(state, s->plane_bytes);

    s->wg_count_x = (unsigned)((w + FPSNR_WG_X - 1) / FPSNR_WG_X);
    s->wg_count_y = (unsigned)((h + FPSNR_WG_Y - 1) / FPSNR_WG_Y);
    s->wg_count = s->wg_count_x * s->wg_count_y;
    const size_t pbytes = (size_t)s->wg_count * sizeof(float);
    s->d_partials = static_cast<float *>(vmaf_sycl_malloc_device(state, pbytes));
    s->h_partials = static_cast<float *>(vmaf_sycl_malloc_host(state, pbytes));

    if (!s->h_ref || !s->h_dis || !s->d_ref || !s->d_dis || !s->d_partials || !s->h_partials) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "float_psnr_sycl: USM allocation failed\n");
        (void)close_fex_sycl(fex);
        return -ENOMEM;
    }

    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (!s->feature_name_dict) {
        (void)close_fex_sycl(fex);
        return -ENOMEM;
    }
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
    auto *s = static_cast<FloatPsnrStateSycl *>(fex->priv);
    auto *qptr = static_cast<sycl::queue *>(vmaf_sycl_get_queue_ptr(s->sycl_state));
    if (!qptr) {
        return -EINVAL;
    }
    sycl::queue &q = *qptr;

    if (s->bpc <= 8) {
        copy_y_plane<uint8_t>(ref_pic, s->h_ref, s->width, s->height);
        copy_y_plane<uint8_t>(dist_pic, s->h_dis, s->width, s->height);
    } else {
        copy_y_plane<uint16_t>(ref_pic, s->h_ref, s->width, s->height);
        copy_y_plane<uint16_t>(dist_pic, s->h_dis, s->width, s->height);
    }
    q.memcpy(s->d_ref, s->h_ref, s->plane_bytes);
    q.memcpy(s->d_dis, s->h_dis, s->plane_bytes);

    launch_float_psnr(q, s->d_ref, s->d_dis, {.partials = s->d_partials}, s->width, s->height,
                      s->bpc, s->wg_count_x);
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
    auto *s = static_cast<FloatPsnrStateSycl *>(fex->priv);
    auto *qptr = static_cast<sycl::queue *>(vmaf_sycl_get_queue_ptr(s->sycl_state));
    if (!qptr) {
        return -EINVAL;
    }
    qptr->wait();

    double total = 0.0;
    for (unsigned i = 0; i < s->wg_count; i++) {
        total += (double)s->h_partials[i];
    }
    const double n_pix = (double)s->width * (double)s->height;
    const double noise = total / n_pix;
    /* Match CPU float_psnr.c — a zero-noise pair reports psnr_max as the
     * infinity sentinel; the truncation at psnr_max applies only when
     * `uncapped` is false. See ADR-1193 / T-UPSTREAM-1109. */
    const double eps = 1e-10;
    const double max_noise = noise > eps ? noise : eps;
    double score;
    if (!s->uncapped) {
        /* Pre-ADR-1193 expression verbatim — bit-identical default. */
        score = 10.0 * std::log10(s->peak * s->peak / max_noise);
        if (score > s->psnr_max) {
            score = s->psnr_max;
        }
    } else if (noise <= 0.0) {
        score = s->psnr_max; /* infinity sentinel */
    } else {
        score = 10.0 * std::log10(s->peak * s->peak / max_noise);
    }
    return vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "float_psnr", score, index);
}

} // namespace

namespace
{

static int close_fex_sycl(VmafFeatureExtractor *fex)
{
    auto *s = static_cast<FloatPsnrStateSycl *>(fex->priv);
    if (s->sycl_state) {
        if (s->h_ref)
            vmaf_sycl_free(s->sycl_state, s->h_ref);
        if (s->h_dis)
            vmaf_sycl_free(s->sycl_state, s->h_dis);
        if (s->d_ref)
            vmaf_sycl_free(s->sycl_state, s->d_ref);
        if (s->d_dis)
            vmaf_sycl_free(s->sycl_state, s->d_dis);
        if (s->d_partials)
            vmaf_sycl_free(s->sycl_state, s->d_partials);
        if (s->h_partials)
            vmaf_sycl_free(s->sycl_state, s->h_partials);
    }
    if (s->feature_name_dict)
        vmaf_dictionary_free(&s->feature_name_dict);
    return 0;
}

static const char *provided_features_float_psnr_sycl[] = {"float_psnr", nullptr};

} // namespace

extern "C" VmafFeatureExtractor vmaf_fex_float_psnr_sycl = {
    .name = "float_psnr_sycl",
    .init = init_fex_sycl,
    .extract = nullptr,
    .flush = nullptr,
    .close = close_fex_sycl,
    .submit = submit_fex_sycl,
    .collect = collect_fex_sycl,
    .options = options_float_psnr_sycl,
    .priv_size = sizeof(FloatPsnrStateSycl),
    .flags = VMAF_FEATURE_EXTRACTOR_SYCL,
    .provided_features = provided_features_float_psnr_sycl,
};
