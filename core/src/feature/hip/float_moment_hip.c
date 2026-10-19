/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  float_moment feature extractor on the HIP backend — fourth consumer
 *  of `core/src/hip/kernel_template.h` (T7-10b batch-3 / ADR-0374).
 *
 *  Mirrors `core/src/feature/cuda/integer_moment_cuda.c`
 *  call-graph-for-call-graph: same private-state struct shape, same
 *  init/submit/collect/close lifecycle, same template helper
 *  invocations. Single dispatch per frame; emits all four metrics
 *  (`float_moment_ref{1st,2nd}`, `float_moment_dis{1st,2nd}`) in one
 *  kernel pass via four uint64 atomic counters — same precision posture
 *  as the CUDA twin.
 *
 *  HIP adaptation from CUDA:
 *  - `hipModuleLoadData` / `hipModuleGetFunction` / `hipModuleLaunchKernel`
 *    instead of `cuModuleLoadData` / `cuModuleGetFunction` / `cuLaunchKernel`.
 *  - Four uint64 atomic accumulators (device readback) — zeroed via
 *    `hipMemsetAsync` before each dispatch.
 *  - Luma planes copied HtoD via `hipMemcpy2DAsync` (pictures arrive as
 *    CPU VmafPictures; VMAF_FEATURE_EXTRACTOR_HIP flag cleared for T7-10b).
 *
 *  When `enable_hipcc=false` (e.g. a CI agent without ROCm), `HAVE_HIPCC`
 *  is undefined and `init()` returns -ENOSYS — same scaffold contract as
 *  the pre-runtime posture.
 *
 *  Algorithm (mirrors CUDA twin):
 *    - Four uint64 atomic counters [ref1st, dis1st, ref2nd, dis2nd]
 *      reduced per-frame, divided by `w*h` on the host to recover four
 *      mean / second-moment metrics.
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include <hip/hip_runtime_api.h>

#include "dict.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "libvmaf/picture.h"

#include "../../hip/common.h"
#include "../../hip/hip_handle.h"
#include "../../hip/kernel_template.h"
#include "../../hip/picture_hip.h"
#include "../../hip/shared_frame.h"
#include "../float_moment_sum.h"
#include "float_moment_hip.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

/* Number of uint64 atomic counters the runtime kernel emits per
 * frame: [ref1st, dis1st, ref2nd, dis2nd]. Pinned by the CUDA twin's
 * `cuda/integer_moment_cuda.c` so the eventual cross-backend numeric
 * gate has nothing fork-specific to track. */
#define MOMENT_HIP_COUNTERS 4u

/* Block dimensions for the moment kernel (mirrors CUDA twin). */
#define MOMENT_HIP_BX 16u
#define MOMENT_HIP_BY 16u

/* ------------------------------------------------------------------ */
/* HIP-to-errno translation                                            */
/* ------------------------------------------------------------------ */

#ifdef HAVE_HIPCC /* only the device-kernel build calls it */
static int moment_hip_rc(hipError_t rc)
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
#endif /* HAVE_HIPCC */

/* ------------------------------------------------------------------ */
/* Private state                                                       */
/* ------------------------------------------------------------------ */

typedef struct MomentStateHip {
    VmafHipKernelLifecycle lc;
    VmafHipKernelReadback rb; /* device: 4 x uint64 accumulators;
                                * host_pinned: readback slot */
    VmafHipContext *ctx;
    /* HIP module + per-bpc kernel function handles. */
    hipModule_t module;
    hipFunction_t funcbpc8;
    hipFunction_t funcbpc16;
    /* The CPU's second-moment sums past 2^53 units (ADR-1497): the four
     * kernels of float_moment_sum_gpu.h and their per-row device buffers,
     * allocated only for a frame whose sums can get there
     * (vmaf_moment_sum_may_round()). */
    hipFunction_t func_row_totals;
    hipFunction_t func_row_plans;
    hipFunction_t func_row_units;
    hipFunction_t func_ordered_totals;
    void *row_totals;
    void *row_plans;
    void *row_units;
    unsigned sum_rows; /* rows the buffers hold: the frame init() was given */
    /* This frame's luma planes on the device, ref + dis: the context's shared
     * frame, or `planes`' own buffers when there is none (ADR-1408). */
    void *ref_in;
    void *dis_in;
    VmafHipPlaneSource planes;
    VmafHipSharedFrame *hip_frame;
    unsigned index;
    unsigned frame_w;
    unsigned frame_h;
    unsigned bpc;
    VmafDictionary *feature_name_dict;
} MomentStateHip;

