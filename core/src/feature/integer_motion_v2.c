/**
 *
 *  Copyright 2016-2025 Netflix, Inc.
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

// Pipelined motion feature extractor.
//
// Computes the same motion score as integer_motion.c but without storing
// blurred frames across extract calls. Instead, it exploits the linearity
// of convolution: SAD(blur[N-1], blur[N]) == sum(|blur(f[N-1] - f[N])|).
//
// The frame difference, blur, and absolute-sum are fused into a single
// row-at-a-time pipeline, requiring only one row of scratch memory.
//
// The framework provides the previous reference frame via fex->prev_ref,
// making each extract call stateless with respect to pixel data.

#include <errno.h>
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

typedef struct MotionV2State {
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
    VmafDictionary *feature_name_dict;
    VmafMotionWindowState window_state; /* ADR-2090: derivation of motion2_v2 / motion3_v2 */
} MotionV2State;

static const VmafOption options[] = {
    {
        .name = "motion_force_zero",
        .alias = "force_0",
        .help = "forces motion score to be 0",
        .offset = offsetof(MotionV2State, motion_force_zero),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "motion_blend_factor",
        .alias = "mbf",
        .help = "blend motion score given an offset",
        .offset = offsetof(MotionV2State, motion_blend_factor),
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
        .offset = offsetof(MotionV2State, motion_blend_offset),
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
        .offset = offsetof(MotionV2State, motion_fps_weight),
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
        .offset = offsetof(MotionV2State, motion_max_val),
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
        .offset = offsetof(MotionV2State, motion_five_frame_window),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "motion_moving_average",
        .alias = "mma",
        .help = "smooth motion3 with a 2-frame moving average",
        .offset = offsetof(MotionV2State, motion_moving_average),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {.name = NULL}};

static inline int mirror(int idx, int size)
{
    if (idx < 0) {
        return -idx;
    }
    if (idx >= size) {
        return 2 * size - idx - 2;
    }
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

static motion_pipeline_fn select_motion_pipeline(unsigned bpc)
{
    motion_pipeline_fn pipeline;
    if (bpc == 8) {
        pipeline = motion_score_pipeline_8;
    } else {
        pipeline = motion_score_pipeline_16;
    }

#if ARCH_X86
    if (vmaf_get_cpu_flags() & VMAF_X86_CPU_FLAG_AVX2) {
        if (bpc == 8) {
            pipeline = motion_score_pipeline_8_avx2;
        } else {
            pipeline = motion_score_pipeline_16_avx2;
        }
    }
#if HAVE_AVX512
    if (vmaf_get_cpu_flags() & VMAF_X86_CPU_FLAG_AVX512) {
        if (bpc == 8) {
            pipeline = motion_score_pipeline_8_avx512;
        } else {
            pipeline = motion_score_pipeline_16_avx512;
        }
    }
#endif
#endif
#if ARCH_AARCH64
    if (vmaf_get_cpu_flags() & VMAF_ARM_CPU_FLAG_NEON) {
        if (bpc == 8) {
            pipeline = motion_score_pipeline_8_neon;
        } else {
            pipeline = motion_score_pipeline_16_neon;
        }
    }
#endif

    return pipeline;
}

static int init(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc, unsigned w,
                unsigned h)
{
    (void)pix_fmt;
    MotionV2State *s = fex->priv;

    s->w = w;
    s->h = h;
    s->bpc = bpc;

    /* The 5-tap separable Gaussian uses reflect-101 mirror padding; the
     * same mirror() helper requires dim >= radius + 1 = 3 in both axes.
     * Reject smaller frames cleanly to prevent out-of-bounds reads. */
    const unsigned min_dim = (unsigned)(filter_width / 2 + 1); /* = 3 */
    if (h < min_dim || w < min_dim) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "motion_v2: frame %ux%u is below the 5-tap filter minimum %ux%u; "
                 "refusing to avoid out-of-bounds mirror reads\n",
                 w, h, min_dim, min_dim);
        return -EINVAL;
    }

    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (!s->feature_name_dict) {
        return -ENOMEM;
    }

    s->y_row = malloc(sizeof(*s->y_row) * w);
    if (!s->y_row) {
        return -ENOMEM;
    }

    s->pipeline = select_motion_pipeline(bpc);
    return 0;
}

static int extract(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                   VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index,
                   VmafFeatureCollector *feature_collector)
{
    MotionV2State *s = fex->priv;

    (void)dist_pic;
    (void)ref_pic_90;
    (void)dist_pic_90;

    if (s->motion_force_zero) {
        return vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                       "VMAF_integer_feature_motion_v2_sad_score",
                                                       0., index);
    }

    /* Netflix a2b59b77: with motion_five_frame_window the SAD of frame n is
     * taken against frame n-2 (fex->prev_prev_ref), so the first two frames
     * have none; otherwise against frame n-1. */
    const unsigned min_idx = s->motion_five_frame_window ? 2 : 1;
    if (index < min_idx) {
        return vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                       "VMAF_integer_feature_motion_v2_sad_score",
                                                       0., index);
    }

    const VmafPicture *prev = s->motion_five_frame_window ? &fex->prev_prev_ref : &fex->prev_ref;
    if (!prev->ref) {
        return -EINVAL;
    }

    const unsigned w = s->w;
    const unsigned h = s->h;
    const uint8_t *prev_data = (const uint8_t *)prev->data[0];
    const uint8_t *cur_data = (const uint8_t *)ref_pic->data[0];

    uint64_t sad = s->pipeline(prev_data, prev->stride[0], cur_data, ref_pic->stride[0], s->y_row,
                               w, h, s->bpc);

    double score = (double)sad / 256. / (w * h);

    return vmaf_feature_collector_append_with_dict(
        feature_collector, s->feature_name_dict, "VMAF_integer_feature_motion_v2_sad_score",
        MIN(score * s->motion_fps_weight, s->motion_max_val), index);
}

