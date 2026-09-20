/**
 *
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *
 *     Licensed under the BSD+Patent License (the "License");
 *     you may not use this file except in compliance with the License.
 *     You may obtain a copy of the License at
 *
 *         https://opensource.org/licenses/BSDplusPatent
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 *
 */

/**
 * SYCL/DPC++ Motion feature extractor.
 *
 * Implements separable 5-tap Gaussian blur + SAD (Sum of Absolute
 * Differences) between consecutive blurred reference frames.
 *
 * Algorithm:
 *   1. Blur the current reference frame with a separable 5-tap Gaussian
 *      kernel: {3571, 16004, 26386, 16004, 3571} (sum = 65536).
 *   2. Compute SAD between current blurred frame and previous blurred frame.
 *   3. motion_score = SAD / 256.0 / (width * height)
 *   4. motion2_score = min(prev_motion_score, cur_motion_score)
 *
 * Uses ping-pong buffers for blurred frames across temporal frames.
 *
 * Pattern: init -> submit (non-blocking) -> collect (wait + scores)
 * TEMPORAL flag: frames must be processed in sequential order.
 */

#include <sycl/sycl.hpp>

#include "sycl_compat.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <utility>

#include "config.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "motion_blend_tools.h"
#include "sycl/common.h"
#include "log.h"

/* Default upper clamp on motion / motion2 / motion3 — mirrors
 * DEFAULT_MOTION_MAX_VAL in libvmaf/src/feature/integer_motion.c.
 * T3-15(c) / ADR-0219. */
static constexpr double MOTION_SYCL_DEFAULT_MAX_VAL = 10000.0;

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

// 5-tap Gaussian filter: {3571, 16004, 26386, 16004, 3571}, sum = 65536
static constexpr int32_t blur_filter[5] = {3571, 16004, 26386, 16004, 3571};

/* ------------------------------------------------------------------ */
/* Extractor private state                                             */
/* ------------------------------------------------------------------ */

struct MotionStateSycl {
    unsigned width, height;
    unsigned bpc;

    bool debug;
    bool motion_force_zero;
    bool motion_five_frame_window; // rejected with -ENOTSUP — see init()
    bool motion_moving_average;
    bool motion_add_uv; // include U + V plane SAD — ADR-0989
    double motion_blend_factor;
    double motion_blend_offset;
    double motion_fps_weight;
    double motion_max_val;

    VmafDictionary *feature_name_dict;

    // Frame tracking
    unsigned frame_index;
    double prev_motion_score;
    // motion3 post-processing state — last *unaveraged* blended
    // score, used by the moving-average rule. Mirrors CPU
    // MotionState.previous_score. T3-15(c) / ADR-0219.
    double prev_motion3_blended;

    // Ping-pong blur buffers for Y plane (device)
    int32_t *d_blur[2]; // alternating current / previous
    int cur_blur;       // index into d_blur for current frame

    // Vertical intermediate buffer (Y only; UV uses same kernel in-kernel)
    int32_t *d_blur_tmp; // vertical pass output

    // Y-plane SAD accumulator (device + host)
    int64_t *d_sad_accum;
    int64_t *h_sad_accum;

    // UV plane support — ADR-0989.
    // When motion_add_uv=true, two additional ping-pong pairs hold the
    // raw input U/V pixels (copied H2D in submit) and the blurred U/V
    // pixels (written by the kernel).  Separate SAD accumulators allow
    // per-plane normalization on the host (UV planes are smaller than Y
    // for YUV420P).
    unsigned chroma_w, chroma_h; // U/V plane dimensions
    void *d_ref_u[2];            // raw U-plane ping-pong (device, void* for bpc agnostic)
    void *d_ref_v[2];            // raw V-plane ping-pong (device)
    int32_t *d_blur_u[2];        // blurred U ping-pong (device, int32)
    int32_t *d_blur_v[2];        // blurred V ping-pong (device, int32)
    int64_t *d_sad_u;            // U-plane SAD accumulator (device)
    int64_t *h_sad_u;            // U-plane SAD readback (host-mapped)
    int64_t *d_sad_v;            // V-plane SAD accumulator (device)
    int64_t *h_sad_v;            // V-plane SAD readback (host-mapped)

    // Deferred state
    unsigned pending_index;
    bool has_pending;

    // Back-pointer for graph-mode checks
    VmafSyclState *sycl_state;
};

/* Options table — mirrors libvmaf/src/feature/integer_motion.c.
 * The motion3-related post-processing options drive a host-side
 * derivation from motion2 (see collect()). T3-15(c) / ADR-0219. */
static const VmafOption option_debug = {
    .name = "debug",
    .help = "debug mode: enable additional output",
    .offset = offsetof(MotionStateSycl, debug),
    .type = VMAF_OPT_TYPE_BOOL,
    .default_val = {.b = true},
};

static const VmafOption option_force_zero = {
    .name = "motion_force_zero",
    .help = "force motion score to zero",
    .offset = offsetof(MotionStateSycl, motion_force_zero),
    .type = VMAF_OPT_TYPE_BOOL,
    .default_val = {.b = false},
    .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
};

