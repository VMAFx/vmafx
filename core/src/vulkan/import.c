/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 *
 *  T7-29 part 2 — VkImage zero-copy import (ADR-0186).
 *  T7-29 part 4 — v2 async pending-fence ring (ADR-0251).
 *
 *  Implements the libvmaf_vulkan.h "import" surface that PR #128
 *  scaffolded as -ENOSYS. Caller hands us an externally-decoded
 *  VkImage (e.g. AVVkFrame from FFmpeg's Vulkan hwframes); we wait
 *  the caller's timeline semaphore on the GPU, transition the
 *  image to TRANSFER_SRC_OPTIMAL, vkCmdCopyImageToBuffer the Y
 *  plane into a per-slot staging VkBuffer (HOST_VISIBLE +
 *  COHERENT), and signal completion via a per-slot fence.
 *
 *  The staging buffers are allocated to match the DATA_ALIGN-rounded
 *  stride that vmaf_picture_alloc would produce, so
 *  vmaf_vulkan_read_imported_pictures can hand them straight to
 *  vmaf_read_pictures without an extra host memcpy.
 *
 *  Synchronisation model (v2 — ADR-0251): import_image submits
 *  to slot `frame_index % ring_size` and returns immediately; if
 *  that slot is still in flight from a prior frame the call waits
 *  the prior fence first (ring back-pressure). wait_compute drains
 *  every outstanding fence in submission order so the host can
 *  read the staging mappings safely. state_build_pictures waits
 *  the per-slot fence for the requested index before exposing
 *  the host pointer to vmaf_read_pictures.
 */

#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "libvmaf/libvmaf_vulkan.h"
#include "../picture.h" /* internal: VmafPicturePrivate, vmaf_picture_priv_init */
#include "../ref.h"     /* internal: VmafRef, vmaf_ref_init */
#include "import_picture.h"
#include "vulkan_internal.h"

static int noop_release_picture(VmafPicture *pic, void *cookie)
{
    (void)pic;
    (void)cookie;
    /* The pixel buffer is owned by the VmafVulkanState's import
     * slot, not by the picture pool. The state's
     * vmaf_vulkan_state_free() (or import_slots_free on
     * geometry change) is what actually releases it. This
     * callback exists so vmaf_picture_unref completes cleanly
     * after vmaf_read_pictures finishes consuming the picture. */
    return 0;
}

/* Mirrors libvmaf/src/picture.c's DATA_ALIGN. Kept local so this
 * file doesn't need to pull in the private "../picture.h" header
 * just for one constant. The two values must stay in sync — if
 * picture.c bumps DATA_ALIGN, this needs to follow. Asserted by
 * test_vulkan_import (compile-time check is awkward across a
 * directory boundary, so the test does a runtime assert). */
#define VMAF_VK_IMPORT_DATA_ALIGN 64u

static size_t aligned_stride_bytes(unsigned w, unsigned bpc)
{
    const unsigned bpp = (bpc > 8u) ? 2u : 1u;
    const unsigned aw = (w + (VMAF_VK_IMPORT_DATA_ALIGN - 1u)) & ~(VMAF_VK_IMPORT_DATA_ALIGN - 1u);
    return (size_t)aw * bpp;
}

/* Drain a single slot's fence if it carries an outstanding
 * submission. Required before re-recording the slot's command
 * buffer (Vulkan spec: command buffer must not be in the
 * pending state). Also called by wait_compute and the teardown
 * path. */
static int drain_slot_fence(struct VmafVulkanState *state, struct VmafVulkanImportSlot *slot)
{
    if (!slot->fence_in_flight)
        return 0;
    VmafVulkanContext *ctx = state->ctx;
    if (vkWaitForFences(ctx->device, 1, &slot->fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS)
        return -EIO;
    if (vkResetFences(ctx->device, 1, &slot->fence) != VK_SUCCESS)
        return -EIO;
    slot->fence_in_flight = 0;
    return 0;
}

static void slot_release(struct VmafVulkanState *state, struct VmafVulkanImportSlot *slot)
{
    VmafVulkanContext *ctx = state->ctx;
    assert(ctx != NULL);
    assert(ctx->device != VK_NULL_HANDLE);
    if (slot->fence != VK_NULL_HANDLE) {
        vkDestroyFence(ctx->device, slot->fence, NULL);
        slot->fence = VK_NULL_HANDLE;
    }
    if (slot->cmd != VK_NULL_HANDLE) {
        assert(ctx->command_pool != VK_NULL_HANDLE);
        vkFreeCommandBuffers(ctx->device, ctx->command_pool, 1, &slot->cmd);
        slot->cmd = VK_NULL_HANDLE;
    }
    if (slot->ref_buf) {
        vmaf_vulkan_buffer_free(ctx, slot->ref_buf);
        slot->ref_buf = NULL;
    }
    if (slot->dis_buf) {
        vmaf_vulkan_buffer_free(ctx, slot->dis_buf);
        slot->dis_buf = NULL;
    }
    slot->ref_pending = slot->dis_pending = 0;
    slot->ref_index = slot->dis_index = 0u;
    slot->fence_in_flight = 0;
}

static int slot_alloc(struct VmafVulkanState *state, struct VmafVulkanImportSlot *slot, size_t size)
{
    VmafVulkanContext *ctx = state->ctx;

    int err = vmaf_vulkan_buffer_alloc(ctx, &slot->ref_buf, size);
    if (err)
        return err;
    err = vmaf_vulkan_buffer_alloc(ctx, &slot->dis_buf, size);
    if (err)
        goto fail;

    VkCommandBufferAllocateInfo cbai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = ctx->command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    if (vkAllocateCommandBuffers(ctx->device, &cbai, &slot->cmd) != VK_SUCCESS) {
        err = -ENOMEM;
        goto fail;
    }

    VkFenceCreateInfo fci = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
    };
    if (vkCreateFence(ctx->device, &fci, NULL, &slot->fence) != VK_SUCCESS) {
        err = -ENOMEM;
        goto fail;
    }
    return 0;

fail:
    slot_release(state, slot);
    return err;
}

static int lazy_alloc_ring(struct VmafVulkanState *state, unsigned w, unsigned h, unsigned bpc)
{
    struct VmafVulkanImportSlots *s = &state->import;
    if (s->w != 0u) {
        /* Already allocated — caller must keep the geometry stable. */
        if (s->w != w || s->h != h || s->bpc != bpc)
            return -EINVAL;
        return 0;
    }
    if (w == 0u || h == 0u)
        return -EINVAL;
    if (bpc != 8u && bpc != 10u && bpc != 12u && bpc != 16u)
        return -EINVAL;

    const size_t stride = aligned_stride_bytes(w, bpc);
    const size_t size = stride * (size_t)h;
    const unsigned ring_size = vmaf_vulkan_clamp_ring_size(state->requested_ring_size);

    for (unsigned i = 0u; i < ring_size; i++) {
        int err = slot_alloc(state, &s->ring[i], size);
        if (err) {
            for (unsigned j = 0u; j < i; j++)
                slot_release(state, &s->ring[j]);
            return err;
        }
    }

    s->w = w;
    s->h = h;
    s->bpc = bpc;
    s->stride_bytes = stride;
    s->ring_size = ring_size;
    return 0;
}

void vmaf_vulkan_import_slots_free(struct VmafVulkanState *state)
{
    if (!state)
        return;
    struct VmafVulkanImportSlots *s = &state->import;
    VmafVulkanContext *ctx = state->ctx;
    assert(ctx != NULL);
    assert(ctx->device != VK_NULL_HANDLE);

    /* Drain every outstanding fence before destroying anything —
     * vkDestroyFence on a fence still owned by a queue submission
     * is undefined behaviour. ADR-0251 invariant. */
    for (unsigned i = 0u; i < s->ring_size; i++) {
        if (s->ring[i].fence_in_flight) {
            (void)drain_slot_fence(state, &s->ring[i]);
        }
    }
    /* Belt-and-braces: idle the queue so any non-tracked work
     * (e.g. a feature kernel that ran on the same queue) drains
     * before we release the staging buffers. The non-zero
     * VmafVulkanContext path may have submitted work outside the
     * ring; vkQueueWaitIdle is the cheapest universal barrier. */
    if (ctx->queue != VK_NULL_HANDLE)
        (void)vkQueueWaitIdle(ctx->queue);

    for (unsigned i = 0u; i < s->ring_size; i++)
        slot_release(state, &s->ring[i]);
    s->w = s->h = s->bpc = 0u;
    s->stride_bytes = 0u;
    s->ring_size = 0u;
}

static int record_image_to_buffer_copy(struct VmafVulkanState *state,
                                       struct VmafVulkanImportSlot *slot, VkImage image,
                                       VkImageLayout current_layout, VkBuffer dst, unsigned w,
                                       unsigned h)
{
    struct VmafVulkanImportSlots *s = &state->import;
    const uint32_t aligned_w = (uint32_t)(s->stride_bytes / ((s->bpc > 8u) ? 2u : 1u));

    if (vkResetCommandBuffer(slot->cmd, 0) != VK_SUCCESS)
        return -EIO;

    VkCommandBufferBeginInfo cbbi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    if (vkBeginCommandBuffer(slot->cmd, &cbbi) != VK_SUCCESS)
        return -EIO;

    /* Layout transition into TRANSFER_SRC_OPTIMAL. We do not
     * transition back — caller-owned images are effectively
     * "read-once" between decoder signals, matching how AVVkFrame
     * is consumed. If a caller needs the original layout
     * preserved, they can re-transition on their side. */
    VkImageMemoryBarrier to_src = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = current_layout,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    vkCmdPipelineBarrier(slot->cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &to_src);

    VkBufferImageCopy region = {
        .bufferOffset = 0,
        .bufferRowLength = aligned_w,
        .bufferImageHeight = h,
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageOffset = {0, 0, 0},
        .imageExtent = {w, h, 1},
    };
    vkCmdCopyImageToBuffer(slot->cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, 1, &region);

    if (vkEndCommandBuffer(slot->cmd) != VK_SUCCESS)
        return -EIO;
    return 0;
}

/* Submit the slot's command buffer and capture the fence for a
 * later wait. Unlike v1's submit_and_wait, this returns as soon
 * as vkQueueSubmit accepts the work — the fence is drained later
 * by drain_slot_fence (called from build_pictures, wait_compute,
 * or the next ring-wrap that lands on the same slot). */
static int submit_to_slot(struct VmafVulkanState *state, struct VmafVulkanImportSlot *slot,
                          VkSemaphore wait_sem, uint64_t wait_value)
{
    VmafVulkanContext *ctx = state->ctx;

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkTimelineSemaphoreSubmitInfo tsi = {
        .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        .waitSemaphoreValueCount = 1,
        .pWaitSemaphoreValues = &wait_value,
    };
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &slot->cmd,
    };
    if (wait_sem != VK_NULL_HANDLE) {
        si.pNext = &tsi;
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &wait_sem;
        si.pWaitDstStageMask = &wait_stage;
    }

    if (vkQueueSubmit(ctx->queue, 1, &si, slot->fence) != VK_SUCCESS)
        return -EIO;
    slot->fence_in_flight = 1;
    return 0;
}

