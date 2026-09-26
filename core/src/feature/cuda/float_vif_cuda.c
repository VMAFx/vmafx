/**
 *  Copyright 2016-2020 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  float_vif feature kernel on the CUDA backend (T7-23 / batch 3
 *  part 5b — ADR-0192 / ADR-0197). CUDA twin of float_vif_vulkan.
 *
 *  v1: kernelscale=1.0 only. CPU's VIF_OPT_HANDLE_BORDERS branch:
 *  per-scale dims = prev/2 (no border crop); decimate samples at
 *  (2*gx, 2*gy) with mirror padding on the input filter taps.
 *
 *  Per-frame flow: 4 compute + 3 decimate launches. Submit/collect
 *  async stream pattern matches motion_cuda.
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

#define FVIF_BX 16
#define FVIF_BY 16

typedef struct FloatVifStateCuda {
    bool debug;
    double vif_enhn_gain_limit;
    bool vif_skip_scale0;
    double vif_kernelscale;
    double vif_sigma_nsq;

    /* Stream + event pair owned by `cuda/kernel_template.h` lifecycle
     * (ADR-0246). Multi-scale 4-pyramid state stays outside the
     * template's single-pair readback bundle. */
    VmafCudaKernelLifecycle lc;
    CUfunction func_compute;
    CUfunction func_decimate;
    /* PTX module backing the VIF kernels — owned here so
     * `close_fex_cuda` can unload it. Skipping the unload leaks
     * ~200-500 KB of GPU-resident PTX backing store per vmaf_close(). */
    CUmodule module;

    VmafCudaBuffer *ref_raw;
    VmafCudaBuffer *dis_raw;
    VmafCudaBuffer *ref_buf[2];
    VmafCudaBuffer *dis_buf[2];

    VmafCudaBuffer *num_partials[4];
    VmafCudaBuffer *den_partials[4];
    float *num_host[4];
    float *den_host[4];
    unsigned wg_count[4];

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
    for (int i = 0; i < 4; i++) {
        float_vif_preserve_error(&rc,
                                 vmaf_cuda_buffer_free_owned(fex->cu_state, &s->num_partials[i]));
        float_vif_preserve_error(&rc,
                                 vmaf_cuda_buffer_free_owned(fex->cu_state, &s->den_partials[i]));
        float_vif_preserve_error(
            &rc, vmaf_cuda_buffer_host_free_owned(fex->cu_state, (void **)&s->num_host[i]));
        float_vif_preserve_error(
            &rc, vmaf_cuda_buffer_host_free_owned(fex->cu_state, (void **)&s->den_host[i]));
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

/* float_vif_alloc_buffers - raw planes, per-scale partials and the name dict.
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
    if (ret)
        return float_vif_init_unwind(fex, s, ret);

    for (int i = 0; i < 4; i++) {
        const unsigned gx = (s->scale_w[i] + FVIF_BX - 1u) / FVIF_BX;
        const unsigned gy = (s->scale_h[i] + FVIF_BY - 1u) / FVIF_BY;
        s->wg_count[i] = gx * gy;
        const size_t pbytes = (size_t)s->wg_count[i] * sizeof(float);
        ret = vmaf_cuda_buffer_alloc(fex->cu_state, &s->num_partials[i], pbytes);
        if (!ret)
            ret = vmaf_cuda_buffer_alloc(fex->cu_state, &s->den_partials[i], pbytes);
        if (!ret)
            ret = vmaf_cuda_buffer_host_alloc(fex->cu_state, (void **)&s->num_host[i], pbytes);
        if (!ret)
            ret = vmaf_cuda_buffer_host_alloc(fex->cu_state, (void **)&s->den_host[i], pbytes);
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

static int init_fex_cuda(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                         unsigned w, unsigned h)
{
    (void)pix_fmt;
    FloatVifStateCuda *s = fex->priv;
    CudaFunctions *cu_f = fex->cu_state->f;

    if (s->vif_kernelscale != 1.0)
        return -EINVAL;

    const int size_err = float_vif_check_frame_size(s, w, h);
    if (size_err)
        return size_err;

    s->width = w;
    s->height = h;
    s->bpc = bpc;
    compute_per_scale_dims(s);

    int err = vmaf_cuda_kernel_lifecycle_init(&s->lc, fex->cu_state);
    if (err)
        return float_vif_init_unwind(fex, s, err);

    int _cuda_err = 0;
    int ctx_pushed = 0;
    CHECK_CUDA_GOTO(cu_f, cuCtxPushCurrent(fex->cu_state->ctx), fail);
    ctx_pushed = 1;
    CHECK_CUDA_GOTO(cu_f, cuModuleLoadData(&s->module, float_vif_score_ptx), fail);
    CHECK_CUDA_GOTO(cu_f, cuModuleGetFunction(&s->func_compute, s->module, "float_vif_compute"),
                    fail);
    CHECK_CUDA_GOTO(cu_f, cuModuleGetFunction(&s->func_decimate, s->module, "float_vif_decimate"),
                    fail);
    CHECK_CUDA_GOTO(cu_f, cuCtxPopCurrent(NULL), fail);

    return float_vif_alloc_buffers(fex, s, w, h, bpc);

fail:
    if (ctx_pushed)
        (void)cu_f->cuCtxPopCurrent(NULL);
    return float_vif_init_unwind(fex, s, _cuda_err);
}

/* FloatVifSubmit - the per-frame constants submit_fex_cuda's seven kernel
 * launches share.
 *
 * HISS-04: introduced only so the launch blocks could move into named
 * helpers. Every field holds the value the identically named local held
 * before, so the launch arguments are bit-identical.
 */
typedef struct FloatVifSubmit {
    CUstream stream;
    ptrdiff_t raw_stride;
    CUdeviceptr ref_raw;
    CUdeviceptr dis_raw;
    CUdeviceptr ref_buf0;
    CUdeviceptr dis_buf0;
    CUdeviceptr ref_buf1;
    CUdeviceptr dis_buf1;
    float nsq;
    float egl;
    float sigma_max_inv;
} FloatVifSubmit;

/* fvif_init_submit - the per-frame half of FloatVifSubmit. */
static void fvif_init_submit(const FloatVifStateCuda *s, FloatVifSubmit *p, CUstream stream)
{
    p->stream = stream;
    p->raw_stride = (ptrdiff_t)(s->width * (s->bpc <= 8u ? 1u : 2u));
    /* Launch sequence: 4 compute + 3 decimate, one stream so launches
     * serialise naturally. */
    p->ref_raw = (CUdeviceptr)s->ref_raw->data;
    p->dis_raw = (CUdeviceptr)s->dis_raw->data;
    p->ref_buf0 = (CUdeviceptr)s->ref_buf[0]->data;
    p->dis_buf0 = (CUdeviceptr)s->dis_buf[0]->data;
    p->ref_buf1 = (CUdeviceptr)s->ref_buf[1]->data;
    p->dis_buf1 = (CUdeviceptr)s->dis_buf[1]->data;

    /* vif_sigma_nsq / vif_enhn_gain_limit are VMAF_OPT_FLAG_FEATURE_PARAM
     * options; the compute kernel used to hardcode their defaults, which
     * silently ignored every non-default value (ADR-1217).  sigma_max_inv is
     * derived exactly as the CPU does in vif_tools.c::vif_statistic_s:
     * powf(nsq, 2.0f) in float, divided in double, narrowed to float. */
    p->nsq = (float)s->vif_sigma_nsq;
    p->egl = (float)s->vif_enhn_gain_limit;
    p->sigma_max_inv = (float)(powf((float)s->vif_sigma_nsq, 2.0f) / (255.0 * 255.0));
}

/* fvif_submit_upload - H2D staging plus the partial-buffer reset.
 *
 * HISS-04: lifted verbatim out of submit_fex_cuda; the copies and the
 * memsets are still issued on the picture stream in the same order.
 */
static int fvif_submit_upload(const FloatVifStateCuda *s, CudaFunctions *cu_f,
                              const VmafPicture *ref_pic, const VmafPicture *dist_pic,
                              const FloatVifSubmit *p)
{
    CUDA_MEMCPY2D cpy = {0};
    cpy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
    cpy.srcDevice = (CUdeviceptr)ref_pic->data[0];
    cpy.srcPitch = ref_pic->stride[0];
    cpy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
    cpy.dstDevice = (CUdeviceptr)s->ref_raw->data;
    cpy.dstPitch = p->raw_stride;
    cpy.WidthInBytes = p->raw_stride;
    cpy.Height = s->height;
    CHECK_CUDA_RETURN(cu_f, cuMemcpy2DAsync(&cpy, p->stream));

    CUDA_MEMCPY2D cpy_d = cpy;
    cpy_d.srcDevice = (CUdeviceptr)dist_pic->data[0];
    cpy_d.srcPitch = dist_pic->stride[0];
    cpy_d.dstDevice = (CUdeviceptr)s->dis_raw->data;
    CHECK_CUDA_RETURN(cu_f, cuMemcpy2DAsync(&cpy_d, p->stream));

    /* Reset partials. */
    for (int i = 0; i < 4; i++) {
        CHECK_CUDA_RETURN(cu_f, cuMemsetD8Async(s->num_partials[i]->data, 0,
                                                (size_t)s->wg_count[i] * sizeof(float), p->stream));
        CHECK_CUDA_RETURN(cu_f, cuMemsetD8Async(s->den_partials[i]->data, 0,
                                                (size_t)s->wg_count[i] * sizeof(float), p->stream));
    }
    return 0;
}

/* fvif_launch_scale0 - the scale-0 compute launch, which reads the raw
 * pictures directly (the kernel selects via `is_raw = (scale == 0)`). */
static int fvif_launch_scale0(CudaFunctions *cu_f, FloatVifStateCuda *s, const FloatVifSubmit *p)
{
    int scale = 0;
    CUdeviceptr num_d = (CUdeviceptr)s->num_partials[0]->data;
    CUdeviceptr den_d = (CUdeviceptr)s->den_partials[0]->data;
    ptrdiff_t f_stride = (ptrdiff_t)s->scale_w[0];
    unsigned w = s->scale_w[0];
    unsigned h = s->scale_h[0];
    unsigned grid_x = (w + FVIF_BX - 1u) / FVIF_BX;
    unsigned grid_y = (h + FVIF_BY - 1u) / FVIF_BY;
    CUdeviceptr null_dptr = 0;
    ptrdiff_t raw_stride = p->raw_stride;
    CUdeviceptr ref_raw_d = p->ref_raw;
    CUdeviceptr dis_raw_d = p->dis_raw;
    float vif_nsq_f = p->nsq;
    float vif_egl_f = p->egl;
    float sigma_max_inv = p->sigma_max_inv;
    void *args[] = {
        &scale,          &ref_raw_d,         &dis_raw_d,         (void *)&raw_stride,
        &null_dptr,      &null_dptr,         (void *)&f_stride,  &num_d,
        &den_d,          (void *)&w,         (void *)&h,         (void *)&s->bpc,
        (void *)&grid_x, (void *)&vif_nsq_f, (void *)&vif_egl_f, (void *)&sigma_max_inv};
    CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_compute, grid_x, grid_y, 1, FVIF_BX, FVIF_BY, 1,
                                           0, p->stream, args, NULL));
    return 0;
}

