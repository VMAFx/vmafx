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

int vmaf_cuda_picture_download_async(VmafPicture *cuda_pic, VmafPicture *pic, uint8_t bitmask)
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

int vmaf_cuda_picture_upload_async(VmafPicture *cuda_pic, VmafPicture *pic, uint8_t bitmask)
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

static int default_release_pinned_picture(VmafPicture *pic, void *cookie)
{
    (void)cookie;
    if (!pic)
        return -EINVAL;

    VmafPicturePrivate *priv = pic->priv;
    CudaFunctions *cu_f = priv->cuda.state->f;
    int _cuda_err = 0;
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

/* pinned_alloc_check_args - the entry guards of vmaf_cuda_picture_alloc_pinned.
 *
 * HISS-04: the four guards, moved whole and in their original order, each
 * still returning -EINVAL.
 */
static int pinned_alloc_check_args(const VmafPicture *pic, enum VmafPixelFormat pix_fmt,
                                   unsigned bpc, unsigned w, unsigned h)
{
    if (!pic)
        return -EINVAL;
    if (!pix_fmt)
        return -EINVAL;
    if (bpc < VMAF_CUDA_PIC_BPC_MIN || bpc > VMAF_CUDA_PIC_BPC_MAX)
        return -EINVAL;
    /* Guard against 32-bit overflow in the stride/pic_size compute below —
     * mirrors the vmaf_picture_alloc host twin. CERT INT30-C. */
    if (w == 0 || w > VMAF_CUDA_PIC_DIM_MAX || h == 0 || h > VMAF_CUDA_PIC_DIM_MAX)
        return -EINVAL;
    return 0;
}

/* picture_set_plane_dims - zero the picture and derive its per-plane extents.
 *
 * HISS-04: the geometry prologue both CUDA picture allocators carried inline,
 * factored into one. Every statement is copied character for character and in
 * the same order (the ceiling-division shifts included), so both callers get
 * the plane dimensions they computed before.
 */
static void picture_set_plane_dims(VmafPicture *pic, enum VmafPixelFormat pix_fmt, unsigned bpc,
                                   unsigned w, unsigned h)
{
    memset(pic, 0, sizeof(*pic));
    pic->pix_fmt = pix_fmt;
    pic->bpc = bpc;
    const int ss_hor = pic->pix_fmt != VMAF_PIX_FMT_YUV444P;
    const int ss_ver = pic->pix_fmt == VMAF_PIX_FMT_YUV420P;
    pic->w[0] = w;
    /* Ceiling division — mirrors picture.c fix (Research-0094). */
    pic->w[1] = pic->w[2] = (w + ((unsigned)ss_hor)) >> ss_hor;
    pic->h[0] = h;
    pic->h[1] = pic->h[2] = (h + ((unsigned)ss_ver)) >> ss_ver;
    if (pic->pix_fmt == VMAF_PIX_FMT_YUV400P)
        pic->w[1] = pic->w[2] = pic->h[1] = pic->h[2] = 0;
}

/* pinned_plane_sizes - the pinned strides and the total allocation size.
 *
 * HISS-04: the stride/size block of vmaf_cuda_picture_alloc_pinned, moved
 * whole. The alignment masks, the `<< hbd` depth shift and the y/uv products
 * are unchanged integer arithmetic in the original order.
 */
static size_t pinned_plane_sizes(VmafPicture *pic, size_t *y_sz, size_t *uv_sz)
{
    const unsigned aligned_y = (pic->w[0] + DATA_ALIGN_PINNED - 1u) & ~(DATA_ALIGN_PINNED - 1u);
    const unsigned aligned_c = (pic->w[1] + DATA_ALIGN_PINNED - 1u) & ~(DATA_ALIGN_PINNED - 1u);
    const int hbd = pic->bpc > 8;
    pic->stride[0] = aligned_y << hbd;
    pic->stride[1] = pic->stride[2] = aligned_c << hbd;
    *y_sz = pic->stride[0] * pic->h[0];
    *uv_sz = pic->stride[1] * pic->h[1];
    return *y_sz + 2 * *uv_sz;
}

/* How far vmaf_cuda_picture_alloc_pinned got before it had to unwind. */
enum {
    PINNED_UNWIND_NONE = 0, /* cuMemHostAlloc returned no buffer */
    PINNED_UNWIND_DATA = 1, /* pinned buffer live */
    PINNED_UNWIND_PRIV = 2, /* pinned buffer + pic->priv live */
};

/* pinned_alloc_unwind - the single teardown path for
 * vmaf_cuda_picture_alloc_pinned.
 *
 * HISS-01: replaces the former free_priv -> free_data -> fail_no_data
 * fall-through cascade. `stage` selects how far the cascade had got, so
 * every exit path releases exactly the resources its label released, in
 * the same order, and returns the same -ENOMEM.
 */
static int pinned_alloc_unwind(VmafPicture *pic, CudaFunctions *cu_f, uint8_t *data, int stage)
{
    if (stage >= PINNED_UNWIND_PRIV)
        free(pic->priv);
    if (stage >= PINNED_UNWIND_DATA)
        (void)cu_f->cuMemFreeHost(data);
    return -ENOMEM;
}

int vmaf_cuda_picture_alloc_pinned(VmafPicture *pic, enum VmafPixelFormat pix_fmt, unsigned bpc,
                                   unsigned w, unsigned h, VmafCudaState *cuda_state)
{
    const int arg_err = pinned_alloc_check_args(pic, pix_fmt, bpc, w, h);
    if (arg_err)
        return arg_err;

    int err = 0;

    picture_set_plane_dims(pic, pix_fmt, bpc, w, h);

    size_t y_sz = 0;
    size_t uv_sz = 0;
    const size_t pic_size = pinned_plane_sizes(pic, &y_sz, &uv_sz);
    CudaFunctions *cu_f = cuda_state->f;
    int _cuda_err = 0;
    int ctx_pushed = 0;
    uint8_t *data = NULL;
    CHECK_CUDA_GOTO(cu_f, cuCtxPushCurrent(cuda_state->ctx), fail);
    ctx_pushed = 1;
    CHECK_CUDA_GOTO(cu_f, cuMemHostAlloc((void **)&data, pic_size, 0x01), fail);
    CHECK_CUDA_GOTO(cu_f, cuCtxPopCurrent(NULL), fail_after_pop);
    if (!data)
        return pinned_alloc_unwind(pic, cu_f, data, PINNED_UNWIND_NONE);

    memset(data, 0, pic_size);
    pic->data[0] = data;
    pic->data[1] = data + y_sz;
    pic->data[2] = data + y_sz + uv_sz;
    if (pic->pix_fmt == VMAF_PIX_FMT_YUV400P)
        pic->data[1] = pic->data[2] = NULL;

    /* vmaf_picture_priv_init allocates pic->priv; check before touching it.
     * Mirrors the fix in picture.c (PR #700, CWE-476): the |= idiom evaluates
     * the right-hand side unconditionally, so a priv-init failure would leave
     * pic->priv == NULL and the subsequent field writes would null-deref. */
    err = vmaf_picture_priv_init(pic);
    if (err)
        return pinned_alloc_unwind(pic, cu_f, data, PINNED_UNWIND_DATA);

    VmafPicturePrivate *priv = pic->priv;
    priv->cuda.state = cuda_state;
    priv->cuda.ctx = cuda_state->ctx;
    err = vmaf_picture_set_release_callback(pic, NULL, default_release_pinned_picture);
    if (err)
        return pinned_alloc_unwind(pic, cu_f, data, PINNED_UNWIND_PRIV);
    priv->buf_type = VMAF_PICTURE_BUFFER_TYPE_CUDA_HOST_PINNED;

    err = vmaf_ref_init(&pic->ref);
    if (err)
        return pinned_alloc_unwind(pic, cu_f, data, PINNED_UNWIND_PRIV);

    return 0;

fail:
    if (ctx_pushed)
        (void)cu_f->cuCtxPopCurrent(NULL);
fail_after_pop:
    return _cuda_err;
}

/* How far vmaf_cuda_picture_alloc got before it had to unwind. */
enum {
    DEV_PIC_UNWIND_PRIV = 0,     /* priv only */
    DEV_PIC_UNWIND_STREAM = 1,   /* + upload stream */
    DEV_PIC_UNWIND_READY = 2,    /* + ready event */
    DEV_PIC_UNWIND_FINISHED = 3, /* + finished event */
    DEV_PIC_UNWIND_DATA = 4,     /* + device planes, context still current */
};

/* device_pic_init_stream - create the upload stream and the two events.
 *
 * HISS-04: the stream/event bring-up of vmaf_cuda_picture_alloc, moved whole.
 * `*stage` advances after each step that succeeds, so a failure sends the
 * caller into device_pic_unwind at exactly the graduated fail_after_* label
 * the inline code would have jumped to (ADR-1090).
 */
static int device_pic_init_stream(VmafPicturePrivate *priv, CudaFunctions *cu_f, int *stage)
{
    /* Use CU_STREAM_NON_BLOCKING so this picture-upload stream does not
     * implicitly serialise with the legacy NULL (default) stream.
     * CU_STREAM_DEFAULT causes every operation on this stream to act as if
     * the default stream were involved, meaning all other non-default streams
     * must complete before any work on this stream starts (and vice versa).
     * At sub-4K resolutions that per-frame round-trip serialisation dominates
     * compute time and makes CUDA motion ~0.55× slower than CPU scalar.
     * CU_STREAM_NON_BLOCKING removes the implicit barrier.
     * ADR-0378. */
    CHECK_CUDA_RETURN(cu_f, cuStreamCreateWithPriority(&priv->cuda.str, CU_STREAM_NON_BLOCKING, 0));
    *stage = DEV_PIC_UNWIND_STREAM;
    CHECK_CUDA_RETURN(cu_f, cuEventCreate(&priv->cuda.ready, CU_EVENT_DEFAULT));
    *stage = DEV_PIC_UNWIND_READY;
    CHECK_CUDA_RETURN(cu_f, cuEventCreate(&priv->cuda.finished, CU_EVENT_DEFAULT));
    *stage = DEV_PIC_UNWIND_FINISHED;
    CHECK_CUDA_RETURN(cu_f, cuEventRecord(priv->cuda.finished, priv->cuda.str));
    return 0;
}

/* device_alloc_check_args - the entry guards of vmaf_cuda_picture_alloc.
 *
 * HISS-04: the four guards, moved whole and in their original order. The two
 * cookie guards still return -1 rather than -EINVAL, as they always have.
 */
static int device_alloc_check_args(const VmafPicture *pic, const void *cookie)
{
    if (!pic)
        return -EINVAL;
    if (!cookie)
        return -EINVAL;

    const VmafCudaCookie *cuda_cookie = cookie;
    if (!cuda_cookie->pix_fmt)
        return -1;
    if (cuda_cookie->bpc < VMAF_CUDA_PIC_BPC_MIN || cuda_cookie->bpc > VMAF_CUDA_PIC_BPC_MAX)
        return -1;
    return 0;
}

/* device_pic_alloc_planes - pitch-allocate the up-to-three device planes.
 *
 * HISS-04: the cuMemAllocPitch loop of vmaf_cuda_picture_alloc, moved whole -
 * same YUV400 early break, same width/height/alignment arguments. The caller
 * routes a failure into device_pic_unwind at DEV_PIC_UNWIND_FINISHED, which is
 * the label the loop used to jump to.
 */
static int device_pic_alloc_planes(VmafPicture *pic, CudaFunctions *cu_f)
{
    const int hbd = pic->bpc > 8;

    for (int i = 0; i < 3; i++) {
        if (pic->pix_fmt == VMAF_PIX_FMT_YUV400P && i > 0) {
            pic->data[1] = pic->data[2] = NULL;
            break;
        }
        CHECK_CUDA_RETURN(
            cu_f, cuMemAllocPitch((CUdeviceptr *)&pic->data[i], (size_t *)&pic->stride[i],
                                  (size_t)pic->w[i] * ((pic->bpc + 7) / 8), pic->h[i], 8 << hbd));
    }
    return 0;
}

/* device_pic_unwind - the single teardown path for vmaf_cuda_picture_alloc.
 *
 * HISS-04: the former fail_after_data -> fail_after_finished -> fail_after_ready
 * -> fail_after_stream -> fail ladder (ADR-1090), moved whole. `stage` says how
 * far the allocator got, so each entry point still releases exactly what its
 * label released, in the same order: planes first while the context is still
 * current, then the pop that clears ctx_pushed, then the two events, the
 * stream, and finally priv. `fail` and `fail_after_data` remain because
 * CHECK_CUDA_GOTO jumps to them; they now just call this.
 */
static int device_pic_unwind(VmafPicture *pic, VmafPicturePrivate *priv, CudaFunctions *cu_f,
                             int stage, int ctx_pushed, int cuda_err)
{
    if (stage >= DEV_PIC_UNWIND_DATA) {
        /* Free any device planes already allocated. Context is still current
         * (cuCtxPopCurrent failed, or we came from the cuMemAllocPitch loop
         * with ctx_pushed==1). */
        for (int i = 0; i < 3; i++) {
            if (pic->data[i])
                (void)cu_f->cuMemFree((CUdeviceptr)pic->data[i]);
        }
        /* Pop once here and clear ctx_pushed so the tail below does not pop
         * a second time. */
        if (ctx_pushed) {
            (void)cu_f->cuCtxPopCurrent(NULL);
            ctx_pushed = 0;
        }
    }
    if (stage >= DEV_PIC_UNWIND_FINISHED)
        (void)cu_f->cuEventDestroy(priv->cuda.finished);
    if (stage >= DEV_PIC_UNWIND_READY)
        (void)cu_f->cuEventDestroy(priv->cuda.ready);
    if (stage >= DEV_PIC_UNWIND_STREAM)
        (void)cu_f->cuStreamDestroy(priv->cuda.str);
    if (ctx_pushed)
        (void)cu_f->cuCtxPopCurrent(NULL);
    free(priv);
    pic->priv = NULL;
    return cuda_err;
}

/* device_pic_free_after_pop - unwind a fully built device picture once the
 * context has already been popped.
 *
 * HISS-04: the vmaf_ref_init failure block of vmaf_cuda_picture_alloc, moved
 * whole. The context is re-pushed only to free the device planes (vmaf_ref_init
 * is a plain malloc-like call that does not touch the CUDA context), and the
 * release order - planes, finished event, ready event, stream, priv - is
 * unchanged.
 */
static void device_pic_free_after_pop(VmafPicture *pic, VmafPicturePrivate *priv,
                                      CudaFunctions *cu_f)
{
    int push_err = cu_f->cuCtxPushCurrent(priv->cuda.ctx);
    if (!push_err) {
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
    const int arg_err = device_alloc_check_args(pic, cookie);
    if (arg_err)
        return arg_err;

    VmafCudaCookie *cuda_cookie = cookie;

    picture_set_plane_dims(pic, cuda_cookie->pix_fmt, cuda_cookie->bpc, cuda_cookie->w,
                           cuda_cookie->h);

    VmafPicturePrivate *priv = pic->priv = malloc(sizeof(VmafPicturePrivate));
    if (!priv)
        return -ENOMEM;

    int _cuda_err = 0;
    int ctx_pushed = 0;
    /* Bound before the first jump: the unwind helper below takes cu_f, and
     * `fail` is reachable from the very first CHECK_CUDA_GOTO. This is the
     * same table as priv->cuda.state->f, which is assigned just below. */
    CudaFunctions *cu_f = cuda_cookie->state->f;
    CHECK_CUDA_GOTO(cu_f, cuCtxPushCurrent(cuda_cookie->state->ctx), fail);
    ctx_pushed = 1;
    priv->cuda.state = cuda_cookie->state;
    priv->cuda.ctx = cuda_cookie->state->ctx;
    int stage = DEV_PIC_UNWIND_PRIV;
    _cuda_err = device_pic_init_stream(priv, cu_f, &stage);
    if (_cuda_err)
        return device_pic_unwind(pic, priv, cu_f, stage, ctx_pushed, _cuda_err);
    priv->buf_type = VMAF_PICTURE_BUFFER_TYPE_CUDA_DEVICE;

    _cuda_err = device_pic_alloc_planes(pic, cu_f);
    if (_cuda_err)
        return device_pic_unwind(pic, priv, cu_f, DEV_PIC_UNWIND_FINISHED, ctx_pushed, _cuda_err);

    /* ADR-1090 — cuCtxPopCurrent failure leaves the context on the stack;
     * fall through to fail_after_data which frees device memory while the
     * context is still current, then pops. */
    CHECK_CUDA_GOTO(cu_f, cuCtxPopCurrent(NULL), fail_after_data);
    {
        int err = vmaf_ref_init(&pic->ref);
        if (err) {
            device_pic_free_after_pop(pic, priv, cu_f);
            return err;
        }
    }
    return 0;

fail_after_data:
    return device_pic_unwind(pic, priv, cu_f, DEV_PIC_UNWIND_DATA, ctx_pushed, _cuda_err);
fail:
    return device_pic_unwind(pic, priv, cu_f, DEV_PIC_UNWIND_PRIV, ctx_pushed, _cuda_err);
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

    int _cuda_err = 0;
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

CUstream vmaf_cuda_picture_get_stream(VmafPicture *pic)
{
    VmafPicturePrivate *priv = pic->priv;
    return priv->cuda.str;
}

CUevent vmaf_cuda_picture_get_ready_event(VmafPicture *pic)
{
    VmafPicturePrivate *priv = pic->priv;
    return priv->cuda.ready;
}

CUevent vmaf_cuda_picture_get_finished_event(VmafPicture *pic)
{
    VmafPicturePrivate *priv = pic->priv;
    return priv->cuda.finished;
}

enum VmafPixelFormat vmaf_cuda_picture_get_pix_fmt(const VmafPicture *pic)
{
    return pic->pix_fmt;
}

/* NOLINTEND(modernize-use-nullptr) */
