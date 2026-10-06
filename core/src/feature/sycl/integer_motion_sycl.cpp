/**
 *
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
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
 * SAD (Sum of Absolute Differences) of the 5-tap Gaussian blur
 * {3571, 16004, 26386, 16004, 3571} (sum = 65536) of the difference between
 * consecutive reference frames, computed by the shared motion pipeline
 * (integer_motion_pipeline_sycl.h), which reproduces the CPU reference
 * integer_motion.c bit for bit:
 *   1. motion_score = SAD(blur(prev - cur)) / 256.0 / (width * height)
 *   2. motion2_score = min(motion_score[i], motion_score[i + 1])
 *   3. motion3_score = host-side blend / clip / moving average of motion2
 *
 * The luma kernel reads the current frame from the shared frame buffer and
 * keeps a copy in a raw ping-pong (d_raw_y) that the next frame differences
 * against; with motion_add_uv the uploaded chroma ping-pongs already hold both
 * frames.
 *
 * With motion_five_frame_window (ADR-1491) the SAD is taken against the frame
 * two back, as on the CPU (integer_motion.c, Netflix a2b59b77): d_raw_y[0]
 * holds frame n-2 and d_raw_y[1] frame n-1, advanced by two device copies
 * after each frame's kernel. collect() then stores the SAD scores only, and
 * advance() and flush() derive motion2 / motion3 of every frame from them,
 * each once its window is complete (ADR-2090), with the CPU's own functions,
 * vmaf_motion_window_advance() / _flush() (motion_window.h, ADR-1478).
 *
 * Pattern: init -> submit (non-blocking) -> collect (wait + scores)
 * TEMPORAL flag: frames must be processed in sequential order.
 */

#include <sycl/sycl.hpp>

#include "integer_motion_pipeline_sycl.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "config.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "motion_blend_tools.h"
#include "motion_window.h"
#include "picture_geometry.h"
#include "sycl/common.h"
#include "log.h"

/* Default upper clamp on motion / motion2 / motion3 — mirrors
 * DEFAULT_MOTION_MAX_VAL in libvmaf/src/feature/integer_motion.c.
 * T3-15(c) / ADR-0219. */
static constexpr double MOTION_SYCL_DEFAULT_MAX_VAL = 10000.0;

// NOLINTBEGIN(misc-use-anonymous-namespace, misc-use-internal-linkage) — ADR-0141 §2 load-bearing invariant: the
// TU-local helpers and the `init_fex_sycl` / `extract_fex_sycl` /
// `submit_fex_sycl` / `collect_fex_sycl` / `flush_fex_sycl` /
// `close_fex_sycl` entry-point functions all use C-style `static` instead of
// an anonymous namespace because their addresses are stored in the
// `extern "C" VmafFeatureExtractor` struct at the bottom of this file
// (which the C ABI consumes via function-pointer types declared in
// `feature_extractor.h`). C++ anon namespace would still work for the type
// match but moves the helpers further from the `static` idiom that the C-API
// boundary expects, and clang-tidy's `misc-use-internal-linkage` no-ops
// against `extern "C"` type aliases anyway. Per CLAUDE.md §12 r12 these are
// load-bearing invariants of the SYCL ↔ libvmaf C-API ABI.

/* ------------------------------------------------------------------ */
/* Extractor private state                                             */
/* ------------------------------------------------------------------ */

struct MotionStateSycl {
    unsigned width, height;
    unsigned bpc;

    bool debug;
    bool motion_force_zero;
    bool motion_five_frame_window; // SAD against frame n-2, window in flush()
    bool motion_moving_average;
    bool motion_add_uv; // include U + V plane SAD — ADR-0989
    double motion_blend_factor;
    double motion_blend_offset;
    double motion_fps_weight;
    double motion_max_val;

    VmafDictionary *feature_name_dict;
    /* motion2 / motion3 of the five-frame window, derived as the SAD scores
     * come in (ADR-2090). */
    VmafMotionWindowState window_state;

    // Frame tracking
    unsigned frame_index;
    double prev_motion_score;
    // motion3 post-processing state — last *unaveraged* blended
    // score, used by the moving-average rule. Mirrors CPU
    // MotionState.previous_score. T3-15(c) / ADR-0219.
    double prev_motion3_blended;

    // Raw Y-plane ping-pong (device): the kernel copies the current frame
    // from the shared frame buffer into d_raw_y[cur_slot]; the next frame
    // differences against it as d_raw_y[1 - cur_slot].
    // With motion_five_frame_window the two planes have fixed roles instead:
    // d_raw_y[0] holds frame n-2 (what the kernel differences against) and
    // d_raw_y[1] frame n-1; motion_post_graph() advances both.
    void *d_raw_y[2];
    int cur_slot; // ping-pong slot of the current frame

    // Y-plane SAD accumulator (device + host)
    int64_t *d_sad_accum;
    int64_t *h_sad_accum;

