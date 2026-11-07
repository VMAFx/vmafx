/**
 *  Copyright 2026 Lusoris and Claude (Anthropic)
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 *
 *  Vulkan per-feature kernel scaffolding template (T7-* — ADR-0246).
 *
 *  This header is **template-only**: no existing kernel includes it
 *  yet. It captures the shape every fork-added Vulkan feature
 *  extractor has converged on (single-pipeline + descriptor-pool +
 *  per-WG int64 partials + sync fence) and exposes it as a small
 *  set of helper inlines + plain structs, so future kernel
 *  migrations (and the long-tail GPU bring-up) can stop hand-rolling
 *  the same ~30 lines of pipeline-creation, descriptor-pool sizing,
 *  command-buffer alloc/submit, and fence-wait boilerplate.
 *
 *  The shared lifecycle every Vulkan feature kernel implements today
 *  (reference: libvmaf/src/feature/vulkan/psnr_vulkan.c):
 *
 *      init() / create_pipeline:
 *          vkCreateDescriptorSetLayout(ssbo bindings × N)
 *          vkCreatePipelineLayout(layout, push_constant_range)
 *          vkCreateShaderModule(spv blob)
 *          vkCreateComputePipelines(layout, shader, spec_constants)
 *          vkCreateDescriptorPool(N_sets × N_buffers × frames-in-flight)
 *
 *      init() / alloc_buffers:
 *          vmaf_vulkan_buffer_alloc(ref/dis input × n_planes)
 *          vmaf_vulkan_buffer_alloc(per-WG int64 partials × n_planes)
 *
 *      extract():
 *          memcpy ref/dis pixels → ref_in/dis_in host pointers
 *          memset(partials, 0, ...) + vmaf_vulkan_buffer_flush
 *          vkAllocateDescriptorSets per plane
 *          vkAllocateCommandBuffers (1)
 *          vkBeginCommandBuffer
 *            vkCmdBindPipeline / vkCmdBindDescriptorSets
 *            vkCmdPushConstants / vkCmdDispatch (per plane)
 *          vkEndCommandBuffer
 *          vkCreateFence + vkQueueSubmit + vkWaitForFences(UINT64_MAX)
 *          host-side reduction over partials
 *          → cleanup: vkDestroyFence / vkFreeCommandBuffers /
 *                     vkFreeDescriptorSets
 *
 *      close_fex():
 *          vkDeviceWaitIdle
 *          vkDestroyDescriptorPool / vkDestroyPipeline /
 *          vkDestroyShaderModule / vkDestroyPipelineLayout /
 *          vkDestroyDescriptorSetLayout
 *          vmaf_vulkan_buffer_free × every alloc
 *          (optional) vmaf_vulkan_context_destroy when owns_ctx
 *
 *  The helpers here own the pieces that DON'T differ per metric:
 *  the SSBO-binding descriptor-set layout, the pool sizing rule,
 *  the fenced-submit + wait pattern, and the close-time sweep.
 *  Per-metric work — push-constant struct, shader bytecode,
 *  spec-constants, dispatch grid math, host-side reduction —
 *  stays in the calling TU.
 *
 *  Reference implementation: libvmaf/src/feature/vulkan/psnr_vulkan.c.
 *  Migration guide: docs/backends/kernel-scaffolding.md.
 *
 *  Why per-backend (not cross-backend): see ADR-0246. CUDA and
 *  Vulkan don't share enough lifecycle shape to make a unified
 *  abstraction worth the added indirection.
 *
 *  Why helper functions (not macros): see ADR-0246. The Vulkan
 *  helpers genuinely benefit from RenderDoc / vkconfig stepping.
 *
 *  Why templates only (no migrations in this PR): each migration
 *  needs its own `places=4` cross-backend gate (ADR-0214). Landing
 *  the templates first lets every future migration PR be a small
 *  diff with a focused gate, instead of one mega-PR with N
 *  migrations and a single overloaded gate.
 */

