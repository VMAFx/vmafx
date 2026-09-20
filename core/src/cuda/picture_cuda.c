/**
 *
 *  Copyright 2016-2023 Netflix, Inc.
 *  Copyright 2021 NVIDIA Corporation.
 *
 *     Licensed under the BSD+Patent License (the "License");
 *     you may not use this file except in compliance with the License.
 *     You may obtain a copy of the License at
 *
 *         https://opensource.org/licenses/BSDplusPatent
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 *
 */

#include "mem.h"
#include "picture_cuda.h"
#include "common.h"
#include "log.h"
#include "ref.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

int vmaf_cuda_picture_download_async(const VmafPicture *cuda_pic, VmafPicture *pic,
                                     uint8_t bitmask)
{
    if (!cuda_pic)
        return -EINVAL;
    if (!pic)
        return -EINVAL;

    CUDA_MEMCPY2D m = {0};
    m.srcMemoryType = CU_MEMORYTYPE_DEVICE;
    m.dstMemoryType = CU_MEMORYTYPE_HOST;

    VmafPicturePrivate *cuda_priv = cuda_pic->priv;
    CudaFunctions *cu_f = cuda_priv->cuda.state->f;
    for (int i = 0; i < 3; i++) {
        m.srcDevice = (CUdeviceptr)cuda_pic->data[i];
        m.srcPitch = cuda_pic->stride[i];
        m.dstHost = pic->data[i];
        m.dstPitch = pic->stride[i];
        m.WidthInBytes = (size_t)cuda_pic->w[i] * ((pic->bpc + 7) / 8);
        m.Height = cuda_pic->h[i];
        if ((bitmask >> i) & 1)
            CHECK_CUDA_RETURN(cu_f, cuMemcpy2DAsync(&m, cuda_priv->cuda.str));
    }

    return 0;
}

int vmaf_cuda_picture_upload_async(VmafPicture *cuda_pic, const VmafPicture *pic, uint8_t bitmask)
{
    if (!cuda_pic)
        return -EINVAL;
    if (!pic)
        return -EINVAL;

    CUDA_MEMCPY2D m = {0};
    m.srcMemoryType = CU_MEMORYTYPE_HOST;
    m.dstMemoryType = CU_MEMORYTYPE_DEVICE;

    VmafPicturePrivate *cuda_priv = cuda_pic->priv;
    CudaFunctions *cu_f = cuda_priv->cuda.state->f;
    for (int i = 0; i < 3; i++) {
        m.srcHost = pic->data[i];
        m.srcPitch = pic->stride[i];
        m.dstDevice = (CUdeviceptr)cuda_pic->data[i];
        m.dstPitch = cuda_pic->stride[i];
        m.WidthInBytes = (size_t)cuda_pic->w[i] * ((pic->bpc + 7) / 8);
        m.Height = cuda_pic->h[i];
        if ((bitmask >> i) & 1)
            CHECK_CUDA_RETURN(cu_f, cuMemcpy2DAsync(&m, cuda_priv->cuda.str));
    }
    CHECK_CUDA_RETURN(cu_f, cuEventRecord(cuda_priv->cuda.ready, cuda_priv->cuda.str));

    return 0;
}

#define DATA_ALIGN_PINNED 32

/* Mirrors picture.c VMAF_PIC_BPC_{MIN,MAX} — kept local so the CUDA
 * TU does not pull libvmaf/src/picture.c into its include set. */
#define VMAF_CUDA_PIC_BPC_MIN 8u
#define VMAF_CUDA_PIC_BPC_MAX 16u

/* Per-side dimension cap — mirrors picture.c VMAF_PIC_DIM_MAX. The
 * stride compute below evaluates `(pic->w[i] + DATA_ALIGN_PINNED - 1u)`
 * in 32-bit unsigned arithmetic; capping each side at 32768 keeps that
 * addition (and the subsequent `stride * h` product feeding pic_size)
 * well clear of the UINT32_MAX wrap that would otherwise be reached
 * near `w >= 0xFFFFFFE1u`, where aligned_y wraps to 0, stride to 0, and
 * cuMemHostAlloc under-allocates → the first frame copy writes OOB.
 * 32K is also well above 8K UHD (7680). CERT INT30-C. */
#define VMAF_CUDA_PIC_DIM_MAX 32768u