    // UV plane support — ADR-0989.
    // When motion_add_uv=true, submit packs the reference U/V planes into
    // pinned host staging, pre_fn copies them H2D on the combined queue into
    // two more ping-pong pairs, and the kernel differences d_ref_u[cur_slot]
    // against d_ref_u[1 - cur_slot].  Separate SAD accumulators allow
    // per-plane normalization on the host (UV planes are smaller than Y
    // for YUV420P).
    unsigned chroma_w, chroma_h; // U/V plane dimensions
    void *h_stage_u;             // packed U staging (pinned host), written in submit
    void *h_stage_v;             // packed V staging (pinned host)
    void *d_stage_u;             // the same for VMAFx device frames (device, ADR-2091)
    void *d_stage_v;
    bool stage_on_device; // this frame's chroma is in d_stage_u / d_stage_v
    void *d_ref_u[2];     // raw U-plane ping-pong (device, void* for bpc agnostic)
    void *d_ref_v[2];     // raw V-plane ping-pong (device)
    int64_t *d_sad_u;     // U-plane SAD accumulator (device)
    int64_t *h_sad_u;     // U-plane SAD readback (host-mapped)
    int64_t *d_sad_v;     // V-plane SAD accumulator (device)
    int64_t *h_sad_v;     // V-plane SAD readback (host-mapped)

    // Deferred state
    unsigned pending_index;
    bool has_pending;

    // Back-pointer for graph-mode checks
    VmafSyclState *sycl_state;
};

/* Options table — mirrors libvmaf/src/feature/integer_motion.c.
 * The motion3-related post-processing options drive a host-side
 * derivation from motion2 (see collect()). T3-15(c) / ADR-0219. */
static const VmafOption options[] = {
    {
        .name = "debug",
        .help = "debug mode: enable additional output",
        .offset = offsetof(MotionStateSycl, debug),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val = {.b = false},
    },
    {
        .name = "motion_force_zero",
        .help = "force motion score to zero",
        .alias = "force_0",
        .offset = offsetof(MotionStateSycl, motion_force_zero),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val = {.b = false},
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "motion_blend_factor",
        .help = "blend motion score given an offset",
        .alias = "mbf",
        .offset = offsetof(MotionStateSycl, motion_blend_factor),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val = {.d = 1.0},
        .min = 0.0,
        .max = 1.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "motion_blend_offset",
        .help = "blend motion score starting from this offset",
        .alias = "mbo",
        .offset = offsetof(MotionStateSycl, motion_blend_offset),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val = {.d = 40.0},
        .min = 0.0,
        .max = 1000.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "motion_fps_weight",
        .help = "fps-aware multiplicative weight/correction",
        .alias = "mfw",
        .offset = offsetof(MotionStateSycl, motion_fps_weight),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val = {.d = 1.0},
        .min = 0.0,
        .max = 5.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "motion_max_val",
        .help = "maximum value allowed; larger values will be clipped to this value",
        .alias = "mmxv",
        .offset = offsetof(MotionStateSycl, motion_max_val),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val = {.d = MOTION_SYCL_DEFAULT_MAX_VAL},
        .min = 0.0,
        .max = 10000.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "motion_five_frame_window",
        .help = "use five-frame temporal window",
        .alias = "mffw",
        .offset = offsetof(MotionStateSycl, motion_five_frame_window),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val = {.b = false},
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "motion_moving_average",
        .help = "use moving average for motion3 scores after first frame",
        .alias = "mma",
        .offset = offsetof(MotionStateSycl, motion_moving_average),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val = {.b = false},
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "motion_add_uv",
        .help = "include U and V plane SADs in the motion score (ADR-0989)",
        .alias = "mau",
        .offset = offsetof(MotionStateSycl, motion_add_uv),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val = {.b = false},
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {.name = nullptr}};

/* ------------------------------------------------------------------ */
/* Feature extractor callbacks                                         */
/* ------------------------------------------------------------------ */

// Forward declarations for combined graph callbacks (defined after init)
static void enqueue_motion_work(void *queue_ptr, void *priv, void *shared_ref, void *shared_dis);
static void motion_pre_graph(void *queue_ptr, void *priv);
static void motion_post_graph(void *queue_ptr, void *priv);
static void config_motion_slot(void *priv, int slot);
static int close_fex_sycl(VmafFeatureExtractor *fex); /* forward decl for init error paths */

static int motion_validate_init(const MotionStateSycl *s, unsigned w, unsigned h)
{
    // motion_add_uv is this twin's own option: the CPU `motion` has none, so
    // the five-frame window with chroma has no reference to equal.
    if (s->motion_five_frame_window && s->motion_add_uv) {
        vmaf_log(VMAF_LOG_LEVEL_WARNING,
                 "motion_sycl: motion_five_frame_window=true with motion_add_uv=true is not "
                 "supported (the CPU extractor `motion` has no chroma mode to match).\n");
        return -ENOTSUP;
    }
    if (h < 3u || w < 3u) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "motion_sycl: frame %ux%u is below the 5-tap filter minimum 3x3; "
                 "refusing to avoid out-of-bounds mirror reads on device\n",
                 w, h);
        return -EINVAL;
    }
    return 0;
}

