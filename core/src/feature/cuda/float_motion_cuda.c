/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  float_motion feature kernel on the CUDA backend (T7-23 / batch 3
 *  part 4b — ADR-0192 / ADR-0196). CUDA twin of float_motion_vulkan.
 *
 *  Submit/collect async pattern matches motion_cuda. Float blur into
 *  ping-pong float buffer; SAD against previous frame's blur. motion2
 *  emitted at index-1 with min(prev_motion_score, motion_score).
 */

#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

#include "common.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "log.h"

#include "cuda/float_motion_cuda.h"
#include "cuda/kernel_template.h"
#include "cuda_helper.cuh"
#include "picture.h"
#include "picture_cuda.h"

typedef struct FloatMotionStateCuda {
    /* Stream + event pair owned by `cuda/kernel_template.h` lifecycle
     * (ADR-0246). */
    VmafCudaKernelLifecycle lc;
    /* Per-WG SAD float partials: device + pinned host. Owned by the
     * template's readback bundle. */
    VmafCudaKernelReadback rb;

    CUfunction funcbpc8;
    CUfunction funcbpc16;
    /* PTX module backing the motion kernels — owned here so
     * `close_fex_cuda` can unload it. Skipping the unload leaks
     * ~200-500 KB of GPU-resident PTX backing store per vmaf_close(). */
    CUmodule module;

    VmafCudaBuffer *ref_in;
    VmafCudaBuffer *blur[2];
    int cur_blur;
    unsigned wg_count;

    unsigned index;
    unsigned frame_w;
    unsigned frame_h;
    unsigned bpc;
    double prev_motion_score;
    double motion_fps_weight;
    bool debug;
    bool motion_force_zero;

    VmafDictionary *feature_name_dict;
} FloatMotionStateCuda;

static const VmafOption options[] = {
    {
        .name = "debug",
        .help = "debug mode: enable additional output",
        .offset = offsetof(FloatMotionStateCuda, debug),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = true,
    },
    {
        .name = "motion_force_zero",
        .help = "force motion score to zero",
        .offset = offsetof(FloatMotionStateCuda, motion_force_zero),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "motion_fps_weight",
        .alias = "mfw",
        .help = "fps-aware multiplicative weight/correction",
        .offset = offsetof(FloatMotionStateCuda, motion_fps_weight),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = 1.0,
        .min = 0.0,
        .max = 5.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {0}};

#define FM_BX 16
#define FM_BY 16

static int extract_force_zero(VmafFeatureExtractor *fex, VmafPicture *ref_pic,
                              VmafPicture *ref_pic_90, VmafPicture *dist_pic,
                              VmafPicture *dist_pic_90, unsigned index,
                              VmafFeatureCollector *feature_collector)
{
    (void)ref_pic;
    (void)ref_pic_90;
    (void)dist_pic;
    (void)dist_pic_90;
    FloatMotionStateCuda *s = fex->priv;

    int err = vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                      "VMAF_feature_motion2_score", 0.0, index);
    if (s->debug && !err) {
        err = vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                      "VMAF_feature_motion_score", 0.0, index);
    }
    return err;
}

