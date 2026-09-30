/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright (c) 2011, Tom Distler (http://tdistler.com)
 *  Copyright 2001-2012 Xiph.Org and contributors.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent AND BSD-3-Clause AND BSD-2-Clause
 *
 *  float_ssim feature extractor on the HIP backend — eighth consumer
 *  of `core/src/hip/kernel_template.h` (T7-10b batch-3 / ADR-0374).
 *
 *  Mirrors `core/src/feature/cuda/integer_ssim_cuda.c` call-graph-for-
 *  call-graph. Two-pass design mirrors the GLSL Vulkan shader and the
 *  CUDA twin:
 *    Pass 1 — horizontal 11-tap separable Gaussian over ref / cmp /
 *             ref^2 / cmp^2 / ref*cmp into five intermediate float
 *             device buffers, grid sized over (W-10) x H.
 *    Pass 2 — vertical 11-tap + per-pixel SSIM combine + per-block
 *             float partial sum, grid sized over (W-10) x (H-10).
 *  Host accumulates partials in double, divides by (W-10)*(H-10) and
 *  emits `float_ssim`.
 *
 *  HIP adaptation from CUDA:
 *  - `hipModuleLoadData` / `hipModuleGetFunction` / `hipModuleLaunchKernel`
 *    instead of `cuModuleLoadData` / `cuModuleGetFunction` / `cuLaunchKernel`.
 *  - Five intermediate float buffers allocated via `hipMalloc` (raw device
 *    pointers) instead of `vmaf_cuda_buffer_alloc` (which carries VmafCudaBuffer
 *    wrapper + `free(wrapper)` dance). The HIP scaffold has no equivalent
 *    buffer-wrapper helper.
 *  - Pictures arrive as CPU VmafPictures (VMAF_FEATURE_EXTRACTOR_HIP flag
 *    cleared, T7-10b posture). Luma planes are copied HtoD via
 *    `hipMemcpy2DAsync` on the private readback stream.
 *  - Pass 1 → Pass 2 ordering: both launches on the same HIP stream, so
 *    the implicit stream order guarantees Pass 1 writes are visible to
 *    Pass 2 reads — same happens-before as the CUDA twin on the CUDA stream.
 *
 *  When `enable_hipcc=false` (e.g. a CI agent without ROCm), `HAVE_HIPCC`
 *  is undefined and `init()` returns -ENOSYS — same scaffold contract as
 *  the pre-runtime posture (registered, runtime not ready).
 *
 *  Options mirror CPU float_ssim.c (ADR-1382, the HIP port of ADR-1365).
 *  `enable_lcs` switches pass 2 to a variant that also reduces the per-pixel
 *  luminance / contrast / structure terms of iqa/ssim_tools.c (clamped
 *  variances, flat-region covariance clamp) into three per-block double
 *  partials and emits `float_ssim_{l,c,s}`. `enable_db` / `clip_db` act on
 *  the host through the shared nonfinite_score.h SSIM helpers.
 *
 *  Pass 2 forms each pixel's SSIM term exactly as the CPU's
 *  ssim_accumulate_default_scalar() does (l * c * s in double from the
 *  CPU-typed factors) and sums one double per block; collect() adds the
 *  blocks in double, divides by the pixel count and rounds the mean to fp32,
 *  as iqa_ssim() returns it (fssim_hip_cpu_mean()). Identical frames then
 *  report what the CPU reports, 1 - 2^-24 (72.247 dB) where its fp32
 *  luminance denominator leaves that residue, instead of a forced 1.
 *
 *  v1: scale=1 only. ADR-1324 falls model-selected host-picture contexts
 *  back to CPU before init when auto resolves above 1; direct requests keep
 *  the -EINVAL capability error.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <hip/hip_runtime_api.h>

#include "dict.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "feature/nonfinite_score.h"
#include "libvmaf/picture.h"
#include "log.h"

#include "../../hip/common.h"
#include "../../hip/hip_handle.h"
#include "../../hip/kernel_template.h"
#include "../../hip/picture_hip.h"
#include "float_ssim_hip.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

/* ------------------------------------------------------------------ */
/* Block geometry constants (match ssim_score.hip)                     */
/* ------------------------------------------------------------------ */

#define SSIM_HIP_BLOCK_X 16u
#define SSIM_HIP_BLOCK_Y 8u
#define SSIM_HIP_K 11u

/* HIP errors map to negative errno through the shared
 * vmaf_hip_rc_to_errno() (core/src/hip/common.h). */

/* ------------------------------------------------------------------ */
/* Private state                                                       */
/* ------------------------------------------------------------------ */

typedef struct SsimStateHip {
    VmafHipKernelLifecycle lc;
    VmafHipKernelReadback rb; /* device: per-block double SSIM partials;
                               * host_pinned: readback slot */
    /* enable_lcs only: per-block double L / C / S partials, laid out
     * [l | c | s] with partials_capacity entries each; unallocated otherwise. */
    VmafHipKernelReadback rb_lcs;
    VmafHipContext *ctx;

    int scale_override;
    /* CPU float_ssim.c options (ADR-1382). `enable_lcs` selects the pass-2
     * kernel that also reduces L / C / S; `enable_db` / `clip_db` act on the
     * host through the nonfinite_score.h SSIM helpers. */
    bool enable_lcs;
    bool enable_db;
    bool clip_db;
    /* vmaf_ssim_max_db(): +inf unless clip_db. */
    double max_db;

    /* Five intermediate float device buffers for the horiz pass output.
     * Sized (w_horiz * h_horiz * sizeof(float)). Allocated via hipMalloc,
     * freed via hipFree. The CUDA twin carries VmafCudaBuffer * wrappers;
     * the HIP scaffold has no equivalent, so raw void * is used. */
    void *d_ref_mu;
    void *d_cmp_mu;
    void *d_ref_sq;
    void *d_cmp_sq;
    void *d_refcmp;

    /* Staging buffers: CPU luma planes → device (HtoD). One each for
     * ref and cmp (luma-only, no chroma). Sized frame_w * frame_h * bpp.
     * Also allocated via hipMalloc. */
    void *ref_in;
    void *cmp_in;

    /* HIP module + per-bpc horiz kernel + vert-combine kernel handles. */
    hipModule_t module;
    hipFunction_t func_horiz_8;
    hipFunction_t func_horiz_16;
    hipFunction_t func_vert;
    hipFunction_t func_vert_lcs;

    unsigned partials_capacity;
    unsigned partials_count;

    unsigned width;
    unsigned height;
    unsigned w_horiz;
    unsigned h_horiz;
    unsigned w_final;
    unsigned h_final;
    unsigned bpc;
    float c1;
    float c2;

    unsigned index;
    VmafDictionary *feature_name_dict;
} SsimStateHip;

/* The CPU float_ssim.c table: same names, defaults and range. */
static const VmafOption options[] = {
    {
        .name = "enable_lcs",
        .help = "enable luminance, contrast and structure intermediate output",
        .offset = offsetof(SsimStateHip, enable_lcs),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
    },
    {
        .name = "enable_db",
        .help = "write SSIM values as dB",
        .offset = offsetof(SsimStateHip, enable_db),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
    },
    {
        .name = "clip_db",
        .help = "clip dB scores",
        .offset = offsetof(SsimStateHip, clip_db),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
    },
    {
        .name = "scale",
        .help = "decimation scale factor (0=auto, 1=no downscaling). "
                "v1: direct GPU use requires scale=1; model dispatch falls back to CPU "
                "when auto resolves above 1.",
        .offset = offsetof(SsimStateHip, scale_override),
        .type = VMAF_OPT_TYPE_INT,
        .default_val.i = 0,
        .min = 0,
        .max = 10,
    },
    {0},
};

/* ------------------------------------------------------------------ */
/* Dimension helpers                                                   */
/* ------------------------------------------------------------------ */

static int ssim_hip_round_to_int(float x)
{
    return (int)(x + (x < 0.0f ? -0.5f : 0.5f));
}

static int ssim_hip_min_int(int a, int b)
{
    return a < b ? a : b;
}

static int ssim_hip_compute_scale(unsigned w, unsigned h, int override_val)
{
    if (override_val > 0)
        return override_val;
    int scaled = ssim_hip_round_to_int((float)ssim_hip_min_int((int)w, (int)h) / 256.0f);
    return scaled < 1 ? 1 : scaled;
}

/* ADR-1324: dimensions are unavailable to the earlier option-value gate. */
static int check_context_hip(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                             unsigned w, unsigned h)
{
    (void)pix_fmt;
    (void)bpc;
    const SsimStateHip *s = fex->priv;
    return ssim_hip_compute_scale(w, h, s->scale_override) == 1 ? 0 : -ENOTSUP;
}

/* Extracted to keep init_fex_hip under the 60-line readability-function-size
 * limit. Mirrors validate logic from the CUDA twin. */
static int ssim_hip_validate_dims(const SsimStateHip *s, unsigned w, unsigned h)
{
    int scale = ssim_hip_compute_scale(w, h, s->scale_override);
    if (scale != 1) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "ssim_hip: v1 supports scale=1 only "
                 "(auto-detected scale=%d at %ux%u). "
                 "Pin --feature float_ssim_hip:scale=1 if intended.\n",
                 scale, w, h);
        return -EINVAL;
    }
    if (w < SSIM_HIP_K || h < SSIM_HIP_K) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "ssim_hip: input %ux%u smaller than 11x11 Gaussian footprint.\n", w, h);
        return -EINVAL;
    }
    return 0;
}

