/**
 *  Copyright 2016-2020 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  float_vif feature extractor on the HIP backend (ADR-0379; the CPU's
 *  arithmetic since ADR-1444).
 *
 *  Numerical contract: every output is the CPU extractor's, bit for bit. The
 *  CPU extractor fixes four things a twin has to copy for that, and this twin
 *  copies them the way the CUDA twin does (ADR-1412):
 *
 *   - the Gaussian taps are vif_get_filter()'s, computed at init in fp32 as
 *     float_vif.c computes them, and handed to every launch by value;
 *   - log2 is the polynomial log2f_approx(), not a math-library call;
 *   - vif_sigma_nsq is a double, so the two log arguments are fp64 quotients
 *     and sums rounded to fp32 once;
 *   - the per-pixel terms are added row by row into one fp32 accumulator and
 *     the rows into another.
 *
 *  The arithmetic is feature/float_vif_gpu_common.h, which the kernels
 *  (float_vif/float_vif_score.hip), this file, the CUDA twin and the
 *  device-free test core/test/test_float_vif_device_math.c all compile.
 *
 *  Per frame and scale on one stream: a decimate launch (scales 1 to 3), a
 *  compute launch that stores the two terms of every pixel, and a row-sum
 *  launch with one thread per row. The readback is two floats per row per
 *  scale into pinned host memory; collect() adds the rows with
 *  fvif_sum_rows().
 *
 *  When `HAVE_HIPCC` is defined (enable_hipcc=true at configure time) the
 *  kernels are built and run. Without it every lifecycle helper returns
 *  -ENOSYS.
 *
 *  The raw planes (ref_raw, dis_raw) come from the context's shared frame
 *  (ADR-1408); the other device buffers are plain hipMalloc allocations.
 */

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "dict.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "feature/nonfinite_score.h"
#include "float_vif_gpu_common.h"
#include "vif_tools.h"
#include "libvmaf/picture.h"
#include "log.h"

#include "../../hip/common.h"
#include "../../hip/kernel_template.h"
#include "../../hip/picture_hip.h"
#include "../../hip/shared_frame.h"

#ifdef HAVE_HIPCC
#include <hip/hip_runtime_api.h>

#include "../../hip/hip_handle.h"
#include "float_vif_hip.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */
#endif /* HAVE_HIPCC */

typedef struct FloatVifStateHip {
    VmafHipKernelLifecycle lc;
    VmafHipContext *ctx;

    bool debug;
    double vif_enhn_gain_limit;
    double vif_kernelscale;
    double vif_sigma_nsq;
    double vif_scale1_min_val;
    double vif_scale2_min_val;
    double vif_scale3_min_val;
    bool vif_skip_scale0; /* host-side suppression: emit 0.0 for scale-0, mirrors float_vif.c */

    /* vif_get_filter() per scale, as float_vif.c caches it. */
    FloatVifGpuTaps taps[FVIF_SCALES];

#ifdef HAVE_HIPCC
    hipModule_t module;
    hipFunction_t func_compute;
    hipFunction_t func_decimate;
    hipFunction_t func_row_sums;

    /* This frame's raw luma planes on the device: the context's shared frame,
     * or `planes`' own buffers when there is none (ADR-1408). */
    void *ref_raw;
    void *dis_raw;
    VmafHipPlaneSource planes;
    /* Intermediate float buffers — ping-pong across scales 1-3. */
    void *ref_buf[2];
    void *dis_buf[2];
    /* The numerator and denominator term of every pixel of the scale being
     * computed (sized for scale 0, reused by the others on the same stream),
     * then per scale the sums of each row, on the device and read back into
     * pinned host memory. */
    void *terms;
    void *rows[FVIF_SCALES];
    float *rows_host[FVIF_SCALES];
#endif /* HAVE_HIPCC */

    unsigned width;
    unsigned height;
    unsigned bpc;
    unsigned scale_w[4];
    unsigned scale_h[4];

    VmafDictionary *feature_name_dict;
} FloatVifStateHip;

