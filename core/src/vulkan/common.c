/**
 *  Copyright 2026 Lusoris and Claude (Anthropic)
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 *
 *  Vulkan runtime core — volk + Vulkan 1.3 + VMA bring-up for the
 *  opt-in, integer-first compute path (ADR-3402, partial revival of
 *  the backend removed by ADR-0726).
 *
 *  Trimmed from the pre-ADR-0726 runtime (snapshot 816a5f33f): the
 *  public state-level API (VmafVulkanState, the FFmpeg external-handle
 *  adoption path) and the VkImage import surface are NOT restored —
 *  ADR-0726's public-API removal stands and the reserved
 *  `VMAF_PICTURE_BUFFER_TYPE_VULKAN_DEVICE` /
 *  `VMAF_FEATURE_EXTRACTOR_VULKAN` enum slots stay untouched pending
 *  the ABI-bump ADR. Frame transport is host upload/download only
 *  (#3302 Phase 1); zero-copy import is a later optimization.
 *
 *  Device hygiene per ADR-3402 (from the recorded failure classes):
 *    - `vkCreateDevice` requests ONLY features the device reports. The
 *      Intel iGPU `0x8086:0xa78b` driver SIGSEGVs inside
 *      `vkCreateDevice` when asked for an unreported feature (2/2 runs,
 *      #2466 comment 6689054122). The old blanket
 *      `shaderBufferInt64Atomics` enable-chain is gone with the
 *      two-level reduction shaders it served (ADR-0350): the in-tree
 *      integer ADM folds its stage-3 reduction into the main shader as
 *      a shared-memory serial sum, so no buffer int64 atomics exist on
 *      this path.
 *    - The selected device is always reported by `vendorID`/`deviceID`
 *      in the init log (research-0090's inverted-attribution lesson:
 *      ordinals lie across drivers; identities do not). Identity-based
 *      selection lives in the probe (`feature/vulkan/probe/`).
 */

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vulkan_common.h"
#include "vulkan_internal.h"

// VK_API_VERSION_1_4 was added in Vulkan Headers 1.3.280 (Jan 2024).
// Ubuntu 22.04 ships libvulkan-dev 1.3.204, which predates this constant.
// Fall back to VK_API_VERSION_1_3 on older SDK headers; the runtime device
// selection path queries the actual device capabilities separately.
// Per ADR-0264 the 1.4 bump is gated on NVIDIA driver regression resolution;
// 1.3 is the active fallback path anyway.
#ifndef VK_API_VERSION_1_4
#define VK_API_VERSION_1_4 VK_API_VERSION_1_3
#endif

#define VK_OR_FAIL(call_, errno_)                                                                  \
    do {                                                                                           \
        VkResult _vkr = (call_);                                                                   \
        if (_vkr != VK_SUCCESS) {                                                                  \
            err = (errno_);                                                                        \
            goto fail;                                                                             \
        }                                                                                          \
    } while (0)

static int g_volk_loaded = 0;

static int load_volk_once(void)
{
    if (g_volk_loaded)
        return 0;
    VkResult vkr = volkInitialize();
    if (vkr != VK_SUCCESS)
        return -ENOSYS;
    g_volk_loaded = 1;
    return 0;
}

static int create_instance(VkInstance *out_instance)
{
    VkApplicationInfo app_info = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "libvmaf",
        .applicationVersion = VK_MAKE_API_VERSION(0, 3, 0, 0),
        .pEngineName = "libvmaf-vulkan",
        .engineVersion = VK_MAKE_API_VERSION(0, 3, 0, 0),
        .apiVersion = VK_API_VERSION_1_4,
    };
    VkInstanceCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app_info,
    };
    VkResult vkr = vkCreateInstance(&create_info, NULL, out_instance);
    if (vkr != VK_SUCCESS)
        return -ENODEV;
    volkLoadInstanceOnly(*out_instance);
    return 0;
}