#ifndef LIBVMAF_VULKAN_KERNEL_TEMPLATE_H_
#define LIBVMAF_VULKAN_KERNEL_TEMPLATE_H_

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include "picture_vulkan.h"
#include "vulkan_common.h"
#include "vulkan_internal.h" /* VmafVulkanContext layout for ctx->device etc. */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Bundle of the five long-lived pipeline objects every Vulkan
 * feature kernel creates in init(). Per-metric shader bytecode,
 * push-constant struct, spec-constant payload, and SSBO-binding
 * count are passed in via VmafVulkanKernelPipelineDesc — the
 * helper does the boilerplate vkCreate* calls and pool sizing.
 */
typedef struct VmafVulkanKernelPipeline {
    VkDescriptorSetLayout dsl;
    VkPipelineLayout pipeline_layout;
    VkShaderModule shader;
    VkPipeline pipeline;
    VkDescriptorPool desc_pool;
} VmafVulkanKernelPipeline;

/*
 * Per-frame submission scratch — one command buffer + fence
 * lifetime tied to a single extract() call.
 *
 * Two ownership modes (see ADR-0256 / T-GPU-OPT-VK-1):
 *
 *   1. Pool-owned (preferred for kernels migrated to the
 *      submit-side template): `cmd` and `fence` are *acquired* from
 *      a `VmafVulkanKernelSubmitPool` pre-allocated at init() time.
 *      The pool resets the fence + command buffer between frames
 *      instead of recreating them. Per-frame
 *      `vkAllocateCommandBuffers` / `vkFreeCommandBuffers` /
 *      `vkCreateFence` / `vkDestroyFence` overhead is eliminated.
 *
 *   2. Self-owned (legacy / one-off paths): the helper allocates
 *      a fresh fence + command buffer per call and the caller
 *      releases them via `vmaf_vulkan_kernel_submit_free`. This is
 *      what `vmaf_vulkan_kernel_submit_begin` did before pool
 *      support was added; preserved for paths that have not yet
 *      migrated.
 */
typedef struct VmafVulkanKernelSubmit {
    VkCommandBuffer cmd;
    VkFence fence;
    /* Non-NULL when this submit borrows from a pool (mode 1). NULL
     * when it owns its own resources (mode 2). */
    struct VmafVulkanKernelSubmitPool *pool;
    uint32_t pool_slot;
} VmafVulkanKernelSubmit;

/*
 * Pre-allocated fence + command-buffer pool that lives across
 * frames. Created once in init() via
 * `vmaf_vulkan_kernel_submit_pool_create`, drained + freed in
 * close_fex() via `vmaf_vulkan_kernel_submit_pool_destroy`.
 *
 * Slot count = ops-per-frame. Single-dispatch extractors
 * (psnr_hvs, vif, adm, float_vif, float_adm) need 1; multi-fence
 * extractors that want intra-frame readback (ms_ssim has 6 = 1
 * pyramid + 5 scales) declare more.
 *
 * Slots are cycled round-robin via an internal `next` cursor —
 * acquire returns the next slot, end_and_wait blocks until the
 * fence signals, then release marks the slot reusable. The pool
 * is single-threaded by design (every Vulkan kernel TU calls
 * `extract()` from a single feature-thread).
 *
 * Why no reusable command buffers across frames inside a single
 * extract(): the per-slot command buffer is reset (not freed) on
 * each acquire via `VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT`
 * — see vmaf_vulkan_kernel_submit_acquire below. The
 * `VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT` flag stays for
 * driver hint correctness; the saved cost is the
 * allocate / free pair, not the recording.
 */
#define VMAF_VULKAN_KERNEL_POOL_MAX_SLOTS 8u

typedef struct VmafVulkanKernelSubmitPool {
    VkCommandBuffer cmd[VMAF_VULKAN_KERNEL_POOL_MAX_SLOTS];
    VkFence fence[VMAF_VULKAN_KERNEL_POOL_MAX_SLOTS];
    uint32_t slot_count;
} VmafVulkanKernelSubmitPool;