/* Populate geometry + SSIM constant fields. Extracted to keep init_fex_hip
 * under the 60-line readability-function-size limit. */
static void ssim_hip_init_dims(SsimStateHip *s, unsigned w, unsigned h, unsigned bpc)
{
    s->width = w;
    s->height = h;
    s->bpc = bpc;
    s->w_horiz = w - (SSIM_HIP_K - 1u);
    s->h_horiz = h;
    s->w_final = w - (SSIM_HIP_K - 1u);
    s->h_final = h - (SSIM_HIP_K - 1u);

    /* SSIM stability constants: L = 255.0, K1 = 0.01, K2 = 0.03.
     * The CUDA twin pins L = 255 (float), independent of bpc, so the
     * cross-backend numeric gate has nothing fork-specific to track. */
    const float L = 255.0f;
    const float K1 = 0.01f;
    const float K2 = 0.03f;
    s->c1 = (K1 * L) * (K1 * L);
    s->c2 = (K2 * L) * (K2 * L);

    const unsigned grid_x = (s->w_final + SSIM_HIP_BLOCK_X - 1u) / SSIM_HIP_BLOCK_X;
    const unsigned grid_y = (s->h_final + SSIM_HIP_BLOCK_Y - 1u) / SSIM_HIP_BLOCK_Y;
    s->partials_capacity = grid_x * grid_y;
    s->max_db = vmaf_ssim_max_db(s->clip_db, bpc, w, h);
}

