/**
 *
 *  Copyright 2026 Lusoris
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

/**
 * SYCL picture management — upload/download Y-plane data between host
 * VmafPicture buffers and SYCL USM device memory.  Also provides the
 * alloc/free callbacks used by VmafPicture pre-allocation pools.
 *
 * NOTE: The primary frame-upload path for SYCL extractors uses the
 * shared-buffer mechanism in common.cpp (vmaf_sycl_shared_frame_upload).
 * The functions here are for:
 *   1. Standalone picture upload/download (testing, future VPL interop).
 *   2. VmafPicture pre-allocation pool callbacks.
 */

#include <cassert>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>

#include <sycl/sycl.hpp>

#include "picture.h"
#include "common.h"
#include "ref.h"
#include "gpu_picture_pool.h"
#include "picture_sycl.h"

/* ------------------------------------------------------------------ */
/* Upload / download                                                   */
/* ------------------------------------------------------------------ */

extern "C" int vmaf_sycl_picture_upload(VmafSyclState *state, void *dst, VmafPicture *pic,
                                        unsigned plane)
{
    if (!state || !dst || !pic)
        return -EINVAL;
    if (plane >= 3)
        return -EINVAL;

    size_t const bpp = (pic->bpc + 7) / 8;
    size_t const row_bytes = pic->w[plane] * bpp;
    size_t const total = row_bytes * pic->h[plane];

    if (pic->stride[plane] == (ptrdiff_t)row_bytes) {
        /* Contiguous — single memcpy */
        return vmaf_sycl_memcpy_h2d(state, dst, pic->data[plane], total);
    }

    /* Non-contiguous — row-by-row pack into device buffer */
    const uint8_t *src = (const uint8_t *)pic->data[plane];
    uint8_t *d = (uint8_t *)dst;
    for (unsigned y = 0; y < pic->h[plane]; y++) {
        int const err =
            vmaf_sycl_memcpy_h2d(state, d + y * row_bytes, src + y * pic->stride[plane], row_bytes);
        if (err)
            return err;
    }
    return 0;
}

extern "C" int vmaf_sycl_picture_download(VmafSyclState *state, const void *src, VmafPicture *pic,
                                          unsigned plane)
{
    if (!state || !src || !pic)
        return -EINVAL;
    if (plane >= 3)
        return -EINVAL;

    size_t const bpp = (pic->bpc + 7) / 8;
    size_t const row_bytes = pic->w[plane] * bpp;
    size_t const total = row_bytes * pic->h[plane];

    if (pic->stride[plane] == (ptrdiff_t)row_bytes) {
        return vmaf_sycl_memcpy_d2h(state, pic->data[plane], src, total);
    }

    /* Non-contiguous — download packed, then scatter rows */
    void *packed = malloc(total);
    if (!packed)
        return -ENOMEM;

    int const err = vmaf_sycl_memcpy_d2h(state, packed, src, total);
    if (!err) {
        uint8_t *dst_ptr = (uint8_t *)pic->data[plane];
        for (unsigned y = 0; y < pic->h[plane]; y++) {
            memcpy(dst_ptr + y * pic->stride[plane], (uint8_t *)packed + y * row_bytes, row_bytes);
        }
    }
    free(packed);
    return err;
}

/* ------------------------------------------------------------------ */
/* VmafPicture pool callbacks                                          */
/* ------------------------------------------------------------------ */

extern "C" int vmaf_sycl_picture_alloc(VmafPicture *pic, void *cookie)
{
    if (!pic || !cookie)
        return -EINVAL;

    VmafSyclCookie const *c = (VmafSyclCookie *)cookie;
    assert(c->w > 0 && c->h > 0);
    assert(c->bpc >= 8 && c->bpc <= 16);
    size_t const bpp = (c->bpc + 7) / 8;
    size_t const plane_size = (size_t)c->w * c->h * bpp;

    /* Y plane only — VMAF operates on luma. DEVICE USM for GPU-resident,
     * HOST USM when the caller wants coherent host-visible buffers. */
    void *buf = nullptr;
    switch (c->method) {
    case VMAF_SYCL_POOL_HOST:
        buf = vmaf_sycl_malloc_host(c->state, plane_size);
        break;
    case VMAF_SYCL_POOL_DEVICE:
    default:
        buf = vmaf_sycl_malloc_device(c->state, plane_size);
        break;
    }
    if (!buf)
        return -ENOMEM;

    memset(pic, 0, sizeof(*pic));
    pic->data[0] = buf;
    pic->stride[0] = c->w * bpp;
    pic->w[0] = c->w;
    pic->h[0] = c->h;
    pic->bpc = c->bpc;
    pic->pix_fmt = c->pix_fmt;

    /* Attach priv + refcount so vmaf_picture_ref/unref work symmetrically
     * with host-backed pictures. buf_type tags this as SYCL-device-owned
     * so validate_pic_params can enforce consistent backing across ref/dist. */
    auto *priv = (VmafPicturePrivate *)calloc(1, sizeof(VmafPicturePrivate));
    if (!priv) {
        vmaf_sycl_free(c->state, buf);
        pic->data[0] = nullptr;
        return -ENOMEM;
    }
    priv->buf_type = VMAF_PICTURE_BUFFER_TYPE_SYCL_DEVICE;
    priv->cookie = cookie;
    priv->release_picture = vmaf_sycl_picture_free;
    pic->priv = priv;

    int const err = vmaf_ref_init(&pic->ref);
    if (err) {
        free(priv);
        pic->priv = nullptr;
        vmaf_sycl_free(c->state, buf);
        pic->data[0] = nullptr;
        return err;
    }

    return 0;
}