/*
 * Pipeline-creation descriptor.
 *
 * `ssbo_binding_count` — number of VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
 *                        bindings in the descriptor set layout.
 *                        E.g. PSNR uses 3 (ref, dis, partials);
 *                        a metric with 4 SSBOs declares 4.
 * `push_constant_size` — bytes; passed to VkPushConstantRange.
 * `spv_bytes` / `spv_size` — raw SPIR-V blob from the generated
 *                            `<feature>_spv.h` header.
 * `pipeline_create_info` — caller fills in stage.module + spec
 *                          constants + layout; the helper supplies
 *                          sType / bindPoint defaults.
 * `max_descriptor_sets`  — frames-in-flight × n_planes-equivalent;
 *                          PSNR uses 12 (4 frames × 3 planes).
 *                          For a single-dispatch kernel use 4.
 */
typedef struct VmafVulkanKernelPipelineDesc {
    uint32_t ssbo_binding_count;
    uint32_t push_constant_size;
    const uint32_t *spv_bytes;
    size_t spv_size;
    /* Optional: set on the VkComputePipelineCreateInfo *before*
     * calling vmaf_vulkan_kernel_pipeline_create. The helper
     * fills .layout and .stage.module; the caller fills
     * .stage.pName, .stage.pSpecializationInfo. */
    VkComputePipelineCreateInfo pipeline_create_info;
    uint32_t max_descriptor_sets;
} VmafVulkanKernelPipelineDesc;

/*
 * Maximum number of SSBO bindings the template's
 * `vmaf_vulkan_kernel_pipeline_create` will accept in a single
 * descriptor-set layout. Current consumers top out at 10
 * (`ssimulacra2_vulkan.c`'s SSIM bundle uses 8); lift this if a
 * future kernel needs more. The conformant Vulkan minimum for
 * `maxDescriptorSetStorageBuffers` is 96, so values well above
 * the current cap remain portable across drivers.
 */
#define VMAF_VULKAN_KERNEL_MAX_SSBO_BINDINGS 16U

/*
 * Build the descriptor-set layout, pipeline layout, shader module,
 * compute pipeline, and descriptor pool.
 *
 * On failure the helper rolls back any partially-created object —
 * the output struct is zeroed out, so the caller's close_fex path
 * (which uses vmaf_vulkan_kernel_pipeline_destroy) is a safe
 * no-op. Returns 0 / -ENOMEM.
 *
 * Caveat: the sample implementation in psnr_vulkan.c uses 3 SSBO
 * bindings (ref, dis, partials). Metrics whose pipeline shape
 * needs uniforms or storage images don't fit this template — the
 * intention is *not* to cover every shape, but the long-tail of
 * SSBO-only reduction kernels (PSNR, motion, ssim, cambi, ...).
 */
