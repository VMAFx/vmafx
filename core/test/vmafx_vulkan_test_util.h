/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * What the VMAFx Vulkan import tests share (RC4 WP3 Vulkan lane): each test
 * source is built once per device lane, with VMAFX_VK_LANE the VmafxBackend
 * value of the lane (1 CUDA, 2 SYCL, 4 HIP), and finds here how that lane
 * takes Vulkan frames:
 *
 * - CUDA: OPAQUE_FD memory of OPTIMAL images (CUDA arrays) and of buffers
 *   (device pointers); VULKAN_SEMAPHORE acquire fences waited on the device;
 *   release signalled into the producer's timeline on the device.
 * - SYCL: buffers exported as OPAQUE_FD (a dma-buf on the Mesa driver) and
 *   Tile4 DRM-modifier images exported as dma-bufs; a sync_file of the
 *   producer's write as the acquire fence.
 * - HIP: buffers exported as OPAQUE_FD and linear DRM-modifier images as
 *   dma-bufs; a sync_file acquire fence.
 *
 * The device is the backend's device 0, and the Vulkan producer the GPU at
 * its PCI location (VmafxDeviceInfo.pci).
 */

#ifndef VMAFX_VULKAN_TEST_UTIL_H
#define VMAFX_VULKAN_TEST_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "vmafx/vmafx.h"
#include "vmafx_fixture_util.h"
#include "vmafx_import_test_util.h"
#include "vmafx_test_util.h"
#include "vmafx_vulkan_producer.h"

#if VMAFX_VK_LANE == 2
#include "vmafx_sycl_cells.h"
#define VK_CELLS vs_cells
#define VK_N_CELLS VS_N_CELLS
#elif VMAFX_VK_LANE == 1 || VMAFX_VK_LANE == 4
#include "vmafx_device_cells.h"
#define VK_CELLS vc_cells
#define VK_N_CELLS VC_N_CELLS
#else
#error "VMAFX_VK_LANE: 1 (CUDA), 2 (SYCL) or 4 (HIP), set by core/test/meson.build"
#endif

/* NOLINTBEGIN(modernize-use-nullptr): C header. The fork builds C as C23,
 * where clang-tidy also proposes the `nullptr` keyword, but MSVC's documented
 * /std:clatest C23 feature set does not include `nullptr` and the required
 * Windows builds compile the tests with cl.exe (C2065). ADR-1138. */

/* I915_FORMAT_MOD_4_TILED (drm_fourcc.h): Tile4, the tiling of DG2 surfaces. */
#define VK_MOD_INTEL_TILE4 0x0100000000000009ull
/* An AMD vendor modifier (fourcc_mod_code(AMD, 1)): no SYCL or HIP device
 * here reads it. */
#define VK_MOD_AMD_UNREAD 0x0200000000000001ull

/* The lane waits on and signals Vulkan semaphores on the device (CUDA). */
#define VK_DEVICE_SEMAPHORES (VMAFX_VK_LANE == 1)

typedef struct VkLayoutCase {
    uint32_t layout; /* enum VkpLayout */
    bool dma_buf;
    uint64_t modifier;
    const char *name;
} VkLayoutCase;

static const VkLayoutCase vk_layouts[] = {
#if VMAFX_VK_LANE == 1
    {VKP_IMAGE_OPTIMAL, false, 0u, "optimal image, opaque fd"},
    {VKP_BUFFER, false, 0u, "buffer, opaque fd"},
#elif VMAFX_VK_LANE == 2
    {VKP_BUFFER, false, 0u, "buffer, opaque fd"},
    {VKP_IMAGE_DRM, true, VK_MOD_INTEL_TILE4, "Tile4 image, dma-buf"},
#else
    {VKP_BUFFER, false, 0u, "buffer, opaque fd"},
    {VKP_IMAGE_DRM, true, 0u, "linear image, dma-buf"},
#endif
};

#define VK_N_LAYOUTS (sizeof(vk_layouts) / sizeof(vk_layouts[0]))
/* The index of the buffer layout (LINEAR) in vk_layouts. */
#define VK_BUFFER_LAYOUT (VMAFX_VK_LANE == 1 ? 1u : 0u)

typedef struct VkGpu {
    VmafxDevice *device;
    VkpDevice *vk;
    uint32_t pci[4];
} VkGpu;

static inline const char *vk_lane_name(void)
{
    switch (VMAFX_VK_LANE) {
    case VMAFX_BACKEND_CUDA:
        return "CUDA";
    case VMAFX_BACKEND_SYCL:
        return "SYCL";
    case VMAFX_BACKEND_HIP:
        return "HIP";
    default:
        return "lane";
    }
}

/* Device 0 of the lane's backend and the Vulkan GPU at its PCI location;
 * false without either, with the reason the test skips on stderr. A CPU-only
 * host (a hosted lane with lavapipe as its only Vulkan device) skips here:
 * the import reads a GPU's Vulkan memory, which lavapipe has none of (Q-346). */