static const VmafOption options[] = {
    {
        .name = "debug",
        .help = "debug mode: enable additional output",
        .offset = offsetof(FloatVifStateHip, debug),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
    },
    {
        .name = "vif_enhn_gain_limit",
        .alias = "egl",
        .help = "enhancement gain imposed on vif (>= 1.0)",
        .offset = offsetof(FloatVifStateHip, vif_enhn_gain_limit),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = 100.0,
        .min = 1.0,
        .max = 100.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "vif_kernelscale",
        .alias = "ks",
        .help = "scaling factor for the gaussian kernel",
        .offset = offsetof(FloatVifStateHip, vif_kernelscale),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = 1.0,
        .min = 0.1,
        .max = 4.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM | VMAF_OPT_FLAG_DEFAULT_ONLY,
    },
    {
        .name = "vif_scale1_min_val",
        .alias = "s1miv",
        .help = "minimum value allowed; smaller values will be set to this value",
        .offset = offsetof(FloatVifStateHip, vif_scale1_min_val),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = 0.0,
        .min = 0.0,
        .max = 1.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "vif_scale2_min_val",
        .alias = "s2miv",
        .help = "minimum value allowed; smaller values will be set to this value",
        .offset = offsetof(FloatVifStateHip, vif_scale2_min_val),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = 0.0,
        .min = 0.0,
        .max = 1.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "vif_scale3_min_val",
        .alias = "s3miv",
        .help = "minimum value allowed; smaller values will be set to this value",
        .offset = offsetof(FloatVifStateHip, vif_scale3_min_val),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = 0.0,
        .min = 0.0,
        .max = 1.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "vif_sigma_nsq",
        .alias = "snsq",
        .help = "neural noise variance",
        .offset = offsetof(FloatVifStateHip, vif_sigma_nsq),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = 2.0,
        .min = 0.0,
        .max = 5.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "vif_skip_scale0",
        .alias = "ssclz",
        .help = "when set, skip scale 0 calculations",
        .offset = offsetof(FloatVifStateHip, vif_skip_scale0),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {0},
};

static void compute_per_scale_dims(FloatVifStateHip *s)
{
    s->scale_w[0] = s->width;
    s->scale_h[0] = s->height;
    for (int i = 1; i < 4; i++) {
        s->scale_w[i] = s->scale_w[i - 1] / 2u;
        s->scale_h[i] = s->scale_h[i - 1] / 2u;
    }
}

/* Each scale's Gaussian, from the CPU's own routine. float_vif.c::init() fills
 * its filter cache with vif_get_filter_size() and vif_get_filter() for
 * (float)vif_kernelscale; the same two calls here give the kernels the same
 * fp32 taps. A filter wider than the kernels' tile halo is refused (it cannot
 * occur at the only kernelscale init accepts). */
static int fvif_hip_init_taps(FloatVifStateHip *s)
{
    for (int scale = 0; scale < FVIF_SCALES; scale++) {
        float filter[128] = {0};
        const int width = vif_get_filter_size(scale, (float)s->vif_kernelscale);
        if (width < 1 || width > FVIF_MAX_FW)
            return -EINVAL;
        vif_get_filter(filter, scale, (float)s->vif_kernelscale);
        memset(&s->taps[scale], 0, sizeof(s->taps[scale]));
        memcpy(s->taps[scale].coeff, filter, (size_t)width * sizeof(filter[0]));
        s->taps[scale].width = width;
    }
    return 0;
}

#ifdef HAVE_HIPCC
/* Translate a HIP error code to a negative errno. */
static int fvif_hip_rc(hipError_t rc)
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

/* Load the HSACO module and look up the three kernel entry points. */
static int fvif_hip_module_load(FloatVifStateHip *s)
{
    hipError_t rc = hipModuleLoadData(&s->module, float_vif_score_hsaco);
    if (rc != hipSuccess)
        return fvif_hip_rc(rc);

    rc = hipModuleGetFunction(&s->func_compute, s->module, "float_vif_compute");
    if (rc == hipSuccess)
        rc = hipModuleGetFunction(&s->func_decimate, s->module, "float_vif_decimate");
    if (rc == hipSuccess)
        rc = hipModuleGetFunction(&s->func_row_sums, s->module, "float_vif_row_sums");
    if (rc != hipSuccess) {
        (void)hipModuleUnload(s->module);
        s->module = NULL;
    }
    return fvif_hip_rc(rc);
}

/* The per-row sums of one scale: num and den per row. */
static size_t fvif_hip_rows_bytes(const FloatVifStateHip *s, int scale)
{
    return (size_t)s->scale_h[scale] * FVIF_TERM_FLOATS * sizeof(float);
}

/* Allocate all device + pinned-host buffers. On failure the buffers already
 * allocated stay set; the caller's fvif_hip_release() frees them through
 * fvif_hip_bufs_free(). */
static int fvif_hip_bufs_alloc(FloatVifStateHip *s)
{
    /* Intermediate float buffers: scale_w[1]*scale_h[1] floats each. Scales 1
     * and 3 are written to pair 0, scale 2 to pair 1. */
    const size_t fbytes = (size_t)s->scale_w[1] * s->scale_h[1] * sizeof(float);
    const size_t term_bytes = (size_t)s->width * s->height * FVIF_TERM_FLOATS * sizeof(float);

    bool ok = (hipMalloc(&s->terms, term_bytes) == hipSuccess);
    for (int i = 0; i < 2 && ok; i++) {
        ok = (hipMalloc(&s->ref_buf[i], fbytes) == hipSuccess) &&
             (hipMalloc(&s->dis_buf[i], fbytes) == hipSuccess);
    }
    for (int i = 0; i < FVIF_SCALES && ok; i++) {
        /* hipHostMalloc's C prototype takes void **; rows_host[] is float *
         * because fvif_sum_rows() reads it as floats. */
        const size_t row_bytes = fvif_hip_rows_bytes(s, i);
        ok = (hipMalloc(&s->rows[i], row_bytes) == hipSuccess) &&
             (hipHostMalloc((void **)&s->rows_host[i], row_bytes, 0) == hipSuccess);
    }
    return ok ? 0 : -ENOMEM;
}

/* Release all device buffers + pinned host slabs + HSACO module.
 * Safe to call with NULL handles. */
static void fvif_hip_bufs_free(FloatVifStateHip *s)
{
    for (int i = FVIF_SCALES - 1; i >= 0; i--) {
        if (s->rows_host[i] != NULL) {
            (void)hipHostFree(s->rows_host[i]);
            s->rows_host[i] = NULL;
        }
        if (s->rows[i] != NULL) {
            (void)hipFree(s->rows[i]);
            s->rows[i] = NULL;
        }
    }
    for (int i = 1; i >= 0; i--) {
        if (s->dis_buf[i] != NULL) {
            (void)hipFree(s->dis_buf[i]);
            s->dis_buf[i] = NULL;
        }
        if (s->ref_buf[i] != NULL) {
            (void)hipFree(s->ref_buf[i]);
            s->ref_buf[i] = NULL;
        }
    }
    if (s->terms != NULL) {
        (void)hipFree(s->terms);
        s->terms = NULL;
    }
    vmaf_hip_plane_source_close(&s->planes);
    s->dis_raw = NULL;
    s->ref_raw = NULL;
    if (s->module != NULL) {
        (void)hipModuleUnload(s->module);
        s->module = NULL;
    }
}

/* The planes scale `scale` is computed from: the raw luma planes at scale 0,
 * otherwise the pair the decimate launch of that scale wrote (scales 1 and 3
 * into pair 0, scale 2 into pair 1). */
static FloatVifGpuInput fvif_hip_scale_input(const FloatVifStateHip *s, int scale)
{
    FloatVifGpuInput in = {
        .width = s->scale_w[scale],
        .height = s->scale_h[scale],
        .bpc = s->bpc,
    };
    if (scale == 0) {
        in.ref = (uint64_t)(uintptr_t)s->ref_raw;
        in.dis = (uint64_t)(uintptr_t)s->dis_raw;
        in.stride = (int64_t)s->width * (s->bpc <= 8u ? 1 : 2);
        in.is_raw = 1u;
        return in;
    }
    const int idx = (scale - 1) % 2;
    in.ref = (uint64_t)(uintptr_t)s->ref_buf[idx];
    in.dis = (uint64_t)(uintptr_t)s->dis_buf[idx];
    in.stride = (int64_t)s->scale_w[scale];
    return in;
}

/* Filter scale `scale - 1` with this scale's taps and keep every second
 * sample: the input of scale `scale`. */
static int fvif_hip_launch_decimate(FloatVifStateHip *s, hipStream_t str, int scale)
{
    const FloatVifGpuInput out = fvif_hip_scale_input(s, scale);
    FloatVifGpuDecimateArgs args = {
        .in = fvif_hip_scale_input(s, scale - 1),
        .taps = s->taps[scale],
        .ref_out = out.ref,
        .dis_out = out.dis,
        .out_width = out.width,
        .out_height = out.height,
    };
    const unsigned grid_x = (out.width + FVIF_BX - 1u) / FVIF_BX;
    const unsigned grid_y = (out.height + FVIF_BY - 1u) / FVIF_BY;
    void *params[] = {&args};
    return fvif_hip_rc(hipModuleLaunchKernel(s->func_decimate, grid_x, grid_y, 1, FVIF_BX, FVIF_BY,
                                             1, 0, str, params, NULL));
}

/* The per-pixel terms of one scale, then their row sums.
 *
 * vif_sigma_nsq / vif_enhn_gain_limit are VMAF_OPT_FLAG_FEATURE_PARAM options
 * and reach the kernel as arguments (ADR-1217). vif_sigma_nsq stays a double
 * and sigma_max_inv is derived as vif_tools.c::vif_statistic_s derives it:
 * powf(nsq, 2.0f) in float, divided in double, narrowed to float. */
static int fvif_hip_launch_scale(FloatVifStateHip *s, hipStream_t str, int scale)
{
    FloatVifGpuComputeArgs args = {
        .in = fvif_hip_scale_input(s, scale),
        .taps = s->taps[scale],
        .terms = (uint64_t)(uintptr_t)s->terms,
        .vif_sigma_nsq = s->vif_sigma_nsq,
        .vif_enhn_gain_limit = (float)s->vif_enhn_gain_limit,
        .sigma_max_inv = (float)(powf((float)s->vif_sigma_nsq, 2.0f) / (255.0 * 255.0)),
    };
    const unsigned grid_x = (args.in.width + FVIF_BX - 1u) / FVIF_BX;
    const unsigned grid_y = (args.in.height + FVIF_BY - 1u) / FVIF_BY;
    void *params[] = {&args};
    const hipError_t rc = hipModuleLaunchKernel(s->func_compute, grid_x, grid_y, 1, FVIF_BX,
                                                FVIF_BY, 1, 0, str, params, NULL);
    if (rc != hipSuccess)
        return fvif_hip_rc(rc);

    FloatVifGpuRowArgs row_args = {
        .terms = args.terms,
        .rows = (uint64_t)(uintptr_t)s->rows[scale],
        .width = args.in.width,
        .height = args.in.height,
    };
    const unsigned row_grid = (args.in.height + FVIF_ROW_THREADS - 1u) / FVIF_ROW_THREADS;
    void *row_params[] = {&row_args};
    return fvif_hip_rc(hipModuleLaunchKernel(s->func_row_sums, row_grid, 1, 1, FVIF_ROW_THREADS, 1,
                                             1, 0, str, row_params, NULL));
}
#endif /* HAVE_HIPCC */

/* Reject the options and frame sizes this twin does not support. */
static int fvif_hip_check_config(const FloatVifStateHip *s, unsigned w, unsigned h)
{
    /* Only kernelscale=1.0 is implemented (mirrors CUDA twin). */
    if (s->vif_kernelscale != 1.0)
        return -EINVAL;

    /* Cross-backend parity with the CPU floor (ADR-0214 places=4). The
     * four-scale ladder halves the working dimension once per scale, so the
     * binding constraint is scale 3 -- at the default kernelscale the minimum
     * is 16, not 8. This backend previously had no dimension floor at all, admitting the
     * 8..15px range that walks the reflect-101 mirror out of the plane at
     * scale 3 (Netflix/vmaf#1582, the same defect fixed on the CPU path).
     * vif_get_min_dim() is the single source of truth shared with
     * float_vif.c; see its derivation in vif_tools.c. */
    const int vif_min_dim = vif_get_min_dim((float)s->vif_kernelscale);
    if (w < (unsigned)vif_min_dim || h < (unsigned)vif_min_dim) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "float_vif_hip: width and height must be >= %d for the four-scale VIF "
                 "ladder (got %ux%u)\n",
                 vif_min_dim, w, h);
        return -EINVAL;
    }
    return 0;
}