static int close_fex(VmafFeatureExtractor *fex)
{
    MotionV2State *s = fex->priv;
    free(s->y_row);
    return vmaf_dictionary_free(&s->feature_name_dict);
}

/*
 * motion2_v2 and motion3_v2 of every frame come from the stored SAD scores
 * through the window the `motion` extractor defines
 * (integer_motion.c::vmaf_motion_window_advance() / _flush(), motion_window.h):
 * upstream's last integer_motion_v2.c (Netflix a4a1492d^) and its
 * integer_motion.c derive them with the same arithmetic, the three-frame
 * window or, with motion_five_frame_window, the five-frame one (a2b59b77,
 * ADR-1478), each frame as soon as its window is complete (ADR-2090).
 */
static VmafMotionWindow motion_v2_window_of(MotionV2State *s)
{
    const VmafMotionWindow window = {
        .sad_feature = "VMAF_integer_feature_motion_v2_sad_score",
        .motion2_feature = "VMAF_integer_feature_motion2_v2_score",
        .motion3_feature = "VMAF_integer_feature_motion3_v2_score",
        .motion_blend_factor = s->motion_blend_factor,
        .motion_blend_offset = s->motion_blend_offset,
        .motion_max_val = s->motion_max_val,
        .motion_five_frame_window = s->motion_five_frame_window,
        .motion_moving_average = s->motion_moving_average,
        .state = &s->window_state,
    };
    return window;
}

/* ADR-2090: motion2_v2 / motion3_v2 of the frames whose window the SAD scores
 * in the collector complete, on the thread that feeds frames. With worker
 * threads this is the registered extractor, which init() never saw: the
 * dictionary is built here and freed by close_fex() (the engine marks the
 * context initialised). */
static int advance(VmafFeatureExtractor *fex, VmafFeatureCollector *feature_collector)
{
    MotionV2State *s = fex->priv;
    if (s->feature_name_dict == NULL) {
        s->feature_name_dict =
            vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
        if (!s->feature_name_dict) {
            return -ENOMEM;
        }
    }
    const VmafMotionWindow window = motion_v2_window_of(s);
    return vmaf_motion_window_advance(feature_collector, s->feature_name_dict, &window);
}

/*
 * The frames no advance() derived, the last one included.
 *
 * In the threaded dispatch path flush() is invoked on the *registered*
 * VmafFeatureExtractorContext rather than on any pool instance; that context
 * never ran init(). When no advance() built the dictionary either, this call
 * owns the one it builds and frees it before returning. When it did exist
 * (the serial or pool-instance path where extract() already ran, or an
 * advance()), close_fex() frees it as normal.
 */
static int flush(VmafFeatureExtractor *fex, VmafFeatureCollector *feature_collector)
{
    MotionV2State *s = fex->priv;
    const bool dict_locally_owned = (s->feature_name_dict == NULL);
    if (dict_locally_owned) {
        s->feature_name_dict =
            vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
        if (!s->feature_name_dict) {
            return -ENOMEM;
        }
    }

    const VmafMotionWindow window = motion_v2_window_of(s);
    const int err = vmaf_motion_window_flush(feature_collector, s->feature_name_dict, &window);
    const int free_err = dict_locally_owned ? vmaf_dictionary_free(&s->feature_name_dict) : 0;
    if (err) {
        return err;
    }
    return free_err ? free_err : 1;
}

/* ADR-1478: the reference picture of frame n-2 is read, and the context
 * keeps it, only while the five-frame window is on. */
static bool reads_prev_prev_ref(const VmafFeatureExtractor *fex)
{
    const MotionV2State *s = fex->priv;
    return s && s->motion_five_frame_window;
}

static const char *provided_features[] = {"VMAF_integer_feature_motion_v2_sad_score",
                                          "VMAF_integer_feature_motion2_v2_score",
                                          "VMAF_integer_feature_motion3_v2_score", NULL};

// NOLINTNEXTLINE(misc-use-internal-linkage): extern symbol referenced by feature_extractor.c registry — cross-TU rebase invariant (ADR-0278).
VmafFeatureExtractor vmaf_fex_integer_motion_v2 = {
    .name = "motion_v2",
    .options = options,
    .init = init,
    .extract = extract,
    .flush = flush,
    .advance = advance,
    .close = close_fex,
    .priv_size = sizeof(MotionV2State),
    .provided_features = provided_features,
    .flags = VMAF_FEATURE_EXTRACTOR_PREV_REF,
    .reads_prev_prev_ref = reads_prev_prev_ref,
};

/* NOLINTEND(modernize-use-nullptr) */
