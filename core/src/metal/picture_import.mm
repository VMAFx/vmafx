/**
 *  Copyright 2026 Lusoris and Claude (Anthropic)
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 *
 *  IOSurface zero-copy import — implementation (ADR-0423 / T8-IOS).
 *  Replaces the audit-first scaffold's -ENOSYS stubs with real
 *  Objective-C++ wiring. The path is:
 *
 *      VideoToolbox -> CVPixelBufferRef -> IOSurfaceRef (caller side)
 *          -> vmaf_metal_picture_import(state, surf, plane, ...)
 *          -> IOSurfaceLock + memcpy into VmafPicture
 *          -> vmaf_metal_read_imported_pictures(ctx, index)
 *          -> vmaf_read_pictures(ctx, ref, dis, index)
 *
 *  Why memcpy and not [MTLDevice newTextureWithDescriptor:iosurface:plane:]:
 *  the libvmaf scoring pipeline consumes VmafPicture host pointers —
 *  the Metal feature kernels (ADR-0421) read shared-storage MTLBuffers
 *  that the runtime allocates per picture (picture_metal.mm). On Apple
 *  Silicon, MTLResourceStorageModeShared and host memory live in the
 *  same physical RAM; a CPU memcpy from the locked IOSurface backing
 *  store to a VmafPicture buffer is the same memory cost as a Blit
 *  encoder GPU copy, with no command-buffer round-trip. A future
 *  texture-direct path stays open (the C-API doesn't expose host
 *  pointers) and would land as ADR-0423 follow-up when a kernel needs
 *  per-sample texture access patterns.
 *
 *  Synchronisation: the IOSurface lock + memcpy path is synchronous;
 *  `vmaf_metal_wait_compute` is a no-op (the data is already host-
 *  visible by the time `vmaf_metal_picture_import` returns). Mirrors
 *  the Vulkan v1 contract before ADR-0251 ring back-pressure landed.
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>

extern "C" {
#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_metal.h"
#include "libvmaf/picture.h"
#include "import.h"
#include "state_priv.h"
}

/* Ring depth — caller is typically consuming index N-1 while
 * preparing index N. Two slots is enough for the FFmpeg
 * libvmaf_metal filter's serial dispatch (matches the SYCL
 * preallocation pool depth and the Vulkan v1 default). */
#define VMAF_METAL_IMPORT_RING 2u

/* Per-slot state. `ref` / `dis` are allocated via vmaf_picture_alloc
 * on the first plane import for (slot, is_ref) and handed to
 * vmaf_read_pictures on the read path (which takes ownership and
 * unrefs them). `planes_filled` tracks which planes have been
 * memcpy'd in so build_pictures can reject half-imported frames. */
struct MetalImportSlot {
    VmafPicture ref;
    VmafPicture dis;
    unsigned ref_index;
    unsigned dis_index;
    unsigned ref_planes_filled; /* bitmask: bit n = plane n filled */
    unsigned dis_planes_filled;
    int ref_pending;
    int dis_pending;
};

struct MetalImportRing {
    struct MetalImportSlot slots[VMAF_METAL_IMPORT_RING];
    unsigned w;   /* luma width (frame dims) */
    unsigned h;   /* luma height */
    unsigned bpc; /* bits per component */
    enum VmafPixelFormat pix_fmt;
};

static struct MetalImportRing *ring_alloc(unsigned w, unsigned h, unsigned bpc)
{
    if (w == 0u || h == 0u) {
        return NULL;
    }
    if (bpc != 8u && bpc != 10u && bpc != 12u && bpc != 16u) {
        return NULL;
    }
    struct MetalImportRing *r =
        (struct MetalImportRing *)calloc(1, sizeof(*r));
    if (r == NULL) {
        return NULL;
    }
    r->w = w;
    r->h = h;
    r->bpc = bpc;
    /* v1: planar 4:2:0 (matches VideoToolbox NV12 / P010 after the
     * libavfilter side de-interleaves). Caller passes plane 0 = Y,
     * plane 1 = U, plane 2 = V. */
    r->pix_fmt = VMAF_PIX_FMT_YUV420P;
    return r;
}

static void slot_release(struct MetalImportSlot *s)
{
    if (s->ref_pending) {
        (void)vmaf_picture_unref(&s->ref);
        s->ref_pending = 0;
    }
    if (s->dis_pending) {
        (void)vmaf_picture_unref(&s->dis);
        s->dis_pending = 0;
    }
    s->ref_planes_filled = 0u;
    s->dis_planes_filled = 0u;
    memset(&s->ref, 0, sizeof(s->ref));
    memset(&s->dis, 0, sizeof(s->dis));
}

/* Memcpy a single plane from a locked IOSurface into a VmafPicture
 * plane. Handles stride mismatches (IOSurface stride is typically
 * page-aligned and >= vmaf_picture_alloc's DATA_ALIGN-rounded
 * stride). */