/* Tear down everything init() may have set up. Every step tolerates a handle
 * that was never created, so this serves both a failed init() and close().
 * The stream is drained first, so no kernel still uses a buffer. Returns the
 * first error; freeing the buffers and the module is best-effort. */
static int fvif_hip_release(FloatVifStateHip *s)
{
    int rc = vmaf_hip_kernel_lifecycle_close(&s->lc, s->ctx);
#ifdef HAVE_HIPCC
    fvif_hip_bufs_free(s);
#endif /* HAVE_HIPCC */
    if (s->feature_name_dict != NULL) {
        const int err = vmaf_dictionary_free(&s->feature_name_dict);
        if (err != 0 && rc == 0)
            rc = err;
    }
    vmaf_hip_context_destroy(s->ctx);
    s->ctx = NULL;
    return rc;
}

static int init_fex_hip(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                        unsigned w, unsigned h)
{
    (void)pix_fmt;
    FloatVifStateHip *s = fex->priv;

    int err = fvif_hip_check_config(s, w, h);
    if (err == 0)
        err = fvif_hip_init_taps(s);
    if (err != 0)
        return err;

    s->width = w;
    s->height = h;
    s->bpc = bpc;
    compute_per_scale_dims(s);

    err = vmaf_hip_context_new(&s->ctx, fex->hip_device_index);
    if (err == 0)
        err = vmaf_hip_kernel_lifecycle_init(&s->lc, s->ctx);
#ifdef HAVE_HIPCC
    if (err == 0)
        err = fvif_hip_module_load(s);
    if (err == 0)
        err = fvif_hip_bufs_alloc(s);
#endif /* HAVE_HIPCC */
    if (err == 0) {
        s->feature_name_dict =
            vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
        if (s->feature_name_dict == NULL)
            err = -ENOMEM;
    }
    if (err != 0)
        (void)fvif_hip_release(s);
    return err;
}