static int devtype_score(VkPhysicalDeviceType type)
{
    switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
        return 4;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
        return 3;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
        return 2;
    case VK_PHYSICAL_DEVICE_TYPE_CPU:
        return 1;
    default:
        return 0;
    }
}

static uint32_t pick_compute_queue_family(VkPhysicalDevice phys)
{
    uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &family_count, NULL);
    if (family_count == 0)
        return UINT32_MAX;

    VkQueueFamilyProperties *families = calloc(family_count, sizeof(*families));
    if (!families)
        return UINT32_MAX;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &family_count, families);

    uint32_t async_compute = UINT32_MAX;
    uint32_t any_compute = UINT32_MAX;
    for (uint32_t i = 0; i < family_count; i++) {
        VkQueueFlags flags = families[i].queueFlags;
        if (!(flags & VK_QUEUE_COMPUTE_BIT))
            continue;
        if (any_compute == UINT32_MAX)
            any_compute = i;
        if (!(flags & VK_QUEUE_GRAPHICS_BIT) && async_compute == UINT32_MAX)
            async_compute = i;
    }
    free(families);

    return (async_compute != UINT32_MAX) ? async_compute : any_compute;
}

static int enumerate_compute_devices(VkInstance instance, VkPhysicalDevice *out, uint32_t *count_io)
{
    uint32_t total = 0;
    VkResult vkr = vkEnumeratePhysicalDevices(instance, &total, NULL);
    if (vkr != VK_SUCCESS)
        return -EIO;
    if (total == 0) {
        *count_io = 0;
        return 0;
    }

    VkPhysicalDevice *all = calloc(total, sizeof(*all));
    if (!all)
        return -ENOMEM;
    vkr = vkEnumeratePhysicalDevices(instance, &total, all);
    if (vkr != VK_SUCCESS) {
        free(all);
        return -EIO;
    }

    int *score = calloc(total, sizeof(*score));
    if (!score) {
        free(all);
        return -ENOMEM;
    }
    uint32_t kept = 0;
    for (uint32_t i = 0; i < total; i++) {
        if (pick_compute_queue_family(all[i]) == UINT32_MAX)
            continue;
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(all[i], &props);
        all[kept] = all[i];
        score[kept] = devtype_score(props.deviceType);
        kept++;
    }

    /* Insertion sort (kept is small in practice). */
    for (uint32_t i = 1; i < kept; i++) {
        VkPhysicalDevice ph = all[i];
        int sc = score[i];
        uint32_t j = i;
        while (j > 0 && score[j - 1] < sc) {
            all[j] = all[j - 1];
            score[j] = score[j - 1];
            j--;
        }
        all[j] = ph;
        score[j] = sc;
    }

    uint32_t to_copy = (kept < *count_io) ? kept : *count_io;
    for (uint32_t i = 0; i < to_copy; i++)
        out[i] = all[i];
    *count_io = kept;
    free(score);
    free(all);
    return 0;
}

int vmaf_vulkan_device_count(void)
{
    int err = load_volk_once();
    if (err)
        return 0;

    VkInstance instance = VK_NULL_HANDLE;
    err = create_instance(&instance);
    if (err)
        return 0;

    VkPhysicalDevice tmp[8];
    uint32_t count = 8;
    err = enumerate_compute_devices(instance, tmp, &count);
    vkDestroyInstance(instance, NULL);
    if (err)
        return err;
    return (int)count;
}

int vmaf_vulkan_context_new(VmafVulkanContext **out, int device_index)
{
    /* Back-compat wrapper: no fp64 strictness. The in-tree integer ADM
     * evaluates the CPU's angle predicate through the fp64-free
     * `adm_angle_flag_i64()` reformulation (research-2030), so no shipped
     * kernel needs `shaderFloat64` (ADR-3402). The `require_fp64` opt-in
     * below stays for a future fp kernel that carries its own parity gate. */
    return vmaf_vulkan_context_new_with_opts(out, device_index, /*require_fp64=*/0);
}

