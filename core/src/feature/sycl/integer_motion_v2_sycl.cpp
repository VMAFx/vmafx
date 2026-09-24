/**
 *  Copyright 2016-2025 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  motion_v2 feature kernel on the SYCL backend (T7-23 / batch 3 part
 *  1c — ADR-0192 / ADR-0193). SYCL twin of motion_v2_vulkan (PR #146)
 *  and motion_v2_cuda (this PR's part 1b).
 *
 *  Stateless variant of `motion_sycl`: exploits convolution linearity
 *  (`SAD(blur(prev), blur(cur)) == sum(|blur(prev - cur)|)`) so each
 *  frame computes its score in one kernel launch over (prev_ref - cur_ref)
 *  without storing blurred frames across submits.
 *
 *  Self-contained submit / collect — does NOT register with
 *  vmaf_sycl_graph_register because motion_v2 needs the previous
 *  frame's raw ref pixels which the shared_frame buffer doesn't
 *  preserve across calls. Each submit copies the current ref Y plane
 *  into a private device-side ping-pong (`d_pix[2]`); the next
 *  frame's submit reads it as "prev". Same shape as ciede_sycl
 *  (PR #137 / ADR-0182) and the Vulkan / CUDA twins of this kernel.
 *
 *  motion2_v2_score = min(score[i], score[i+1]) and motion3_v2_score
 *  (per-frame blend + clip + optional moving-average) are both emitted
 *  host-side in flush() — mirrors CPU integer_motion_v2.c::flush and the
 *  CUDA twin integer_motion_v2_cuda.c::flush_fex_cuda. The motion3_v2
 *  post-process and its option surface were added in ADR-1108 (the
 *  cross-backend follow-up to the CUDA twin landed in #909).
 *
 *  Mirror padding: reflect-101 (`2 * size - idx - 2` for idx >= size),
 *  matching CPU `integer_motion_v2.c::mirror` and `motion_sycl`.
 */

#include <sycl/sycl.hpp>

#include "sycl_compat.h"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <utility>

#include "config.h"
#include "dict.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "log.h"
#include "motion_blend_tools.h"
#include "picture.h"
#include "sycl/common.h"

/* Default maximum value allowed for motion — mirrors
 * DEFAULT_MOTION_MAX_VAL in integer_motion_v2.c (the CPU reference). */
#define MOTION_V2_SYCL_DEFAULT_MAX_VAL (10000.0)

namespace
{

struct MotionV2StateSycl {
    /* Frame geometry. */
    unsigned width;
    unsigned height;
    unsigned bpc;
    size_t plane_bytes;

    /* SYCL state back-pointer. */
    VmafSyclState *sycl_state;

    /* Pinned host staging — single buffer reused per submit for the
     * cur ref Y upload. */
    void *h_pix;

    /* Ping-pong of raw ref Y planes on device. d_pix[index%2] is the
     * current frame's slot; d_pix[(index+1)%2] is the previous. */
    void *d_pix[2];

    /* Single int64 SAD accumulator (device + host). */
    int64_t *d_sad;
    int64_t *h_sad;

    /* Submit/collect plumbing. */
    bool has_pending;
    unsigned pending_index;
    unsigned frame_index;

    /* fps-aware weight applied to the v2 SAD score in flush().
     * Default 1.0 is a no-op. Mirrors motion_sycl and motion_cuda
     * (ADR-0192 / PR #851). */
    double motion_fps_weight;

    /* motion3_v2 post-process options — mirror the CPU reference
     * (integer_motion_v2.c) option table byte-for-byte so a model
     * file carrying `motion_v2_sycl=motion_blend_factor=…` loads
     * and scores identically to the CPU and CUDA paths (ADR-1108). */
    double motion_blend_factor;
    double motion_blend_offset;
    double motion_max_val;
    bool motion_moving_average;

    VmafDictionary *feature_name_dict;
};

} // namespace

