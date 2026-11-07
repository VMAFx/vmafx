/**
 *  Copyright 2026 Lusoris and Claude (Anthropic)
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 *
 *  ADR-0238: VmafPicture pool for the public Vulkan preallocation
 *  surface (`VmafVulkanPicturePreallocationMethod`).
 *
 *  ADR-0239 follow-up: the round-robin / mutex / unwind shape now
 *  lives in the backend-agnostic `VmafGpuPicturePool`
 *  (`libvmaf/src/gpu_picture_pool.{h,c}`). This file shrank from
 *  ~180 LOC of hand-rolled lifecycle to ~80 LOC of Vulkan-specific
 *  alloc/free callbacks plus a thin wrapper struct that owns the
 *  per-pool state pointer for the callbacks.
 *
 *  HOST method backs each picture with regular `vmaf_picture_alloc()`;
 *  DEVICE method backs the luma plane with a host-visible
 *  VmafVulkanBuffer (VMA `AUTO_PREFER_HOST`) so the caller's host
 *  writes land in the same memory the kernel descriptor sets bind.
 */

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "libvmaf/picture.h"
#include "picture.h"
#include "ref.h"

#include "gpu_picture_pool.h"
#include "picture_vulkan.h"
#include "vulkan_internal.h"

/* Per-picture release closure: a single VmafVulkanBuffer was attached
 * to pic->data[0]. Stored as the picture's `cookie` so the unref path
 * can free the buffer without going through the pool. */
struct VmafVulkanPicReleaseCookie {
    VmafVulkanContext *ctx;
    VmafVulkanBuffer *buf;
};

static int release_device_picture(VmafPicture *pic, void *cookie)
{
    if (!pic || !cookie)
        return -EINVAL;
    struct VmafVulkanPicReleaseCookie *c = cookie;
    if (c->buf)
        vmaf_vulkan_buffer_free(c->ctx, c->buf);
    free(c);
    pic->data[0] = NULL;
    pic->stride[0] = 0;
    return 0;
}

/* Wrapper struct: owns the cookie storage so the alloc/free callbacks
 * registered with VmafGpuPicturePool have a stable pointer for the
 * pool's lifetime. The generic pool itself owns the round-robin slots,
 * the mutex, and the unwind logic. */
struct VmafVulkanPicturePool {
    VmafVulkanContext *ctx;
    enum VmafVulkanPoolMethod method;
    enum VmafPixelFormat pix_fmt;
    unsigned w, h, bpc;
    VmafGpuPicturePool *gpool;
};

/* Generic-pool alloc callback. The cookie is the wrapper struct
 * pointer (see init below); the wrapper carries the geometry the
 * callback needs. */
static int vulkan_pool_alloc_cb(VmafPicture *pic, void *cookie)
{
    struct VmafVulkanPicturePool *pool = cookie;
    if (pool->method == VMAF_VULKAN_POOL_HOST) {
        return vmaf_picture_alloc(pic, pool->pix_fmt, pool->bpc, pool->w, pool->h);
    }

    /* DEVICE method: per-picture host-visible VkBuffer. */
    assert(pool->ctx != NULL);
    assert(pool->bpc >= 8u && pool->bpc <= 16u);

    const size_t bpp = (pool->bpc + 7u) / 8u;
    const size_t plane_size = (size_t)pool->w * pool->h * bpp;

    VmafVulkanBuffer *buf = NULL;
    int err = vmaf_vulkan_buffer_alloc(pool->ctx, &buf, plane_size);
    if (err)
        return err;

    memset(pic, 0, sizeof(*pic));
    pic->data[0] = vmaf_vulkan_buffer_host(buf);
    pic->stride[0] = (int)((size_t)pool->w * bpp);
    pic->w[0] = pool->w;
    pic->h[0] = pool->h;
    pic->bpc = pool->bpc;
    pic->pix_fmt = pool->pix_fmt;

    VmafPicturePrivate *priv = calloc(1, sizeof(*priv));
    if (!priv) {
        vmaf_vulkan_buffer_free(pool->ctx, buf);
        pic->data[0] = NULL;
        return -ENOMEM;
    }
    priv->buf_type = VMAF_PICTURE_BUFFER_TYPE_VULKAN_DEVICE;
    priv->release_picture = release_device_picture;

    struct VmafVulkanPicReleaseCookie *rc = calloc(1, sizeof(*rc));
    if (!rc) {
        free(priv);
        vmaf_vulkan_buffer_free(pool->ctx, buf);
        pic->data[0] = NULL;
        return -ENOMEM;
    }
    rc->ctx = pool->ctx;
    rc->buf = buf;
    priv->cookie = rc;

    pic->priv = priv;

    err = vmaf_ref_init(&pic->ref);
    if (err) {
        free(rc);
        free(priv);
        vmaf_vulkan_buffer_free(pool->ctx, buf);
        pic->data[0] = NULL;
        pic->priv = NULL;
        return err;
    }
    return 0;
}

