/**
 *  Copyright 2026 Lusoris and Claude (Anthropic)
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 *
 *  float_motion feature kernel on the SYCL backend (T7-23 / batch 3
 *  part 4c — ADR-0192 / ADR-0196). SYCL twin of float_motion_vulkan
 *  + float_motion_cuda. Self-contained submit/collect.
 */

#include <sycl/sycl.hpp>

#include "sycl_compat.h"

#include <cerrno>
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

struct FloatMotionStateSycl {
    bool debug;
    bool motion_force_zero;
    double motion_fps_weight;

    unsigned width;
    unsigned height;
    unsigned bpc;
    size_t plane_bytes;

    VmafSyclState *sycl_state;

    void *h_ref;
    void *d_ref;

    /* Ping-pong of float blurred refs. */
    float *d_blur[2];
    int cur_blur;

    /* Per-WG float SAD partials. */
    float *d_sad;
    float *h_sad;
    unsigned wg_count_x;
    unsigned wg_count_y;
    unsigned wg_count;

    bool has_pending;
    unsigned pending_index;
    unsigned frame_index;
    double prev_motion_score;

    VmafDictionary *feature_name_dict;
};

static constexpr int FM_WG_X = 32;
static constexpr int FM_WG_Y = 4;
static constexpr int FM_HALF = 2;
static constexpr int FM_TILE_W = FM_WG_X + 2 * FM_HALF; /* 36 */
static constexpr int FM_TILE_H = FM_WG_Y + 2 * FM_HALF; /* 8  */

static constexpr float FM_FILT[5] = {
    0.054488685f, 0.244201342f, 0.402619947f, 0.244201342f, 0.054488685f,
};

static inline int dev_mirror_fm(int idx, int sup)
{
    if (idx < 0)
        return -idx;
    if (idx >= sup)
        return 2 * (sup - 1) - idx;
    return idx;
}

