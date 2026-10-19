/**
 *
 *  Copyright 2016-2026 Netflix, Inc.
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

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "dict.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "integer_motion.h"
#include "log.h"
#include "motion_blend_tools.h"
#include "motion_window.h"

#define MIN(x, y) (((x) < (y)) ? (x) : (y))

/* Default maximum value allowed for motion */
#define DEFAULT_MOTION_MAX_VAL (10000.0)

#if ARCH_X86
#include "x86/motion_avx2.h"
#if HAVE_AVX512
#include "x86/motion_avx512.h"
#endif
#endif

#if ARCH_AARCH64
#include "arm64/motion_v2_neon.h"
#endif

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

typedef uint64_t (*motion_pipeline_fn)(const uint8_t *, ptrdiff_t, const uint8_t *, ptrdiff_t,
                                       int32_t *, unsigned, unsigned, unsigned bpc);

typedef struct MotionState {
    int32_t *y_row;
    unsigned w, h, bpc;
    motion_pipeline_fn pipeline;
    double motion_max_val;
    double motion_blend_factor;
    double motion_blend_offset;
    double motion_fps_weight;
    bool motion_five_frame_window;
    bool motion_moving_average;
    bool motion_force_zero;
    bool debug;
    VmafDictionary *feature_name_dict;
    VmafMotionWindowState window_state; /* ADR-2090: derivation of motion2 / motion3 */
} MotionState;

static const VmafOption options[] = {
    {
        .name = "motion_force_zero",
        .alias = "force_0",
        .help = "forcing motion score to zero",
        .offset = offsetof(MotionState, motion_force_zero),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "motion_blend_factor",
        .alias = "mbf",
        .help = "blend motion score given an offset",
        .offset = offsetof(MotionState, motion_blend_factor),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = 1.0,
        .min = 0.0,
        .max = 1.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "motion_blend_offset",
        .alias = "mbo",
        .help = "blend motion score starting from this offset",
        .offset = offsetof(MotionState, motion_blend_offset),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = 40.0,
        .min = 0.0,
        .max = 1000.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "motion_fps_weight",
        .alias = "mfw",
        .help = "fps-aware multiplicative weight/correction",
        .offset = offsetof(MotionState, motion_fps_weight),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = 1.0,
        .min = 0.0,
        .max = 5.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "motion_max_val",
        .alias = "mmxv",
        .help = "maximum value allowed; larger values will be clipped to this value",
        .offset = offsetof(MotionState, motion_max_val),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = DEFAULT_MOTION_MAX_VAL,
        .min = 0.0,
        .max = 10000.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "motion_five_frame_window",
        .alias = "mffw",
        .help = "use five-frame temporal window",
        .offset = offsetof(MotionState, motion_five_frame_window),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "motion_moving_average",
        .alias = "mma",
        .help = "use moving average for motion scores after first frame",
        .offset = offsetof(MotionState, motion_moving_average),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "debug",
        .help = "debug mode: enable additional output",
        .offset = offsetof(MotionState, debug),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
    },
    {0}};

static inline int mirror(int idx, int size)
{
    if (idx < 0)
        return -idx;
    if (idx >= size)
        return 2 * size - idx - 2;
    return idx;
}

static uint64_t motion_score_pipeline_8(const uint8_t *prev, ptrdiff_t prev_stride,
                                        const uint8_t *cur, ptrdiff_t cur_stride, int32_t *y_row,
                                        unsigned w, unsigned h, unsigned bpc)
{
    (void)bpc;
    const int radius = filter_width / 2;
    const int32_t y_round = 1 << 7;
    const int32_t x_round = 1 << 15;

    uint64_t sad = 0;

    for (unsigned i = 0; i < h; i++) {
        // Fused diff + y_conv for row i (shift by 8, matching v1 precision)
        int32_t any_nonzero = 0;
        for (unsigned j = 0; j < w; j++) {
            int32_t accum = 0;
            for (int k = 0; k < filter_width; k++) {
                const int row = mirror((int)i - radius + k, (int)h);
                int32_t diff = prev[row * prev_stride + j] - cur[row * cur_stride + j];
                accum += (int32_t)filter[k] * diff;
            }
            y_row[j] = (accum + y_round) >> 8;
            any_nonzero |= y_row[j];
        }

        if (!any_nonzero)
            continue;

        // x_conv + abs + accumulate for row i
        uint32_t row_sad = 0;
        for (unsigned j = 0; j < w; j++) {
            int64_t accum = 0;
            for (int k = 0; k < filter_width; k++) {
                const int col = mirror((int)j - radius + k, (int)w);
                accum += (int64_t)filter[k] * y_row[col];
            }
            int32_t val = (int32_t)((accum + x_round) >> 16);
            row_sad += abs(val);
        }
        sad += row_sad;
    }

    return sad;
}

