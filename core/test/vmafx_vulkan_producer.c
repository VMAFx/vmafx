/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * The producer of the VMAFx Vulkan import tests; see vmafx_vulkan_producer.h.
 *
 * A frame is one image (or buffer) per plane, each with its own exportable
 * dedicated or plain allocation, and one exportable timeline semaphore for
 * the whole frame (FFmpeg keeps one per image and signals them in one
 * submission; the tests that need several build them from several frames).
 * A write copies a staging buffer into the planes, moves the images to
 * VK_IMAGE_LAYOUT_GENERAL and releases them to VK_QUEUE_FAMILY_EXTERNAL, as
 * FFmpeg's hwcontext does before it hands a frame to another API; the next
 * write acquires them back first.
 */

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <vulkan/vulkan.h>

#include "vmafx_vulkan_producer.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

/* Writes kept before the frame waits for its queue and recycles them. */
#define VKP_MAX_WRITES 16u
/* Row pitch alignment of buffer planes. */
#define VKP_BUFFER_PITCH 256u
/* Physical devices looked at. */
#define VKP_MAX_GPUS 8u
/* Bytes of each of the two scratch buffers busy work copies between. */
#define VKP_BUSY_BYTES (64ull << 20)
/* Busy copies one write queues at most. */
#define VKP_BUSY_MAX 256u

typedef struct VkpFns {
    PFN_vkGetMemoryFdKHR get_memory_fd;
    PFN_vkGetSemaphoreFdKHR get_semaphore_fd;
    PFN_vkGetImageDrmFormatModifierPropertiesEXT get_modifier;
} VkpFns;

struct VkpDevice {
    VkInstance instance;
    VkPhysicalDevice phys;
    VkDevice dev;
    VkQueue queue;
    uint32_t family;
    VkCommandPool pool;
    VkSemaphore gate;
    VkpFns fn;
    bool has_drm; /* VK_EXT_image_drm_format_modifier and dma-buf export */
    uint32_t pci[4];
    char text[1024];
    VkBuffer busy[2]; /* scratch of the busy work, made on first use */
    VkDeviceMemory busy_memory[2];
};

typedef struct VkpPlane {
    VkImage image;
    VkBuffer buffer;
    VkDeviceMemory memory;
    VkDeviceSize size; /* of the allocation */
    uint64_t offset;   /* of the plane's first byte in it */
    uint64_t pitch;    /* row pitch; 0 for an optimal image */
    uint64_t modifier;
    VkFormat format;
    uint32_t w;
    uint32_t h;
    uint32_t texel; /* bytes per texel */
    bool dedicated;
} VkpPlane;

/* One submitted write and what it holds until its queue finished. */
typedef struct VkpPending {
    VkCommandBuffer cmd;
    VkBuffer staging;
    VkDeviceMemory staging_memory;
} VkpPending;

struct VkpFrame {
    VkpDevice *d;
    VkpFrameDesc desc;
    uint32_t n_planes;
    VkpPlane plane[3];
    VkSemaphore timeline;
    VkSemaphore binary; /* desc.sync_fd */
    bool external;      /* the planes were released to VK_QUEUE_FAMILY_EXTERNAL */
    bool binary_pending;
    VkpPending pending[VKP_MAX_WRITES];
    uint32_t n_pending;
};

/* ---- Device ------------------------------------------------------------------------ */

static VkInstance make_instance(void)
{
    const VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                   .pApplicationName = "vmafx-vulkan-import-test",
                                   .apiVersion = VK_API_VERSION_1_3};
    const VkInstanceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                       .pApplicationInfo = &app};
    VkInstance instance = VK_NULL_HANDLE;
    return vkCreateInstance(&info, NULL, &instance) == VK_SUCCESS ? instance : VK_NULL_HANDLE;
}

/* The device's PCI location and its description for the evidence. */
static void describe_phys(VkpDevice *d)
{
    VkPhysicalDevicePCIBusInfoPropertiesEXT pci = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT};
    VkPhysicalDeviceDriverProperties driver = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES, .pNext = &pci};
    VkPhysicalDeviceProperties2 props = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                         .pNext = &driver};
    vkGetPhysicalDeviceProperties2(d->phys, &props);
    d->pci[0] = pci.pciDomain;
    d->pci[1] = pci.pciBus;
    d->pci[2] = pci.pciDevice;
    d->pci[3] = pci.pciFunction;
    const uint32_t api = props.properties.apiVersion;
    (void)snprintf(d->text, sizeof(d->text),
                   "%.200s, %.200s %.200s, Vulkan %u.%u.%u, PCI %04x:%02x:%02x.%x",
                   props.properties.deviceName, driver.driverName, driver.driverInfo,
                   VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api), VK_API_VERSION_PATCH(api),
                   d->pci[0], d->pci[1], d->pci[2], d->pci[3]);
}

/* How vkp_open*() picks its physical device. */
typedef struct VkpPick {
    uint32_t vendor;     /* 0: any */
    const uint32_t *pci; /* NULL: any */
    bool other;          /* a device NOT at `pci` */
} VkpPick;

