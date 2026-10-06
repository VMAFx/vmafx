/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  float_motion feature extractor on the Metal backend (T8-1h / ADR-0421).
 *  Dispatches the kernels of float_motion.metal; the design is
 *  float_motion_hip's (ADR-1404, ADR-1419), the arithmetic is
 *  metal_float_motion_math.h's (ADR-1498).
 *
 *  Temporal design: per plane (Y, and U and V with motion_add_uv), a
 *  `blur[2]` ping-pong of float MTLBuffers holds the blurred current and
 *  previous frames and `ref_in` the raw plane. On each submit the blur kernel
 *  writes the current slot and, from the second frame on, |cur - prev| of
 *  every sample, transposed; the row kernel adds the differences of every
 *  row into one fp32 sum, and the row sums of every plane and scale land in
 *  the read-back `rb`.
 *
 *  The sum is the CPU's (ADR-1409): float_motion.c adds the absolute
 *  differences of a row into one fp32 accumulator, the rows into another, and
 *  divides in fp32, and that order decides the low bits of the score. The
 *  host finishes each plane through vmaf_mtl_fm_plane_score()
 *  (vmaf_float_motion_score_from_row_sads(), feature/float_motion_sad.h) and
 *  adds the planes in double, as float_motion.c::motion_score_pair() does.
 *  `motion_filter_size` selects the blur filter in the kernel;
 *  `motion_add_scale1` adds the SAD of both blurred frames scaled to half
 *  size (a second kernel).
 *
 *  Scores, at the CPU float_motion.c's indices (ADR-1404); every motion and
 *  motion2 value goes through motion_clip() (the fps weight, then the
 *  `motion_max_val` cap), the debug score included:
 *  VMAF_feature_motion_score at `index` (debug);
 *  VMAF_feature_motion2_score = min(prev, cur) at `index - 1`; and
 *  VMAF_feature_motion3_score, motion_blend_clip() of the same value (fps
 *  weight, the `motion_blend_factor` / `motion_blend_offset` blend of
 *  motion_blend_tools.h, the cap): frame 0 from the first SAD, then the blended
 *  motion2. flush() emits the tail motion2 / motion3 of the last SAD, and
 *  motion3 = 0 for a one-frame run.
 */

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>

/* The CPU's blend (motion_blend()). Ahead of Foundation: its header defines
 * MIN as well, and only when MIN is not defined yet, so this order keeps one
 * definition and no redefinition warning. */
extern "C" {
#include "motion_blend_tools.h"
}

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

/* feature_extractor.h uses `#if defined(__cplusplus)` to include <atomic>
 * (Xcode 16.4 / macOS 15 libc++ emits "templates must have C++ linkage"
 * when that header is pulled into an extern "C" block — ADR-fix macOS-Metal). */
#include "feature_extractor.h"

extern "C" {
#include "dict.h"
#include "feature_collector.h"
#include "feature_name.h"
#include "libvmaf/picture.h"
#include "log.h"
#include "motion_tools.h"
#include "picture_geometry.h"

#include "../../metal/common.h"
#include "../../metal/kernel_template.h"
}

#include "../../metal/objc_handle.h"

#include "metal_float_motion_math.h"


/* Y, and U and V with motion_add_uv. */
#define FMM_MAX_PLANES 3u

/* CPU float_motion.c DEFAULT_MOTION_MAX_VAL. */
#define FMM_DEFAULT_MAX_VAL (10000.0)

namespace {

/* One picture plane: its geometry, where its row sums sit in the read-back,
 * and its buffers. */
typedef struct FmPlaneMetal {
    unsigned w;
    unsigned h;
    /* Half-size plane of the scale-1 SAD (motion_add_scale1). */
    unsigned sw;
    unsigned sh;
    /* Rows of the scale-1 SAD in the read-back: `sh` with motion_add_scale1,
     * 0 without. The scale-0 SAD has `h` rows. */
    unsigned rows1;
    /* Index of the first scale-0 and of the first scale-1 row sum. */
    unsigned off0;
    unsigned off1;
    /* __bridge_retained id<MTLBuffer> handles: this frame's raw plane
     * (packed rows), the blurred ping-pong (float, w * h each) and the
     * transposed |cur - prev| planes of scale 0 and, with
     * motion_add_scale1, scale 1. */
    void *ref_in;
    void *blur[2];
    void *diff[2];
} FmPlaneMetal;

typedef struct FloatMotionStateMetal {
    VmafMetalKernelLifecycle lc;
    VmafMetalKernelBuffer rb;        /* float row sums of every plane and scale */
    VmafMetalContext *ctx;
    void *pso_blur;
    void *pso_scale1;
    void *pso_row_sum;

    FmPlaneMetal plane[FMM_MAX_PLANES];
    unsigned n_planes;
    /* Floats in the read-back: the row sums of every plane and scale. */
    unsigned row_count;

    int cur_blur;
    double prev_motion_score;
    double motion_fps_weight;
    double motion_blend_factor;
    double motion_blend_offset;
    double motion_max_val;
    int motion_filter_size;
    unsigned frame_index;
    unsigned bpc;
    bool debug;
    bool motion_force_zero;
    bool motion_add_scale1;
    bool motion_add_uv;

    VmafDictionary *feature_name_dict;
} FloatMotionStateMetal;
} // namespace