static const VmafOption options[] = {{0}};

/* ------------------------------------------------------------------ */
/* HAVE_HIPCC helpers                                                  */
/* ------------------------------------------------------------------ */

#ifdef HAVE_HIPCC
/* Load the HSACO fat binary and resolve the six kernel function handles. On
 * failure the module is unloaded again and `s->module` is NULL. */
static int moment_hip_module_load(MomentStateHip *s)
{
    static const char *const names[] = {
        "calculate_moment_hip_kernel_8bpc",
        "calculate_moment_hip_kernel_16bpc",
        "moment_row_totals",
        "moment_row_plans",
        "moment_row_units",
        "moment_ordered_totals",
    };
    hipFunction_t *const fns[] = {
        &s->funcbpc8,       &s->funcbpc16,      &s->func_row_totals,
        &s->func_row_plans, &s->func_row_units, &s->func_ordered_totals,
    };
    hipError_t hip_rc = hipModuleLoadData(&s->module, moment_score_hsaco);
    if (hip_rc != hipSuccess)
        return moment_hip_rc(hip_rc);
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]) && hip_rc == hipSuccess; i++)
        hip_rc = hipModuleGetFunction(fns[i], s->module, names[i]);
    if (hip_rc == hipSuccess)
        return 0;
    const int unload_err = moment_hip_rc(hipModuleUnload(s->module));
    s->module = NULL;
    const int err = moment_hip_rc(hip_rc);
    return err ? err : unload_err;
}

/* The per-row device buffers of the CPU's second-moment sums, for a frame of
 * `h` rows whose sums can pass 2^53 units; none otherwise. */
static int moment_hip_sum_alloc(MomentStateHip *s, unsigned w, unsigned h, unsigned bpc)
{
    if (!vmaf_moment_sum_may_round(w, h, bpc))
        return 0;
    const size_t rows = (size_t)VMAF_MOMENT_SUM_PLANES * h;
    s->sum_rows = h;
    hipError_t hip_rc = hipMalloc(&s->row_totals, rows * sizeof(uint64_t));
    if (hip_rc == hipSuccess)
        hip_rc = hipMalloc(&s->row_plans, rows * sizeof(int));
    if (hip_rc == hipSuccess)
        hip_rc = hipMalloc(&s->row_units, rows * 2u * sizeof(int64_t));
    return moment_hip_rc(hip_rc);
}

/* Frees one device buffer of moment_hip_sum_alloc(); returns its error. */
static int moment_hip_sum_free_one(void **buf)
{
    if (*buf == NULL)
        return 0;
    const int err = moment_hip_rc(hipFree(*buf));
    *buf = NULL;
    return err;
}

/* The four kernels that replace the second-moment sums with the CPU's past
 * 2^53 units, on `str` after the frame kernel. Each returns at once while a
 * plane's exact sum is at most 2^53. */
static int moment_hip_launch_sum(MomentStateHip *s, size_t row_bytes, hipStream_t str)
{
    if (s->frame_h > s->sum_rows)
        return -EINVAL;
    VmafMomentSumArgs a = {
        .luma = {(const uint8_t *)s->ref_in, (const uint8_t *)s->dis_in},
        .stride = {row_bytes, row_bytes},
        .width = s->frame_w,
        .height = s->frame_h,
        .row_totals = (uint64_t *)s->row_totals,
        .plans = (int *)s->row_plans,
        .row_units = (int64_t *)s->row_units,
        .sums = (uint64_t *)s->rb.device,
    };
    void *args[] = {&a};
    const unsigned lanes = VMAF_MOMENT_SUM_LANES;
    const unsigned planes = VMAF_MOMENT_SUM_PLANES;
    /* Kernel, grid x, grid y, work-group size: float_moment_sum_gpu.h. */
    const struct {
        hipFunction_t fn;
        unsigned gx;
        unsigned gy;
        unsigned bx;
    } launches[] = {
        {s->func_row_totals, s->frame_h, planes, lanes},
        {s->func_row_plans, planes, 1u, VMAF_MOMENT_SUM_BATCH},
        {s->func_row_units, s->frame_h, planes, lanes},
        {s->func_ordered_totals, planes, 1u, lanes},
    };
    hipError_t hip_rc = hipSuccess;
    for (size_t i = 0; i < sizeof(launches) / sizeof(launches[0]) && hip_rc == hipSuccess; i++) {
        hip_rc = hipModuleLaunchKernel(launches[i].fn, launches[i].gx, launches[i].gy, 1u,
                                       launches[i].bx, 1u, 1u, 0, str, args, NULL);
    }
    return moment_hip_rc(hip_rc);
}