static bool pci_of(VkPhysicalDevice phys, uint32_t pci[4])
{
    VkPhysicalDevicePCIBusInfoPropertiesEXT bus = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT};
    VkPhysicalDeviceProperties2 props = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                         .pNext = &bus};
    vkGetPhysicalDeviceProperties2(phys, &props);
    const uint32_t v[4] = {bus.pciDomain, bus.pciBus, bus.pciDevice, bus.pciFunction};
    memcpy(pci, v, sizeof(v));
    return props.properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU;
}

static bool picked(VkPhysicalDevice phys, const VkpPick *pick)
{
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(phys, &p);
    uint32_t pci[4];
    if (!pci_of(phys, pci) || (pick->vendor && p.vendorID != pick->vendor)) {
        return false;
    }
    const bool same = pick->pci && memcmp(pci, pick->pci, sizeof(pci)) == 0;
    return !pick->pci || (pick->other ? !same : same);
}

/* The first physical device `pick` accepts. */
static bool pick_phys(VkpDevice *d, const VkpPick *pick)
{
    VkPhysicalDevice list[VKP_MAX_GPUS];
    uint32_t n = VKP_MAX_GPUS;
    if (vkEnumeratePhysicalDevices(d->instance, &n, list) < 0) {
        return false;
    }
    for (uint32_t i = 0; i < n; i++) {
        if (picked(list[i], pick)) {
            d->phys = list[i];
            return true;
        }
    }
    return false;
}

/* A queue family with transfer (graphics and compute imply it). */
static bool pick_family(VkpDevice *d)
{
    VkQueueFamilyProperties fam[16];
    uint32_t n = 16u;
    vkGetPhysicalDeviceQueueFamilyProperties(d->phys, &n, fam);
    for (uint32_t i = 0; i < n; i++) {
        if (fam[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
            d->family = i;
            return true;
        }
    }
    return false;
}

static bool has_extension(VkPhysicalDevice phys, const char *name)
{
    VkExtensionProperties ext[512];
    uint32_t n = 512u;
    (void)vkEnumerateDeviceExtensionProperties(phys, NULL, &n, ext);
    for (uint32_t i = 0; i < n; i++) {
        if (strcmp(ext[i].extensionName, name) == 0) {
            return true;
        }
    }
    return false;
}

static bool make_device(VkpDevice *d)
{
    const char *ext[5] = {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
                          VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
                          VK_EXT_PCI_BUS_INFO_EXTENSION_NAME, NULL, NULL};
    uint32_t n_ext = 3u;
    d->has_drm = has_extension(d->phys, VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME) &&
                 has_extension(d->phys, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
    if (d->has_drm) {
        ext[n_ext++] = VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME;
        ext[n_ext++] = VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME;
    }
    VkPhysicalDeviceVulkan12Features v12 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .timelineSemaphore = VK_TRUE};
    const float priority = 1.0f;
    const VkDeviceQueueCreateInfo queue = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                           .queueFamilyIndex = d->family,
                                           .queueCount = 1,
                                           .pQueuePriorities = &priority};
    const VkDeviceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                     .pNext = &v12,
                                     .queueCreateInfoCount = 1,
                                     .pQueueCreateInfos = &queue,
                                     .enabledExtensionCount = n_ext,
                                     .ppEnabledExtensionNames = ext};
    if (vkCreateDevice(d->phys, &info, NULL, &d->dev) != VK_SUCCESS) {
        return false;
    }
    vkGetDeviceQueue(d->dev, d->family, 0, &d->queue);
    d->fn.get_memory_fd = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(d->dev, "vkGetMemoryFdKHR");
    d->fn.get_semaphore_fd =
        (PFN_vkGetSemaphoreFdKHR)vkGetDeviceProcAddr(d->dev, "vkGetSemaphoreFdKHR");
    d->fn.get_modifier = (PFN_vkGetImageDrmFormatModifierPropertiesEXT)vkGetDeviceProcAddr(
        d->dev, "vkGetImageDrmFormatModifierPropertiesEXT");
    return d->fn.get_memory_fd && d->fn.get_semaphore_fd;
}

/* A timeline semaphore at 0, exportable as an opaque descriptor when `export`. */
static VkSemaphore make_timeline(VkDevice dev, bool export)
{
    VkExportSemaphoreCreateInfo ex = {.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
                                      .handleTypes =
                                          VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT};
    VkSemaphoreTypeCreateInfo type = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                      .pNext = export ? &ex : NULL,
                                      .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
                                      .initialValue = 0};
    const VkSemaphoreCreateInfo info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
                                        .pNext = &type};
    VkSemaphore s = VK_NULL_HANDLE;
    return vkCreateSemaphore(dev, &info, NULL, &s) == VK_SUCCESS ? s : VK_NULL_HANDLE;
}