namespace
{

static constexpr int32_t MV2_FILTER[5] = {3571, 16004, 26386, 16004, 3571};
static constexpr int MV2_WG_X = 32;
static constexpr int MV2_WG_Y = 8;
static constexpr int MV2_HALF_FW = 2;
static constexpr int MV2_TILE_W = MV2_WG_X + 2 * MV2_HALF_FW; /* 36 */
static constexpr int MV2_TILE_H = MV2_WG_Y + 2 * MV2_HALF_FW; /* 12 */

struct Mv2KernelArgs {
    const void *prev;
    const void *cur;
    int64_t *sad;
    unsigned width;
    unsigned height;
    unsigned bpc;
};

} // namespace

namespace
{

static inline int dev_mirror_mv2(int idx, int sup)
{
    if (idx < 0) {
        return -idx;
    }
    if (idx >= sup) {
        return 2 * sup - idx - 2;
    }
    return idx;
}

static inline int32_t mv2_read_pixel(const void *plane, int y, int x, const Mv2KernelArgs &args)
{
    const size_t offset = (size_t)y * args.width + (size_t)x;
    if (args.bpc <= 8) {
        return (int32_t)static_cast<const uint8_t *>(plane)[offset];
    }
    return (int32_t)static_cast<const uint16_t *>(plane)[offset];
}

} // namespace

namespace
{

static inline void mv2_load_diff(sycl::nd_item<2> item,
                                 const sycl::local_accessor<int32_t, 2> &diff,
                                 const Mv2KernelArgs &args)
{
    const unsigned lid = item.get_local_linear_id();
    const int tile_y = (int)(item.get_group(0) * MV2_WG_Y) - MV2_HALF_FW;
    const int tile_x = (int)(item.get_group(1) * MV2_WG_X) - MV2_HALF_FW;
    const bool interior = (tile_y >= 0) && (tile_y + MV2_TILE_H <= (int)args.height) &&
                          (tile_x >= 0) && (tile_x + MV2_TILE_W <= (int)args.width);
    constexpr unsigned tile_elems = MV2_TILE_H * MV2_TILE_W;
    constexpr unsigned group_size = MV2_WG_X * MV2_WG_Y;
    for (unsigned i = lid; i < tile_elems; i += group_size) {
        const unsigned row = i / MV2_TILE_W;
        const unsigned col = i % MV2_TILE_W;
        int pixel_y = tile_y + (int)row;
        int pixel_x = tile_x + (int)col;
        if (!interior) {
            pixel_y = dev_mirror_mv2(pixel_y, (int)args.height);
            pixel_x = dev_mirror_mv2(pixel_x, (int)args.width);
        }
        diff[row][col] = mv2_read_pixel(args.prev, pixel_y, pixel_x, args) -
                         mv2_read_pixel(args.cur, pixel_y, pixel_x, args);
    }
}

} // namespace

namespace
{

static inline int64_t mv2_filtered_abs(sycl::nd_item<2> item,
                                       const sycl::local_accessor<int32_t, 2> &diff,
                                       const Mv2KernelArgs &args)
{
    const int x = (int)item.get_global_id(1);
    const int y = (int)item.get_global_id(0);
    if (!std::cmp_less(x, args.width) || !std::cmp_less(y, args.height)) {
        return 0;
    }
    const unsigned local_x = item.get_local_id(1);
    const unsigned local_y = item.get_local_id(0);
    const int64_t round_y = (int64_t)1 << ((int)args.bpc - 1);
    const int shift_y = (int)args.bpc;
    int32_t vertical[5];
#pragma unroll
    for (int hx = 0; hx < 5; hx++) {
        const unsigned tile_col = local_x + (unsigned)hx;
        int64_t const sum =
            (int64_t)MV2_FILTER[0] * (diff[local_y][tile_col] + diff[local_y + 4][tile_col]) +
            (int64_t)MV2_FILTER[1] * (diff[local_y + 1][tile_col] + diff[local_y + 3][tile_col]) +
            (int64_t)MV2_FILTER[2] * diff[local_y + 2][tile_col];
        vertical[hx] = (int32_t)((sum + round_y) >> shift_y);
    }
    const int64_t horizontal = (int64_t)MV2_FILTER[0] * (vertical[0] + vertical[4]) +
                               (int64_t)MV2_FILTER[1] * (vertical[1] + vertical[3]) +
                               (int64_t)MV2_FILTER[2] * vertical[2];
    const int64_t blurred = (horizontal + 32768) >> 16;
    return blurred < 0 ? -blurred : blurred;
}

} // namespace