static sycl::event launch_float_motion(sycl::queue &q, const void *ref, float *cur_blur,
                                       const float *prev_blur, float *sad_partials, unsigned width,
                                       unsigned height, unsigned bpc, unsigned compute_sad,
                                       unsigned wg_count_x)
{
    const size_t global_x = ((width + FM_WG_X - 1) / FM_WG_X) * FM_WG_X;
    const size_t global_y = ((height + FM_WG_Y - 1) / FM_WG_Y) * FM_WG_Y;
    const unsigned e_w = width;
    const unsigned e_h = height;
    const unsigned e_bpc = bpc;
    const unsigned e_compute_sad = compute_sad;
    const unsigned e_wgx = wg_count_x;
    const void *e_ref = ref;
    float *e_cur = cur_blur;
    const float *e_prev = prev_blur;
    float *e_sad = sad_partials;

    return q.submit([&](sycl::handler &cgh) {
        sycl::local_accessor<float, 2> s_tile(sycl::range<2>(FM_TILE_H, FM_TILE_W), cgh);
        sycl::local_accessor<float, 2> s_vert(sycl::range<2>(FM_WG_Y, FM_TILE_W), cgh);
        constexpr int MAX_SUBGROUPS = FM_WG_X * FM_WG_Y;
        sycl::local_accessor<float, 1> s_sad(sycl::range<1>(MAX_SUBGROUPS), cgh);

        cgh.parallel_for(
            sycl::nd_range<2>(sycl::range<2>(global_y, global_x), sycl::range<2>(FM_WG_Y, FM_WG_X)),
            [=](sycl::nd_item<2> item) VMAF_SYCL_REQD_SG_SIZE(32) {
                const int gx = (int)item.get_global_id(1);
                const int gy = (int)item.get_global_id(0);
                const unsigned lid = item.get_local_linear_id();
                const unsigned lx = item.get_local_id(1);
                const unsigned ly = item.get_local_id(0);
                const bool valid = (gx < (int)e_w && gy < (int)e_h);

                float scaler = 1.0f;
                if (e_bpc == 10)
                    scaler = 4.0f;
                else if (e_bpc == 12)
                    scaler = 16.0f;
                else if (e_bpc == 16)
                    scaler = 256.0f;
                const float inv_scaler = 1.0f / scaler;

                /* Phase 1: tile load */
                const int tile_oy = (int)(item.get_group(0) * FM_WG_Y) - FM_HALF;
                const int tile_ox = (int)(item.get_group(1) * FM_WG_X) - FM_HALF;
                constexpr unsigned tile_elems = FM_TILE_H * FM_TILE_W;
                constexpr unsigned WG_SIZE = FM_WG_X * FM_WG_Y;

                auto read_pix = [&](int y, int x) -> float {
                    if (e_bpc <= 8) {
                        const uint8_t v =
                            static_cast<const uint8_t *>(e_ref)[(size_t)y * e_w + (size_t)x];
                        return (float)v - 128.0f;
                    }
                    const uint16_t v =
                        static_cast<const uint16_t *>(e_ref)[(size_t)y * e_w + (size_t)x];
                    return (float)v * inv_scaler - 128.0f;
                };

                const bool interior = (tile_oy >= 0) && (tile_oy + FM_TILE_H <= (int)e_h) &&
                                      (tile_ox >= 0) && (tile_ox + FM_TILE_W <= (int)e_w);

                for (unsigned i = lid; i < tile_elems; i += WG_SIZE) {
                    const unsigned tr = i / FM_TILE_W;
                    const unsigned tc = i % FM_TILE_W;
                    int py = tile_oy + (int)tr;
                    int px = tile_ox + (int)tc;
                    if (!interior) {
                        py = dev_mirror_fm(py, (int)e_h);
                        px = dev_mirror_fm(px, (int)e_w);
                    }
                    s_tile[tr][tc] = read_pix(py, px);
                }
                item.barrier(sycl::access::fence_space::local_space);

                /* Phase 2: vertical filter */
                for (unsigned i = lid; i < (unsigned)(FM_WG_Y * FM_TILE_W); i += WG_SIZE) {
                    const unsigned r = i / FM_TILE_W;
                    const unsigned c = i % FM_TILE_W;
                    float acc = 0.0f;
                    for (int k = 0; k < 5; k++)
                        acc += FM_FILT[k] * s_tile[r + k][c];
                    s_vert[r][c] = acc;
                }
                item.barrier(sycl::access::fence_space::local_space);

                /* Phase 3: horizontal filter + SAD */
                float abs_diff = 0.0f;
                if (valid) {
                    float blurred = 0.0f;
                    for (int k = 0; k < 5; k++)
                        blurred += FM_FILT[k] * s_vert[ly][lx + k];
                    e_cur[(size_t)gy * e_w + (size_t)gx] = blurred;

                    if (e_compute_sad != 0u) {
                        const float prev = e_prev[(size_t)gy * e_w + (size_t)gx];
                        const float diff = blurred - prev;
                        abs_diff = diff < 0.0f ? -diff : diff;
                    }
                }

                /* Phase 4: subgroup + cross-subgroup reduction */
                if (e_compute_sad != 0u) {
                    sycl::sub_group sg = item.get_sub_group();
                    const float sg_sum = sycl::reduce_over_group(sg, abs_diff, sycl::plus<float>{});
                    const uint32_t sg_id = sg.get_group_linear_id();
                    const uint32_t sg_lid = sg.get_local_linear_id();
                    const uint32_t n_subgroups = sg.get_group_linear_range();
                    if (sg_lid == 0)
                        s_sad[sg_id] = sg_sum;
                    item.barrier(sycl::access::fence_space::local_space);
                    if (lid == 0) {
                        float total = 0.0f;
                        for (uint32_t s = 0; s < n_subgroups; s++)
                            total += s_sad[s];
                        const size_t wg_idx = item.get_group(0) * e_wgx + item.get_group(1);
                        e_sad[wg_idx] = total;
                    }
                } else {
                    if (lid == 0) {
                        const size_t wg_idx = item.get_group(0) * e_wgx + item.get_group(1);
                        e_sad[wg_idx] = 0.0f;
                    }
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

static const VmafOption options_float_motion_sycl[] = {
    {.name = "debug",
     .help = "debug mode: enable additional output",
     .offset = offsetof(FloatMotionStateSycl, debug),
     .type = VMAF_OPT_TYPE_BOOL,
     .default_val = {.b = true}},
    {.name = "motion_force_zero",
     .alias = "force_0",
     .help = "force motion score to zero",
     .offset = offsetof(FloatMotionStateSycl, motion_force_zero),
     .type = VMAF_OPT_TYPE_BOOL,
     .default_val = {.b = false},
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "motion_fps_weight",
     .alias = "mfw",
     .help = "fps-aware multiplicative weight/correction",
     .offset = offsetof(FloatMotionStateSycl, motion_fps_weight),
     .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val = {.d = 1.0},
     .min = 0.0,
     .max = 5.0,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {0}};

static int init_fex_sycl(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                         unsigned w, unsigned h)
{
    (void)pix_fmt;
    auto *s = static_cast<FloatMotionStateSycl *>(fex->priv);

    /* The 5-tap SYCL float_motion kernel uses reflect-101 mirror padding;
     * dev_mirror_fm() returns 2*sup - idx - 2, which is negative when sup < 3.
     * Refuse smaller frames up front to prevent out-of-bounds device reads.
     * Minimum: filter_width/2 + 1 = 3. */
    if (h < 3u || w < 3u) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "float_motion_sycl: frame %ux%u is below the 5-tap filter minimum 3x3; "
                 "refusing to avoid out-of-bounds mirror reads on device\n",
                 w, h);
        return -EINVAL;
    }

    s->width = w;
    s->height = h;
    s->bpc = bpc;
    s->frame_index = 0;
    s->prev_motion_score = 0.0;
    s->cur_blur = 0;
    s->has_pending = false;

    if (!fex->sycl_state) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "float_motion_sycl: no SYCL state\n");
        return -EINVAL;
    }
    VmafSyclState *state = fex->sycl_state;
    s->sycl_state = state;

    s->plane_bytes = (size_t)w * h * (bpc <= 8 ? 1u : 2u);
    s->h_ref = vmaf_sycl_malloc_host(state, s->plane_bytes);
    s->d_ref = vmaf_sycl_malloc_device(state, s->plane_bytes);

    const size_t blur_bytes = (size_t)w * h * sizeof(float);
    s->d_blur[0] = static_cast<float *>(vmaf_sycl_malloc_device(state, blur_bytes));
    s->d_blur[1] = static_cast<float *>(vmaf_sycl_malloc_device(state, blur_bytes));

    s->wg_count_x = (unsigned)((w + FM_WG_X - 1) / FM_WG_X);
    s->wg_count_y = (unsigned)((h + FM_WG_Y - 1) / FM_WG_Y);
    s->wg_count = s->wg_count_x * s->wg_count_y;
    const size_t sad_bytes = (size_t)s->wg_count * sizeof(float);
    s->d_sad = static_cast<float *>(vmaf_sycl_malloc_device(state, sad_bytes));
    s->h_sad = static_cast<float *>(vmaf_sycl_malloc_host(state, sad_bytes));

    if (!s->h_ref || !s->d_ref || !s->d_blur[0] || !s->d_blur[1] || !s->d_sad || !s->h_sad) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "float_motion_sycl: USM allocation failed\n");
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
    (void)dist_pic;
    (void)dist_pic_90;
    auto *s = static_cast<FloatMotionStateSycl *>(fex->priv);
    auto *qptr = static_cast<sycl::queue *>(vmaf_sycl_get_queue_ptr(s->sycl_state));
    if (!qptr)
        return -EINVAL;
    sycl::queue &q = *qptr;

    if (s->bpc <= 8)
        copy_y_plane<uint8_t>(ref_pic, s->h_ref, s->width, s->height);
    else
        copy_y_plane<uint16_t>(ref_pic, s->h_ref, s->width, s->height);
    q.memcpy(s->d_ref, s->h_ref, s->plane_bytes);

    const unsigned cur_idx = (unsigned)s->cur_blur;
    const unsigned prev_idx = 1u - cur_idx;
    const unsigned compute_sad = (s->frame_index > 0) ? 1u : 0u;
    launch_float_motion(q, s->d_ref, s->d_blur[cur_idx], s->d_blur[prev_idx], s->d_sad, s->width,
                        s->height, s->bpc, compute_sad, s->wg_count_x);
    if (compute_sad != 0u)
        q.memcpy(s->h_sad, s->d_sad, (size_t)s->wg_count * sizeof(float));

    s->pending_index = index;
    s->has_pending = true;
    return 0;
}

static double reduce_sad(const FloatMotionStateSycl *s)
{
    double total = 0.0;
    for (unsigned i = 0; i < s->wg_count; i++)
        total += (double)s->h_sad[i];
    return total / ((double)s->width * s->height);
}

static int collect_fex_sycl(VmafFeatureExtractor *fex, unsigned index,
                            VmafFeatureCollector *feature_collector)
{
    auto *s = static_cast<FloatMotionStateSycl *>(fex->priv);
    auto *qptr = static_cast<sycl::queue *>(vmaf_sycl_get_queue_ptr(s->sycl_state));
    if (!qptr)
        return -EINVAL;
    qptr->wait();

    int err = 0;

    if (s->frame_index == 0) {
        err = vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                      "VMAF_feature_motion2_score", 0.0, index);
        if (s->debug && !err)
            err = vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                          "VMAF_feature_motion_score", 0.0, index);
        s->cur_blur = 1 - s->cur_blur;
        s->frame_index++;
        return err;
    }

    const double motion_score = reduce_sad(s);

    if (s->frame_index == 1) {
        if (s->debug) {
            err = vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                          "VMAF_feature_motion_score", motion_score,
                                                          index);
        }
    } else {
        /* Apply fps weight before taking the min — mirrors float_motion.c CPU path.
         * Bit-exact when motion_fps_weight = 1.0 (default). */
        const double w_cur = motion_score * s->motion_fps_weight;
        const double w_prev = s->prev_motion_score * s->motion_fps_weight;
        const double motion2 = (w_cur < w_prev) ? w_cur : w_prev;
        err = vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                      "VMAF_feature_motion2_score", motion2,
                                                      index - 1);
        if (s->debug && !err)
            err = vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                          "VMAF_feature_motion_score", motion_score,
                                                          index);
    }

    s->prev_motion_score = motion_score;
    s->cur_blur = 1 - s->cur_blur;
    s->frame_index++;
    return err;
}