/* ------------------------------------------------------------------ */
/* HAVE_HIPCC helpers                                                  */
/* ------------------------------------------------------------------ */

#ifdef HAVE_HIPCC
/* Load the HSACO fat binary and resolve the three kernel function handles.
 * On failure the module is unloaded again and `s->module` is NULL. */
static int ssim_hip_module_load(SsimStateHip *s)
{
    hipError_t hip_rc = hipModuleLoadData(&s->module, ssim_score_hsaco);
    if (hip_rc != hipSuccess)
        return vmaf_hip_rc_to_errno(hip_rc);

    hip_rc = hipModuleGetFunction(&s->func_horiz_8, s->module, "calculate_ssim_hip_horiz_8bpc");
    if (hip_rc == hipSuccess) {
        hip_rc =
            hipModuleGetFunction(&s->func_horiz_16, s->module, "calculate_ssim_hip_horiz_16bpc");
    }
    if (hip_rc == hipSuccess)
        hip_rc = hipModuleGetFunction(&s->func_vert, s->module, "calculate_ssim_hip_vert_combine");
    if (hip_rc == hipSuccess) {
        hip_rc = hipModuleGetFunction(&s->func_vert_lcs, s->module,
                                      "calculate_ssim_hip_vert_combine_lcs");
    }
    if (hip_rc != hipSuccess) {
        (void)hipModuleUnload(s->module);
        s->module = NULL;
    }
    return vmaf_hip_rc_to_errno(hip_rc);
}

/* Number of device buffers: five horiz-pass planes + two luma staging. */
#define SSIM_HIP_N_BUFS 7u
#define SSIM_HIP_N_HORIZ 5u

/* The seven device buffers, horiz-pass planes first. */
static void ssim_hip_buf_slots(SsimStateHip *s, void **slots[SSIM_HIP_N_BUFS])
{
    slots[0] = &s->d_ref_mu;
    slots[1] = &s->d_cmp_mu;
    slots[2] = &s->d_ref_sq;
    slots[3] = &s->d_cmp_sq;
    slots[4] = &s->d_refcmp;
    slots[5] = &s->ref_in;
    slots[6] = &s->cmp_in;
}

