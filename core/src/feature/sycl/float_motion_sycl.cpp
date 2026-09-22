/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
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

} // namespace

namespace
{

static constexpr int FM_WG_X = 32;
static constexpr int FM_WG_Y = 4;
static constexpr int FM_HALF = 2;
static constexpr int FM_TILE_W = FM_WG_X + 2 * FM_HALF; /* 36 */
static constexpr int FM_TILE_H = FM_WG_Y + 2 * FM_HALF; /* 8  */

static constexpr float FM_FILT[5] = {
    0.054488685f, 0.244201342f, 0.402619947f, 0.244201342f, 0.054488685f,
};

struct FmKernelArgs {
    const void *ref;
    float *cur_blur;
    const float *prev_blur;
    float *sad_partials;
    unsigned width;
    unsigned height;
    unsigned bpc;
    unsigned compute_sad;
    unsigned wg_count_x;
};

} // namespace

namespace
{

static inline int dev_mirror_fm(int idx, int sup)
{
    if (idx < 0) {
        return -idx;
    }
    if (idx >= sup) {
        return 2 * (sup - 1) - idx;
    }
    return idx;
}

static inline float fm_inv_scaler(unsigned bpc)
{
    float scaler = 1.0f;
    if (bpc == 10) {
        scaler = 4.0f;
    } else if (bpc == 12) {
        scaler = 16.0f;
    } else if (bpc == 16) {
        scaler = 256.0f;
    }
    return 1.0f / scaler;
}

} // namespace

namespace
{

static inline float fm_read_pixel(const FmKernelArgs &args, int y, int x, float inv_scaler)
{
    const size_t offset = (size_t)y * args.width + (size_t)x;
    if (args.bpc <= 8) {
        const uint8_t value = static_cast<const uint8_t *>(args.ref)[offset];
        return (float)value - 128.0f;
    }
    const uint16_t value = static_cast<const uint16_t *>(args.ref)[offset];
    return (float)value * inv_scaler - 128.0f;
}

} // namespace

namespace
{

static inline void fm_load_tile(sycl::nd_item<2> item, const sycl::local_accessor<float, 2> &tile,
                                const FmKernelArgs &args, float inv_scaler)
{
    const unsigned lid = item.get_local_linear_id();
    const int tile_y = (int)(item.get_group(0) * FM_WG_Y) - FM_HALF;
    const int tile_x = (int)(item.get_group(1) * FM_WG_X) - FM_HALF;
    const bool interior = (tile_y >= 0) && (tile_y + FM_TILE_H <= (int)args.height) &&
                          (tile_x >= 0) && (tile_x + FM_TILE_W <= (int)args.width);
    constexpr unsigned tile_elems = FM_TILE_H * FM_TILE_W;
    constexpr unsigned group_size = FM_WG_X * FM_WG_Y;
    for (unsigned i = lid; i < tile_elems; i += group_size) {
        const unsigned row = i / FM_TILE_W;
        const unsigned col = i % FM_TILE_W;
        int pixel_y = tile_y + (int)row;
        int pixel_x = tile_x + (int)col;
        if (!interior) {
            pixel_y = dev_mirror_fm(pixel_y, (int)args.height);
            pixel_x = dev_mirror_fm(pixel_x, (int)args.width);
        }
        tile[row][col] = fm_read_pixel(args, pixel_y, pixel_x, inv_scaler);
    }
}

} // namespace

namespace
{

static inline void fm_filter_vertical(sycl::nd_item<2> item,
                                      const sycl::local_accessor<float, 2> &tile,
                                      const sycl::local_accessor<float, 2> &vertical)
{
    constexpr unsigned group_size = FM_WG_X * FM_WG_Y;
    for (unsigned i = item.get_local_linear_id(); i < (unsigned)(FM_WG_Y * FM_TILE_W);
         i += group_size) {
        const unsigned row = i / FM_TILE_W;
        const unsigned col = i % FM_TILE_W;
        float sum = 0.0f;
        for (int k = 0; k < 5; k++) {
            sum += FM_FILT[k] * tile[row + k][col];
        }
        vertical[row][col] = sum;
    }
}

} // namespace

namespace
{

static inline float fm_filter_horizontal(sycl::nd_item<2> item,
                                         const sycl::local_accessor<float, 2> &vertical,
                                         const FmKernelArgs &args)
{
    const int x = (int)item.get_global_id(1);
    const int y = (int)item.get_global_id(0);
    if (!std::cmp_less(x, args.width) || !std::cmp_less(y, args.height)) {
        return 0.0f;
    }
    const unsigned local_x = item.get_local_id(1);
    const unsigned local_y = item.get_local_id(0);
    float blurred = 0.0f;
    for (int k = 0; k < 5; k++) {
        blurred += FM_FILT[k] * vertical[local_y][local_x + k];
    }
    const size_t offset = (size_t)y * args.width + (size_t)x;
    args.cur_blur[offset] = blurred;
    if (args.compute_sad == 0u) {
        return 0.0f;
    }
    const float diff = blurred - args.prev_blur[offset];
    return diff < 0.0f ? -diff : diff;
}

} // namespace