static inline int vmaf_vulkan_kernel_pipeline_create(VmafVulkanContext *ctx,
                                                     const VmafVulkanKernelPipelineDesc *desc,
                                                     VmafVulkanKernelPipeline *out)
{
    if (ctx == NULL || desc == NULL || out == NULL) {
        return -EINVAL;
    }
    if (desc->ssbo_binding_count == 0U ||
        desc->ssbo_binding_count > VMAF_VULKAN_KERNEL_MAX_SSBO_BINDINGS) {
        return -EINVAL;
    }
    out->dsl = VK_NULL_HANDLE;
    out->pipeline_layout = VK_NULL_HANDLE;
    out->shader = VK_NULL_HANDLE;
    out->pipeline = VK_NULL_HANDLE;
    out->desc_pool = VK_NULL_HANDLE;

    /* 1. Descriptor set layout: N storage-buffer bindings. */
    VkDescriptorSetLayoutBinding bindings[VMAF_VULKAN_KERNEL_MAX_SSBO_BINDINGS] = {0};
    for (uint32_t i = 0; i < desc->ssbo_binding_count; i++) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dslci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = desc->ssbo_binding_count,
        .pBindings = bindings,
    };
    if (vkCreateDescriptorSetLayout(ctx->device, &dslci, NULL, &out->dsl) != VK_SUCCESS) {
        return -ENOMEM;
    }

    /* 2. Pipeline layout: 1 set + 1 push-constant range. */
    const VkPushConstantRange pcr = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0,
        .size = desc->push_constant_size,
    };
    VkPipelineLayoutCreateInfo plci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &out->dsl,
        .pushConstantRangeCount = (desc->push_constant_size > 0U) ? 1U : 0U,
        .pPushConstantRanges = (desc->push_constant_size > 0U) ? &pcr : NULL,
    };
    if (vkCreatePipelineLayout(ctx->device, &plci, NULL, &out->pipeline_layout) != VK_SUCCESS) {
        vkDestroyDescriptorSetLayout(ctx->device, out->dsl, NULL);
        out->dsl = VK_NULL_HANDLE;
        return -ENOMEM;
    }

    /* 3. Shader module from caller-supplied SPV blob. */
    VkShaderModuleCreateInfo smci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = desc->spv_size,
        .pCode = desc->spv_bytes,
    };
    if (vkCreateShaderModule(ctx->device, &smci, NULL, &out->shader) != VK_SUCCESS) {
        vkDestroyPipelineLayout(ctx->device, out->pipeline_layout, NULL);
        vkDestroyDescriptorSetLayout(ctx->device, out->dsl, NULL);
        out->pipeline_layout = VK_NULL_HANDLE;
        out->dsl = VK_NULL_HANDLE;
        return -ENOMEM;
    }

    /* 4. Compute pipeline — caller supplied stage settings + spec
     * constants; helper fills layout + module. */
    VkComputePipelineCreateInfo cpci = desc->pipeline_create_info;
    cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = out->shader;
    cpci.layout = out->pipeline_layout;
    if (vkCreateComputePipelines(ctx->device, VK_NULL_HANDLE, 1, &cpci, NULL, &out->pipeline) !=
        VK_SUCCESS) {
        vkDestroyShaderModule(ctx->device, out->shader, NULL);
        vkDestroyPipelineLayout(ctx->device, out->pipeline_layout, NULL);
        vkDestroyDescriptorSetLayout(ctx->device, out->dsl, NULL);
        out->shader = VK_NULL_HANDLE;
        out->pipeline_layout = VK_NULL_HANDLE;
        out->dsl = VK_NULL_HANDLE;
        return -ENOMEM;
    }

    /* 5. Descriptor pool sized for max_descriptor_sets sets ×
     * ssbo_binding_count buffers. PSNR's heuristic was 4 frames
     * in flight × n_planes — see psnr_vulkan.c create_pipeline()
     * for the back-of-the-envelope. */
    const uint32_t max_sets = (desc->max_descriptor_sets == 0U) ? 4U : desc->max_descriptor_sets;
    VkDescriptorPoolSize pool_size = {
        .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = max_sets * desc->ssbo_binding_count,
    };
    VkDescriptorPoolCreateInfo dpci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets = max_sets,
        .poolSizeCount = 1,
        .pPoolSizes = &pool_size,
    };
    if (vkCreateDescriptorPool(ctx->device, &dpci, NULL, &out->desc_pool) != VK_SUCCESS) {
        vkDestroyPipeline(ctx->device, out->pipeline, NULL);
        vkDestroyShaderModule(ctx->device, out->shader, NULL);
        vkDestroyPipelineLayout(ctx->device, out->pipeline_layout, NULL);
        vkDestroyDescriptorSetLayout(ctx->device, out->dsl, NULL);
        out->pipeline = VK_NULL_HANDLE;
        out->shader = VK_NULL_HANDLE;
        out->pipeline_layout = VK_NULL_HANDLE;
        out->dsl = VK_NULL_HANDLE;
        return -ENOMEM;
    }
    return 0;
}