static void init_picture_layout(VmafPicture *pic, enum VmafPixelFormat pix_fmt, unsigned bpc,
                                unsigned w, unsigned h)
{
    memset(pic, 0, sizeof(*pic));
    pic->pix_fmt = pix_fmt;
    pic->bpc = bpc;
    const int ss_hor = pix_fmt != VMAF_PIX_FMT_YUV444P;
    const int ss_ver = pix_fmt == VMAF_PIX_FMT_YUV420P;
    pic->w[0] = w;
    pic->w[1] = pic->w[2] = (w + (unsigned)ss_hor) >> ss_hor;
    pic->h[0] = h;
    pic->h[1] = pic->h[2] = (h + (unsigned)ss_ver) >> ss_ver;
    if (pix_fmt == VMAF_PIX_FMT_YUV400P)
        pic->w[1] = pic->w[2] = pic->h[1] = pic->h[2] = 0;
}

static int default_release_pinned_picture(VmafPicture *pic, const void *cookie)
{
    (void)cookie;
    if (!pic)
        return -EINVAL;

    VmafPicturePrivate *priv = pic->priv;
    CudaFunctions *cu_f = priv->cuda.state->f;
    int _cuda_err;
    int ctx_pushed = 0;
    CHECK_CUDA_GOTO(cu_f, cuCtxPushCurrent(priv->cuda.ctx), fail);
    ctx_pushed = 1;
    CHECK_CUDA_GOTO(cu_f, cuMemFreeHost(pic->data[0]), fail);
    CHECK_CUDA_GOTO(cu_f, cuCtxPopCurrent(NULL), fail_after_pop);
    return 0;

fail:
    if (ctx_pushed)
        (void)cu_f->cuCtxPopCurrent(NULL);
fail_after_pop:
    return _cuda_err;
}

static int alloc_pinned_pixels(VmafPicture *pic, VmafCudaState *cuda_state, uint8_t **data,
                               size_t *y_size, size_t *uv_size)
{
    const unsigned aligned_y = (pic->w[0] + DATA_ALIGN_PINNED - 1u) & ~(DATA_ALIGN_PINNED - 1u);
    const unsigned aligned_c = (pic->w[1] + DATA_ALIGN_PINNED - 1u) & ~(DATA_ALIGN_PINNED - 1u);
    const int hbd = pic->bpc > 8;
    pic->stride[0] = aligned_y << hbd;
    pic->stride[1] = pic->stride[2] = aligned_c << hbd;
    *y_size = pic->stride[0] * pic->h[0];
    *uv_size = pic->stride[1] * pic->h[1];
    const size_t pic_size = *y_size + 2 * *uv_size;
    CudaFunctions *cu_f = cuda_state->f;
    int _cuda_err;
    int ctx_pushed = 0;
    CHECK_CUDA_GOTO(cu_f, cuCtxPushCurrent(cuda_state->ctx), fail);
    ctx_pushed = 1;
    CHECK_CUDA_GOTO(cu_f, cuMemHostAlloc((void **)data, pic_size, 0x01), fail);
    CHECK_CUDA_GOTO(cu_f, cuCtxPopCurrent(NULL), fail_after_pop);
    if (!*data)
        return -ENOMEM;
    return 0;

fail:
    if (ctx_pushed)
        (void)cu_f->cuCtxPopCurrent(NULL);
fail_after_pop:
    return _cuda_err;
}

int vmaf_cuda_picture_alloc_pinned(VmafPicture *pic, enum VmafPixelFormat pix_fmt, unsigned bpc,
                                   unsigned w, unsigned h, VmafCudaState *cuda_state)
{
    if (!pic || !pix_fmt || bpc < VMAF_CUDA_PIC_BPC_MIN || bpc > VMAF_CUDA_PIC_BPC_MAX)
        return -EINVAL;
    if (w == 0 || w > VMAF_CUDA_PIC_DIM_MAX || h == 0 || h > VMAF_CUDA_PIC_DIM_MAX)
        return -EINVAL;
    if (!cuda_state || !cuda_state->f)
        return -EINVAL;

    init_picture_layout(pic, pix_fmt, bpc, w, h);
    size_t y_size = 0;
    size_t uv_size = 0;
    uint8_t *data = NULL;
    int err = alloc_pinned_pixels(pic, cuda_state, &data, &y_size, &uv_size);
    if (err)
        return err;

    memset(data, 0, y_size + 2 * uv_size);
    pic->data[0] = data;
    pic->data[1] = data + y_size;
    pic->data[2] = data + y_size + uv_size;
    if (pic->pix_fmt == VMAF_PIX_FMT_YUV400P)
        pic->data[1] = pic->data[2] = NULL;

    err = vmaf_picture_priv_init(pic);
    if (err) {
        (void)cuda_state->f->cuMemFreeHost(data);
        return -ENOMEM;
    }

    VmafPicturePrivate *priv = pic->priv;
    priv->cuda.state = cuda_state;
    priv->cuda.ctx = cuda_state->ctx;
    err = vmaf_picture_set_release_callback(pic, NULL, default_release_pinned_picture);
    if (err) {
        free(pic->priv);
        (void)cuda_state->f->cuMemFreeHost(data);
        return -ENOMEM;
    }
    priv->buf_type = VMAF_PICTURE_BUFFER_TYPE_CUDA_HOST_PINNED;

    err = vmaf_ref_init(&pic->ref);
    if (err) {
        free(pic->priv);
        (void)cuda_state->f->cuMemFreeHost(data);
        return -ENOMEM;
    }

    return 0;
}