/* Launch the per-bpc kernel on `str`. Both kernels take the same seven
 * arguments: the 16bpc one reads raw uint16_t samples whatever the bit
 * depth, and ref_in/dis_in hold 2 bytes per sample. */
static int moment_hip_launch_kernel(MomentStateHip *s, ptrdiff_t row_w, hipStream_t str)
{
    const unsigned gx = (s->frame_w + MOMENT_HIP_BX - 1u) / MOMENT_HIP_BX;
    const unsigned gy = (s->frame_h + MOMENT_HIP_BY - 1u) / MOMENT_HIP_BY;
    void *sums_dev = s->rb.device;
    void *args[] = {
        (void *)&s->ref_in, (void *)&s->dis_in,  (void *)&row_w,      (void *)&row_w,
        (void *)&sums_dev,  (void *)&s->frame_w, (void *)&s->frame_h,
    };
    hipFunction_t fn = (s->bpc == 8u) ? s->funcbpc8 : s->funcbpc16;
    return moment_hip_rc(hipModuleLaunchKernel(fn, gx, gy, 1u, MOMENT_HIP_BX, MOMENT_HIP_BY, 1u, 0,
                                               str, args, NULL));
}

/*
 * Per-frame submit body: HtoD copies of both luma planes, zero the four
 * uint64 accumulators, kernel launch, DtoH readback.
 */
static int moment_hip_launch(MomentStateHip *s, VmafPicture *ref_pic, VmafPicture *dist_pic)
{
    const size_t bpp = (s->bpc <= 8u) ? 1u : 2u;
    const ptrdiff_t row_w = (ptrdiff_t)(s->frame_w * bpp);
    hipStream_t str = vmaf_hip_stream_of(s->lc.str);
    const size_t sums_bytes = (size_t)MOMENT_HIP_COUNTERS * sizeof(uint64_t);

    /* Returns once both pictures are read: the caller recycles them when
     * submit() returns (T-HIP-PAGEABLE-UPLOAD-RACE-2026-09-18). */
    int err = vmaf_hip_plane_source_acquire_luma(&s->planes, s->hip_frame, ref_pic, dist_pic,
                                                 s->lc.str, &s->ref_in, &s->dis_in);
    /* Zero the four uint64 accumulators after the upload, directly ahead of
     * the kernel that adds into them (ADR-1427): a clear queued ahead of the
     * upload is lost in the first context of a process that needs larger
     * planes than the contexts before it. */
    if (err == 0)
        err = moment_hip_rc(hipMemsetAsync(s->rb.device, 0, sums_bytes, str));
    if (err == 0)
        err = moment_hip_launch_kernel(s, row_w, str);
    if (err == 0 && s->row_totals != NULL)
        err = moment_hip_launch_sum(s, (size_t)row_w, str);
    if (err != 0)
        return err;

    /* Record submit event, DtoH copy of the four uint64 accumulators,
     * record finished event. */
    hipError_t hip_rc = hipEventRecord(vmaf_hip_event_of(s->lc.submit), str);
    if (hip_rc == hipSuccess) {
        hip_rc =
            hipMemcpyAsync(s->rb.host_pinned, s->rb.device, sums_bytes, hipMemcpyDeviceToHost, str);
    }
    if (hip_rc != hipSuccess)
        return moment_hip_rc(hip_rc);

    return vmaf_hip_kernel_submit_post_record(&s->lc, s->ctx);
}

/* Let go of the planes and unload the module. Safe with NULL handles.
 * Returns the first error. */
static int moment_hip_module_free(MomentStateHip *s)
{
    vmaf_hip_plane_source_close(&s->planes);
    s->ref_in = NULL;
    s->dis_in = NULL;
    int rc = moment_hip_sum_free_one(&s->row_totals);
    const int plans_err = moment_hip_sum_free_one(&s->row_plans);
    rc = rc ? rc : plans_err;
    const int units_err = moment_hip_sum_free_one(&s->row_units);
    rc = rc ? rc : units_err;
    if (s->module != NULL) {
        const int e = moment_hip_rc(hipModuleUnload(s->module));
        s->module = NULL;
        if (rc == 0)
            rc = e;
    }
    return rc;
}
#endif /* HAVE_HIPCC */

