/**
 *  Copyright 2016-2020 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  float_vif feature kernel on the CUDA backend (T7-23 / batch 3
 *  part 5b — ADR-0192 / ADR-0197).
 *
 *  kernelscale=1.0 only. CPU's VIF_OPT_HANDLE_BORDERS branch:
 *  per-scale dims = prev/2 (no border crop); decimate samples at
 *  (2*gx, 2*gy) with mirror padding on the input filter taps.
 *
 *  Per-frame flow: per scale one compute launch and one row-sum launch,
 *  preceded from scale 1 on by a decimate launch. Submit/collect async
 *  stream pattern matches motion_cuda.
 *
 *  The twin returns the CPU extractor's values bit for bit (ADR-1412): the
 *  Gaussian taps are vif_get_filter()'s, computed here as float_vif.c
 *  computes them and handed to the kernels; the kernels evaluate
 *  vif_tools.c's arithmetic (float_vif/float_vif_device.h) and add the
 *  per-pixel terms of each row in one thread; fvif_sum_rows() adds the rows
 *  here, top to bottom, in the reference's fp32 accumulators.
 */

#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

#include "common.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "feature/nonfinite_score.h"
#include "vif_tools.h"
#include "log.h"

#include "cuda/float_vif/float_vif_device.h"
#include "cuda/float_vif_cuda.h"
#include "cuda/kernel_template.h"
#include "cuda_helper.cuh"
#include "picture.h"
#include "picture_cuda.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

typedef struct FloatVifStateCuda {
    bool debug;
    double vif_enhn_gain_limit;
    bool vif_skip_scale0;
    double vif_kernelscale;
    double vif_sigma_nsq;
    double vif_scale1_min_val;
    double vif_scale2_min_val;
    double vif_scale3_min_val;

    /* Stream + event pair owned by `cuda/kernel_template.h` lifecycle
     * (ADR-0246). Multi-scale 4-pyramid state stays outside the
     * template's single-pair readback bundle. */
    VmafCudaKernelLifecycle lc;
    CUfunction func_compute;
    CUfunction func_decimate;
    CUfunction func_row_sums;
    /* PTX module backing the VIF kernels — owned here so
     * `close_fex_cuda` can unload it. Skipping the unload leaks
     * ~200-500 KB of GPU-resident PTX backing store per vmaf_close(). */
    CUmodule module;

    VmafCudaBuffer *ref_raw;
    VmafCudaBuffer *dis_raw;
    VmafCudaBuffer *ref_buf[2];
    VmafCudaBuffer *dis_buf[2];

    /* The numerator and denominator term of every pixel of the scale being
     * computed (sized for scale 0, reused by the others on the same stream),
     * then per scale the sums of each row, on the device and read back. */
    VmafCudaBuffer *terms;
    VmafCudaBuffer *rows[FVIF_SCALES];
    float *rows_host[FVIF_SCALES];

    /* vif_get_filter() per scale, as float_vif.c caches it. */
    FloatVifCudaTaps taps[FVIF_SCALES];

    unsigned width;
    unsigned height;
    unsigned bpc;
    unsigned scale_w[4];
    unsigned scale_h[4];

    VmafDictionary *feature_name_dict;
} FloatVifStateCuda;

