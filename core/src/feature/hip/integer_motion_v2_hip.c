/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  motion_v2 feature extractor on the HIP backend -- sixth consumer
 *  of `libvmaf/src/hip/kernel_template.h` (T7-10b follow-up /
 *  ADR-0267).  Real kernel promotion: T7-10b batch-4 / ADR-0377.
 *
 *  This TU mirrors `libvmaf/src/feature/cuda/integer_motion_v2_cuda.c`
 *  call-graph-for-call-graph. When `HAVE_HIPCC` is defined the real HIP
 *  Module API path is active: module load, raw-pixel ping-pong (`pix[2]`)
 *  via `hipMalloc`, per-frame HtoD copy + `hipModuleLaunchKernel`, and a
 *  host-side flush() computing both `motion2_v2 = min(cur, next)` and
 *  `motion3_v2` (per-frame blend + clip + optional moving-average).
 *  Without `HAVE_HIPCC` the scaffold posture is preserved.
 *
 *  The motion3_v2 post-process and its four-option surface mirror the
 *  CUDA twin (PR #909) and the CPU reference integer_motion_v2.c::flush
 *  byte-for-byte, bit-exact at default options. They were added on the
 *  HIP backend in ADR-1108 (cross-backend follow-up closing the GPU-twin
 *  deferral ADR-0337 left open). No GPU work is needed for the
 *  post-process; it reuses the shared motion_blend_tools.h helper.
 *
 *  Bit-exactness (ADR-0138/0139): the HIP kernel uses arithmetic right
 *  shifts on int32/int64 -- the same as the CPU reference and CUDA twin.
 *  A logical (unsigned) shift would diverge for negative signed values and
 *  was the root cause of the AVX2 srlv_epi64 divergence fixed in PR #587.
 *
 *  Unique vs other HIP consumers: TEMPORAL extractor with a raw-pixel
 *  ping-pong (`pix[2]`) stored as plain `void *` device pointers, mirroring
 *  the CUDA twin's `VmafCudaBuffer *pix[2]` allocation strategy. The template
 *  readback bundle holds only the single int64 SAD accumulator.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dict.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "libvmaf/picture.h"
#include "log.h"
#include "motion_blend_tools.h"

#include "../../hip/common.h"
#include "../../hip/kernel_template.h"
#include "integer_motion_v2_hip.h"

#ifdef HAVE_HIPCC
#define __HIP_PLATFORM_AMD__ 1
#include <hip/hip_runtime_api.h>
#endif /* HAVE_HIPCC */

/* Default maximum value allowed for motion — mirrors
 * DEFAULT_MOTION_MAX_VAL in integer_motion_v2.c (the CPU reference) and
 * MOTION_V2_CUDA_DEFAULT_MAX_VAL in the CUDA twin. */
#define MOTION_V2_HIP_DEFAULT_MAX_VAL (10000.0)

typedef struct MotionV2StateHip {
    /* Lifecycle (private stream + submit/finished event pair) and the
     * (device int64 SAD accumulator, pinned host readback slot) pair
     * are managed by `hip/kernel_template.h` (T7-10b sixth consumer /
     * ADR-0267). */
    VmafHipKernelLifecycle lc;
    VmafHipKernelReadback rb;
    VmafHipContext *ctx;

#ifdef HAVE_HIPCC
    hipModule_t module;
    hipFunction_t funcbpc8;
    hipFunction_t funcbpc16;
    /* Ping-pong of raw ref Y planes on device. pix[index%2] is the current
     * frame's slot; pix[(index+1)%2] is the previous frame's slot.
     * Outside the template's readback bundle (template models one device+host
     * pair, not a ping-pong of device-only buffers). */
    void *pix[2];
#endif /* HAVE_HIPCC */

    size_t plane_bytes;
    unsigned index;
    unsigned frame_w;
    unsigned frame_h;
    unsigned bpc;
    double motion_fps_weight;

    /* motion3_v2 post-process options — mirror the CPU reference
     * (integer_motion_v2.c) option table byte-for-byte so a model file
     * carrying `motion_v2_hip=motion_blend_factor=…` loads and scores
     * identically to the CPU and CUDA paths (ADR-1108). */
    double motion_blend_factor;
    double motion_blend_offset;
    double motion_max_val;
    bool motion_moving_average;

    VmafDictionary *feature_name_dict;
} MotionV2StateHip;

#define MV2H_BX 16u
#define MV2H_BY 16u

/* Option table mirrors integer_motion_v2.c (CPU reference) for the
 * subset of options the HIP twin's host-side motion3_v2 post-process
 * consumes: name / alias / type / default / min / max / flags match
 * byte-for-byte so co-scheduled CPU+HIP runs name features identically
 * and model files load on either path (ADR-1108, matching the CUDA
 * twin). motion_force_zero and motion_five_frame_window are CPU-only
 * knobs (the HIP kernel always computes the SAD, and the 5-frame window
 * is unsupported per ADR-0337); they are intentionally omitted from this
 * twin's surface. */
static const VmafOption options[] = {
    {
        .name = "motion_fps_weight",
        .alias = "mfw",
        .help = "fps-aware multiplicative weight/correction",
        .offset = offsetof(MotionV2StateHip, motion_fps_weight),
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
        .offset = offsetof(MotionV2StateHip, motion_blend_factor),
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
        .offset = offsetof(MotionV2StateHip, motion_blend_offset),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = 40.0,
        .min = 0.0,
        .max = 1000.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "motion_max_val",
        .alias = "mmxv",
        .help = "maximum value allowed; larger values will be clipped to this value",
        .offset = offsetof(MotionV2StateHip, motion_max_val),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = MOTION_V2_HIP_DEFAULT_MAX_VAL,
        .min = 0.0,
        .max = 10000.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "motion_moving_average",
        .alias = "mma",
        .help = "smooth motion3 with a 2-frame moving average",
        .offset = offsetof(MotionV2StateHip, motion_moving_average),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {0}};

#ifdef HAVE_HIPCC
/* Translate a HIP error code to a negative errno. */
static int mv2_hip_rc(hipError_t rc)
{
    if (rc == hipSuccess)
        return 0;
    switch (rc) {
    case hipErrorInvalidValue:
    case hipErrorInvalidHandle:
        return -EINVAL;
    case hipErrorOutOfMemory:
        return -ENOMEM;
    case hipErrorNoDevice:
    case hipErrorInvalidDevice:
        return -ENODEV;
    case hipErrorNotSupported:
        return -ENOSYS;
    default:
        return -EIO;
    }
}

/* Load HSACO module and resolve both kernel entry points. */
static int mv2_hip_module_load(MotionV2StateHip *s)
{
    hipError_t rc = hipModuleLoadData(&s->module, motion_v2_score_hsaco);
    if (rc != hipSuccess)
        return mv2_hip_rc(rc);

    rc = hipModuleGetFunction(&s->funcbpc8, s->module, "motion_v2_kernel_8bpc");
    if (rc != hipSuccess) {
        (void)hipModuleUnload(s->module);
        s->module = NULL;
        return mv2_hip_rc(rc);
    }
    rc = hipModuleGetFunction(&s->funcbpc16, s->module, "motion_v2_kernel_16bpc");
    if (rc != hipSuccess) {
        (void)hipModuleUnload(s->module);
        s->module = NULL;
        return mv2_hip_rc(rc);
    }
    return 0;
}

/* Allocate ping-pong device buffers. On failure frees partial allocs. */
static int mv2_hip_bufs_alloc(MotionV2StateHip *s)
{
    hipError_t rc = hipMalloc(&s->pix[0], s->plane_bytes);
    if (rc != hipSuccess)
        return -ENOMEM;
    rc = hipMalloc(&s->pix[1], s->plane_bytes);
    if (rc != hipSuccess) {
        (void)hipFree(s->pix[0]);
        s->pix[0] = NULL;
        return -ENOMEM;
    }
    return 0;
}

/* Free ping-pong buffers and unload the module. Safe with NULL handles. */
static void mv2_hip_bufs_free(MotionV2StateHip *s)
{
    if (s->pix[1] != NULL) {
        (void)hipFree(s->pix[1]);
        s->pix[1] = NULL;
    }
    if (s->pix[0] != NULL) {
        (void)hipFree(s->pix[0]);
        s->pix[0] = NULL;
    }
    if (s->module != NULL) {
        (void)hipModuleUnload(s->module);
        s->module = NULL;
    }
}

/* Per-frame submit: HtoD copy, optional kernel launch, event/DtoH copy.
 * Extracted to keep submit_fex_hip under the 60-line readability limit. */
static int mv2_hip_launch(MotionV2StateHip *s, VmafPicture *ref_pic, unsigned index)
{
    hipStream_t str = (hipStream_t)s->lc.str;
    const unsigned cur_idx = index % 2u;
    const unsigned prev_idx = (index + 1u) % 2u;
    const size_t bpp = (s->bpc <= 8u) ? 1u : 2u;
    const ptrdiff_t plane_pitch = (ptrdiff_t)(s->frame_w * bpp);

    /* HtoD copy of current ref Y plane into ping-pong slot cur_idx. */
    hipError_t rc = hipMemcpy2DAsync(s->pix[cur_idx], (size_t)plane_pitch, ref_pic->data[0],
                                     (size_t)ref_pic->stride[0], (size_t)plane_pitch,
                                     (size_t)s->frame_h, hipMemcpyHostToDevice, str);
    if (rc != hipSuccess)
        return mv2_hip_rc(rc);

    /* Frame 0: nothing to diff against; record submit event so collect
     * can sync. Emit 0 in collect. */
    if (index == 0u) {
        rc = hipEventRecord((hipEvent_t)s->lc.submit, str);
        return (rc == hipSuccess) ? 0 : mv2_hip_rc(rc);
    }

    /* Reset device int64 SAD accumulator (single uint64_t). Must run
     * before the kernel so the atomicAdd starts from 0. */
    rc = hipMemsetAsync(s->rb.device, 0, sizeof(uint64_t), str);
    if (rc != hipSuccess)
        return mv2_hip_rc(rc);

    const unsigned gx = (s->frame_w + MV2H_BX - 1u) / MV2H_BX;
    const unsigned gy = (s->frame_h + MV2H_BY - 1u) / MV2H_BY;
    uint8_t *prev_dev = (uint8_t *)s->pix[prev_idx];
    uint8_t *cur_dev = (uint8_t *)s->pix[cur_idx];
    uint64_t *sad_dev = (uint64_t *)s->rb.device;
    unsigned w = s->frame_w;
    unsigned h = s->frame_h;
    unsigned bpc = s->bpc;

    hipFunction_t func;
    if (s->bpc == 8u) {
        func = s->funcbpc8;
        void *args[] = {(void *)&prev_dev,
                        (void *)&cur_dev,
                        (void *)&plane_pitch,
                        (void *)&plane_pitch,
                        (void *)&sad_dev,
                        (void *)&w,
                        (void *)&h};
        rc = hipModuleLaunchKernel(func, gx, gy, 1, MV2H_BX, MV2H_BY, 1, 0, str, args, NULL);
    } else {
        func = s->funcbpc16;
        void *args[] = {(void *)&prev_dev,    (void *)&cur_dev, (void *)&plane_pitch,
                        (void *)&plane_pitch, (void *)&sad_dev, (void *)&w,
                        (void *)&h,           (void *)&bpc};
        rc = hipModuleLaunchKernel(func, gx, gy, 1, MV2H_BX, MV2H_BY, 1, 0, str, args, NULL);
    }
    if (rc != hipSuccess)
        return mv2_hip_rc(rc);

    rc = hipEventRecord((hipEvent_t)s->lc.submit, str);
    if (rc != hipSuccess)
        return mv2_hip_rc(rc);

    rc = hipMemcpyAsync(s->rb.host_pinned, s->rb.device, sizeof(uint64_t), hipMemcpyDeviceToHost,
                        str);
    if (rc != hipSuccess)
        return mv2_hip_rc(rc);

    return vmaf_hip_kernel_submit_post_record(&s->lc, s->ctx);
}
#endif /* HAVE_HIPCC */

static int init_fex_hip(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                        unsigned w, unsigned h)
{
    (void)pix_fmt;
    MotionV2StateHip *s = fex->priv;

    s->frame_w = w;
    s->frame_h = h;
    s->bpc = bpc;
    s->plane_bytes = (size_t)w * h * (bpc <= 8u ? 1u : 2u);

    /* The 5-tap HIP kernel uses reflect-101 mirror padding; mv2_mirror()
     * returns 2*sup - idx - 2, which is negative when sup < 3.  Refuse
     * smaller frames up front.  Minimum: filter_width/2 + 1 = 3. */
    if (h < 3u || w < 3u) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "motion_v2_hip: frame %ux%u is below the 5-tap filter minimum 3x3; "
                 "refusing to avoid out-of-bounds mirror reads on device\n",
                 w, h);
        return -EINVAL;
    }

    int err = vmaf_hip_context_new(&s->ctx, 0);
    if (err != 0) {
        return err;
    }

    err = vmaf_hip_kernel_lifecycle_init(&s->lc, s->ctx);
    if (err != 0) {
        goto fail_after_ctx;
    }

    /* Readback pair: single int64 SAD accumulator + pinned host slot. */
    err = vmaf_hip_kernel_readback_alloc(&s->rb, s->ctx, sizeof(uint64_t));
    if (err != 0) {
        goto fail_after_lc;
    }

#ifdef HAVE_HIPCC
    err = mv2_hip_module_load(s);
    if (err != 0) {
        goto fail_after_rb;
    }

    err = mv2_hip_bufs_alloc(s);
    if (err != 0) {
        goto fail_after_module;
    }
#endif /* HAVE_HIPCC */

    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (s->feature_name_dict == NULL) {
        err = -ENOMEM;
#ifdef HAVE_HIPCC
        mv2_hip_bufs_free(s);
        goto fail_after_rb;
#else
        goto fail_after_rb;
#endif
    }

    return 0;

#ifdef HAVE_HIPCC
fail_after_module:
    if (s->module != NULL) {
        (void)hipModuleUnload(s->module);
        s->module = NULL;
    }
#endif /* HAVE_HIPCC */
fail_after_rb:
    (void)vmaf_hip_kernel_readback_free(&s->rb, s->ctx);
fail_after_lc:
    (void)vmaf_hip_kernel_lifecycle_close(&s->lc, s->ctx);
fail_after_ctx:
    vmaf_hip_context_destroy(s->ctx);
    s->ctx = NULL;
    return err;
}