static const VmafOption option_blend_factor = {
    .name = "motion_blend_factor",
    .help = "blend motion score given an offset",
    .alias = "mbf",
    .offset = offsetof(MotionStateSycl, motion_blend_factor),
    .type = VMAF_OPT_TYPE_DOUBLE,
    .default_val = {.d = 1.0},
    .min = 0.0,
    .max = 1.0,
    .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
};

static const VmafOption option_blend_offset = {
    .name = "motion_blend_offset",
    .help = "blend motion score starting from this offset",
    .alias = "mbo",
    .offset = offsetof(MotionStateSycl, motion_blend_offset),
    .type = VMAF_OPT_TYPE_DOUBLE,
    .default_val = {.d = 40.0},
    .min = 0.0,
    .max = 1000.0,
    .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
};

static const VmafOption option_fps_weight = {
    .name = "motion_fps_weight",
    .help = "fps-aware multiplicative weight/correction",
    .alias = "mfw",
    .offset = offsetof(MotionStateSycl, motion_fps_weight),
    .type = VMAF_OPT_TYPE_DOUBLE,
    .default_val = {.d = 1.0},
    .min = 0.0,
    .max = 5.0,
    .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
};

static const VmafOption option_max_val = {
    .name = "motion_max_val",
    .help = "maximum value allowed; larger values will be clipped to this value",
    .alias = "mmxv",
    .offset = offsetof(MotionStateSycl, motion_max_val),
    .type = VMAF_OPT_TYPE_DOUBLE,
    .default_val = {.d = MOTION_SYCL_DEFAULT_MAX_VAL},
    .min = 0.0,
    .max = 10000.0,
    .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
};

static const VmafOption option_five_frame_window = {
    .name = "motion_five_frame_window",
    .help = "use five-frame temporal window (NOT YET SUPPORTED on SYCL — T3-15(c) deferred)",
    .alias = "mffw",
    .offset = offsetof(MotionStateSycl, motion_five_frame_window),
    .type = VMAF_OPT_TYPE_BOOL,
    .default_val = {.b = false},
    .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
};

static const VmafOption option_moving_average = {
    .name = "motion_moving_average",
    .help = "use moving average for motion3 scores after first frame",
    .alias = "mma",
    .offset = offsetof(MotionStateSycl, motion_moving_average),
    .type = VMAF_OPT_TYPE_BOOL,
    .default_val = {.b = false},
    .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
};

static const VmafOption option_add_uv = {
    .name = "motion_add_uv",
    .help = "include U and V plane SADs in the motion score (ADR-0989)",
    .alias = "mau",
    .offset = offsetof(MotionStateSycl, motion_add_uv),
    .type = VMAF_OPT_TYPE_BOOL,
    .default_val = {.b = false},
    .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
};

static const VmafOption options[] = {
    option_debug,      option_force_zero, option_blend_factor,      option_blend_offset,
    option_fps_weight, option_max_val,    option_five_frame_window, option_moving_average,
    option_add_uv,     {.name = nullptr},
};

/* ------------------------------------------------------------------ */
/* Device helpers                                                      */
/* ------------------------------------------------------------------ */

/* Skip-boundary mirror matching CPU integer_motion's edge_8 / edge_16:
 *   idx=-1    -> 1     (skip row 0 in the reflection)
 *   idx=-2    -> 2
 *   idx=sup   -> sup-2  (skip row sup-1; `-2` below, not `-1`, enforces
 *                        the skip semantics)
 *   idx=sup+1 -> sup-3
 * Previously used `2 * sup - idx - 1` which reflected idx=sup back to
 * sup-1 (repeated the boundary row), producing a systematic ~2.6e-3
 * motion drift vs CPU on every frame after the first. Same root cause
 * as the CUDA mirror() bug also fixed in PR #120 (T7-15). */
static inline int dev_mirror_motion(int idx, int sup)
{
    if (idx < 0)
        return -idx;
    if (idx >= sup)
        return 2 * sup - idx - 2;
    return idx;
}

/* ------------------------------------------------------------------ */
/* SYCL Kernel: Fused 2D Gaussian Blur + SAD (single dispatch)        */
/*                                                                     */
/* Eliminates the intermediate vertical output buffer by computing     */
/* the full separable filter from a 2D SLM tile in registers:          */
/*   Step A: vertical filter at 5 horizontal positions → vtmp[5]       */
/*   Step B: horizontal filter on vtmp[5] → final blurred pixel        */
/* This matches the V→H ordering for exact bit-identical results.      */
/* ------------------------------------------------------------------ */

static constexpr int MOTION_WG_X = 32;
static constexpr int MOTION_WG_Y = 8;
static constexpr int MOTION_HALF_FW = 2;
static constexpr int MOTION_TILE_H = MOTION_WG_Y + 2 * MOTION_HALF_FW;
static constexpr int MOTION_TILE_W = MOTION_WG_X + 2 * MOTION_HALF_FW;
static constexpr int MOTION_WG_SIZE = MOTION_WG_X * MOTION_WG_Y;
static constexpr int MOTION_MAX_SUBGROUPS = 32;

struct MotionBlurKernelParams {
    const void *input;
    int32_t *blur_out;
    const int32_t *prev_blur;
    int64_t *sad_accum;
    unsigned width;
    unsigned height;
    unsigned bpc;
    bool compute_sad;
};

static inline int32_t motion_read_sample(const MotionBlurKernelParams &p, int y, int x)
{
    if (p.bpc <= 8)
        return static_cast<const uint8_t *>(p.input)[y * p.width + x];
    return static_cast<const uint16_t *>(p.input)[y * p.width + x];
}

