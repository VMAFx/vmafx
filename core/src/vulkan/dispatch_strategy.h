/**
 *  Copyright 2026 Lusoris and Claude (Anthropic)
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 *
 *  Vulkan dispatch_strategy — translates a per-feature descriptor
 *  into a Vulkan command-buffer strategy. Today every Vulkan
 *  extractor records a fresh primary command buffer per frame; this
 *  stub exposes the registry-aware decision surface so a future PR
 *  can opt-in secondary-cmdbuf reuse for ADM (16 dispatches/frame
 *  is the obvious first candidate). See ADR-0181.
 */
#ifndef LIBVMAF_VULKAN_DISPATCH_STRATEGY_H_
#define LIBVMAF_VULKAN_DISPATCH_STRATEGY_H_

#include "feature/feature_characteristics.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    /// Record a primary command buffer per frame — current default
    /// for every Vulkan extractor.
    VMAF_VULKAN_DISPATCH_PRIMARY_CMDBUF = 0,
    /// Record once into a secondary command buffer, replay every
    /// frame from the primary. Future opt-in for ADM.
    VMAF_VULKAN_DISPATCH_SECONDARY_CMDBUF_REUSE,
} VmafVulkanDispatchStrategy;

/**
 * Returns the Vulkan dispatch strategy for the given feature.
 *
 * @param feature_name  Feature name; used by env-override parser.
 * @param chars         Per-feature characteristics descriptor.
 * @param frame_w       Frame width in pixels.
 * @param frame_h       Frame height in pixels.
 *
 * Env override: VMAF_VULKAN_DISPATCH=<feature>:reuse,<feature>:primary,...
 *
 * Stub: today returns PRIMARY_CMDBUF for every input pending
 * profiler-driven secondary-cmdbuf follow-up. See ADR-0181.
 */
VmafVulkanDispatchStrategy vmaf_vulkan_select_strategy(const char *feature_name,
                                                       const VmafFeatureCharacteristics *chars,
                                                       unsigned frame_w, unsigned frame_h);

#ifdef __cplusplus
}
#endif

#endif /* LIBVMAF_VULKAN_DISPATCH_STRATEGY_H_ */
