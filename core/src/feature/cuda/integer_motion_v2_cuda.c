/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  motion_v2 feature kernel on the CUDA backend (T7-23 / batch 3
 *  part 1b — ADR-0192 / ADR-0193). CUDA twin of motion_v2_vulkan
 *  (PR #146).
 *
 *  Stateless variant of `motion_cuda`: exploits convolution
 *  linearity (`SAD(blur(prev), blur(cur)) == sum(|blur(prev - cur)|)`)
 *  so each frame computes its score in one kernel launch over
 *  (prev_ref - cur_ref) without storing blurred frames across
 *  submits. Uses a raw-pixel ping-pong (`pix[2]`) instead of
 *  motion_cuda's blurred-frame ping-pong — one D2D copy per submit
 *  caches the current ref Y plane so the next frame can read it as
 *  "prev".
 *
 *  motion2_v2_score = min(score[i], score[i+1]) is emitted host-side
 *  in flush() (same shape as CPU integer_motion_v2.c::flush). No GPU
 *  work needed; the kernel only emits motion_v2_sad_score.
 */

#include <errno.h>
#include <string.h>

#include "common.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "log.h"

#include "cuda/integer_motion_v2_cuda.h"
#include "cuda/kernel_template.h"
#include "cuda_helper.cuh"
#include "picture.h"
#include "picture_cuda.h"

typedef struct MotionV2StateCuda {
    /* Stream + event pair owned by `cuda/kernel_template.h` lifecycle
     * (ADR-0246). */
    VmafCudaKernelLifecycle lc;
    /* Single int64 atomic accumulator per frame: device + pinned
     * host. Owned by the template's readback bundle. */
    VmafCudaKernelReadback rb;

    CUfunction funcbpc8;
    CUfunction funcbpc16;
    /* PTX module backing the motion_v2 kernels — owned here so
     * `close_fex_cuda` can unload it. Skipping the unload leaks
     * ~200-500 KB of GPU-resident PTX backing store per vmaf_close(). */
    CUmodule module;

    /* Ping-pong of raw ref Y planes (uint8 for bpc<=8, uint16 for
     * bpc>8 — bytes_per_pixel * w * h). pix[index%2] is the current
     * frame's slot; pix[(index+1)%2] is the previous frame's slot.
     * Kept outside the template's readback bundle because the
     * template models a single device+host pair, not a ping-pong
     * of device-only buffers. */
    VmafCudaBuffer *pix[2];

    unsigned index;
    unsigned frame_w;
    unsigned frame_h;
    unsigned bpc;
    size_t plane_bytes;
    double motion_fps_weight;

    VmafDictionary *feature_name_dict;
} MotionV2StateCuda;

static const VmafOption options[] = {{
                                         .name = "motion_fps_weight",
                                         .alias = "mfw",
                                         .help = "fps-aware multiplicative weight/correction",
                                         .offset = offsetof(MotionV2StateCuda, motion_fps_weight),
                                         .type = VMAF_OPT_TYPE_DOUBLE,
                                         .default_val.d = 1.0,
                                         .min = 0.0,
                                         .max = 5.0,
                                         .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
                                     },
                                     {0}};

static int init_fex_cuda(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                         unsigned w, unsigned h)
{
    (void)pix_fmt;
    MotionV2StateCuda *s = fex->priv;
    CudaFunctions *cu_f = fex->cu_state->f;

    /* The 5-tap CUDA motion_v2 kernel uses reflect-101 mirror padding;
     * mirror() returns 2*sup - idx - 2, which is negative when sup < 3.
     * Refuse smaller frames up front to prevent out-of-bounds device reads.
     * Minimum: filter_width/2 + 1 = 3. */
    if (h < 3u || w < 3u) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "motion_v2_cuda: frame %ux%u is below the 5-tap filter minimum 3x3; "
                 "refusing to avoid out-of-bounds mirror reads on device\n",
                 w, h);
        return -EINVAL;
    }

    s->frame_w = w;
    s->frame_h = h;
    s->bpc = bpc;
    s->plane_bytes = (size_t)w * h * (bpc <= 8 ? 1u : 2u);

    int err = vmaf_cuda_kernel_lifecycle_init(&s->lc, fex->cu_state);
    if (err)
        return err;

    int _cuda_err = 0;
    int ctx_pushed = 0;
    CHECK_CUDA_GOTO(cu_f, cuCtxPushCurrent(fex->cu_state->ctx), fail);
    ctx_pushed = 1;

    CHECK_CUDA_GOTO(cu_f, cuModuleLoadData(&s->module, motion_v2_score_ptx), fail);
    CHECK_CUDA_GOTO(cu_f, cuModuleGetFunction(&s->funcbpc8, s->module, "motion_v2_kernel_8bpc"),
                    fail);
    CHECK_CUDA_GOTO(cu_f, cuModuleGetFunction(&s->funcbpc16, s->module, "motion_v2_kernel_16bpc"),
                    fail);

    CHECK_CUDA_GOTO(cu_f, cuCtxPopCurrent(NULL), fail_after_pop);

    int ret = 0;
    ret |= vmaf_cuda_buffer_alloc(fex->cu_state, &s->pix[0], s->plane_bytes);
    ret |= vmaf_cuda_buffer_alloc(fex->cu_state, &s->pix[1], s->plane_bytes);
    if (ret)
        goto free_buffers;

    ret = vmaf_cuda_kernel_readback_alloc(&s->rb, fex->cu_state, sizeof(uint64_t));
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
    if (s->pix[0]) {
        (void)vmaf_cuda_buffer_free(fex->cu_state, s->pix[0]);
        free(s->pix[0]);
        s->pix[0] = NULL;
    }
    if (s->pix[1]) {
        (void)vmaf_cuda_buffer_free(fex->cu_state, s->pix[1]);
        free(s->pix[1]);
        s->pix[1] = NULL;
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
    MotionV2StateCuda *s = fex->priv;
    CudaFunctions *cu_f = fex->cu_state->f;

    s->index = index;
    s->frame_w = ref_pic->w[0];
    s->frame_h = ref_pic->h[0];

    const unsigned cur_idx = index % 2u;
    const unsigned prev_idx = (index + 1u) % 2u;

    CUstream pic_stream = vmaf_cuda_picture_get_stream(ref_pic);

    /* Cache cur ref Y plane into pix[cur_idx] so the next frame can
     * read it as "prev". The picture's plane is itself on device — a
     * D2D copy of the contiguous Y plane (stride = w*bpp; libvmaf
     * picture allocator already packs tightly). */
    CHECK_CUDA_RETURN(cu_f,
                      cuStreamWaitEvent(pic_stream, vmaf_cuda_picture_get_ready_event(ref_pic),
                                        CU_EVENT_WAIT_DEFAULT));
    /* Source stride may exceed plane width — copy row by row with
     * cuMemcpy2D so we land a tightly-packed copy in pix[cur_idx].
     * Width of the copy = w * bpp; height = h. */
    CUDA_MEMCPY2D copy = {0};
    copy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
    copy.srcDevice = (CUdeviceptr)ref_pic->data[0];
    copy.srcPitch = ref_pic->stride[0];
    copy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
    copy.dstDevice = (CUdeviceptr)s->pix[cur_idx]->data;
    copy.dstPitch = s->frame_w * (s->bpc <= 8u ? 1u : 2u);
    copy.WidthInBytes = copy.dstPitch;
    copy.Height = s->frame_h;
    CHECK_CUDA_RETURN(cu_f, cuMemcpy2DAsync(&copy, pic_stream));

    /* Frame 0: nothing more to do — emit 0 in collect. */
    if (index == 0) {
        CHECK_CUDA_RETURN(cu_f, cuEventRecord(s->lc.submit, pic_stream));
        CHECK_CUDA_RETURN(cu_f, cuStreamWaitEvent(s->lc.str, s->lc.submit, CU_EVENT_WAIT_DEFAULT));
        return 0;
    }

    /* Reset device SAD accumulator (single int64). The template's
     * vmaf_cuda_kernel_submit_pre_launch isn't a fit here because
     * the memset has to run on pic_stream — the kernel reads the
     * accumulator before the host-side D2H copy on lc.str — and
     * the wait was already issued earlier in this submit. */
    CHECK_CUDA_RETURN(cu_f, cuMemsetD8Async(s->rb.device->data, 0, s->rb.bytes, pic_stream));

    const unsigned block_dim_x = 16;
    const unsigned block_dim_y = 16;
    const unsigned grid_dim_x = DIV_ROUND_UP(s->frame_w, block_dim_x);
    const unsigned grid_dim_y = DIV_ROUND_UP(s->frame_h, block_dim_y);
    const ptrdiff_t plane_pitch = (ptrdiff_t)(s->frame_w * (s->bpc <= 8u ? 1u : 2u));

    if (s->bpc == 8u) {
        void *args[] = {
            &s->pix[prev_idx]->data, &s->pix[cur_idx]->data, (void *)&plane_pitch,
            (void *)&plane_pitch,    (void *)s->rb.device,   (void *)&s->frame_w,
            (void *)&s->frame_h,
        };
        CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->funcbpc8, grid_dim_x, grid_dim_y, 1, block_dim_x,
                                               block_dim_y, 1, 0, pic_stream, args, NULL));
    } else {
        void *args[] = {
            &s->pix[prev_idx]->data, &s->pix[cur_idx]->data, (void *)&plane_pitch,
            (void *)&plane_pitch,    (void *)s->rb.device,   (void *)&s->frame_w,
            (void *)&s->frame_h,     (void *)&s->bpc,
        };
        CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->funcbpc16, grid_dim_x, grid_dim_y, 1, block_dim_x,
                                               block_dim_y, 1, 0, pic_stream, args, NULL));
    }

    CHECK_CUDA_RETURN(cu_f, cuEventRecord(s->lc.submit, pic_stream));
    CHECK_CUDA_RETURN(cu_f, cuStreamWaitEvent(s->lc.str, s->lc.submit, CU_EVENT_WAIT_DEFAULT));

    CHECK_CUDA_RETURN(cu_f, cuMemcpyDtoHAsync(s->rb.host_pinned, (CUdeviceptr)s->rb.device->data,
                                              s->rb.bytes, s->lc.str));
    return vmaf_cuda_kernel_submit_post_record(&s->lc, fex->cu_state);
}

