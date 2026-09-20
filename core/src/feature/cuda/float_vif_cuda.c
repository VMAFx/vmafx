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

#include "vmaf_nullptr.h"

#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

#include "common.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "vif_tools.h"
#include "log.h"

#include "cuda/float_vif_cuda.h"
#include "cuda/kernel_template.h"
#include "cuda_helper.cuh"
#include "picture.h"
#include "picture_cuda.h"

/* lint rationale: C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `VMAF_NULLPTR` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

#define FVIF_BX 16
#define FVIF_BY 16

typedef struct FloatVifStateCuda {
    bool debug;
    double vif_enhn_gain_limit;
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

static const VmafOption options[] = {{.name = "debug",
                                      .help = "debug mode: enable additional output",
                                      .offset = offsetof(FloatVifStateCuda, debug),
                                      .type = VMAF_OPT_TYPE_BOOL,
                                      .default_val.b = false},
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
                                      .help = "scaling factor for the gaussian kernel",
                                      .offset = offsetof(FloatVifStateCuda, vif_kernelscale),
                                      .type = VMAF_OPT_TYPE_DOUBLE,
                                      .default_val.d = 1.0,
                                      .min = 0.1,
                                      .max = 4.0,
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

static void free_float_vif_buffer(VmafFeatureExtractor *fex, VmafCudaBuffer **buffer)
{
    if (!*buffer)
        return;
    (void)vmaf_cuda_buffer_free(fex->cu_state, *buffer);
    free(*buffer);
    *buffer = VMAF_NULLPTR;
}

static void cleanup_float_vif_buffers(VmafFeatureExtractor *fex)
{
    FloatVifStateCuda *s = fex->priv;
    free_float_vif_buffer(fex, &s->ref_raw);
    free_float_vif_buffer(fex, &s->dis_raw);
    for (int i = 0; i < 2; i++) {
        free_float_vif_buffer(fex, &s->ref_buf[i]);
        free_float_vif_buffer(fex, &s->dis_buf[i]);
    }
    for (int i = 0; i < 4; i++) {
        free_float_vif_buffer(fex, &s->num_partials[i]);
        free_float_vif_buffer(fex, &s->den_partials[i]);
        if (s->num_host[i])
            (void)vmaf_cuda_buffer_host_free(fex->cu_state, s->num_host[i]);
        if (s->den_host[i])
            (void)vmaf_cuda_buffer_host_free(fex->cu_state, s->den_host[i]);
        s->num_host[i] = VMAF_NULLPTR;
        s->den_host[i] = VMAF_NULLPTR;
    }
    (void)vmaf_dictionary_free(&s->feature_name_dict);
    (void)vmaf_cuda_kernel_lifecycle_close(&s->lc, fex->cu_state);
}

static int init_float_vif_buffers(VmafFeatureExtractor *fex, size_t raw_bytes)
{
    FloatVifStateCuda *s = fex->priv;
    const size_t fbytes = (size_t)s->scale_w[1] * s->scale_h[1] * sizeof(float);
    int ret = vmaf_cuda_buffer_alloc(fex->cu_state, &s->ref_raw, raw_bytes);
    ret |= vmaf_cuda_buffer_alloc(fex->cu_state, &s->dis_raw, raw_bytes);
    ret |= vmaf_cuda_buffer_alloc(fex->cu_state, &s->ref_buf[0], fbytes);
    ret |= vmaf_cuda_buffer_alloc(fex->cu_state, &s->dis_buf[0], fbytes);
    ret |= vmaf_cuda_buffer_alloc(fex->cu_state, &s->ref_buf[1], fbytes);
    ret |= vmaf_cuda_buffer_alloc(fex->cu_state, &s->dis_buf[1], fbytes);
    for (int i = 0; i < 4 && !ret; i++) {
        const unsigned gx = (s->scale_w[i] + FVIF_BX - 1u) / FVIF_BX;
        const unsigned gy = (s->scale_h[i] + FVIF_BY - 1u) / FVIF_BY;
        s->wg_count[i] = gx * gy;
        const size_t pbytes = (size_t)s->wg_count[i] * sizeof(float);
        ret |= vmaf_cuda_buffer_alloc(fex->cu_state, &s->num_partials[i], pbytes);
        ret |= vmaf_cuda_buffer_alloc(fex->cu_state, &s->den_partials[i], pbytes);
        ret |= vmaf_cuda_buffer_host_alloc(fex->cu_state, (void **)&s->num_host[i], pbytes);
        ret |= vmaf_cuda_buffer_host_alloc(fex->cu_state, (void **)&s->den_host[i], pbytes);
    }
    if (!ret) {
        s->feature_name_dict =
            vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
        ret = s->feature_name_dict ? 0 : -ENOMEM;
    }
    if (ret)
        cleanup_float_vif_buffers(fex);
    return ret ? -ENOMEM : 0;
}

static int init_fex_cuda(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                         unsigned w, unsigned h)
{
    (void)pix_fmt;
    FloatVifStateCuda *s = fex->priv;
    CudaFunctions *cu_f = fex->cu_state->f;

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
                 "float_vif_cuda: width and height must be >= %d for the four-scale VIF "
                 "ladder (got %ux%u)\n",
                 vif_min_dim, w, h);
        return -EINVAL;
    }

    s->width = w;
    s->height = h;
    s->bpc = bpc;
    compute_per_scale_dims(s);

    int err = vmaf_cuda_kernel_lifecycle_init(&s->lc, fex->cu_state);
    if (err)
        return err;

    int _cuda_err;
    int ctx_pushed = 0;
    CHECK_CUDA_GOTO(cu_f, cuCtxPushCurrent(fex->cu_state->ctx), fail);
    ctx_pushed = 1;
    CHECK_CUDA_GOTO(cu_f, cuModuleLoadData(&s->module, float_vif_score_ptx), fail);
    CHECK_CUDA_GOTO(cu_f, cuModuleGetFunction(&s->func_compute, s->module, "float_vif_compute"),
                    fail);
    CHECK_CUDA_GOTO(cu_f, cuModuleGetFunction(&s->func_decimate, s->module, "float_vif_decimate"),
                    fail);
    CHECK_CUDA_GOTO(cu_f, cuCtxPopCurrent(VMAF_NULLPTR), fail_after_pop);

    const size_t bpp = (bpc <= 8u) ? 1u : 2u;
    return init_float_vif_buffers(fex, (size_t)w * h * bpp);

fail:
    if (ctx_pushed)
        (void)cu_f->cuCtxPopCurrent(VMAF_NULLPTR);
fail_after_pop:
    (void)vmaf_cuda_kernel_lifecycle_close(&s->lc, fex->cu_state);
    return _cuda_err;
}

typedef struct FloatVifLaunchContext {
    CUdeviceptr ref_raw;
    CUdeviceptr dis_raw;
    ptrdiff_t raw_stride;
    float noise;
    float gain_limit;
    float sigma_max_inv;
    CUstream stream;
    CudaFunctions *cu_f;
} FloatVifLaunchContext;

static int prepare_float_vif_submit(FloatVifStateCuda *s, const VmafPicture *ref_pic,
                                    const VmafPicture *dist_pic, FloatVifLaunchContext *ctx)
{
    CUDA_MEMCPY2D cpy;
    memset(&cpy, 0, sizeof(cpy));
    cpy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
    cpy.srcDevice = (CUdeviceptr)ref_pic->data[0];
    cpy.srcPitch = ref_pic->stride[0];
    cpy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
    cpy.dstDevice = ctx->ref_raw;
    cpy.dstPitch = ctx->raw_stride;
    cpy.WidthInBytes = ctx->raw_stride;
    cpy.Height = s->height;
    CHECK_CUDA_RETURN(ctx->cu_f, cuMemcpy2DAsync(&cpy, ctx->stream));
    cpy.srcDevice = (CUdeviceptr)dist_pic->data[0];
    cpy.srcPitch = dist_pic->stride[0];
    cpy.dstDevice = ctx->dis_raw;
    CHECK_CUDA_RETURN(ctx->cu_f, cuMemcpy2DAsync(&cpy, ctx->stream));
    for (int i = 0; i < 4; i++) {
        const size_t bytes = (size_t)s->wg_count[i] * sizeof(float);
        CHECK_CUDA_RETURN(ctx->cu_f,
                          cuMemsetD8Async(s->num_partials[i]->data, 0, bytes, ctx->stream));
        CHECK_CUDA_RETURN(ctx->cu_f,
                          cuMemsetD8Async(s->den_partials[i]->data, 0, bytes, ctx->stream));
    }
    return 0;
}

static int launch_float_vif_compute(FloatVifStateCuda *s, FloatVifLaunchContext *ctx, int scale)
{
    const int buffer_index = (scale - 1) % 2;
    CUdeviceptr ref_float = scale ? (CUdeviceptr)s->ref_buf[buffer_index]->data : 0;
    CUdeviceptr dis_float = scale ? (CUdeviceptr)s->dis_buf[buffer_index]->data : 0;
    CUdeviceptr num = (CUdeviceptr)s->num_partials[scale]->data;
    CUdeviceptr den = (CUdeviceptr)s->den_partials[scale]->data;
    ptrdiff_t float_stride = (ptrdiff_t)s->scale_w[scale];
    unsigned w = s->scale_w[scale];
    unsigned h = s->scale_h[scale];
    unsigned grid_x = (w + FVIF_BX - 1u) / FVIF_BX;
    unsigned grid_y = (h + FVIF_BY - 1u) / FVIF_BY;
    void *args[] = {&scale,
                    &ctx->ref_raw,
                    &ctx->dis_raw,
                    &ctx->raw_stride,
                    &ref_float,
                    &dis_float,
                    &float_stride,
                    &num,
                    &den,
                    &w,
                    &h,
                    &s->bpc,
                    &grid_x,
                    &ctx->noise,
                    &ctx->gain_limit,
                    &ctx->sigma_max_inv};
    CHECK_CUDA_RETURN(ctx->cu_f, cuLaunchKernel(s->func_compute, grid_x, grid_y, 1, FVIF_BX,
                                                FVIF_BY, 1, 0, ctx->stream, args, VMAF_NULLPTR));
    return 0;
}

static int launch_float_vif_decimate(FloatVifStateCuda *s, FloatVifLaunchContext *ctx,
                                     int next_scale)
{
    const int dst_index = (next_scale - 1) % 2;
    const bool raw_input = next_scale == 1;
    CUdeviceptr ref_in = raw_input ? ctx->ref_raw : (CUdeviceptr)s->ref_buf[1 - dst_index]->data;
    CUdeviceptr dis_in = raw_input ? ctx->dis_raw : (CUdeviceptr)s->dis_buf[1 - dst_index]->data;
    CUdeviceptr ref_out = (CUdeviceptr)s->ref_buf[dst_index]->data;
    CUdeviceptr dis_out = (CUdeviceptr)s->dis_buf[dst_index]->data;
    ptrdiff_t in_stride = raw_input ? 0 : (ptrdiff_t)s->scale_w[next_scale - 1];
    ptrdiff_t out_stride = (ptrdiff_t)s->scale_w[next_scale];
    unsigned out_w = s->scale_w[next_scale];
    unsigned out_h = s->scale_h[next_scale];
    unsigned in_w = s->scale_w[next_scale - 1];
    unsigned in_h = s->scale_h[next_scale - 1];
    unsigned grid_x = (out_w + FVIF_BX - 1u) / FVIF_BX;
    unsigned grid_y = (out_h + FVIF_BY - 1u) / FVIF_BY;
    void *args[] = {&next_scale, &ref_in,  &dis_in,  &ctx->raw_stride, &ref_in, &dis_in,
                    &in_stride,  &ref_out, &dis_out, &out_stride,      &out_w,  &out_h,
                    &in_w,       &in_h,    &s->bpc};
    CHECK_CUDA_RETURN(ctx->cu_f, cuLaunchKernel(s->func_decimate, grid_x, grid_y, 1, FVIF_BX,
                                                FVIF_BY, 1, 0, ctx->stream, args, VMAF_NULLPTR));
    return 0;
}

static int finish_float_vif_submit(VmafFeatureExtractor *fex, FloatVifLaunchContext *ctx)
{
    FloatVifStateCuda *s = fex->priv;
    CHECK_CUDA_RETURN(ctx->cu_f, cuEventRecord(s->lc.submit, ctx->stream));
    CHECK_CUDA_RETURN(ctx->cu_f, cuStreamWaitEvent(s->lc.str, s->lc.submit, CU_EVENT_WAIT_DEFAULT));
    for (int i = 0; i < 4; i++) {
        const size_t bytes = (size_t)s->wg_count[i] * sizeof(float);
        CHECK_CUDA_RETURN(ctx->cu_f, cuMemcpyDtoHAsync(s->num_host[i], s->num_partials[i]->data,
                                                       bytes, s->lc.str));
        CHECK_CUDA_RETURN(ctx->cu_f, cuMemcpyDtoHAsync(s->den_host[i], s->den_partials[i]->data,
                                                       bytes, s->lc.str));
    }
    return vmaf_cuda_kernel_submit_post_record(&s->lc, fex->cu_state);
}

static int submit_fex_cuda(VmafFeatureExtractor *fex, const VmafPicture *ref_pic, const VmafPicture *ref_pic_90,
                           const VmafPicture *dist_pic, const VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic_90;
    (void)dist_pic_90;
    (void)index;
    FloatVifStateCuda *s = fex->priv;
    CudaFunctions *cu_f = fex->cu_state->f;

    const ptrdiff_t raw_stride = (ptrdiff_t)s->width * (s->bpc <= 8u ? 1 : 2);

    CUstream pic_stream = vmaf_cuda_picture_get_stream(ref_pic);
    CHECK_CUDA_RETURN(cu_f,
                      cuStreamWaitEvent(pic_stream, vmaf_cuda_picture_get_ready_event(dist_pic),
                                        CU_EVENT_WAIT_DEFAULT));

    FloatVifLaunchContext ctx = {
        .ref_raw = (CUdeviceptr)s->ref_raw->data,
        .dis_raw = (CUdeviceptr)s->dis_raw->data,
        .raw_stride = raw_stride,
        .noise = (float)s->vif_sigma_nsq,
        .gain_limit = (float)s->vif_enhn_gain_limit,
        .sigma_max_inv = (float)(powf((float)s->vif_sigma_nsq, 2.0f) / (255.0 * 255.0)),
        .stream = pic_stream,
        .cu_f = cu_f,
    };
    int err = prepare_float_vif_submit(s, ref_pic, dist_pic, &ctx);
    if (err)
        return err;
    err = launch_float_vif_compute(s, &ctx, 0);
    for (int scale = 1; scale < 4 && !err; scale++) {
        err = launch_float_vif_decimate(s, &ctx, scale);
        if (!err)
            err = launch_float_vif_compute(s, &ctx, scale);
    }
    return err ? err : finish_float_vif_submit(fex, &ctx);
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

    int err = 0;
    err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "VMAF_feature_vif_scale0_score",
                                                   scores[0] / scores[1], index);
    err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "VMAF_feature_vif_scale1_score",
                                                   scores[2] / scores[3], index);
    err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "VMAF_feature_vif_scale2_score",
                                                   scores[4] / scores[5], index);
    err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "VMAF_feature_vif_scale3_score",
                                                   scores[6] / scores[7], index);

    if (s->debug && !err) {
        double score_num = scores[0] + scores[2] + scores[4] + scores[6];
        double score_den = scores[1] + scores[3] + scores[5] + scores[7];
        double score = score_den == 0.0 ? 1.0 : score_num / score_den;
        err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                       "vif", score, index);
        err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                       "vif_num", score_num, index);
        err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                       "vif_den", score_den, index);
        const char *names[8] = {"vif_num_scale0", "vif_den_scale0", "vif_num_scale1",
                                "vif_den_scale1", "vif_num_scale2", "vif_den_scale2",
                                "vif_num_scale3", "vif_den_scale3"};
        for (int i = 0; i < 8; i++) {
            err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                           names[i], scores[i], index);
        }
    }

    return err;
}

static int close_fex_cuda(VmafFeatureExtractor *fex)
{
    FloatVifStateCuda *s = fex->priv;
    int ret = vmaf_cuda_kernel_lifecycle_close(&s->lc, fex->cu_state);
    if (s->ref_raw) {
        ret |= vmaf_cuda_buffer_free(fex->cu_state, s->ref_raw);
        free(s->ref_raw);
    }
    if (s->dis_raw) {
        ret |= vmaf_cuda_buffer_free(fex->cu_state, s->dis_raw);
        free(s->dis_raw);
    }
    for (int i = 0; i < 2; i++) {
        if (s->ref_buf[i]) {
            ret |= vmaf_cuda_buffer_free(fex->cu_state, s->ref_buf[i]);
            free(s->ref_buf[i]);
        }
        if (s->dis_buf[i]) {
            ret |= vmaf_cuda_buffer_free(fex->cu_state, s->dis_buf[i]);
            free(s->dis_buf[i]);
        }
    }
    for (int i = 0; i < 4; i++) {
        if (s->num_partials[i]) {
            ret |= vmaf_cuda_buffer_free(fex->cu_state, s->num_partials[i]);
            free(s->num_partials[i]);
        }
        if (s->den_partials[i]) {
            ret |= vmaf_cuda_buffer_free(fex->cu_state, s->den_partials[i]);
            free(s->den_partials[i]);
        }
        if (s->num_host[i])
            ret |= vmaf_cuda_buffer_host_free(fex->cu_state, s->num_host[i]);
        if (s->den_host[i])
            ret |= vmaf_cuda_buffer_host_free(fex->cu_state, s->den_host[i]);
    }
    ret |= vmaf_dictionary_free(&s->feature_name_dict);
    const CudaFunctions *cu_f = fex->cu_state->f;
    if (cu_f && s->module)
        (void)cu_f->cuModuleUnload(s->module);
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
                                          VMAF_NULLPTR};

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