static void motion_reset_state(MotionStateSycl *s, unsigned w, unsigned h, unsigned bpc)
{
    s->width = w;
    s->height = h;
    s->bpc = bpc;
    s->frame_index = 0;
    s->prev_motion_score = 0.0;
    s->prev_motion3_blended = 0.0;
    s->has_pending = false;
    s->cur_slot = 0;
}

static int motion_alloc_luma(VmafSyclState *state, MotionStateSycl *s, unsigned w, unsigned h,
                             unsigned bpc)
{
    size_t const buf_size = (size_t)w * h * ((bpc <= 8) ? 1U : 2U);
    s->d_raw_y[0] = vmaf_sycl_malloc_device(state, buf_size);
    s->d_raw_y[1] = vmaf_sycl_malloc_device(state, buf_size);
    s->d_sad_accum = static_cast<int64_t *>(vmaf_sycl_malloc_device(state, sizeof(int64_t)));
    s->h_sad_accum = static_cast<int64_t *>(vmaf_sycl_malloc_host(state, sizeof(int64_t)));
    if (!s->d_raw_y[0] || !s->d_raw_y[1] || !s->d_sad_accum || !s->h_sad_accum) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "motion_sycl: device memory allocation failed\n");
        return -ENOMEM;
    }
    if (!s->motion_five_frame_window)
        return 0;

    // Five-frame window: the kernel runs on every frame, also on the first
    // two, whose SAD nobody reads (enqueue_motion_work()). Give it defined
    // planes to read until frames 0 and 1 have been copied in.
    auto *q = static_cast<sycl::queue *>(vmaf_sycl_get_queue_ptr(state));
    if (!q)
        return -EINVAL;
    try {
        q->memset(s->d_raw_y[0], 0, buf_size);
        q->memset(s->d_raw_y[1], 0, buf_size);
        q->wait();
    } catch (const sycl::exception &e) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "motion_sycl: clearing the frame planes: %s\n", e.what());
        return -EIO;
    }
    return 0;
}

static void motion_reset_chroma(MotionStateSycl *s)
{
    s->h_stage_u = nullptr;
    s->h_stage_v = nullptr;
    s->d_stage_u = nullptr;
    s->d_stage_v = nullptr;
    s->stage_on_device = false;
    s->d_ref_u[0] = nullptr;
    s->d_ref_u[1] = nullptr;
    s->d_ref_v[0] = nullptr;
    s->d_ref_v[1] = nullptr;
    s->d_sad_u = nullptr;
    s->h_sad_u = nullptr;
    s->d_sad_v = nullptr;
    s->h_sad_v = nullptr;
    s->chroma_w = 0;
    s->chroma_h = 0;
}

static int motion_configure_chroma(MotionStateSycl *s, enum VmafPixelFormat pix_fmt, unsigned w,
                                   unsigned h)
{
    if (!s->motion_add_uv)
        return 0;
    switch (pix_fmt) {
    case VMAF_PIX_FMT_YUV420P:
    case VMAF_PIX_FMT_YUV422P:
    case VMAF_PIX_FMT_YUV444P:
        break;
    case VMAF_PIX_FMT_YUV400P:
    case VMAF_PIX_FMT_UNKNOWN:
    default:
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "motion_sycl: motion_add_uv=true requires a YUV format with chroma "
                 "planes (got %d)\n",
                 (int)pix_fmt);
        return -EINVAL;
    }
    const bool ss_hor = (pix_fmt != VMAF_PIX_FMT_YUV444P);
    const bool ss_ver = (pix_fmt == VMAF_PIX_FMT_YUV420P);
    s->chroma_w = vmaf_chroma_extent(w, ss_hor);
    s->chroma_h = vmaf_chroma_extent(h, ss_ver);
    return 0;
}

static int motion_alloc_chroma(VmafSyclState *state, MotionStateSycl *s, unsigned bpc)
{
    if (!s->motion_add_uv)
        return 0;

    size_t const bpp = (bpc <= 8) ? 1U : 2U;
    size_t const uv_raw_size = (size_t)s->chroma_w * s->chroma_h * bpp;
    s->h_stage_u = vmaf_sycl_malloc_host(state, uv_raw_size);
    s->h_stage_v = vmaf_sycl_malloc_host(state, uv_raw_size);
    s->d_ref_u[0] = vmaf_sycl_malloc_device(state, uv_raw_size);
    s->d_ref_u[1] = vmaf_sycl_malloc_device(state, uv_raw_size);
    s->d_ref_v[0] = vmaf_sycl_malloc_device(state, uv_raw_size);
    s->d_ref_v[1] = vmaf_sycl_malloc_device(state, uv_raw_size);
    s->d_sad_u = static_cast<int64_t *>(vmaf_sycl_malloc_device(state, sizeof(int64_t)));
    s->h_sad_u = static_cast<int64_t *>(vmaf_sycl_malloc_host(state, sizeof(int64_t)));
    s->d_sad_v = static_cast<int64_t *>(vmaf_sycl_malloc_device(state, sizeof(int64_t)));
    s->h_sad_v = static_cast<int64_t *>(vmaf_sycl_malloc_host(state, sizeof(int64_t)));
    if (!s->h_stage_u || !s->h_stage_v || !s->d_ref_u[0] || !s->d_ref_u[1] || !s->d_ref_v[0] ||
        !s->d_ref_v[1] || !s->d_sad_u || !s->h_sad_u || !s->d_sad_v || !s->h_sad_v) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "motion_sycl: UV device memory allocation failed\n");
        return -ENOMEM;
    }
    return 0;
}