static int collect_fex_cuda(VmafFeatureExtractor *fex, unsigned index,
                            VmafFeatureCollector *feature_collector)
{
    MotionV2StateCuda *s = fex->priv;

    int sync_err = vmaf_cuda_kernel_collect_wait(&s->lc, fex->cu_state);
    if (sync_err)
        return sync_err;

    if (index == 0) {
        return vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                       "VMAF_integer_feature_motion_v2_sad_score",
                                                       0.0, index);
    }

    const uint64_t *sad_host = s->rb.host_pinned;
    const double sad_score = (double)*sad_host / 256.0 / ((double)s->frame_w * s->frame_h);
    return vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "VMAF_integer_feature_motion_v2_sad_score",
                                                   sad_score, index);
}

static int flush_fex_cuda(VmafFeatureExtractor *fex, VmafFeatureCollector *feature_collector)
{
    MotionV2StateCuda *s = fex->priv;

    unsigned n_frames = 0;
    double dummy;
    while (!vmaf_feature_collector_get_score(
        feature_collector, "VMAF_integer_feature_motion_v2_sad_score", &dummy, n_frames))
        n_frames++;

    if (n_frames < 2)
        return 1;

    for (unsigned i = 0; i < n_frames; i++) {
        double score_cur;
        double score_next;
        vmaf_feature_collector_get_score(feature_collector,
                                         "VMAF_integer_feature_motion_v2_sad_score", &score_cur, i);
        /* Apply fps weight — mirrors CPU integer_motion_v2.c flush logic.
         * Bit-exact when motion_fps_weight = 1.0 (default). */
        score_cur *= s->motion_fps_weight;

        double motion2;
        if (i + 1 < n_frames) {
            vmaf_feature_collector_get_score(
                feature_collector, "VMAF_integer_feature_motion_v2_sad_score", &score_next, i + 1);
            score_next *= s->motion_fps_weight;
            motion2 = score_cur < score_next ? score_cur : score_next;
        } else {
            motion2 = score_cur;
        }

        int append_err = vmaf_feature_collector_append(
            feature_collector, "VMAF_integer_feature_motion2_v2_score", motion2, i);
        if (append_err)
            return append_err;
    }

    return 1;
}