namespace {

/* The CPU float_motion.c table: same names, aliases, defaults, ranges and
 * order. The order spells the feature names. */
static const VmafOption options[] = {
    {
        .name        = "debug",
        .help        = "debug mode: enable additional output",
        .offset      = offsetof(FloatMotionStateMetal, debug),
        .type        = VMAF_OPT_TYPE_BOOL,
        .default_val = {.b = true},
    },
    {
        .name        = "motion_force_zero",
        .help        = "forcing motion score to zero",
        .alias       = "force_0",
        .offset      = offsetof(FloatMotionStateMetal, motion_force_zero),
        .type        = VMAF_OPT_TYPE_BOOL,
        .default_val = {.b = false},
        .flags       = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name        = "motion_fps_weight",
        .help        = "fps-aware multiplicative weight/correction",
        .alias       = "mfw",
        .offset      = offsetof(FloatMotionStateMetal, motion_fps_weight),
        .type        = VMAF_OPT_TYPE_DOUBLE,
        .default_val = {.d = 1.0},
        .min         = 0.0,
        .max         = 5.0,
        .flags       = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name        = "motion_blend_factor",
        .help        = "blend motion score given an offset",
        .alias       = "mbf",
        .offset      = offsetof(FloatMotionStateMetal, motion_blend_factor),
        .type        = VMAF_OPT_TYPE_DOUBLE,
        .default_val = {.d = 1.0},
        .min         = 0.0,
        .max         = 1.0,
        .flags       = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name        = "motion_blend_offset",
        .help        = "blend motion score starting from this offset",
        .alias       = "mbo",
        .offset      = offsetof(FloatMotionStateMetal, motion_blend_offset),
        .type        = VMAF_OPT_TYPE_DOUBLE,
        .default_val = {.d = 40.0},
        .min         = 0.0,
        .max         = 1000.0,
        .flags       = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name        = "motion_add_scale1",
        .help        = "add motion score from scale1",
        .alias       = "mdc",
        .offset      = offsetof(FloatMotionStateMetal, motion_add_scale1),
        .type        = VMAF_OPT_TYPE_BOOL,
        .default_val = {.b = false},
        .flags       = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name        = "motion_filter_size",
        .help        = "filtering size",
        .alias       = "mfs",
        .offset      = offsetof(FloatMotionStateMetal, motion_filter_size),
        .type        = VMAF_OPT_TYPE_INT,
        .default_val = {.i = DEFAULT_MOTION_FILTER_SIZE},
        .min         = 0,
        .max         = 9,
        .flags       = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name        = "motion_add_uv",
        .help        = "include U and V terms",
        .alias       = "mau",
        .offset      = offsetof(FloatMotionStateMetal, motion_add_uv),
        .type        = VMAF_OPT_TYPE_BOOL,
        .default_val = {.b = false},
        .flags       = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name        = "motion_max_val",
        .help        = "maximum value allowed; larger values will be clipped to this value",
        .alias       = "mmxv",
        .offset      = offsetof(FloatMotionStateMetal, motion_max_val),
        .type        = VMAF_OPT_TYPE_DOUBLE,
        .default_val = {.d = FMM_DEFAULT_MAX_VAL},
        .min         = 0.0,
        .max         = 10000.0,
        .flags       = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {0},
};

int fm_metal_append(const FloatMotionStateMetal *s, VmafFeatureCollector *feature_collector,
                           const char *name, double score, unsigned index)
{
    return vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict, name,
                                                   score, index);
}

/* CPU float_motion.c::motion_clip (motion and motion2): the fps weight, then
 * the motion_max_val cap. */
double fm_metal_motion_clip(const FloatMotionStateMetal *s, double score)
{
    return MIN(score * s->motion_fps_weight, s->motion_max_val);
}

/* CPU float_motion.c::motion_blend_clip (motion3): the fps weight, the blend
 * of motion_blend_tools.h, then the motion_max_val cap. */
double fm_metal_motion_blend_clip(const FloatMotionStateMetal *s, double score)
{
    return MIN(motion_blend(score * s->motion_fps_weight, s->motion_blend_factor,
                            s->motion_blend_offset),
               s->motion_max_val);
}

size_t fm_metal_bytes_per_sample(const FloatMotionStateMetal *s)
{
    return (s->bpc <= 8u) ? 1u : 2u;
}

/* ------------------------------------------------------------------ */
/* Geometry                                                           */
/* ------------------------------------------------------------------ */

/* The blur needs filter_size / 2 + 1 samples on each axis of every plane it
 * runs on: CPU float_motion.c::motion_check_min_dim(), where only
 * motion_filter_size == 3 narrows the filter. The kernels index a plane's
 * transposed differences in 32 bits, which also bounds the pixel count
 * vmaf_float_motion_score_from_row_sads() divides by (an int on the CPU). */
int fm_metal_check_plane(const FloatMotionStateMetal *s, const FmPlaneMetal *p,
                                const char *name)
{
    const unsigned taps = (s->motion_filter_size == 3) ? 3u : 5u;
    const unsigned min_dim = taps / 2u + 1u;
    if (p->w < min_dim || p->h < min_dim) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "float_motion_metal: %s plane %ux%u is below the %u-tap filter minimum %ux%u\n",
                 name, p->w, p->h, taps, min_dim, min_dim);
        return -EINVAL;
    }
    if (vmaf_mtl_fm_diff_count(p->w, p->h) > (uint64_t)INT32_MAX) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "float_motion_metal: %s plane %ux%u is too large\n", name,
                 p->w, p->h);
        return -EINVAL;
    }
    return 0;
}