static int init_fex_sycl(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                         unsigned w, unsigned h)
{
    auto *s = static_cast<MotionStateSycl *>(fex->priv);
    int err = motion_validate_init(s, w, h);
    if (err)
        return err;
    motion_reset_state(s, w, h, bpc);

    if (!fex->sycl_state) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "motion_sycl: no SYCL state\n");
        return -EINVAL;
    }

    VmafSyclState *state = fex->sycl_state;

    err = vmaf_sycl_shared_frame_init(state, w, h, bpc);
    if (err)
        return err;

    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (!s->feature_name_dict)
        return -ENOMEM;
    // motion_force_zero publishes zeros and computes no SAD, as the CPU does:
    // no planes, no kernels, no place in the combined graph.
    if (s->motion_force_zero)
        return 0;

    err = motion_alloc_luma(state, s, w, h, bpc);
    if (err) {
        close_fex_sycl(fex);
        return err;
    }

    motion_reset_chroma(s);
    err = motion_configure_chroma(s, pix_fmt, w, h);
    if (!err)
        err = motion_alloc_chroma(state, s, bpc);
    if (err) {
        close_fex_sycl(fex);
        return err;
    }

    // Store back-pointer for graph-mode checks in post_fn
    s->sycl_state = state;

    // Register with combined command graph
    err = vmaf_sycl_graph_register(state, enqueue_motion_work, motion_pre_graph, motion_post_graph,
                                   config_motion_slot, s, "MOTION");
    if (err) {
        close_fex_sycl(fex);
        return err;
    }

    return 0;
}

/* The CPU's per-frame outputs (integer_motion.c::extract): the SAD score on
 * every frame, and the same value as the motion score with debug=true.
 * `sad_score` is the frame's SAD weighted by motion_fps_weight and capped at
 * motion_max_val; 0 on the first frame and under motion_force_zero. */
static int motion_append_sad_score(const MotionStateSycl *s, double sad_score, unsigned index,
                                   VmafFeatureCollector *feature_collector)
{
    int err = vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                      "VMAF_integer_feature_motion_sad_score",
                                                      sad_score, index);
    if (s->debug) {
        err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                       "VMAF_integer_feature_motion_score",
                                                       sad_score, index);
    }
    return err;
}

/* One frame under motion_force_zero. The CPU appends a SAD score of 0 on
 * every frame (and repeats it as the motion score under debug), and its
 * advance() and flush() derive motion2 = motion3 = 0 from those zeros for
 * every frame (integer_motion.c). All of a frame's outputs are known here, so
 * advance_fex_sycl() and flush() have nothing to add. */