static int copy_plane(VmafPicture *pic, unsigned plane,
                      const void *src, size_t src_stride)
{
    if (pic->data[plane] == NULL) {
        return -EINVAL;
    }
    const unsigned bpp = (pic->bpc > 8u) ? 2u : 1u;
    const size_t row_bytes = (size_t)pic->w[plane] * bpp;
    const size_t dst_stride = (size_t)pic->stride[plane];
    if (src_stride < row_bytes || dst_stride < row_bytes) {
        return -EINVAL;
    }
    const uint8_t *s = (const uint8_t *)src;
    uint8_t *d = (uint8_t *)pic->data[plane];
    for (unsigned y = 0u; y < pic->h[plane]; y++) {
        memcpy(d + (size_t)y * dst_stride,
               s + (size_t)y * src_stride,
               row_bytes);
    }
    return 0;
}

/* ----------------------------------------------------------------- */
/* Public C-API                                                       */
/* ----------------------------------------------------------------- */

int vmaf_metal_state_init_external(VmafMetalState **out,
                                   VmafMetalExternalHandles handles)
{
    if (out == NULL) {
        return -EINVAL;
    }
    *out = NULL;

    id<MTLDevice> device = nil;
    int device_owned_externally = 0;
    if (handles.device != 0u) {
        device = (__bridge id<MTLDevice>)(void *)handles.device;
        if (device == nil) {
            return -EINVAL;
        }
        device_owned_externally = 1;
    } else {
        /* Fallback: pick the system default Metal device. FFmpeg
         * n8.1.1 does not expose an AVMetalDeviceContext, so the
         * libvmaf_metal filter relies on this path. Once FFmpeg
         * ships a device-context API the caller passes the
         * VideoToolbox-rendering MTLDevice explicitly and we hit
         * the external-device branch. */
        device = MTLCreateSystemDefaultDevice();
        if (device == nil) {
            return -ENODEV;
        }
    }
    if (![device supportsFamily:MTLGPUFamilyApple7]) {
        return -ENODEV;
    }

    id<MTLCommandQueue> queue = nil;
    int queue_owned_externally = 0;
    if (handles.command_queue != 0u) {
        queue = (__bridge id<MTLCommandQueue>)(void *)handles.command_queue;
        if (queue == nil) {
            return -EINVAL;
        }
        if (queue.device != device) {
            return -EINVAL;
        }
        queue_owned_externally = 1;
    } else {
        queue = [device newCommandQueue];
        if (queue == nil) {
            return -ENOMEM;
        }
    }

    VmafMetalState *state = (VmafMetalState *)calloc(1, sizeof(*state));
    if (state == NULL) {
        return -ENOMEM;
    }
    state->ctx.device_index = -1; /* external; no -d N enumeration */

    /* Device: bridge-retain so we balance the bridge_transfer in
     * vmaf_metal_state_free. When caller-owned, retain ours
     * explicitly so we drop only our own reference on teardown. */
    if (device_owned_externally) {
        CFRetain((__bridge CFTypeRef)device);
    }
    state->ctx.device = (__bridge_retained void *)device;
    if (queue_owned_externally) {
        CFRetain((__bridge CFTypeRef)queue);
        state->ctx.command_queue = (__bridge_retained void *)queue;
    } else {
        state->ctx.command_queue = (__bridge_retained void *)queue;
    }
    state->import_ring = NULL;

    *out = state;
    return 0;
}