/* fvif_launch_decimate - decimate the previous scale into ref_buf/dis_buf.
 *
 * The two buffers written are handed back so the compute launch that
 * follows reads exactly the ones this launch produced.
 */
static int fvif_launch_decimate(CudaFunctions *cu_f, FloatVifStateCuda *s, const FloatVifSubmit *p,
                                int next_scale, CUdeviceptr *ref_out_p, CUdeviceptr *dis_out_p)
{
    /* Decimate prev → ref_buf[(next-1)%2], dis_buf[(next-1)%2]. */
    const int dst_idx = (next_scale - 1) % 2;
    const bool prev_is_raw = (next_scale == 1);
    CUdeviceptr ref_in = prev_is_raw ? p->ref_raw : (dst_idx == 0 ? p->ref_buf1 : p->ref_buf0);
    CUdeviceptr dis_in = prev_is_raw ? p->dis_raw : (dst_idx == 0 ? p->dis_buf1 : p->dis_buf0);
    const ptrdiff_t in_f_stride = prev_is_raw ? 0 : (ptrdiff_t)s->scale_w[next_scale - 1];
    CUdeviceptr ref_out = (dst_idx == 0) ? p->ref_buf0 : p->ref_buf1;
    CUdeviceptr dis_out = (dst_idx == 0) ? p->dis_buf0 : p->dis_buf1;
    const ptrdiff_t out_f_stride = (ptrdiff_t)s->scale_w[next_scale];
    unsigned out_w = s->scale_w[next_scale];
    unsigned out_h = s->scale_h[next_scale];
    unsigned in_w = s->scale_w[next_scale - 1];
    unsigned in_h = s->scale_h[next_scale - 1];
    unsigned dec_grid_x = (out_w + FVIF_BX - 1u) / FVIF_BX;
    unsigned dec_grid_y = (out_h + FVIF_BY - 1u) / FVIF_BY;
    int scale_arg = next_scale;
    ptrdiff_t raw_stride = p->raw_stride;
    *ref_out_p = ref_out;
    *dis_out_p = dis_out;
    void *args[] = {
        &scale_arg,
        &ref_in,
        &dis_in,
        (void *)&raw_stride,
        &ref_in,
        &dis_in,
        (void *)&in_f_stride,
        &ref_out,
        &dis_out,
        (void *)&out_f_stride,
        (void *)&out_w,
        (void *)&out_h,
        (void *)&in_w,
        (void *)&in_h,
        (void *)&s->bpc,
    };
    CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_decimate, dec_grid_x, dec_grid_y, 1, FVIF_BX,
                                           FVIF_BY, 1, 0, p->stream, args, NULL));
    return 0;
}