enum CudaPictureInitStage {
    CUDA_PICTURE_PRIV_ALLOCATED,
    CUDA_PICTURE_STREAM_CREATED,
    CUDA_PICTURE_READY_CREATED,
    CUDA_PICTURE_FINISHED_CREATED,
};

static void cleanup_cuda_picture_init(VmafPicture *pic, VmafCudaCookie *cookie,
                                      enum CudaPictureInitStage stage, bool context_pushed)
{
    VmafPicturePrivate *priv = pic->priv;
    CudaFunctions *cu_f = cookie->state->f;
    if (stage >= CUDA_PICTURE_FINISHED_CREATED) {
        for (int i = 0; i < 3; i++) {
            if (pic->data[i])
                (void)cu_f->cuMemFree((CUdeviceptr)pic->data[i]);
            pic->data[i] = NULL;
        }
        (void)cu_f->cuEventDestroy(priv->cuda.finished);
    }
    if (stage >= CUDA_PICTURE_READY_CREATED)
        (void)cu_f->cuEventDestroy(priv->cuda.ready);
    if (stage >= CUDA_PICTURE_STREAM_CREATED)
        (void)cu_f->cuStreamDestroy(priv->cuda.str);
    if (context_pushed)
        (void)cu_f->cuCtxPopCurrent(NULL);
    free(priv);
    pic->priv = NULL;
}

static int init_cuda_picture_resources(VmafPicture *pic, VmafCudaCookie *cookie)
{
    VmafPicturePrivate *priv = pic->priv;
    CudaFunctions *cu_f = cookie->state->f;
    enum CudaPictureInitStage stage = CUDA_PICTURE_PRIV_ALLOCATED;
    int _cuda_err;
    int ctx_pushed = 0;
    CHECK_CUDA_GOTO(cu_f, cuCtxPushCurrent(cookie->state->ctx), fail);
    ctx_pushed = 1;
    priv->cuda.state = cookie->state;
    priv->cuda.ctx = cookie->state->ctx;
    CHECK_CUDA_GOTO(cu_f, cuStreamCreateWithPriority(&priv->cuda.str, CU_STREAM_NON_BLOCKING, 0),
                    fail);
    stage = CUDA_PICTURE_STREAM_CREATED;
    CHECK_CUDA_GOTO(cu_f, cuEventCreate(&priv->cuda.ready, CU_EVENT_DEFAULT), fail);
    stage = CUDA_PICTURE_READY_CREATED;
    CHECK_CUDA_GOTO(cu_f, cuEventCreate(&priv->cuda.finished, CU_EVENT_DEFAULT), fail);
    stage = CUDA_PICTURE_FINISHED_CREATED;
    CHECK_CUDA_GOTO(cu_f, cuEventRecord(priv->cuda.finished, priv->cuda.str), fail);
    priv->buf_type = VMAF_PICTURE_BUFFER_TYPE_CUDA_DEVICE;

    const int hbd = pic->bpc > 8;
    for (int i = 0; i < 3; i++) {
        if (pic->pix_fmt == VMAF_PIX_FMT_YUV400P && i > 0)
            break;
        CHECK_CUDA_GOTO(cu_f,
                        cuMemAllocPitch((CUdeviceptr *)&pic->data[i], (size_t *)&pic->stride[i],
                                        (size_t)pic->w[i] * ((pic->bpc + 7) / 8), pic->h[i],
                                        8 << hbd),
                        fail);
    }
    CHECK_CUDA_GOTO(cu_f, cuCtxPopCurrent(NULL), fail);
    return 0;

fail:
    cleanup_cuda_picture_init(pic, cookie, stage, ctx_pushed != 0);
    return _cuda_err;
}