static VkpDevice *open_picked(const VkpPick *pick)
{
    VkpDevice *const d = calloc(1, sizeof(*d));
    if (!d) {
        return NULL;
    }
    d->instance = make_instance();
    bool ok = d->instance != VK_NULL_HANDLE && pick_phys(d, pick) && pick_family(d);
    if (ok) {
        describe_phys(d);
        ok = make_device(d);
    }
    const VkCommandPoolCreateInfo pool = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                          .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                          .queueFamilyIndex = d->family};
    ok = ok && vkCreateCommandPool(d->dev, &pool, NULL, &d->pool) == VK_SUCCESS;
    d->gate = ok ? make_timeline(d->dev, false) : VK_NULL_HANDLE;
    if (!ok || d->gate == VK_NULL_HANDLE) {
        (void)fprintf(stderr, "[no Vulkan device: vendor 0x%04x, %s PCI location] ",
                      (unsigned)pick->vendor,
                      pick->pci ? (pick->other ? "another" : "the") : "any");
        vkp_close(d);
        return NULL;
    }
    return d;
}

VkpDevice *vkp_open(uint32_t vendor)
{
    const VkpPick pick = {.vendor = vendor, .pci = NULL, .other = false};
    return open_picked(&pick);
}

int vkp_index_of_pci(const uint32_t pci[4])
{
    VkInstance instance = make_instance();
    VkPhysicalDevice list[VKP_MAX_GPUS];
    uint32_t n = VKP_MAX_GPUS;
    int found = -1;
    if (instance && vkEnumeratePhysicalDevices(instance, &n, list) >= 0) {
        for (uint32_t i = 0; i < n && found < 0; i++) {
            uint32_t at[4];
            found = pci_of(list[i], at) && memcmp(at, pci, sizeof(at)) == 0 ? (int)i : -1;
        }
    }
    if (instance) {
        vkDestroyInstance(instance, NULL);
    }
    return found;
}

VkpDevice *vkp_open_pci(const uint32_t pci[4])
{
    const VkpPick pick = {.vendor = 0u, .pci = pci, .other = false};
    return open_picked(&pick);
}

VkpDevice *vkp_open_other(const uint32_t pci[4])
{
    const VkpPick pick = {.vendor = 0u, .pci = pci, .other = true};
    return open_picked(&pick);
}

void vkp_close(VkpDevice *d)
{
    if (!d) {
        return;
    }
    if (d->dev) {
        (void)vkDeviceWaitIdle(d->dev);
        for (uint32_t i = 0; i < 2u; i++) {
            vkDestroyBuffer(d->dev, d->busy[i], NULL);
            vkFreeMemory(d->dev, d->busy_memory[i], NULL);
        }
        vkDestroySemaphore(d->dev, d->gate, NULL);
        vkDestroyCommandPool(d->dev, d->pool, NULL);
        vkDestroyDevice(d->dev, NULL);
    }
    if (d->instance) {
        vkDestroyInstance(d->instance, NULL);
    }
    free(d);
}

void vkp_pci(const VkpDevice *d, uint32_t pci[4])
{
    memcpy(pci, d->pci, sizeof(d->pci));
}

const char *vkp_describe(const VkpDevice *d)
{
    return d->text;
}

uint32_t vkp_loader_version(void)
{
    uint32_t v = 0;
    return vkEnumerateInstanceVersion(&v) == VK_SUCCESS ? v : 0u;
}

int vkp_gate_signal(VkpDevice *d, uint64_t value)
{
    const VkSemaphoreSignalInfo info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, .semaphore = d->gate, .value = value};
    return vkSignalSemaphore(d->dev, &info) == VK_SUCCESS ? 0 : -1;
}

/* ---- Frame planes --------------------------------------------------------------------- */

static bool semi_planar(uint32_t pix_fmt)
{
    return pix_fmt == VMAFX_PIXEL_FORMAT_NV12 || pix_fmt == VMAFX_PIXEL_FORMAT_P010 ||
           pix_fmt == VMAFX_PIXEL_FORMAT_P016;
}

/* Geometry and format of each plane of `desc`. */
static void plan_planes(VkpFrame *f)
{
    const VkpFrameDesc *const d = &f->desc;
    const uint32_t bytes = d->bpc > 8u ? 2u : 1u;
    const bool semi = semi_planar(d->pix_fmt);
    f->n_planes = semi ? 2u : 3u;
    for (uint32_t i = 0; i < f->n_planes; i++) {
        VkpPlane *const p = &f->plane[i];
        const bool chroma = i > 0u;
        p->w = chroma ? (d->w + 1u) / 2u : d->w;
        p->h = chroma ? (d->h + 1u) / 2u : d->h;
        p->texel = (chroma && semi ? 2u : 1u) * bytes;
        static const VkFormat formats[2][2] = {{VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM},
                                               {VK_FORMAT_R16_UNORM, VK_FORMAT_R16G16_UNORM}};
        p->format = formats[bytes - 1u][chroma && semi ? 1u : 0u];
    }
}