/* fvif_launch_compute - compute at this scale on the just-written buffer. */
static int fvif_launch_compute(CudaFunctions *cu_f, FloatVifStateCuda *s, const FloatVifSubmit *p,
                               int next_scale, CUdeviceptr ref_out, CUdeviceptr dis_out)
{
    CUdeviceptr num_d = (CUdeviceptr)s->num_partials[next_scale]->data;
    CUdeviceptr den_d = (CUdeviceptr)s->den_partials[next_scale]->data;
    const ptrdiff_t comp_f_stride = (ptrdiff_t)s->scale_w[next_scale];
    unsigned w = s->scale_w[next_scale];
    unsigned h = s->scale_h[next_scale];
    unsigned grid_x = (w + FVIF_BX - 1u) / FVIF_BX;
    unsigned grid_y = (h + FVIF_BY - 1u) / FVIF_BY;
    int scale_arg2 = next_scale;
    ptrdiff_t raw_stride = p->raw_stride;
    CUdeviceptr ref_raw_d = p->ref_raw;
    CUdeviceptr dis_raw_d = p->dis_raw;
    float vif_nsq_f = p->nsq;
    float vif_egl_f = p->egl;
    float sigma_max_inv = p->sigma_max_inv;
    void *cargs[] = {&scale_arg2,
                     &ref_raw_d,
                     &dis_raw_d,
                     (void *)&raw_stride,
                     &ref_out,
                     &dis_out,
                     (void *)&comp_f_stride,
                     &num_d,
                     &den_d,
                     (void *)&w,
                     (void *)&h,
                     (void *)&s->bpc,
                     (void *)&grid_x,
                     (void *)&vif_nsq_f,
                     (void *)&vif_egl_f,
                     (void *)&sigma_max_inv};
    CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_compute, grid_x, grid_y, 1, FVIF_BX, FVIF_BY, 1,
                                           0, p->stream, cargs, NULL));
    return 0;
}