static int init_fex_cuda(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                         unsigned w, unsigned h)
{
    (void)pix_fmt;
    FloatMotionStateCuda *s = fex->priv;
    CudaFunctions *cu_f = fex->cu_state->f;

    /* The 5-tap CUDA float_motion kernel uses reflect-101 mirror padding;
     * mirror() returns 2*sup - idx - 2, which is negative when sup < 3.
     * Refuse smaller frames up front to prevent out-of-bounds device reads.
     * Minimum: filter_width/2 + 1 = 3. */
    if (h < 3u || w < 3u) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "float_motion_cuda: frame %ux%u is below the 5-tap filter minimum 3x3; "
                 "refusing to avoid out-of-bounds mirror reads on device\n",
                 w, h);
        return -EINVAL;
    }

    s->frame_w = w;
    s->frame_h = h;
    s->bpc = bpc;
    s->index = 0;
    s->prev_motion_score = 0.0;
    s->cur_blur = 0;

    int err = vmaf_cuda_kernel_lifecycle_init(&s->lc, fex->cu_state);
    if (err)
        return err;

    int _cuda_err = 0;
    int ctx_pushed = 0;
    CHECK_CUDA_GOTO(cu_f, cuCtxPushCurrent(fex->cu_state->ctx), fail);
    ctx_pushed = 1;

    CHECK_CUDA_GOTO(cu_f, cuModuleLoadData(&s->module, float_motion_score_ptx), fail);
    CHECK_CUDA_GOTO(cu_f, cuModuleGetFunction(&s->funcbpc8, s->module, "float_motion_kernel_8bpc"),
                    fail);
    CHECK_CUDA_GOTO(
        cu_f, cuModuleGetFunction(&s->funcbpc16, s->module, "float_motion_kernel_16bpc"), fail);
    CHECK_CUDA_GOTO(cu_f, cuCtxPopCurrent(NULL), fail_after_pop);

    if (s->motion_force_zero) {
        fex->extract = extract_force_zero;
        fex->submit = NULL;
        fex->collect = NULL;
        fex->flush = NULL;
        fex->close = NULL;
        return 0;
    }

    const size_t bpp = (bpc <= 8u) ? 1u : 2u;
    const size_t plane_bytes = (size_t)w * h * bpp;
    const size_t blur_bytes = (size_t)w * h * sizeof(float);
    const unsigned gx = (w + FM_BX - 1u) / FM_BX;
    const unsigned gy = (h + FM_BY - 1u) / FM_BY;
    s->wg_count = gx * gy;
    const size_t pbytes = (size_t)s->wg_count * sizeof(float);

    int ret = 0;
    ret |= vmaf_cuda_buffer_alloc(fex->cu_state, &s->ref_in, plane_bytes);
    ret |= vmaf_cuda_buffer_alloc(fex->cu_state, &s->blur[0], blur_bytes);
    ret |= vmaf_cuda_buffer_alloc(fex->cu_state, &s->blur[1], blur_bytes);
    if (ret)
        goto free_buffers;
    ret = vmaf_cuda_kernel_readback_alloc(&s->rb, fex->cu_state, pbytes);
    if (ret)
        goto free_buffers;

    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (!s->feature_name_dict) {
        ret = -ENOMEM;
        goto free_buffers;
    }
    return 0;

free_buffers:
    if (s->ref_in) {
        (void)vmaf_cuda_buffer_free(fex->cu_state, s->ref_in);
        free(s->ref_in);
    }
    if (s->blur[0]) {
        (void)vmaf_cuda_buffer_free(fex->cu_state, s->blur[0]);
        free(s->blur[0]);
    }
    if (s->blur[1]) {
        (void)vmaf_cuda_buffer_free(fex->cu_state, s->blur[1]);
        free(s->blur[1]);
    }
    (void)vmaf_cuda_kernel_readback_free(&s->rb, fex->cu_state);
    (void)vmaf_dictionary_free(&s->feature_name_dict);
    (void)vmaf_cuda_kernel_lifecycle_close(&s->lc, fex->cu_state);
    return ret;

fail:
    if (ctx_pushed)
        (void)cu_f->cuCtxPopCurrent(NULL);
fail_after_pop:
    (void)vmaf_cuda_kernel_lifecycle_close(&s->lc, fex->cu_state);
    return _cuda_err;
}