static VkExternalMemoryHandleTypeFlagBits handle_type(const VkpFrame *f)
{
    return f->desc.dma_buf ? VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT :
                             VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
}

static VkImageTiling tiling_of(const VkpFrame *f)
{
    switch (f->desc.layout) {
    case VKP_IMAGE_LINEAR:
        return VK_IMAGE_TILING_LINEAR;
    case VKP_IMAGE_DRM:
        return VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    default:
        return VK_IMAGE_TILING_OPTIMAL;
    }
}

/* A device-local memory type of `bits`. */
static uint32_t memory_type(const VkpDevice *d, uint32_t bits)
{
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(d->phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if ((bits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            return i;
        }
    }
    return UINT32_MAX;
}

/* A host-visible, coherent memory type of `bits`. */
static uint32_t host_type(const VkpDevice *d, uint32_t bits)
{
    const VkMemoryPropertyFlags want =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(d->phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) {
            return i;
        }
    }
    return UINT32_MAX;
}

/* Whether the driver exports images of plane `p` this way, and must give
 * them a dedicated allocation. */
static bool image_exportable(const VkpFrame *f, const VkpPlane *p, bool *dedicated_only)
{
    VkPhysicalDeviceImageDrmFormatModifierInfoEXT mod = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
        .drmFormatModifier = f->desc.modifier,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkPhysicalDeviceExternalImageFormatInfo ext = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
        .pNext = f->desc.layout == VKP_IMAGE_DRM ? &mod : NULL,
        .handleType = handle_type(f)};
    const VkPhysicalDeviceImageFormatInfo2 info = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
        .pNext = &ext,
        .format = p->format,
        .type = VK_IMAGE_TYPE_2D,
        .tiling = tiling_of(f),
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    VkExternalImageFormatProperties eprops = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
    VkImageFormatProperties2 props = {.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
                                      .pNext = &eprops};
    const VkResult r = vkGetPhysicalDeviceImageFormatProperties2(f->d->phys, &info, &props);
    const VkExternalMemoryFeatureFlags feat =
        eprops.externalMemoryProperties.externalMemoryFeatures;
    *dedicated_only = (feat & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT) != 0u;
    return r == VK_SUCCESS && (feat & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) &&
           props.imageFormatProperties.maxExtent.width >= p->w &&
           props.imageFormatProperties.maxExtent.height >= p->h;
}

static bool create_image(const VkpFrame *f, VkpPlane *p)
{
    VkImageDrmFormatModifierListCreateInfoEXT mods = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT,
        .drmFormatModifierCount = 1,
        .pDrmFormatModifiers = &f->desc.modifier};
    VkExternalMemoryImageCreateInfo ext = {.sType =
                                               VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
                                           .pNext = f->desc.layout == VKP_IMAGE_DRM ? &mods : NULL,
                                           .handleTypes = handle_type(f)};
    const VkImageCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                    .pNext = &ext,
                                    .imageType = VK_IMAGE_TYPE_2D,
                                    .format = p->format,
                                    .extent = {p->w, p->h, 1u},
                                    .mipLevels = 1,
                                    .arrayLayers = 1,
                                    .samples = VK_SAMPLE_COUNT_1_BIT,
                                    .tiling = tiling_of(f),
                                    .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                             VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    return vkCreateImage(f->d->dev, &info, NULL, &p->image) == VK_SUCCESS;
}

static bool create_buffer(const VkpFrame *f, VkpPlane *p)
{
    p->pitch =
        ((uint64_t)p->w * p->texel + VKP_BUFFER_PITCH - 1u) & ~(uint64_t)(VKP_BUFFER_PITCH - 1u);
    VkExternalMemoryBufferCreateInfo ext = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .handleTypes = handle_type(f)};
    const VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                     .pNext = &ext,
                                     .size = p->pitch * p->h,
                                     .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                              VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                     .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    return vkCreateBuffer(f->d->dev, &info, NULL, &p->buffer) == VK_SUCCESS;
}

/* Allocate and bind the plane's exportable memory. */
static bool bind_memory(const VkpFrame *f, VkpPlane *p, bool dedicated_only)
{
    VkMemoryDedicatedRequirements dreq = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    VkMemoryRequirements2 req = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, .pNext = &dreq};
    if (p->image) {
        const VkImageMemoryRequirementsInfo2 info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2, .image = p->image};
        vkGetImageMemoryRequirements2(f->d->dev, &info, &req);
    } else {
        const VkBufferMemoryRequirementsInfo2 info = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2, .buffer = p->buffer};
        vkGetBufferMemoryRequirements2(f->d->dev, &info, &req);
    }
    p->dedicated =
        dedicated_only || dreq.requiresDedicatedAllocation || dreq.prefersDedicatedAllocation;
    VkMemoryDedicatedAllocateInfo ded = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
                                         .image = p->image,
                                         .buffer = p->buffer};
    VkExportMemoryAllocateInfo exp = {.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
                                      .pNext = p->dedicated ? &ded : NULL,
                                      .handleTypes = handle_type(f)};
    const VkMemoryAllocateInfo info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &exp,
        .allocationSize = req.memoryRequirements.size,
        .memoryTypeIndex = memory_type(f->d, req.memoryRequirements.memoryTypeBits)};
    p->size = req.memoryRequirements.size;
    if (info.memoryTypeIndex == UINT32_MAX ||
        vkAllocateMemory(f->d->dev, &info, NULL, &p->memory) != VK_SUCCESS) {
        return false;
    }
    return p->image ? vkBindImageMemory(f->d->dev, p->image, p->memory, 0) == VK_SUCCESS :
                      vkBindBufferMemory(f->d->dev, p->buffer, p->memory, 0) == VK_SUCCESS;
}