int vmaf_vulkan_import_image(VmafVulkanState *state, uintptr_t vk_image, uint32_t vk_format,
                             uint32_t vk_layout, uintptr_t vk_semaphore,
                             uint64_t vk_semaphore_value, unsigned w, unsigned h, unsigned bpc,
                             int is_ref, unsigned index)
{
    (void)vk_format; /* For v1, we trust the caller's bpc. Future
                      * versions may use vk_format to disambiguate
                      * planar layouts. */
    if (!state || !state->ctx)
        return -EINVAL;
    if (vk_image == 0u)
        return -EINVAL;

    int err = lazy_alloc_ring(state, w, h, bpc);
    if (err)
        return err;

    struct VmafVulkanImportSlots *s = &state->import;
    const unsigned slot_idx = index % s->ring_size;
    struct VmafVulkanImportSlot *slot = &s->ring[slot_idx];

    /* Ring back-pressure: if the slot still holds an in-flight
     * submission from a previous frame (frame index `index -
     * ring_size`), wait that fence before re-recording. This is
     * exactly the "frames-in-flight = ring_size" stall — it
     * shouldn't happen if the consumer drains in order, but it's
     * the spec-required guard against re-recording a command
     * buffer that's still pending. ADR-0251 invariant. */
    err = drain_slot_fence(state, slot);
    if (err)
        return err;

    VmafVulkanBuffer *dst = is_ref ? slot->ref_buf : slot->dis_buf;
    err = record_image_to_buffer_copy(state, slot, (VkImage)vk_image, (VkImageLayout)vk_layout,
                                      (VkBuffer)vmaf_vulkan_buffer_vkhandle(dst), w, h);
    if (err)
        return err;

    err = submit_to_slot(state, slot, (VkSemaphore)vk_semaphore, vk_semaphore_value);
    if (err)
        return err;

    if (is_ref) {
        slot->ref_pending = 1;
        slot->ref_index = index;
    } else {
        slot->dis_pending = 1;
        slot->dis_index = index;
    }
    return 0;
}