/* ------------------------------------------------------------------ */
/* init / close                                                        */
/* ------------------------------------------------------------------ */

/* Tear down everything init() may have set up. Every step tolerates a handle
 * that was never created, so this serves both a failed init() and close().
 * The stream is drained first, so no kernel still uses a buffer. Returns the
 * first error. */
static int moment_hip_release(MomentStateHip *s)
{
    int rc = vmaf_hip_kernel_lifecycle_close(&s->lc, s->ctx);
    int e = 0;
#ifdef HAVE_HIPCC
    e = moment_hip_module_free(s);
    if (rc == 0)
        rc = e;
#endif /* HAVE_HIPCC */
    e = vmaf_hip_kernel_readback_free(&s->rb, s->ctx);
    if (rc == 0)
        rc = e;
    if (s->feature_name_dict != NULL) {
        e = vmaf_dictionary_free(&s->feature_name_dict);
        if (rc == 0)
            rc = e;
    }
    vmaf_hip_context_destroy(s->ctx);
    s->ctx = NULL;
    return rc;
}

static int init_fex_hip(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                        unsigned w, unsigned h)
{
    (void)pix_fmt;
    MomentStateHip *s = fex->priv;

    s->bpc = bpc;
    s->frame_w = w;
    s->frame_h = h;

    int err = vmaf_hip_context_new(&s->ctx, fex->hip_device_index);
    if (err == 0)
        err = vmaf_hip_kernel_lifecycle_init(&s->lc, s->ctx);
    if (err == 0) {
        err = vmaf_hip_kernel_readback_alloc(&s->rb, s->ctx,
                                             (size_t)MOMENT_HIP_COUNTERS * sizeof(uint64_t));
    }
#ifdef HAVE_HIPCC
    if (err == 0)
        err = moment_hip_module_load(s);
    if (err == 0)
        err = moment_hip_sum_alloc(s, w, h, bpc);
#else
    if (err == 0)
        err = -ENOSYS;
#endif
    if (err == 0) {
        s->feature_name_dict =
            vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
        if (s->feature_name_dict == NULL)
            err = -ENOMEM;
    }
    if (err != 0)
        (void)moment_hip_release(s);
    return err;
}

static int close_fex_hip(VmafFeatureExtractor *fex)
{
    return moment_hip_release(fex->priv);
}

/* ------------------------------------------------------------------ */
/* submit / collect                                                    */
/* ------------------------------------------------------------------ */

static int submit_fex_hip(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                          VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic_90;
    (void)dist_pic_90;

#ifndef HAVE_HIPCC
    (void)fex;
    (void)ref_pic;
    (void)dist_pic;
    (void)index;
    return -ENOSYS;
#else
    MomentStateHip *s = fex->priv;
    s->index = index;
    s->frame_w = ref_pic->w[0];
    s->frame_h = ref_pic->h[0];
    s->hip_frame = fex->hip_frame;
    /* Pictures arrive as host VmafPictures (ADR-0530). moment_hip_launch()
     * gets the luma planes on the device, launches the kernel, copies four
     * uint64 accumulators device->host, and records the finished event. */
    return moment_hip_launch(s, ref_pic, dist_pic);
#endif /* HAVE_HIPCC */
}

#ifdef HAVE_HIPCC
/* The scaler picture_copy() divides a sample by before moment.c accumulates
 * it: 4 at 10 bpc, 16 at 12 bpc, 256 at 16 bpc (core/src/feature/
 * picture_copy.cpp). The kernel accumulates the raw codeword, so collect()
 * divides by it; without that a 10-bit input reported ref1st / dis1st 4x and
 * ref2nd / dis2nd 16x too large (ADR-1212).
 *
 * ADR-1447: the device sums are exact integers of the CPU's own terms (the
 * samples, and the float squares the CPU forms), in units of 1 / scaler and
 * 1 / scaler^2. Every term is a multiple of the unit, so the CPU's running
 * double sum is exact, and equal to the integer sum, while it is at most
 * 2^53 units; dividing by a power of two and then by the pixel count are the
 * CPU's two operations. A second-moment sum can pass 2^53 units only on a
 * 16-bit frame of more than 2^21 pixels (each term is below 2^32). There the
 * CPU rounds as it adds, and moment_hip_launch_sum() has replaced the two
 * second-moment sums with the CPU's rounded ones (ADR-1497), each a double
 * the conversion below holds exactly. test_hip_float_moment_parity checks
 * both ranges with ==. */
