/* Copyright 2026 luoye-cpu
 * SPDX-License-Identifier: EUPL-1.2
 */
#include "common.h"

#include "libvmaf/libvmaf_vulkan.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

/*
 * Dynamic loader + minimal bring-up. See common.h for the draft-scope
 * rationale (no SDK / volk / VMA, loader-only, shaderInt64 only).
 */

VmafxVulkanFn vmafx_vk;

#define VMAFX_VK_ENTRY_LIST(X)      \
    X(GetInstanceProcAddr)          \
    X(CreateInstance)               \
    X(DestroyInstance)              \
    X(EnumeratePhysicalDevices)     \
    X(GetPhysicalDeviceProperties)  \
    X(GetPhysicalDeviceFeatures)    \
    X(GetPhysicalDeviceQueueFamilyProperties) \
    X(GetPhysicalDeviceMemoryProperties)      \
    X(EnumerateDeviceExtensionProperties)     \
    X(CreateDevice)                 \
    X(DestroyDevice)                \
    X(GetDeviceQueue)               \
    X(DeviceWaitIdle)               \
    X(CreateBuffer)                 \
    X(DestroyBuffer)                \
    X(GetBufferMemoryRequirements)  \
    X(AllocateMemory)               \
    X(FreeMemory)                   \
    X(BindBufferMemory)             \
    X(MapMemory)                    \
    X(UnmapMemory)                  \
    X(CreateDescriptorSetLayout)    \
    X(DestroyDescriptorSetLayout)   \
    X(CreateDescriptorPool)         \
    X(DestroyDescriptorPool)        \
    X(AllocateDescriptorSets)       \
    X(UpdateDescriptorSets)         \
    X(CreateShaderModule)           \
    X(DestroyShaderModule)          \
    X(CreatePipelineLayout)         \
    X(DestroyPipelineLayout)        \
    X(CreateComputePipelines)       \
    X(DestroyPipeline)              \
    X(CreateCommandPool)            \
    X(DestroyCommandPool)           \
    X(AllocateCommandBuffers)       \
    X(FreeCommandBuffers)           \
    X(BeginCommandBuffer)           \
    X(EndCommandBuffer)             \
    X(ResetCommandBuffer)           \
    X(CmdBindPipeline)              \
    X(CmdBindDescriptorSets)        \
    X(CmdPushConstants)             \
    X(CmdDispatch)                  \
    X(CmdPipelineBarrier)           \
    X(QueueSubmit)                  \
    X(CreateFence)                  \
    X(DestroyFence)                 \
    X(ResetFences)                  \
    X(WaitForFences)

static void *g_loader;

int vmafx_vulkan_load(void)
{
    if (g_loader)
        return 0;

#ifdef _WIN32
    g_loader = (void *)LoadLibraryA("vulkan-1.dll");
#else
    g_loader = dlopen("libvulkan.so.1", RTLD_LAZY | RTLD_LOCAL);
    if (!g_loader)
        g_loader = dlopen("libvulkan.so.2", RTLD_LAZY | RTLD_LOCAL);
#endif
    if (!g_loader)
        return -ENOENT;

#ifdef _WIN32
    vmafx_vk.GetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)(uintptr_t)
        GetProcAddress((HMODULE)g_loader, "vkGetInstanceProcAddr");
#else
    vmafx_vk.GetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)(uintptr_t)
        dlsym(g_loader, "vkGetInstanceProcAddr");
#endif
    if (!vmafx_vk.GetInstanceProcAddr)
        return -ENOENT;

    /* First pass: vkCreateInstance only. Some installed loaders (observed
     * in the wild: a host with a miner-installed loader shim) resolve no
     * other entry point for a NULL instance, although the spec requires
     * instance-level trampolies to resolve there — so the rest of the
     * table is filled in vmafx_vulkan_resolve() against the real instance
     * handle. */
    vmafx_vk.CreateInstance = (PFN_vkCreateInstance)(uintptr_t)
        vmafx_vk.GetInstanceProcAddr(NULL, "vkCreateInstance");
    if (!vmafx_vk.CreateInstance)
        return -ENOSYS;
    return 0;
}

