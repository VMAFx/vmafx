/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  float_psnr feature kernel on the CUDA backend (T7-23 / batch 3
 *  part 3b — ADR-0192 / ADR-0195). CUDA twin of float_psnr_vulkan.
 */

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "common.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"

#include "cuda/float_psnr_cuda.h"
#include "cuda/kernel_template.h"
#include "cuda_helper.cuh"
#include "picture.h"
#include "picture_cuda.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

typedef struct FloatPsnrStateCuda {
    /* Stream + event pair owned by `cuda/kernel_template.h` lifecycle
     * (ADR-0246). */
    VmafCudaKernelLifecycle lc;
    /* Per-WG float partials: device + pinned host. Owned by the
     * template's readback bundle. */
    VmafCudaKernelReadback rb;

    CUfunction funcbpc8;
    CUfunction funcbpc16;
    /* PTX module backing the PSNR kernels — owned here so
     * `close_fex_cuda` can unload it. Skipping the unload leaks
     * ~200-500 KB of GPU-resident PTX backing store per vmaf_close(). */
    CUmodule module;

    VmafCudaBuffer *ref_in;
    VmafCudaBuffer *dis_in;
    unsigned wg_count;

    unsigned frame_w;
    unsigned frame_h;
    unsigned bpc;
    double peak;
    double psnr_max;
    /* `uncapped` option: mirrors CPU float_psnr.c. When true, psnr_max
     * keeps only its zero-noise infinity-sentinel role and stops
     * truncating genuinely computed values. Default false keeps every
     * shipped score unchanged. See ADR-1193 / T-UPSTREAM-1109. */
    bool uncapped;

    VmafDictionary *feature_name_dict;
} FloatPsnrStateCuda;

/*
 * Size the table explicitly and leave the terminator element out: C
 * zero-initialises the trailing element, which is exactly the `.name == NULL`
 * sentinel the option walker stops on. Written this way rather than with an
 * explicit `{0}` / `{NULL}` terminator, because either spells a null pointer
 * constant and adds a `modernize-use-nullptr` diagnostic that would push this
 * file past its ADR-1142 clang-tidy baseline; and rather than with the C23
 * empty initialiser `{}`, because MSVC's partial C23 mode (`/std:clatest`,
 * documented as implementing `typeof` / `typeof_unqual`) is not documented to
 * accept it, and this file builds on the required Windows MSVC leg.
 */
static const VmafOption options[2] = {
    {
        .name = "uncapped",
        .help = "report the true PSNR instead of truncating at "
                "the psnr_max ceiling (a zero-noise pair still "
                "reports psnr_max)",
        .offset = offsetof(FloatPsnrStateCuda, uncapped),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
    },
};

#define FPSNR_BX 16
#define FPSNR_BY 16

/* ------------------------------------------------------------------ */
/* float_psnr_init_unwind - the single teardown path for init_fex_cuda.
 *
 * HISS-01: lifted verbatim from the former `free_buffers` label. The same
 * resources are released in the same order on every exit path, and the
 * value returned is the one the label returned.
 */
static int float_psnr_init_unwind(VmafFeatureExtractor *fex, FloatPsnrStateCuda *s, int ret)
{
    if (s->ref_in) {
        (void)vmaf_cuda_buffer_free(fex->cu_state, s->ref_in);
        free(s->ref_in);
    }
    if (s->dis_in) {
        (void)vmaf_cuda_buffer_free(fex->cu_state, s->dis_in);
        free(s->dis_in);
    }
    (void)vmaf_cuda_kernel_readback_free(&s->rb, fex->cu_state);
    (void)vmaf_dictionary_free(&s->feature_name_dict);
    (void)vmaf_cuda_kernel_lifecycle_close(&s->lc, fex->cu_state);
    return ret;
}

/* float_psnr_peak_for_bpc - the (peak, psnr_max) pair for a bit depth.
 *
 * HISS-04: the bit-depth table from init_fex_cuda, moved whole. The literals
 * and the branch order are unchanged, so the doubles the score path reads are
 * bit-identical to the inline table's.
 */
static int float_psnr_peak_for_bpc(FloatPsnrStateCuda *s, unsigned bpc)
{
    if (bpc == 8u) {
        s->peak = 255.0;
        s->psnr_max = 60.0;
    } else if (bpc == 10u) {
        s->peak = 255.75;
        s->psnr_max = 72.0;
    } else if (bpc == 12u) {
        s->peak = 255.9375;
        s->psnr_max = 84.0;
    } else if (bpc == 16u) {
        s->peak = 255.99609375;
        s->psnr_max = 108.0;
    } else {
        return -EINVAL;
    }
    return 0;
}