namespace
{

static inline void fm_store_sad(sycl::nd_item<2> item,
                                const sycl::local_accessor<float, 1> &scratch, float abs_diff,
                                const FmKernelArgs &args)
{
    const unsigned lid = item.get_local_linear_id();
    const size_t group_index = item.get_group(0) * args.wg_count_x + item.get_group(1);
    if (args.compute_sad == 0u) {
        if (lid == 0) {
            args.sad_partials[group_index] = 0.0f;
        }
        return;
    }
    sycl::sub_group const subgroup = item.get_sub_group();
    const float subgroup_sum = sycl::reduce_over_group(subgroup, abs_diff, sycl::plus<float>{});
    const uint32_t subgroup_id = subgroup.get_group_linear_id();
    const uint32_t subgroup_lane = subgroup.get_local_linear_id();
    const uint32_t subgroup_count = subgroup.get_group_linear_range();
    if (subgroup_lane == 0) {
        scratch[subgroup_id] = subgroup_sum;
    }
    item.barrier(sycl::access::fence_space::local_space);
    if (lid == 0) {
        float total = 0.0f;
        for (uint32_t i = 0; i < subgroup_count; i++) {
            total += scratch[i];
        }
        args.sad_partials[group_index] = total;
    }
}

} // namespace

namespace
{

static sycl::event launch_float_motion(sycl::queue &q, const FmKernelArgs &args)
{
    const size_t global_x = ((static_cast<size_t>(args.width) + FM_WG_X - 1) / FM_WG_X) * FM_WG_X;
    const size_t global_y = ((static_cast<size_t>(args.height) + FM_WG_Y - 1) / FM_WG_Y) * FM_WG_Y;
    return q.submit([&](sycl::handler &cgh) {
        sycl::local_accessor<float, 2> const s_tile(sycl::range<2>(FM_TILE_H, FM_TILE_W), cgh);
        sycl::local_accessor<float, 2> const s_vert(sycl::range<2>(FM_WG_Y, FM_TILE_W), cgh);
        constexpr int MAX_SUBGROUPS = FM_WG_X * FM_WG_Y;
        sycl::local_accessor<float, 1> const s_sad(sycl::range<1>(MAX_SUBGROUPS), cgh);

        cgh.parallel_for(
            sycl::nd_range<2>(sycl::range<2>(global_y, global_x), sycl::range<2>(FM_WG_Y, FM_WG_X)),
            [=](sycl::nd_item<2> item) VMAF_SYCL_REQD_SG_SIZE(32) {
                fm_load_tile(item, s_tile, args, fm_inv_scaler(args.bpc));
                item.barrier(sycl::access::fence_space::local_space);
                fm_filter_vertical(item, s_tile, s_vert);
                item.barrier(sycl::access::fence_space::local_space);
                const float abs_diff = fm_filter_horizontal(item, s_vert, args);
                fm_store_sad(item, s_sad, abs_diff, args);
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

static const VmafOption options_float_motion_sycl[] = {
    {.name = "debug",
     .help = "debug mode: enable additional output",
     .offset = offsetof(FloatMotionStateSycl, debug),
     .type = VMAF_OPT_TYPE_BOOL,
     .default_val = {.b = true}},
    {.name = "motion_force_zero",
     .help = "force motion score to zero",
     .alias = "force_0",
     .offset = offsetof(FloatMotionStateSycl, motion_force_zero),
     .type = VMAF_OPT_TYPE_BOOL,
     .default_val = {.b = false},
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "motion_fps_weight",
     .help = "fps-aware multiplicative weight/correction",
     .alias = "mfw",
     .offset = offsetof(FloatMotionStateSycl, motion_fps_weight),
     .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val = {.d = 1.0},
     .min = 0.0,
     .max = 5.0,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = nullptr}};

} // namespace

namespace
{

static int allocate_motion_buffers(FloatMotionStateSycl *s)
{
    VmafSyclState *state = s->sycl_state;
    s->plane_bytes = (size_t)s->width * s->height * (s->bpc <= 8 ? 1u : 2u);
    s->h_ref = vmaf_sycl_malloc_host(state, s->plane_bytes);
    s->d_ref = vmaf_sycl_malloc_device(state, s->plane_bytes);
    const size_t blur_bytes = (size_t)s->width * s->height * sizeof(float);
    s->d_blur[0] = static_cast<float *>(vmaf_sycl_malloc_device(state, blur_bytes));
    s->d_blur[1] = static_cast<float *>(vmaf_sycl_malloc_device(state, blur_bytes));
    s->wg_count_x = (unsigned)((s->width + FM_WG_X - 1) / FM_WG_X);
    s->wg_count_y = (unsigned)((s->height + FM_WG_Y - 1) / FM_WG_Y);
    s->wg_count = s->wg_count_x * s->wg_count_y;
    const size_t sad_bytes = (size_t)s->wg_count * sizeof(float);
    s->d_sad = static_cast<float *>(vmaf_sycl_malloc_device(state, sad_bytes));
    s->h_sad = static_cast<float *>(vmaf_sycl_malloc_host(state, sad_bytes));
    if (!s->h_ref || !s->d_ref || !s->d_blur[0] || !s->d_blur[1] || !s->d_sad || !s->h_sad) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "float_motion_sycl: USM allocation failed\n");
        return -ENOMEM;
    }
    return 0;
}

} // namespace

namespace
{

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
    s->sycl_state = fex->sycl_state;
    const int alloc_err = allocate_motion_buffers(s);
    if (alloc_err) {
        return alloc_err;
    }

    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (!s->feature_name_dict) {
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
    (void)dist_pic;
    (void)dist_pic_90;
    auto *s = static_cast<FloatMotionStateSycl *>(fex->priv);
    auto *qptr = static_cast<sycl::queue *>(vmaf_sycl_get_queue_ptr(s->sycl_state));
    if (!qptr) {
        return -EINVAL;
    }
    sycl::queue &q = *qptr;

    if (s->bpc <= 8) {
        copy_y_plane<uint8_t>(ref_pic, s->h_ref, s->width, s->height);
    } else {
        copy_y_plane<uint16_t>(ref_pic, s->h_ref, s->width, s->height);
    }
    q.memcpy(s->d_ref, s->h_ref, s->plane_bytes);

    const unsigned cur_idx = (unsigned)s->cur_blur;
    const unsigned prev_idx = 1u - cur_idx;
    const unsigned compute_sad = (s->frame_index > 0) ? 1u : 0u;
    launch_float_motion(q, {.ref = s->d_ref,
                            .cur_blur = s->d_blur[cur_idx],
                            .prev_blur = s->d_blur[prev_idx],
                            .sad_partials = s->d_sad,
                            .width = s->width,
                            .height = s->height,
                            .bpc = s->bpc,
                            .compute_sad = compute_sad,
                            .wg_count_x = s->wg_count_x});
    if (compute_sad != 0u) {
        q.memcpy(s->h_sad, s->d_sad, (size_t)s->wg_count * sizeof(float));
    }

    s->pending_index = index;
    s->has_pending = true;
    return 0;
}

} // namespace

namespace
{

static double reduce_sad(const FloatMotionStateSycl *s)
{
    double total = 0.0;
    for (unsigned i = 0; i < s->wg_count; i++) {
        total += (double)s->h_sad[i];
    }
    return total / ((double)s->width * s->height);
}

} // namespace

namespace
{

static int collect_fex_sycl(VmafFeatureExtractor *fex, unsigned index,
                            VmafFeatureCollector *feature_collector)
{
    auto *s = static_cast<FloatMotionStateSycl *>(fex->priv);
    auto *qptr = static_cast<sycl::queue *>(vmaf_sycl_get_queue_ptr(s->sycl_state));
    if (!qptr) {
        return -EINVAL;
    }
    qptr->wait();

    int err = 0;

    if (s->frame_index == 0) {
        err = vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                      "VMAF_feature_motion2_score", 0.0, index);
        if (s->debug && !err) {
            err = vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                          "VMAF_feature_motion_score", 0.0, index);
        }
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
        if (s->debug && !err) {
            err = vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                          "VMAF_feature_motion_score", motion_score,
                                                          index);
        }
    }

    s->prev_motion_score = motion_score;
    s->cur_blur = 1 - s->cur_blur;
    s->frame_index++;
    return err;
}

} // namespace