static const VmafOption options[] = {
    {.name = "debug",
     .help = "debug mode: enable additional output",
     .offset = offsetof(FloatVifStateCuda, debug),
     .type = VMAF_OPT_TYPE_BOOL,
     .default_val.b = false},
    {
        .name = "vif_skip_scale0",
        .alias = "ssclz",
        .help = "skip scale 0 (finest scale) VIF computation; "
                "score0 is forced to 0.0 (parity with CPU option)",
        .offset = offsetof(FloatVifStateCuda, vif_skip_scale0),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {.name = "vif_enhn_gain_limit",
     .alias = "egl",
     .help = "enhancement gain imposed on vif (>= 1.0)",
     .offset = offsetof(FloatVifStateCuda, vif_enhn_gain_limit),
     .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val.d = 100.0,
     .min = 1.0,
     .max = 100.0,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "vif_kernelscale",
     .alias = "ks",
     .help = "scaling factor for the gaussian kernel",
     .offset = offsetof(FloatVifStateCuda, vif_kernelscale),
     .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val.d = 1.0,
     .min = 0.1,
     .max = 4.0,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM | VMAF_OPT_FLAG_DEFAULT_ONLY},
    {.name = "vif_scale1_min_val",
     .alias = "s1miv",
     .help = "minimum value allowed; smaller values will be set to this value",
     .offset = offsetof(FloatVifStateCuda, vif_scale1_min_val),
     .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val.d = 0.0,
     .min = 0.0,
     .max = 1.0,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "vif_scale2_min_val",
     .alias = "s2miv",
     .help = "minimum value allowed; smaller values will be set to this value",
     .offset = offsetof(FloatVifStateCuda, vif_scale2_min_val),
     .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val.d = 0.0,
     .min = 0.0,
     .max = 1.0,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "vif_scale3_min_val",
     .alias = "s3miv",
     .help = "minimum value allowed; smaller values will be set to this value",
     .offset = offsetof(FloatVifStateCuda, vif_scale3_min_val),
     .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val.d = 0.0,
     .min = 0.0,
     .max = 1.0,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "vif_sigma_nsq",
     .alias = "snsq",
     .help = "neural noise variance",
     .offset = offsetof(FloatVifStateCuda, vif_sigma_nsq),
     .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val.d = 2.0,
     .min = 0.0,
     .max = 5.0,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {0}};

static void compute_per_scale_dims(FloatVifStateCuda *s)
{
    s->scale_w[0] = s->width;
    s->scale_h[0] = s->height;
    for (int i = 1; i < 4; i++) {
        s->scale_w[i] = s->scale_w[i - 1] / 2u;
        s->scale_h[i] = s->scale_h[i - 1] / 2u;
    }
}

/* ------------------------------------------------------------------ */
static void float_vif_preserve_error(int *rc, int err)
{
    if (!*rc)
        *rc = err;
}

static int float_vif_release_buffers(VmafFeatureExtractor *fex, FloatVifStateCuda *s, int rc)
{
    float_vif_preserve_error(&rc, vmaf_cuda_buffer_free_owned(fex->cu_state, &s->ref_raw));
    float_vif_preserve_error(&rc, vmaf_cuda_buffer_free_owned(fex->cu_state, &s->dis_raw));
    for (int i = 0; i < 2; i++) {
        float_vif_preserve_error(&rc, vmaf_cuda_buffer_free_owned(fex->cu_state, &s->ref_buf[i]));
        float_vif_preserve_error(&rc, vmaf_cuda_buffer_free_owned(fex->cu_state, &s->dis_buf[i]));
    }
    float_vif_preserve_error(&rc, vmaf_cuda_buffer_free_owned(fex->cu_state, &s->terms));
    for (int i = 0; i < FVIF_SCALES; i++) {
        float_vif_preserve_error(&rc, vmaf_cuda_buffer_free_owned(fex->cu_state, &s->rows[i]));
        float_vif_preserve_error(
            &rc, vmaf_cuda_buffer_host_free_owned(fex->cu_state, (void **)&s->rows_host[i]));
    }
    return rc;
}

/* float_vif_init_unwind - the single teardown path for init_fex_cuda.
 *
 * Drain the lifecycle before releasing anything queued work may reference.
 * The original init failure remains the first returned error.
 */
static int float_vif_init_unwind(VmafFeatureExtractor *fex, FloatVifStateCuda *s, int err)
{
    const int lifecycle_rc = vmaf_cuda_kernel_lifecycle_close(&s->lc, fex->cu_state);
    if (lifecycle_rc)
        return err ? err : lifecycle_rc;

    int rc = float_vif_release_buffers(fex, s, err);
    float_vif_preserve_error(&rc, vmaf_dictionary_free(&s->feature_name_dict));
    float_vif_preserve_error(&rc, vmaf_cuda_module_unload(fex->cu_state, &s->module));
    return rc;
}

/* float_vif_rows_bytes - the per-row sums of one scale: num and den per row. */
static size_t float_vif_rows_bytes(const FloatVifStateCuda *s, int scale)
{
    return (size_t)s->scale_h[scale] * FVIF_TERM_FLOATS * sizeof(float);
}

/* float_vif_alloc_buffers - raw planes, the term plane, per-scale row sums and
 * the name dict.
 *
 * The per-scale loop bounds and byte sizes match the scoring path. Each
 * allocation stops on its first failure and unwinds through the owned helpers.
 */
static int float_vif_alloc_buffers(VmafFeatureExtractor *fex, FloatVifStateCuda *s, unsigned w,
                                   unsigned h, unsigned bpc)
{
    const size_t bpp = (bpc <= 8u) ? 1u : 2u;
    const size_t raw_bytes = (size_t)w * h * bpp;
    int ret = vmaf_cuda_buffer_alloc(fex->cu_state, &s->ref_raw, raw_bytes);
    if (!ret)
        ret = vmaf_cuda_buffer_alloc(fex->cu_state, &s->dis_raw, raw_bytes);
    const size_t fbytes = (size_t)s->scale_w[1] * s->scale_h[1] * sizeof(float);
    if (!ret)
        ret = vmaf_cuda_buffer_alloc(fex->cu_state, &s->ref_buf[0], fbytes);
    if (!ret)
        ret = vmaf_cuda_buffer_alloc(fex->cu_state, &s->dis_buf[0], fbytes);
    if (!ret)
        ret = vmaf_cuda_buffer_alloc(fex->cu_state, &s->ref_buf[1], fbytes);
    if (!ret)
        ret = vmaf_cuda_buffer_alloc(fex->cu_state, &s->dis_buf[1], fbytes);
    const size_t term_bytes = (size_t)w * h * FVIF_TERM_FLOATS * sizeof(float);
    if (!ret)
        ret = vmaf_cuda_buffer_alloc(fex->cu_state, &s->terms, term_bytes);
    if (ret)
        return float_vif_init_unwind(fex, s, ret);

    for (int i = 0; i < FVIF_SCALES; i++) {
        const size_t row_bytes = float_vif_rows_bytes(s, i);
        ret = vmaf_cuda_buffer_alloc(fex->cu_state, &s->rows[i], row_bytes);
        if (!ret)
            ret = vmaf_cuda_buffer_host_alloc(fex->cu_state, (void **)&s->rows_host[i], row_bytes);
        if (ret)
            break;
    }
    if (ret)
        return float_vif_init_unwind(fex, s, ret);

    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (!s->feature_name_dict)
        return float_vif_init_unwind(fex, s, -ENOMEM);
    return 0;
}

/* float_vif_check_frame_size - enforce the four-scale VIF dimension floor.
 *
 * HISS-04: the entry guard of init_fex_cuda, moved whole - same
 * vif_get_min_dim() call, same comparison, same message, same -EINVAL.
 */
static int float_vif_check_frame_size(const FloatVifStateCuda *s, unsigned w, unsigned h)
{
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
                 "float_vif_cuda: width and height must be >= %d for the four-scale VIF "
                 "ladder (got %ux%u)\n",
                 vif_min_dim, w, h);
        return -EINVAL;
    }
    return 0;
}