int vmaf_vulkan_context_new_with_opts(VmafVulkanContext **out, int device_index, int require_fp64)
{
    if (!out)
        return -EINVAL;

    int err = load_volk_once();
    if (err)
        return err;
    assert(g_volk_loaded == 1);

    VmafVulkanContext *ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return -ENOMEM;
    ctx->volk_loaded = 1;
    ctx->owns_handles = 1;
    ctx->device_index = device_index;
    assert(ctx->instance == VK_NULL_HANDLE);
    assert(ctx->device == VK_NULL_HANDLE);

    err = create_instance(&ctx->instance);
    if (err)
        goto fail_alloc;

    VkPhysicalDevice phys[16];
    uint32_t total = 16;
    err = enumerate_compute_devices(ctx->instance, phys, &total);
    if (err)
        goto fail;
    if (total == 0) {
        err = -ENODEV;
        goto fail;
    }
    if (device_index < 0) {
        ctx->device_index = 0;
    } else if ((uint32_t)device_index >= total) {
        err = -EINVAL;
        goto fail;
    } else {
        ctx->device_index = device_index;
    }
    ctx->physical_device = phys[ctx->device_index];

    vkGetPhysicalDeviceProperties(ctx->physical_device, &ctx->props);
    vkGetPhysicalDeviceMemoryProperties(ctx->physical_device, &ctx->mem_props);

    ctx->queue_family_index = pick_compute_queue_family(ctx->physical_device);
    if (ctx->queue_family_index == UINT32_MAX) {
        err = -ENODEV;
        goto fail;
    }

    /* Probe the device-reported features. ADR-3402: the opt-in integer
     * path needs exactly one feature, `shaderInt64` (per-WG int64
     * accumulators and the fp64-free `adm_angle_flag_i64()` angle
     * predicate, research-2030). `shaderFloat64` is never requested — the
     * ADR-3402 rule is that no in-tree shader may require fp64, and the
     * Intel iGPU `0x8086:0xa78b` (shaderFloat64=0) must stay serviceable.
     * No other feature and no Vulkan-12 struct is ever requested — the
     * Intel driver SIGSEGVs inside `vkCreateDevice` when asked for an
     * unreported feature (#2466 comment 6689054122), so this code requests
     * only flags the query above answered VK_TRUE. */
    VkPhysicalDeviceFeatures avail_feats = {0};
    vkGetPhysicalDeviceFeatures(ctx->physical_device, &avail_feats);
    ctx->has_float64 = avail_feats.shaderFloat64 ? 1 : 0;

    if (!avail_feats.shaderInt64) {
        fprintf(stderr,
                "libvmaf: Vulkan integer path unavailable on \"%s\" "
                "(vendorID 0x%04x deviceID 0x%04x) — shaderInt64 not "
                "reported (ADR-3402). Feature extractors answer -ENOTSUP "
                "on this device class.\n",
                ctx->props.deviceName, ctx->props.vendorID, ctx->props.deviceID);
        err = -ENOTSUP;
        goto fail;
    }
    if (!ctx->has_float64 && require_fp64) {
        fprintf(stderr,
                "libvmaf: Vulkan integer path unavailable on \"%s\" "
                "(vendorID 0x%04x deviceID 0x%04x) — shaderFloat64 not "
                "reported and `require_fp64` was set. The in-tree integer "
                "kernels are fp64-free (ADR-3402); this refusal exists for "
                "a future fp kernel that carries its own parity gate.\n",
                ctx->props.deviceName, ctx->props.vendorID, ctx->props.deviceID);
        err = -ENOTSUP;
        goto fail;
    }

    float queue_priority = 1.0f;
    VkDeviceQueueCreateInfo queue_create = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = ctx->queue_family_index,
        .queueCount = 1,
        .pQueuePriorities = &queue_priority,
    };
    /* Request exactly one feature, verified reported above (ADR-3402:
     * no in-tree shader requires shaderFloat64). */
    VkPhysicalDeviceFeatures features = {
        .shaderInt64 = VK_TRUE,
    };
    VkDeviceCreateInfo device_create = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_create,
        .pEnabledFeatures = &features,
    };
    VK_OR_FAIL(vkCreateDevice(ctx->physical_device, &device_create, NULL, &ctx->device), -ENODEV);
    volkLoadDevice(ctx->device);
    vkGetDeviceQueue(ctx->device, ctx->queue_family_index, 0, &ctx->queue);

    VmaVulkanFunctions vma_fns = {
        .vkGetInstanceProcAddr = vkGetInstanceProcAddr,
        .vkGetDeviceProcAddr = vkGetDeviceProcAddr,
    };
    VmaAllocatorCreateInfo alloc_info = {
        .vulkanApiVersion = VK_API_VERSION_1_4,
        .physicalDevice = ctx->physical_device,
        .device = ctx->device,
        .instance = ctx->instance,
        .pVulkanFunctions = &vma_fns,
    };
    VK_OR_FAIL(vmaCreateAllocator(&alloc_info, &ctx->allocator), -ENOMEM);

    VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = ctx->queue_family_index,
    };
    VK_OR_FAIL(vkCreateCommandPool(ctx->device, &pool_info, NULL, &ctx->command_pool), -ENOMEM);

    /* Power-of-10 §5: pin the post-condition that every handle the
     * caller may dereference is now non-null. Catches a future
     * mistake where one of the create calls is moved out of order. */
    assert(ctx->instance != VK_NULL_HANDLE);
    assert(ctx->physical_device != VK_NULL_HANDLE);
    assert(ctx->device != VK_NULL_HANDLE);
    assert(ctx->queue != VK_NULL_HANDLE);
    assert(ctx->allocator != VK_NULL_HANDLE);
    assert(ctx->command_pool != VK_NULL_HANDLE);

    /* ADR-3402: always report the attached device identity. Ordinals
     * differ across drivers (research-0090); vendorID/deviceID do not.
     * shaderFloat64 is reported as device information only — it is not
     * requested and no kernel needs it. */
    fprintf(stderr,
            "libvmaf: Vulkan integer path attached to \"%s\" "
            "(vendorID 0x%04x deviceID 0x%04x, api %u.%u.%u, "
            "shaderInt64=1, device shaderFloat64=%d)\n",
            ctx->props.deviceName, ctx->props.vendorID, ctx->props.deviceID,
            ctx->props.apiVersion >> 22, (ctx->props.apiVersion >> 12) & 0x3ffu,
            ctx->props.apiVersion & 0xfffu, ctx->has_float64);

    *out = ctx;
    return 0;