/* Allocate five intermediate float device buffers + two luma staging
 * buffers. On failure the buffers already allocated stay set; the caller's
 * ssim_hip_release() frees them. */
static int ssim_hip_bufs_alloc(SsimStateHip *s)
{
    const size_t horiz_bytes = (size_t)s->w_horiz * s->h_horiz * sizeof(float);
    const size_t bpp = (s->bpc <= 8u) ? 1u : 2u;
    const size_t stage_bytes = (size_t)s->width * s->height * bpp;

    void **slots[SSIM_HIP_N_BUFS];
    ssim_hip_buf_slots(s, slots);
    hipError_t hip_rc = hipSuccess;
    for (unsigned i = 0u; i < SSIM_HIP_N_BUFS && hip_rc == hipSuccess; i++)
        hip_rc = hipMalloc(slots[i], (i < SSIM_HIP_N_HORIZ) ? horiz_bytes : stage_bytes);
    return vmaf_hip_rc_to_errno(hip_rc);
}

/* Free all seven device buffers, last allocated first. Safe to call with
 * NULL pointers. */
static void ssim_hip_bufs_free(SsimStateHip *s)
{
    void **slots[SSIM_HIP_N_BUFS];
    ssim_hip_buf_slots(s, slots);
    for (unsigned i = SSIM_HIP_N_BUFS; i > 0u; i--) {
        void **slot = slots[i - 1u];
        if (*slot != NULL)
            (void)hipFree(*slot);
        *slot = NULL;
    }
}

/*
 * Pass 1 — horizontal 11-tap Gaussian kernel launch.
 * Grid sized over w_horiz x h_horiz. Block 16x8.
 * Writes five intermediate float buffers on `str`.
 */
static int ssim_hip_launch_horiz(SsimStateHip *s, hipStream_t str)
{
    const unsigned grid_horiz_x = (s->w_horiz + SSIM_HIP_BLOCK_X - 1u) / SSIM_HIP_BLOCK_X;
    const unsigned grid_horiz_y = (s->h_horiz + SSIM_HIP_BLOCK_Y - 1u) / SSIM_HIP_BLOCK_Y;

    const ptrdiff_t ref_stride = (ptrdiff_t)s->width * ((s->bpc <= 8u) ? 1 : 2);
    /* Horiz kernel takes raw uint8* for both bpc variants; the 16bpc one
     * takes one more argument, `bpc`. */
    void *args8[] = {
        (void *)&s->ref_in,   (void *)&ref_stride,  (void *)&s->cmp_in,   (void *)&ref_stride,
        (void *)&s->d_ref_mu, (void *)&s->d_cmp_mu, (void *)&s->d_ref_sq, (void *)&s->d_cmp_sq,
        (void *)&s->d_refcmp, (void *)&s->w_horiz,  (void *)&s->h_horiz,
    };
    void *args16[] = {
        (void *)&s->ref_in,   (void *)&ref_stride,  (void *)&s->cmp_in,   (void *)&ref_stride,
        (void *)&s->d_ref_mu, (void *)&s->d_cmp_mu, (void *)&s->d_ref_sq, (void *)&s->d_cmp_sq,
        (void *)&s->d_refcmp, (void *)&s->w_horiz,  (void *)&s->h_horiz,  (void *)&s->bpc,
    };
    const bool is8 = (s->bpc == 8u);
    return vmaf_hip_rc_to_errno(hipModuleLaunchKernel(
        is8 ? s->func_horiz_8 : s->func_horiz_16, grid_horiz_x, grid_horiz_y, 1u, SSIM_HIP_BLOCK_X,
        SSIM_HIP_BLOCK_Y, 1u, 0, str, is8 ? args8 : args16, NULL));
}

/* Pass 2 (vertical 11-tap + SSIM combine + per-block partial sum) on `str`,
 * after pass 1 on the same stream. `enable_lcs` selects the kernel that also
 * reduces the L / C / S terms into rb_lcs. Grid over w_final x h_final. */