/* float_vif_init_taps - each scale's Gaussian, from the CPU's own routine.
 *
 * float_vif.c::init() fills its filter cache with vif_get_filter_size() and
 * vif_get_filter() for (float)vif_kernelscale; the same two calls here give
 * the kernels the same fp32 taps. A filter wider than the kernels' tile halo
 * is refused (it cannot occur at the only kernelscale init accepts).
 */
static int float_vif_init_taps(FloatVifStateCuda *s)
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

/* float_vif_load_module - load the fatbin and resolve the three kernels.
 *
 * The context is popped again on every path; the caller unwinds.
 */
static int float_vif_load_module(VmafFeatureExtractor *fex, FloatVifStateCuda *s)
{
    CudaFunctions *cu_f = fex->cu_state->f;
    int _cuda_err = 0;
    int ctx_pushed = 0;
    CHECK_CUDA_GOTO(cu_f, cuCtxPushCurrent(fex->cu_state->ctx), fail);
    ctx_pushed = 1;
    CHECK_CUDA_GOTO(cu_f, cuModuleLoadData(&s->module, float_vif_score_ptx), fail);
    CHECK_CUDA_GOTO(cu_f, cuModuleGetFunction(&s->func_compute, s->module, "float_vif_compute"),
                    fail);
    CHECK_CUDA_GOTO(cu_f, cuModuleGetFunction(&s->func_decimate, s->module, "float_vif_decimate"),
                    fail);
    CHECK_CUDA_GOTO(cu_f, cuModuleGetFunction(&s->func_row_sums, s->module, "float_vif_row_sums"),
                    fail);
    CHECK_CUDA_GOTO(cu_f, cuCtxPopCurrent(NULL), fail);
    return 0;

fail:
    if (ctx_pushed)
        (void)cu_f->cuCtxPopCurrent(NULL);
    return _cuda_err;
}