namespace
{

static inline void mv2_reduce_sad(sycl::nd_item<2> item,
                                  const sycl::local_accessor<int64_t, 1> &scratch, int64_t absolute,
                                  int64_t *sad)
{
    sycl::sub_group const subgroup = item.get_sub_group();
    const int64_t subgroup_sum = sycl::reduce_over_group(subgroup, absolute, sycl::plus<int64_t>{});
    const uint32_t subgroup_id = subgroup.get_group_linear_id();
    const uint32_t subgroup_lane = subgroup.get_local_linear_id();
    const uint32_t subgroup_count = subgroup.get_group_linear_range();
    if (subgroup_lane == 0) {
        scratch[subgroup_id] = subgroup_sum;
    }
    item.barrier(sycl::access::fence_space::local_space);
    if (item.get_local_linear_id() == 0) {
        int64_t total = 0;
        for (uint32_t i = 0; i < subgroup_count; i++) {
            total += scratch[i];
        }
        sycl::atomic_ref<int64_t, sycl::memory_order::relaxed, sycl::memory_scope::device,
                         sycl::access::address_space::global_space> const output(*sad);
        output.fetch_add(total);
    }
}

} // namespace

namespace
{

static sycl::event launch_motion_v2(sycl::queue &q, Mv2KernelArgs args)
{
    const size_t global_height = ((size_t)args.height + MV2_WG_Y - 1) / MV2_WG_Y * MV2_WG_Y;
    const size_t global_width = ((size_t)args.width + MV2_WG_X - 1) / MV2_WG_X * MV2_WG_X;
    sycl::range<2> global(global_height, global_width);
    sycl::range<2> local(MV2_WG_Y, MV2_WG_X);

    return q.submit([&](sycl::handler &cgh) {
        sycl::local_accessor<int32_t, 2> const s_diff(sycl::range<2>(MV2_TILE_H, MV2_TILE_W), cgh);
        constexpr int MAX_SUBGROUPS = 32;
        sycl::local_accessor<int64_t, 1> const lmem(sycl::range<1>(MAX_SUBGROUPS), cgh);

        cgh.parallel_for(sycl::nd_range<2>(global, local),
                         [=](sycl::nd_item<2> item) VMAF_SYCL_REQD_SG_SIZE(32) {
                             mv2_load_diff(item, s_diff, args);
                             item.barrier(sycl::access::fence_space::local_space);
                             const int64_t absolute = mv2_filtered_abs(item, s_diff, args);
                             mv2_reduce_sad(item, lmem, absolute, args.sad);
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

/* Option table mirrors integer_motion_v2.c (CPU reference) for the
 * subset of options the SYCL twin's host-side motion3_v2 post-process
 * consumes: name / alias / type / default / min / max / flags match
 * byte-for-byte so co-scheduled CPU+SYCL runs name features identically
 * and model files load on either path (ADR-1108). motion_force_zero and
 * motion_five_frame_window are CPU-only knobs (the SYCL kernel always
 * computes the SAD, and the 5-frame window is unsupported per ADR-0337);
 * they are intentionally omitted from this twin's surface — matching the
 * CUDA twin integer_motion_v2_cuda.c. */
static const VmafOption motion_weight_option = {
    .name = "motion_fps_weight",
    .help = "fps-aware multiplicative weight/correction",
    .alias = "mfw",
    .offset = offsetof(MotionV2StateSycl, motion_fps_weight),
    .type = VMAF_OPT_TYPE_DOUBLE,
    .default_val = {.d = 1.0},
    .min = 0.0,
    .max = 5.0,
    .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
};

static const VmafOption motion_blend_factor_option = {
    .name = "motion_blend_factor",
    .help = "blend motion score given an offset",
    .alias = "mbf",
    .offset = offsetof(MotionV2StateSycl, motion_blend_factor),
    .type = VMAF_OPT_TYPE_DOUBLE,
    .default_val = {.d = 1.0},
    .min = 0.0,
    .max = 1.0,
    .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
};

} // namespace

namespace
{

static const VmafOption motion_blend_offset_option = {
    .name = "motion_blend_offset",
    .help = "blend motion score starting from this offset",
    .alias = "mbo",
    .offset = offsetof(MotionV2StateSycl, motion_blend_offset),
    .type = VMAF_OPT_TYPE_DOUBLE,
    .default_val = {.d = 40.0},
    .min = 0.0,
    .max = 1000.0,
    .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
};

static const VmafOption motion_max_option = {
    .name = "motion_max_val",
    .help = "maximum value allowed; larger values will be clipped to this value",
    .alias = "mmxv",
    .offset = offsetof(MotionV2StateSycl, motion_max_val),
    .type = VMAF_OPT_TYPE_DOUBLE,
    .default_val = {.d = MOTION_V2_SYCL_DEFAULT_MAX_VAL},
    .min = 0.0,
    .max = 10000.0,
    .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
};

} // namespace

namespace
{

static const VmafOption motion_average_option = {
    .name = "motion_moving_average",
    .help = "smooth motion3 with a 2-frame moving average",
    .alias = "mma",
    .offset = offsetof(MotionV2StateSycl, motion_moving_average),
    .type = VMAF_OPT_TYPE_BOOL,
    .default_val = {.b = false},
    .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
};

static const VmafOption options_motion_v2_sycl[] = {
    motion_weight_option, motion_blend_factor_option, motion_blend_offset_option,
    motion_max_option,    motion_average_option,      {.name = nullptr},
};

} // namespace

namespace
{

static int allocate_motion_v2(MotionV2StateSycl *s)
{
    s->plane_bytes = (size_t)s->width * s->height * (s->bpc <= 8 ? 1u : 2u);
    s->h_pix = vmaf_sycl_malloc_host(s->sycl_state, s->plane_bytes);
    s->d_pix[0] = vmaf_sycl_malloc_device(s->sycl_state, s->plane_bytes);
    s->d_pix[1] = vmaf_sycl_malloc_device(s->sycl_state, s->plane_bytes);
    s->d_sad = static_cast<int64_t *>(vmaf_sycl_malloc_device(s->sycl_state, sizeof(int64_t)));
    s->h_sad = static_cast<int64_t *>(vmaf_sycl_malloc_host(s->sycl_state, sizeof(int64_t)));
    if (!s->h_pix || !s->d_pix[0] || !s->d_pix[1] || !s->d_sad || !s->h_sad) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "motion_v2_sycl: USM allocation failed\n");
        return -ENOMEM;
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
    auto *s = static_cast<MotionV2StateSycl *>(fex->priv);

    /* The 5-tap SYCL motion_v2 kernel uses reflect-101 mirror padding;
     * dev_mirror_mv2() returns 2*sup - idx - 2, which is negative when sup < 3.
     * Refuse smaller frames up front to prevent out-of-bounds device reads.
     * Minimum: filter_width/2 + 1 = 3. */
    if (h < 3u || w < 3u) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "motion_v2_sycl: frame %ux%u is below the 5-tap filter minimum 3x3; "
                 "refusing to avoid out-of-bounds mirror reads on device\n",
                 w, h);
        return -EINVAL;
    }

    s->width = w;
    s->height = h;
    s->bpc = bpc;
    s->frame_index = 0;
    s->has_pending = false;

    if (!fex->sycl_state) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "motion_v2_sycl: no SYCL state\n");
        return -EINVAL;
    }
    s->sycl_state = fex->sycl_state;
    const int alloc_err = allocate_motion_v2(s);
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
    auto *s = static_cast<MotionV2StateSycl *>(fex->priv);
    auto *qptr = static_cast<sycl::queue *>(vmaf_sycl_get_queue_ptr(s->sycl_state));
    if (!qptr) {
        return -EINVAL;
    }
    sycl::queue &q = *qptr;

    /* Pack cur ref Y into pinned host staging (handles arbitrary
     * pic stride), then upload to d_pix[index%2]. */
    if (s->bpc <= 8) {
        copy_y_plane<uint8_t>(ref_pic, s->h_pix, s->width, s->height);
    } else {
        copy_y_plane<uint16_t>(ref_pic, s->h_pix, s->width, s->height);
    }

    const unsigned cur_idx = index % 2u;
    q.memcpy(s->d_pix[cur_idx], s->h_pix, s->plane_bytes);

    if (index > 0) {
        const unsigned prev_idx = (index + 1u) % 2u;
        q.memset(s->d_sad, 0, sizeof(int64_t));
        launch_motion_v2(q, {.prev = s->d_pix[prev_idx],
                             .cur = s->d_pix[cur_idx],
                             .sad = s->d_sad,
                             .width = s->width,
                             .height = s->height,
                             .bpc = s->bpc});
        q.memcpy(s->h_sad, s->d_sad, sizeof(int64_t));
    }

    s->pending_index = index;
    s->has_pending = true;
    s->frame_index = index + 1u;
    return 0;
}

} // namespace