static int ssim_hip_launch_vert(SsimStateHip *s, hipStream_t str)
{
    const unsigned grid_x = (s->w_final + SSIM_HIP_BLOCK_X - 1u) / SSIM_HIP_BLOCK_X;
    const unsigned grid_y = (s->h_final + SSIM_HIP_BLOCK_Y - 1u) / SSIM_HIP_BLOCK_Y;
    if (!s->enable_lcs) {
        void *args[] = {
            (void *)&s->d_ref_mu, (void *)&s->d_cmp_mu,  (void *)&s->d_ref_sq, (void *)&s->d_cmp_sq,
            (void *)&s->d_refcmp, (void *)&s->rb.device, (void *)&s->w_horiz,  (void *)&s->w_final,
            (void *)&s->h_final,  (void *)&s->c1,        (void *)&s->c2,
        };
        return vmaf_hip_rc_to_errno(hipModuleLaunchKernel(s->func_vert, grid_x, grid_y, 1u,
                                                          SSIM_HIP_BLOCK_X, SSIM_HIP_BLOCK_Y, 1u, 0,
                                                          str, args, NULL));
    }
    void *args[] = {
        (void *)&s->d_ref_mu,
        (void *)&s->d_cmp_mu,
        (void *)&s->d_ref_sq,
        (void *)&s->d_cmp_sq,
        (void *)&s->d_refcmp,
        (void *)&s->rb.device,
        (void *)&s->rb_lcs.device,
        (void *)&s->w_horiz,
        (void *)&s->w_final,
        (void *)&s->h_final,
        (void *)&s->partials_count,
        (void *)&s->c1,
        (void *)&s->c2,
    };
    return vmaf_hip_rc_to_errno(hipModuleLaunchKernel(s->func_vert_lcs, grid_x, grid_y, 1u,
                                                      SSIM_HIP_BLOCK_X, SSIM_HIP_BLOCK_Y, 1u, 0,
                                                      str, args, NULL));
}

/*
 * Pass 2, then the DtoH read-back of its partials and the finished-event
 * record. Both passes run on the same stream, so implicit ordering holds;
 * collect() is the one host wait.
 */
static int ssim_hip_launch_vert_readback(SsimStateHip *s, hipStream_t str)
{
    int err = ssim_hip_launch_vert(s, str);
    if (err != 0)
        return err;

    /* Record submit event on the picture stream, then DtoH copy on the
     * private readback stream (same pattern as float_psnr_hip.c). */
    hipError_t hip_rc = hipEventRecord(vmaf_hip_event_of(s->lc.submit), str);
    if (hip_rc != hipSuccess)
        return vmaf_hip_rc_to_errno(hip_rc);

    const size_t copy_bytes = (size_t)s->partials_count * sizeof(double);
    hip_rc =
        hipMemcpyAsync(s->rb.host_pinned, s->rb.device, copy_bytes, hipMemcpyDeviceToHost, str);
    if (hip_rc == hipSuccess && s->enable_lcs) {
        const size_t lcs_bytes = 3u * (size_t)s->partials_count * sizeof(double);
        hip_rc = hipMemcpyAsync(s->rb_lcs.host_pinned, s->rb_lcs.device, lcs_bytes,
                                hipMemcpyDeviceToHost, str);
    }
    if (hip_rc != hipSuccess)
        return vmaf_hip_rc_to_errno(hip_rc);

    return vmaf_hip_kernel_submit_post_record(&s->lc, s->ctx);
}
#endif /* HAVE_HIPCC */

/* ------------------------------------------------------------------ */
/* init / close                                                        */
/* ------------------------------------------------------------------ */

/* Tear down everything init() may have set up. Every step tolerates a handle
 * that was never created, so this serves both a failed init() and close().
 * The stream is drained first, so no kernel still uses a buffer. Returns the
 * first error. */