/* Where a linear or modifier image's rows are, and its modifier. */
static void read_layout(const VkpFrame *f, VkpPlane *p)
{
    if (f->desc.layout != VKP_IMAGE_LINEAR && f->desc.layout != VKP_IMAGE_DRM) {
        return;
    }
    const VkImageSubresource sub = {.aspectMask = f->desc.layout == VKP_IMAGE_DRM ?
                                                      VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT :
                                                      VK_IMAGE_ASPECT_COLOR_BIT};
    VkSubresourceLayout layout;
    vkGetImageSubresourceLayout(f->d->dev, p->image, &sub, &layout);
    p->offset = layout.offset;
    p->pitch = layout.rowPitch;
    VkImageDrmFormatModifierPropertiesEXT mod = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT};
    if (f->desc.layout == VKP_IMAGE_DRM && f->d->fn.get_modifier &&
        f->d->fn.get_modifier(f->d->dev, p->image, &mod) == VK_SUCCESS) {
        p->modifier = mod.drmFormatModifier;
    }
}

static bool make_plane(const VkpFrame *f, VkpPlane *p)
{
    bool dedicated_only = false;
    if (f->desc.layout == VKP_BUFFER) {
        return create_buffer(f, p) && bind_memory(f, p, false);
    }
    if (!image_exportable(f, p, &dedicated_only)) {
        (void)fprintf(stderr, "[unsupported: format %d, tiling %d, %s export] ", (int)p->format,
                      (int)tiling_of(f), f->desc.dma_buf ? "dma-buf" : "opaque fd");
        return false;
    }
    const bool ok = create_image(f, p) && bind_memory(f, p, dedicated_only);
    if (ok) {
        read_layout(f, p);
    }
    return ok;
}

/* A binary semaphore exportable as a sync_file. */
static VkSemaphore make_binary(VkDevice dev)
{
    VkExportSemaphoreCreateInfo ex = {.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
                                      .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT};
    const VkSemaphoreCreateInfo info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
                                        .pNext = &ex};
    VkSemaphore s = VK_NULL_HANDLE;
    return vkCreateSemaphore(dev, &info, NULL, &s) == VK_SUCCESS ? s : VK_NULL_HANDLE;
}

VkpFrame *vkp_frame_new(VkpDevice *d, const VkpFrameDesc *desc)
{
    VkpFrame *const f = calloc(1, sizeof(*f));
    if (!f) {
        return NULL;
    }
    f->d = d;
    f->desc = *desc;
    plan_planes(f);
    bool ok = desc->layout != VKP_IMAGE_DRM || d->has_drm;
    for (uint32_t i = 0; i < f->n_planes && ok; i++) {
        ok = make_plane(f, &f->plane[i]);
    }
    f->timeline = ok ? make_timeline(d->dev, true) : VK_NULL_HANDLE;
    f->binary = ok && desc->sync_fd ? make_binary(d->dev) : VK_NULL_HANDLE;
    if (!f->timeline || (desc->sync_fd && !f->binary)) {
        vkp_frame_free(f);
        return NULL;
    }
    return f;
}

static void free_pending(VkpFrame *f, VkpPending *w)
{
    vkFreeCommandBuffers(f->d->dev, f->d->pool, 1, &w->cmd);
    vkDestroyBuffer(f->d->dev, w->staging, NULL);
    vkFreeMemory(f->d->dev, w->staging_memory, NULL);
    memset(w, 0, sizeof(*w));
}

void vkp_frame_free(VkpFrame *f)
{
    if (!f) {
        return;
    }
    VkDevice dev = f->d->dev;
    (void)vkQueueWaitIdle(f->d->queue);
    for (uint32_t i = 0; i < f->n_pending; i++) {
        free_pending(f, &f->pending[i]);
    }
    for (uint32_t i = 0; i < 3u; i++) {
        vkDestroyImage(dev, f->plane[i].image, NULL);
        vkDestroyBuffer(dev, f->plane[i].buffer, NULL);
        vkFreeMemory(dev, f->plane[i].memory, NULL);
    }
    vkDestroySemaphore(dev, f->timeline, NULL);
    vkDestroySemaphore(dev, f->binary, NULL);
    free(f);
}

/* ---- Writes --------------------------------------------------------------------------- */