int vmaf_metal_picture_import(VmafMetalState *state, uintptr_t iosurface,
                              unsigned plane, unsigned w, unsigned h,
                              unsigned bpc, int is_ref, unsigned index)
{
    if (state == NULL || iosurface == 0u) {
        return -EINVAL;
    }
    if (plane >= 3u) {
        return -EINVAL;
    }
    if (w == 0u || h == 0u) {
        return -EINVAL;
    }
    if (is_ref != 0 && is_ref != 1) {
        return -EINVAL;
    }

    IOSurfaceRef surf = (IOSurfaceRef)(uintptr_t)iosurface;
    if (surf == NULL) {
        return -EINVAL;
    }

    /* Lazy-allocate the ring on first import. Geometry is pinned to
     * the first frame's (w, h, bpc) — re-imports with different
     * dims surface as -EINVAL (caller must allocate a new state for
     * a resolution switch, same contract Vulkan enforces). */
    if (state->import_ring == NULL) {
        struct MetalImportRing *r = ring_alloc(w, h, bpc);
        if (r == NULL) {
            return -ENOMEM;
        }
        state->import_ring = r;
    }
    struct MetalImportRing *ring = (struct MetalImportRing *)state->import_ring;
    if (ring->w != w || ring->h != h || ring->bpc != bpc) {
        return -EINVAL;
    }

    const unsigned slot_idx = index % VMAF_METAL_IMPORT_RING;
    struct MetalImportSlot *slot = &ring->slots[slot_idx];

    /* If the slot still holds an older frame's pictures (caller
     * didn't drain via read_imported_pictures), discard them
     * before re-using the slot. */
    if (is_ref) {
        if (slot->ref_pending && slot->ref_index != index) {
            (void)vmaf_picture_unref(&slot->ref);
            slot->ref_pending = 0;
            slot->ref_planes_filled = 0u;
        }
    } else {
        if (slot->dis_pending && slot->dis_index != index) {
            (void)vmaf_picture_unref(&slot->dis);
            slot->dis_pending = 0;
            slot->dis_planes_filled = 0u;
        }
    }

    VmafPicture *pic = is_ref ? &slot->ref : &slot->dis;
    int *pending = is_ref ? &slot->ref_pending : &slot->dis_pending;
    unsigned *filled = is_ref ? &slot->ref_planes_filled
                              : &slot->dis_planes_filled;
    unsigned *slot_index_field = is_ref ? &slot->ref_index : &slot->dis_index;

    if (!*pending) {
        int err = vmaf_picture_alloc(pic, ring->pix_fmt, ring->bpc,
                                     ring->w, ring->h);
        if (err) {
            return err;
        }
        *pending = 1;
        *slot_index_field = index;
        *filled = 0u;
    }

    /* Lock the IOSurface read-only and memcpy the requested plane
     * into the VmafPicture's host buffer. */
    IOReturn lock_ret = IOSurfaceLock(surf, kIOSurfaceLockReadOnly, NULL);
    if (lock_ret != kIOReturnSuccess) {
        return -EIO;
    }

    const void *src = IOSurfaceGetBaseAddressOfPlane(surf, (size_t)plane);
    const size_t src_stride = IOSurfaceGetBytesPerRowOfPlane(surf, (size_t)plane);
    int err = (src == NULL) ? -EIO : copy_plane(pic, plane, src, src_stride);

    IOReturn unlock_ret = IOSurfaceUnlock(surf, kIOSurfaceLockReadOnly, NULL);
    if (err) {
        return err;
    }
    if (unlock_ret != kIOReturnSuccess) {
        return -EIO;
    }

    *filled |= (1u << plane);
    return 0;
}

int vmaf_metal_wait_compute(VmafMetalState *state)
{
    if (state == NULL) {
        return -EINVAL;
    }
    /* Synchronous CPU memcpy path: data is host-visible the moment
     * IOSurfaceUnlock returns. Future GPU-async paths replace this
     * with a per-frame MTLSharedEvent drain (same shape as Vulkan
     * ring back-pressure under ADR-0251). */
    return 0;
}

/* ----------------------------------------------------------------- */
/* Internal helpers consumed by libvmaf.c HAVE_METAL block            */
/* ----------------------------------------------------------------- */

int vmaf_metal_state_build_pictures(VmafMetalState *state, unsigned index,
                                    VmafPicture *out_ref, VmafPicture *out_dis)
{
    if (state == NULL || out_ref == NULL || out_dis == NULL) {
        return -EINVAL;
    }
    if (state->import_ring == NULL) {
        return -EINVAL;
    }
    struct MetalImportRing *ring = (struct MetalImportRing *)state->import_ring;
    const unsigned slot_idx = index % VMAF_METAL_IMPORT_RING;
    struct MetalImportSlot *slot = &ring->slots[slot_idx];

    if (!slot->ref_pending || !slot->dis_pending) {
        return -EINVAL;
    }
    if (slot->ref_index != index || slot->dis_index != index) {
        return -EINVAL;
    }

    /* All 3 planes (Y/U/V) must have been imported. */
    const unsigned want = 0x7u;
    if ((slot->ref_planes_filled & want) != want) {
        return -EINVAL;
    }
    if ((slot->dis_planes_filled & want) != want) {
        return -EINVAL;
    }

    /* Transfer ownership: caller hands these to vmaf_read_pictures
     * which unrefs them. Slot returns to a fresh state for the next
     * frame at this ring position. */
    *out_ref = slot->ref;
    *out_dis = slot->dis;
    memset(&slot->ref, 0, sizeof(slot->ref));
    memset(&slot->dis, 0, sizeof(slot->dis));
    slot->ref_pending = 0;
    slot->dis_pending = 0;
    slot->ref_planes_filled = 0u;
    slot->dis_planes_filled = 0u;
    return 0;
}

void vmaf_metal_state_import_ring_free(VmafMetalState *state)
{
    if (state == NULL || state->import_ring == NULL) {
        return;
    }
    struct MetalImportRing *ring = (struct MetalImportRing *)state->import_ring;
    for (unsigned i = 0u; i < VMAF_METAL_IMPORT_RING; i++) {
        slot_release(&ring->slots[i]);
    }
    free(ring);
    state->import_ring = NULL;
}