template <typename Tile>
static inline void motion_load_tile(const MotionBlurKernelParams &p, sycl::nd_item<2> item,
                                    const Tile &tile)
{
    unsigned const lid = item.get_local_linear_id();
    int const origin_y = (int)(item.get_group(0) * MOTION_WG_Y) - MOTION_HALF_FW;
    int const origin_x = (int)(item.get_group(1) * MOTION_WG_X) - MOTION_HALF_FW;
    bool const interior = origin_y >= 0 && origin_y + MOTION_TILE_H <= (int)p.height &&
                          origin_x >= 0 && origin_x + MOTION_TILE_W <= (int)p.width;
    constexpr unsigned tile_elems = MOTION_TILE_H * MOTION_TILE_W;
    for (unsigned i = lid; i < tile_elems; i += MOTION_WG_SIZE) {
        unsigned const row = i / MOTION_TILE_W;
        unsigned const col = i % MOTION_TILE_W;
        int y = origin_y + (int)row;
        int x = origin_x + (int)col;
        if (!interior) {
            y = dev_mirror_motion(y, (int)p.height);
            x = dev_mirror_motion(x, (int)p.width);
        }
        tile[row][col] = motion_read_sample(p, y, x);
    }
}

template <typename Tile>
static inline int64_t motion_blur_pixel(const MotionBlurKernelParams &p, sycl::nd_item<2> item,
                                        const Tile &tile)
{
    int const x = item.get_global_id(1);
    int const y = item.get_global_id(0);
    if (std::cmp_greater_equal(x, p.width) || std::cmp_greater_equal(y, p.height))
        return 0;
    unsigned const lx = item.get_local_id(1);
    unsigned const ly = item.get_local_id(0);
    int32_t const round1 = 1 << (p.bpc - 1);
    int32_t vertical[5];
#pragma unroll
    for (int tap = 0; tap < 5; tap++) {
        unsigned const col = lx + (unsigned)tap;
        int32_t const sum = blur_filter[0] * (tile[ly][col] + tile[ly + 4][col]) +
                            blur_filter[1] * (tile[ly + 1][col] + tile[ly + 3][col]) +
                            blur_filter[2] * tile[ly + 2][col];
        vertical[tap] = (sum + round1) >> p.bpc;
    }
    int64_t const horizontal = (int64_t)blur_filter[0] * (vertical[0] + vertical[4]) +
                               (int64_t)blur_filter[1] * (vertical[1] + vertical[3]) +
                               (int64_t)blur_filter[2] * vertical[2];
    int32_t const blurred = (int32_t)((horizontal + 32768) >> 16);
    p.blur_out[y * p.width + x] = blurred;
    if (!p.compute_sad)
        return 0;
    int32_t const diff = blurred - p.prev_blur[y * p.width + x];
    return (diff < 0) ? -(int64_t)diff : (int64_t)diff;
}

template <typename LocalMemory>
static inline void motion_reduce_sad(const MotionBlurKernelParams &p, sycl::nd_item<2> item,
                                     const LocalMemory &lmem, int64_t abs_diff)
{
    sycl::sub_group const subgroup = item.get_sub_group();
    int64_t const subgroup_sum = sycl::reduce_over_group(subgroup, abs_diff, sycl::plus<int64_t>{});
    uint32_t const subgroup_id = subgroup.get_group_linear_id();
    uint32_t const subgroup_lane = subgroup.get_local_linear_id();
    uint32_t const subgroup_count = subgroup.get_group_linear_range();
    if (subgroup_lane == 0)
        lmem[subgroup_id] = subgroup_sum;
    item.barrier(sycl::access::fence_space::local_space);
    if (item.get_local_linear_id() != 0)
        return;
    int64_t total = 0;
    for (uint32_t subgroup_idx = 0; subgroup_idx < subgroup_count; subgroup_idx++)
        total += lmem[subgroup_idx];
    sycl::atomic_ref<int64_t, sycl::memory_order::relaxed, sycl::memory_scope::device,
                     sycl::access::address_space::global_space> const accumulator(*p.sad_accum);
    accumulator.fetch_add(total);
}

static sycl::event launch_blur_sad_fused(sycl::queue &q, const void *input, int32_t *blur_out,
                                         const int32_t *prev_blur, int64_t *sad_accum,
                                         unsigned width, unsigned height, unsigned bpc,
                                         bool compute_sad)
{
    MotionBlurKernelParams const p = {input, blur_out, prev_blur, sad_accum,
                                      width, height,   bpc,       compute_sad};
    sycl::range<2> const global((size_t)((height + MOTION_WG_Y - 1) / MOTION_WG_Y) * MOTION_WG_Y,
                                (size_t)((width + MOTION_WG_X - 1) / MOTION_WG_X) * MOTION_WG_X);
    sycl::range<2> const local(MOTION_WG_Y, MOTION_WG_X);
    return q.submit([&](sycl::handler &cgh) {
        sycl::local_accessor<int32_t, 2> const tile(sycl::range<2>(MOTION_TILE_H, MOTION_TILE_W),
                                                    cgh);
        sycl::local_accessor<int64_t, 1> const lmem(sycl::range<1>(MOTION_MAX_SUBGROUPS), cgh);
        cgh.parallel_for(sycl::nd_range<2>(global, local),
                         [=](sycl::nd_item<2> item) VMAF_SYCL_REQD_SG_SIZE(32) {
                             motion_load_tile(p, item, tile);
                             item.barrier(sycl::access::fence_space::local_space);
                             int64_t const abs_diff = motion_blur_pixel(p, item, tile);
                             if (p.compute_sad)
                                 motion_reduce_sad(p, item, lmem, abs_diff);
                         });
    });
}