namespace
{

static int collect_fex_sycl(VmafFeatureExtractor *fex, unsigned index,
                            VmafFeatureCollector *feature_collector)
{
    auto *s = static_cast<MotionV2StateSycl *>(fex->priv);
    auto *qptr = static_cast<sycl::queue *>(vmaf_sycl_get_queue_ptr(s->sycl_state));
    if (!qptr) {
        return -EINVAL;
    }
    qptr->wait();

    if (index == 0) {
        return vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                       "VMAF_integer_feature_motion_v2_sad_score",
                                                       0.0, index);
    }

    const double sad_score = (double)*s->h_sad / 256.0 / ((double)s->width * (double)s->height);
    return vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "VMAF_integer_feature_motion_v2_sad_score",
                                                   sad_score, index);
}

} // namespace

namespace
{

static unsigned motion_frame_count(VmafFeatureCollector *collector, const char *sad_name)
{
    unsigned count = 0;
    double unused;
    while (!vmaf_feature_collector_get_score(collector, sad_name, &unused, count)) {
        count++;
    }
    return count;
}

static double motion_stamp_value(VmafFeatureCollector *collector, const MotionV2StateSycl *s,
                                 const char *sad_name, unsigned frame_count)
{
    constexpr unsigned min_index = 1;
    double value = 0.0;
    double sad;
    if (frame_count > min_index &&
        !vmaf_feature_collector_get_score(collector, sad_name, &sad, min_index)) {
        value = MIN(motion_blend(sad, s->motion_blend_factor, s->motion_blend_offset),
                    s->motion_max_val);
    }
    return value;
}

} // namespace

