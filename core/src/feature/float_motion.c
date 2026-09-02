/**
 *
 *  Copyright 2016-2026 Netflix, Inc.
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

#include <errno.h>
#include <math.h>
#include <string.h>
#include <stddef.h>

#include "cpu.h"
#include "common/convolution.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "log.h"
#include "mem.h"
#include "motion.h"
#include "motion_blend_tools.h"
#include "motion_tools.h"
#include "vif_tools.h"

#include "picture_copy.h"

#if ARCH_X86
#include "x86/float_motion_avx2.h"
#if HAVE_AVX512
#include "x86/float_motion_avx512.h"
#endif
#endif

#if ARCH_AARCH64
#include "arm64/float_motion_neon.h"
#endif

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is an
 * upstream-mirror file whose Netflix source spells the null pointer constant
 * `NULL` (every upstream sync would re-conflict against a keyword rewrite) and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

/* Default maximum value allowed for motion */
#define DEFAULT_MOTION_MAX_VAL (10000.0)

#define MIN(x, y) (((x) < (y)) ? (x) : (y))

/* Blur ring depth: frames index, index + 1 and index + 2 are kept modulo 3. */
#define MOTION_BLUR_RING 3

typedef float (*MotionSadLineFn)(const float *, const float *, int);

static float float_sad_line_c(const float *img1, const float *img2, int w)
{
    float accum = 0.0f;
    for (int j = 0; j < w; j++) {
        float diff = img1[j] - img2[j];
        accum += diff < 0 ? -diff : diff;
    }
    return accum;
}

/* Working set of one picture plane: the float copy of the source plane, the
 * separable-convolution scratch row block and the three-slot blur ring. */
typedef struct MotionPlane {
    float *ref;
    float *tmp;
    float *blur[MOTION_BLUR_RING];
} MotionPlane;

typedef struct MotionState {
    size_t float_stride;
    MotionPlane plane[3]; /* Y, U, V; U and V are allocated only with motion_add_uv */
    unsigned index;
    double score;
    bool debug;
    bool motion_add_scale1;
    bool motion_add_uv;
    bool motion_force_zero;
    double motion_fps_weight;
    double motion_blend_factor;
    double motion_blend_offset;
    int motion_filter_size;
    double motion_max_val;
    VmafDictionary *feature_name_dict;
    MotionSadLineFn sad_line;
} MotionState;

static const VmafOption options[] = {
    {
        .name = "debug",
        .help = "debug mode: enable additional output",
        .offset = offsetof(MotionState, debug),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = true,
    },
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
        .name = "motion_add_scale1",
        .alias = "mdc",
        .help = "add motion score from scale1",
        .offset = offsetof(MotionState, motion_add_scale1),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "motion_filter_size",
        .alias = "mfs",
        .help = "filtering size",
        .offset = offsetof(MotionState, motion_filter_size),
        .type = VMAF_OPT_TYPE_INT,
        .default_val.i = DEFAULT_MOTION_FILTER_SIZE,
        .min = 0,
        .max = 9,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "motion_add_uv",
        .alias = "mau",
        .help = "include U and V terms",
        .offset = offsetof(MotionState, motion_add_uv),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "motion_max_val",
        .help = "maximum value allowed; larger values will be clipped to this value",
        .offset = offsetof(MotionState, motion_max_val),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = DEFAULT_MOTION_MAX_VAL,
        .min = 0.0,
        .max = 10000.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "mmxv",
    },
    {0}};

/*
 * SIMD-dispatched SAD for the Y-plane default path (motion_add_scale1=false,
 * motion_add_uv=false).  Bit-identical to vmaf_image_sad_c with scale1=0
 * because float_sad_line accumulates sequentially.
 */
static double compute_motion_simd(const float *ref, const float *dis, int w, int h, int ref_stride,
                                  int dis_stride, MotionSadLineFn sad_line)
{
    int ref_px_stride = ref_stride / sizeof(float);
    int dis_px_stride = dis_stride / sizeof(float);
    float accum = 0.0f;

    for (int i = 0; i < h; i++) {
        accum +=
            sad_line(ref + (ptrdiff_t)i * ref_px_stride, dis + (ptrdiff_t)i * dis_px_stride, w);
    }

    return (double)(accum / (w * h));
}