/* ------------------------------------------------------------------ */
/* Feature extractor callbacks                                         */
/* ------------------------------------------------------------------ */

// Forward declarations for combined graph callbacks (defined after init)
static void enqueue_motion_work(void *queue_ptr, void *priv, void *shared_ref, void *shared_dis);
static void motion_pre_graph(void *queue_ptr, void *priv);
static void motion_post_graph(void *queue_ptr, void *priv);
static void config_motion_slot(void *priv, int slot);
static int close_fex_sycl(VmafFeatureExtractor *fex); /* forward decl for init error paths */

static int motion_validate_init(VmafFeatureExtractor *fex, unsigned bpc, unsigned width,
                                unsigned height)
{
    auto *s = static_cast<MotionStateSycl *>(fex->priv);
    if (s->motion_five_frame_window) {
        vmaf_log(VMAF_LOG_LEVEL_WARNING,
                 "motion_sycl: motion_five_frame_window=true is not yet supported on SYCL "
                 "(T3-15(c) deferred). Use the CPU extractor `motion` instead.\n");
        return -ENOTSUP;
    }
    if (height < 3u || width < 3u) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "motion_sycl: frame %ux%u is below the 5-tap filter minimum 3x3; "
                 "refusing to avoid out-of-bounds mirror reads on device\n",
                 width, height);
        return -EINVAL;
    }
    if (!fex->sycl_state) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "motion_sycl: no SYCL state\n");
        return -EINVAL;
    }
    s->width = width;
    s->height = height;
    s->bpc = bpc;
    s->frame_index = 0;
    s->prev_motion_score = 0.0;
    s->prev_motion3_blended = 0.0;
    s->has_pending = false;
    s->cur_blur = 0;
    return 0;
}

static int motion_alloc_luma(VmafFeatureExtractor *fex, MotionStateSycl *s, VmafSyclState *state)
{
    size_t const buf_size = (size_t)s->width * s->height * sizeof(int32_t);
    s->d_blur[0] = static_cast<int32_t *>(vmaf_sycl_malloc_device(state, buf_size));
    s->d_blur[1] = static_cast<int32_t *>(vmaf_sycl_malloc_device(state, buf_size));
    s->d_blur_tmp = static_cast<int32_t *>(vmaf_sycl_malloc_device(state, buf_size));
    s->d_sad_accum = static_cast<int64_t *>(vmaf_sycl_malloc_device(state, sizeof(int64_t)));
    s->h_sad_accum = static_cast<int64_t *>(vmaf_sycl_malloc_host(state, sizeof(int64_t)));
    if (!s->d_blur[0] || !s->d_blur[1] || !s->d_blur_tmp || !s->d_sad_accum || !s->h_sad_accum) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "motion_sycl: device memory allocation failed\n");
        close_fex_sycl(fex);
        return -ENOMEM;
    }
    return 0;
}

static void motion_reset_uv(MotionStateSycl *s)
{
    s->d_ref_u[0] = nullptr;
    s->d_ref_u[1] = nullptr;
    s->d_ref_v[0] = nullptr;
    s->d_ref_v[1] = nullptr;
    s->d_blur_u[0] = nullptr;
    s->d_blur_u[1] = nullptr;
    s->d_blur_v[0] = nullptr;
    s->d_blur_v[1] = nullptr;
    s->d_sad_u = nullptr;
    s->h_sad_u = nullptr;
    s->d_sad_v = nullptr;
    s->h_sad_v = nullptr;
    s->chroma_w = 0;
    s->chroma_h = 0;
}

