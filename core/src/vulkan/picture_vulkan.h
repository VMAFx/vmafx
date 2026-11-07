/**
 *  Copyright 2026 Lusoris and Claude (Anthropic)
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 */

#ifndef LIBVMAF_VULKAN_PICTURE_VULKAN_H_
#define LIBVMAF_VULKAN_PICTURE_VULKAN_H_

#include <stddef.h>
#include <stdint.h>

#include "libvmaf/picture.h"

#include "vulkan_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Host-visible Vulkan buffer that backs a single picture plane. The
 * struct itself is opaque on the public surface; kernel TUs in
 * libvmaf/src/feature/vulkan/ include `picture_vulkan_internal.h`
 * for the layout.
 */
typedef struct VmafVulkanBuffer VmafVulkanBuffer;

/*
 * Allocate an UPLOAD buffer — CPU writes, GPU reads.
 *
 * The buffer is sized exactly `size` bytes, allocated via VMA with
 * `VMA_MEMORY_USAGE_AUTO_PREFER_HOST` and
 * `VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT`.  VMA will
 * select a write-combining / BAR heap on discrete GPUs — optimal for
 * streaming host→device; not suited for CPU readback of GPU results.
 *
 * Exposes:
 *   - `host_ptr`: persistent mapped pointer (no map/unmap per upload).
 *   - `vk_buffer`: the VkBuffer handle for descriptor-set binding.
 *
 * Call vmaf_vulkan_buffer_flush() after each host write before dispatch.
 *
 * `*out_buf` receives a freshly allocated VmafVulkanBuffer; release
 * with vmaf_vulkan_buffer_free(). Returns 0 / -ENOMEM / -EINVAL.
 *
 * See ADR-0357 for the UPLOAD vs READBACK buffer classification.
 */
int vmaf_vulkan_buffer_alloc(VmafVulkanContext *ctx, VmafVulkanBuffer **out_buf, size_t size);

/*
 * Allocate a READBACK buffer — GPU writes, CPU reads.
 *
 * Uses `VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT` so VMA can select a
 * HOST_CACHED heap on discrete GPUs (VMA §5.3), giving 4–8x faster CPU
 * readback than the sequential-write / BAR path.
 *
 * Callers MUST call vmaf_vulkan_buffer_invalidate() after the GPU fence-wait
 * and before reading the result via vmaf_vulkan_buffer_host(), because
 * HOST_CACHED heaps are typically not HOST_COHERENT on dGPU drivers (Vulkan
 * 1.3 spec §11.2.2).
 *
 * Use for: per-workgroup accumulator slots, partial-sum arrays, any buffer
 * where the GPU is the writer and the CPU is the final reader.
 *
 * Returns 0 / -ENOMEM / -EINVAL.
 *
 * See ADR-0357 for the UPLOAD vs READBACK buffer classification.
 */
int vmaf_vulkan_buffer_alloc_readback(VmafVulkanContext *ctx, VmafVulkanBuffer **out_buf,
                                      size_t size);

/* Returns the persistent mapped host pointer (writeable). */
void *vmaf_vulkan_buffer_host(VmafVulkanBuffer *buf);

/* Returns the underlying VkBuffer handle as a uintptr_t so callers
 * who don't include <vulkan/vulkan.h> can still pass it through to
 * descriptor-set binding helpers. Internal kernel TUs use the
 * `picture_vulkan_internal.h` typed accessor instead. */
uintptr_t vmaf_vulkan_buffer_vkhandle(VmafVulkanBuffer *buf);

/* Returns the buffer size in bytes (as passed to alloc). */
size_t vmaf_vulkan_buffer_size(VmafVulkanBuffer *buf);

/* Flush host writes to the device. No-op for HOST_COHERENT memory but
 * we don't assume coherence — VMA may pick a non-coherent heap on
 * dGPUs (e.g. AMD ReBAR off). Call after every host upload before
 * dispatch. */
int vmaf_vulkan_buffer_flush(VmafVulkanContext *ctx, VmafVulkanBuffer *buf);

