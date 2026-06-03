/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 *
 *  Vulkan Memory Allocator implementation TU.
 *
 *  VMA is a single-header C++ library — exactly one TU in the
 *  binary must define `VMA_IMPLEMENTATION` before including
 *  `vk_mem_alloc.h` so the implementation symbols actually emit.
 *  This file is that TU. The rest of libvmaf includes
 *  `vk_mem_alloc.h` plain via `vulkan_internal.h`.
 */

/* volk loads every Vulkan entry point at runtime, so VMA must call
 * the function pointers we hand it via VmaVulkanFunctions rather
 * than the prototypes the system headers would otherwise emit. */
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif

/* VMA_VULKAN_VERSION matches the API version we request in
 * vmaCreateAllocator() — anything lower would silently disable
 * features we want (e.g. VK_KHR_buffer_device_address). */
#define VMA_VULKAN_VERSION 1004000

/* Use volk's function-pointer table; VMA picks them up via
 * VmaAllocatorCreateInfo.pVulkanFunctions = vma_fns. */
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1

#define VMA_IMPLEMENTATION
#include <volk.h>
#include <vk_mem_alloc.h>