static int motion_alloc_uv(VmafFeatureExtractor *fex, MotionStateSycl *s, VmafSyclState *state,
                           enum VmafPixelFormat pix_fmt)
{
    if (!s->motion_add_uv)
        return 0;
    if (pix_fmt == VMAF_PIX_FMT_YUV400P || pix_fmt == VMAF_PIX_FMT_UNKNOWN) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "motion_sycl: motion_add_uv=true requires a YUV format with chroma "
                 "planes (got %d)\n",
                 (int)pix_fmt);
        close_fex_sycl(fex);
        return -EINVAL;
    }
    s->chroma_w = (s->width + 1U) >> 1U;
    s->chroma_h = (s->height + 1U) >> 1U;
    size_t const bpp = (s->bpc <= 8) ? 1U : 2U;
    size_t const raw_size = (size_t)s->chroma_w * s->chroma_h * bpp;
    size_t const blur_size = (size_t)s->chroma_w * s->chroma_h * sizeof(int32_t);
    s->d_ref_u[0] = vmaf_sycl_malloc_device(state, raw_size);
    s->d_ref_u[1] = vmaf_sycl_malloc_device(state, raw_size);
    s->d_ref_v[0] = vmaf_sycl_malloc_device(state, raw_size);
    s->d_ref_v[1] = vmaf_sycl_malloc_device(state, raw_size);
    s->d_blur_u[0] = static_cast<int32_t *>(vmaf_sycl_malloc_device(state, blur_size));
    s->d_blur_u[1] = static_cast<int32_t *>(vmaf_sycl_malloc_device(state, blur_size));
    s->d_blur_v[0] = static_cast<int32_t *>(vmaf_sycl_malloc_device(state, blur_size));
    s->d_blur_v[1] = static_cast<int32_t *>(vmaf_sycl_malloc_device(state, blur_size));
    s->d_sad_u = static_cast<int64_t *>(vmaf_sycl_malloc_device(state, sizeof(int64_t)));
    s->h_sad_u = static_cast<int64_t *>(vmaf_sycl_malloc_host(state, sizeof(int64_t)));
    s->d_sad_v = static_cast<int64_t *>(vmaf_sycl_malloc_device(state, sizeof(int64_t)));
    s->h_sad_v = static_cast<int64_t *>(vmaf_sycl_malloc_host(state, sizeof(int64_t)));
    bool const failed = !s->d_ref_u[0] || !s->d_ref_u[1] || !s->d_ref_v[0] || !s->d_ref_v[1] ||
                        !s->d_blur_u[0] || !s->d_blur_u[1] || !s->d_blur_v[0] || !s->d_blur_v[1] ||
                        !s->d_sad_u || !s->h_sad_u || !s->d_sad_v || !s->h_sad_v;
    if (!failed)
        return 0;
    vmaf_log(VMAF_LOG_LEVEL_ERROR, "motion_sycl: UV device memory allocation failed\n");
    close_fex_sycl(fex);
    return -ENOMEM;
}

static int init_fex_sycl(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                         unsigned w, unsigned h)
{
    int err = motion_validate_init(fex, bpc, w, h);
    if (err)
        return err;
    auto *s = static_cast<MotionStateSycl *>(fex->priv);
    VmafSyclState *state = fex->sycl_state;
    err = vmaf_sycl_shared_frame_init(state, w, h, bpc);
    if (err)
        return err;
    err = motion_alloc_luma(fex, s, state);
    if (err)
        return err;
    motion_reset_uv(s);
    err = motion_alloc_uv(fex, s, state, pix_fmt);
    if (err)
        return err;
    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (!s->feature_name_dict) {
        close_fex_sycl(fex);
        return -ENOMEM;
    }

    s->sycl_state = state;
    int const err2 = vmaf_sycl_graph_register(state, enqueue_motion_work, motion_pre_graph,
                                              motion_post_graph, config_motion_slot, s, "MOTION");
    if (err2) {
        close_fex_sycl(fex);
        return err2;
    }

    return 0;
}

static int extract_force_zero(VmafFeatureExtractor *fex, const VmafPicture *ref,
                              const VmafPicture *ref_90, const VmafPicture *dist,
                              const VmafPicture *dist_90, unsigned index,
                              VmafFeatureCollector *feature_collector)
{
    (void)ref;
    (void)ref_90;
    (void)dist;
    (void)dist_90;
    auto *s = static_cast<MotionStateSycl *>(fex->priv);

    int err = 0;
    if (s->frame_index > 0) {
        err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                       "VMAF_integer_feature_motion_score", 0.0,
                                                       index);
    }
    err |= vmaf_feature_collector_append_with_dict(
        feature_collector, s->feature_name_dict, "VMAF_integer_feature_motion2_score", 0.0, index);
    err |= vmaf_feature_collector_append_with_dict(
        feature_collector, s->feature_name_dict, "VMAF_integer_feature_motion3_score", 0.0, index);

    s->frame_index++;
    return err;
}

/* ------------------------------------------------------------------ */
/* motion3 post-processing — pure host-side scalar work.              */
/*                                                                     */
/* Mirrors libvmaf/src/feature/integer_motion.c lines 510-560 and    */
/* the Vulkan + CUDA twins. T3-15(c) / ADR-0219.                     */
/* ------------------------------------------------------------------ */
static double motion3_postprocess_sycl(MotionStateSycl *s, double score2)
{
    /* ``score2`` already carries ``motion_fps_weight`` and the
     * ``motion_max_val`` clip: every caller applies both before handing the
     * value over, exactly as the CPU reference does once in extract()
     * (integer_motion.c:372).  Re-weighting here would square the factor
     * whenever ``motion_fps_weight != 1.0``.  ADR-1216. */
    double const blended = motion_blend(score2, s->motion_blend_factor, s->motion_blend_offset);
    double const clipped = MIN(blended, s->motion_max_val);
    double const previous_unaveraged = s->prev_motion3_blended;
    s->prev_motion3_blended = clipped;
    if (s->motion_moving_average && s->frame_index > 1) {
        return (clipped + previous_unaveraged) / 2.0;
    }
    return clipped;
}

/* ------------------------------------------------------------------ */
/* C-compatible callbacks for combined command graph                    */
/* ------------------------------------------------------------------ */

