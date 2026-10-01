/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright (c) 2011, Tom Distler (http://tdistler.com)
 *  Copyright 2001-2012 Xiph.Org and contributors.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent AND BSD-3-Clause AND BSD-2-Clause
 *
 *  integer_ssim feature extractor on the HIP backend (ADR-0564).
 *
 *  Provides the `"ssim"` feature of the CPU reference `integer_ssim.c`
 *  (`vmaf_fex_ssim`) and mirrors its CUDA twin `cuda/ssim_cuda.c`
 *  (`vmaf_fex_integer_ssim_cuda`) call for call. The kernels live in
 *  `integer_ssim/integer_ssim_score.hip`:
 *
 *    Pass 1 (integer_ssim_horiz_{8,16}bpc): 9-tap integer Gaussian over each
 *      row, int64 moments into six W x H int64 planes.
 *    Pass 2: 9-tap over the columns of those planes and the per-pixel SSIM
 *      term in double. Frames above ISSIM_HIP_RASTER_MAX_PIXELS pixels run
 *      integer_ssim_vert_combine, which reduces to one (term sum, int64
 *      weight sum) pair per 16x8 block; smaller frames run
 *      integer_ssim_vert_terms, which writes one (term, weight) pair per
 *      pixel (ADR-1400).
 *  collect() adds the pairs in index order and returns sum(term) /
 *  sum(weight), the `ssim / ssimw` that calc_ssim() returns on the CPU. For
 *  the per-pixel pairs index order is the CPU's raster order, so the sum is
 *  the CPU's double.
 *
 *  Options mirror CPU integer_ssim.c (ADR-1382, the HIP port of ADR-1365):
 *  `enable_db` / `clip_db` convert the frame score on the host through the
 *  shared nonfinite_score.h helpers (vmaf_ssim_max_db()). On the per-block
 *  path an identical window scores exactly its weight, so identical frames
 *  report +inf / the clip_db ceiling (issim_pixel_term()); on the per-pixel
 *  path the score is the CPU's double, which on an identical frame is 1 or
 *  an ulp or two away from it (156.54 dB for an identical 1x1 frame of zeros).
 *
 *  HIP adaptations from the CUDA twin:
 *  - Pictures arrive as host VmafPictures; the two luma planes are staged
 *    into packed device buffers with hipMemcpy2DAsync on the private stream
 *    before pass 1 (T7-10b posture, no HIP picture pool yet).
 *  - hipModuleLoadData / hipModuleGetFunction / hipModuleLaunchKernel stand
 *    in for the cuModule* / cuLaunchKernel calls; the kernels are extern "C"
 *    so the lookups by name resolve.
 *  - Device buffers are raw hipMalloc pointers instead of VmafCudaBuffer.
 *
 *  Without enable_hipcc there is no kernel blob to load and init() returns
 *  -ENOSYS, the scaffold contract every HIP extractor shares.
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
#include "../../hip/shared_frame.h"
#include "integer_ssim_hip.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

/* Launch geometry; must match ISSIM_BLOCK_X / ISSIM_BLOCK_Y in
 * integer_ssim_score.hip, whose block reduction is sized for it. */
#define ISSIM_HIP_BLOCK_X 16u
#define ISSIM_HIP_BLOCK_Y 8u

/* Pass-1 output planes, in kernel argument order: mux, muy, x2, xy, y2, w. */
#define ISSIM_HIP_MOMENTS 6u

typedef struct IssimStateHip {
    VmafHipKernelLifecycle lc;
    /* One double term (sum) and one int64 weight (sum) per pair. */
    VmafHipKernelReadback rb_ssim;
    VmafHipKernelReadback rb_wgt;
    VmafHipContext *ctx;

    /* Pass-1 planes, width * height int64_t each. */
    void *d_moment[ISSIM_HIP_MOMENTS];

    /* This frame's packed luma planes on the device, width *
     * bytes-per-sample per row: the context's shared frame, or `planes`' own
     * buffers when there is none (ADR-1408). */
    void *ref_in;
    void *cmp_in;
    VmafHipPlaneSource planes;

    hipModule_t module;
    hipFunction_t func_horiz_8;
    hipFunction_t func_horiz_16;
    hipFunction_t func_vert;
    hipFunction_t func_vert_terms;

    unsigned width;
    unsigned height;
    unsigned bpc;
    unsigned grid_x;
    unsigned grid_y;
    /* Number of (term, weight) pairs pass 2 writes: one per block, or one
     * per pixel when `raster`. */
    unsigned pair_count;
    /* Pass 2 leaves the terms unreduced and collect() adds them in the CPU's
     * raster order (ADR-1400). */
    bool raster;
    /* (1 << bpc) - 1, the CPU's `samplemax`, as the kernel's double. */
    double samplemax;
    /* CPU integer_ssim.c options; host-side dB conversion. */
    bool enable_db;
    bool clip_db;
    /* vmaf_ssim_max_db(): +inf unless clip_db. */
    double max_db;

    VmafDictionary *feature_name_dict;
} IssimStateHip;

/* The CPU integer_ssim.c table: same names and defaults. */
static const VmafOption options[] = {
    {
        .name = "enable_db",
        .help = "write SSIM values as dB: -10*log10(1-ssim)",
        .offset = offsetof(IssimStateHip, enable_db),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
    },
    {
        .name = "clip_db",
        .help = "clip dB scores to a peak-derived ceiling",
        .offset = offsetof(IssimStateHip, clip_db),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
    },
    {0},
};

static size_t issim_hip_bytes_per_sample(unsigned bpc)
{
    return (bpc <= 8u) ? 1u : 2u;
}

static void issim_hip_init_dims(IssimStateHip *s, unsigned w, unsigned h, unsigned bpc)
{
    s->width = w;
    s->height = h;
    s->bpc = bpc;
    s->grid_x = (w + ISSIM_HIP_BLOCK_X - 1u) / ISSIM_HIP_BLOCK_X;
    s->grid_y = (h + ISSIM_HIP_BLOCK_Y - 1u) / ISSIM_HIP_BLOCK_Y;
    s->raster = (size_t)w * h <= ISSIM_HIP_RASTER_MAX_PIXELS;
    s->pair_count = s->raster ? w * h : s->grid_x * s->grid_y;
    s->samplemax = (double)((1u << bpc) - 1u);
    s->max_db = vmaf_ssim_max_db(s->clip_db, bpc, w, h);
}

/* Load the kernel blob and resolve the four kernels by their extern "C"
 * names. */
static int issim_hip_module_load(IssimStateHip *s, const char *fex_name)
{
#ifdef HAVE_HIPCC
    (void)fex_name;
    hipError_t rc = hipModuleLoadData(&s->module, integer_ssim_score_hsaco);
    if (rc != hipSuccess)
        return vmaf_hip_rc_to_errno(rc);
    rc = hipModuleGetFunction(&s->func_horiz_8, s->module, "integer_ssim_horiz_8bpc");
    if (rc == hipSuccess)
        rc = hipModuleGetFunction(&s->func_horiz_16, s->module, "integer_ssim_horiz_16bpc");
    if (rc == hipSuccess)
        rc = hipModuleGetFunction(&s->func_vert, s->module, "integer_ssim_vert_combine");
    if (rc == hipSuccess)
        rc = hipModuleGetFunction(&s->func_vert_terms, s->module, "integer_ssim_vert_terms");
    if (rc != hipSuccess) {
        (void)hipModuleUnload(s->module);
        s->module = NULL;
    }
    return vmaf_hip_rc_to_errno(rc);
#else
    (void)s;
    vmaf_log(VMAF_LOG_LEVEL_ERROR,
             "feature '%s' requires HIP device kernels compiled with -Denable_hipcc=true\n",
             fex_name);
    return -ENOSYS;
#endif
}

/* Release every device buffer and let go of the planes. Safe on a partially
 * allocated state. */
static int issim_hip_bufs_free(IssimStateHip *s)
{
    vmaf_hip_plane_source_close(&s->planes);
    s->ref_in = NULL;
    s->cmp_in = NULL;

    int err = 0;
    for (unsigned i = 0u; i < ISSIM_HIP_MOMENTS; i++) {
        if (s->d_moment[i] == NULL)
            continue;
        const int e = vmaf_hip_rc_to_errno(hipFree(s->d_moment[i]));
        s->d_moment[i] = NULL;
        if (err == 0)
            err = e;
    }
    return err;
}

static int issim_hip_bufs_alloc(IssimStateHip *s)
{
    const size_t plane_bytes = (size_t)s->width * s->height * sizeof(int64_t);

    hipError_t rc = hipSuccess;
    for (unsigned i = 0u; i < ISSIM_HIP_MOMENTS && rc == hipSuccess; i++)
        rc = hipMalloc(&s->d_moment[i], plane_bytes);
    if (rc != hipSuccess) {
        (void)issim_hip_bufs_free(s);
        return vmaf_hip_rc_to_errno(rc);
    }
    return 0;
}

/* Tear down everything init() may have set up, in reverse order. Every step
 * tolerates a handle that was never created, so this serves both a failed
 * init() and close(). Returns the first error. */
static int issim_hip_release(IssimStateHip *s)
{
    /* Drains the private stream first, so no kernel still uses a buffer. */
    int rc = vmaf_hip_kernel_lifecycle_close(&s->lc, s->ctx);
    int e = issim_hip_bufs_free(s);
    if (rc == 0)
        rc = e;
    if (s->module != NULL) {
        e = vmaf_hip_rc_to_errno(hipModuleUnload(s->module));
        s->module = NULL;
        if (rc == 0)
            rc = e;
    }
    e = vmaf_hip_kernel_readback_free(&s->rb_wgt, s->ctx);
    if (rc == 0)
        rc = e;
    e = vmaf_hip_kernel_readback_free(&s->rb_ssim, s->ctx);
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
    IssimStateHip *s = fex->priv;

    /* Any size works: near the border the window is truncated, as on the
     * CPU. Only an empty frame has no pixel to average over. */
    if (w == 0u || h == 0u) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "integer_ssim_hip: empty input %ux%u\n", w, h);
        return -EINVAL;
    }
    issim_hip_init_dims(s, w, h, bpc);

    int err = vmaf_hip_context_new(&s->ctx, 0);
    if (err == 0)
        err = vmaf_hip_kernel_lifecycle_init(&s->lc, s->ctx);
    if (err == 0)
        err = issim_hip_module_load(s, fex->name);
    if (err == 0) {
        err = vmaf_hip_kernel_readback_alloc(&s->rb_ssim, s->ctx,
                                             (size_t)s->pair_count * sizeof(double));
    }
    if (err == 0) {
        err = vmaf_hip_kernel_readback_alloc(&s->rb_wgt, s->ctx,
                                             (size_t)s->pair_count * sizeof(int64_t));
    }
    if (err == 0)
        err = issim_hip_bufs_alloc(s);
    if (err == 0) {
        s->feature_name_dict =
            vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
        if (s->feature_name_dict == NULL)
            err = -ENOMEM;
    }
    if (err != 0)
        (void)issim_hip_release(s);
    return err;
}