static int motion_append_forced_zero(MotionStateSycl *s, unsigned index,
                                     VmafFeatureCollector *feature_collector)
{
    int err = motion_append_sad_score(s, 0.0, index, feature_collector);
    err |= vmaf_feature_collector_append_with_dict(
        feature_collector, s->feature_name_dict, "VMAF_integer_feature_motion2_score", 0.0, index);
    err |= vmaf_feature_collector_append_with_dict(
        feature_collector, s->feature_name_dict, "VMAF_integer_feature_motion3_score", 0.0, index);

    s->frame_index++;
    s->has_pending = false;
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

// Pre-graph (direct enqueue, outside graph): the chroma H2D copies from the
// staging submit filled, then the SAD accumulator resets. The combined queue
// is in order and the graph replay is fenced behind a barrier, so the kernels
// read this frame's chroma without any host wait
// (T-SYCL-MOTION-ADD-UV-SUBMIT-WAIT-2026-09-29, ADR-1371).
static void motion_pre_graph(void *queue_ptr, void *priv)
{
    sycl::queue &q = *static_cast<sycl::queue *>(queue_ptr);
    auto *s = static_cast<MotionStateSycl *>(priv);
    if (s->motion_add_uv) {
        size_t const bytes = (size_t)s->chroma_w * s->chroma_h * ((s->bpc <= 8) ? 1U : 2U);
        q.memcpy(s->d_ref_u[s->cur_slot], s->stage_on_device ? s->d_stage_u : s->h_stage_u, bytes);
        q.memcpy(s->d_ref_v[s->cur_slot], s->stage_on_device ? s->d_stage_v : s->h_stage_v, bytes);
    }
    // Five-frame window: the kernel adds into the accumulator on every frame
    // (enqueue_motion_work()), so it is cleared on every frame.
    if (s->frame_index > 0 || s->motion_five_frame_window) {
        q.memset(s->d_sad_accum, 0, sizeof(int64_t));
        if (s->motion_add_uv) {
            q.memset(s->d_sad_u, 0, sizeof(int64_t));
            q.memset(s->d_sad_v, 0, sizeof(int64_t));
        }
    }
}

/* One plane's share of the frame: the SAD against the previous frame, or on
 * the first frame (nothing to difference against) only the copy of the
 * current frame that the next one needs. */
static void enqueue_motion_plane(sycl::queue &q, const motion_sycl_pipeline::SadArgs &args,
                                 bool has_prev)
{
    if (has_prev) {
        motion_sycl_pipeline::enqueue_sad(q, args);
    } else {
        motion_sycl_pipeline::enqueue_copy(q, args);
    }
}

// Five-frame window: sum(|blur(frame n-2 - frame n)|), with frame n-2 in
// d_raw_y[0]. Enqueued on every frame, also the first two, whose result
// collect() does not read: the combined graph is recorded once and replayed
// (common.cpp), so what a frame enqueues cannot depend on its index. The
// kernel keeps no copy; the copies that advance the two planes are plain
// queue commands behind the replay (motion_post_graph()), which need not
// agree with the graph's two slots.
static void enqueue_motion_window(sycl::queue &q, const MotionStateSycl *s, void *shared_ref)
{
    motion_sycl_pipeline::enqueue_sad(q, {.prev = s->d_raw_y[0],
                                          .cur = shared_ref,
                                          .cur_copy = nullptr,
                                          .sad = s->d_sad_accum,
                                          .width = s->width,
                                          .height = s->height,
                                          .bpc = s->bpc});
}

// Graph-recorded: compute kernels only (Y plane + optional UV planes)
static void enqueue_motion_work(void *queue_ptr, void *priv, void *shared_ref, void *shared_dis)
{
    (void)shared_dis; // Motion only uses ref
    sycl::queue &q = *static_cast<sycl::queue *>(queue_ptr);
    auto *s = static_cast<MotionStateSycl *>(priv);

    if (s->motion_five_frame_window) {
        enqueue_motion_window(q, s, shared_ref);
        return;
    }

    bool const has_prev = (s->frame_index > 0);
    int const cur = s->cur_slot;
    int const prev = 1 - cur;

    // Y plane (always). The current frame comes from the shared frame buffer,
    // which a later frame overwrites, so the kernel keeps a copy of it.
    enqueue_motion_plane(q,
                         {.prev = s->d_raw_y[prev],
                          .cur = shared_ref,
                          .cur_copy = s->d_raw_y[cur],
                          .sad = s->d_sad_accum,
                          .width = s->width,
                          .height = s->height,
                          .bpc = s->bpc},
                         has_prev);

    // UV planes — ADR-0989. d_ref_u[cur] / d_ref_v[cur] were uploaded H2D in
    // submit_fex_sycl before the graph fires and are not overwritten before
    // the next frame reads them, so no copy is needed; the pointer values are
    // stable across graph replays (ping-pong captured per slot via config_fn).
    if (s->motion_add_uv) {
        enqueue_motion_plane(q,
                             {.prev = s->d_ref_u[prev],
                              .cur = s->d_ref_u[cur],
                              .cur_copy = nullptr,
                              .sad = s->d_sad_u,
                              .width = s->chroma_w,
                              .height = s->chroma_h,
                              .bpc = s->bpc},
                             has_prev);
        enqueue_motion_plane(q,
                             {.prev = s->d_ref_v[prev],
                              .cur = s->d_ref_v[cur],
                              .cur_copy = nullptr,
                              .sad = s->d_sad_v,
                              .width = s->chroma_w,
                              .height = s->chroma_h,
                              .bpc = s->bpc},
                             has_prev);
    }
}

// Post-graph: D2H SAD accumulator download (direct enqueue, outside graph)
// With two graph slots (config_fn sets cur_slot=slot), each slot writes
// to d_raw_y[slot] and reads from d_raw_y[1-slot].  The natural ping-pong
// alternation keeps the "previous" buffer correct — no extra copy needed.
static void motion_post_graph(void *queue_ptr, void *priv)
{
    sycl::queue &q = *static_cast<sycl::queue *>(queue_ptr);
    auto *s = static_cast<MotionStateSycl *>(priv);
    if (s->motion_five_frame_window) {
        // The SAD of a frame that has a frame n-2, then the two planes move
        // on: frame n-1 becomes frame n-2 and this frame becomes frame n-1.
        // The queue is in order and the replay is fenced by a barrier, so
        // the kernel has read d_raw_y[0] before the first copy overwrites it.
        if (s->frame_index >= 2) {
            q.memcpy(s->h_sad_accum, s->d_sad_accum, sizeof(int64_t));
        }
        size_t const bytes = (size_t)s->width * s->height * ((s->bpc <= 8) ? 1U : 2U);
        const void *cur = vmaf_sycl_get_shared_plane(s->sycl_state, 1, 0);
        q.memcpy(s->d_raw_y[0], s->d_raw_y[1], bytes);
        if (cur) {
            q.memcpy(s->d_raw_y[1], cur, bytes);
        }
        return;
    }
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
    s->cur_slot = slot;
}

/* ------------------------------------------------------------------ */
/* Submit / Collect                                                    */
/* ------------------------------------------------------------------ */

/* Pack one reference chroma plane, `rows` rows of `row_bytes`, into pinned
 * staging. */
static void motion_stage_plane(const VmafPicture *pic, unsigned plane, void *dst, size_t row_bytes,
                               unsigned rows)
{
    const auto *src = static_cast<const uint8_t *>(pic->data[plane]);
    auto *out = static_cast<uint8_t *>(dst);
    for (unsigned row = 0; row < rows; row++) {
        std::memcpy(out + ((size_t)row * row_bytes), src + ((size_t)row * pic->stride[plane]),
                    row_bytes);
    }
}

/* A frame of the VMAFx API on this device (ADR-2091): its reference U and V
 * are copied on the device, on the combined queue, into device staging that
 * motion_pre_graph's copies then read (the frame's planes are never read on
 * the host). The staging is allocated with the first such frame. */
static int motion_stage_device_chroma(MotionStateSycl *s, const VmafPicture *ref_pic,
                                      size_t row_bytes)
{
    void *const q = vmaf_sycl_get_combined_queue(s->sycl_state);
    size_t const bytes = row_bytes * s->chroma_h;
    if (!s->d_stage_u)
        s->d_stage_u = vmaf_sycl_malloc_device(s->sycl_state, bytes);
    if (!s->d_stage_v)
        s->d_stage_v = vmaf_sycl_malloc_device(s->sycl_state, bytes);
    if (!q || !s->d_stage_u || !s->d_stage_v)
        return -ENOMEM;
    const int err = vmaf_sycl_picture_read_plane(ref_pic, 1U, q, s->d_stage_u, row_bytes, row_bytes,
                                                 s->chroma_h, nullptr);
    return err ? err :
                 vmaf_sycl_picture_read_plane(ref_pic, 2U, q, s->d_stage_v, row_bytes, row_bytes,
                                              s->chroma_h, nullptr);
}

/* Stage this frame's reference U and V for motion_pre_graph's H2D copies.
 * Host work only: the graph for this frame is enqueued by the last extractor
 * to submit, so it cannot start before this runs, and this extractor's collect
 * of the previous frame has already drained the copies that read the staging
 * last time. */
static int motion_stage_chroma(MotionStateSycl *s, const VmafPicture *ref_pic)
{
    if (!s->motion_add_uv || ref_pic == nullptr)
        return 0;

    size_t const row_bytes = (size_t)s->chroma_w * ((s->bpc <= 8) ? 1U : 2U);
    s->stage_on_device = vmaf_sycl_picture_on_device(ref_pic);
    if (s->stage_on_device)
        return motion_stage_device_chroma(s, ref_pic, row_bytes);
    motion_stage_plane(ref_pic, 1U, s->h_stage_u, row_bytes, s->chroma_h);
    motion_stage_plane(ref_pic, 2U, s->h_stage_v, row_bytes, s->chroma_h);
    return 0;
}

static int submit_fex_sycl(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                           VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic_90;
    (void)dist_pic;
    (void)dist_pic_90;

    auto *s = static_cast<MotionStateSycl *>(fex->priv);
    if (s->motion_force_zero) {
        // Not in the combined graph (init): nothing to stage or enqueue.
        s->pending_index = index;
        s->has_pending = true;
        return 0;
    }
    int const stage_err = motion_stage_chroma(s, ref_pic);
    if (stage_err)
        return stage_err;

    // Combined graph submit (once per frame — the last extractor's call
    // enqueues every registered extractor's work)
    int const err = vmaf_sycl_graph_submit(fex->sycl_state);
    if (err)
        return err;

    s->pending_index = index;
    s->has_pending = true;

    return 0;
}

static double motion_score_from_sad(const MotionStateSycl *s)
{
    double motion_score = 0.0;
    // The CPU's min_idx (integer_motion.c::extract): the first frame, and the
    // second with the five-frame window, have no frame to difference against.
    unsigned const min_idx = s->motion_five_frame_window ? 2U : 1U;
    if (s->frame_index >= min_idx) {
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
                                VmafFeatureCollector *feature_collector)
{
    int err = vmaf_feature_collector_append_with_dict(
        feature_collector, s->feature_name_dict, "VMAF_integer_feature_motion2_score", 0.0, index);
    err |= motion_append_sad_score(s, 0.0, index, feature_collector);
    return err;
}

static int motion_collect_second(MotionStateSycl *s, double motion_score, unsigned index,
                                 VmafFeatureCollector *feature_collector)
{
    double const score_clipped = MIN(motion_score * s->motion_fps_weight, s->motion_max_val);
    double const motion3_score = motion3_postprocess_sycl(s, score_clipped);
    int err = vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                      "VMAF_integer_feature_motion3_score",
                                                      motion3_score, index - 1);
    err |= motion_append_sad_score(s, score_clipped, index, feature_collector);
    return err;
}

static int motion_collect_later(MotionStateSycl *s, double motion_score, unsigned index,
                                VmafFeatureCollector *feature_collector)
{
    double const motion2 =
        (motion_score < s->prev_motion_score) ? motion_score : s->prev_motion_score;
    double const motion2_clipped = MIN(motion2 * s->motion_fps_weight, s->motion_max_val);
    int err = vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                      "VMAF_integer_feature_motion2_score",
                                                      motion2_clipped, index - 1);
    double const motion3_score = motion3_postprocess_sycl(s, motion2_clipped);
    err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "VMAF_integer_feature_motion3_score",
                                                   motion3_score, index - 1);
    double const score_clipped = MIN(motion_score * s->motion_fps_weight, s->motion_max_val);
    err |= motion_append_sad_score(s, score_clipped, index, feature_collector);
    return err;
}