static uint64_t packed_bytes(const VkpFrame *f)
{
    uint64_t total = 0;
    for (uint32_t i = 0; i < f->n_planes; i++) {
        total += (uint64_t)f->plane[i].w * f->plane[i].texel * f->plane[i].h;
    }
    return total;
}

/* A mapped host buffer holding `bytes` of `src`. */
static bool make_staging(VkpFrame *f, VkpPending *w, const uint8_t *src, uint64_t bytes)
{
    const VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                     .size = bytes,
                                     .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                     .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    if (vkCreateBuffer(f->d->dev, &info, NULL, &w->staging) != VK_SUCCESS) {
        return false;
    }
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(f->d->dev, w->staging, &req);
    const VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                     .allocationSize = req.size,
                                     .memoryTypeIndex = host_type(f->d, req.memoryTypeBits)};
    void *map = NULL;
    const bool ok =
        ai.memoryTypeIndex != UINT32_MAX &&
        vkAllocateMemory(f->d->dev, &ai, NULL, &w->staging_memory) == VK_SUCCESS &&
        vkBindBufferMemory(f->d->dev, w->staging, w->staging_memory, 0) == VK_SUCCESS &&
        vkMapMemory(f->d->dev, w->staging_memory, 0, VK_WHOLE_SIZE, 0, &map) == VK_SUCCESS;
    if (ok) {
        memcpy(map, src, bytes);
        vkUnmapMemory(f->d->dev, w->staging_memory);
    }
    return ok;
}

/* Barrier of plane `p` from the queue's family to the external one (release)
 * or back (acquire, `to_device`). */
static void plane_barrier(const VkpFrame *f, VkCommandBuffer cmd, const VkpPlane *p, bool to_device)
{
    const uint32_t external = VK_QUEUE_FAMILY_EXTERNAL;
    const bool first = to_device && !f->external;
    const uint32_t src_family =
        to_device ? (first ? VK_QUEUE_FAMILY_IGNORED : external) : f->d->family;
    const uint32_t dst_family =
        to_device ? (first ? VK_QUEUE_FAMILY_IGNORED : f->d->family) : external;
    if (p->image) {
        const VkImageMemoryBarrier b = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = to_device ? 0u : VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = to_device ? VK_ACCESS_TRANSFER_WRITE_BIT : 0u,
            .oldLayout = to_device ? (first ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL) :
                                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = to_device ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = src_family,
            .dstQueueFamilyIndex = dst_family,
            .image = p->image,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
        return;
    }
    const VkBufferMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                                     .srcAccessMask = to_device ? 0u : VK_ACCESS_TRANSFER_WRITE_BIT,
                                     .dstAccessMask = to_device ? VK_ACCESS_TRANSFER_WRITE_BIT : 0u,
                                     .srcQueueFamilyIndex = src_family,
                                     .dstQueueFamilyIndex = dst_family,
                                     .buffer = p->buffer,
                                     .size = VK_WHOLE_SIZE};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 1, &b, 0, NULL);
}

/* Copy one packed plane from the staging buffer at `src` into plane `p`. */
static void copy_plane(VkCommandBuffer cmd, VkBuffer staging, uint64_t src, const VkpPlane *p)
{
    const uint64_t row = (uint64_t)p->w * p->texel;
    if (p->image) {
        const VkBufferImageCopy c = {.bufferOffset = src,
                                     .bufferRowLength = p->w,
                                     .bufferImageHeight = p->h,
                                     .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                     .imageExtent = {p->w, p->h, 1u}};
        vkCmdCopyBufferToImage(cmd, staging, p->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        return;
    }
    for (uint32_t y = 0; y < p->h; y++) {
        const VkBufferCopy c = {.srcOffset = src + y * row, .dstOffset = y * p->pitch, .size = row};
        vkCmdCopyBuffer(cmd, staging, p->buffer, 1, &c);
    }
}

/* The device's two scratch buffers of the busy work. */
static bool busy_buffers(VkpDevice *d)
{
    for (uint32_t i = 0; i < 2u && !d->busy[i]; i++) {
        const VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                         .size = VKP_BUSY_BYTES,
                                         .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                                  VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                         .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        VkMemoryRequirements req;
        if (vkCreateBuffer(d->dev, &info, NULL, &d->busy[i]) != VK_SUCCESS) {
            return false;
        }
        vkGetBufferMemoryRequirements(d->dev, d->busy[i], &req);
        const VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                         .allocationSize = req.size,
                                         .memoryTypeIndex = memory_type(d, req.memoryTypeBits)};
        if (ai.memoryTypeIndex == UINT32_MAX ||
            vkAllocateMemory(d->dev, &ai, NULL, &d->busy_memory[i]) != VK_SUCCESS ||
            vkBindBufferMemory(d->dev, d->busy[i], d->busy_memory[i], 0) != VK_SUCCESS) {
            return false;
        }
    }
    return true;
}