fail:
    if (ctx->command_pool != VK_NULL_HANDLE)
        vkDestroyCommandPool(ctx->device, ctx->command_pool, NULL);
    if (ctx->allocator != VK_NULL_HANDLE)
        vmaDestroyAllocator(ctx->allocator);
    if (ctx->device != VK_NULL_HANDLE)
        vkDestroyDevice(ctx->device, NULL);
    if (ctx->instance != VK_NULL_HANDLE)
        vkDestroyInstance(ctx->instance, NULL);
fail_alloc:
    free(ctx);
    *out = NULL;
    return err;
}

void vmaf_vulkan_context_destroy(VmafVulkanContext *ctx)
{
    if (!ctx)
        return;
    /* command_pool + allocator are always libvmaf-owned. */
    if (ctx->command_pool != VK_NULL_HANDLE)
        vkDestroyCommandPool(ctx->device, ctx->command_pool, NULL);
    if (ctx->allocator != VK_NULL_HANDLE)
        vmaDestroyAllocator(ctx->allocator);
    if (ctx->owns_handles) {
        if (ctx->device != VK_NULL_HANDLE)
            vkDestroyDevice(ctx->device, NULL);
        if (ctx->instance != VK_NULL_HANDLE)
            vkDestroyInstance(ctx->instance, NULL);
    }
    free(ctx);
}
