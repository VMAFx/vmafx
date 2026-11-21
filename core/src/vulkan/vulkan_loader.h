/* Copyright 2026 luoye-cpu
 * SPDX-License-Identifier: EUPL-1.2
 */
#ifndef LIBVMAF_SRC_VULKAN_VULKAN_LOADER_H_
#define LIBVMAF_SRC_VULKAN_VULKAN_LOADER_H_

/*
 * The dynamic-loader function table. The runtime compiles with
 * VK_NO_PROTOTYPES (set per-TU from meson when enable_vulkan is on) and
 * resolves every entry through vkGetInstanceProcAddr. Resolution is
 * two-pass (see common.c): vmafx_vulkan_load() fetches the loader root
 * and vkCreateInstance alone, and vmafx_vulkan_resolve() fills the rest
 * of the table against the created instance — instance-level entry
 * points cover all core device functions, so no vkGetDeviceProcAddr
 * pass is needed for this surface. Some non-compliant loaders resolve
 * nothing but vkCreateInstance for a NULL instance, which is why the
 * first pass does not attempt the full table.
 */

typedef struct VmafxVulkanFn {
    /* loader root */
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr;

    /* instance / physical device */
    PFN_vkCreateInstance CreateInstance;
    PFN_vkDestroyInstance DestroyInstance;
    PFN_vkEnumeratePhysicalDevices EnumeratePhysicalDevices;
    PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties;
    PFN_vkGetPhysicalDeviceFeatures GetPhysicalDeviceFeatures;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
    PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties;

    /* device */
    PFN_vkCreateDevice CreateDevice;
    PFN_vkDestroyDevice DestroyDevice;
    PFN_vkGetDeviceQueue GetDeviceQueue;
    PFN_vkDeviceWaitIdle DeviceWaitIdle;

    /* memory + buffers */
    PFN_vkCreateBuffer CreateBuffer;
    PFN_vkDestroyBuffer DestroyBuffer;
    PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements;
    PFN_vkAllocateMemory AllocateMemory;
    PFN_vkFreeMemory FreeMemory;
    PFN_vkBindBufferMemory BindBufferMemory;
    PFN_vkMapMemory MapMemory;
    PFN_vkUnmapMemory UnmapMemory;

    /* descriptors + pipelines */
    PFN_vkCreateDescriptorSetLayout CreateDescriptorSetLayout;
    PFN_vkDestroyDescriptorSetLayout DestroyDescriptorSetLayout;
    PFN_vkCreateDescriptorPool CreateDescriptorPool;
    PFN_vkDestroyDescriptorPool DestroyDescriptorPool;
    PFN_vkAllocateDescriptorSets AllocateDescriptorSets;
    PFN_vkUpdateDescriptorSets UpdateDescriptorSets;
    PFN_vkCreateShaderModule CreateShaderModule;
    PFN_vkDestroyShaderModule DestroyShaderModule;
    PFN_vkCreatePipelineLayout CreatePipelineLayout;
    PFN_vkDestroyPipelineLayout DestroyPipelineLayout;
    PFN_vkCreateComputePipelines CreateComputePipelines;
    PFN_vkDestroyPipeline DestroyPipeline;

    /* command submission */
    PFN_vkCreateCommandPool CreateCommandPool;
    PFN_vkDestroyCommandPool DestroyCommandPool;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
    PFN_vkFreeCommandBuffers FreeCommandBuffers;
    PFN_vkBeginCommandBuffer BeginCommandBuffer;
    PFN_vkEndCommandBuffer EndCommandBuffer;
    PFN_vkResetCommandBuffer ResetCommandBuffer;
    PFN_vkCmdBindPipeline CmdBindPipeline;
    PFN_vkCmdBindDescriptorSets CmdBindDescriptorSets;
    PFN_vkCmdPushConstants CmdPushConstants;
    PFN_vkCmdDispatch CmdDispatch;
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier;
    PFN_vkQueueSubmit QueueSubmit;
    PFN_vkCreateFence CreateFence;
    PFN_vkDestroyFence DestroyFence;
    PFN_vkResetFences ResetFences;
    PFN_vkWaitForFences WaitForFences;
} VmafxVulkanFn;

/* The single table instance, defined in common.c. */
extern VmafxVulkanFn vmafx_vk;

#endif /* LIBVMAF_SRC_VULKAN_VULKAN_LOADER_H_ */