static inline uint64_t motion_score_pipeline_16(const uint8_t *prev_u8, ptrdiff_t prev_stride,
                                                const uint8_t *cur_u8, ptrdiff_t cur_stride,
                                                int32_t *y_row, unsigned w, unsigned h,
                                                unsigned bpc)
{
    const uint16_t *prev = (const uint16_t *)prev_u8;
    const uint16_t *cur = (const uint16_t *)cur_u8;
    const ptrdiff_t p_stride = prev_stride / 2;
    const ptrdiff_t c_stride = cur_stride / 2;

    const int radius = filter_width / 2;
    const int32_t y_round = 1 << (bpc - 1);
    const int32_t x_round = 1 << 15;

    uint64_t sad = 0;

    for (unsigned i = 0; i < h; i++) {
        // Fused diff + y_conv for row i
        int32_t any_nonzero = 0;
        for (unsigned j = 0; j < w; j++) {
            int64_t accum = 0;
            for (int k = 0; k < filter_width; k++) {
                const int row = mirror((int)i - radius + k, (int)h);
                int32_t diff = prev[row * p_stride + j] - cur[row * c_stride + j];
                accum += (int64_t)filter[k] * diff;
            }
            y_row[j] = (int32_t)((accum + y_round) >> bpc);
            any_nonzero |= y_row[j];
        }

        if (!any_nonzero)
            continue;

        // x_conv + abs + accumulate for row i
        uint32_t row_sad = 0;
        for (unsigned j = 0; j < w; j++) {
            int64_t accum = 0;
            for (int k = 0; k < filter_width; k++) {
                const int col = mirror((int)j - radius + k, (int)w);
                accum += (int64_t)filter[k] * y_row[col];
            }
            int32_t val = (int32_t)((accum + x_round) >> 16);
            row_sad += abs(val);
        }
        sad += row_sad;
    }

    return sad;
}

/* Pick the SAD pipeline for this bit depth and CPU. Lifted out of `init`
 * verbatim — the same assignments guarded by the same ISA checks, evaluated in
 * the same order, so the last matching ISA still wins — to keep `init` under
 * the HISS-04 / NASA Rule 4 function-size limit (ADR-0141). s->bpc is already
 * set by the caller, so the AArch64 branch reads the same value as before. */
static void motion_select_pipeline(MotionState *s, unsigned bpc)
{
    if (bpc == 8) {
        s->pipeline = motion_score_pipeline_8;
    } else {
        s->pipeline = motion_score_pipeline_16;
    }

#if ARCH_X86
    if (vmaf_get_cpu_flags() & VMAF_X86_CPU_FLAG_AVX2) {
        if (bpc == 8) {
            s->pipeline = motion_score_pipeline_8_avx2;
        } else {
            s->pipeline = motion_score_pipeline_16_avx2;
        }
    }
#if HAVE_AVX512
    if (vmaf_get_cpu_flags() & VMAF_X86_CPU_FLAG_AVX512) {
        if (bpc == 8) {
            s->pipeline = motion_score_pipeline_8_avx512;
        } else {
            s->pipeline = motion_score_pipeline_16_avx512;
        }
    }
#endif
#endif

#if ARCH_AARCH64
    {
        unsigned flags = vmaf_get_cpu_flags();
        if (flags & VMAF_ARM_CPU_FLAG_NEON) {
            s->pipeline =
                s->bpc == 8 ? motion_score_pipeline_8_neon : motion_score_pipeline_16_neon;
        }
    }
#endif
}