static int collect_fex_sycl(VmafFeatureExtractor *fex, unsigned index,
                            VmafFeatureCollector *feature_collector)
{
    auto *s = static_cast<MotionStateSycl *>(fex->priv);
    VmafSyclState *state = fex->sycl_state;
    if (s->motion_force_zero)
        return motion_append_forced_zero(s, index, feature_collector);

    // Combined graph wait (idempotent per frame — first extractor wins). A
    // failed wait leaves the SAD stale: fail, never score it.
    int const wait_err = vmaf_sycl_graph_wait(state);
    if (wait_err)
        return wait_err;

    double const motion_score = motion_score_from_sad(s);
    int err;
    if (s->motion_five_frame_window) {
        // The SAD score only (0 for frames 0 and 1); advance_fex_sycl() and
        // flush() derive motion2 and motion3 from the stored scores.
        err =
            motion_append_sad_score(s, MIN(motion_score * s->motion_fps_weight, s->motion_max_val),
                                    index, feature_collector);
    } else if (s->frame_index == 0) {
        err = motion_collect_first(s, index, feature_collector);
    } else if (s->frame_index == 1) {
        err = motion_collect_second(s, motion_score, index, feature_collector);
    } else {
        err = motion_collect_later(s, motion_score, index, feature_collector);
    }

    // Advance state
    s->prev_motion_score = motion_score;
    s->cur_slot = 1 - s->cur_slot; // flip ping-pong (direct mode)
    s->frame_index++;
    s->has_pending = false;

    return err;
}