// Pre-graph: zero SAD accumulators (direct enqueue, outside graph)
static void motion_pre_graph(void *queue_ptr, void *priv)
{
    sycl::queue &q = *static_cast<sycl::queue *>(queue_ptr);
    auto *s = static_cast<MotionStateSycl *>(priv);
    if (s->frame_index > 0) {
        q.memset(s->d_sad_accum, 0, sizeof(int64_t));
        if (s->motion_add_uv) {
            q.memset(s->d_sad_u, 0, sizeof(int64_t));
            q.memset(s->d_sad_v, 0, sizeof(int64_t));
        }
    }
}

// Graph-recorded: compute kernels only (Y plane + optional UV planes)
static void enqueue_motion_work(void *queue_ptr, void *priv, void *shared_ref, void *shared_dis)
{
    (void)shared_dis; // Motion only uses ref
    sycl::queue &q = *static_cast<sycl::queue *>(queue_ptr);
    auto *s = static_cast<MotionStateSycl *>(priv);

    bool const compute_sad = (s->frame_index > 0);
    int const cur = s->cur_blur;
    int const prev = 1 - cur;

    // Y-plane kernel (always)
    launch_blur_sad_fused(q, shared_ref, s->d_blur[cur], s->d_blur[prev], s->d_sad_accum, s->width,
                          s->height, s->bpc, compute_sad);

    // UV-plane kernels — ADR-0989.
    // d_ref_u[cur] / d_ref_v[cur] were uploaded H2D in submit_fex_sycl
    // before the graph fires; the pointer values are stable across graph
    // replays (ping-pong captured per slot via config_fn).
    if (s->motion_add_uv) {
        launch_blur_sad_fused(q, s->d_ref_u[cur], s->d_blur_u[cur], s->d_blur_u[prev], s->d_sad_u,
                              s->chroma_w, s->chroma_h, s->bpc, compute_sad);
        launch_blur_sad_fused(q, s->d_ref_v[cur], s->d_blur_v[cur], s->d_blur_v[prev], s->d_sad_v,
                              s->chroma_w, s->chroma_h, s->bpc, compute_sad);
    }
}

// Post-graph: D2H SAD accumulator download (direct enqueue, outside graph)
// With two graph slots (config_fn sets cur_blur=slot), each slot writes
// to d_blur[slot] and reads from d_blur[1-slot].  The natural ping-pong
// alternation keeps the "previous" buffer correct — no copy needed.
static void motion_post_graph(void *queue_ptr, void *priv)
{
    sycl::queue &q = *static_cast<sycl::queue *>(queue_ptr);
    auto *s = static_cast<MotionStateSycl *>(priv);
    if (s->frame_index > 0) {
        q.memcpy(s->h_sad_accum, s->d_sad_accum, sizeof(int64_t));
        if (s->motion_add_uv) {
            q.memcpy(s->h_sad_u, s->d_sad_u, sizeof(int64_t));
            q.memcpy(s->h_sad_v, s->d_sad_v, sizeof(int64_t));
        }
    }
}

static void config_motion_slot(void *priv, int slot)
{
    auto *s = static_cast<MotionStateSycl *>(priv);
    s->cur_blur = slot;
}

/* ------------------------------------------------------------------ */
/* Submit / Collect                                                    */
/* ------------------------------------------------------------------ */

static int motion_upload_uv_rows(VmafSyclState *state, const MotionStateSycl *s,
                                 const VmafPicture *ref_pic, uint8_t *u_dst, uint8_t *v_dst,
                                 size_t row_bytes)
{
    const uint8_t *u_src = static_cast<const uint8_t *>(ref_pic->data[1]);
    const uint8_t *v_src = static_cast<const uint8_t *>(ref_pic->data[2]);
    if (std::cmp_equal(ref_pic->stride[1], row_bytes)) {
        size_t const bytes = (size_t)s->chroma_h * row_bytes;
        int const u_err = vmaf_sycl_memcpy_h2d_async(state, u_dst, u_src, bytes);
        int const v_err = vmaf_sycl_memcpy_h2d_async(state, v_dst, v_src, bytes);
        return u_err ? u_err : v_err;
    }
    for (unsigned row = 0; row < s->chroma_h; row++) {
        int const u_err = vmaf_sycl_memcpy_h2d_async(state, u_dst + row * row_bytes,
                                                     u_src + row * ref_pic->stride[1], row_bytes);
        int const v_err = vmaf_sycl_memcpy_h2d_async(state, v_dst + row * row_bytes,
                                                     v_src + row * ref_pic->stride[2], row_bytes);
        if (u_err || v_err)
            return u_err ? u_err : v_err;
    }
    return 0;
}

static int motion_upload_uv(VmafSyclState *state, const MotionStateSycl *s,
                            const VmafPicture *ref_pic)
{
    if (!s->motion_add_uv || ref_pic == nullptr)
        return 0;
    int const slot = s->cur_blur;
    size_t const bytes_per_pixel = (s->bpc <= 8) ? 1U : 2U;
    size_t const row_bytes = s->chroma_w * bytes_per_pixel;
    auto *u_dst = static_cast<uint8_t *>(s->d_ref_u[slot]);
    auto *v_dst = static_cast<uint8_t *>(s->d_ref_v[slot]);
    int const err = motion_upload_uv_rows(state, s, ref_pic, u_dst, v_dst, row_bytes);
    if (err)
        return err;
    return vmaf_sycl_queue_wait(state);
}