static int init(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc, unsigned w,
                unsigned h)
{
    (void)pix_fmt;
    MotionState *s = fex->priv;

    /* The 5-tap separable Gaussian uses reflect-101 mirror padding; the
     * mirror() helper requires dim >= radius + 1 = 3 in each axis.
     * Reject smaller frames up front to prevent out-of-bounds reads.
     * Mirrors the check in integer_motion_v2.c and float_motion.c. */
    const unsigned min_dim = (unsigned)(filter_width / 2 + 1); /* = 3 */
    if (h < min_dim || w < min_dim) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "motion: frame %ux%u is below the 5-tap filter minimum %ux%u; "
                 "refusing to avoid out-of-bounds mirror reads\n",
                 w, h, min_dim, min_dim);
        return -EINVAL;
    }

    s->w = w;
    s->h = h;
    s->bpc = bpc;

    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (!s->feature_name_dict)
        return -ENOMEM;

    s->y_row = malloc(sizeof(*s->y_row) * w);
    if (!s->y_row)
        return -ENOMEM;

    motion_select_pipeline(s, bpc);

    return 0;
}

static int extract(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                   VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index,
                   VmafFeatureCollector *feature_collector)
{
    MotionState *s = fex->priv;

    (void)dist_pic;
    (void)ref_pic_90;
    (void)dist_pic_90;

    double score = 0.;
    int err = 0;

    /* motion_force_zero pins the reported score at 0 and skips the SAD
     * pipeline entirely; the score is still appended below, exactly as when
     * the pipeline runs. */
    if (!s->motion_force_zero) {
        /* Netflix a2b59b77 / a4a1492d: with motion_five_frame_window the SAD
         * of frame n is taken against frame n-2 (fex->prev_prev_ref), so the
         * first two frames have none; otherwise against frame n-1. */
        const unsigned min_idx = s->motion_five_frame_window ? 2 : 1;
        if (index >= min_idx) {
            const VmafPicture *prev =
                s->motion_five_frame_window ? &fex->prev_prev_ref : &fex->prev_ref;
            if (!prev->ref)
                return -EINVAL;

            const unsigned w = s->w;
            const unsigned h = s->h;
            const uint8_t *prev_data = (const uint8_t *)prev->data[0];
            const uint8_t *cur_data = (const uint8_t *)ref_pic->data[0];

            uint64_t sad = s->pipeline(prev_data, prev->stride[0], cur_data, ref_pic->stride[0],
                                       s->y_row, w, h, s->bpc);

            score = MIN((double)sad / 256. / (w * h) * s->motion_fps_weight, s->motion_max_val);
        }
    }

    err = vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                  "VMAF_integer_feature_motion_sad_score", score,
                                                  index);
    if (err)
        return err;

    if (s->debug) {
        return vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                       "VMAF_integer_feature_motion_score", score,
                                                       index);
    }

    return 0;
}

static int close_fex(VmafFeatureExtractor *fex)
{
    MotionState *s = fex->priv;
    free(s->y_row);
    return vmaf_dictionary_free(&s->feature_name_dict);
}

/* Derive and append motion2 / motion3 for one frame index.
 *
 * Lifted verbatim out of flush()'s per-index loop: the same statements in the
 * same order, with the loop-carried `prev_processed` threaded through a pointer
 * so the moving-average recurrence is unchanged. No arithmetic was rewritten,
 * so every emitted score is bit-identical. Keeps flush() under the HISS-04 /
 * NASA Rule 4 function-size limit (ADR-0141). The options and the feature keys
 * come through `window`, so the extractors that share the window (motion_v2
 * and the GPU twins, motion_window.h) run these statements too. */
static int motion_flush_one(VmafFeatureCollector *feature_collector,
                            VmafDictionary *feature_name_dict, const VmafMotionWindow *window,
                            const char *sad_name, unsigned i, double stamp_value,
                            double *prev_processed)
{
    const unsigned stride = window->motion_five_frame_window ? 2 : 1;
    const unsigned min_idx = window->motion_five_frame_window ? 2 : 1;

    double sad_i;
    vmaf_feature_collector_get_score(feature_collector, sad_name, &sad_i, i);

    double motion2;

    if (i < min_idx) {
        motion2 = 0.;
    } else {
        const int lo_idx = (int)i - (int)(stride - 1);
        const int hi_idx = (int)i + 1;
        double hi;
        const bool has_hi =
            !vmaf_feature_collector_get_score(feature_collector, sad_name, &hi, hi_idx);
        if (!has_hi) {
            motion2 = sad_i;
        } else if (lo_idx >= (int)min_idx) {
            double lo;
            vmaf_feature_collector_get_score(feature_collector, sad_name, &lo, lo_idx);
            motion2 = lo < hi ? lo : hi;
        } else {
            motion2 = hi;
        }
    }

    int append_err = vmaf_feature_collector_append_with_dict(feature_collector, feature_name_dict,
                                                             window->motion2_feature, motion2, i);
    if (append_err)
        return append_err;

    double motion3;
    if (i < min_idx) {
        motion3 = stamp_value;
        *prev_processed = stamp_value;
    } else {
        double processed =
            MIN(motion_blend(motion2, window->motion_blend_factor, window->motion_blend_offset),
                window->motion_max_val);
        motion3 = window->motion_moving_average ? (processed + *prev_processed) / 2.0 : processed;
        *prev_processed = processed;
    }

    append_err = vmaf_feature_collector_append_with_dict(feature_collector, feature_name_dict,
                                                         window->motion3_feature, motion3, i);
    if (append_err)
        return append_err;
    return 0;
}