static int ssim_hip_release(SsimStateHip *s)
{
    int rc = vmaf_hip_kernel_lifecycle_close(&s->lc, s->ctx);
    int e = 0;
#ifdef HAVE_HIPCC
    ssim_hip_bufs_free(s);
    if (s->module != NULL) {
        e = vmaf_hip_rc_to_errno(hipModuleUnload(s->module));
        s->module = NULL;
        if (rc == 0)
            rc = e;
    }
#endif /* HAVE_HIPCC */
    e = vmaf_hip_kernel_readback_free(&s->rb_lcs, s->ctx);
    if (rc == 0)
        rc = e;
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
    SsimStateHip *s = fex->priv;

    int err = ssim_hip_validate_dims(s, w, h);
    if (err != 0)
        return err;

    ssim_hip_init_dims(s, w, h, bpc);

    err = vmaf_hip_context_new(&s->ctx, 0);
    if (err == 0)
        err = vmaf_hip_kernel_lifecycle_init(&s->lc, s->ctx);
    if (err == 0) {
        err = vmaf_hip_kernel_readback_alloc(&s->rb, s->ctx,
                                             (size_t)s->partials_capacity * sizeof(double));
    }
    if (err == 0 && s->enable_lcs) {
        err = vmaf_hip_kernel_readback_alloc(&s->rb_lcs, s->ctx,
                                             3u * (size_t)s->partials_capacity * sizeof(double));
    }
#ifdef HAVE_HIPCC
    if (err == 0)
        err = ssim_hip_module_load(s);
    if (err == 0)
        err = ssim_hip_bufs_alloc(s);
#else
    if (err == 0) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "feature '%s' requires HIP device kernels compiled with -Denable_hipcc=true\n",
                 fex->name);
        err = -ENOSYS;
    }
#endif
    if (err == 0) {
        s->feature_name_dict =
            vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
        if (s->feature_name_dict == NULL)
            err = -ENOMEM;
    }
    if (err != 0)
        (void)ssim_hip_release(s);
    return err;
}

static int close_fex_hip(VmafFeatureExtractor *fex)
{
    return ssim_hip_release(fex->priv);
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
    vmaf_log(VMAF_LOG_LEVEL_ERROR,
             "feature '%s' requires HIP device kernels compiled with -Denable_hipcc=true\n",
             fex->name);
    return -ENOSYS;
#else
    SsimStateHip *s = fex->priv;
    s->index = index;
    const unsigned grid_x = (s->w_final + SSIM_HIP_BLOCK_X - 1u) / SSIM_HIP_BLOCK_X;
    const unsigned grid_y = (s->h_final + SSIM_HIP_BLOCK_Y - 1u) / SSIM_HIP_BLOCK_Y;
    s->partials_count = grid_x * grid_y;

    /* Copy both luma planes HtoD on the private stream, then dispatch
     * Pass 1 (horiz Gaussian) and Pass 2 (vert + SSIM combine) on the
     * same stream. VMAF_FEATURE_EXTRACTOR_HIP is not set (T7-10b
     * posture), so pictures arrive as CPU VmafPictures. */
    hipStream_t str = vmaf_hip_stream_of(s->lc.str);
    const size_t bpp = (s->bpc <= 8u) ? 1u : 2u;
    const ptrdiff_t row_w = (ptrdiff_t)(s->width * bpp);

    /* Returns once both pictures are read: the caller recycles them when
     * submit() returns (T-HIP-PAGEABLE-UPLOAD-RACE-2026-09-18). */
    const VmafHipPlaneUpload planes[] = {
        {.dst = s->ref_in,
         .dst_pitch = (size_t)row_w,
         .pic = ref_pic,
         .plane = 0u,
         .row_bytes = (size_t)row_w,
         .rows = s->height},
        {.dst = s->cmp_in,
         .dst_pitch = (size_t)row_w,
         .pic = dist_pic,
         .plane = 0u,
         .row_bytes = (size_t)row_w,
         .rows = s->height},
    };
    int err = vmaf_hip_picture_upload(planes, 2u, s->lc.str);
    if (err != 0)
        return err;

    err = ssim_hip_launch_horiz(s, str);
    if (err != 0)
        return err;

    return ssim_hip_launch_vert_readback(s, str);
#endif /* HAVE_HIPCC */
}

#ifdef HAVE_HIPCC
/* The frame mean of one per-block partial row, as CPU iqa_ssim() returns its
 * means: the double sum over the pixel count, rounded to fp32
 * (`(float)(sum / (double)(w * h))`). Validates the ratio first (ADR-1302). */
static int fssim_hip_cpu_mean(const double *partials, unsigned count, double n_pixels,
                              const char *name, unsigned index, double *mean)
{
    double sum = 0.0;
    for (unsigned i = 0; i < count; i++)
        sum += partials[i];
    double ratio = 0.0;
    const int err =
        vmaf_feature_finite_ratio_named("float_ssim_hip", name, sum, n_pixels, index, &ratio);
    if (err == 0)
        *mean = (double)(float)ratio;
    return err;
}

/* enable_lcs: the three per-block L / C / S partial rows become the frame
 * means float_ssim_{l,c,s}, published with the score in CPU float_ssim.c
 * order after the shared SSIM validation (ADR-1302). */