/* Geometry and read-back offsets of one plane; returns the index after its
 * last row sum. */
unsigned fm_metal_plane_layout(const FloatMotionStateMetal *s, FmPlaneMetal *p, unsigned w,
                                      unsigned h, unsigned offset)
{
    p->w = w;
    p->h = h;
    p->sw = vmaf_mtl_fm_scaled_extent(w);
    p->sh = vmaf_mtl_fm_scaled_extent(h);
    p->rows1 = s->motion_add_scale1 ? p->sh : 0u;
    p->off0 = offset;
    p->off1 = offset + h;
    return p->off1 + p->rows1;
}

/* Geometry of every plane the options ask for, checked as the CPU checks it
 * (motion_check_min_dim_all_planes()). Touches no device object. */
int fm_metal_init_geometry(FloatMotionStateMetal *s, enum VmafPixelFormat pix_fmt,
                                  unsigned w, unsigned h)
{
    s->n_planes = 1u;
    s->row_count = fm_metal_plane_layout(s, &s->plane[0], w, h, 0u);
    int const err = fm_metal_check_plane(s, &s->plane[0], "luma");
    if (err != 0 || !s->motion_add_uv) {
        return err;
    }
    if (pix_fmt != VMAF_PIX_FMT_YUV420P && pix_fmt != VMAF_PIX_FMT_YUV422P &&
        pix_fmt != VMAF_PIX_FMT_YUV444P) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "float_motion_metal: motion_add_uv needs a pixel format with chroma planes\n");
        return -EINVAL;
    }
    const unsigned cw = vmaf_chroma_extent(w, pix_fmt != VMAF_PIX_FMT_YUV444P);
    const unsigned ch = vmaf_chroma_extent(h, pix_fmt == VMAF_PIX_FMT_YUV420P);
    for (unsigned c = 1u; c < FMM_MAX_PLANES; c++) {
        s->row_count = fm_metal_plane_layout(s, &s->plane[c], cw, ch, s->row_count);
    }
    s->n_planes = FMM_MAX_PLANES;
    return fm_metal_check_plane(s, &s->plane[1], "chroma");
}

/* ------------------------------------------------------------------ */
/* Device objects                                                     */
/* ------------------------------------------------------------------ */

/* One compute pipeline of the embedded library; refuses a pipeline that
 * cannot run `threads` threads per threadgroup. */
int fm_metal_pipeline(id<MTLDevice> device, id<MTLLibrary> lib, NSString *name,
                             NSUInteger threads, void **out)
{
    id<MTLFunction> const fn = [lib newFunctionWithName:name];
    if (fn == nil) {
        return -ENODEV;
    }
    NSError *err = nil;
    id<MTLComputePipelineState> const pso = [device newComputePipelineStateWithFunction:fn error:&err];
    if (pso == nil || pso.maxTotalThreadsPerThreadgroup < threads) {
        return -ENODEV;
    }
    *out = (__bridge_retained void *)pso;
    return 0;
}