int vmaf_vulkan_wait_compute(VmafVulkanState *state)
{
    if (!state || !state->ctx)
        return -EINVAL;
    /* v2 (ADR-0251): drain every outstanding ring fence so the
     * caller can read the staging mappings or reuse the imported
     * VkImages. v1's no-op contract is preserved when no slot
     * has work in flight. */
    struct VmafVulkanImportSlots *s = &state->import;
    for (unsigned i = 0u; i < s->ring_size; i++) {
        int err = drain_slot_fence(state, &s->ring[i]);
        if (err)
            return err;
    }
    return 0;
}

static int build_one_picture(VmafPicture *out, VmafVulkanBuffer *src_buf, unsigned w, unsigned h,
                             unsigned bpc, size_t stride_bytes)
{
    memset(out, 0, sizeof(*out));
    out->pix_fmt = VMAF_PIX_FMT_YUV400P;
    out->bpc = bpc;
    out->w[0] = w;
    out->h[0] = h;
    out->stride[0] = (ptrdiff_t)stride_bytes;
    out->data[0] = vmaf_vulkan_buffer_host(src_buf);
    if (!out->data[0])
        return -EIO;

    int err = vmaf_picture_priv_init(out);
    if (err)
        return err;
    err = vmaf_picture_set_release_callback(out, NULL, noop_release_picture);
    if (err) {
        free(out->priv);
        out->priv = NULL;
        return err;
    }
    err = vmaf_ref_init(&out->ref);
    if (err) {
        free(out->priv);
        out->priv = NULL;
        return err;
    }
    return 0;
}

