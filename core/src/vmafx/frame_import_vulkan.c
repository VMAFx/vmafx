/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * The checks every device lane shares for VMAFX_MEMORY_VULKAN imports and
 * the further acquire fences (RC4 WP3 Vulkan lane, ADR-1852 design section
 * 2.7, ADR-1897). Import only (Q-011): the library never creates a Vulkan
 * object, it imports what a Vulkan producer exported.
 *
 * A VULKAN descriptor names how the memory was exported, the images' tiling
 * and the producer's GPU. The checks here are the ones that do not depend on
 * the device: the handle and tiling are values the API knows, each plane has
 * its descriptor and the allocation's size, its rows fit in it, and the
 * producer's GPU is the device's (memory of another GPU is refused, never
 * read across devices). A lane that reads Linux dma-bufs (SYCL, HIP) takes a
 * LINEAR or DRM_FORMAT_MODIFIER frame through its DMABUF path
 * (vmafx_import_vulkan_as_dmabuf()); the CUDA lane imports the opaque memory
 * itself (cuda/import_vulkan.c).
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "error_internal.h"
#include "internal.h"
#include "picture.h"
#include "sync_object.h"
#include "vmafx/vmafx.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

static const char *const acquire_more_names[] = {"desc.acquire_more[0]", "desc.acquire_more[1]"};

const char *vmafx_vulkan_handle_name(uint32_t type)
{
    switch (type) {
    case VMAFX_VULKAN_HANDLE_OPAQUE_FD:
        return "OPAQUE_FD";
    case VMAFX_VULKAN_HANDLE_OPAQUE_WIN32:
        return "OPAQUE_WIN32";
    case VMAFX_VULKAN_HANDLE_OPAQUE_WIN32_KMT:
        return "OPAQUE_WIN32_KMT";
    case VMAFX_VULKAN_HANDLE_DMA_BUF:
        return "DMA_BUF";
    default:
        return "unknown";
    }
}

const char *vmafx_vulkan_tiling_name(uint32_t tiling)
{
    switch (tiling) {
    case VMAFX_VULKAN_TILING_OPTIMAL:
        return "OPTIMAL";
    case VMAFX_VULKAN_TILING_LINEAR:
        return "LINEAR";
    case VMAFX_VULKAN_TILING_DRM_FORMAT_MODIFIER:
        return "DRM_FORMAT_MODIFIER";
    default:
        return "unknown";
    }
}

/* ---- Descriptor fields ------------------------------------------------------------- */

/* The handle type, tiling and flags are values the API knows; Windows
 * handles are declared, and refused until a Windows device runs the tests. */
static VmafxStatus check_vulkan_fields(const VmafxReport *report, const VmafxFrameImport *d)
{
    const uint32_t type = d->vulkan_handle_type;
    if (strcmp(vmafx_vulkan_handle_name(type), "unknown") == 0) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER,
                          "desc.vulkan_handle_type",
                          "handle type %u is not a VmafxVulkanHandleType; VULKAN memory says how "
                          "it was exported (OPAQUE_FD, DMA_BUF, OPAQUE_WIN32, OPAQUE_WIN32_KMT)",
                          (unsigned)type);
    }
    if (type == VMAFX_VULKAN_HANDLE_OPAQUE_WIN32 || type == VMAFX_VULKAN_HANDLE_OPAQUE_WIN32_KMT) {
        return VMAFX_FAIL(report, VMAFX_E_NOTSUP, 0, VMAFX_SUBJECT_PARAMETER,
                          "desc.vulkan_handle_type",
                          "handle type %s: Windows handles of Vulkan memory are not imported "
                          "until a Windows device runs their tests; on Linux export OPAQUE_FD "
                          "or DMA_BUF",
                          vmafx_vulkan_handle_name(type));
    }
    if (strcmp(vmafx_vulkan_tiling_name(d->vulkan_tiling), "unknown") == 0) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER, "desc.vulkan_tiling",
                          "tiling %u is not a VmafxVulkanTiling", (unsigned)d->vulkan_tiling);
    }
    if (d->vulkan_flags & ~(uint32_t)VMAFX_VULKAN_DEDICATED) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER, "desc.vulkan_flags",
                          "unknown flag bits 0x%x",
                          (unsigned)(d->vulkan_flags & ~VMAFX_VULKAN_DEDICATED));
    }
    return VMAFX_OK;
}

