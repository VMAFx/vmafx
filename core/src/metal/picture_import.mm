/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
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

#include <bit>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>

extern "C" {
#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_metal.h"
#include "libvmaf/picture.h"
#include "import.h"
#include "iosurface_layout.h"
#include "state_priv.h"
}

#include "objc_handle.h"

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

namespace {

struct MetalImportRing *ring_alloc(unsigned w, unsigned h, unsigned bpc)
{
    if (w == 0u || h == 0u) {
        return nullptr;
    }
    if (bpc != 8u && bpc != 10u && bpc != 12u && bpc != 16u) {
        return nullptr;
    }
    struct MetalImportRing *r =
        (struct MetalImportRing *)calloc(1, sizeof(*r));
    if (r == nullptr) {
        return nullptr;
    }
    r->w = w;
    r->h = h;
    r->bpc = bpc;
    /* Planar 4:2:0, the layout of every surface iosurface_layout.h
     * accepts (ADR-1679). Caller passes plane 0 = Y, plane 1 = U,
     * plane 2 = V; a bi-planar surface's U and V are the even and odd
     * samples of its second plane. */
    r->pix_fmt = VMAF_PIX_FMT_YUV420P;
    return r;
}

void slot_release(struct MetalImportSlot *s)
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

/* Plan the read of VmafPicture plane `plane` from the surface (ADR-1679):
 * the surface's own pixel format decides the layout, so a bi-planar NV12 or
 * P010 surface is de-interleaved and an MSB-aligned sample shifted, and a
 * layout outside iosurface_layout.h's table is refused rather than copied
 * as if it were planar. */
int plan_plane_read(IOSurfaceRef surf, const VmafPicture *pic, unsigned plane,
                           VmafMetalPlaneRead *rd)
{
    const VmafMetalSurfaceFormat *fmt =
        vmaf_metal_surface_format((uint32_t)IOSurfaceGetPixelFormat(surf));
    if (fmt == nullptr) {
        return -ENOTSUP;
    }
    const size_t src_plane = (size_t)vmaf_metal_surface_src_plane(fmt, plane);
    const VmafMetalSurfacePlane geometry = {
        .width = IOSurfaceGetWidthOfPlane(surf, src_plane),
        .height = IOSurfaceGetHeightOfPlane(surf, src_plane),
        .bytes_per_element = IOSurfaceGetBytesPerElementOfPlane(surf, src_plane),
        .bytes_per_row = IOSurfaceGetBytesPerRowOfPlane(surf, src_plane),
    };
    return vmaf_metal_plane_read_plan(fmt, IOSurfaceGetPlaneCount(surf), plane, pic->bpc,
                                      &geometry, pic->w[plane], pic->h[plane], rd);
}

/* Copy one planned plane out of the locked surface into the VmafPicture.
 * Handles stride mismatches (IOSurface stride is typically page-aligned and
 * >= vmaf_picture_alloc's DATA_ALIGN-rounded stride). */
int copy_plane(IOSurfaceRef surf, VmafPicture *pic, unsigned plane,
                      const VmafMetalPlaneRead *rd)
{
    if (pic->data[plane] == nullptr) {
        return -EINVAL;
    }
    const void *src = IOSurfaceGetBaseAddressOfPlane(surf, (size_t)rd->src_plane);
    if (src == nullptr) {
        return -EIO;
    }
    const size_t src_stride = IOSurfaceGetBytesPerRowOfPlane(surf, (size_t)rd->src_plane);
    vmaf_metal_read_plane((uint8_t *)pic->data[plane], (size_t)pic->stride[plane],
                          (const uint8_t *)src, src_stride, pic->w[plane], pic->h[plane], rd);
    return 0;
}
} // namespace

/* ----------------------------------------------------------------- */
/* Public C-API                                                       */
/* ----------------------------------------------------------------- */

