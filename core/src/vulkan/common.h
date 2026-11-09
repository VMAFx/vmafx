/* Copyright 2026 luoye-cpu
 * SPDX-License-Identifier: EUPL-1.2
 */
#ifndef LIBVMAF_SRC_VULKAN_COMMON_H_
#define LIBVMAF_SRC_VULKAN_COMMON_H_

#include <stdint.h>

#include <vulkan/vulkan.h>

#include "vulkan_loader.h"

/*
 * Minimal Vulkan runtime for the integer compute path (draft scope:
 * integer ADM). Deliberately narrower than the backend ADR-0726 removed:
 *
 *   - no volk, no Vulkan Memory Allocator, no SDK: the runtime compiles
 *     with VK_NO_PROTOTYPES and loads `vulkan-1` / `libvulkan.so.1`
 *     dynamically, so building the backend needs only Vulkan headers;
 *   - no zero-copy import surfaces: pictures are plain VkBuffers the
 *     extractor uploads into through host-visible memory;
 *   - device selection: first compute-capable physical device, discrete
 *     before integrated, with an explicit `device_index` override;
 *   - feature requirements: shaderInt64 only (the integer ADM shader is
 *     fp64-free by construction — see adm_angle_flag_i64()).
 *
 * A later PR can grow this toward the removed backend's import story;
 * ADR-3402 scopes the integer path explicitly to this surface.
 */

/* The concrete definition behind the opaque public `VmafVulkanState`
 * (libvmaf/libvmaf_vulkan.h). The internal typedef repeats the public
 * one — the two denote the same type, which C11 (6.7 3) and C++ permit
 * a typedef to be redefined to. */
struct VmafVulkanState {
    VkInstance instance;
    VkPhysicalDevice physical_device;
    VkDevice device;
    uint32_t compute_queue_family;
    VkQueue queue;

    int device_index;      /* index into the enumeration at bring-up */
    char device_name[256];
    uint32_t vendor_id;
    uint32_t device_id;
    uint32_t api_version;  /* packed VK_API_VERSION_… of the device */

    int32_t host_visible_mt;  /* HOST_VISIBLE|HOST_COHERENT memory type, -1 n/a */
    int32_t device_local_mt;  /* DEVICE_LOCAL memory type, -1 when n/a */

    /* Buffer sizes the extractor validates its allocations against. */
    VkPhysicalDeviceMemoryProperties memprops;
    VkPhysicalDeviceProperties props;
};

typedef struct VmafVulkanState VmafVulkanState;

typedef struct VmafxVulkanBuffer {
    VkBuffer buf;
    VkDeviceMemory mem;
    void *mapped;          /* non-NULL for host-visible allocations */
    VkDeviceSize size;
} VmafxVulkanBuffer;

/* All functions return 0 or a negative errno. */
int vmafx_vulkan_load(void);                 /* resolve the loader, once */
int vmafx_vulkan_resolve(VmafVulkanState *s); /* fill the table against an instance */
void vmafx_vulkan_unload(void);

int vmafx_vulkan_state_create(VmafVulkanState **state, int device_index);
int vmafx_vulkan_state_destroy(VmafVulkanState *state);

int vmafx_vulkan_create_buffer(VmafVulkanState *s, VkDeviceSize size,
                               int host_visible, VmafxVulkanBuffer *out);
void vmafx_vulkan_destroy_buffer(VmafVulkanState *s, VmafxVulkanBuffer *b);

const char *vmafx_vulkan_result_str(VkResult r);

#endif /* LIBVMAF_SRC_VULKAN_COMMON_H_ */