/* Chroma plane heights for the motion_add_uv buffers; -EINVAL when the pixel
 * format carries no chroma planes. */
static int motion_chroma_heights(enum VmafPixelFormat pix_fmt, unsigned h, unsigned *h_u,
                                 unsigned *h_v)
{
    switch (pix_fmt) {
    case VMAF_PIX_FMT_YUV420P:
        *h_u = h / 2;
        *h_v = h / 2;
        return 0;
    case VMAF_PIX_FMT_YUV422P:
    case VMAF_PIX_FMT_YUV444P:
        *h_u = h;
        *h_v = h;
        return 0;
    case VMAF_PIX_FMT_UNKNOWN:
    case VMAF_PIX_FMT_YUV400P:
    default:
        return -EINVAL;
    }
}

static void motion_plane_free(MotionPlane *p)
{
    aligned_free(p->ref);
    aligned_free(p->tmp);
    for (unsigned k = 0; k < MOTION_BLUR_RING; k++) {
        aligned_free(p->blur[k]);
    }
    memset(p, 0, sizeof(*p));
}

static int motion_plane_alloc(MotionPlane *p, size_t float_stride, unsigned h)
{
    const size_t plane_sz = float_stride * h;
    p->ref = aligned_malloc(plane_sz, 32);
    p->tmp = aligned_malloc(plane_sz, 32);
    bool ok = p->ref != NULL && p->tmp != NULL;
    for (unsigned k = 0; k < MOTION_BLUR_RING; k++) {
        p->blur[k] = aligned_malloc(plane_sz, 32);
        ok = ok && p->blur[k] != NULL;
    }
    if (!ok) {
        motion_plane_free(p);
        return -ENOMEM;
    }
    return 0;
}

static void motion_free_planes(MotionState *s)
{
    motion_plane_free(&s->plane[0]);
    if (s->motion_add_uv) {
        motion_plane_free(&s->plane[1]);
        motion_plane_free(&s->plane[2]);
    }
}

/* The separable Gaussian convolution uses reflect-101 mirror padding.
 * For a filter of size N the bottom-edge formula requires
 * dim >= N/2 + 1 in each axis.  For the default 5-tap filter that is
 * 3; for the 3-tap option it is 2.  When motion_filter_size is 0 the
 * option has not been applied yet (e.g. in unit tests that manually
 * allocate priv) — treat it as the default.  Refuse smaller frames
 * up front to prevent out-of-bounds reads in the convolution kernel. */
static int motion_check_min_dim(const MotionState *s, unsigned w, unsigned h)
{
    const int configured =
        s->motion_filter_size > 0 ? s->motion_filter_size : DEFAULT_MOTION_FILTER_SIZE;
    const unsigned effective_filter_size = (unsigned)configured;
    if (effective_filter_size > 1u) {
        const unsigned min_dim = effective_filter_size / 2u + 1u;
        if (h < min_dim || w < min_dim) {
            vmaf_log(VMAF_LOG_LEVEL_ERROR,
                     "float_motion: frame %ux%u is below the %u-tap filter minimum %ux%u; "
                     "refusing to avoid out-of-bounds mirror reads\n",
                     w, h, effective_filter_size, min_dim, min_dim);
            return -EINVAL;
        }
    }
    return 0;
}

static MotionSadLineFn motion_select_sad_line(void)
{
    MotionSadLineFn sad_line = float_sad_line_c;
#if ARCH_X86
    const unsigned flags = vmaf_get_cpu_flags();
    if (flags & VMAF_X86_CPU_FLAG_AVX2) {
        sad_line = float_sad_line_avx2;
    }
#if HAVE_AVX512
    if (flags & VMAF_X86_CPU_FLAG_AVX512) {
        sad_line = float_sad_line_avx512;
    }
#endif
#elif ARCH_AARCH64
    const unsigned flags = vmaf_get_cpu_flags();
    if (flags & VMAF_ARM_CPU_FLAG_NEON) {
        sad_line = float_sad_line_neon;
    }
#endif
    return sad_line;
}