/*
 * Pre-allocate a submit pool with `slot_count` reusable
 * command buffers + fences. Call this once in init().
 *
 * `slot_count` must be in [1, VMAF_VULKAN_KERNEL_POOL_MAX_SLOTS].
 * Pass 1 for single-dispatch extractors (most kernels), 6 for
 * `ms_ssim_vulkan` (1 pyramid + 5 scales). Beyond that, refactor
 * to fewer dispatches first — the limit is a sanity bound.
 *
 * On failure all partially-allocated handles are released; the
 * caller's close_fex path (vmaf_vulkan_kernel_submit_pool_destroy)
 * is a safe no-op on a zeroed pool. Returns 0 / -EINVAL / -ENOMEM.
 *
 * Implementation note: command buffers are *not* allocated with
 * VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT here — the
 * context-level command pool is created in vulkan_common.c with
 * that flag (see ADR-0256). vkResetCommandBuffer() on each acquire
 * recycles the buffer in-place.
 */
static inline int vmaf_vulkan_kernel_submit_pool_create(VmafVulkanContext *ctx, uint32_t slot_count,
                                                        VmafVulkanKernelSubmitPool *out)
{
    if (ctx == NULL || out == NULL) {
        return -EINVAL;
    }
    if (slot_count == 0U || slot_count > VMAF_VULKAN_KERNEL_POOL_MAX_SLOTS) {
        return -EINVAL;
    }
    for (uint32_t i = 0; i < VMAF_VULKAN_KERNEL_POOL_MAX_SLOTS; i++) {
        out->cmd[i] = VK_NULL_HANDLE;
        out->fence[i] = VK_NULL_HANDLE;
    }
    out->slot_count = slot_count;

    VkCommandBufferAllocateInfo cbai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = ctx->command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = slot_count,
    };
    if (vkAllocateCommandBuffers(ctx->device, &cbai, out->cmd) != VK_SUCCESS) {
        return -ENOMEM;
    }
    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    for (uint32_t i = 0; i < slot_count; i++) {
        if (vkCreateFence(ctx->device, &fci, NULL, &out->fence[i]) != VK_SUCCESS) {
            for (uint32_t j = 0; j < i; j++) {
                vkDestroyFence(ctx->device, out->fence[j], NULL);
                out->fence[j] = VK_NULL_HANDLE;
            }
            vkFreeCommandBuffers(ctx->device, ctx->command_pool, slot_count, out->cmd);
            for (uint32_t j = 0; j < slot_count; j++) {
                out->cmd[j] = VK_NULL_HANDLE;
            }
            return -ENOMEM;
        }
    }
    return 0;
}

/*
 * close_fex()-side: drain + destroy every per-slot fence + cmd
 * buffer. Safe on a partially-initialised pool. Caller must have
 * already vkDeviceWaitIdle (or this will block in vkWaitForFences
 * implicitly via vkFreeCommandBuffers spec semantics).
 */
static inline void vmaf_vulkan_kernel_submit_pool_destroy(VmafVulkanContext *ctx,
                                                          VmafVulkanKernelSubmitPool *pool)
{
    if (ctx == NULL || pool == NULL || ctx->device == VK_NULL_HANDLE) {
        return;
    }
    for (uint32_t i = 0; i < pool->slot_count; i++) {
        if (pool->fence[i] != VK_NULL_HANDLE) {
            vkDestroyFence(ctx->device, pool->fence[i], NULL);
            pool->fence[i] = VK_NULL_HANDLE;
        }
    }
    /* Free the cmd buffers in one batch — the context's command
     * pool was created with VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT
     * (see vulkan/common.c) so per-buffer free is supported. */
    for (uint32_t i = 0; i < pool->slot_count; i++) {
        if (pool->cmd[i] != VK_NULL_HANDLE) {
            vkFreeCommandBuffers(ctx->device, ctx->command_pool, 1, &pool->cmd[i]);
            pool->cmd[i] = VK_NULL_HANDLE;
        }
    }
    pool->slot_count = 0;
}