static int extract_fex_sycl(VmafFeatureExtractor *fex, VmafPicture *ref_pic,
                            VmafPicture *ref_pic_90, VmafPicture *dist_pic,
                            VmafPicture *dist_pic_90, unsigned index,
                            VmafFeatureCollector *feature_collector)
{
    // submit / collect handle motion_force_zero: libvmaf drives a SYCL
    // extractor through that pair, never through extract().
    int const err = submit_fex_sycl(fex, ref_pic, ref_pic_90, dist_pic, dist_pic_90, index);
    if (err)
        return err;
    return collect_fex_sycl(fex, index, feature_collector);
}

/* The five-frame window of this twin's options, on its state: the CPU
 * extractor's (integer_motion.c, motion_window.h). */
static VmafMotionWindow motion_window_of(MotionStateSycl *s)
{
    const VmafMotionWindow window = {
        .sad_feature = "VMAF_integer_feature_motion_sad_score",
        .motion2_feature = "VMAF_integer_feature_motion2_score",
        .motion3_feature = "VMAF_integer_feature_motion3_score",
        .motion_blend_factor = s->motion_blend_factor,
        .motion_blend_offset = s->motion_blend_offset,
        .motion_max_val = s->motion_max_val,
        .motion_five_frame_window = true,
        .motion_moving_average = s->motion_moving_average,
        .state = &s->window_state,
    };
    return window;
}

/* motion2 and motion3 of the frames no advance derived, with the five-frame
 * window, from the stored SAD scores by the CPU extractor's own function
 * (integer_motion.c::vmaf_motion_window_flush(), ADR-1478): the scores are
 * the CPU's whenever the SADs are (ADR-1491). */
static int motion_flush_window(MotionStateSycl *s, VmafFeatureCollector *feature_collector)
{
    const VmafMotionWindow window = motion_window_of(s);
    int const err = vmaf_motion_window_flush(feature_collector, s->feature_name_dict, &window);
    return err ? err : 1;
}