static int submit_fex_hip(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                          VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index)
{
    (void)dist_pic;
    (void)ref_pic_90;
    (void)dist_pic_90;
    MotionV2StateHip *s = fex->priv;

    s->index = index;
    s->frame_w = ref_pic->w[0];
    s->frame_h = ref_pic->h[0];

#ifdef HAVE_HIPCC
    return mv2_hip_launch(s, ref_pic, index);
#else
    return -ENOSYS;
#endif /* HAVE_HIPCC */
}

static int collect_fex_hip(VmafFeatureExtractor *fex, unsigned index,
                           VmafFeatureCollector *feature_collector)
{
    MotionV2StateHip *s = fex->priv;

    int err = vmaf_hip_kernel_collect_wait(&s->lc, s->ctx);
    if (err != 0) {
        return err;
    }

#ifdef HAVE_HIPCC
    /* Frame 0: no diff was computed -- emit 0. */
    if (index == 0u) {
        return vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                       "VMAF_integer_feature_motion_v2_sad_score",
                                                       0.0, index);
    }

    /* SAD sum -> motion_v2_sad_score = sad / 256.0 / (w*h).
     * Matches the CUDA twin's collect formula verbatim (ADR-0138/0139). */
    const uint64_t *sad_host = s->rb.host_pinned;
    const double sad_score = (double)*sad_host / 256.0 / ((double)s->frame_w * (double)s->frame_h);
    return vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "VMAF_integer_feature_motion_v2_sad_score",
                                                   sad_score, index);