/* The rows of a LINEAR or DRM_FORMAT_MODIFIER plane fit in its allocation. */
static VmafxStatus check_vulkan_rows(const VmafxReport *report, const VmafxImportPlane *p,
                                     uint32_t i, uint64_t row, uint64_t rows)
{
    if (p->pitch < row) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PLANE,
                          vmafx_import_plane_field(i, "pitch"),
                          "plane %u: pitch %llu cannot hold a row of %llu bytes", (unsigned)i,
                          (unsigned long long)p->pitch, (unsigned long long)row);
    }
    const bool fits = (rows - 1u) <= (UINT64_MAX - row) / p->pitch && p->offset <= p->size &&
                      (rows - 1u) * p->pitch + row <= p->size - p->offset;
    if (!fits) {
        return VMAFX_FAIL(report, VMAFX_E_RANGE, 0, VMAFX_SUBJECT_PLANE,
                          vmafx_import_plane_field(i, "size"),
                          "plane %u: %llu rows of pitch %llu from offset %llu do not fit in the "
                          "allocation of %llu bytes",
                          (unsigned)i, (unsigned long long)rows, (unsigned long long)p->pitch,
                          (unsigned long long)p->offset, (unsigned long long)p->size);
    }
    return VMAFX_OK;
}

/* One plane: its descriptor, its allocation's size, one image per plane,
 * and where its samples are. */
static VmafxStatus check_vulkan_plane(const VmafxReport *report, const VmafxFrameImport *d,
                                      uint32_t i, uint64_t row, uint64_t rows)
{
    const VmafxImportPlane *const p = &d->plane[i];
    if (p->fd < 0) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PLANE,
                          vmafx_import_plane_field(i, "fd"),
                          "plane %u: no descriptor of the exported memory", (unsigned)i);
    }
    if (p->size == 0u) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PLANE,
                          vmafx_import_plane_field(i, "size"),
                          "plane %u: the allocation's size (VkMemoryRequirements.size, AVVkFrame "
                          "size[]) is required",
                          (unsigned)i);
    }
    if (p->plane_index != 0u) {
        return VMAFX_FAIL(report, VMAFX_E_NOTSUP, 0, VMAFX_SUBJECT_PLANE,
                          vmafx_import_plane_field(i, "plane_index"),
                          "plane %u: plane %u of a multi-plane image; the devices import one "
                          "image per plane (FFmpeg: AV_VK_FRAME_FLAG_DISABLE_MULTIPLANE)",
                          (unsigned)i, (unsigned)p->plane_index);
    }
    if (d->vulkan_tiling == VMAFX_VULKAN_TILING_LINEAR && p->modifier != 0u) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PLANE,
                          vmafx_import_plane_field(i, "modifier"),
                          "plane %u: modifier 0x%llx on LINEAR tiling (DRM_FORMAT_MODIFIER tiling "
                          "carries a modifier)",
                          (unsigned)i, (unsigned long long)p->modifier);
    }
    if (d->vulkan_tiling != VMAFX_VULKAN_TILING_OPTIMAL) {
        return check_vulkan_rows(report, p, i, row, rows);
    }
    return p->offset < p->size ?
               VMAFX_OK :
               VMAFX_FAIL(report, VMAFX_E_RANGE, 0, VMAFX_SUBJECT_PLANE,
                          vmafx_import_plane_field(i, "offset"),
                          "plane %u: offset %llu past the allocation of %llu bytes", (unsigned)i,
                          (unsigned long long)p->offset, (unsigned long long)p->size);
}

VmafxStatus vmafx_import_check_vulkan(const VmafxReport *report, const VmafxFrameImport *d,
                                      const VmafxImportLayout *layout)
{
    assert(d->memory == VMAFX_MEMORY_VULKAN);
    VmafxStatus status = check_vulkan_fields(report, d);
    unsigned pw[3];
    unsigned ph[3];
    vmaf_picture_plane_extents(vmafx_engine_pixel_format(layout->planar_fmt), d->w, d->h, pw, ph);
    for (uint32_t i = 0; i < layout->n_planes && status == VMAFX_OK; i++) {
        uint64_t row = 0;
        uint64_t rows = 0;
        vmafx_import_plane_extent(layout, d->bpc, i, pw, ph, &row, &rows);
        status = check_vulkan_plane(report, d, i, row, rows);
    }
    return status;
}

/* ---- The producer's GPU -------------------------------------------------------------- */

