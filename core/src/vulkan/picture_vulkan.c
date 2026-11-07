/**
 *  Copyright 2026 Lusoris and Claude (Anthropic)
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 *
 *  Picture allocation / lifecycle for the Vulkan backend (T5-1b).
 *
 *  Two buffer-classification functions are provided (ADR-0357):
 *
 *    vmaf_vulkan_buffer_alloc()          — UPLOAD buffers (CPU writes, GPU
 *        reads).  VMA flag: HOST_ACCESS_SEQUENTIAL_WRITE.  VMA prefers a
 *        write-combining, non-cached heap on dGPUs (PCIe BAR1).  Optimal
 *        for streaming host→device transfers; reads back from host are slow.
 *
 *    vmaf_vulkan_buffer_alloc_readback() — READBACK buffers (GPU writes, CPU
 *        reads).  VMA flag: HOST_ACCESS_RANDOM.  VMA prefers a HOST_CACHED
 *        heap on dGPUs, giving full CPU cache-line bandwidth on readback.
 *        Required before CPU readback: vmaf_vulkan_buffer_invalidate() to
 *        flush CPU-side cache lines on non-coherent heaps.
 *
 *  Caller responsibility:
 *    - After host writes to an UPLOAD buffer: call vmaf_vulkan_buffer_flush().
 *    - After GPU writes to a READBACK buffer (post fence-wait): call
 *      vmaf_vulkan_buffer_invalidate() before reading via
 *      vmaf_vulkan_buffer_host().
 *    - Bidirectional buffers (alternating host/GPU writes in the same frame,
 *      e.g., cambi pipeline scratch) use the UPLOAD variant — they are
 *      memcpy'd by the host and then overwritten by the GPU in a later pass,
 *      so the sequential-write heap is the safer choice; the CPU never needs
 *      to read a stale GPU write from them at full bandwidth.
 */

#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "picture_vulkan.h"
#include "vulkan_internal.h"

struct VmafVulkanBuffer {
    VkBuffer vk_buffer;
    VmaAllocation allocation;
    VmaAllocationInfo info;
    size_t size;
};

/* Bookkeeping for the legacy `void*` picture-alloc API used by the
 * T5-1 smoke test. We allocate at most a handful of pictures across
 * a session, so a flat append-only array with linear scan is fine.
 * If the count ever grows beyond ~32 entries we should swap this for
 * a hash table; for now keep it simple. */
typedef struct {
    void *host_ptr;
    VmafVulkanBuffer *buf;
} ShimEntry;

#define VMAF_VK_SHIM_MAX 32
static ShimEntry g_shim_entries[VMAF_VK_SHIM_MAX];
static size_t g_shim_count = 0;

/* --- shared buffer-create helper ----------------------------------------- */

static int alloc_buffer_impl(VmafVulkanContext *ctx, VmafVulkanBuffer **out_buf, size_t size,
                             VmaAllocationCreateFlags vma_flags)
{
    if (!ctx || !out_buf || size == 0)
        return -EINVAL;
    assert(ctx->allocator != VK_NULL_HANDLE);
    assert(ctx->device != VK_NULL_HANDLE);

    VmafVulkanBuffer *b = calloc(1, sizeof(*b));
    if (!b)
        return -ENOMEM;
    b->size = size;
    assert(b->vk_buffer == VK_NULL_HANDLE);

    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VmaAllocationCreateInfo aci = {
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        .flags = vma_flags | VMA_ALLOCATION_CREATE_MAPPED_BIT,
    };
    VkResult vkr =
        vmaCreateBuffer(ctx->allocator, &bci, &aci, &b->vk_buffer, &b->allocation, &b->info);
    if (vkr != VK_SUCCESS) {
        free(b);
        return -ENOMEM;
    }
    *out_buf = b;
    return 0;
}

/* --- public API ----------------------------------------------------------- */

/*
 * UPLOAD buffer — CPU writes, GPU reads.
 *
 * Uses VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT so VMA can
 * select a write-combining / BAR heap on discrete GPUs.  Optimal for
 * streaming frame data host→device.  Not suited for CPU readback.
 */