#else
    (void)feature_collector;
    (void)index;
    return -ENOSYS;
#endif /* HAVE_HIPCC */
}

static int flush_fex_hip(VmafFeatureExtractor *fex, VmafFeatureCollector *feature_collector)
{
#ifndef HAVE_HIPCC
    (void)fex;
    (void)feature_collector;
    return 1;
#else
    MotionV2StateHip *s = fex->priv;

    /* Host-only post-pass: motion2_v2 = min(score[i], score[i+1]) and
     * motion3_v2 = per-frame blend + clip + optional moving-average.
     * Mirrors the CUDA twin's flush_fex_cuda shape and the CPU reference
     * integer_motion_v2.c::flush byte-for-byte (ADR-1108). */

    /* Resolve the (possibly renamed, for sfr/hfr co-schedule) SAD feature
     * name from the dict — mirrors integer_motion_v2.c::flush. */
    VmafDictionaryEntry *e_sad =
        vmaf_dictionary_get(&s->feature_name_dict, "VMAF_integer_feature_motion_v2_sad_score", 0);
    const char *sad_name = e_sad ? e_sad->val : "VMAF_integer_feature_motion_v2_sad_score";

    unsigned n_frames = 0;
    double dummy;
    while (!vmaf_feature_collector_get_score(feature_collector, sad_name, &dummy, n_frames))
        n_frames++;

    if (n_frames < 2u)
        return 1;

    /* motion3_v2 seeding — mirrors integer_motion_v2.c::flush exactly.
     * 3-frame mode only (min_idx = 1; the 5-frame window is unsupported
     * on motion_v2, ADR-0337). stamp_value blends the *raw SAD* at
     * min_idx, clipped to motion_max_val; it is emitted for all indices
     * i < min_idx. */
    const unsigned min_idx = 1;
    double stamp_value = 0.;
    if (n_frames > min_idx) {
        double sad_at_min_idx;
        if (!vmaf_feature_collector_get_score(feature_collector, sad_name, &sad_at_min_idx,
                                              min_idx)) {
            stamp_value =
                MIN(motion_blend(sad_at_min_idx, s->motion_blend_factor, s->motion_blend_offset),
                    s->motion_max_val);
        }
    }

    double prev_processed = 0.;
    for (unsigned i = 0; i < n_frames; i++) {
        double score_cur;
        double score_next;
        vmaf_feature_collector_get_score(feature_collector, sad_name, &score_cur, i);
        /* Apply fps weight — mirrors CPU integer_motion_v2.c flush logic.
         * Bit-exact when motion_fps_weight = 1.0 (default). */
        score_cur *= s->motion_fps_weight;

        double motion2;
        if (i + 1u < n_frames) {
            vmaf_feature_collector_get_score(feature_collector, sad_name, &score_next, i + 1u);
            score_next *= s->motion_fps_weight;
            motion2 = score_cur < score_next ? score_cur : score_next;
        } else {
            motion2 = score_cur;
        }

        int append_err = vmaf_feature_collector_append_with_dict(
            feature_collector, s->feature_name_dict, "VMAF_integer_feature_motion2_v2_score",
            motion2, i);
        if (append_err)
            return append_err;

        /* motion3_v2_score: per-frame blend + clip + optional moving-average.
         * Mirrors integer_motion_v2.c::flush byte-for-byte. */
        double motion3;
        if (i < min_idx) {
            motion3 = stamp_value;
            prev_processed = stamp_value;
        } else {
            double processed =
                MIN(motion_blend(motion2, s->motion_blend_factor, s->motion_blend_offset),
                    s->motion_max_val);
            motion3 = s->motion_moving_average ? (processed + prev_processed) / 2.0 : processed;
            prev_processed = processed;
        }

        append_err = vmaf_feature_collector_append_with_dict(
            feature_collector, s->feature_name_dict, "VMAF_integer_feature_motion3_v2_score",
            motion3, i);
        if (append_err)
            return append_err;
    }

    return 1;
#endif /* HAVE_HIPCC */
}