#ifdef HAVE_HIPCC
/* Bridge the kernels' stream to the private stream: record the submit event
 * on `pic_stream`, make the private stream wait for it, enqueue the DtoH
 * copies of every scale's row sums and record the finished event. */
static int fvif_hip_readback(FloatVifStateHip *s, hipStream_t pic_stream)
{
    hipStream_t str = vmaf_hip_stream_of(s->lc.str);
    hipEvent_t submit_ev = vmaf_hip_event_of(s->lc.submit);
    hipError_t rc = hipEventRecord(submit_ev, pic_stream);
    if (rc == hipSuccess)
        rc = hipStreamWaitEvent(str, submit_ev, 0);

    for (int i = 0; i < FVIF_SCALES && rc == hipSuccess; i++) {
        rc = hipMemcpyAsync(s->rows_host[i], s->rows[i], fvif_hip_rows_bytes(s, i),
                            hipMemcpyDeviceToHost, str);
    }
    if (rc != hipSuccess)
        return fvif_hip_rc(rc);
    return vmaf_hip_kernel_submit_post_record(&s->lc, s->ctx);
}
#endif /* HAVE_HIPCC */

/* Scaffold posture of submit_fex_hip() (enable_hipcc=false): report
 * not-implemented, which is the contract `meson_options.txt` documents and
 * every HIP parity test skips on (ADR-1264).
 *
 * The scaffold branch used to call `vmaf_hip_kernel_submit_pre_launch(&s->lc,
 * s->ctx, NULL, 0, 0)` first and return its result on error. That call passes
 * `rb == NULL`, which the helper rejects outright, so it ALWAYS returned
 * -EINVAL and the `-ENOSYS` after it was unreachable. The extractor therefore
 * failed instead of skipping on every default-configured HIP build, and
 * `test_hip_float_vif_parity` failed with it. The call did nothing else: the
 * NULL check is the helper's first statement, ahead of any work.
 *
 * This explanation sits here rather than in the branch it describes so the
 * function body stays inside the 60-line limit. */