int vmaf_vulkan_state_build_pictures(VmafVulkanState *state, unsigned index, VmafPicture *out_ref,
                                     VmafPicture *out_dis)
{
    if (!state || !out_ref || !out_dis)
        return -EINVAL;
    struct VmafVulkanImportSlots *s = &state->import;
    if (s->w == 0u || s->ring_size == 0u)
        return -EINVAL;

    const unsigned slot_idx = index % s->ring_size;
    struct VmafVulkanImportSlot *slot = &s->ring[slot_idx];

    if (!slot->ref_pending || !slot->dis_pending)
        return -EINVAL;
    if (slot->ref_index != index || slot->dis_index != index)
        return -EINVAL;

    /* v2 (ADR-0251): the slot's fence is the natural drain point
     * for "host can now read the staging buffers". Wait it
     * before exposing the host pointers to vmaf_read_pictures —
     * if the caller already drained via vmaf_vulkan_wait_compute,
     * fence_in_flight is 0 and this is a no-op. */
    int err = drain_slot_fence(state, slot);
    if (err)
        return err;

    /* HOST_VISIBLE | (potentially) NON_COHERENT memory needs an
     * explicit invalidate before the host reads back the GPU's
     * write. VMA's flush helper covers both directions on
     * coherent and non-coherent heaps. The flush has to come
     * AFTER the fence wait so the GPU writes are actually
     * visible to the device-cache invalidate. */
    err = vmaf_vulkan_buffer_flush(state->ctx, slot->ref_buf);
    if (err)
        return err;
    err = vmaf_vulkan_buffer_flush(state->ctx, slot->dis_buf);
    if (err)
        return err;

    err = build_one_picture(out_ref, slot->ref_buf, s->w, s->h, s->bpc, s->stride_bytes);
    if (err)
        return err;
    err = build_one_picture(out_dis, slot->dis_buf, s->w, s->h, s->bpc, s->stride_bytes);
    if (err) {
        (void)vmaf_picture_unref(out_ref);
        return err;
    }

    slot->ref_pending = 0;
    slot->dis_pending = 0;
    return 0;
}