/* Generic-pool free callback. Mirrors the SYCL pattern (PR #266):
 * the per-method free routine + priv + ref cleanup that the old
 * hand-rolled close loop did. */
static int vulkan_pool_free_cb(VmafPicture *pic, void *cookie)
{
    struct VmafVulkanPicturePool *pool = cookie;
    if (pool->method == VMAF_VULKAN_POOL_DEVICE && pic->priv) {
        VmafPicturePrivate *priv = (VmafPicturePrivate *)pic->priv;
        if (priv->release_picture)
            (void)priv->release_picture(pic, priv->cookie);
        free(priv);
        pic->priv = NULL;
    } else {
        (void)vmaf_picture_unref(pic);
    }
    if (pic->ref) {
        vmaf_ref_close(pic->ref);
        pic->ref = NULL;
    }
    return 0;
}

int vmaf_vulkan_picture_pool_init(VmafVulkanPicturePool **pool_out, VmafVulkanContext *ctx,
                                  unsigned pic_cnt, unsigned w, unsigned h, unsigned bpc,
                                  enum VmafPixelFormat pix_fmt, enum VmafVulkanPoolMethod method)
{
    if (!pool_out || !ctx)
        return -EINVAL;
    if (pic_cnt == 0u || w == 0u || h == 0u)
        return -EINVAL;
    if (bpc < 8u || bpc > 16u)
        return -EINVAL;
    if (method != VMAF_VULKAN_POOL_HOST && method != VMAF_VULKAN_POOL_DEVICE)
        return -EINVAL;

    VmafVulkanPicturePool *pool = calloc(1, sizeof(*pool));
    if (!pool)
        return -ENOMEM;

    pool->ctx = ctx;
    pool->method = method;
    pool->pix_fmt = pix_fmt;
    pool->w = w;
    pool->h = h;
    pool->bpc = bpc;
    pool->gpool = NULL;

    VmafGpuPicturePoolConfig cfg = {0};
    cfg.pic_cnt = pic_cnt;
    cfg.alloc_picture_callback = vulkan_pool_alloc_cb;
    cfg.free_picture_callback = vulkan_pool_free_cb;
    cfg.synchronize_picture_callback = NULL;
    cfg.cookie = pool;

    const int err = vmaf_gpu_picture_pool_init(&pool->gpool, cfg);
    if (err) {
        free(pool);
        return err;
    }

    *pool_out = pool;
    return 0;
}

int vmaf_vulkan_picture_pool_fetch(VmafVulkanPicturePool *pool, VmafPicture *pic_out)
{
    if (!pool || !pic_out)
        return -EINVAL;
    return vmaf_gpu_picture_pool_fetch(pool->gpool, pic_out);
}

int vmaf_vulkan_picture_pool_close(VmafVulkanPicturePool *pool)
{
    if (!pool)
        return 0;
    const int err = vmaf_gpu_picture_pool_close(pool->gpool);
    free(pool);
    return err;
}