/* fvif_submit_download - sync to the event-driven stream and copy partials. */
static int fvif_submit_download(VmafFeatureExtractor *fex, FloatVifStateCuda *s,
                                CudaFunctions *cu_f, CUstream pic_stream)
{
    CHECK_CUDA_RETURN(cu_f, cuEventRecord(s->lc.submit, pic_stream));
    CHECK_CUDA_RETURN(cu_f, cuStreamWaitEvent(s->lc.str, s->lc.submit, CU_EVENT_WAIT_DEFAULT));
    for (int i = 0; i < 4; i++) {
        CHECK_CUDA_RETURN(cu_f,
                          cuMemcpyDtoHAsync(s->num_host[i], (CUdeviceptr)s->num_partials[i]->data,
                                            (size_t)s->wg_count[i] * sizeof(float), s->lc.str));
        CHECK_CUDA_RETURN(cu_f,
                          cuMemcpyDtoHAsync(s->den_host[i], (CUdeviceptr)s->den_partials[i]->data,
                                            (size_t)s->wg_count[i] * sizeof(float), s->lc.str));
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

    FloatVifSubmit p;
    fvif_init_submit(s, &p, pic_stream);
    int err = fvif_submit_upload(s, cu_f, ref_pic, dist_pic, &p);
    if (err)
        return err;
    err = fvif_launch_scale0(cu_f, s, &p);
    if (err)
        return err;

    for (int next_scale = 1; next_scale < 4; next_scale++) {
        CUdeviceptr ref_out = 0;
        CUdeviceptr dis_out = 0;
        err = fvif_launch_decimate(cu_f, s, &p, next_scale, &ref_out, &dis_out);
        if (err)
            return err;
        err = fvif_launch_compute(cu_f, s, &p, next_scale, ref_out, dis_out);
        if (err)
            return err;
    }

    /* Sync over to our event-driven stream + D2H copy partials. */
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

    double scores[8];
    for (int i = 0; i < 4; i++) {
        double n = 0.0;
        double d = 0.0;
        for (unsigned j = 0; j < s->wg_count[i]; j++) {
            n += (double)s->num_host[i][j];
            d += (double)s->den_host[i][j];
        }
        scores[2 * i + 0] = n;
        scores[2 * i + 1] = d;
    }

    const unsigned scale_start = s->vif_skip_scale0 ? 1u : 0u;
    double score_num = 0.0;
    double score_den = 0.0;
    for (unsigned scale = scale_start; scale < 4u; ++scale) {
        score_num += scores[scale * 2u];
        score_den += scores[scale * 2u + 1u];
    }

    VmafVifScoreSet output = {
        .score_num = score_num,
        .score_den = score_den,
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