static int close_fex_cuda(VmafFeatureExtractor *fex)
{
    MotionV2StateCuda *s = fex->priv;

    int rc = vmaf_cuda_kernel_lifecycle_close(&s->lc, fex->cu_state);

    if (s->pix[0]) {
        const int e = vmaf_cuda_buffer_free(fex->cu_state, s->pix[0]);
        free(s->pix[0]);
        if (rc == 0)
            rc = e;
    }
    if (s->pix[1]) {
        const int e = vmaf_cuda_buffer_free(fex->cu_state, s->pix[1]);
        free(s->pix[1]);
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

static const char *provided_features[] = {"VMAF_integer_feature_motion_v2_sad_score",
                                          "VMAF_integer_feature_motion2_v2_score", NULL};

VmafFeatureExtractor vmaf_fex_integer_motion_v2_cuda = {
    .name = "motion_v2_cuda",
    .init = init_fex_cuda,
    .submit = submit_fex_cuda,
    .collect = collect_fex_cuda,
    .flush = flush_fex_cuda,
    .close = close_fex_cuda,
    .options = options,
    .priv_size = sizeof(MotionV2StateCuda),
    .provided_features = provided_features,
    .flags = VMAF_FEATURE_EXTRACTOR_TEMPORAL | VMAF_FEATURE_EXTRACTOR_CUDA,
};