static int submit_fex_cuda(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                           VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index)
{
    (void)dist_pic;
    (void)ref_pic_90;
    (void)dist_pic_90;
    FloatMotionStateCuda *s = fex->priv;
    CudaFunctions *cu_f = fex->cu_state->f;

    s->index = index;
    s->frame_w = ref_pic->w[0];
    s->frame_h = ref_pic->h[0];
    const ptrdiff_t plane_pitch = (ptrdiff_t)(s->frame_w * (s->bpc <= 8u ? 1u : 2u));

    CUstream pic_stream = vmaf_cuda_picture_get_stream(ref_pic);

    /* Cache cur ref pixels into ref_in via D2D copy. */
    CHECK_CUDA_RETURN(cu_f,
                      cuStreamWaitEvent(pic_stream, vmaf_cuda_picture_get_ready_event(ref_pic),
                                        CU_EVENT_WAIT_DEFAULT));
    CUDA_MEMCPY2D copy = {0};
    copy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
    copy.srcDevice = (CUdeviceptr)ref_pic->data[0];
    copy.srcPitch = ref_pic->stride[0];
    copy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
    copy.dstDevice = (CUdeviceptr)s->ref_in->data;
    copy.dstPitch = plane_pitch;
    copy.WidthInBytes = plane_pitch;
    copy.Height = s->frame_h;
    CHECK_CUDA_RETURN(cu_f, cuMemcpy2DAsync(&copy, pic_stream));

    const unsigned cur_idx = (unsigned)s->cur_blur;
    const unsigned prev_idx = 1u - cur_idx;
    const unsigned compute_sad = (s->index > 0) ? 1u : 0u;
    const unsigned grid_x = (s->frame_w + FM_BX - 1u) / FM_BX;
    const unsigned grid_y = (s->frame_h + FM_BY - 1u) / FM_BY;

    if (s->bpc == 8u) {
        void *args[] = {
            &s->ref_in->data,         (void *)&plane_pitch, &s->blur[cur_idx]->data,
            &s->blur[prev_idx]->data, (void *)s->rb.device, (void *)&s->frame_w,
            (void *)&s->frame_h,      (void *)&compute_sad,
        };
        CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->funcbpc8, grid_x, grid_y, 1, FM_BX, FM_BY, 1, 0,
                                               pic_stream, args, NULL));
    } else {
        void *args[] = {
            &s->ref_in->data,         (void *)&plane_pitch, &s->blur[cur_idx]->data,
            &s->blur[prev_idx]->data, (void *)s->rb.device, (void *)&s->frame_w,
            (void *)&s->frame_h,      (void *)&s->bpc,      (void *)&compute_sad,
        };
        CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->funcbpc16, grid_x, grid_y, 1, FM_BX, FM_BY, 1, 0,
                                               pic_stream, args, NULL));
    }

    CHECK_CUDA_RETURN(cu_f, cuEventRecord(s->lc.submit, pic_stream));
    CHECK_CUDA_RETURN(cu_f, cuStreamWaitEvent(s->lc.str, s->lc.submit, CU_EVENT_WAIT_DEFAULT));

    if (s->index > 0) {
        CHECK_CUDA_RETURN(cu_f, cuMemcpyDtoHAsync(((float *)s->rb.host_pinned),
                                                  (CUdeviceptr)s->rb.device->data,
                                                  (size_t)s->wg_count * sizeof(float), s->lc.str));
        return vmaf_cuda_kernel_submit_post_record(&s->lc, fex->cu_state);
    }
    return 0;
}

static double reduce_sad(const FloatMotionStateCuda *s)
{
    double total = 0.0;
    for (unsigned i = 0; i < s->wg_count; i++)
        total += (double)((float *)s->rb.host_pinned)[i];
    return total / ((double)s->frame_w * s->frame_h);
}