static void cleanup_cuda_picture_ref_failure(VmafPicture *pic, VmafCudaCookie *cookie)
{
    VmafPicturePrivate *priv = pic->priv;
    CudaFunctions *cu_f = cookie->state->f;
    const CUresult push_res = cu_f->cuCtxPushCurrent(priv->cuda.ctx);
    if (push_res == CUDA_SUCCESS) {
        for (int i = 0; i < 3; i++) {
            if (pic->data[i])
                (void)cu_f->cuMemFree((CUdeviceptr)pic->data[i]);
            pic->data[i] = NULL;
        }
        (void)cu_f->cuCtxPopCurrent(NULL);
    }
    (void)cu_f->cuEventDestroy(priv->cuda.finished);
    (void)cu_f->cuEventDestroy(priv->cuda.ready);
    (void)cu_f->cuStreamDestroy(priv->cuda.str);
    free(priv);
    pic->priv = NULL;
}

int vmaf_cuda_picture_alloc(VmafPicture *pic, void *cookie)
{
    if (!pic || !cookie)
        return -EINVAL;

    VmafCudaCookie *cuda_cookie = cookie;
    if (!cuda_cookie->pix_fmt || cuda_cookie->bpc < VMAF_CUDA_PIC_BPC_MIN ||
        cuda_cookie->bpc > VMAF_CUDA_PIC_BPC_MAX)
        return -1;
    init_picture_layout(pic, cuda_cookie->pix_fmt, cuda_cookie->bpc, cuda_cookie->w,
                        cuda_cookie->h);

    pic->priv = malloc(sizeof(VmafPicturePrivate));
    if (!pic->priv)
        return -ENOMEM;
    const int init_err = init_cuda_picture_resources(pic, cuda_cookie);
    if (init_err)
        return init_err;

    const int ref_err = vmaf_ref_init(&pic->ref);
    if (ref_err)
        cleanup_cuda_picture_ref_failure(pic, cuda_cookie);
    return ref_err;
}

int vmaf_cuda_picture_free(VmafPicture *pic, void *cookie)
{
    if (!pic)
        return -EINVAL;

    long err = vmaf_ref_load(pic->ref);
    if (!err)
        return -EINVAL;

    VmafPicturePrivate *priv = pic->priv;
    VmafCudaCookie *cuda_cookie = cookie;
    CudaFunctions *cu_f = cuda_cookie->state->f;

    int _cuda_err;
    int ctx_pushed = 0;
    CHECK_CUDA_GOTO(cu_f, cuCtxPushCurrent(cuda_cookie->state->ctx), fail);
    ctx_pushed = 1;
    CHECK_CUDA_GOTO(cu_f, cuStreamSynchronize(priv->cuda.str), fail);

    for (int i = 0; i < 3; i++) {
        if (pic->data[i]) {
            CHECK_CUDA_GOTO(cu_f, cuMemFree((CUdeviceptr)pic->data[i]), fail);
        }
    }

    CHECK_CUDA_GOTO(cu_f, cuEventDestroy(priv->cuda.finished), fail);
    CHECK_CUDA_GOTO(cu_f, cuEventDestroy(priv->cuda.ready), fail);
    CHECK_CUDA_GOTO(cu_f, cuStreamDestroy(priv->cuda.str), fail);
    CHECK_CUDA_GOTO(cu_f, cuCtxPopCurrent(NULL), fail_after_pop);
    vmaf_ref_close(pic->ref);
    free(priv);
    memset(pic, 0, sizeof(*pic));

    return 0;

fail:
    if (ctx_pushed)
        (void)cu_f->cuCtxPopCurrent(NULL);
fail_after_pop:
    return _cuda_err;
}

int vmaf_cuda_picture_synchronize(VmafPicture *pic, void *cookie)
{
    if (!pic)
        return -EINVAL;
    (void)cookie;

    VmafPicturePrivate *priv = pic->priv;
    CudaFunctions *cu_f = priv->cuda.state->f;
    CHECK_CUDA_RETURN(cu_f, cuEventSynchronize(priv->cuda.finished));
    // cuStreamSynchronize after cuEventSynchronize on the same stream is
    // redundant — the event was recorded on this stream, so the sync
    // already guarantees all prior work on the stream is complete.
    // cuCtxPushCurrent/cuCtxPopCurrent with no work between them is a no-op.
    return 0;
}

CUstream vmaf_cuda_picture_get_stream(const VmafPicture *pic)
{
    VmafPicturePrivate *priv = pic->priv;
    return priv->cuda.str;
}

CUevent vmaf_cuda_picture_get_ready_event(const VmafPicture *pic)
{
    VmafPicturePrivate *priv = pic->priv;
    return priv->cuda.ready;
}

CUevent vmaf_cuda_picture_get_finished_event(const VmafPicture *pic)
{
    VmafPicturePrivate *priv = pic->priv;
    return priv->cuda.finished;
}

/* NOLINTEND(modernize-use-nullptr) */