namespace
{

static int append_motion_frame(VmafFeatureCollector *collector, MotionV2StateSycl *s,
                               const char *sad_name, unsigned index, unsigned frame_count,
                               double stamp_value, double &previous)
{
    double score_current;
    vmaf_feature_collector_get_score(collector, sad_name, &score_current, index);
    score_current *= s->motion_fps_weight;
    double motion2 = score_current;
    if (index + 1 < frame_count) {
        double score_next;
        vmaf_feature_collector_get_score(collector, sad_name, &score_next, index + 1);
        score_next *= s->motion_fps_weight;
        motion2 = score_current < score_next ? score_current : score_next;
    }
    int err = vmaf_feature_collector_append_with_dict(
        collector, s->feature_name_dict, "VMAF_integer_feature_motion2_v2_score", motion2, index);
    if (err) {
        return err;
    }
    double motion3;
    if (index < 1) {
        motion3 = stamp_value;
        previous = stamp_value;
    } else {
        double const processed =
            MIN(motion_blend(motion2, s->motion_blend_factor, s->motion_blend_offset),
                s->motion_max_val);
        motion3 = s->motion_moving_average ? (processed + previous) / 2.0 : processed;
        previous = processed;
    }
    err = vmaf_feature_collector_append_with_dict(
        collector, s->feature_name_dict, "VMAF_integer_feature_motion3_v2_score", motion3, index);
    return err;
}

} // namespace