static int collect_fex_cuda(VmafFeatureExtractor *fex, unsigned index,
                            VmafFeatureCollector *feature_collector)
{
    FloatMotionStateCuda *s = fex->priv;
    int sync_err = vmaf_cuda_kernel_collect_wait(&s->lc, fex->cu_state);
    if (sync_err)
        return sync_err;

    int err = 0;

    if (index == 0) {
        err = vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                      "VMAF_feature_motion2_score", 0.0, index);
        if (s->debug && !err)
            err = vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                          "VMAF_feature_motion_score", 0.0, index);
        s->cur_blur = 1 - s->cur_blur;
        return err;
    }

    const double motion_score = reduce_sad(s);

    if (index == 1) {
        if (s->debug) {
            err = vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                          "VMAF_feature_motion_score", motion_score,
                                                          index);
        }
    } else {
        /* Apply fps weight before taking the min — mirrors float_motion.c CPU path.
         * Bit-exact when motion_fps_weight = 1.0 (default). */
        const double w_cur = motion_score * s->motion_fps_weight;
        const double w_prev = s->prev_motion_score * s->motion_fps_weight;
        const double motion2 = (w_cur < w_prev) ? w_cur : w_prev;
        err = vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                      "VMAF_feature_motion2_score", motion2,
                                                      index - 1);
        if (s->debug && !err)
            err = vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                          "VMAF_feature_motion_score", motion_score,
                                                          index);
    }

    s->prev_motion_score = motion_score;
    s->cur_blur = 1 - s->cur_blur;
    return err;
}

static int flush_fex_cuda(VmafFeatureExtractor *fex, VmafFeatureCollector *feature_collector)
{
    FloatMotionStateCuda *s = fex->priv;
    int sync_err = vmaf_cuda_kernel_collect_wait(&s->lc, fex->cu_state);
    if (sync_err)
        return sync_err;

    int ret = 0;
    if (s->index > 0) {
        /* Idempotency guard: same rationale as integer_motion_cuda's
         * flush — the post-#312 flush_context_cuda may have already
         * written motion2_score[s->index] via the pending-collect.
         * Probe and skip in that case (re-append would trip the
         * "cannot be overwritten" warning and surface as
         * "context could not be synchronized"). */
        double existing;
        if (vmaf_feature_collector_get_score(feature_collector, "VMAF_feature_motion2_score",
                                             &existing, s->index) != 0) {
            /* Apply fps weight on the tail motion2 — mirrors the collect path.
             * Bit-exact when motion_fps_weight = 1.0 (default). */
            ret = vmaf_feature_collector_append_with_dict(
                feature_collector, s->feature_name_dict, "VMAF_feature_motion2_score",
                s->prev_motion_score * s->motion_fps_weight, s->index);
        }
    }
    return (ret < 0) ? ret : !ret;
}

static int close_fex_cuda(VmafFeatureExtractor *fex)
{
    FloatMotionStateCuda *s = fex->priv;
    int ret = vmaf_cuda_kernel_lifecycle_close(&s->lc, fex->cu_state);

    if (s->ref_in) {
        ret |= vmaf_cuda_buffer_free(fex->cu_state, s->ref_in);
        free(s->ref_in);
    }
    if (s->blur[0]) {
        ret |= vmaf_cuda_buffer_free(fex->cu_state, s->blur[0]);
        free(s->blur[0]);
    }
    if (s->blur[1]) {
        ret |= vmaf_cuda_buffer_free(fex->cu_state, s->blur[1]);
        free(s->blur[1]);
    }
    ret |= vmaf_cuda_kernel_readback_free(&s->rb, fex->cu_state);
    ret |= vmaf_dictionary_free(&s->feature_name_dict);
    const CudaFunctions *cu_f = fex->cu_state->f;
    if (cu_f && s->module)
        (void)cu_f->cuModuleUnload(s->module);
    return ret;
}

static const char *provided_features[] = {"VMAF_feature_motion_score", "VMAF_feature_motion2_score",
                                          NULL};

VmafFeatureExtractor vmaf_fex_float_motion_cuda = {
    .name = "float_motion_cuda",
    .init = init_fex_cuda,
    .submit = submit_fex_cuda,
    .collect = collect_fex_cuda,
    .flush = flush_fex_cuda,
    .close = close_fex_cuda,
    .options = options,
    .priv_size = sizeof(FloatMotionStateCuda),
    .provided_features = provided_features,
    .flags = VMAF_FEATURE_EXTRACTOR_TEMPORAL | VMAF_FEATURE_EXTRACTOR_CUDA,
};