static int init(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc, unsigned w,
                unsigned h)
{
    (void)bpc;
    MotionState *s = fex->priv;
    unsigned h_u = 0;
    unsigned h_v = 0;

    int err = motion_check_min_dim(s, w, h);
    if (err) {
        return err;
    }
    if (s->motion_add_uv) {
        err = motion_chroma_heights(pix_fmt, h, &h_u, &h_v);
        if (err) {
            return err;
        }
    }

    s->float_stride = ALIGN_CEIL(w * sizeof(float));
    err = motion_plane_alloc(&s->plane[0], s->float_stride, h);
    if (!err && s->motion_add_uv) {
        err = motion_plane_alloc(&s->plane[1], s->float_stride, h_u);
    }
    if (!err && s->motion_add_uv) {
        err = motion_plane_alloc(&s->plane[2], s->float_stride, h_v);
    }
    if (err) {
        motion_free_planes(s);
        return err;
    }

    if (s->motion_force_zero) {
        fex->flush = NULL;
    }
    s->score = 0;
    s->sad_line = motion_select_sad_line();

    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (!s->feature_name_dict) {
        motion_free_planes(s);
        return -ENOMEM;
    }

    return 0;
}

static int motion_append(const MotionState *s, VmafFeatureCollector *feature_collector,
                         const char *name, double score, unsigned index)
{
    return vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict, name,
                                                   score, index);
}

/* motion_fps_weight scaling followed by the motion_max_val clip (motion / motion2). */
static double motion_clip(const MotionState *s, double score)
{
    return MIN(score * s->motion_fps_weight, s->motion_max_val);
}

/* Blended score (motion3) followed by the motion_max_val clip. */
static double motion_blend_clip(const MotionState *s, double score)
{
    return MIN(
        motion_blend(score * s->motion_fps_weight, s->motion_blend_factor, s->motion_blend_offset),
        s->motion_max_val);
}

/* cppcheck-suppress constParameterCallback ; prototype is fixed by the VmafFeatureExtractor.flush
 * callback type in feature_extractor.h — only fex->priv is read here */
static int flush(VmafFeatureExtractor *fex, VmafFeatureCollector *feature_collector)
{
    const MotionState *s = fex->priv;
    int ret = 0;

    if (s->index > 0) {
        ret = motion_append(s, feature_collector, "VMAF_feature_motion2_score",
                            motion_clip(s, s->score), s->index);
        ret |= motion_append(s, feature_collector, "VMAF_feature_motion3_score",
                             motion_blend_clip(s, s->score), s->index);
    } else {
        ret |= motion_append(s, feature_collector, "VMAF_feature_motion3_score", 0, s->index);
    }

    return (ret < 0) ? ret : !ret;
}

static int motion_append_forced_zero(const MotionState *s, VmafFeatureCollector *feature_collector,
                                     unsigned index)
{
    int err = motion_append(s, feature_collector, "VMAF_feature_motion2_score", 0., index);
    err |= motion_append(s, feature_collector, "VMAF_feature_motion3_score", 0., index);
    if (s->debug) {
        err |= motion_append(s, feature_collector, "VMAF_feature_motion_score", 0., index);
    }
    return err;
}

/* Blur one plane of the current frame into blur ring slot blur_idx. */
static void motion_blur_plane(const MotionState *s, const MotionPlane *p, unsigned w, unsigned h,
                              unsigned blur_idx)
{
    const int px_stride = (int)(s->float_stride / sizeof(float));
    const float *filter = FILTER_5_s;
    int filter_size = 5;
    if (s->motion_filter_size == 1) {
        filter = FILTER_5_NO_OP_s;
    } else if (s->motion_filter_size == 3) {
        filter = FILTER_3_s;
        filter_size = 3;
    }
    convolution_f32_c_s(filter, filter_size, p->ref, p->blur[blur_idx], p->tmp, (int)w, (int)h,
                        px_stride, px_stride);
}

static void motion_copy_and_blur(MotionState *s, VmafPicture *ref_pic, unsigned blur_idx)
{
    const unsigned n_planes = s->motion_add_uv ? 3 : 1;
    for (unsigned c = 0; c < n_planes; c++) {
        picture_copy(s->plane[c].ref, (ptrdiff_t)s->float_stride, ref_pic, -128, ref_pic->bpc,
                     (int)c);
        motion_blur_plane(s, &s->plane[c], ref_pic->w[c], ref_pic->h[c], blur_idx);
    }
}

/*
 * Motion between blur ring slots idx_a and idx_b.  Y-plane: use the
 * SIMD-dispatched path when scale1 is disabled (default).  Falls back to
 * compute_motion() when motion_add_scale1=true since the vif_scale downscale
 * logic lives there, and adds the U and V terms with motion_add_uv.
 */