/*
 * Acquire slot `pool_slot` from the pool, reset its fence + cmd
 * buffer, begin recording. After this returns the caller emits
 * its `vkCmdBindPipeline` / `vkCmdDispatch` sequence and then calls
 * `vmaf_vulkan_kernel_submit_end_and_wait`.
 *
 * Caller is responsible for using each slot index at most once
 * per `extract()` call (or for explicitly reusing the same slot
 * after a prior `submit_end_and_wait`, which leaves the fence in
 * the signalled state ready to be reset on the next acquire).
 *
 * Returns 0 / -EINVAL / -EIO.
 */
static inline int vmaf_vulkan_kernel_submit_acquire(VmafVulkanContext *ctx,
                                                    VmafVulkanKernelSubmitPool *pool,
                                                    uint32_t pool_slot, VmafVulkanKernelSubmit *out)
{
    if (ctx == NULL || pool == NULL || out == NULL) {
        return -EINVAL;
    }
    if (pool_slot >= pool->slot_count) {
        return -EINVAL;
    }
    out->cmd = pool->cmd[pool_slot];
    out->fence = pool->fence[pool_slot];
    out->pool = pool;
    out->pool_slot = pool_slot;

    /* Reset the command buffer in place (requires
     * VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT on the pool).
     * Spec § 6.1: vkResetCommandBuffer is only valid when the cmd
     * buffer is not in the pending state — fence-wait on the prior
     * frame's submission ensures that. */
    if (vkResetCommandBuffer(out->cmd, 0) != VK_SUCCESS) {
        return -EIO;
    }
    VkCommandBufferBeginInfo cbbi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    if (vkBeginCommandBuffer(out->cmd, &cbbi) != VK_SUCCESS) {
        return -EIO;
    }
    /* Reset the fence to unsignalled before the next submit. */
    if (vkResetFences(ctx->device, 1, &out->fence) != VK_SUCCESS) {
        (void)vkEndCommandBuffer(out->cmd);
        return -EIO;
    }
    return 0;
}

/*
 * Self-owning submit_begin (legacy mode 2). Allocates a fresh
 * cmdbuf + fence per call. Use only for paths that have not yet
 * adopted a `VmafVulkanKernelSubmitPool`. Pool-aware kernels
 * should call `vmaf_vulkan_kernel_submit_acquire` instead.
 *
 * Returns 0 / -ENOMEM. The fence is created here too so submit_end
 * has nothing to allocate.
 */
static inline int vmaf_vulkan_kernel_submit_begin(VmafVulkanContext *ctx,
                                                  VmafVulkanKernelSubmit *out)
{
    if (ctx == NULL || out == NULL) {
        return -EINVAL;
    }
    out->cmd = VK_NULL_HANDLE;
    out->fence = VK_NULL_HANDLE;
    out->pool = NULL;
    out->pool_slot = 0;

    VkCommandBufferAllocateInfo cbai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = ctx->command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    if (vkAllocateCommandBuffers(ctx->device, &cbai, &out->cmd) != VK_SUCCESS) {
        return -ENOMEM;
    }
    VkCommandBufferBeginInfo cbbi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    if (vkBeginCommandBuffer(out->cmd, &cbbi) != VK_SUCCESS) {
        vkFreeCommandBuffers(ctx->device, ctx->command_pool, 1, &out->cmd);
        out->cmd = VK_NULL_HANDLE;
        return -ENOMEM;
    }

    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkCreateFence(ctx->device, &fci, NULL, &out->fence) != VK_SUCCESS) {
        /* The command buffer is mid-recording; vkEndCommandBuffer
         * is required before vkFreeCommandBuffers per spec. */
        (void)vkEndCommandBuffer(out->cmd);
        vkFreeCommandBuffers(ctx->device, ctx->command_pool, 1, &out->cmd);
        out->cmd = VK_NULL_HANDLE;
        return -ENOMEM;
    }
    return 0;
}

/*
 * End recording, submit on the context's queue, wait on the fence.
 * Synchronous submit — matches the existing kernels' behaviour.
 *
 * Works for both pool-owned and self-owned submits. Caller is
 * responsible for vmaf_vulkan_kernel_submit_free even on failure,
 * so this routine never half-frees.
 *
 * Returns 0 / -EIO.
 */