/* upstream flush()'s stamp value: motion3 of the frames below min_idx, from
 * the SAD score of frame min_idx when the n SAD scores reach past it, else 0.
 * The statements of upstream's flush(), lifted out so that the first
 * derivation computes it, whether that is an advance or the flush (ADR-2090);
 * both see n > min_idx exactly when the flush over the whole stream does. */
static double motion_window_stamp(VmafFeatureCollector *feature_collector, const char *sad_name,
                                  const VmafMotionWindow *window, unsigned n)
{
    const unsigned min_idx = window->motion_five_frame_window ? 2 : 1;
    double stamp_value = 0.;
    if (n > min_idx) {
        double sad_at_min_idx;
        if (!vmaf_feature_collector_get_score(feature_collector, sad_name, &sad_at_min_idx,
                                              min_idx)) {
            stamp_value = MIN(motion_blend(sad_at_min_idx, window->motion_blend_factor,
                                           window->motion_blend_offset),
                              window->motion_max_val);
        }
    }
    return stamp_value;
}

/* Extend state->n_sad over the SAD scores the collector holds without a gap:
 * upstream flush()'s count of n, resumed where the last call stopped. A score
 * a worker thread has not appended yet ends the run; a later call finds it. */
static void motion_window_count_sads(VmafFeatureCollector *feature_collector, const char *sad_name,
                                     VmafMotionWindowState *state)
{
    double score;
    while (state->n_sad < UINT_MAX &&
           !vmaf_feature_collector_get_score(feature_collector, sad_name, &score, state->n_sad))
        state->n_sad++;
}

/* Derive frames state->next .. end - 1, in index order, with upstream
 * flush()'s per-frame statements (motion_flush_one()) and the values it
 * carries kept in `state`. */
static int motion_window_derive(VmafFeatureCollector *feature_collector,
                                VmafDictionary *feature_name_dict, const VmafMotionWindow *window,
                                const char *sad_name, unsigned end)
{
    VmafMotionWindowState *state = window->state;
    assert(state != NULL);
    if (state->next == 0 && end > 0)
        state->stamp_value = motion_window_stamp(feature_collector, sad_name, window, state->n_sad);
    for (; state->next < end; state->next++) {
        const int err = motion_flush_one(feature_collector, feature_name_dict, window, sad_name,
                                         state->next, state->stamp_value, &state->prev_processed);
        if (err)
            return err;
    }
    return 0;
}

/* The collector name of the window's SAD score, or NULL. */
static const char *motion_window_sad_name(VmafDictionary *feature_name_dict,
                                          const VmafMotionWindow *window)
{
    const VmafDictionaryEntry *sad_entry =
        vmaf_dictionary_get(&feature_name_dict, window->sad_feature, 0);
    return sad_entry ? sad_entry->val : NULL;
}

/* ADR-2090: frame i is complete once the SAD scores of frames
 * 0 .. max(i + 1, min_idx) are in: then its motion2 has its later frame and
 * its motion3 the moving-average state of frames 0 .. i - 1, the values the
 * flush over the whole stream gives it. */
int vmaf_motion_window_advance(VmafFeatureCollector *feature_collector,
                               VmafDictionary *feature_name_dict, const VmafMotionWindow *window)
{
    if (!window->state)
        return -EINVAL;
    const char *sad_name = motion_window_sad_name(feature_name_dict, window);
    if (!sad_name)
        return -EINVAL;

    VmafMotionWindowState *state = window->state;
    motion_window_count_sads(feature_collector, sad_name, state);
    const unsigned min_idx = window->motion_five_frame_window ? 2 : 1;
    if (state->n_sad <= min_idx)
        return 0;
    return motion_window_derive(feature_collector, feature_name_dict, window, sad_name,
                                state->n_sad - 1);
}