static int submit_fex_hip(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                          VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic_90;
    (void)dist_pic_90;
    (void)index;
    FloatVifStateHip *s = fex->priv;

#ifdef HAVE_HIPCC
    hipStream_t pic_stream = vmaf_hip_stream_of(0u);

    /* The tightly-pitched ref and dist luma planes on the device. Returns
     * once both pictures are read: the caller recycles them when submit()
     * returns (T-HIP-PAGEABLE-UPLOAD-RACE-2026-09-18). A plane no other twin
     * uploaded yet is uploaded on the private stream, never on the null
     * stream the kernels use: a null-stream copy would queue behind every
     * other extractor's kernels of this frame, and the wait would block the
     * host on all of them. The copies are complete before the kernels are
     * enqueued. */
    int err = vmaf_hip_plane_source_acquire_luma(&s->planes, fex->hip_frame, ref_pic, dist_pic,
                                                 s->lc.str, &s->ref_raw, &s->dis_raw);
    if (err != 0)
        return err;

    /* Per scale: decimate (scales 1 to 3), the terms, their row sums. */
    for (int scale = 0; scale < FVIF_SCALES && err == 0; scale++) {
        if (scale > 0)
            err = fvif_hip_launch_decimate(s, pic_stream, scale);
        if (err == 0)
            err = fvif_hip_launch_scale(s, pic_stream, scale);
    }
    if (err != 0)
        return err;

    return fvif_hip_readback(s, pic_stream);
#else
    /* Scaffold posture: -ENOSYS. The why is above this function. */
    (void)ref_pic;
    (void)dist_pic;
    (void)s;
    return -ENOSYS;
#endif /* HAVE_HIPCC */
}