static double moment_hip_scaler(unsigned bpc)
{
    if (bpc == 10u)
        return 4.0;
    if (bpc == 12u)
        return 16.0;
    return (bpc == 16u) ? 256.0 : 1.0;
}
#endif /* HAVE_HIPCC */

static int collect_fex_hip(VmafFeatureExtractor *fex, unsigned index,
                           VmafFeatureCollector *feature_collector)
{
#ifndef HAVE_HIPCC
    (void)fex;
    (void)index;
    (void)feature_collector;
    return -ENOSYS;
#else
    MomentStateHip *s = fex->priv;

    int err = vmaf_hip_kernel_collect_wait(&s->lc, s->ctx);
    if (err != 0)
        return err;

    /* Read the four uint64 accumulators from the pinned host buffer,
     * divide by the pixel count to recover means / second moments.
     * Mirrors the CUDA twin's collect path. */
    const uint64_t *sums = (const uint64_t *)s->rb.host_pinned;
    /* The sums are in units of 1 / scaler and 1 / scaler^2: see
     * moment_hip_scaler(). */
    const double moment_scaler = moment_hip_scaler(s->bpc);
    const double moment_scaler_sq = moment_scaler * moment_scaler;
    const double n_pix = (double)s->frame_w * (double)s->frame_h;
    const double ref1st = ((double)sums[0] / moment_scaler) / n_pix;
    const double dis1st = ((double)sums[1] / moment_scaler) / n_pix;
    const double ref2nd = ((double)sums[2] / moment_scaler_sq) / n_pix;
    const double dis2nd = ((double)sums[3] / moment_scaler_sq) / n_pix;

    err = vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                  "float_moment_ref1st", ref1st, index);
    if (err != 0)
        return err;
    err = vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                  "float_moment_dis1st", dis1st, index);
    if (err != 0)
        return err;
    err = vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                  "float_moment_ref2nd", ref2nd, index);
    if (err != 0)
        return err;
    return vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "float_moment_dis2nd", dis2nd, index);
#endif /* HAVE_HIPCC */
}

/* ------------------------------------------------------------------ */
/* Registration                                                        */
/* ------------------------------------------------------------------ */

static const char *provided_features[] = {
    "float_moment_ref1st",
    "float_moment_dis1st",
    "float_moment_ref2nd",
    "float_moment_dis2nd",
    NULL,
};

/* Load-bearing: the feature extractor is registered via
 * `extern VmafFeatureExtractor vmaf_fex_float_moment_hip;` in
 * `core/src/feature/feature_extractor.cpp`'s
 * `feature_extractor_list[]`. Making this static would unlink the
 * extractor from the registry and fail every name lookup. Same
 * pattern every CUDA / SYCL / Vulkan feature extractor uses (see
 * e.g. `vmaf_fex_float_moment_cuda` in
 * `core/src/feature/cuda/integer_moment_cuda.c`). */
// NOLINTNEXTLINE(misc-use-internal-linkage): cross-TU registry pattern — external linkage required (ADR-0278).
VmafFeatureExtractor vmaf_fex_float_moment_hip = {
    .name = "float_moment_hip",
    .init = init_fex_hip,
    .submit = submit_fex_hip,
    .collect = collect_fex_hip,
    .close = close_fex_hip,
    .options = options,
    .priv_size = sizeof(MomentStateHip),
    .provided_features = provided_features,
    .flags = VMAF_FEATURE_EXTRACTOR_HIP,
    /* 1 dispatch/frame, reduction-dominated; AUTO + 1080p area
     * matches the CUDA twin's profile. */
    .chars =
        {
            .n_dispatches_per_frame = 1,
            .is_reduction_only = true,
            .min_useful_frame_area = 1920U * 1080U,
            .dispatch_hint = VMAF_FEATURE_DISPATCH_AUTO,
        },
};

/* NOLINTEND(modernize-use-nullptr) */