static int close_fex_hip(VmafFeatureExtractor *fex)
{
    return issim_hip_release(fex->priv);
}

/* Pass 1. The staged planes are packed, so one row is `width` samples. */
static int issim_hip_launch_horiz(IssimStateHip *s, hipStream_t str)
{
    ptrdiff_t stride = (ptrdiff_t)s->width * (ptrdiff_t)issim_hip_bytes_per_sample(s->bpc);
    void *args[] = {
        (void *)&s->ref_in,      (void *)&stride,         (void *)&s->cmp_in,
        (void *)&stride,         (void *)&s->d_moment[0], (void *)&s->d_moment[1],
        (void *)&s->d_moment[2], (void *)&s->d_moment[3], (void *)&s->d_moment[4],
        (void *)&s->d_moment[5], (void *)&s->width,       (void *)&s->height,
    };
    hipFunction_t fn = (s->bpc <= 8u) ? s->func_horiz_8 : s->func_horiz_16;
    return vmaf_hip_rc_to_errno(hipModuleLaunchKernel(fn, s->grid_x, s->grid_y, 1u,
                                                      ISSIM_HIP_BLOCK_X, ISSIM_HIP_BLOCK_Y, 1u, 0u,
                                                      str, args, NULL));
}

/* Pass 2. Implicitly ordered after pass 1: both run on `str`. */
static int issim_hip_launch_vert(IssimStateHip *s, hipStream_t str)
{
    void *args[] = {
        (void *)&s->d_moment[0],    (void *)&s->d_moment[1],   (void *)&s->d_moment[2],
        (void *)&s->d_moment[3],    (void *)&s->d_moment[4],   (void *)&s->d_moment[5],
        (void *)&s->rb_ssim.device, (void *)&s->rb_wgt.device, (void *)&s->width,
        (void *)&s->height,         (void *)&s->samplemax,
    };
    hipFunction_t fn = s->raster ? s->func_vert_terms : s->func_vert;
    return vmaf_hip_rc_to_errno(hipModuleLaunchKernel(fn, s->grid_x, s->grid_y, 1u,
                                                      ISSIM_HIP_BLOCK_X, ISSIM_HIP_BLOCK_Y, 1u, 0u,
                                                      str, args, NULL));
}