int build_pipelines(FloatMotionStateMetal *s, id<MTLDevice> device)
{
    int load_rc = 0;
    id<MTLLibrary> const lib = vmaf_metal_library_load(device, &load_rc);
    if (lib == nil) { return load_rc; }

    const NSUInteger block = (NSUInteger)VMAF_MTL_FM_BLOCK * VMAF_MTL_FM_BLOCK;
    int rc = fm_metal_pipeline(device, lib, @"float_motion_blur", block, &s->pso_blur);
    if (rc == 0) {
        rc = fm_metal_pipeline(device, lib, @"float_motion_scale1_diff", block, &s->pso_scale1);
    }
    if (rc == 0) {
        rc = fm_metal_pipeline(device, lib, @"float_motion_row_sum", VMAF_MTL_FM_ROW_GROUP,
                               &s->pso_row_sum);
    }
    return rc;
}

int fm_metal_new_buffer(id<MTLDevice> device, size_t bytes, void **out)
{
    id<MTLBuffer> const buf = [device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    if (buf == nil) {
        return -ENOMEM;
    }
    *out = (__bridge_retained void *)buf;
    return 0;
}

/* The raw plane, the blur ping-pong and the difference planes of one plane. */
int fm_metal_plane_alloc(const FloatMotionStateMetal *s, FmPlaneMetal *p,
                                id<MTLDevice> device)
{
    const size_t pixels = (size_t)p->w * p->h;
    int err = fm_metal_new_buffer(device, pixels * fm_metal_bytes_per_sample(s), &p->ref_in);
    if (err == 0) {
        err = fm_metal_new_buffer(device, pixels * sizeof(float), &p->blur[0]);
    }
    if (err == 0) {
        err = fm_metal_new_buffer(device, pixels * sizeof(float), &p->blur[1]);
    }
    if (err == 0) {
        const size_t floats = (size_t)vmaf_mtl_fm_diff_count(p->w, p->h);
        err = fm_metal_new_buffer(device, floats * sizeof(float), &p->diff[0]);
    }
    if (err == 0 && p->rows1 != 0u) {
        const size_t floats = (size_t)vmaf_mtl_fm_diff_count(p->sw, p->sh);
        err = fm_metal_new_buffer(device, floats * sizeof(float), &p->diff[1]);
    }
    return err;
}

/* Releases one __bridge_retained handle; safe on NULL. */
void fm_metal_drop(void **slot)
{
    if (*slot != nullptr) {
        (void)(__bridge_transfer id)(*slot);
        *slot = nullptr;
    }
}

void fm_metal_drop_objects(FloatMotionStateMetal *s)
{
    for (auto & c : s->plane) {
        FmPlaneMetal *p = &c;
        fm_metal_drop(&p->diff[1]);
        fm_metal_drop(&p->diff[0]);
        fm_metal_drop(&p->blur[1]);
        fm_metal_drop(&p->blur[0]);
        fm_metal_drop(&p->ref_in);
    }
    fm_metal_drop(&s->pso_row_sum);
    fm_metal_drop(&s->pso_scale1);
    fm_metal_drop(&s->pso_blur);
}

int fm_metal_release_device(FloatMotionStateMetal *s)
{
    int rc = vmaf_metal_kernel_lifecycle_close(&s->lc, s->ctx);
    fm_metal_drop_objects(s);

    int const err = vmaf_metal_kernel_buffer_free(&s->rb, s->ctx);
    if (err != 0 && rc == 0) {
        rc = err;
    }
    if (s->ctx) {
        vmaf_metal_context_destroy(s->ctx);
        s->ctx = nullptr;
    }
    return rc;
}

int fm_metal_release(FloatMotionStateMetal *s)
{
    int rc = fm_metal_release_device(s);
    if (s->feature_name_dict != nullptr) {
        int const err = vmaf_dictionary_free(&s->feature_name_dict);
        if (err != 0 && rc == 0) {
            rc = err;
        }
    }
    return rc;
}

int close_fex_metal(VmafFeatureExtractor *fex)
{
    FloatMotionStateMetal *s = (FloatMotionStateMetal *)fex->priv;
    return fm_metal_release(s);
}

/* ------------------------------------------------------------------ */
/* motion_force_zero                                                  */
/* ------------------------------------------------------------------ */

int extract_force_zero_metal(VmafFeatureExtractor *fex, VmafPicture *ref_pic,
                                    VmafPicture *ref_pic_90, VmafPicture *dist_pic,
                                    VmafPicture *dist_pic_90, unsigned index,
                                    VmafFeatureCollector *feature_collector)
{
    (void)ref_pic;
    (void)ref_pic_90;
    (void)dist_pic;
    (void)dist_pic_90;
    const FloatMotionStateMetal *const s = (FloatMotionStateMetal *)fex->priv;

    /* CPU float_motion.c::motion_append_forced_zero: every output 0. */
    int err = fm_metal_append(s, feature_collector, "VMAF_feature_motion2_score", 0.0, index);
    if (err == 0) {
        err = fm_metal_append(s, feature_collector, "VMAF_feature_motion3_score", 0.0, index);
    }
    if (s->debug && err == 0) {
        err = fm_metal_append(s, feature_collector, "VMAF_feature_motion_score", 0.0, index);
    }
    return err;
}

int init_force_zero_metal(VmafFeatureExtractor *fex, FloatMotionStateMetal *s)
{
    fex->extract = extract_force_zero_metal;
    fex->submit = NULL;
    fex->collect = NULL;
    fex->flush = NULL;
    fex->close = close_fex_metal;
    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features,
                                                      fex->options, s);
    if (s->feature_name_dict == nullptr) {
        return -ENOMEM;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* init                                                               */
/* ------------------------------------------------------------------ */

/* Device half of init: read-back, pipelines, buffers, name dictionary. */
int fm_metal_init_device(VmafFeatureExtractor *fex, FloatMotionStateMetal *s)
{
    int err = vmaf_metal_kernel_buffer_alloc(&s->rb, s->ctx, (size_t)s->row_count * sizeof(float));
    if (err != 0) { return err; }
    if (s->rb.host_view == nullptr) { return -ENOMEM; }

    const void *const dh = vmaf_metal_context_device_handle(s->ctx);
    if (dh == nullptr) { return -ENODEV; }
    id<MTLDevice> const device = (__bridge id<MTLDevice>)dh;

    err = build_pipelines(s, device);
    for (unsigned c = 0u; c < s->n_planes && err == 0; c++) {
        err = fm_metal_plane_alloc(s, &s->plane[c], device);
    }
    if (err != 0) { return err; }

    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features,
                                                      fex->options, s);
    if (s->feature_name_dict == nullptr) { return -ENOMEM; }
    return 0;
}

int init_fex_metal(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt,
                          unsigned bpc, unsigned w, unsigned h)
{
    FloatMotionStateMetal *s = (FloatMotionStateMetal *)fex->priv;

    s->bpc               = bpc;
    s->frame_index       = 0;
    s->prev_motion_score = 0.0;
    s->cur_blur          = 0;

    /* The blur mirrors past the plane (reflect-101); refuse planes below the
     * filter's minimum up front, as the CPU does. The kernel's tile indices
     * are folded into the plane for every size (float_motion.metal), so this
     * guard is the CPU's contract, not what keeps the loads in bounds. */
    int err = fm_metal_init_geometry(s, pix_fmt, w, h);
    if (err != 0) {
        return err;
    }

    err = vmaf_metal_context_new(&s->ctx, 0);
    if (err == 0) {
        err = vmaf_metal_kernel_lifecycle_init(&s->lc, s->ctx);
    }
    if (err == 0 && s->motion_force_zero) {
        /* extract_force_zero_metal needs the name dictionary, not the device
         * objects; close_fex_metal frees the dictionary later. */
        err = init_force_zero_metal(fex, s);
        const int released = fm_metal_release_device(s);
        return (err != 0) ? err : released;
    }
    if (err == 0) {
        err = fm_metal_init_device(fex, s);
    }
    if (err != 0 && fm_metal_release(s) != 0) {
        vmaf_log(VMAF_LOG_LEVEL_WARNING,
                 "float_motion_metal: release after a failed init failed\n");
    }
    return err;
}

/* ------------------------------------------------------------------ */
/* submit                                                             */
/* ------------------------------------------------------------------ */

/* Copies every plane the extractor reads into its packed `ref_in`. */
int fm_metal_upload(const FloatMotionStateMetal *s, const VmafPicture *pic)
{
    const size_t bps = fm_metal_bytes_per_sample(s);
    for (unsigned c = 0u; c < s->n_planes && c < FMM_MAX_PLANES; c++) {
        const FmPlaneMetal *p = &s->plane[c];
        if (pic->w[c] != p->w || pic->h[c] != p->h) {
            return -EINVAL;
        }
        uint8_t *dst = (uint8_t *)[(__bridge id<MTLBuffer>)p->ref_in contents];
        const uint8_t *src = (const uint8_t *)pic->data[c];
        const size_t row_bytes = (size_t)p->w * bps;
        for (unsigned y = 0u; y < p->h; y++) {
            memcpy(dst + (size_t)y * row_bytes, src + (ptrdiff_t)y * pic->stride[c], row_bytes);
        }
    }
    return 0;
}

MTLSize fm_metal_groups(unsigned w, unsigned h)
{
    return MTLSizeMake((w + VMAF_MTL_FM_BLOCK - 1u) / VMAF_MTL_FM_BLOCK,
                       (h + VMAF_MTL_FM_BLOCK - 1u) / VMAF_MTL_FM_BLOCK, 1);
}

/* Blur kernel of plane `p`: blurs the raw plane into the current ping-pong
 * slot and, when `compute_sad` is set, stores |cur - prev| of every sample in
 * the plane's scale-0 differences. */
int fm_metal_encode_blur(const FloatMotionStateMetal *s, id<MTLCommandBuffer> cmd,
                                const FmPlaneMetal *p, unsigned compute_sad)
{
    id<MTLComputeCommandEncoder> const enc = [cmd computeCommandEncoder];
    if (enc == nil) {
        return -ENOMEM;
    }
    const VmafMtlFmBlurArgs args = {.width=p->w, .height=p->h, .bpc=s->bpc, .filter_size=(vmaf_mtl_u32)s->motion_filter_size,
                                    .compute_sad=compute_sad};
    [enc setComputePipelineState:(__bridge id<MTLComputePipelineState>)s->pso_blur];
    [enc setBuffer:(__bridge id<MTLBuffer>)p->ref_in offset:0 atIndex:0];
    [enc setBuffer:(__bridge id<MTLBuffer>)p->blur[s->cur_blur] offset:0 atIndex:1];
    [enc setBuffer:(__bridge id<MTLBuffer>)p->blur[1 - s->cur_blur] offset:0 atIndex:2];
    [enc setBuffer:(__bridge id<MTLBuffer>)p->diff[0] offset:0 atIndex:3];
    [enc setBytes:&args length:sizeof(args) atIndex:4];
    [enc dispatchThreadgroups:fm_metal_groups(p->w, p->h)
        threadsPerThreadgroup:MTLSizeMake(VMAF_MTL_FM_BLOCK, VMAF_MTL_FM_BLOCK, 1)];
    [enc endEncoding];
    return 0;
}

/* Scale-1 difference kernel of plane `p`, after its blur kernel: scales both
 * ping-pong slots to half size and stores |cur - prev| of every half-size
 * sample in the plane's scale-1 differences. */
int fm_metal_encode_scale1(const FloatMotionStateMetal *s, id<MTLCommandBuffer> cmd,
                                  const FmPlaneMetal *p)
{
    id<MTLComputeCommandEncoder> const enc = [cmd computeCommandEncoder];
    if (enc == nil) {
        return -ENOMEM;
    }
    const VmafMtlFmScale1Args args = {.width=p->w, .height=p->h, .scaled_width=p->sw, .scaled_height=p->sh,
                                      .ratio_x=vmaf_mtl_fm_scale_ratio(p->w, p->sw),
                                      .ratio_y=vmaf_mtl_fm_scale_ratio(p->h, p->sh)};
    [enc setComputePipelineState:(__bridge id<MTLComputePipelineState>)s->pso_scale1];
    [enc setBuffer:(__bridge id<MTLBuffer>)p->blur[s->cur_blur] offset:0 atIndex:0];
    [enc setBuffer:(__bridge id<MTLBuffer>)p->blur[1 - s->cur_blur] offset:0 atIndex:1];
    [enc setBuffer:(__bridge id<MTLBuffer>)p->diff[1] offset:0 atIndex:2];
    [enc setBytes:&args length:sizeof(args) atIndex:3];
    [enc dispatchThreadgroups:fm_metal_groups(p->sw, p->sh)
        threadsPerThreadgroup:MTLSizeMake(VMAF_MTL_FM_BLOCK, VMAF_MTL_FM_BLOCK, 1)];
    [enc endEncoding];
    return 0;
}

/* Row kernel: one thread per row adds the `width` differences of its row in
 * the CPU's order and writes `height` row sums at `first_row` of the
 * read-back. `diff` is a transposed plane a kernel of this frame wrote. */
int fm_metal_encode_row_sum(const FloatMotionStateMetal *s, id<MTLCommandBuffer> cmd,
                                   void *diff, unsigned first_row, unsigned width,
                                   unsigned height)
{
    id<MTLComputeCommandEncoder> const enc = [cmd computeCommandEncoder];
    if (enc == nil) {
        return -ENOMEM;
    }
    const VmafMtlFmRowArgs args = {.width=width, .height=height, .first_row=first_row};
    [enc setComputePipelineState:(__bridge id<MTLComputePipelineState>)s->pso_row_sum];
    [enc setBuffer:(__bridge id<MTLBuffer>)diff offset:0 atIndex:0];
    [enc setBuffer:vmaf_metal::borrow<id<MTLBuffer>>(s->rb.buffer) offset:0 atIndex:1];
    [enc setBytes:&args length:sizeof(args) atIndex:2];
    [enc dispatchThreadgroups:MTLSizeMake((height + VMAF_MTL_FM_ROW_GROUP - 1u) /
                                              VMAF_MTL_FM_ROW_GROUP, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(VMAF_MTL_FM_ROW_GROUP, 1, 1)];
    [enc endEncoding];
    return 0;
}

/* Every kernel of plane `p`, in order: the blur and, from the second frame
 * on, the scale-0 row sums and, with motion_add_scale1, the scale-1
 * differences and their row sums. One encoder per kernel orders them. */
int fm_metal_encode_plane(const FloatMotionStateMetal *s, id<MTLCommandBuffer> cmd,
                                 const FmPlaneMetal *p, unsigned compute_sad)
{
    int err = fm_metal_encode_blur(s, cmd, p, compute_sad);
    if (err != 0 || compute_sad == 0u) {
        return err;
    }
    err = fm_metal_encode_row_sum(s, cmd, p->diff[0], p->off0, p->w, p->h);
    if (err != 0 || p->rows1 == 0u) {
        return err;
    }
    err = fm_metal_encode_scale1(s, cmd, p);
    if (err == 0) {
        err = fm_metal_encode_row_sum(s, cmd, p->diff[1], p->off1, p->sw, p->sh);
    }
    return err;
}

int submit_fex_metal(VmafFeatureExtractor *fex, VmafPicture *ref_pic,
                            VmafPicture *ref_pic_90, VmafPicture *dist_pic,
                            VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic_90; (void)dist_pic_90; (void)dist_pic;
    FloatMotionStateMetal *s = (FloatMotionStateMetal *)fex->priv;
    s->frame_index = index;

    int err = fm_metal_upload(s, ref_pic);
    if (err != 0) { return err; }

    const void *const qh = vmaf_metal_context_queue_handle(s->ctx);
    if (qh == nullptr) { return -ENODEV; }
    id<MTLCommandQueue> const queue = (__bridge id<MTLCommandQueue>)qh;
    id<MTLCommandBuffer> const cmd = [queue commandBuffer];
    if (cmd == nil) { return -ENOMEM; }

    /* The first frame has no previous blurred frame: the blur kernel writes
     * the current slot and no SAD kernel runs. */
    const unsigned compute_sad = (index > 0u) ? 1u : 0u;
    for (unsigned c = 0u; c < s->n_planes && c < FMM_MAX_PLANES && err == 0; c++) {
        err = fm_metal_encode_plane(s, cmd, &s->plane[c], compute_sad);
    }
    if (err != 0) { return err; }

    [cmd commit];
    [cmd waitUntilCompleted];
    if ([cmd status] != MTLCommandBufferStatusCompleted) { return -EIO; }

    /* Advance the blur ping-pong. */
    s->cur_blur = 1 - s->cur_blur;
    return 0;
}

/* ------------------------------------------------------------------ */
/* collect and flush                                                  */
/* ------------------------------------------------------------------ */

/* The frame's SAD score from the read-back row sums: per plane
 * motion.c::vmaf_image_sad_c() (the fp32 mean absolute difference, plus the
 * fp32 scale-1 mean with motion_add_scale1), and the planes added in double
 * (CPU float_motion.c::motion_score_pair()). */
double fm_metal_frame_score(const FloatMotionStateMetal *s)
{
    const float *rows = (const float *)s->rb.host_view;
    double score = 0.0;
    for (unsigned c = 0u; c < s->n_planes && c < FMM_MAX_PLANES; c++) {
        const FmPlaneMetal *p = &s->plane[c];
        const float *scale1_rows = (p->rows1 != 0u) ? rows + p->off1 : nullptr;
        score += vmaf_mtl_fm_plane_score(rows + p->off0, p->w, p->h, scale1_rows, p->sw, p->sh);
    }
    return score;
}

/* The motion2 and motion3 that frame `index`'s SAD completes, in the CPU
 * float_motion.c::extract() order: motion2 = 0 at index 0 (motion3 at 0
 * waits for the first SAD or for flush), motion3 at 0 from the first SAD
 * alone, then motion2 / motion3 = min(prev, cur) at `index - 1`. */
int fm_metal_emit_motion23(const FloatMotionStateMetal *s,
                                  VmafFeatureCollector *feature_collector, unsigned index,
                                  double motion_score)
{
    if (index == 0u) {
        return fm_metal_append(s, feature_collector, "VMAF_feature_motion2_score", 0.0, 0u);
    }
    /* The smaller of the previous frame's two SADs; at index 1 there is one. */
    const double motion2 = (index > 1u && s->prev_motion_score < motion_score) ?
                               s->prev_motion_score :
                               motion_score;
    int err = 0;
    if (index > 1u) {
        err = fm_metal_append(s, feature_collector, "VMAF_feature_motion2_score",
                              fm_metal_motion_clip(s, motion2), index - 1u);
    }
    if (err == 0) {
        err = fm_metal_append(s, feature_collector, "VMAF_feature_motion3_score",
                              fm_metal_motion_blend_clip(s, motion2), index - 1u);
    }
    return err;
}

int collect_fex_metal(VmafFeatureExtractor *fex, unsigned index,
                             VmafFeatureCollector *feature_collector)
{
    FloatMotionStateMetal *s = (FloatMotionStateMetal *)fex->priv;

    /* The frame's SAD, added and divided as the CPU does (ADR-1409); the
     * first frame has none. */
    const double motion_score = (index > 0u) ? fm_metal_frame_score(s) : 0.0;

    int err = 0;
    if (s->debug) {
        /* The CPU emits the debug score motion_clip()ped, fps weight and cap
         * included; frame 0's is 0. */
        const double debug_score = (index > 0u) ? fm_metal_motion_clip(s, motion_score) : 0.0;
        err = fm_metal_append(s, feature_collector, "VMAF_feature_motion_score", debug_score,
                              index);
    }
    if (err == 0) {
        err = fm_metal_emit_motion23(s, feature_collector, index, motion_score);
    }
    s->prev_motion_score = motion_score;
    return err;
}

/* The tail of CPU float_motion.c::flush: motion2 / motion3 of the last SAD
 * at the last frame index, motion_clip()ped and motion_blend_clip()ped. */
int fm_metal_emit_tail(const FloatMotionStateMetal *s,
                              VmafFeatureCollector *feature_collector)
{
    int err = fm_metal_append(s, feature_collector, "VMAF_feature_motion2_score",
                              fm_metal_motion_clip(s, s->prev_motion_score), s->frame_index);
    if (err == 0) {
        err = fm_metal_append(s, feature_collector, "VMAF_feature_motion3_score",
                              fm_metal_motion_blend_clip(s, s->prev_motion_score),
                              s->frame_index);
    }
    return err;
}

int flush_fex_metal(VmafFeatureExtractor *fex, VmafFeatureCollector *feature_collector)
{
    FloatMotionStateMetal *s = (FloatMotionStateMetal *)fex->priv;

    /* No collect writes motion3 at the last frame index (it writes index - 1),
     * so a motion3 there means this flush already ran: the probe makes a
     * repeated flush idempotent. */
    static const char feature_name[] = "VMAF_feature_motion3_score";
    const VmafDictionaryEntry *entry = vmaf_dictionary_get(&s->feature_name_dict, feature_name, 0);
    const char *resolved_name = entry ? entry->val : feature_name;
    double existing = 0.0;
    if (vmaf_feature_collector_get_score(feature_collector, resolved_name, &existing,
                                         s->frame_index) == 0) {
        return 1;
    }

    /* CPU float_motion.c::flush: a one-frame run has no SAD, and its motion3
     * is 0; otherwise the tail of the last SAD. */
    const int err = (s->frame_index == 0u) ?
                        fm_metal_append(s, feature_collector, feature_name, 0.0, 0u) :
                        fm_metal_emit_tail(s, feature_collector);
    return (err != 0) ? err : 1;
}

static const char *provided_features[] = {
    "VMAF_feature_motion_score", "VMAF_feature_motion2_score", "VMAF_feature_motion3_score", NULL
};
} // namespace

extern "C" {
/* Registered via extern in feature_extractor.c's feature_extractor_list[];
 * making this static would unlink the extractor from the registry — same
 * pattern every CUDA / HIP / SYCL feature extractor uses (ADR-0361 Metal
 * backend; ADR-0278 cite form). */
// NOLINTNEXTLINE(misc-use-internal-linkage) — ADR-0361 / ADR-0278
VmafFeatureExtractor vmaf_fex_float_motion_metal = {
    .name              = "float_motion_metal",
    .init              = init_fex_metal,
    .flush             = flush_fex_metal,
    .close             = close_fex_metal,
    .submit            = submit_fex_metal,
    .collect           = collect_fex_metal,
    .options           = options,
    .priv_size         = sizeof(FloatMotionStateMetal),
    .flags             = VMAF_FEATURE_EXTRACTOR_TEMPORAL | VMAF_FEATURE_EXTRACTOR_METAL,
    .provided_features = provided_features,
    .chars = {
        .n_dispatches_per_frame = 1,
        .is_reduction_only      = true,
        .min_useful_frame_area  = 1920U * 1080U,
        .dispatch_hint          = VMAF_FEATURE_DISPATCH_AUTO,
    },
};
} /* extern "C" */