static int motion_score_pair(const MotionState *s, const VmafPicture *ref_pic, unsigned idx_a,
                             unsigned idx_b, double *score)
{
    const int stride = (int)s->float_stride;
    if (!s->motion_add_scale1 && !s->motion_add_uv) {
        *score = compute_motion_simd(s->plane[0].blur[idx_a], s->plane[0].blur[idx_b],
                                     (int)ref_pic->w[0], (int)ref_pic->h[0], stride, stride,
                                     s->sad_line);
        return 0;
    }
    int err = compute_motion(s->plane[0].blur[idx_a], s->plane[0].blur[idx_b], (int)ref_pic->w[0],
                             (int)ref_pic->h[0], stride, stride, score, s->motion_add_scale1);
    if (err) {
        return err;
    }
    if (s->motion_add_uv) {
        for (unsigned c = 1; c < 3; c++) {
            double score_c = 0.0;
            err |=
                compute_motion(s->plane[c].blur[idx_a], s->plane[c].blur[idx_b], (int)ref_pic->w[c],
                               (int)ref_pic->h[c], stride, stride, &score_c, s->motion_add_scale1);
            *score += score_c;
        }
    }
    return err;
}

static int extract(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                   VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index,
                   VmafFeatureCollector *feature_collector)
{
    MotionState *s = fex->priv;

    (void)dist_pic;
    (void)ref_pic_90;
    (void)dist_pic_90;

    if (s->motion_force_zero) {
        return motion_append_forced_zero(s, feature_collector, index);
    }

    s->index = index;
    const unsigned blur_idx_0 = (index + 0) % 3;
    const unsigned blur_idx_1 = (index + 1) % 3;
    const unsigned blur_idx_2 = (index + 2) % 3;

    motion_copy_and_blur(s, ref_pic, blur_idx_0);

    int err = 0;
    if (index == 0) {
        err = motion_append(s, feature_collector, "VMAF_feature_motion2_score", 0., index);
        if (s->debug) {
            err |= motion_append(s, feature_collector, "VMAF_feature_motion_score", 0., index);
        }
        return err;
    }

    double score = 0.0;
    err = motion_score_pair(s, ref_pic, blur_idx_2, blur_idx_0, &score);
    if (err) {
        return err;
    }
    if (s->debug) {
        err |= motion_append(s, feature_collector, "VMAF_feature_motion_score",
                             motion_clip(s, score), index);
    }
    if (err) {
        return err;
    }
    s->score = score;

    if (index == 1) {
        err |= motion_append(s, feature_collector, "VMAF_feature_motion3_score",
                             motion_blend_clip(s, score), 0);
        return err;
    }

    double score2 = 0.0;
    err = motion_score_pair(s, ref_pic, blur_idx_2, blur_idx_1, &score2);
    if (err) {
        return err;
    }

    score2 = score2 < score ? score2 : score;
    err = motion_append(s, feature_collector, "VMAF_feature_motion2_score", motion_clip(s, score2),
                        index - 1);
    err |= motion_append(s, feature_collector, "VMAF_feature_motion3_score",
                         motion_blend_clip(s, score2), index - 1);
    return err;
}

static int close(VmafFeatureExtractor *fex)
{
    MotionState *s = fex->priv;
    motion_free_planes(s);
    vmaf_dictionary_free(&s->feature_name_dict);
    return 0;
}

static const char *provided_features[] = {"VMAF_feature_motion_score", "VMAF_feature_motion2_score",
                                          "VMAF_feature_motion3_score", NULL};

// NOLINTNEXTLINE(misc-use-internal-linkage): cross-TU registry pattern — external linkage required; referenced as `extern VmafFeatureExtractor vmaf_fex_float_motion` by feature_extractor.cpp's feature_extractor_list[] (ADR-0278).
VmafFeatureExtractor vmaf_fex_float_motion = {
    .name = "float_motion",
    .init = init,
    .extract = extract,
    .options = options,
    .flush = flush,
    .close = close,
    .priv_size = sizeof(MotionState),
    .provided_features = provided_features,
    .flags = VMAF_FEATURE_EXTRACTOR_TEMPORAL,
};

/* NOLINTEND(modernize-use-nullptr) */