/* Copy both pair arrays back and record the `finished` event that collect()
 * waits on. */
static int issim_hip_readback(IssimStateHip *s, hipStream_t str)
{
    hipError_t rc = hipEventRecord(vmaf_hip_event_of(s->lc.submit), str);
    if (rc == hipSuccess) {
        rc = hipMemcpyAsync(s->rb_ssim.host_pinned, s->rb_ssim.device,
                            (size_t)s->pair_count * sizeof(double), hipMemcpyDeviceToHost, str);
    }
    if (rc == hipSuccess) {
        rc = hipMemcpyAsync(s->rb_wgt.host_pinned, s->rb_wgt.device,
                            (size_t)s->pair_count * sizeof(int64_t), hipMemcpyDeviceToHost, str);
    }
    if (rc != hipSuccess)
        return vmaf_hip_rc_to_errno(rc);
    return vmaf_hip_kernel_submit_post_record(&s->lc, s->ctx);
}

/* Get both luma planes on the device, packed.
 *
 * The call returns only once the pictures have been read, and that is
 * load-bearing: the caller may recycle them as soon as submit() returns, and
 * the CLI's picture pool refills a slot with the next frame right away.
 * Without the wait some frames were scored against a mix of their own and
 * the next frame's samples (off by up to 0.2 on the Netflix 576x324 pair, a
 * different set of frames on every run;
 * T-HIP-PAGEABLE-UPLOAD-RACE-2026-09-18). When the context shares the frame
 * (ADR-1408) another twin may already have uploaded the planes, and this
 * waits for nothing. */