extern "C" int vmaf_sycl_picture_free(VmafPicture *pic, void *cookie)
{
    if (!pic || !cookie)
        return -EINVAL;

    VmafSyclCookie const *c = (VmafSyclCookie *)cookie;
    assert(c->state != nullptr);

    /* When invoked via vmaf_picture_unref the caller has already detached
     * priv + ref on its side, so only the USM buffers remain to free here.
     * When invoked directly by the pool on close, we also own priv + ref
     * and must release both. */
    for (unsigned i = 0; i < 3; i++) {
        if (pic->data[i]) {
            vmaf_sycl_free(c->state, pic->data[i]);
            pic->data[i] = nullptr;
        }
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Picture pool                                                        */
/* ------------------------------------------------------------------ */
/* ADR-0239: the round-robin slot/lock/init-unwind shape lives in the
 * backend-agnostic VmafGpuPicturePool. SYCL keeps its public-internal
 * `VmafSyclPicturePool` typedef + state-aware alloc/free hooks but
 * delegates the storage and locking to the generic pool. The wrapper
 * struct only exists to own the per-pool cookie so the
 * alloc/free/sync callbacks have a stable VmafSyclCookie pointer for
 * the lifetime of the pool. */

struct VmafSyclPicturePool {
    VmafSyclCookie cookie;
    VmafGpuPicturePool *gpool;
};

/* The generic pool's free callback fires both on per-slot init unwind
 * and on close; vmaf_sycl_picture_free only frees the USM data buffers,
 * so wrap it with the priv + ref cleanup the old SYCL-specific close
 * loop did. */
extern "C" {
static int sycl_pool_free_cb(VmafPicture *pic, void *cookie)
{
    int err = vmaf_sycl_picture_free(pic, cookie);
    if (pic->priv) {
        free(pic->priv);
        pic->priv = nullptr;
    }
    if (pic->ref) {
        vmaf_ref_close(pic->ref);
        pic->ref = nullptr;
    }
    return err;
}
}

extern "C" int vmaf_sycl_picture_pool_init(VmafSyclPicturePool **pool_out, VmafSyclState *state,
                                           unsigned pic_cnt, unsigned w, unsigned h, unsigned bpc,
                                           enum VmafPixelFormat pix_fmt,
                                           enum VmafSyclPoolMethod method)
{
    if (!pool_out || !state)
        return -EINVAL;
    if (pic_cnt == 0 || w == 0 || h == 0)
        return -EINVAL;
    if (bpc < 8 || bpc > 16)
        return -EINVAL;

    auto *wrap = new (std::nothrow) VmafSyclPicturePool();
    if (!wrap)
        return -ENOMEM;
    wrap->cookie.pix_fmt = pix_fmt;
    wrap->cookie.bpc = bpc;
    wrap->cookie.w = w;
    wrap->cookie.h = h;
    wrap->cookie.state = state;
    wrap->cookie.method = method;
    wrap->gpool = nullptr;

    VmafGpuPicturePoolConfig cfg = {};
    cfg.pic_cnt = pic_cnt;
    cfg.alloc_picture_callback = vmaf_sycl_picture_alloc;
    cfg.free_picture_callback = sycl_pool_free_cb;
    cfg.synchronize_picture_callback = nullptr;
    cfg.cookie = &wrap->cookie;

    int const err = vmaf_gpu_picture_pool_init(&wrap->gpool, cfg);
    if (err) {
        delete wrap;
        return err;
    }

    *pool_out = wrap;
    return 0;
}

extern "C" int vmaf_sycl_picture_pool_fetch(VmafSyclPicturePool *pool, VmafPicture *pic_out)
{
    if (!pool || !pic_out)
        return -EINVAL;
    /* vmaf_gpu_picture_pool_fetch handles round-robin + the
     * vmaf_picture_ref the caller will eventually unref. */
    return vmaf_gpu_picture_pool_fetch(pool->gpool, pic_out);
}

extern "C" int vmaf_sycl_picture_pool_close(VmafSyclPicturePool *pool)
{
    if (!pool)
        return -EINVAL;
    int const err = vmaf_gpu_picture_pool_close(pool->gpool);
    delete pool;
    return err;
}