static int flush_fex_sycl(VmafFeatureExtractor *fex, VmafFeatureCollector *feature_collector)
{
    auto *s = static_cast<FloatMotionStateSycl *>(fex->priv);
    int ret = 0;
    if (s->motion_force_zero)
        return 1;

    if (s->frame_index > 1) {
        /* Apply fps weight on the tail motion2 — mirrors the collect path.
         * Bit-exact when motion_fps_weight = 1.0 (default). */
        ret = vmaf_feature_collector_append_with_dict(
            feature_collector, s->feature_name_dict, "VMAF_feature_motion2_score",
            s->prev_motion_score * s->motion_fps_weight, s->frame_index - 1);
    }
    return (ret < 0) ? ret : !ret;
}

static int close_fex_sycl(VmafFeatureExtractor *fex)
{
    auto *s = static_cast<FloatMotionStateSycl *>(fex->priv);
    if (s->sycl_state) {
        if (s->h_ref)
            vmaf_sycl_free(s->sycl_state, s->h_ref);
        if (s->d_ref)
            vmaf_sycl_free(s->sycl_state, s->d_ref);
        if (s->d_blur[0])
            vmaf_sycl_free(s->sycl_state, s->d_blur[0]);
        if (s->d_blur[1])
            vmaf_sycl_free(s->sycl_state, s->d_blur[1]);
        if (s->d_sad)
            vmaf_sycl_free(s->sycl_state, s->d_sad);
        if (s->h_sad)
            vmaf_sycl_free(s->sycl_state, s->h_sad);
    }
    if (s->feature_name_dict)
        vmaf_dictionary_free(&s->feature_name_dict);
    return 0;
}

static const char *provided_features_float_motion_sycl[] = {"VMAF_feature_motion_score",
                                                            "VMAF_feature_motion2_score", NULL};

extern "C" VmafFeatureExtractor vmaf_fex_float_motion_sycl = {
    .name = "float_motion_sycl",
    .init = init_fex_sycl,
    .extract = NULL,
    .flush = flush_fex_sycl,
    .close = close_fex_sycl,
    .submit = submit_fex_sycl,
    .collect = collect_fex_sycl,
    .options = options_float_motion_sycl,
    .priv_size = sizeof(FloatMotionStateSycl),
    .flags = VMAF_FEATURE_EXTRACTOR_TEMPORAL | VMAF_FEATURE_EXTRACTOR_SYCL,
    .provided_features = provided_features_float_motion_sycl,
};

} /* extern "C" */