static int ssim_hip_emit_lcs(const SsimStateHip *s, double score, double n_pixels, unsigned index,
                             VmafFeatureCollector *feature_collector)
{
    static const char *const atom_names[3] = {"float_ssim_l", "float_ssim_c", "float_ssim_s"};
    const double *lcs = (const double *)s->rb_lcs.host_pinned;
    VmafNamedScore atoms[3];
    int err = 0;
    for (unsigned k = 0; k < 3u && err == 0; k++) {
        atoms[k].name = atom_names[k];
        err = fssim_hip_cpu_mean(lcs + ((size_t)k * s->partials_count), s->partials_count, n_pixels,
                                 atom_names[k], index, &atoms[k].value);
    }
    if (err != 0)
        return err;
    return vmaf_ssim_emit_scores_named(feature_collector, s->feature_name_dict, "float_ssim_hip",
                                       "float_ssim", score, s->enable_db, s->max_db, atoms, 3u,
                                       index);
}
#endif /* HAVE_HIPCC */

static int collect_fex_hip(VmafFeatureExtractor *fex, unsigned index,
                           VmafFeatureCollector *feature_collector)
{
#ifndef HAVE_HIPCC
    (void)fex;
    (void)index;
    (void)feature_collector;
    vmaf_log(VMAF_LOG_LEVEL_ERROR,
             "feature '%s' requires HIP device kernels compiled with -Denable_hipcc=true\n",
             fex->name);
    return -ENOSYS;
#else
    SsimStateHip *s = fex->priv;

    int err = vmaf_hip_kernel_collect_wait(&s->lc, s->ctx);
    if (err != 0)
        return err;

    /* The CPU's frame score: the double sum of the per-pixel terms over the
     * pixel count, rounded to fp32 (fssim_hip_cpu_mean()). */
    const double n_pixels = (double)s->w_final * (double)s->h_final;
    double score = 0.0;
    err = fssim_hip_cpu_mean((const double *)s->rb.host_pinned, s->partials_count, n_pixels,
                             "float_ssim", index, &score);
    if (err != 0)
        return err;
    if (!s->enable_lcs) {
        return vmaf_ssim_emit_score_named(feature_collector, s->feature_name_dict, "float_ssim_hip",
                                          "float_ssim", score, s->enable_db, s->max_db, index);
    }
    return ssim_hip_emit_lcs(s, score, n_pixels, index, feature_collector);
#endif /* HAVE_HIPCC */
}

/* ------------------------------------------------------------------ */
/* Registration                                                        */
/* ------------------------------------------------------------------ */

static const char *provided_features[] = {"float_ssim", NULL};

/* Load-bearing: the feature extractor is registered via
 * `extern VmafFeatureExtractor vmaf_fex_float_ssim_hip;` in
 * `core/src/feature/feature_extractor.cpp`'s
 * `feature_extractor_list[]`. Making this static would unlink the
 * extractor from the registry and fail every name lookup. Same
 * pattern every CUDA / SYCL / Vulkan feature extractor uses (see
 * e.g. `vmaf_fex_float_ssim_cuda` in
 * `core/src/feature/cuda/integer_ssim_cuda.c`). */
// NOLINTNEXTLINE(misc-use-internal-linkage): cross-TU registry pattern — external linkage required (ADR-0278).
VmafFeatureExtractor vmaf_fex_float_ssim_hip = {
    .name = "float_ssim_hip",
    .init = init_fex_hip,
    .submit = submit_fex_hip,
    .collect = collect_fex_hip,
    .close = close_fex_hip,
    .options = options,
    .priv_size = sizeof(SsimStateHip),
    .provided_features = provided_features,
    .flags = VMAF_FEATURE_EXTRACTOR_HIP,
    /* 2 dispatches/frame (horiz + vert+combine). The horiz intermediate
     * buffers are filled per-frame — not a pure reduction, so
     * is_reduction_only = false. */
    .chars =
        {
            .n_dispatches_per_frame = 2,
            .is_reduction_only = false,
            .min_useful_frame_area = 1920U * 1080U,
            .dispatch_hint = VMAF_FEATURE_DISPATCH_AUTO,
        },
    .context_check = check_context_hip,
    .context_fallback_name = "float_ssim",
};

/* NOLINTEND(modernize-use-nullptr) */