static int close_fex_hip(VmafFeatureExtractor *fex)
{
    MotionV2StateHip *s = fex->priv;

    int rc = vmaf_hip_kernel_lifecycle_close(&s->lc, s->ctx);

#ifdef HAVE_HIPCC
    /* mv2_hip_bufs_free also unloads the module; best-effort only. */
    mv2_hip_bufs_free(s);
#endif /* HAVE_HIPCC */

    int err = vmaf_hip_kernel_readback_free(&s->rb, s->ctx);
    if (err != 0 && rc == 0) {
        rc = err;
    }
    if (s->feature_name_dict != NULL) {
        err = vmaf_dictionary_free(&s->feature_name_dict);
        if (err != 0 && rc == 0) {
            rc = err;
        }
    }
    if (s->ctx != NULL) {
        vmaf_hip_context_destroy(s->ctx);
        s->ctx = NULL;
    }
    return rc;
}

static const char *provided_features[] = {"VMAF_integer_feature_motion_v2_sad_score",
                                          "VMAF_integer_feature_motion2_v2_score",
                                          "VMAF_integer_feature_motion3_v2_score", NULL};

/* Load-bearing: the feature extractor is registered via
 * `extern VmafFeatureExtractor vmaf_fex_integer_motion_v2_hip;` in
 * `libvmaf/src/feature/feature_extractor.c`'s
 * `feature_extractor_list[]`. Making this static would unlink the
 * extractor from the registry and fail every name lookup. Same
 * pattern every CUDA / SYCL / Vulkan feature extractor uses (see
 * e.g. `vmaf_fex_integer_motion_v2_cuda` in
 * `libvmaf/src/feature/cuda/integer_motion_v2_cuda.c`). */
