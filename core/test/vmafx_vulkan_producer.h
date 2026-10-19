/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * The producer side of the VMAFx Vulkan import tests (RC4 WP3 Vulkan lane):
 * a Vulkan device on one GPU that writes frames into exportable images or
 * buffers with transfer commands, orders them with timeline semaphores as
 * FFmpeg's AVVkFrame and GStreamer's Vulkan memory do, and describes a
 * written frame as a VmafxFrameImport of VMAFX_MEMORY_VULKAN memory.
 *
 * Every write can wait on the device first, for a value of a gate the test
 * signals from the host (so the consumer is provably ahead of the write) or
 * for a value of the frame's own timeline (the release the library signals),
 * the two orderings the fence tests check. No Vulkan compute: the writes are
 * buffer-to-image (or buffer-to-buffer) copies of host data.
 */

#ifndef VMAFX_VULKAN_PRODUCER_H
#define VMAFX_VULKAN_PRODUCER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "vmafx/vmafx.h"

/* NOLINTBEGIN(modernize-use-using): C header included by C translation units only. ADR-1138. */
typedef struct VkpDevice VkpDevice;
typedef struct VkpFrame VkpFrame;
/* NOLINTEND(modernize-use-using) */

/* Where the planes of a frame live. */
enum VkpLayout {
    VKP_IMAGE_OPTIMAL = 0, /* one image per plane, VK_IMAGE_TILING_OPTIMAL */
    VKP_IMAGE_LINEAR = 1,  /* one image per plane, VK_IMAGE_TILING_LINEAR */
    VKP_BUFFER = 2,        /* one buffer per plane, rows at a pitch */
    VKP_IMAGE_DRM = 3,     /* one image per plane, VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT */
};

typedef struct VkpFrameDesc {
    uint32_t pix_fmt;  /* VMAFX_PIXEL_FORMAT_YUV420P, NV12, P010 or P016 */
    uint32_t bpc;      /* 8 to 16 */
    uint32_t w;        /* luma width */
    uint32_t h;        /* luma height */
    uint32_t layout;   /* enum VkpLayout */
    bool dma_buf;      /* export VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, else OPAQUE_FD */
    uint64_t modifier; /* VKP_IMAGE_DRM: the one modifier to ask for */
    bool sync_fd;      /* every write also signals a binary semaphore exportable as a sync_file */
} VkpFrameDesc;

/* One write of a frame. */
typedef struct VkpWrite {
    const uint8_t *packed; /* tightly packed planes in the frame's pix_fmt (luma, then chroma) */
    uint64_t wait_gate;    /* the device waits for the gate to reach it first; 0: no wait */
    uint64_t wait_self;    /* ... and for the frame's timeline to reach it; 0: no wait */
    uint64_t signal;       /* the frame's timeline value the write signals (> every earlier) */
    uint32_t busy;         /* copies of a 64 MiB scratch buffer queued before the write, so
                            * it lands later on the device without a host-signalled wait */
} VkpWrite;

/* The first Vulkan GPU of PCI vendor `vendor` (0x10de, 0x1002, 0x8086), or
 * NULL without one (the reason on stderr). */
VkpDevice *vkp_open(uint32_t vendor);
/* The Vulkan GPU at PCI location `pci` (VmafxDeviceInfo.pci), or NULL. */
VkpDevice *vkp_open_pci(const uint32_t pci[4]);
/* The first Vulkan GPU at another PCI location than `pci`, or NULL. */
VkpDevice *vkp_open_other(const uint32_t pci[4]);
/* The index of the Vulkan GPU at `pci` among the loader's physical devices
 * (FFmpeg's "vulkan" device index), or -1. */
int vkp_index_of_pci(const uint32_t pci[4]);
void vkp_close(VkpDevice *d);
/* PCI domain, bus, device and function of the device (VK_EXT_pci_bus_info). */
void vkp_pci(const VkpDevice *d, uint32_t pci[4]);
/* Device name, driver, driver info and API version, for the evidence. */
const char *vkp_describe(const VkpDevice *d);
/* The loader's instance version, for the evidence. */
uint32_t vkp_loader_version(void);
/* Signal the device's gate to `value` from the host. 0 or -1. */
int vkp_gate_signal(VkpDevice *d, uint64_t value);

/* A frame of `desc`, or NULL (the reason on stderr; -ENOTSUP-like
 * unsupported layouts print "unsupported"). */
VkpFrame *vkp_frame_new(VkpDevice *d, const VkpFrameDesc *desc);
/* Wait for the frame's work and free it. */
void vkp_frame_free(VkpFrame *f);
/* Submit one write. 0 or -1. */
int vkp_frame_write(VkpFrame *f, const VkpWrite *w);
/* Host wait for the frame's timeline to reach `value`: 1 reached, 0 not
 * within `timeout_ns`, -1 on an error. */
int vkp_frame_wait(VkpFrame *f, uint64_t value, uint64_t timeout_ns);
/* The frame's timeline value now, or UINT64_MAX on an error. */
uint64_t vkp_frame_value(VkpFrame *f);
/* Signal the frame's timeline to `value` from the host (vkSignalSemaphore).
 * 0 or -1. */
int vkp_frame_signal_host(VkpFrame *f, uint64_t value);
/* The sync_file of the last write's binary semaphore (desc.sync_fd), a new
 * descriptor the caller owns, or -1. */
int vkp_frame_sync_file(VkpFrame *f);

/* `imp` describes the frame as VMAFX_MEMORY_VULKAN memory with freshly
 * exported descriptors (one per plane), and `acquire` set to the frame's
 * timeline at `wait` (0: NONE). Close them with vkp_import_close(). 0 or -1. */
int vkp_frame_describe(VkpFrame *f, uint64_t wait, VmafxFrameImport *imp);
/* Close the descriptors vkp_frame_describe() exported into `imp`. */
void vkp_import_close(VmafxFrameImport *imp);
/* A VULKAN_SEMAPHORE fence on the frame's timeline at `value`, with a freshly
 * exported descriptor the caller closes. 0 or -1. */
int vkp_frame_fence(VkpFrame *f, uint64_t value, VmafxFence *fence);

#endif /* VMAFX_VULKAN_PRODUCER_H */