static inline int vmaf_vulkan_kernel_submit_end_and_wait(VmafVulkanContext *ctx,
                                                         VmafVulkanKernelSubmit *sub)
{
    if (vkEndCommandBuffer(sub->cmd) != VK_SUCCESS) {
        return -EIO;
    }
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &sub->cmd,
    };
    if (vkQueueSubmit(ctx->queue, 1, &si, sub->fence) != VK_SUCCESS) {
        return -EIO;
    }
    if (vkWaitForFences(ctx->device, 1, &sub->fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
        return -EIO;
    }
    return 0;
}

/*
 * Release per-frame submit scratch. For pool-borrowed submits
 * this is a near-no-op (just clears the local handles); the pool
 * keeps the underlying fence + cmd buffer alive across frames.
 * For self-owned submits this destroys the fence and frees the
 * cmd buffer, matching the pre-pool behaviour. Safe on a
 * partially-initialised struct (handles that are VK_NULL_HANDLE
 * are skipped).
 */
static inline void vmaf_vulkan_kernel_submit_free(VmafVulkanContext *ctx,
                                                  VmafVulkanKernelSubmit *sub)
{
    if (sub->pool != NULL) {
        /* Pool owns the resources; just clear the local handles
         * so a subsequent stray free is a safe no-op. */
        sub->cmd = VK_NULL_HANDLE;
        sub->fence = VK_NULL_HANDLE;
        sub->pool = NULL;
        sub->pool_slot = 0;
        return;
    }
    if (sub->fence != VK_NULL_HANDLE) {
        vkDestroyFence(ctx->device, sub->fence, NULL);
        sub->fence = VK_NULL_HANDLE;
    }
    if (sub->cmd != VK_NULL_HANDLE) {
        vkFreeCommandBuffers(ctx->device, ctx->command_pool, 1, &sub->cmd);
        sub->cmd = VK_NULL_HANDLE;
    }
}

/*
 * Pre-allocate `count` descriptor sets from a pool with a single
 * shared layout. Use this in init() once the pipeline + pool are
 * created; subsequent frames update the bindings via
 * `vkUpdateDescriptorSets` instead of re-allocating per frame
 * (T-GPU-OPT-VK-4 / ADR-0256).
 *
 * `count` is bounded by the pool's `maxSets` — passing more than
 * the pool was sized for returns -ENOMEM. The descriptor sets are
 * destroyed implicitly when the pool is destroyed
 * (`vmaf_vulkan_kernel_pipeline_destroy`), so callers do *not*
 * call `vkFreeDescriptorSets` on them at close time.
 *
 * Returns 0 / -EINVAL / -ENOMEM. On failure `out_sets` is left
 * with `VK_NULL_HANDLE` entries.
 */
static inline int vmaf_vulkan_kernel_descriptor_sets_alloc(VmafVulkanContext *ctx,
                                                           VkDescriptorPool pool,
                                                           VkDescriptorSetLayout layout,
                                                           uint32_t count,
                                                           VkDescriptorSet *out_sets)
{
    if (ctx == NULL || out_sets == NULL || pool == VK_NULL_HANDLE || layout == VK_NULL_HANDLE) {
        return -EINVAL;
    }
    if (count == 0U) {
        return -EINVAL;
    }
    /* 32 covers the largest in-tree caller (ssimulacra2's pyramid +
     * blur sets). Compile-time-bounded array keeps Power-of-10 §3
     * happy. */
    enum { MAX_PRE_ALLOC_SETS = 32 };
    if (count > (uint32_t)MAX_PRE_ALLOC_SETS) {
        return -EINVAL;
    }
    VkDescriptorSetLayout layouts[MAX_PRE_ALLOC_SETS];
    for (uint32_t i = 0; i < count; i++) {
        layouts[i] = layout;
        out_sets[i] = VK_NULL_HANDLE;
    }
    VkDescriptorSetAllocateInfo dsai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = pool,
        .descriptorSetCount = count,
        .pSetLayouts = layouts,
    };
    if (vkAllocateDescriptorSets(ctx->device, &dsai, out_sets) != VK_SUCCESS) {
        for (uint32_t i = 0; i < count; i++) {
            out_sets[i] = VK_NULL_HANDLE;
        }
        return -ENOMEM;
    }
    return 0;
}