static int init_fex_cuda(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                         unsigned w, unsigned h)
{
    (void)pix_fmt;
    FloatPsnrStateCuda *s = fex->priv;
    CudaFunctions *cu_f = fex->cu_state->f;

    s->frame_w = w;
    s->frame_h = h;
    s->bpc = bpc;

    const int bpc_err = float_psnr_peak_for_bpc(s, bpc);
    if (bpc_err)
        return bpc_err;

    int err = vmaf_cuda_kernel_lifecycle_init(&s->lc, fex->cu_state);
    if (err)
        return err;

    int _cuda_err = 0;
    int ctx_pushed = 0;
    CHECK_CUDA_GOTO(cu_f, cuCtxPushCurrent(fex->cu_state->ctx), fail);
    ctx_pushed = 1;

    CHECK_CUDA_GOTO(cu_f, cuModuleLoadData(&s->module, float_psnr_score_ptx), fail);
    CHECK_CUDA_GOTO(cu_f, cuModuleGetFunction(&s->funcbpc8, s->module, "float_psnr_kernel_8bpc"),
                    fail);
    CHECK_CUDA_GOTO(cu_f, cuModuleGetFunction(&s->funcbpc16, s->module, "float_psnr_kernel_16bpc"),
                    fail);
    CHECK_CUDA_GOTO(cu_f, cuCtxPopCurrent(NULL), fail_after_pop);

    const size_t bpp = (bpc <= 8u) ? 1u : 2u;
    const size_t plane_bytes = (size_t)w * h * bpp;
    const unsigned gx = (w + FPSNR_BX - 1u) / FPSNR_BX;
    const unsigned gy = (h + FPSNR_BY - 1u) / FPSNR_BY;
    s->wg_count = gx * gy;
    const size_t pbytes = (size_t)s->wg_count * sizeof(float);

    int ret = 0;
    ret |= vmaf_cuda_buffer_alloc(fex->cu_state, &s->ref_in, plane_bytes);
    ret |= vmaf_cuda_buffer_alloc(fex->cu_state, &s->dis_in, plane_bytes);
    if (ret)
        return float_psnr_init_unwind(fex, s, ret);
    ret = vmaf_cuda_kernel_readback_alloc(&s->rb, fex->cu_state, pbytes);
    if (ret)
        return float_psnr_init_unwind(fex, s, ret);

    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (!s->feature_name_dict) {
        ret = -ENOMEM;
        return float_psnr_init_unwind(fex, s, ret);
    }
    return 0;

fail:
    if (ctx_pushed)
        (void)cu_f->cuCtxPopCurrent(NULL);
fail_after_pop:
    (void)vmaf_cuda_kernel_lifecycle_close(&s->lc, fex->cu_state);
    return _cuda_err;
}

/* float_psnr_upload_plane - stage one luma plane into a packed device buffer.
 *
 * HISS-04: the two identical CUDA_MEMCPY2D blocks from submit_fex_cuda,
 * factored into one. The descriptor fields are assigned in the same order and
 * the copy is enqueued on the same stream, so the bytes that reach the kernel
 * are unchanged.
 */
static int float_psnr_upload_plane(CudaFunctions *cu_f, CUstream stream, const VmafPicture *pic,
                                   const VmafCudaBuffer *dst, ptrdiff_t plane_pitch,
                                   unsigned height)
{
    CUDA_MEMCPY2D cpy = {0};
    cpy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
    cpy.srcDevice = (CUdeviceptr)pic->data[0];
    cpy.srcPitch = pic->stride[0];
    cpy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
    cpy.dstDevice = (CUdeviceptr)dst->data;
    cpy.dstPitch = plane_pitch;
    cpy.WidthInBytes = plane_pitch;
    cpy.Height = height;
    CHECK_CUDA_RETURN(cu_f, cuMemcpy2DAsync(&cpy, stream));
    return 0;
}