static int init_fex_cuda(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                         unsigned w, unsigned h)
{
    (void)pix_fmt;
    FloatVifStateCuda *s = fex->priv;

    if (s->vif_kernelscale != 1.0)
        return -EINVAL;

    const int size_err = float_vif_check_frame_size(s, w, h);
    if (size_err)
        return size_err;
    const int taps_err = float_vif_init_taps(s);
    if (taps_err)
        return taps_err;

    s->width = w;
    s->height = h;
    s->bpc = bpc;
    compute_per_scale_dims(s);

    int err = vmaf_cuda_kernel_lifecycle_init(&s->lc, fex->cu_state);
    if (!err)
        err = float_vif_load_module(fex, s);
    if (err)
        return float_vif_init_unwind(fex, s, err);
    return float_vif_alloc_buffers(fex, s, w, h, bpc);
}

/* fvif_copy_luma - one luma plane into its tightly packed raw buffer. */
static int fvif_copy_luma(const FloatVifStateCuda *s, CudaFunctions *cu_f, const VmafPicture *pic,
                          const VmafCudaBuffer *raw, CUstream stream)
{
    const size_t raw_stride = (size_t)s->width * (s->bpc <= 8u ? 1u : 2u);
    const CUDA_MEMCPY2D copy = {
        .srcMemoryType = CU_MEMORYTYPE_DEVICE,
        .srcDevice = (CUdeviceptr)pic->data[0],
        .srcPitch = pic->stride[0],
        .dstMemoryType = CU_MEMORYTYPE_DEVICE,
        .dstDevice = (CUdeviceptr)raw->data,
        .dstPitch = raw_stride,
        .WidthInBytes = raw_stride,
        .Height = s->height,
    };
    CHECK_CUDA_RETURN(cu_f, cuMemcpy2DAsync(&copy, stream));
    return 0;
}

/* fvif_submit_upload - copy both luma planes into the raw buffers the kernels
 * read. */
static int fvif_submit_upload(const FloatVifStateCuda *s, CudaFunctions *cu_f,
                              const VmafPicture *ref_pic, const VmafPicture *dist_pic,
                              CUstream stream)
{
    const int err = fvif_copy_luma(s, cu_f, ref_pic, s->ref_raw, stream);
    return err ? err : fvif_copy_luma(s, cu_f, dist_pic, s->dis_raw, stream);
}

/* fvif_scale_input - the planes scale `scale` is computed from: the raw luma
 * planes at scale 0, otherwise the pair the decimate launch of that scale
 * wrote (scale 1 and 3 into buffer 0, scale 2 into buffer 1). */
static FloatVifCudaInput fvif_scale_input(const FloatVifStateCuda *s, int scale)
{
    FloatVifCudaInput in = {
        .width = s->scale_w[scale],
        .height = s->scale_h[scale],
        .bpc = s->bpc,
    };
    if (scale == 0) {
        in.ref = (uint64_t)s->ref_raw->data;
        in.dis = (uint64_t)s->dis_raw->data;
        in.stride = (int64_t)s->width * (s->bpc <= 8u ? 1 : 2);
        in.is_raw = 1u;
        return in;
    }
    const int idx = (scale - 1) % 2;
    in.ref = (uint64_t)s->ref_buf[idx]->data;
    in.dis = (uint64_t)s->dis_buf[idx]->data;
    in.stride = (int64_t)s->scale_w[scale];
    return in;
}

/* fvif_launch_decimate - filter scale `scale - 1` with this scale's taps and
 * keep every second sample: the input of scale `scale`. */