namespace
{

static int flush_fex_sycl(VmafFeatureExtractor *fex, VmafFeatureCollector *feature_collector)
{
    auto *s = static_cast<FloatMotionStateSycl *>(fex->priv);
    int ret = 0;
    if (s->motion_force_zero) {
        return 1;
    }

    if (s->frame_index > 1) {
        /* Apply fps weight on the tail motion2 — mirrors the collect path.
         * Bit-exact when motion_fps_weight = 1.0 (default). */
        ret = vmaf_feature_collector_append_with_dict(
            feature_collector, s->feature_name_dict, "VMAF_feature_motion2_score",
            s->prev_motion_score * s->motion_fps_weight, s->frame_index - 1);
    }
    return (ret < 0) ? ret : !ret;
}

} // namespace

namespace
{

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
                                                            "VMAF_feature_motion2_score", nullptr};

} // namespace

extern "C" VmafFeatureExtractor vmaf_fex_float_motion_sycl = {
    .name = "float_motion_sycl",
    .init = init_fex_sycl,
    .extract = nullptr,
    .flush = flush_fex_sycl,
    .close = close_fex_sycl,
    .submit = submit_fex_sycl,
    .collect = collect_fex_sycl,
    .options = options_float_motion_sycl,
    .priv_size = sizeof(FloatMotionStateSycl),
    .flags = VMAF_FEATURE_EXTRACTOR_TEMPORAL | VMAF_FEATURE_EXTRACTOR_SYCL,
    .provided_features = provided_features_float_motion_sycl,
};