static int issim_hip_upload(IssimStateHip *s, VmafHipSharedFrame *frame, const VmafPicture *ref_pic,
                            const VmafPicture *dist_pic)
{
    return vmaf_hip_plane_source_acquire_luma(&s->planes, frame, ref_pic, dist_pic, s->lc.str,
                                              &s->ref_in, &s->cmp_in);
}

static int submit_fex_hip(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                          VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic_90;
    (void)dist_pic_90;
    (void)index;
    IssimStateHip *s = fex->priv;
    hipStream_t str = vmaf_hip_stream_of(s->lc.str);

    int err = issim_hip_upload(s, fex->hip_frame, ref_pic, dist_pic);
    if (err == 0)
        err = issim_hip_launch_horiz(s, str);
    if (err == 0)
        err = issim_hip_launch_vert(s, str);
    if (err == 0)
        err = issim_hip_readback(s, str);
    return err;
}

static int collect_fex_hip(VmafFeatureExtractor *fex, unsigned index,
                           VmafFeatureCollector *feature_collector)
{
    IssimStateHip *s = fex->priv;

    const int err = vmaf_hip_kernel_collect_wait(&s->lc, s->ctx);
    if (err != 0)
        return err;

    const double *term_partials = s->rb_ssim.host_pinned;
    const int64_t *weight_partials = s->rb_wgt.host_pinned;
    double total_term = 0.0;
    int64_t total_weight = 0;
    /* Ascending index order is load-bearing on the per-pixel path: it is the
     * CPU's raster order, and a double sum depends on its order. */
    for (unsigned i = 0u; i < s->pair_count; i++) {
        total_term += term_partials[i];
        total_weight += weight_partials[i];
    }
    return vmaf_ssim_emit_ratio_score_named(feature_collector, s->feature_name_dict,
                                            "integer_ssim_hip", "ssim", total_term,
                                            (double)total_weight, s->enable_db, s->max_db, index);
}

static const char *provided_features[] = {"ssim", NULL};

/* integer_ssim on HIP (ADR-0564): the 9-tap int64 algorithm of the CPU
 * `ssim` extractor, flagged for model-driven dispatch under --backend hip.
 * Declared via extern in feature_extractor.cpp. */
// NOLINTNEXTLINE(misc-use-internal-linkage): cross-TU registry pattern — external linkage required (ADR-0278).
VmafFeatureExtractor vmaf_fex_integer_ssim_hip = {
    .name = "integer_ssim_hip",
    .init = init_fex_hip,
    .submit = submit_fex_hip,
    .collect = collect_fex_hip,
    .close = close_fex_hip,
    .options = options,
    .priv_size = sizeof(IssimStateHip),
    .provided_features = provided_features,
    .flags = VMAF_FEATURE_EXTRACTOR_HIP,
    .chars =
        {
            .n_dispatches_per_frame = 2,
            .is_reduction_only = false,
            .min_useful_frame_area = 1920U * 1080U,
            .dispatch_hint = VMAF_FEATURE_DISPATCH_AUTO,
        },
};

/* NOLINTEND(modernize-use-nullptr) */