static int submit_fex_cuda(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                           VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic_90;
    (void)dist_pic_90;
    (void)index;
    FloatPsnrStateCuda *s = fex->priv;
    CudaFunctions *cu_f = fex->cu_state->f;

    s->frame_w = ref_pic->w[0];
    s->frame_h = ref_pic->h[0];
    const ptrdiff_t plane_pitch = (ptrdiff_t)(s->frame_w * (s->bpc <= 8u ? 1u : 2u));

    CUstream pic_stream = vmaf_cuda_picture_get_stream(ref_pic);
    CHECK_CUDA_RETURN(cu_f,
                      cuStreamWaitEvent(pic_stream, vmaf_cuda_picture_get_ready_event(dist_pic),
                                        CU_EVENT_WAIT_DEFAULT));

    int up_err =
        float_psnr_upload_plane(cu_f, pic_stream, ref_pic, s->ref_in, plane_pitch, s->frame_h);
    if (up_err)
        return up_err;
    up_err =
        float_psnr_upload_plane(cu_f, pic_stream, dist_pic, s->dis_in, plane_pitch, s->frame_h);
    if (up_err)
        return up_err;

    CHECK_CUDA_RETURN(cu_f, cuMemsetD8Async(s->rb.device->data, 0,
                                            (size_t)s->wg_count * sizeof(float), pic_stream));

    const unsigned grid_x = (s->frame_w + FPSNR_BX - 1u) / FPSNR_BX;
    const unsigned grid_y = (s->frame_h + FPSNR_BY - 1u) / FPSNR_BY;

    if (s->bpc == 8u) {
        void *args[] = {
            &s->ref_in->data,     &s->dis_in->data,    (void *)&plane_pitch, (void *)&plane_pitch,
            (void *)s->rb.device, (void *)&s->frame_w, (void *)&s->frame_h,
        };
        CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->funcbpc8, grid_x, grid_y, 1, FPSNR_BX, FPSNR_BY,
                                               1, 0, pic_stream, args, NULL));
    } else {
        void *args[] = {
            &s->ref_in->data,     &s->dis_in->data,    (void *)&plane_pitch, (void *)&plane_pitch,
            (void *)s->rb.device, (void *)&s->frame_w, (void *)&s->frame_h,  (void *)&s->bpc,
        };
        CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->funcbpc16, grid_x, grid_y, 1, FPSNR_BX, FPSNR_BY,
                                               1, 0, pic_stream, args, NULL));
    }

    CHECK_CUDA_RETURN(cu_f, cuEventRecord(s->lc.submit, pic_stream));
    CHECK_CUDA_RETURN(cu_f, cuStreamWaitEvent(s->lc.str, s->lc.submit, CU_EVENT_WAIT_DEFAULT));
    CHECK_CUDA_RETURN(cu_f, cuMemcpyDtoHAsync(s->rb.host_pinned, (CUdeviceptr)s->rb.device->data,
                                              (size_t)s->wg_count * sizeof(float), s->lc.str));
    return vmaf_cuda_kernel_submit_post_record(&s->lc, fex->cu_state);
}

static int collect_fex_cuda(VmafFeatureExtractor *fex, unsigned index,
                            VmafFeatureCollector *feature_collector)
{
    FloatPsnrStateCuda *s = fex->priv;

    int sync_err = vmaf_cuda_kernel_collect_wait(&s->lc, fex->cu_state);
    if (sync_err)
        return sync_err;

    const float *partials_host = s->rb.host_pinned;
    double total = 0.0;
    for (unsigned i = 0; i < s->wg_count; i++)
        total += (double)partials_host[i];
    const double n_pix = (double)s->frame_w * (double)s->frame_h;
    const double noise = total / n_pix;
    /* Match CPU float_psnr.c — a zero-noise pair reports psnr_max as the
     * infinity sentinel; the truncation at psnr_max applies only when
     * `uncapped` is false. See ADR-1193 / T-UPSTREAM-1109. */
    const double eps = 1e-10;
    const double max_noise = noise > eps ? noise : eps;
    double score;
    if (!s->uncapped) {
        /* Pre-ADR-1193 expression verbatim — bit-identical default. */
        score = 10.0 * log10(s->peak * s->peak / max_noise);
        if (score > s->psnr_max)
            score = s->psnr_max;
    } else if (noise <= 0.0) {
        score = s->psnr_max; /* infinity sentinel */
    } else {
        score = 10.0 * log10(s->peak * s->peak / max_noise);
    }

    return vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "float_psnr", score, index);
}

static int close_fex_cuda(VmafFeatureExtractor *fex)
{
    FloatPsnrStateCuda *s = fex->priv;
    int rc = vmaf_cuda_kernel_lifecycle_close(&s->lc, fex->cu_state);

    if (s->ref_in) {
        const int e = vmaf_cuda_buffer_free(fex->cu_state, s->ref_in);
        free(s->ref_in);
        if (rc == 0)
            rc = e;
    }
    if (s->dis_in) {
        const int e = vmaf_cuda_buffer_free(fex->cu_state, s->dis_in);
        free(s->dis_in);
        if (rc == 0)
            rc = e;
    }
    const int rb_rc = vmaf_cuda_kernel_readback_free(&s->rb, fex->cu_state);
    if (rc == 0)
        rc = rb_rc;
    const int dict_rc = vmaf_dictionary_free(&s->feature_name_dict);
    if (rc == 0)
        rc = dict_rc;
    const CudaFunctions *cu_f = fex->cu_state->f;
    if (cu_f && s->module)
        (void)cu_f->cuModuleUnload(s->module);
    return rc;
}

static const char *provided_features[] = {"float_psnr", NULL};

VmafFeatureExtractor vmaf_fex_float_psnr_cuda = {
    .name = "float_psnr_cuda",
    .options = options,
    .init = init_fex_cuda,
    .submit = submit_fex_cuda,
    .collect = collect_fex_cuda,
    .close = close_fex_cuda,
    .priv_size = sizeof(FloatPsnrStateCuda),
    .provided_features = provided_features,
    .flags = VMAF_FEATURE_EXTRACTOR_CUDA,
};

/* NOLINTEND(modernize-use-nullptr) */