static inline bool vk_open(VkGpu *g)
{
    memset(g, 0, sizeof(*g));
    VmafxDeviceDesc desc = VMAFX_DEVICE_DESC_INIT;
    desc.backend = VMAFX_VK_LANE;
    desc.index = 0;
    VmafxDeviceInfo info = VMAFX_DEVICE_INFO_INIT;
    if (vmafx_device_create(&desc, &g->device, NULL) != VMAFX_OK ||
        vmafx_device_describe(g->device, &info, NULL) != VMAFX_OK) {
        (void)fprintf(stderr,
                      "skipped: no %s device, so no GPU Vulkan memory to import (lavapipe is a "
                      "CPU device; the producer needs the importing GPU's memory)\n",
                      vk_lane_name());
        return false;
    }
    memcpy(g->pci, info.pci, sizeof(g->pci));
    g->vk = vkp_open_pci(g->pci);
    if (!g->vk) {
        (void)fprintf(stderr,
                      "skipped: no GPU Vulkan device on %s %s (lavapipe is a CPU device; the "
                      "producer needs the importing GPU's memory)\n",
                      vk_lane_name(), info.name);
        return false;
    }
    const uint32_t v = vkp_loader_version();
    (void)fprintf(stderr, "[device %s; Vulkan producer %s; loader %u.%u.%u] ", info.name,
                  vkp_describe(g->vk), v >> 22, (v >> 12) & 0x3ffu, v & 0xfffu);
    return true;
}

static inline void vk_close(VkGpu *g)
{
    vkp_close(g->vk);
    vmafx_device_unref(g->device);
    memset(g, 0, sizeof(*g));
}

/* The frame `planar` (tightly packed, geometry `d`) packed as `pix_fmt`:
 * planar YUV420P as it is, NV12 / P010 / P016 with the chroma interleaved
 * (and P010 shifted up by 6). The caller frees it. */
static inline uint8_t *vk_pack(const VmafxFrameDesc *d, const uint8_t *planar, uint32_t pix_fmt)
{
    const size_t bytes = vt_frame_bytes(d);
    uint8_t *const out = malloc(bytes);
    if (out && pix_fmt == VMAFX_PIXEL_FORMAT_YUV420P) {
        memcpy(out, planar, bytes);
    } else if (out) {
        vt_to_semiplanar(d, planar, pix_fmt == VMAFX_PIXEL_FORMAT_P010 ? 6u : 0u, out);
    }
    return out;
}

/* One frame in Vulkan memory, written once and its timeline at 1. */
typedef struct VkFrame {
    VkpFrame *f;
    int sync; /* the sync_file of the write (lanes without device semaphores), else -1 */
} VkFrame;

static inline VkpFrameDesc vk_frame_desc(const VmafxFrameDesc *d, uint32_t pix_fmt,
                                         const VkLayoutCase *lay)
{
    const VkpFrameDesc fd = {.pix_fmt = pix_fmt,
                             .bpc = d->bpc,
                             .w = d->w,
                             .h = d->h,
                             .layout = lay->layout,
                             .dma_buf = lay->dma_buf,
                             .modifier = lay->modifier,
                             .sync_fd = !VK_DEVICE_SEMAPHORES};
    return fd;
}

/* Write `planar` into a new frame of `pix_fmt` in layout `lay`. */
static inline bool vk_frame_make(const VkGpu *g, const VmafxFrameDesc *d, const uint8_t *planar,
                                 uint32_t pix_fmt, const VkLayoutCase *lay, VkFrame *out)
{
    const VkpFrameDesc fd = vk_frame_desc(d, pix_fmt, lay);
    out->sync = -1;
    out->f = vkp_frame_new(g->vk, &fd);
    uint8_t *const packed = out->f ? vk_pack(d, planar, pix_fmt) : NULL;
    const VkpWrite w = {.packed = packed, .wait_gate = 0u, .wait_self = 0u, .signal = 1u};
    const bool ok = packed && vkp_frame_write(out->f, &w) == 0;
    free(packed);
    if (ok && !VK_DEVICE_SEMAPHORES) {
        out->sync = vkp_frame_sync_file(out->f);
    }
    return ok && (VK_DEVICE_SEMAPHORES || out->sync >= 0);
}

static inline void vk_frame_free(VkFrame *f)
{
    if (f->sync >= 0) {
        (void)close(f->sync);
    }
    vkp_frame_free(f->f);
    memset(f, 0, sizeof(*f));
    f->sync = -1;
}

/* The import descriptor of `f` with the lane's acquire fence for timeline
 * value `wait` (CUDA: the timeline; others: the write's sync_file, borrowed). */
static inline bool vk_describe(VkFrame *f, uint64_t wait, VmafxFrameImport *imp)
{
    if (vkp_frame_describe(f->f, VK_DEVICE_SEMAPHORES ? wait : 0u, imp) != 0) {
        return false;
    }
    if (!VK_DEVICE_SEMAPHORES && f->sync >= 0) {
        imp->acquire.kind = VMAFX_FENCE_SYNC_FILE;
        imp->acquire.fd = f->sync;
    }
    /* CUDA reads OPTIMAL images out of arrays: a planar frame is a device copy. */
    if (imp->vulkan_tiling == VMAFX_VULKAN_TILING_OPTIMAL &&
        imp->pix_fmt == VMAFX_PIXEL_FORMAT_YUV420P) {
        imp->flags |= VMAFX_IMPORT_ALLOW_COPY;
    }
    return true;
}

/* Import `f` through the import rule of `context` (or plainly without one). */
static inline VmafxFrame *vk_import(const VkGpu *g, VmafxContext *context, VkFrame *f)
{
    VmafxFrameImport imp;
    if (!vk_describe(f, 1u, &imp)) {
        return NULL;
    }
    VmafxFrame *frame = NULL;
    const VmafxStatus status =
        context ? vmafx_context_import_frame(context, g->device, &imp, "main", &frame, NULL) :
                  vmafx_frame_import(g->device, &imp, &frame, NULL);
    if (!VK_DEVICE_SEMAPHORES) {
        imp.acquire.kind = VMAFX_FENCE_NONE; /* the sync_file stays the frame's */
    }
    vkp_import_close(&imp);
    return status == VMAFX_OK ? frame : NULL;
}

/* NOLINTEND(modernize-use-nullptr) */

#endif /* VMAFX_VULKAN_TEST_UTIL_H */