/* ADR-2090: motion2 / motion3 of the frames whose five-frame window the SAD
 * scores collected so far complete (vmaf_motion_window_advance()). The
 * three-frame path and motion_force_zero emit their own scores in collect(). */
static int advance_fex_sycl(VmafFeatureExtractor *fex, VmafFeatureCollector *feature_collector)
{
    auto *s = static_cast<MotionStateSycl *>(fex->priv);
    if (!s->motion_five_frame_window || s->motion_force_zero || s->feature_name_dict == nullptr)
        return 0;
    const VmafMotionWindow window = motion_window_of(s);
    return vmaf_motion_window_advance(feature_collector, s->feature_name_dict, &window);
}

static int flush_fex_sycl(VmafFeatureExtractor *fex, VmafFeatureCollector *feature_collector)
{
    if (!fex)
        return -EINVAL;
    auto *s = static_cast<MotionStateSycl *>(fex->priv);
    VmafSyclState *state = fex->sycl_state;
    if (state)
        vmaf_sycl_queue_wait(state);
    // Every frame's motion2 / motion3 was appended with its SAD score.
    if (s->motion_force_zero)
        return 1;
    if (s->motion_five_frame_window)
        return motion_flush_window(s, feature_collector);

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

static void motion_free_luma(VmafSyclState *state, MotionStateSycl *s)
{
    if (s->d_raw_y[0])
        vmaf_sycl_free(state, s->d_raw_y[0]);
    if (s->d_raw_y[1])
        vmaf_sycl_free(state, s->d_raw_y[1]);
    if (s->d_sad_accum)
        vmaf_sycl_free(state, s->d_sad_accum);
    if (s->h_sad_accum)
        vmaf_sycl_free(state, s->h_sad_accum);
}

static void motion_free_chroma(VmafSyclState *state, MotionStateSycl *s)
{
    if (s->h_stage_u)
        vmaf_sycl_free(state, s->h_stage_u);
    if (s->h_stage_v)
        vmaf_sycl_free(state, s->h_stage_v);
    if (s->d_stage_u)
        vmaf_sycl_free(state, s->d_stage_u);
    if (s->d_stage_v)
        vmaf_sycl_free(state, s->d_stage_v);
    if (s->d_ref_u[0])
        vmaf_sycl_free(state, s->d_ref_u[0]);
    if (s->d_ref_u[1])
        vmaf_sycl_free(state, s->d_ref_u[1]);
    if (s->d_ref_v[0])
        vmaf_sycl_free(state, s->d_ref_v[0]);
    if (s->d_ref_v[1])
        vmaf_sycl_free(state, s->d_ref_v[1]);
    if (s->d_sad_u)
        vmaf_sycl_free(state, s->d_sad_u);
    if (s->h_sad_u)
        vmaf_sycl_free(state, s->h_sad_u);
    if (s->d_sad_v)
        vmaf_sycl_free(state, s->d_sad_v);
    if (s->h_sad_v)
        vmaf_sycl_free(state, s->h_sad_v);
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
        motion_free_luma(state, s);
        motion_free_chroma(state, s);
    }

    if (s->feature_name_dict)
        vmaf_dictionary_free(&s->feature_name_dict);

    return 0;
}

/* ------------------------------------------------------------------ */
/* Feature extractor definition                                        */
/* ------------------------------------------------------------------ */

/* The CPU `motion` set (integer_motion.c): the SAD score on every frame, the
 * same value as `motion_score` with debug=true, and motion2 / motion3, with
 * the three-frame window and with motion_five_frame_window (ADR-1491). */
static const char *provided_features[] = {
    "VMAF_integer_feature_motion_sad_score",
    "VMAF_integer_feature_motion_score",
    "VMAF_integer_feature_motion2_score",
    "VMAF_integer_feature_motion3_score",
    nullptr,
};

/* The zero-copy path (ADR-1688): the luma SAD reads the shared frame. With
 * motion_add_uv it also reads the reference's U and V host planes, which the
 * zero-copy path does not have: it would add the SAD of stale staging. */
static bool reads_shared_luma_only(const VmafFeatureExtractor *fex)
{
    const auto *s = static_cast<const MotionStateSycl *>(fex->priv);
    return !s->motion_add_uv;
}

// NOLINTEND(misc-use-anonymous-namespace, misc-use-internal-linkage)

extern "C" VmafFeatureExtractor vmaf_fex_integer_motion_sycl = {
    .name = "motion_sycl",
    .init = init_fex_sycl,
    .extract = extract_fex_sycl,
    .flush = flush_fex_sycl,
    .advance = advance_fex_sycl,
    .close = close_fex_sycl,
    .submit = submit_fex_sycl,
    .collect = collect_fex_sycl,
    .options = options,
    .priv_size = sizeof(MotionStateSycl),
    .flags = VMAF_FEATURE_EXTRACTOR_TEMPORAL | VMAF_FEATURE_EXTRACTOR_SYCL,
    .provided_features = provided_features,
    .reads_shared_luma_only = reads_shared_luma_only,
};
