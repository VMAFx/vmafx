/**
 *  Copyright 2026 Lusoris and Claude (Anthropic)
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 *
 *  float_psnr feature kernel on the SYCL backend (T7-23 / batch 3
 *  part 3c — ADR-0192 / ADR-0195). SYCL twin of float_psnr_vulkan +
 *  float_psnr_cuda.
 */

#include <sycl/sycl.hpp>

#include "sycl_compat.h"

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstring>

extern "C" {
#include "config.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "log.h"
#include "picture.h"
#include "sycl/common.h"
}

namespace
{

struct FloatPsnrStateSycl {
    unsigned width;
    unsigned height;
    unsigned bpc;
    double peak;
    double psnr_max;
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

static constexpr int FPSNR_WG_X = 16;
static constexpr int FPSNR_WG_Y = 16;

static sycl::event launch_float_psnr(sycl::queue &q, const void *ref, const void *dis,
                                     float *partials, unsigned width, unsigned height, unsigned bpc,
                                     unsigned wg_count_x)
{
    const size_t global_x = ((width + FPSNR_WG_X - 1) / FPSNR_WG_X) * FPSNR_WG_X;
    const size_t global_y = ((height + FPSNR_WG_Y - 1) / FPSNR_WG_Y) * FPSNR_WG_Y;
    const unsigned e_w = width;
    const unsigned e_h = height;
    const unsigned e_bpc = bpc;
    const unsigned e_wgx = wg_count_x;
    const void *e_ref = ref;
    const void *e_dis = dis;

    return q.submit([&](sycl::handler &cgh) {
        constexpr int MAX_SUBGROUPS = FPSNR_WG_X * FPSNR_WG_Y;
        sycl::local_accessor<float, 1> s_partials(sycl::range<1>(MAX_SUBGROUPS), cgh);

        cgh.parallel_for(sycl::nd_range<2>(sycl::range<2>(global_y, global_x),
                                           sycl::range<2>(FPSNR_WG_Y, FPSNR_WG_X)),
                         [=](sycl::nd_item<2> item) VMAF_SYCL_REQD_SG_SIZE(32) {
                             const int gx = (int)item.get_global_id(1);
                             const int gy = (int)item.get_global_id(0);
                             const unsigned lid = item.get_local_linear_id();

                             float scaler = 1.0f;
                             if (e_bpc == 10)
                                 scaler = 4.0f;
                             else if (e_bpc == 12)
                                 scaler = 16.0f;
                             else if (e_bpc == 16)
                                 scaler = 256.0f;
                             const float inv_scaler = 1.0f / scaler;

                             float my_noise = 0.0f;
                             if (gx < (int)e_w && gy < (int)e_h) {
                                 float r;
                                 float d;
                                 if (e_bpc <= 8) {
                                     r = (float)static_cast<const uint8_t *>(
                                         e_ref)[(size_t)gy * e_w + (size_t)gx];
                                     d = (float)static_cast<const uint8_t *>(
                                         e_dis)[(size_t)gy * e_w + (size_t)gx];
                                 } else {
                                     r = (float)static_cast<const uint16_t *>(
                                             e_ref)[(size_t)gy * e_w + (size_t)gx] *
                                         inv_scaler;
                                     d = (float)static_cast<const uint16_t *>(
                                             e_dis)[(size_t)gy * e_w + (size_t)gx] *
                                         inv_scaler;
                                 }
                                 const float diff = r - d;
                                 my_noise = diff * diff;
                             }

                             sycl::sub_group sg = item.get_sub_group();
                             const float sg_sum =
                                 sycl::reduce_over_group(sg, my_noise, sycl::plus<float>{});
                             const uint32_t sg_id = sg.get_group_linear_id();
                             const uint32_t sg_lid = sg.get_local_linear_id();
                             const uint32_t n_subgroups = sg.get_group_linear_range();
                             if (sg_lid == 0)
                                 s_partials[sg_id] = sg_sum;
                             item.barrier(sycl::access::fence_space::local_space);

                             if (lid == 0) {
                                 float total = 0.0f;
                                 for (uint32_t s = 0; s < n_subgroups; s++)
                                     total += s_partials[s];
                                 const size_t wg_idx =
                                     item.get_group(0) * e_wgx + item.get_group(1);
                                 partials[wg_idx] = total;
                             }
                         });
    });
}

template <typename T>
static void copy_y_plane(const VmafPicture *pic, void *dst, unsigned w, unsigned h)
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

} /* anonymous namespace */

extern "C" {

static const VmafOption options_float_psnr_sycl[] = {{0}};

static int init_fex_sycl(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                         unsigned w, unsigned h)
{
    (void)pix_fmt;
    auto *s = static_cast<FloatPsnrStateSycl *>(fex->priv);
    s->width = w;
    s->height = h;
    s->bpc = bpc;
    s->has_pending = false;

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
        return -ENOMEM;
    }

    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (!s->feature_name_dict)
        return -ENOMEM;
    return 0;
}

static int submit_fex_sycl(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                           VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic_90;
    (void)dist_pic_90;
    auto *s = static_cast<FloatPsnrStateSycl *>(fex->priv);
    auto *qptr = static_cast<sycl::queue *>(vmaf_sycl_get_queue_ptr(s->sycl_state));
    if (!qptr)
        return -EINVAL;
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

    launch_float_psnr(q, s->d_ref, s->d_dis, s->d_partials, s->width, s->height, s->bpc,
                      s->wg_count_x);
    q.memcpy(s->h_partials, s->d_partials, (size_t)s->wg_count * sizeof(float));

    s->pending_index = index;
    s->has_pending = true;
    return 0;
}

static int collect_fex_sycl(VmafFeatureExtractor *fex, unsigned index,
                            VmafFeatureCollector *feature_collector)
{
    auto *s = static_cast<FloatPsnrStateSycl *>(fex->priv);
    auto *qptr = static_cast<sycl::queue *>(vmaf_sycl_get_queue_ptr(s->sycl_state));
    if (!qptr)
        return -EINVAL;
    qptr->wait();

    double total = 0.0;
    for (unsigned i = 0; i < s->wg_count; i++)
        total += (double)s->h_partials[i];
    const double n_pix = (double)s->width * (double)s->height;
    const double noise = total / n_pix;
    const double eps = 1e-10;
    const double max_noise = noise > eps ? noise : eps;
    double score = 10.0 * std::log10(s->peak * s->peak / max_noise);
    if (score > s->psnr_max)
        score = s->psnr_max;
    return vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "float_psnr", score, index);
}

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

static const char *provided_features_float_psnr_sycl[] = {"float_psnr", NULL};

extern "C" VmafFeatureExtractor vmaf_fex_float_psnr_sycl = {
    .name = "float_psnr_sycl",
    .init = init_fex_sycl,
    .extract = NULL,
    .flush = NULL,
    .close = close_fex_sycl,
    .submit = submit_fex_sycl,
    .collect = collect_fex_sycl,
    .options = options_float_psnr_sycl,
    .priv_size = sizeof(FloatPsnrStateSycl),
    .flags = VMAF_FEATURE_EXTRACTOR_SYCL,
    .provided_features = provided_features_float_psnr_sycl,
};

} /* extern "C" */