VmafxStatus vmafx_import_check_vulkan_device(const VmafxReport *report, const VmafxFrameImport *d,
                                             const uint32_t pci[4], const char *backend)
{
    static const uint32_t unknown[4] = {UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};
    if (memcmp(pci, unknown, sizeof(unknown)) == 0) {
        return VMAFX_FAIL(report, VMAFX_E_NOTSUP, 0, VMAFX_SUBJECT_DEVICE, "device",
                          "backend %s: the device reports no PCI location, so VULKAN memory "
                          "cannot be proven to be its own and is refused",
                          backend);
    }
    if (memcmp(d->vulkan_pci, pci, sizeof(d->vulkan_pci)) == 0) {
        return VMAFX_OK;
    }
    return VMAFX_FAIL(report, VMAFX_E_NOTSUP, 0, VMAFX_SUBJECT_PARAMETER, "desc.vulkan_pci",
                      "backend %s: VULKAN memory of the GPU at PCI %04x:%02x:%02x.%x, the device "
                      "is at %04x:%02x:%02x.%x; memory of another GPU is never read across "
                      "devices (create the Vulkan device on the scoring GPU)",
                      backend, (unsigned)d->vulkan_pci[0], (unsigned)d->vulkan_pci[1],
                      (unsigned)d->vulkan_pci[2], (unsigned)d->vulkan_pci[3], (unsigned)pci[0],
                      (unsigned)pci[1], (unsigned)pci[2], (unsigned)pci[3]);
}

/* ---- Further acquire fences ---------------------------------------------------------- */

VmafxStatus vmafx_import_check_acquire_more(const VmafxReport *report, const VmafxFrameImport *d,
                                            uint32_t backend)
{
    for (uint32_t i = 0; i < 2u; i++) {
        const uint32_t kind = d->acquire_more[i].kind;
        if (kind == VMAFX_FENCE_NONE) {
            continue;
        }
        if (kind > VMAFX_FENCE_KIND_LAST) {
            return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_FENCE,
                              acquire_more_names[i], "kind %u is not a VmafxFenceKind",
                              (unsigned)kind);
        }
        if (backend != VMAFX_BACKEND_CUDA) {
            return VMAFX_FAIL(report, VMAFX_E_NOTSUP, 0, VMAFX_SUBJECT_FENCE, acquire_more_names[i],
                              "backend %s: further acquire fences (kind %u); this device takes "
                              "one acquire fence per import (a Vulkan producer exports one "
                              "sync_file for the whole frame)",
                              vmafx_backend_name(backend), (unsigned)kind);
        }
    }
    return VMAFX_OK;
}

const char *vmafx_import_acquire_name(uint32_t i)
{
    assert(i < 3u);
    return i == 0u ? "desc.acquire" : acquire_more_names[i - 1u];
}

/* ---- The dma-buf route of the SYCL and HIP lanes ------------------------------------- */

VmafxStatus vmafx_import_vulkan_as_dmabuf(const VmafxReport *report, const VmafxFrameImport *d,
                                          const char *backend, VmafxFrameImport *out)
{
    assert(d->memory == VMAFX_MEMORY_VULKAN);
    if (d->vulkan_tiling == VMAFX_VULKAN_TILING_OPTIMAL) {
        return VMAFX_FAIL(report, VMAFX_E_NOTSUP, 0, VMAFX_SUBJECT_PARAMETER, "desc.vulkan_tiling",
                          "backend %s: OPTIMAL tiling is a layout only the producer's driver "
                          "knows; this device reads LINEAR images or buffers and DRM format "
                          "modifier images (export the frame with "
                          "VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT or LINEAR)",
                          backend);
    }
    for (uint32_t i = 0; i < d->n_planes && i < 3u; i++) {
        if (d->vulkan_handle_type == VMAFX_VULKAN_HANDLE_OPAQUE_FD &&
            !vmafx_fd_is_dmabuf(d->plane[i].fd)) {
            return VMAFX_FAIL(report, VMAFX_E_NOTSUP, 0, VMAFX_SUBJECT_PARAMETER,
                              "desc.vulkan_handle_type",
                              "backend %s: plane %u's OPAQUE_FD descriptor is no dma-buf; this "
                              "device imports Vulkan memory as a dma-buf (export DMA_BUF)",
                              backend, (unsigned)i);
        }
    }
    *out = *d;
    out->memory = VMAFX_MEMORY_DMABUF;
    return VMAFX_OK;
}

/* NOLINTEND(modernize-use-nullptr) */