int vmaf_vulkan_buffer_alloc(VmafVulkanContext *ctx, VmafVulkanBuffer **out_buf, size_t size)
{
    return alloc_buffer_impl(ctx, out_buf, size,
                             VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
}

/*
 * READBACK buffer — GPU writes, CPU reads.
 *
 * Uses VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT so VMA can select a
 * HOST_CACHED heap on discrete GPUs (VMA §5.3).  CPU readback bandwidth is
 * 4–8x faster than a sequential-write / BAR heap.  Callers MUST call
 * vmaf_vulkan_buffer_invalidate() after the GPU fence-wait and before
 * calling vmaf_vulkan_buffer_host() to read results, because HOST_CACHED
 * heaps are not HOST_COHERENT on most dGPU drivers (Vulkan 1.3 spec
 * §11.2.2).
 */
int vmaf_vulkan_buffer_alloc_readback(VmafVulkanContext *ctx, VmafVulkanBuffer **out_buf,
                                      size_t size)
{
    return alloc_buffer_impl(ctx, out_buf, size, VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT);
}

void *vmaf_vulkan_buffer_host(VmafVulkanBuffer *buf)
{
    return buf ? buf->info.pMappedData : NULL;
}

uintptr_t vmaf_vulkan_buffer_vkhandle(VmafVulkanBuffer *buf)
{
    return buf ? (uintptr_t)buf->vk_buffer : 0;
}

size_t vmaf_vulkan_buffer_size(VmafVulkanBuffer *buf)
{
    return buf ? buf->size : 0;
}

/*
 * Flush host writes to the device.  No-op for HOST_COHERENT memory, but we
 * never assume coherence — VMA may pick a non-coherent heap (e.g., AMD ReBAR
 * off).  Call after every host upload before dispatch.
 */
int vmaf_vulkan_buffer_flush(VmafVulkanContext *ctx, VmafVulkanBuffer *buf)
{
    if (!ctx || !buf)
        return -EINVAL;
    VkResult vkr = vmaFlushAllocation(ctx->allocator, buf->allocation, 0, VK_WHOLE_SIZE);
    return (vkr == VK_SUCCESS) ? 0 : -EIO;
}

int vmaf_vulkan_buffer_invalidate(VmafVulkanContext *ctx, VmafVulkanBuffer *buf)
{
    if (!ctx || !buf)
        return -EINVAL;
    /* VMA handles the coherency check: on HOST_COHERENT allocations
     * vmaInvalidateAllocation is a no-op (spec §10.2.1). On non-coherent
     * heaps it issues vkInvalidateMappedMemoryRanges with proper alignment
     * to nonCoherentAtomSize. ADR-0350: called after fence wait, before
     * reading GPU-written reduced accumulator buffers. */
    VkResult vkr = vmaInvalidateAllocation(ctx->allocator, buf->allocation, 0, VK_WHOLE_SIZE);
    return (vkr == VK_SUCCESS) ? 0 : -EIO;
}

void vmaf_vulkan_buffer_free(VmafVulkanContext *ctx, VmafVulkanBuffer *buf)
{
    if (!ctx || !buf)
        return;
    vmaDestroyBuffer(ctx->allocator, buf->vk_buffer, buf->allocation);
    free(buf);
}

/* Legacy void* shim used by the T5-1 smoke test — keep working. */
int vmaf_vulkan_picture_alloc(VmafVulkanContext *ctx, void **out, size_t size)
{
    if (!ctx || !out || size == 0)
        return -EINVAL;
    assert(g_shim_count <= VMAF_VK_SHIM_MAX);
    if (g_shim_count >= VMAF_VK_SHIM_MAX)
        return -ENOMEM;

    VmafVulkanBuffer *b = NULL;
    int err = vmaf_vulkan_buffer_alloc(ctx, &b, size);
    if (err)
        return err;
    assert(b != NULL);

    void *host = vmaf_vulkan_buffer_host(b);
    if (!host) {
        vmaf_vulkan_buffer_free(ctx, b);
        return -ENOMEM;
    }
    g_shim_entries[g_shim_count].host_ptr = host;
    g_shim_entries[g_shim_count].buf = b;
    g_shim_count++;
    *out = host;
    return 0;
}

void vmaf_vulkan_picture_free(VmafVulkanContext *ctx, void *host_ptr)
{
    if (!ctx || !host_ptr)
        return;
    for (size_t i = 0; i < g_shim_count; i++) {
        if (g_shim_entries[i].host_ptr != host_ptr)
            continue;
        vmaf_vulkan_buffer_free(ctx, g_shim_entries[i].buf);
        /* Compact in place — preserve insertion order isn't required. */
        g_shim_entries[i] = g_shim_entries[--g_shim_count];
        return;
    }
}