int vmaf_metal_state_init_external(VmafMetalState **out,
                                   VmafMetalExternalHandles handles)
{
    if (out == nullptr) {
        return -EINVAL;
    }
    *out = nullptr;

    id<MTLDevice> device = nil;
    if (handles.device != 0u) {
        device = vmaf_metal::borrow<id<MTLDevice>>(handles.device);
        if (device == nil) {
            return -EINVAL;
        }
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
    if (handles.command_queue != 0u) {
        queue = vmaf_metal::borrow<id<MTLCommandQueue>>(handles.command_queue);
        if (queue == nil) {
            return -EINVAL;
        }
        if (queue.device != device) {
            return -EINVAL;
        }
    } else {
        queue = [device newCommandQueue];
        if (queue == nil) {
            return -ENOMEM;
        }
    }

    VmafMetalState *state = (VmafMetalState *)calloc(1, sizeof(*state));
    if (state == nullptr) {
        return -ENOMEM;
    }
    state->ctx.device_index = -1; /* external; no -d N enumeration */

    /* Device: __bridge_retained takes one +1 retain that vmaf_metal_state_free
     * balances with __bridge_transfer. Both caller-owned and library-created
     * devices go through the same path; the caller retains its own reference
     * independently so dropping ours on teardown is safe either way. */
    state->ctx.device = (__bridge_retained void *)device;
    /* Queue: same ownership contract — one __bridge_retained matched by one
     * __bridge_transfer in vmaf_metal_state_free, regardless of whether the
     * queue was caller-supplied or created here. */
    state->ctx.command_queue = (__bridge_retained void *)queue;
    state->import_ring = nullptr;

    *out = state;
    return 0;
}

namespace {

/* Lazy-allocate the ring on first import. Geometry is pinned to
 * the first frame's (w, h, bpc) — re-imports with different
 * dims surface as -EINVAL (caller must allocate a new state for
 * a resolution switch, same contract Vulkan enforces). */
int import_ring_for(VmafMetalState *state, unsigned w, unsigned h, unsigned bpc,
                           struct MetalImportRing **out)
{
    if (state->import_ring == nullptr) {
        struct MetalImportRing *const r = ring_alloc(w, h, bpc);
        if (r == nullptr) {
            return -ENOMEM;
        }
        state->import_ring = r;
    }
    struct MetalImportRing *ring = (struct MetalImportRing *)state->import_ring;
    if (ring->w != w || ring->h != h || ring->bpc != bpc) {
        return -EINVAL;
    }
    *out = ring;
    return 0;
}

/* The ref or dis picture of frame `index`, allocated on its first plane.
 * If the slot still holds an older frame's picture (caller didn't drain
 * via read_imported_pictures), it is discarded before the slot is reused. */
int slot_picture(struct MetalImportRing *ring, unsigned index, int is_ref,
                        VmafPicture **pic_out, unsigned **filled_out)
{
    struct MetalImportSlot *slot = &ring->slots[index % VMAF_METAL_IMPORT_RING];
    VmafPicture *pic = is_ref ? &slot->ref : &slot->dis;
    int *pending = is_ref ? &slot->ref_pending : &slot->dis_pending;
    unsigned *filled = is_ref ? &slot->ref_planes_filled : &slot->dis_planes_filled;
    unsigned *slot_index = is_ref ? &slot->ref_index : &slot->dis_index;

    if (*pending && *slot_index != index) {
        (void)vmaf_picture_unref(pic);
        *pending = 0;
        *filled = 0u;
    }
    if (!*pending) {
        int const err = vmaf_picture_alloc(pic, ring->pix_fmt, ring->bpc, ring->w, ring->h);
        if (err) {
            return err;
        }
        *pending = 1;
        *slot_index = index;
        *filled = 0u;
    }
    *pic_out = pic;
    *filled_out = filled;
    return 0;
}

/* Lock the IOSurface read-only, copy the planned plane into the
 * VmafPicture's host buffer, unlock. */
int read_locked_plane(IOSurfaceRef surf, VmafPicture *pic, unsigned plane,
                             const VmafMetalPlaneRead *rd)
{
    IOReturn const lock_ret = IOSurfaceLock(surf, kIOSurfaceLockReadOnly, nullptr);
    if (lock_ret != kIOReturnSuccess) {
        return -EIO;
    }
    int const err = copy_plane(surf, pic, plane, rd);
    IOReturn const unlock_ret = IOSurfaceUnlock(surf, kIOSurfaceLockReadOnly, nullptr);
    if (err) {
        return err;
    }
    return (unlock_ret != kIOReturnSuccess) ? -EIO : 0;
}
} // namespace

int vmaf_metal_picture_import(VmafMetalState *state, uintptr_t iosurface,
                              unsigned plane, unsigned w, unsigned h,
                              unsigned bpc, int is_ref, unsigned index)
{
    if (state == nullptr || iosurface == 0u || plane >= 3u) {
        return -EINVAL;
    }
    if (w == 0u || h == 0u || (is_ref != 0 && is_ref != 1)) {
        return -EINVAL;
    }
    IOSurfaceRef surf = std::bit_cast<IOSurfaceRef>(iosurface);

    struct MetalImportRing *ring = nullptr;
    int err = import_ring_for(state, w, h, bpc, &ring);
    if (err) {
        return err;
    }
    VmafPicture *pic = nullptr;
    unsigned *filled = nullptr;
    err = slot_picture(ring, index, is_ref, &pic, &filled);
    if (err) {
        return err;
    }
    VmafMetalPlaneRead rd;
    err = plan_plane_read(surf, pic, plane, &rd);
    if (err) {
        return err;
    }
    err = read_locked_plane(surf, pic, plane, &rd);
    if (err) {
        return err;
    }
    *filled |= (1u << plane);
    return 0;
}

int vmaf_metal_wait_compute(VmafMetalState *state)
{
    if (state == nullptr) {
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
    if (state == nullptr || out_ref == nullptr || out_dis == nullptr) {
        return -EINVAL;
    }
    if (state->import_ring == nullptr) {
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
    if (state == nullptr || state->import_ring == nullptr) {
        return;
    }
    struct MetalImportRing *ring = (struct MetalImportRing *)state->import_ring;
    for (auto & slot : ring->slots) {
        slot_release(&slot);
    }
    free(ring);
    state->import_ring = nullptr;
}