int vmafx_vulkan_resolve(VmafVulkanState *s)
{
    /* Second pass: everything else resolves against the instance and
     * merges into the table — an entry that resolved in the first pass
     * stays (vkCreateInstance may legitimately be NULL for a valid
     * instance handle). */
#define X(name) \
    if (strcmp(#name, "GetInstanceProcAddr") != 0) { \
        void *p = (void *)(uintptr_t)vmafx_vk.GetInstanceProcAddr(s->instance, "vk" #name); \
        if (p) \
            vmafx_vk.name = (PFN_vk##name)(uintptr_t)p; \
    }
    VMAFX_VK_ENTRY_LIST(X)
#undef X

    if (!vmafx_vk.CreateInstance || !vmafx_vk.EnumeratePhysicalDevices ||
        !vmafx_vk.CreateDevice || !vmafx_vk.DestroyInstance)
        return -ENOSYS;
    return 0;
}

void vmafx_vulkan_unload(void)
{
    if (!g_loader)
        return;
#ifdef _WIN32
    FreeLibrary((HMODULE)g_loader);
#else
    dlclose(g_loader);
#endif
    g_loader = NULL;
    memset(&vmafx_vk, 0, sizeof(vmafx_vk));
}

const char *vmafx_vulkan_result_str(VkResult r)
{
    switch (r) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_OUT_OF_POOL_MEMORY: return "VK_ERROR_OUT_OF_POOL_MEMORY";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    default: return "VK_ERROR_(other)";
    }
}

static int find_compute_queue(VmafVulkanState *s)
{
    uint32_t nq = 0;
    vmafx_vk.GetPhysicalDeviceQueueFamilyProperties(s->physical_device, &nq, NULL);
    VkQueueFamilyProperties *qp = calloc(nq ? nq : 1, sizeof(*qp));
    if (!qp)
        return -ENOMEM;
    vmafx_vk.GetPhysicalDeviceQueueFamilyProperties(s->physical_device, &nq, qp);
    int found = -1;
    for (uint32_t i = 0; i < nq; i++) {
        if (qp[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
            found = (int)i;
            break;
        }
    }
    free(qp);
    if (found < 0)
        return -ENODEV;
    s->compute_queue_family = (uint32_t)found;
    return 0;
}

static int pick_memory_types(VmafVulkanState *s)
{
    s->host_visible_mt = -1;
    s->device_local_mt = -1;
    for (uint32_t m = 0; m < s->memprops.memoryTypeCount; m++) {
        const VkMemoryType *t = &s->memprops.memoryTypes[m];
        if (s->host_visible_mt < 0 &&
            (t->propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
            (t->propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
            s->host_visible_mt = (int32_t)m;
        if (s->device_local_mt < 0 &&
            (t->propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            s->device_local_mt = (int32_t)m;
    }
    if (s->host_visible_mt < 0)
        return -ENODEV;
    return 0;
}

int vmafx_vulkan_state_create(VmafVulkanState **state, int device_index)
{
    if (!state)
        return -EINVAL;
    int err = vmafx_vulkan_load();
    if (err)
        return err;

    VmafVulkanState *s = calloc(1, sizeof(*s));
    if (!s)
        return -ENOMEM;
    s->device_index = device_index;

    VkApplicationInfo app;
    memset(&app, 0, sizeof(app));
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "libvmaf-vulkan";
    app.applicationVersion = 1;

    static const uint32_t want_api[] = {
        VK_MAKE_API_VERSION(0, 1, 3, 0),
        VK_MAKE_API_VERSION(0, 1, 1, 0),
        VK_MAKE_API_VERSION(0, 1, 0, 0),
    };
    VkInstanceCreateInfo ici;
    memset(&ici, 0, sizeof(ici));
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    VkResult r = VK_ERROR_INCOMPATIBLE_DRIVER;
    for (unsigned i = 0; i < 3; i++) {
        app.apiVersion = want_api[i];
        r = vmafx_vk.CreateInstance(&ici, NULL, &s->instance);
        if (r == VK_SUCCESS)
            break;
    }
    if (r != VK_SUCCESS) {
        fprintf(stderr, "libvmaf[vulkan]: vkCreateInstance: %s\n",
                vmafx_vulkan_result_str(r));
        err = -ENODEV;
        goto fail;
    }

    err = vmafx_vulkan_resolve(s);
    if (err) {
        fprintf(stderr,
                "libvmaf[vulkan]: instance resolves an incomplete entry table; missing:");
#define X(name) \
        if (!vmafx_vk.name && strcmp(#name, "GetInstanceProcAddr") != 0) \
            fprintf(stderr, " vk%s", #name);
        VMAFX_VK_ENTRY_LIST(X)
#undef X
        fprintf(stderr, "\n");
        goto fail;
    }

    uint32_t np = 0;
    vmafx_vk.EnumeratePhysicalDevices(s->instance, &np, NULL);
    if (!np) {
        fprintf(stderr, "libvmaf[vulkan]: no Vulkan physical devices\n");
        err = -ENODEV;
        goto fail;
    }
    VkPhysicalDevice *devs = calloc(np, sizeof(*devs));
    if (!devs) {
        err = -ENOMEM;
        goto fail;
    }
    vmafx_vk.EnumeratePhysicalDevices(s->instance, &np, devs);

    /* Selection: explicit index, else discrete > integrated > anything. */
    if (device_index >= 0 && (uint32_t)device_index < np) {
        s->physical_device = devs[device_index];
    } else {
        int best = -1, best_rank = -1;
        for (uint32_t i = 0; i < np; i++) {
            VkPhysicalDeviceProperties p;
            vmafx_vk.GetPhysicalDeviceProperties(devs[i], &p);
            int rank;
            if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
                rank = 2;
            else if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)
                rank = 1;
            else
                rank = 0;
            if (rank > best_rank) {
                best_rank = rank;
                best = (int)i;
            }
        }
        if (best < 0) {
            err = -ENODEV;
            free(devs);
            goto fail;
        }
        s->physical_device = devs[best];
        s->device_index = best;
    }
    free(devs);

    vmafx_vk.GetPhysicalDeviceProperties(s->physical_device, &s->props);
    snprintf(s->device_name, sizeof(s->device_name), "%s", s->props.deviceName);
    s->vendor_id = s->props.vendorID;
    s->device_id = s->props.deviceID;
    s->api_version = s->props.apiVersion;

    VkPhysicalDeviceFeatures feats;
    vmafx_vk.GetPhysicalDeviceFeatures(s->physical_device, &feats);
    if (!feats.shaderInt64) {
        fprintf(stderr, "libvmaf[vulkan]: %s lacks shaderInt64\n", s->device_name);
        err = -ENODEV;
        goto fail;
    }

    err = find_compute_queue(s);
    if (err)
        goto fail;

    vmafx_vk.GetPhysicalDeviceMemoryProperties(s->physical_device, &s->memprops);
    err = pick_memory_types(s);
    if (err)
        goto fail;

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci;
    memset(&qci, 0, sizeof(qci));
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = s->compute_queue_family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;

    VkPhysicalDeviceFeatures enabled;
    memset(&enabled, 0, sizeof(enabled));
    enabled.shaderInt64 = VK_TRUE;

    VkDeviceCreateInfo dci;
    memset(&dci, 0, sizeof(dci));
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.pEnabledFeatures = &enabled;
    r = vmafx_vk.CreateDevice(s->physical_device, &dci, NULL, &s->device);
    if (r != VK_SUCCESS) {
        fprintf(stderr, "libvmaf[vulkan]: vkCreateDevice on %s: %s\n",
                s->device_name, vmafx_vulkan_result_str(r));
        err = -ENODEV;
        goto fail;
    }
    vmafx_vk.GetDeviceQueue(s->device, s->compute_queue_family, 0, &s->queue);

    *state = s;
    return 0;

fail:
    if (s->instance && vmafx_vk.DestroyInstance)
        vmafx_vk.DestroyInstance(s->instance, NULL);
    free(s);
    return err;
}

int vmafx_vulkan_state_destroy(VmafVulkanState *s)
{
    if (!s)
        return -EINVAL;
    if (s->device)
        vmafx_vk.DeviceWaitIdle(s->device);
    if (s->device)
        vmafx_vk.DestroyDevice(s->device, NULL);
    if (s->instance)
        vmafx_vk.DestroyInstance(s->instance, NULL);
    free(s);
    return 0;
}

int vmafx_vulkan_create_buffer(VmafVulkanState *s, VkDeviceSize size,
                               int host_visible, VmafxVulkanBuffer *out)
{
    if (!s || !out || !size)
        return -EINVAL;

    VkBufferCreateInfo bci;
    memset(&bci, 0, sizeof(bci));
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = size;
    bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkResult r = vmafx_vk.CreateBuffer(s->device, &bci, NULL, &out->buf);
    if (r != VK_SUCCESS)
        return -ENOMEM;

    VkMemoryRequirements mr;
    vmafx_vk.GetBufferMemoryRequirements(s->device, out->buf, &mr);

    uint32_t mtype = 0xffffffff;
    const int32_t want = host_visible ? s->host_visible_mt : s->device_local_mt;
    for (uint32_t m = 0; m < s->memprops.memoryTypeCount; m++) {
        if (!(mr.memoryTypeBits & (1u << m)))
            continue;
        if (want >= 0 && (uint32_t)want == m) {
            mtype = m;
            break;
        }
        if (want < 0 && host_visible &&
            (s->memprops.memoryTypes[m].propertyFlags &
             (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            mtype = m;
            break;
        }
        if (want < 0 && !host_visible &&
            (s->memprops.memoryTypes[m].propertyFlags &
             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            mtype = m;
            break;
        }
    }
    if (mtype == 0xffffffff) {
        vmafx_vk.DestroyBuffer(s->device, out->buf, NULL);
        return -ENODEV;
    }

    VkMemoryAllocateInfo mai;
    memset(&mai, 0, sizeof(mai));
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = mtype;
    r = vmafx_vk.AllocateMemory(s->device, &mai, NULL, &out->mem);
    if (r != VK_SUCCESS) {
        vmafx_vk.DestroyBuffer(s->device, out->buf, NULL);
        return -ENOMEM;
    }
    r = vmafx_vk.BindBufferMemory(s->device, out->buf, out->mem, 0);
    if (r != VK_SUCCESS) {
        vmafx_vk.FreeMemory(s->device, out->mem, NULL);
        vmafx_vk.DestroyBuffer(s->device, out->buf, NULL);
        return -ENOMEM;
    }
    out->mapped = NULL;
    out->size = mr.size;
    if (host_visible) {
        r = vmafx_vk.MapMemory(s->device, out->mem, 0, mr.size, 0, &out->mapped);
        if (r != VK_SUCCESS) {
            vmafx_vk.FreeMemory(s->device, out->mem, NULL);
            vmafx_vk.DestroyBuffer(s->device, out->buf, NULL);
            return -ENOMEM;
        }
        memset(out->mapped, 0, (size_t)mr.size);
    }
    return 0;
}

void vmafx_vulkan_destroy_buffer(VmafVulkanState *s, VmafxVulkanBuffer *b)
{
    if (!s || !b)
        return;
    if (b->mapped)
        vmafx_vk.UnmapMemory(s->device, b->mem);
    if (b->mem)
        vmafx_vk.FreeMemory(s->device, b->mem, NULL);
    if (b->buf)
        vmafx_vk.DestroyBuffer(s->device, b->buf, NULL);
    memset(b, 0, sizeof(*b));
}

/*
 * Public API wrappers (libvmaf/libvmaf_vulkan.h). The import hook
 * (vmaf_vulkan_import_state) lives in libvmaf.c beside the other
 * backends' imports — it needs VmafContext field access, the same
 * convention as the CUDA / SYCL / HIP / Metal twins.
 */

int vmaf_vulkan_available(void)
{
    return 1;
}

int vmaf_vulkan_state_init(VmafVulkanState **out, VmafVulkanConfiguration cfg)
{
    if (!out)
        return -EINVAL;
    *out = NULL;
    if (cfg.flags)
        return -EINVAL;
    return vmafx_vulkan_state_create(out, cfg.device_index);
}

void vmaf_vulkan_state_free(VmafVulkanState **state)
{
    if (!state || !*state)
        return;
    vmafx_vulkan_state_destroy(*state);
    *state = NULL;
}