static int fvif_launch_decimate(CudaFunctions *cu_f, FloatVifStateCuda *s, int scale,
                                CUstream stream)
{
    const FloatVifCudaInput out = fvif_scale_input(s, scale);
    FloatVifCudaDecimateArgs args = {
        .in = fvif_scale_input(s, scale - 1),
        .taps = s->taps[scale],
        .ref_out = out.ref,
        .dis_out = out.dis,
        .out_width = out.width,
        .out_height = out.height,
    };
    const unsigned grid_x = (out.width + FVIF_BX - 1u) / FVIF_BX;
    const unsigned grid_y = (out.height + FVIF_BY - 1u) / FVIF_BY;
    void *params[] = {&args};
    CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_decimate, grid_x, grid_y, 1, FVIF_BX, FVIF_BY, 1,
                                           0, stream, params, NULL));
    return 0;
}

/* fvif_launch_scale - the per-pixel terms of one scale, then their row sums.
 *
 * vif_sigma_nsq / vif_enhn_gain_limit are VMAF_OPT_FLAG_FEATURE_PARAM options
 * and reach the kernel as arguments (ADR-1217). vif_sigma_nsq stays a double
 * and sigma_max_inv is derived as vif_tools.c::vif_statistic_s derives it:
 * powf(nsq, 2.0f) in float, divided in double, narrowed to float.
 */
static int fvif_launch_scale(CudaFunctions *cu_f, FloatVifStateCuda *s, int scale, CUstream stream)
{
    FloatVifCudaComputeArgs args = {
        .in = fvif_scale_input(s, scale),
        .taps = s->taps[scale],
        .terms = (uint64_t)s->terms->data,
        .vif_sigma_nsq = s->vif_sigma_nsq,
        .vif_enhn_gain_limit = (float)s->vif_enhn_gain_limit,
        .sigma_max_inv = (float)(powf((float)s->vif_sigma_nsq, 2.0f) / (255.0 * 255.0)),
    };
    const unsigned grid_x = (args.in.width + FVIF_BX - 1u) / FVIF_BX;
    const unsigned grid_y = (args.in.height + FVIF_BY - 1u) / FVIF_BY;
    void *params[] = {&args};
    CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_compute, grid_x, grid_y, 1, FVIF_BX, FVIF_BY, 1,
                                           0, stream, params, NULL));

    FloatVifCudaRowArgs row_args = {
        .terms = (uint64_t)s->terms->data,
        .rows = (uint64_t)s->rows[scale]->data,
        .width = args.in.width,
        .height = args.in.height,
    };
    const unsigned row_grid = (args.in.height + FVIF_ROW_THREADS - 1u) / FVIF_ROW_THREADS;
    void *row_params[] = {&row_args};
    CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_row_sums, row_grid, 1, 1, FVIF_ROW_THREADS, 1, 1,
                                           0, stream, row_params, NULL));
    return 0;
}

/* fvif_submit_download - sync to the event-driven stream and copy the row
 * sums of every scale. */
static int fvif_submit_download(VmafFeatureExtractor *fex, FloatVifStateCuda *s,
                                CudaFunctions *cu_f, CUstream pic_stream)
{
    CHECK_CUDA_RETURN(cu_f, cuEventRecord(s->lc.submit, pic_stream));
    CHECK_CUDA_RETURN(cu_f, cuStreamWaitEvent(s->lc.str, s->lc.submit, CU_EVENT_WAIT_DEFAULT));
    for (int i = 0; i < FVIF_SCALES; i++) {
        CHECK_CUDA_RETURN(cu_f, cuMemcpyDtoHAsync(s->rows_host[i], (CUdeviceptr)s->rows[i]->data,
                                                  float_vif_rows_bytes(s, i), s->lc.str));
    }
    return vmaf_cuda_kernel_submit_post_record(&s->lc, fex->cu_state);
}