static int submit_fex_sycl(VmafFeatureExtractor *fex, const VmafPicture *ref_pic, const VmafPicture *ref_pic_90,
                           const VmafPicture *dist_pic, const VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic_90;
    (void)dist_pic;
    (void)dist_pic_90;

    auto *s = static_cast<MotionStateSycl *>(fex->priv);
    VmafSyclState *state = fex->sycl_state;
    int err = motion_upload_uv(state, s, ref_pic);
    if (err)
        return err;
    err = vmaf_sycl_graph_submit(state);
    if (err)
        return err;

    s->pending_index = index;
    s->has_pending = true;

    return 0;
}

static double motion_score_from_accumulators(const MotionStateSycl *s)
{
    double motion_score = 0.0;
    if (s->frame_index > 0) {
        int64_t const sad_y = *s->h_sad_accum;
        motion_score = (double)sad_y / 256.0 / ((double)s->width * s->height);
        if (s->motion_add_uv) {
            int64_t const sad_u = *s->h_sad_u;
            int64_t const sad_v = *s->h_sad_v;
            double const uv_area = (double)s->chroma_w * s->chroma_h;
            motion_score += (double)sad_u / 256.0 / uv_area;
            motion_score += (double)sad_v / 256.0 / uv_area;
        }
    }
    return motion_score;
}

static int motion_collect_first(const MotionStateSycl *s, unsigned index,
                                VmafFeatureCollector *collector)
{
    int err = 0;
    err |= vmaf_feature_collector_append_with_dict(
        collector, s->feature_name_dict, "VMAF_integer_feature_motion2_score", 0.0, index);
    if (s->debug) {
        err |= vmaf_feature_collector_append_with_dict(
            collector, s->feature_name_dict, "VMAF_integer_feature_motion_score", 0.0, index);
    }
    return err;
}

static int motion_collect_second(MotionStateSycl *s, double motion_score, unsigned index,
                                 VmafFeatureCollector *collector)
{
    double const clipped = MIN(motion_score * s->motion_fps_weight, s->motion_max_val);
    double const motion3 = motion3_postprocess_sycl(s, clipped);
    int err = vmaf_feature_collector_append_with_dict(
        collector, s->feature_name_dict, "VMAF_integer_feature_motion3_score", motion3, index - 1);
    if (s->debug) {
        err |= vmaf_feature_collector_append_with_dict(
            collector, s->feature_name_dict, "VMAF_integer_feature_motion_score", clipped, index);
    }
    return err;
}

static int motion_collect_later(MotionStateSycl *s, double motion_score, unsigned index,
                                VmafFeatureCollector *collector)
{
    double const motion2 =
        (motion_score < s->prev_motion_score) ? motion_score : s->prev_motion_score;
    double const clipped = MIN(motion2 * s->motion_fps_weight, s->motion_max_val);
    int err = vmaf_feature_collector_append_with_dict(
        collector, s->feature_name_dict, "VMAF_integer_feature_motion2_score", clipped, index - 1);
    double const motion3 = motion3_postprocess_sycl(s, clipped);
    err |= vmaf_feature_collector_append_with_dict(
        collector, s->feature_name_dict, "VMAF_integer_feature_motion3_score", motion3, index - 1);
    if (s->debug) {
        double const raw_clipped = MIN(motion_score * s->motion_fps_weight, s->motion_max_val);
        err |= vmaf_feature_collector_append_with_dict(collector, s->feature_name_dict,
                                                       "VMAF_integer_feature_motion_score",
                                                       raw_clipped, index);
    }
    return err;
}

static int collect_fex_sycl(VmafFeatureExtractor *fex, unsigned index,
                            VmafFeatureCollector *feature_collector)
{
    auto *s = static_cast<MotionStateSycl *>(fex->priv);
    vmaf_sycl_graph_wait(fex->sycl_state);
    double const motion_score = motion_score_from_accumulators(s);
    int err;
    if (s->frame_index == 0)
        err = motion_collect_first(s, index, feature_collector);
    else if (s->frame_index == 1)
        err = motion_collect_second(s, motion_score, index, feature_collector);
    else
        err = motion_collect_later(s, motion_score, index, feature_collector);
    s->prev_motion_score = motion_score;
    s->cur_blur = 1 - s->cur_blur;
    s->frame_index++;
    s->has_pending = false;
    return err;
}

static int extract_fex_sycl(VmafFeatureExtractor *fex, const VmafPicture *ref_pic,
                            const VmafPicture *ref_pic_90, const VmafPicture *dist_pic,
                            const VmafPicture *dist_pic_90, unsigned index,
                            VmafFeatureCollector *feature_collector)
{
    auto *s = static_cast<MotionStateSycl *>(fex->priv);

    if (s->motion_force_zero) {
        return extract_force_zero(fex, ref_pic, ref_pic_90, dist_pic, dist_pic_90, index,
                                  feature_collector);
    }

    int const err = submit_fex_sycl(fex, ref_pic, ref_pic_90, dist_pic, dist_pic_90, index);
    if (err)
        return err;
    return collect_fex_sycl(fex, index, feature_collector);
}