/*
 * Create an additional `VkPipeline` that **shares** the existing
 * pipeline's layout + shader module + descriptor-set layout +
 * descriptor pool. The base `VmafVulkanKernelPipeline *pl` was
 * already initialised via `vmaf_vulkan_kernel_pipeline_create`;
 * this helper builds a sibling pipeline that differs only in
 * caller-supplied spec-constants (`pipeline_create_info.stage.
 * pSpecializationInfo`).
 *
 * Use case: kernels that ship two pipelines differing in a single
 * spec-constant — `motion_vulkan.c` (compute-SAD on/off for
 * first frame vs subsequent), `ssim_vulkan.c` (horizontal vs
 * vertical pass). The variant pipeline is the caller's
 * responsibility to destroy *before* calling
 * `vmaf_vulkan_kernel_pipeline_destroy()` (which destroys the
 * shared layout + shader and would invalidate the variant).
 *
 * Caveat: only the spec-constant payload + pName are taken from
 * `pipeline_create_info`. The helper overrides `.layout` and
 * `.stage.module` with the base's values; the rest of the
 * `VkComputePipelineCreateInfo` defaults to zeroed sType-injected
 * values. Returns 0 / -EINVAL / -ENOMEM.
 */
static inline int vmaf_vulkan_kernel_pipeline_add_variant(
    VmafVulkanContext *ctx, const VmafVulkanKernelPipeline *base,
    const VkComputePipelineCreateInfo *variant_info, VkPipeline *out_pipeline)
{
    if (ctx == NULL || base == NULL || variant_info == NULL || out_pipeline == NULL) {
        return -EINVAL;
    }
    if (base->pipeline_layout == VK_NULL_HANDLE || base->shader == VK_NULL_HANDLE) {
        return -EINVAL;
    }
    VkComputePipelineCreateInfo cpci = *variant_info;
    cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = base->shader;
    cpci.layout = base->pipeline_layout;
    *out_pipeline = VK_NULL_HANDLE;
    if (vkCreateComputePipelines(ctx->device, VK_NULL_HANDLE, 1, &cpci, NULL, out_pipeline) !=
        VK_SUCCESS) {
        return -ENOMEM;
    }
    return 0;
}

/*
 * close_fex()-side sweep: vkDeviceWaitIdle, then destroy the five
 * long-lived pipeline objects in reverse-creation order. Safe to
 * call on a partially-created pipeline.
 */
static inline void vmaf_vulkan_kernel_pipeline_destroy(VmafVulkanContext *ctx,
                                                       VmafVulkanKernelPipeline *pl)
{
    if (ctx == NULL || ctx->device == VK_NULL_HANDLE) {
        return;
    }
    vkDeviceWaitIdle(ctx->device);
    if (pl->desc_pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(ctx->device, pl->desc_pool, NULL);
        pl->desc_pool = VK_NULL_HANDLE;
    }
    if (pl->pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(ctx->device, pl->pipeline, NULL);
        pl->pipeline = VK_NULL_HANDLE;
    }
    if (pl->shader != VK_NULL_HANDLE) {
        vkDestroyShaderModule(ctx->device, pl->shader, NULL);
        pl->shader = VK_NULL_HANDLE;
    }
    if (pl->pipeline_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(ctx->device, pl->pipeline_layout, NULL);
        pl->pipeline_layout = VK_NULL_HANDLE;
    }
    if (pl->dsl != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(ctx->device, pl->dsl, NULL);
        pl->dsl = VK_NULL_HANDLE;
    }
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LIBVMAF_VULKAN_KERNEL_TEMPLATE_H_ */