// NOLINTNEXTLINE(misc-use-internal-linkage): cross-TU registry pattern — external linkage required (ADR-0278).
VmafFeatureExtractor vmaf_fex_integer_motion_v2_hip = {
    .name = "motion_v2_hip",
    .init = init_fex_hip,
    .submit = submit_fex_hip,
    .collect = collect_fex_hip,
    .flush = flush_fex_hip,
    .close = close_fex_hip,
    .options = options,
    .priv_size = sizeof(MotionV2StateHip),
    .provided_features = provided_features,
    /* TEMPORAL flag is mandatory: motion_v2 needs the previous-frame
     * carry, so the feature engine has to drive collect before the
     * next submit. Mirrors the CUDA twin verbatim.
     *
     * Intentionally no VMAF_FEATURE_EXTRACTOR_HIP flag yet -- the
     * picture buffer-type plumbing for HIP lands with T7-10c. Until
     * then pictures arrive as CPU VmafPictures and mv2_hip_launch()
     * does explicit HtoD copies. Same posture as all prior HIP
     * consumers (ADR-0241, ADR-0254, ADR-0372, ADR-0373). */
    .flags = VMAF_FEATURE_EXTRACTOR_TEMPORAL,
    .chars =
        {
            .n_dispatches_per_frame = 1,
            .is_reduction_only = true,
            .min_useful_frame_area = 1920U * 1080U,
            .dispatch_hint = VMAF_FEATURE_DISPATCH_AUTO,
        },
};