/*
 * Invalidate CPU-side cache lines for a READBACK buffer.
 *
 * Must be called after the GPU fence-wait and before reading results via
 * vmaf_vulkan_buffer_host() on any buffer allocated with
 * vmaf_vulkan_buffer_alloc_readback().  This is a no-op on HOST_COHERENT
 * heaps; on HOST_CACHED non-coherent heaps it flushes CPU cache lines so
 * the host sees the GPU's latest writes (Vulkan 1.3 spec §11.2.2).
 *
 * Returns 0 / -EIO / -EINVAL.
 */
int vmaf_vulkan_buffer_invalidate(VmafVulkanContext *ctx, VmafVulkanBuffer *buf);

void vmaf_vulkan_buffer_free(VmafVulkanContext *ctx, VmafVulkanBuffer *buf);

/* Invalidate the host-visible cache range so that GPU-written data
 * becomes visible to subsequent host reads. Required for non-coherent
 * host-visible heaps (common on dGPUs without ReBAR).  Call this
 * after a fence wait and before vmaf_vulkan_buffer_host() reads on
 * any buffer the GPU wrote (ADR-0350 two-level reduction output).
 * Returns 0 / -EINVAL / -EIO. */
int vmaf_vulkan_buffer_invalidate(VmafVulkanContext *ctx, VmafVulkanBuffer *buf);

/* Compatibility shim for the T5-1 scaffold smoke test. The original
 * picture-alloc API returned a void* directly. We keep the surface so
 * `libvmaf/test/test_vulkan_smoke.c` still compiles, but route the
 * call through `vmaf_vulkan_buffer_alloc` and return the host pointer.
 * Returns 0 / -ENOMEM / -EINVAL; on success `*out` is populated with
 * the host pointer. The companion buffer handle is opaque and freed
 * by `vmaf_vulkan_picture_free()` — which scans an internal bookkeeping
 * map keyed on host pointer. */
int vmaf_vulkan_picture_alloc(VmafVulkanContext *ctx, void **out, size_t size);
void vmaf_vulkan_picture_free(VmafVulkanContext *ctx, void *buf);

/* ---- ADR-0238: VmafPicture pool for the public preallocation surface ---- */

/* Internal pool method — picked from the public
 * `VmafVulkanPicturePreallocationMethod` by the libvmaf-side wrapper.
 * `NONE` is never instantiated as a pool (the public API short-circuits
 * to `vmaf_picture_alloc` directly). */
enum VmafVulkanPoolMethod {
    VMAF_VULKAN_POOL_HOST = 0,
    VMAF_VULKAN_POOL_DEVICE = 1,
};

typedef struct VmafVulkanPicturePool VmafVulkanPicturePool;

/* Allocate `pic_cnt` VmafPictures up-front. HOST uses regular
 * `vmaf_picture_alloc`; DEVICE backs each plane with a host-visible
 * VmafVulkanBuffer (VMA `AUTO_PREFER_HOST`). */
int vmaf_vulkan_picture_pool_init(VmafVulkanPicturePool **pool, VmafVulkanContext *ctx,
                                  unsigned pic_cnt, unsigned w, unsigned h, unsigned bpc,
                                  enum VmafPixelFormat pix_fmt, enum VmafVulkanPoolMethod method);

/* Hand the next VmafPicture to the caller. Round-robins through the
 * pic_cnt slots; the caller is expected to release ownership back to
 * the pool via the regular VmafPicture refcount path. Returns 0 /
 * -EINVAL. */
int vmaf_vulkan_picture_pool_fetch(VmafVulkanPicturePool *pool, VmafPicture *pic);

/* Release every backing buffer + the pool itself. Safe to call on
 * NULL. */
int vmaf_vulkan_picture_pool_close(VmafVulkanPicturePool *pool);

#ifdef __cplusplus
}
#endif

#endif /* LIBVMAF_VULKAN_PICTURE_VULKAN_H_ */