static int collect_fex_hip(VmafFeatureExtractor *fex, unsigned index,
                           VmafFeatureCollector *feature_collector)
{
    FloatVifStateHip *s = fex->priv;

    int err = vmaf_hip_kernel_collect_wait(&s->lc, s->ctx);
    if (err != 0)
        return err;

#ifdef HAVE_HIPCC
    /* compute_vif(): each scale's num and den are the fp32 sums of
     * vif_statistic_s(), widened to double. */
    double scores[2 * FVIF_SCALES];
    for (size_t i = 0u; i < FVIF_SCALES; i++)
        fvif_sum_rows(s->rows_host[i], s->scale_h[i], &scores[2u * i], &scores[2u * i + 1u]);

    VmafVifScoreSet output = {
        .minimum = {s->vif_scale1_min_val, s->vif_scale2_min_val, s->vif_scale3_min_val},
        .use_minimums = true,
        .skip_scale0 = s->vif_skip_scale0,
        .debug = s->debug,
    };
    for (size_t scale = s->vif_skip_scale0 ? 1u : 0u; scale < FVIF_SCALES; ++scale) {
        output.score_num += scores[scale * 2u];
        output.score_den += scores[scale * 2u + 1u];
    }
    for (size_t i = 0u; i < sizeof(scores) / sizeof(scores[0]); ++i)
        output.scale[i] = scores[i];
    output.score = output.score_den > 0.0 ? output.score_num / output.score_den : NAN;
    return vmaf_vif_emit_scores(feature_collector, s->feature_name_dict, "float_vif_hip", &output,
                                VMAF_VIF_FLOAT_NAMES, index);
#else
    (void)feature_collector;
    (void)index;
    return -ENOSYS;
#endif /* HAVE_HIPCC */
}

static int close_fex_hip(VmafFeatureExtractor *fex)
{
    return fvif_hip_release(fex->priv);
}

static const char *provided_features[] = {
    "VMAF_feature_vif_scale0_score",
    "VMAF_feature_vif_scale1_score",
    "VMAF_feature_vif_scale2_score",
    "VMAF_feature_vif_scale3_score",
    "vif",
    "vif_num",
    "vif_den",
    "vif_num_scale0",
    "vif_den_scale0",
    "vif_num_scale1",
    "vif_den_scale1",
    "vif_num_scale2",
    "vif_den_scale2",
    "vif_num_scale3",
    "vif_den_scale3",
    NULL,
};

/* Load-bearing: registered via extern in feature_extractor.c's
 * feature_extractor_list[].  Making this static would unlink the
 * extractor from the registry — same rule as every other HIP consumer
 * (e.g. vmaf_fex_float_motion_hip). */
// NOLINTNEXTLINE(misc-use-internal-linkage): cross-TU registry pattern — external linkage required (ADR-0278).
VmafFeatureExtractor vmaf_fex_float_vif_hip = {
    .name = "float_vif_hip",
    .init = init_fex_hip,
    .submit = submit_fex_hip,
    .collect = collect_fex_hip,
    .close = close_fex_hip,
    .options = options,
    .priv_size = sizeof(FloatVifStateHip),
    .provided_features = provided_features,
/* No TEMPORAL flag: VIF is stateless across frames.
     * VMAF_FEATURE_EXTRACTOR_HIP is gated behind the
     * enable_float_vif_hip_autodispatch Meson option (ADR-0623), on by
     * default since device frames of the VMAFx API reach the twin on the
     * device (ADR-2092, the T7-10c condition). */
#if defined(FLOAT_VIF_HIP_AUTODISPATCH)
    .flags = VMAF_FEATURE_EXTRACTOR_HIP,
#else
    .flags = 0,
#endif
    .chars =
        {
            .n_dispatches_per_frame = 1,
            .is_reduction_only = false,
            .min_useful_frame_area = 1920U * 1080U,
            .dispatch_hint = VMAF_FEATURE_DISPATCH_AUTO,
        },
};

/* NOLINTEND(modernize-use-nullptr) */