/* The body of flush() below the feature-name dictionary: upstream's
 * statements, shared through motion_window.h (ADR-1478), from the first frame
 * an advance has not derived (ADR-2090). */
int vmaf_motion_window_flush(VmafFeatureCollector *feature_collector,
                             VmafDictionary *feature_name_dict, const VmafMotionWindow *window)
{
    const char *sad_name = motion_window_sad_name(feature_name_dict, window);
    if (!sad_name)
        return -EINVAL;

    VmafMotionWindowState fresh = {0};
    VmafMotionWindow whole = *window;
    if (!whole.state)
        whole.state = &fresh;
    motion_window_count_sads(feature_collector, sad_name, whole.state);
    return motion_window_derive(feature_collector, feature_name_dict, &whole, sad_name,
                                whole.state->n_sad);
}

/* The window of this extractor's options and keys, on its state. */
static VmafMotionWindow motion_window_of(MotionState *s)
{
    const VmafMotionWindow window = {
        .sad_feature = "VMAF_integer_feature_motion_sad_score",
        .motion2_feature = "VMAF_integer_feature_motion2_score",
        .motion3_feature = "VMAF_integer_feature_motion3_score",
        .motion_blend_factor = s->motion_blend_factor,
        .motion_blend_offset = s->motion_blend_offset,
        .motion_max_val = s->motion_max_val,
        .motion_five_frame_window = s->motion_five_frame_window,
        .motion_moving_average = s->motion_moving_average,
        .state = &s->window_state,
    };
    return window;
}

/* The feature-name dictionary, built on first use. With worker threads the
 * engine advances and flushes the registered extractor, whose init() never
 * runs (ADR-2090; flush_non_temporal_cpu_extractors() in libvmaf.c). */
static int motion_ensure_dict(VmafFeatureExtractor *fex, MotionState *s)
{
    if (s->feature_name_dict)
        return 0;
    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    return s->feature_name_dict ? 0 : -ENOMEM;
}

/* ADR-2090: motion2 / motion3 of the frames whose window the SAD scores in
 * the collector complete. Called by the engine on the thread that feeds
 * frames, never next to extract() of the same instance or flush(). */
static int advance(VmafFeatureExtractor *fex, VmafFeatureCollector *feature_collector)
{
    MotionState *s = fex->priv;
    const int err = motion_ensure_dict(fex, s);
    if (err)
        return err;
    const VmafMotionWindow window = motion_window_of(s);
    return vmaf_motion_window_advance(feature_collector, s->feature_name_dict, &window);
}

static int flush(VmafFeatureExtractor *fex, VmafFeatureCollector *feature_collector)
{
    MotionState *s = fex->priv;

    const int dict_err = motion_ensure_dict(fex, s);
    if (dict_err)
        return dict_err;

    const VmafMotionWindow window = motion_window_of(s);
    const int err = vmaf_motion_window_flush(feature_collector, s->feature_name_dict, &window);
    if (err)
        return err;

    const int free_err = vmaf_dictionary_free(&s->feature_name_dict);
    return free_err ? free_err : 1;
}

/* ADR-1478: the reference picture of frame n-2 is read, and the context
 * keeps it, only while the five-frame window is on. */
static bool reads_prev_prev_ref(const VmafFeatureExtractor *fex)
{
    const MotionState *s = fex->priv;
    return s && s->motion_five_frame_window;
}

static const char *provided_features[] = {
    "VMAF_integer_feature_motion_sad_score",
    "VMAF_integer_feature_motion_score",
    "VMAF_integer_feature_motion2_score",
    "VMAF_integer_feature_motion3_score",
    NULL,
};

// NOLINTNEXTLINE(misc-use-internal-linkage): cross-TU registry pattern — external linkage required; referenced as extern VmafFeatureExtractor vmaf_fex_integer_motion by feature_extractor.cpp's feature_extractor_list[] (ADR-0278).
VmafFeatureExtractor vmaf_fex_integer_motion = {
    .name = "motion",
    .options = options,
    .init = init,
    .extract = extract,
    .flush = flush,
    .advance = advance,
    .close = close_fex,
    .priv_size = sizeof(MotionState),
    .provided_features = provided_features,
    .flags = VMAF_FEATURE_EXTRACTOR_PREV_REF,
    .reads_prev_prev_ref = reads_prev_prev_ref,
};

/* NOLINTEND(modernize-use-nullptr) */