/* `n` scratch copies, each behind the last (the write's delay). */
static void record_busy(const VkpDevice *d, VkCommandBuffer cmd, uint32_t n)
{
    const VkBufferCopy c = {.srcOffset = 0, .dstOffset = 0, .size = VKP_BUSY_BYTES};
    const VkMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                               .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                               .dstAccessMask =
                                   VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT};
    for (uint32_t i = 0; i < n && i < VKP_BUSY_MAX; i++) {
        vkCmdCopyBuffer(cmd, d->busy[i % 2u], d->busy[(i + 1u) % 2u], 1, &c);
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                             1, &b, 0, NULL, 0, NULL);
    }
}

static bool record_write(VkpFrame *f, VkpPending *w, uint32_t busy)
{
    const VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                            .commandPool = f->d->pool,
                                            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                            .commandBufferCount = 1};
    const VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                         .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    if (vkAllocateCommandBuffers(f->d->dev, &ai, &w->cmd) != VK_SUCCESS ||
        vkBeginCommandBuffer(w->cmd, &bi) != VK_SUCCESS) {
        return false;
    }
    if (busy) {
        record_busy(f->d, w->cmd, busy);
    }
    uint64_t src = 0;
    for (uint32_t i = 0; i < f->n_planes; i++) {
        const VkpPlane *const p = &f->plane[i];
        plane_barrier(f, w->cmd, p, true);
        copy_plane(w->cmd, w->staging, src, p);
        plane_barrier(f, w->cmd, p, false);
        src += (uint64_t)p->w * p->texel * p->h;
    }
    return vkEndCommandBuffer(w->cmd) == VK_SUCCESS;
}

static bool submit_write(VkpFrame *f, const VkpPending *w, const VkpWrite *wr)
{
    VkSemaphore waits[2];
    uint64_t wait_values[2];
    VkPipelineStageFlags stages[2] = {VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                      VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
    uint32_t n_wait = 0;
    if (wr->wait_gate) {
        waits[n_wait] = f->d->gate;
        wait_values[n_wait++] = wr->wait_gate;
    }
    if (wr->wait_self) {
        waits[n_wait] = f->timeline;
        wait_values[n_wait++] = wr->wait_self;
    }
    const VkSemaphore signals[2] = {f->timeline, f->binary};
    const uint64_t signal_values[2] = {wr->signal, 0u};
    const VkTimelineSemaphoreSubmitInfo tl = {.sType =
                                                  VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
                                              .waitSemaphoreValueCount = n_wait,
                                              .pWaitSemaphoreValues = wait_values,
                                              .signalSemaphoreValueCount = f->binary ? 2u : 1u,
                                              .pSignalSemaphoreValues = signal_values};
    const VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                             .pNext = &tl,
                             .waitSemaphoreCount = n_wait,
                             .pWaitSemaphores = waits,
                             .pWaitDstStageMask = stages,
                             .commandBufferCount = 1,
                             .pCommandBuffers = &w->cmd,
                             .signalSemaphoreCount = f->binary ? 2u : 1u,
                             .pSignalSemaphores = signals};
    return vkQueueSubmit(f->d->queue, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS;
}

/* Free the writes that finished (their staging and command buffers). */
static void recycle(VkpFrame *f)
{
    if (f->n_pending < VKP_MAX_WRITES) {
        return;
    }
    /* Every write of a full ring signalled a value at most the latest; wait
     * for the frame's queue to drain them (no gate is held by then in the
     * tests: they open each gate before the next write). */
    (void)vkQueueWaitIdle(f->d->queue);
    for (uint32_t i = 0; i < f->n_pending; i++) {
        free_pending(f, &f->pending[i]);
    }
    f->n_pending = 0;
}

int vkp_frame_write(VkpFrame *f, const VkpWrite *w)
{
    recycle(f);
    if (f->binary && f->binary_pending) {
        const int stale = vkp_frame_sync_file(f); /* the last write's, never collected */
        if (stale >= 0) {
            (void)close(stale);
        }
    }
    VkpPending *const p = &f->pending[f->n_pending];
    memset(p, 0, sizeof(*p));
    const bool ok = (!w->busy || busy_buffers(f->d)) &&
                    make_staging(f, p, w->packed, packed_bytes(f)) && record_write(f, p, w->busy) &&
                    submit_write(f, p, w);
    if (!ok) {
        free_pending(f, p);
        return -1;
    }
    f->n_pending++;
    f->external = true;
    f->binary_pending = f->binary != VK_NULL_HANDLE;
    return 0;
}

int vkp_frame_wait(VkpFrame *f, uint64_t value, uint64_t timeout_ns)
{
    const VkSemaphoreWaitInfo info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
                                      .semaphoreCount = 1,
                                      .pSemaphores = &f->timeline,
                                      .pValues = &value};
    const VkResult r = vkWaitSemaphores(f->d->dev, &info, timeout_ns);
    return r == VK_SUCCESS ? 1 : r == VK_TIMEOUT ? 0 : -1;
}