namespace
{

static int flush_fex_sycl(VmafFeatureExtractor *fex, VmafFeatureCollector *feature_collector)
{
    auto *s = static_cast<MotionV2StateSycl *>(fex->priv);

    /* Resolve the (possibly renamed, for sfr/hfr co-schedule) SAD feature
     * name from the dict — mirrors integer_motion_v2.c::flush and the CUDA
     * twin flush_fex_cuda. */
    VmafDictionaryEntry const *e_sad =
        vmaf_dictionary_get(&s->feature_name_dict, "VMAF_integer_feature_motion_v2_sad_score", 0);
    const char *sad_name = e_sad ? e_sad->val : "VMAF_integer_feature_motion_v2_sad_score";

    const unsigned n_frames = motion_frame_count(feature_collector, sad_name);
    if (n_frames < 2) {
        return 1;
    }
    const double stamp_value = motion_stamp_value(feature_collector, s, sad_name, n_frames);
    double prev_processed = 0.;
    for (unsigned i = 0; i < n_frames; i++) {
        const int err = append_motion_frame(feature_collector, s, sad_name, i, n_frames,
                                            stamp_value, prev_processed);
        if (err) {
            return err;
        }
    }

    return 1;
}

} // namespace

namespace
{

static int close_fex_sycl(VmafFeatureExtractor *fex)
{
    auto *s = static_cast<MotionV2StateSycl *>(fex->priv);
    if (s->sycl_state) {
        if (s->h_pix)
            vmaf_sycl_free(s->sycl_state, s->h_pix);
        if (s->d_pix[0])
            vmaf_sycl_free(s->sycl_state, s->d_pix[0]);
        if (s->d_pix[1])
            vmaf_sycl_free(s->sycl_state, s->d_pix[1]);
        if (s->d_sad)
            vmaf_sycl_free(s->sycl_state, s->d_sad);
        if (s->h_sad)
            vmaf_sycl_free(s->sycl_state, s->h_sad);
    }
    if (s->feature_name_dict)
        vmaf_dictionary_free(&s->feature_name_dict);
    return 0;
}

static const char *provided_features_motion_v2_sycl[] = {
    "VMAF_integer_feature_motion_v2_sad_score", "VMAF_integer_feature_motion2_v2_score",
    "VMAF_integer_feature_motion3_v2_score", nullptr};

} // namespace

extern "C" VmafFeatureExtractor vmaf_fex_integer_motion_v2_sycl = {
    .name = "motion_v2_sycl",
    .init = init_fex_sycl,
    .extract = nullptr,
    .flush = flush_fex_sycl,
    .close = close_fex_sycl,
    .submit = submit_fex_sycl,
    .collect = collect_fex_sycl,
    .options = options_motion_v2_sycl,
    .priv_size = sizeof(MotionV2StateSycl),
    .flags = VMAF_FEATURE_EXTRACTOR_TEMPORAL | VMAF_FEATURE_EXTRACTOR_SYCL,
    .provided_features = provided_features_motion_v2_sycl,
};