static int submit_fex_cuda(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                           VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic_90;
    (void)dist_pic_90;
    (void)index;
    FloatVifStateCuda *s = fex->priv;
    CudaFunctions *cu_f = fex->cu_state->f;

    CUstream pic_stream = vmaf_cuda_picture_get_stream(ref_pic);
    CHECK_CUDA_RETURN(cu_f,
                      cuStreamWaitEvent(pic_stream, vmaf_cuda_picture_get_ready_event(dist_pic),
                                        CU_EVENT_WAIT_DEFAULT));

    int err = fvif_submit_upload(s, cu_f, ref_pic, dist_pic, pic_stream);
    for (int scale = 0; scale < FVIF_SCALES && !err; scale++) {
        if (scale > 0)
            err = fvif_launch_decimate(cu_f, s, scale, pic_stream);
        if (!err)
            err = fvif_launch_scale(cu_f, s, scale, pic_stream);
    }
    if (err)
        return err;

    /* Sync over to our event-driven stream + D2H copy of the row sums. */
    return fvif_submit_download(fex, s, cu_f, pic_stream);
}

static int collect_fex_cuda(VmafFeatureExtractor *fex, unsigned index,
                            VmafFeatureCollector *feature_collector)
{
    FloatVifStateCuda *s = fex->priv;
    /* Drain via the template helper so engine-scope fence batching
     * (T-GPU-OPT-1, ADR-0242) can short-circuit the per-stream
     * cuStreamSynchronize when the engine has already waited on
     * lc.finished as part of a batched drain. */
    int sync_err = vmaf_cuda_kernel_collect_wait(&s->lc, fex->cu_state);
    if (sync_err) {
        return sync_err;
    }

    /* compute_vif(): each scale's num and den are the fp32 sums of
     * vif_statistic_s(), widened to double. */
    double scores[2 * FVIF_SCALES];
    for (size_t i = 0u; i < FVIF_SCALES; i++)
        fvif_sum_rows(s->rows_host[i], s->scale_h[i], &scores[2u * i], &scores[2u * i + 1u]);

    const size_t scale_start = s->vif_skip_scale0 ? 1u : 0u;
    double score_num = 0.0;
    double score_den = 0.0;
    for (size_t scale = scale_start; scale < FVIF_SCALES; ++scale) {
        score_num += scores[scale * 2u];
        score_den += scores[scale * 2u + 1u];
    }

    VmafVifScoreSet output = {
        .score_num = score_num,
        .score_den = score_den,
        .minimum = {s->vif_scale1_min_val, s->vif_scale2_min_val, s->vif_scale3_min_val},
        .use_minimums = true,
        .skip_scale0 = s->vif_skip_scale0,
        .debug = s->debug,
    };
    for (size_t i = 0u; i < 8u; ++i)
        output.scale[i] = scores[i];
    output.score = output.score_den > 0.0 ? output.score_num / output.score_den : NAN;
    return vmaf_vif_emit_scores(feature_collector, s->feature_name_dict, "float_vif_cuda", &output,
                                VMAF_VIF_FLOAT_NAMES, index);
}

static int close_fex_cuda(VmafFeatureExtractor *fex)
{
    FloatVifStateCuda *s = fex->priv;
    int ret = vmaf_cuda_kernel_lifecycle_close(&s->lc, fex->cu_state);
    if (ret)
        return ret;

    ret = float_vif_release_buffers(fex, s, 0);
    float_vif_preserve_error(&ret, vmaf_dictionary_free(&s->feature_name_dict));
    float_vif_preserve_error(&ret, vmaf_cuda_module_unload(fex->cu_state, &s->module));
    return ret;
}

static const char *provided_features[] = {"VMAF_feature_vif_scale0_score",
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
                                          NULL};

// NOLINTNEXTLINE(misc-use-internal-linkage): cross-TU registry pattern — external linkage required; referenced as `extern VmafFeatureExtractor vmaf_fex_float_vif_cuda` by feature_extractor.cpp's feature_extractor_list[] (ADR-0278).
VmafFeatureExtractor vmaf_fex_float_vif_cuda = {
    .name = "float_vif_cuda",
    .init = init_fex_cuda,
    .submit = submit_fex_cuda,
    .collect = collect_fex_cuda,
    .close = close_fex_cuda,
    .options = options,
    .priv_size = sizeof(FloatVifStateCuda),
    .provided_features = provided_features,
    .flags = VMAF_FEATURE_EXTRACTOR_CUDA,
};

/* NOLINTEND(modernize-use-nullptr) */