static int flush_fex_sycl(VmafFeatureExtractor *fex, VmafFeatureCollector *feature_collector)
{
    if (!fex)
        return -EINVAL;
    auto *s = static_cast<MotionStateSycl *>(fex->priv);
    VmafSyclState *state = fex->sycl_state;
    if (state)
        vmaf_sycl_queue_wait(state);

    int ret = 0;
    // Write the final motion2 + motion3 scores (delayed-by-one pattern).
    if (s->frame_index > 1) {
        double const last_motion2 =
            MIN(s->prev_motion_score * s->motion_fps_weight, s->motion_max_val);
        ret = vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                      "VMAF_integer_feature_motion2_score",
                                                      last_motion2, s->frame_index - 1);
        if (ret >= 0) {
            double const motion3_score = motion3_postprocess_sycl(s, last_motion2);
            int const ret_m3 = vmaf_feature_collector_append_with_dict(
                feature_collector, s->feature_name_dict, "VMAF_integer_feature_motion3_score",
                motion3_score, s->frame_index - 1);
            if (ret_m3 < 0)
                ret = ret_m3;
        }
    } else if (s->frame_index == 1) {
        /* Single-frame run. collect() wrote motion2[0] = 0, but motion3[0] is
         * back-filled only when a second frame arrives (the frame_index == 1
         * branch there), so on a one-frame run it was never written at all. The
         * model needs it, and libvmaf reports the gap as "problem generating
         * pooled VMAF score" with no indication of which feature is missing.
         *
         * The CPU twin's flush emits both for every frame: with n == 1 its
         * `i < min_idx` branch gives motion2 = 0 and motion3 = stamp_value,
         * and stamp_value is 0 because `n > min_idx` is false. Match that. */
        ret = vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                      "VMAF_integer_feature_motion3_score", 0.0, 0);
    }
    return (ret < 0) ? ret : !ret; // 1 = done, negative = error
}

static int close_fex_sycl(VmafFeatureExtractor *fex)
{
    auto *s = static_cast<MotionStateSycl *>(fex->priv);
    VmafSyclState *state = fex->sycl_state;

    if (state) {
        vmaf_sycl_queue_wait(state);

        /* Unregister from the combined command graph before freeing priv.
         * Without this, a second VmafContext sharing the same sycl_state
         * would inherit a dangling priv pointer in the graph registry,
         * causing a SIGSEGV when the graph is re-recorded. ADR-0989.
         * vmaf_sycl_graph_unregister() now also drains combined_queue
         * before destroying any recorded exec-graphs (Level Zero
         * command-list release races against in-flight GPU work). */
        (void)vmaf_sycl_graph_unregister(state, s);

        if (s->d_blur[0])
            vmaf_sycl_free(state, s->d_blur[0]);
        if (s->d_blur[1])
            vmaf_sycl_free(state, s->d_blur[1]);
        if (s->d_blur_tmp)
            vmaf_sycl_free(state, s->d_blur_tmp);
        if (s->d_sad_accum)
            vmaf_sycl_free(state, s->d_sad_accum);
        if (s->h_sad_accum)
            vmaf_sycl_free(state, s->h_sad_accum);

        // UV plane resources — ADR-0989
        if (s->d_ref_u[0])
            vmaf_sycl_free(state, s->d_ref_u[0]);
        if (s->d_ref_u[1])
            vmaf_sycl_free(state, s->d_ref_u[1]);
        if (s->d_ref_v[0])
            vmaf_sycl_free(state, s->d_ref_v[0]);
        if (s->d_ref_v[1])
            vmaf_sycl_free(state, s->d_ref_v[1]);
        if (s->d_blur_u[0])
            vmaf_sycl_free(state, s->d_blur_u[0]);
        if (s->d_blur_u[1])
            vmaf_sycl_free(state, s->d_blur_u[1]);
        if (s->d_blur_v[0])
            vmaf_sycl_free(state, s->d_blur_v[0]);
        if (s->d_blur_v[1])
            vmaf_sycl_free(state, s->d_blur_v[1]);
        if (s->d_sad_u)
            vmaf_sycl_free(state, s->d_sad_u);
        if (s->h_sad_u)
            vmaf_sycl_free(state, s->h_sad_u);
        if (s->d_sad_v)
            vmaf_sycl_free(state, s->d_sad_v);
        if (s->h_sad_v)
            vmaf_sycl_free(state, s->h_sad_v);
    }

    if (s->feature_name_dict)
        vmaf_dictionary_free(&s->feature_name_dict);

    return 0;
}

/* ------------------------------------------------------------------ */
/* Feature extractor definition                                        */
/* ------------------------------------------------------------------ */

/* T3-15(c) / ADR-0219: motion3_score is now provided (3-frame mode
 * only). The 5-frame window mode remains deferred — init() rejects
 * it with -ENOTSUP. */
static const char *provided_features[] = {"VMAF_integer_feature_motion_score",
                                          "VMAF_integer_feature_motion2_score",
                                          "VMAF_integer_feature_motion3_score", nullptr};

extern "C" VmafFeatureExtractor vmaf_fex_integer_motion_sycl = {
    .name = "motion_sycl",
    .init = init_fex_sycl,
    .extract = extract_fex_sycl,
    .flush = flush_fex_sycl,
    .close = close_fex_sycl,
    .submit = submit_fex_sycl,
    .collect = collect_fex_sycl,
    .options = options,
    .priv_size = sizeof(MotionStateSycl),
    .flags = VMAF_FEATURE_EXTRACTOR_TEMPORAL | VMAF_FEATURE_EXTRACTOR_SYCL,
    .provided_features = provided_features,
};
