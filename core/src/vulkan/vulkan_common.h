/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 */

#ifndef LIBVMAF_VULKAN_COMMON_H_
#define LIBVMAF_VULKAN_COMMON_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Vulkan runtime context (T5-1b, ADR-0175 follow-up).
 *
 * Owns the VkInstance + VkPhysicalDevice + VkDevice + compute queue
 * + a VMA allocator + the command pool used by every feature kernel.
 * Created via vmaf_vulkan_context_new(); released via
 * vmaf_vulkan_context_destroy().
 *
 * The opaque struct is defined in common.c. Callers pass the
 * VmafVulkanContext pointer to per-feature dispatch wrappers in
 * libvmaf/src/feature/vulkan/. Internal Vulkan handles (volk-loaded)
 * are NOT exposed in the public ABI — feature TUs include
 * `vulkan_internal.h` to read the handle layout.
 */
typedef struct VmafVulkanContext VmafVulkanContext;

/*
 * Allocate + initialise a Vulkan compute context.
 *
 * device_index:
 *   < 0  → auto: prefer discrete > integrated > virtual > cpu.
 *   >= 0 → pick the Nth physical device with VK_QUEUE_COMPUTE_BIT.
 *
 * Returns 0 on success, -EINVAL on bad pointer / device_index out of
 * range, -ENOMEM on allocation failure, -ENOSYS if no Vulkan loader
 * is available at runtime, -ENODEV if no compute-capable physical
 * device is present.
 */
int vmaf_vulkan_context_new(VmafVulkanContext **ctx, int device_index);

/*
 * Variant of @ref vmaf_vulkan_context_new that also accepts the
 * `require_fp64` opt-in. Non-zero `require_fp64` makes the init refuse
 * to attach to devices without `VkPhysicalDeviceFeatures::shaderFloat64`
 * with `-ENOTSUP`. Default (0) auto-falls-back to the fp32 VIF shader
 * variant on such devices (ADR-0509, supersedes ADR-0492's hard refusal).
 * Existing callers that pass through `vmaf_vulkan_context_new` get the
 * default auto-fallback behaviour for free.
 */
int vmaf_vulkan_context_new_with_opts(VmafVulkanContext **ctx, int device_index, int require_fp64);

void vmaf_vulkan_context_destroy(VmafVulkanContext *ctx);

/*
 * Accessor that lets feature kernels reuse the context owned by an
 * imported public-API state. Returns NULL on a NULL state. The public
 * VmafVulkanState handle is opaque in libvmaf_vulkan.h, so this lives
 * on the internal common.h header.
 */
struct VmafVulkanState;
VmafVulkanContext *vmaf_vulkan_state_get_context(struct VmafVulkanState *state);

/*
 * Number of physical devices that expose a VK_QUEUE_COMPUTE_BIT
 * queue family. Returns 0 if Vulkan is unavailable; negative
 * value (-errno) on enumeration failure.
 *
 * Safe to call without holding a context — the function spins up a
 * temporary VkInstance, queries, and tears it down.
 */
int vmaf_vulkan_device_count(void);

#ifdef __cplusplus
}
#endif

#endif /* LIBVMAF_VULKAN_COMMON_H_ */