int vkp_frame_signal_host(VkpFrame *f, uint64_t value)
{
    const VkSemaphoreSignalInfo info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, .semaphore = f->timeline, .value = value};
    return vkSignalSemaphore(f->d->dev, &info) == VK_SUCCESS ? 0 : -1;
}

uint64_t vkp_frame_value(VkpFrame *f)
{
    uint64_t v = 0;
    return vkGetSemaphoreCounterValue(f->d->dev, f->timeline, &v) == VK_SUCCESS ? v : UINT64_MAX;
}

int vkp_frame_sync_file(VkpFrame *f)
{
    if (!f->binary || !f->binary_pending) {
        return -1;
    }
    const VkSemaphoreGetFdInfoKHR info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
                                          .semaphore = f->binary,
                                          .handleType =
                                              VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT};
    int fd = -1;
    f->binary_pending = false;
    return f->d->fn.get_semaphore_fd(f->d->dev, &info, &fd) == VK_SUCCESS ? fd : -1;
}

/* ---- Descriptors ---------------------------------------------------------------------- */

static int export_memory(const VkpFrame *f, const VkpPlane *p)
{
    const VkMemoryGetFdInfoKHR info = {.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
                                       .memory = p->memory,
                                       .handleType = handle_type(f)};
    int fd = -1;
    return f->d->fn.get_memory_fd(f->d->dev, &info, &fd) == VK_SUCCESS ? fd : -1;
}

int vkp_frame_fence(VkpFrame *f, uint64_t value, VmafxFence *fence)
{
    const VkSemaphoreGetFdInfoKHR info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
                                          .semaphore = f->timeline,
                                          .handleType =
                                              VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT};
    int fd = -1;
    if (f->d->fn.get_semaphore_fd(f->d->dev, &info, &fd) != VK_SUCCESS) {
        return -1;
    }
    const VmafxFence init = VMAFX_FENCE_INIT;
    *fence = init;
    fence->kind = VMAFX_FENCE_VULKAN_SEMAPHORE;
    fence->fd = fd;
    fence->value = value;
    return 0;
}

static uint32_t vulkan_tiling(const VkpFrame *f)
{
    switch (f->desc.layout) {
    case VKP_IMAGE_OPTIMAL:
        return VMAFX_VULKAN_TILING_OPTIMAL;
    case VKP_IMAGE_DRM:
        return VMAFX_VULKAN_TILING_DRM_FORMAT_MODIFIER;
    default:
        return VMAFX_VULKAN_TILING_LINEAR; /* linear images and buffers */
    }
}

int vkp_frame_describe(VkpFrame *f, uint64_t wait, VmafxFrameImport *imp)
{
    const VmafxFrameImport init = VMAFX_FRAME_IMPORT_INIT;
    *imp = init;
    imp->memory = VMAFX_MEMORY_VULKAN;
    imp->pix_fmt = f->desc.pix_fmt;
    imp->bpc = f->desc.bpc;
    imp->w = f->desc.w;
    imp->h = f->desc.h;
    imp->n_planes = f->n_planes;
    for (uint32_t i = 0; i < 3u; i++) {
        imp->plane[i].fd = -1;
    }
    imp->vulkan_handle_type =
        f->desc.dma_buf ? VMAFX_VULKAN_HANDLE_DMA_BUF : VMAFX_VULKAN_HANDLE_OPAQUE_FD;
    imp->vulkan_tiling = vulkan_tiling(f);
    imp->vulkan_flags = f->plane[0].dedicated ? VMAFX_VULKAN_DEDICATED : 0u;
    memcpy(imp->vulkan_pci, f->d->pci, sizeof(imp->vulkan_pci));
    bool ok = true;
    for (uint32_t i = 0; i < f->n_planes; i++) {
        const VkpPlane *const p = &f->plane[i];
        assert(p->dedicated == f->plane[0].dedicated);
        imp->plane[i].fd = export_memory(f, p);
        imp->plane[i].size = p->size;
        imp->plane[i].offset = p->offset;
        imp->plane[i].pitch = p->pitch;
        imp->plane[i].modifier = p->modifier;
        ok = ok && imp->plane[i].fd >= 0;
    }
    if (ok && wait) {
        ok = vkp_frame_fence(f, wait, &imp->acquire) == 0;
    }
    if (!ok) {
        vkp_import_close(imp);
    }
    return ok ? 0 : -1;
}

void vkp_import_close(VmafxFrameImport *imp)
{
    const uint32_t n = imp->n_planes < 3u ? imp->n_planes : 3u;
    for (uint32_t i = 0; i < n; i++) {
        if (imp->plane[i].fd >= 0) {
            (void)close(imp->plane[i].fd);
        }
        imp->plane[i].fd = -1;
    }
    if (imp->acquire.kind == VMAFX_FENCE_VULKAN_SEMAPHORE && imp->acquire.fd >= 0) {
        (void)close(imp->acquire.fd);
        imp->acquire.fd = -1;
    }
}

/* NOLINTEND(modernize-use-nullptr) */
