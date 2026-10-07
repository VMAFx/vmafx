/*
 * Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * VulkanImage buffers (GStreamer's Vulkan decoders and uploader) scored on the VMAFx device of the
 * same GPU, with one copy on the GPU and no copy to the host.
 *
 * GStreamer allocates its image memory without export information and its decoders write one
 * multi-plane image per frame, and the library imports one exported image per plane. So each frame
 * is copied once, on the Vulkan device of the buffer, into a slot of the element's own pool of
 * exportable per-plane images, and the slot's memory is imported as VMAFX_MEMORY_VULKAN. The
 * copy is waited for on the host (the copy is a GPU operation, not a download), so the import
 * needs no acquire fence; the library's HOST release fence says when a slot may be written again.
 * The Vulkan functions come through GStreamer's own proc-address lookups, so the plug-in links no
 * Vulkan loader. The VMAFx device is the one at the Vulkan device's PCI location: frames are never
 * read across GPUs.
 */

#include "gstvmafx.h"

#define GST_CAT_DEFAULT gst_vmafx_debug

#ifndef HAVE_GST_VULKAN

gboolean gst_vmafx_vulkan_built(void)
{
    return FALSE;
}

gboolean gst_vmafx_vulkan_probe(GstBuffer *buffer, uint32_t pci[4], uint32_t *vendor)
{
    (void)buffer;
    (void)pci;
    (void)vendor;
    return FALSE;
}

gboolean gst_vmafx_vulkan_stage(GstVmafx *self, guint pad, GstBuffer **buffer, gchar **error)
{
    (void)self;
    (void)buffer;
    *error = g_strdup_printf("pad %s: this build has no GStreamer Vulkan support",
                             gst_vmafx_pad_name(pad));
    return FALSE;
}

VmafxStatus gst_vmafx_vulkan_import(GstVmafx *self, GstVmafxRt *rt, GstBuffer *buffer, guint pad,
                                    VmafxFrame **out, gchar **error)
{
    (void)self;
    (void)rt;
    (void)buffer;
    (void)out;
    *error = g_strdup_printf("pad %s: this build has no GStreamer Vulkan support",
                             gst_vmafx_pad_name(pad));
    return VMAFX_E_NOTSUP;
}

void gst_vmafx_vulkan_free_pools(GstVmafx *self)
{
    (void)self;
}

gboolean gst_vmafx_vulkan_context_query(GstVmafx *self, GstAggregatorPad *pad, GstQuery *query)
{
    (void)self;
    (void)pad;
    (void)query;
    return FALSE;
}

void gst_vmafx_vulkan_context_free(GstVmafx *self)
{
    (void)self;
}

#else /* HAVE_GST_VULKAN */

#include <gst/vulkan/vulkan.h>
#include <string.h>
#include <unistd.h>

/* Slots wait this long for the library to let go of a frame. */
#define SLOT_WAIT_US (10 * G_USEC_PER_SEC)

#define INSTANCE_FNS(X)                                                                            \
    X(GetPhysicalDeviceMemoryProperties)                                                           \
    X(GetPhysicalDeviceProperties2)                                                                \
    X(GetPhysicalDeviceImageFormatProperties2)

#define DEVICE_FNS(X)                                                                              \
    X(CreateImage)                                                                                 \
    X(DestroyImage)                                                                                \
    X(GetImageMemoryRequirements2)                                                                 \
    X(AllocateMemory)                                                                              \
    X(FreeMemory)                                                                                  \
    X(BindImageMemory)                                                                             \
    X(GetImageSubresourceLayout)                                                                   \
    X(GetMemoryFdKHR)                                                                              \
    X(CmdCopyImage)                                                                                \
    X(WaitSemaphores)

typedef struct {
#define DECLARE(name) PFN_vk##name name;
    INSTANCE_FNS(DECLARE)
    DEVICE_FNS(DECLARE)
#undef DECLARE
} VkFns;

typedef struct {
    VkImage image;
    VkDeviceMemory memory;
    VkDeviceSize size;
    uint64_t offset;
    uint64_t pitch; /* linear images only */
    gboolean dedicated;
} Plane;

typedef struct Pool Pool;

typedef struct {
    Pool *pool;
    Plane plane[3];
    gboolean busy;
    VmafxFence release; /* HOST fence of the frame that holds the slot */
} Slot;

struct Pool {
    GstVulkanDevice *device;
    GstVulkanQueue *queue;
    GstVulkanCommandPool *cmd_pool;
    GstVulkanOperation *op;
    VkDevice dev;
    VkPhysicalDevice phys;
    VkFns fn;
    const GstVmafxFormat *fmt;
    guint n_planes;
    VkFormat vk_format[3];
    guint pw[3];
    guint ph[3];
    VkImageTiling tiling;
    uint32_t pci[4];
    GMutex lock;
    GCond cond;
    Slot *slot;
    guint n_slots;
    guint next; /* the slot looked at first: slots are used in turn, not first free */
};

static gboolean load_fns(Pool *p)
{
    GstVulkanInstance *inst = gst_vulkan_device_get_instance(p->device);
    gboolean ok = TRUE;
#define LOAD_INSTANCE(name)                                                                        \
    p->fn.name = (PFN_vk##name)gst_vulkan_instance_get_proc_address(inst, "vk" #name);             \
    ok = ok && p->fn.name != NULL;
    INSTANCE_FNS(LOAD_INSTANCE)
#undef LOAD_INSTANCE
#define LOAD_DEVICE(name)                                                                          \
    p->fn.name = (PFN_vk##name)gst_vulkan_device_get_proc_address(p->device, "vk" #name);          \
    if (p->fn.name == NULL) {                                                                      \
        GST_INFO("Vulkan device function vk" #name " is not available");                           \
    }                                                                                              \
    ok = ok && p->fn.name != NULL;
    DEVICE_FNS(LOAD_DEVICE)
#undef LOAD_DEVICE
    gst_object_unref(inst);
    return ok;
}

/* The VkPhysicalDevice's PCI location and vendor. */
static gboolean device_pci(Pool *p, uint32_t pci[4], uint32_t *vendor)
{
    VkPhysicalDevicePCIBusInfoPropertiesEXT bus = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT};
    VkPhysicalDeviceProperties2 props = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                         .pNext = &bus};
    p->fn.GetPhysicalDeviceProperties2(p->phys, &props);
    pci[0] = bus.pciDomain;
    pci[1] = bus.pciBus;
    pci[2] = bus.pciDevice;
    pci[3] = bus.pciFunction;
    *vendor = props.properties.vendorID;
    return bus.pciBus != 0 || bus.pciDevice != 0 || bus.pciDomain != 0 || bus.pciFunction != 0;
}

gboolean gst_vmafx_vulkan_built(void)
{
    return TRUE;
}

/* The image memory of the buffer's first plane, or NULL. */
static GstVulkanImageMemory *image_memory(GstBuffer *buffer, guint i)
{
    GstMemory *mem = i < gst_buffer_n_memory(buffer) ? gst_buffer_peek_memory(buffer, i) : NULL;
    return mem != NULL && gst_is_vulkan_image_memory(mem) ? (GstVulkanImageMemory *)mem : NULL;
}

/* The PCI location and vendor of the GPU the buffer's images live on. */
gboolean gst_vmafx_vulkan_probe(GstBuffer *buffer, uint32_t pci[4], uint32_t *vendor)
{
    GstVulkanImageMemory *mem = image_memory(buffer, 0);
    if (mem == NULL) {
        return FALSE;
    }
    Pool p = {.device = mem->device};
    p.phys = gst_vulkan_device_get_physical_device(mem->device);
    if (!load_fns(&p)) {
        return FALSE;
    }
    return device_pci(&p, pci, vendor);
}

/* Width and height of plane `i` (chroma planes are halved as the format says). */
static void plane_size(const Pool *p, const GstVideoInfo *info, guint i, guint *w, guint *h)
{
    const guint width = (guint)GST_VIDEO_INFO_WIDTH(info);
    const guint height = (guint)GST_VIDEO_INFO_HEIGHT(info);
    const uint32_t fmt = p->fmt->pix_fmt;
    const gboolean half_w = i > 0 && fmt != VMAFX_PIXEL_FORMAT_YUV444P;
    const gboolean half_h = half_w && fmt != VMAFX_PIXEL_FORMAT_YUV422P;
    *w = half_w ? (width + 1) / 2 : width;
    *h = half_h ? (height + 1) / 2 : height;
}

/* Geometry and Vulkan format of each plane. */
static void plan_planes(Pool *p, const GstVideoInfo *info)
{
    const gboolean wide = p->fmt->bpc > 8;
    p->n_planes = p->fmt->semi_planar ? 2 : 3;
    for (guint i = 0; i < p->n_planes; i++) {
        const gboolean pair = i > 0 && p->fmt->semi_planar; /* interleaved Cb/Cr */
        plane_size(p, info, i, &p->pw[i], &p->ph[i]);
        p->vk_format[i] = wide ? (pair ? VK_FORMAT_R16G16_UNORM : VK_FORMAT_R16_UNORM) :
                                 (pair ? VK_FORMAT_R8G8_UNORM : VK_FORMAT_R8_UNORM);
    }
}

static uint32_t memory_type_with(Pool *p, uint32_t bits, VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties mp;
    p->fn.GetPhysicalDeviceMemoryProperties(p->phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) {
            return i;
        }
    }
    return UINT32_MAX;
}

static uint32_t memory_type(Pool *p, uint32_t bits)
{
    return memory_type_with(p, bits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
}

/* Whether the driver exports an image of `format`, and whether it wants a dedicated allocation. */
static gboolean exportable(Pool *p, guint i, gboolean *dedicated_only)
{
    VkPhysicalDeviceExternalImageFormatInfo ext = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
    const VkPhysicalDeviceImageFormatInfo2 info = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
        .pNext = &ext,
        .format = p->vk_format[i],
        .type = VK_IMAGE_TYPE_2D,
        .tiling = p->tiling,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    VkExternalImageFormatProperties eprops = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
    VkImageFormatProperties2 props = {.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
                                      .pNext = &eprops};
    const VkResult r = p->fn.GetPhysicalDeviceImageFormatProperties2(p->phys, &info, &props);
    const VkExternalMemoryFeatureFlags feat =
        eprops.externalMemoryProperties.externalMemoryFeatures;
    *dedicated_only = (feat & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT) != 0;
    return r == VK_SUCCESS && (feat & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) &&
           props.imageFormatProperties.maxExtent.width >= p->pw[i] &&
           props.imageFormatProperties.maxExtent.height >= p->ph[i];
}

static gboolean create_image(Pool *p, guint i, Plane *pl)
{
    VkExternalMemoryImageCreateInfo ext = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
    const VkImageCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                    .pNext = &ext,
                                    .imageType = VK_IMAGE_TYPE_2D,
                                    .format = p->vk_format[i],
                                    .extent = {p->pw[i], p->ph[i], 1},
                                    .mipLevels = 1,
                                    .arrayLayers = 1,
                                    .samples = VK_SAMPLE_COUNT_1_BIT,
                                    .tiling = p->tiling,
                                    .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                             VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    return p->fn.CreateImage(p->dev, &info, NULL, &pl->image) == VK_SUCCESS;
}

/* Allocate the image's exportable memory and bind it. */
static gboolean bind_memory(Pool *p, Plane *pl, gboolean dedicated_only)
{
    VkMemoryDedicatedRequirements dreq = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    VkMemoryRequirements2 req = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, .pNext = &dreq};
    const VkImageMemoryRequirementsInfo2 ri = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2, .image = pl->image};
    p->fn.GetImageMemoryRequirements2(p->dev, &ri, &req);
    const gboolean dedicated =
        dedicated_only || dreq.requiresDedicatedAllocation || dreq.prefersDedicatedAllocation;
    VkMemoryDedicatedAllocateInfo ded = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
                                         .image = pl->image};
    VkExportMemoryAllocateInfo exp = {.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
                                      .pNext = dedicated ? &ded : NULL,
                                      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
    const VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                     .pNext = &exp,
                                     .allocationSize = req.memoryRequirements.size,
                                     .memoryTypeIndex =
                                         memory_type(p, req.memoryRequirements.memoryTypeBits)};
    pl->size = req.memoryRequirements.size;
    pl->dedicated = dedicated;
    return ai.memoryTypeIndex != UINT32_MAX &&
           p->fn.AllocateMemory(p->dev, &ai, NULL, &pl->memory) == VK_SUCCESS &&
           p->fn.BindImageMemory(p->dev, pl->image, pl->memory, 0) == VK_SUCCESS;
}

static void read_layout(Pool *p, Plane *pl)
{
    if (p->tiling == VK_IMAGE_TILING_LINEAR) {
        const VkImageSubresource sub = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT};
        VkSubresourceLayout layout;
        p->fn.GetImageSubresourceLayout(p->dev, pl->image, &sub, &layout);
        pl->offset = layout.offset;
        pl->pitch = layout.rowPitch;
    }
}

static gboolean make_plane(Pool *p, guint i, Plane *pl)
{
    gboolean dedicated_only = FALSE;
    if (!exportable(p, i, &dedicated_only) || !create_image(p, i, pl) ||
        !bind_memory(p, pl, dedicated_only)) {
        return FALSE;
    }
    read_layout(p, pl);
    return TRUE;
}

static void destroy_plane(Pool *p, Plane *pl)
{
    if (pl->image != VK_NULL_HANDLE) {
        p->fn.DestroyImage(p->dev, pl->image, NULL);
    }
    if (pl->memory != VK_NULL_HANDLE) {
        p->fn.FreeMemory(p->dev, pl->memory, NULL);
    }
    memset(pl, 0, sizeof(*pl));
}

static void pool_free(Pool *p)
{
    if (p == NULL) {
        return;
    }
    for (guint s = 0; s < p->n_slots; s++) {
        for (guint i = 0; i < p->n_planes; i++) {
            destroy_plane(p, &p->slot[s].plane[i]);
        }
    }
    g_free(p->slot);
    gst_clear_object(&p->op);
    gst_clear_object(&p->cmd_pool);
    gst_clear_object(&p->queue);
    gst_clear_object(&p->device);
    g_mutex_clear(&p->lock);
    g_cond_clear(&p->cond);
    g_free(p);
}

void gst_vmafx_vulkan_free_pools(GstVmafx *self)
{
    for (guint i = 0; i < 2; i++) {
        pool_free(self->vk_pool[i]);
        self->vk_pool[i] = NULL;
    }
}

static gboolean make_slots(Pool *p, guint n)
{
    p->slot = g_new0(Slot, n);
    p->n_slots = n;
    for (guint s = 0; s < n; s++) {
        p->slot[s].pool = p;
        p->slot[s].release = (VmafxFence)VMAFX_FENCE_INIT;
        for (guint i = 0; i < p->n_planes; i++) {
            if (!make_plane(p, i, &p->slot[s].plane[i])) {
                return FALSE;
            }
        }
    }
    return TRUE;
}

static gboolean can_export(const Pool *p, gchar **error)
{
    if (gst_vulkan_device_is_extension_enabled(p->device, "VK_KHR_external_memory_fd")) {
        return TRUE;
    }
    *error = g_strdup("the Vulkan device of these frames cannot export memory (it was not made by "
                      "this element: link the element directly downstream of the Vulkan element, "
                      "or share a device with VK_KHR_external_memory_fd)");
    return FALSE;
}

/* The device side of a pool: export extension, a graphics queue with an operation to record the
 * copy on, the function table and the PCI location. */
static gboolean pool_open(Pool *p, uint32_t *vendor, gchar **error)
{
    GError *err = NULL;
    if (!can_export(p, error)) {
        return FALSE;
    }
    p->queue = gst_vulkan_device_select_queue(p->device, VK_QUEUE_GRAPHICS_BIT);
    p->cmd_pool = p->queue ? gst_vulkan_queue_create_command_pool(p->queue, &err) : NULL;
    p->op = p->cmd_pool ? gst_vulkan_operation_new(p->cmd_pool) : NULL;
    if (p->fmt == NULL || !load_fns(p) || !device_pci(p, p->pci, vendor) || p->op == NULL ||
        !gst_vulkan_operation_use_sync2(p->op)) {
        *error = g_strdup_printf("the Vulkan device offers no queue, command pool or "
                                 "synchronization2 for the copy%s%s",
                                 err ? ": " : "", err ? err->message : "");
        g_clear_error(&err);
        return FALSE;
    }
    return TRUE;
}

/* The pool of one pad: slots of exportable per-plane images on the buffer's device. */
static Pool *pool_new(GstVmafx *self, guint pad, GstVulkanImageMemory *mem, gchar **error)
{
    Pool *p = g_new0(Pool, 1);
    uint32_t vendor = 0;
    const GstVideoInfo *info = &self->pad_info[pad].info;
    g_mutex_init(&p->lock);
    g_cond_init(&p->cond);
    p->device = gst_object_ref(mem->device);
    p->dev = mem->device->device;
    p->phys = gst_vulkan_device_get_physical_device(mem->device);
    p->fmt = gst_vmafx_format_lookup(GST_VIDEO_INFO_FORMAT(info));
    if (!pool_open(p, &vendor, error)) {
        pool_free(p);
        return NULL;
    }
    /* Exportable images in the layout the VMAFx device of this GPU reads. */
    p->tiling = gst_vmafx_backend_for_vulkan(&self->opts, vendor) == VMAFX_BACKEND_CUDA ?
                    VK_IMAGE_TILING_OPTIMAL :
                    VK_IMAGE_TILING_LINEAR;
    plan_planes(p, info);
    /* The library holds at most R + 2T(R+1) + 1 frames of an input (R <= 2 earlier references,
     * T worker threads) and the frame being copied: that many slots never wait. */
    if (!make_slots(p, 8 + 6 * (guint)CLAMP(self->opts.threads, 0, 64))) {
        *error = g_strdup_printf("the driver exports no %s image of this size for the VMAFx device",
                                 p->tiling == VK_IMAGE_TILING_LINEAR ? "linear" : "optimal");
        pool_free(p);
        return NULL;
    }
    return p;
}

/* A free slot, taken in turn so that a slot is rewritten as late as possible; waits for the
 * library to let go of one. */
static Slot *take_slot(Pool *p)
{
    const gint64 end = g_get_monotonic_time() + SLOT_WAIT_US;
    Slot *found = NULL;
    g_mutex_lock(&p->lock);
    while (found == NULL) {
        for (guint k = 0; k < p->n_slots && found == NULL; k++) {
            Slot *candidate = &p->slot[(p->next + k) % p->n_slots];
            found = candidate->busy ? NULL : candidate;
        }
        if (found == NULL && !g_cond_wait_until(&p->cond, &p->lock, end)) {
            break;
        }
    }
    if (found != NULL) {
        found->busy = TRUE;
        p->next = (guint)(found - p->slot + 1) % p->n_slots;
    }
    g_mutex_unlock(&p->lock);
    return found;
}

static void put_slot(Slot *slot)
{
    g_mutex_lock(&slot->pool->lock);
    slot->busy = FALSE;
    g_cond_signal(&slot->pool->cond);
    g_mutex_unlock(&slot->pool->lock);
}

/* The library let go of the frame: wait for the device's last read, then free the slot. */
static void slot_release(void *user)
{
    Slot *slot = user;
    VmafxError *err = NULL;
    if (slot->release.kind == VMAFX_FENCE_HOST) {
        const VmafxStatus waited = vmafx_fence_wait(&slot->release, UINT64_MAX, &err);
        if (waited != VMAFX_OK) {
            GST_WARNING("release fence wait: %s: %s", vmafx_status_name(waited),
                        vmafx_error_message(err));
        }
        vmafx_error_free(err);
        err = NULL;
        (void)vmafx_fence_destroy(&slot->release, &err);
        vmafx_error_free(err);
    }
    slot->release = (VmafxFence)VMAFX_FENCE_INIT;
    put_slot(slot);
}

static VkImageMemoryBarrier2 slot_barrier(const Pool *p, const Plane *pl, VkImageLayout from,
                                          VkImageLayout to, gboolean release)
{
    VkImageMemoryBarrier2 b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
        .oldLayout = from,
        .newLayout = to,
        .srcQueueFamilyIndex = release ? p->queue->family : VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = release ? VK_QUEUE_FAMILY_EXTERNAL : VK_QUEUE_FAMILY_IGNORED,
        .image = pl->image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    return b;
}

static gboolean slot_barriers(Pool *p, Slot *slot, VkImageLayout to, gboolean release)
{
    VkImageMemoryBarrier2 b[3];
    for (guint i = 0; i < p->n_planes; i++) {
        b[i] =
            slot_barrier(p, &slot->plane[i],
                         release ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                         to, release);
    }
    VkDependencyInfo dep = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                            .imageMemoryBarrierCount = p->n_planes,
                            .pImageMemoryBarriers = b};
    return gst_vulkan_operation_pipeline_barrier2(p->op, &dep);
}

/* Record the copy of every plane of `buffer` into `slot`. */
static gboolean record_copies(Pool *p, GstBuffer *buffer, Slot *slot)
{
    const guint n_mem = gst_buffer_n_memory(buffer);
    VkCommandBuffer cmd = p->op->cmd_buf->cmd;
    for (guint i = 0; i < p->n_planes; i++) {
        GstVulkanImageMemory *mem = image_memory(buffer, n_mem == 1 ? 0 : i);
        if (mem == NULL) {
            return FALSE;
        }
        const VkImageCopy region = {
            .srcSubresource = {n_mem == 1 ? (VkImageAspectFlags)(VK_IMAGE_ASPECT_PLANE_0_BIT << i) :
                                            VK_IMAGE_ASPECT_COLOR_BIT,
                               0, 0, 1},
            .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .extent = {p->pw[i], p->ph[i], 1}};
        p->fn.CmdCopyImage(cmd, mem->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           slot->plane[i].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    }
    return TRUE;
}

/* Barriers moving the source images from the layout GStreamer tracks to a transfer source layout.
 * The producer's stages (video decode, say) may not exist on this queue: the producer is waited
 * for on the host, so the barrier's source is all commands. */
static guint source_barriers(GstBuffer *buffer, VkImageMemoryBarrier2 *out)
{
    const guint n_mem = MIN(gst_buffer_n_memory(buffer), 3u);
    for (guint i = 0; i < n_mem; i++) {
        GstVulkanImageMemory *mem = image_memory(buffer, i);
        out[i] = (VkImageMemoryBarrier2){.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                         .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                         .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                                         .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                         .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
                                         .oldLayout = mem->barrier.image_layout,
                                         .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                         .image = mem->image,
                                         .subresourceRange = mem->barrier.subresource_range};
    }
    return n_mem;
}

static gboolean record_source_barrier(Pool *p, GstBuffer *buffer)
{
    VkImageMemoryBarrier2 b[3];
    VkDependencyInfo dep = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .pImageMemoryBarriers = b};
    dep.imageMemoryBarrierCount = source_barriers(buffer, b);
    return gst_vulkan_operation_pipeline_barrier2(p->op, &dep);
}

/* The images are left in the transfer source layout: say so in the state GStreamer tracks, so its
 * next barrier on them (the decoder reusing them, a download) starts from where they are. */
static void track_source_layout(GstBuffer *buffer)
{
    for (guint i = 0; i < gst_buffer_n_memory(buffer); i++) {
        GstVulkanImageMemory *mem = image_memory(buffer, i);
        mem->barrier.image_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        mem->barrier.parent.pipeline_stages = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        mem->barrier.parent.access_flags = VK_ACCESS_2_TRANSFER_READ_BIT;
    }
}

/* Wait on the host for the producer of the buffer's images (their timeline semaphores). */
static gboolean wait_producer(Pool *p, GstBuffer *buffer)
{
    for (guint i = 0; i < gst_buffer_n_memory(buffer); i++) {
        GstVulkanImageMemory *mem = image_memory(buffer, i);
        if (mem == NULL) {
            return FALSE;
        }
        const guint64 value = mem->barrier.parent.semaphore_value;
        const VkSemaphoreWaitInfo wi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
                                        .semaphoreCount = 1,
                                        .pSemaphores = &mem->barrier.parent.semaphore,
                                        .pValues = &value};
        if (mem->barrier.parent.semaphore != VK_NULL_HANDLE &&
            p->fn.WaitSemaphores(p->dev, &wi, (uint64_t)SLOT_WAIT_US * 1000u) != VK_SUCCESS) {
            return FALSE;
        }
    }
    return TRUE;
}

/* Copy `buffer` into `slot` on the GPU and wait for the copy. */
/* The commands of the copy: the source to a transfer source layout, the slot to a transfer
 * destination, the plane copies, the slot released to the importer in the general layout. */
static gboolean record_copy(Pool *p, GstBuffer *buffer, Slot *slot)
{
    return record_source_barrier(p, buffer) &&
           slot_barriers(p, slot, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, FALSE) &&
           record_copies(p, buffer, slot) && slot_barriers(p, slot, VK_IMAGE_LAYOUT_GENERAL, TRUE);
}

static gboolean copy_into(Pool *p, GstBuffer *buffer, Slot *slot, gchar **error)
{
    GError *err = NULL;
    gboolean ok = wait_producer(p, buffer) && gst_vulkan_operation_begin(p->op, &err) &&
                  record_copy(p, buffer, slot) && gst_vulkan_operation_end(p->op, &err) &&
                  gst_vulkan_operation_wait(p->op);
    gst_vulkan_operation_reset(p->op);
    if (ok) {
        track_source_layout(buffer);
    } else {
        *error = g_strdup_printf("the copy of the Vulkan frame on the GPU failed%s%s",
                                 err ? ": " : "", err ? err->message : "");
    }
    g_clear_error(&err);
    return ok;
}

static void close_fds(VmafxFrameImport *imp)
{
    for (guint i = 0; i < 3; i++) {
        if (imp->plane[i].fd >= 0) {
            close(imp->plane[i].fd);
        }
    }
}

/* The descriptor of a copied slot: one exported image per plane. */
static gboolean describe(Pool *p, Slot *slot, VmafxFrameImport *imp)
{
    imp->memory = VMAFX_MEMORY_VULKAN;
    imp->pix_fmt = p->fmt->pix_fmt;
    imp->bpc = p->fmt->bpc;
    imp->w = p->pw[0];
    imp->h = p->ph[0];
    imp->n_planes = p->n_planes;
    imp->vulkan_handle_type = VMAFX_VULKAN_HANDLE_OPAQUE_FD;
    imp->vulkan_tiling = (uint32_t)p->tiling; /* the values equal VkImageTiling */
    memcpy(imp->vulkan_pci, p->pci, sizeof(imp->vulkan_pci));
    for (guint i = 0; i < 3; i++) {
        imp->plane[i].fd = -1;
    }
    for (guint i = 0; i < p->n_planes; i++) {
        const VkMemoryGetFdInfoKHR gi = {.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
                                         .memory = slot->plane[i].memory,
                                         .handleType =
                                             VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
        if (p->fn.GetMemoryFdKHR(p->dev, &gi, &imp->plane[i].fd) != VK_SUCCESS) {
            return FALSE;
        }
        imp->plane[i].size = slot->plane[i].size;
        imp->plane[i].offset = slot->plane[i].offset;
        imp->plane[i].pitch = slot->plane[i].pitch;
    }
    imp->vulkan_flags = 0;
    for (guint i = 0; i < p->n_planes; i++) {
        if (slot->plane[i].dedicated) {
            imp->vulkan_flags |= VMAFX_VULKAN_DEDICATED;
        }
    }
    return TRUE;
}

/* The pool of `pad`, made when its first frame arrives. */
static Pool *pool_of(GstVmafx *self, guint pad, GstBuffer *buffer, gchar **error)
{
    if (self->vk_pool[pad] == NULL) {
        GstVulkanImageMemory *mem = image_memory(buffer, 0);
        if (mem == NULL) {
            *error = g_strdup_printf("pad %s: the buffer holds no Vulkan image",
                                     gst_vmafx_pad_name(pad));
            return NULL;
        }
        gchar *why = NULL;
        self->vk_pool[pad] = pool_new(self, pad, mem, &why);
        if (self->vk_pool[pad] == NULL) {
            *error = g_strdup_printf("pad %s: %s", gst_vmafx_pad_name(pad), why);
            g_free(why);
        }
    }
    return self->vk_pool[pad];
}

/* The import of a copied slot; the frame owns the slot from a successful import on. */
static VmafxStatus import_slot(GstVmafxRt *rt, Pool *p, Slot *slot, guint pad, VmafxFrame **out,
                               gchar **error, gboolean *consumed)
{
    VmafxFrameImport imp = VMAFX_FRAME_IMPORT_INIT;
    VmafxError *err = NULL;
    if (!describe(p, slot, &imp)) {
        close_fds(&imp);
        *error =
            g_strdup_printf("pad %s: cannot export the copy's memory", gst_vmafx_pad_name(pad));
        return VMAFX_E_DEVICE;
    }
    if (p->tiling == VK_IMAGE_TILING_OPTIMAL && !p->fmt->semi_planar) {
        imp.flags |= VMAFX_IMPORT_ALLOW_COPY; /* planar CUDA arrays: the library's device copy */
    }
    imp.release = slot_release;
    imp.user = slot;
    VmafxStatus status = vmafx_context_import_frame(rt->context, rt->device, &imp,
                                                    gst_vmafx_pad_name(pad), out, &err);
    close_fds(&imp);
    if (status == VMAFX_OK) {
        *consumed = TRUE;
        status = vmafx_frame_release_fence(*out, VMAFX_FENCE_HOST, &slot->release, &err);
        if (status != VMAFX_OK) {
            vmafx_frame_unref(*out);
            *out = NULL;
        }
    }
    if (status != VMAFX_OK) {
        gchar *what = g_strdup_printf("pad %s: format %s", gst_vmafx_pad_name(pad), p->fmt->name);
        *error = gst_vmafx_status_text(what, status, err);
        g_free(what);
    }
    return status;
}

/* A slot nobody imported goes back to its pool with the buffer that carried it. */
static void staged_slot_free(gpointer data)
{
    put_slot(data);
}

static GQuark staged_quark(void)
{
    return g_quark_from_static_string("gst-vmafx-vulkan-staged-slot");
}

/* Copy the frame into a slot of the pad's pool on the GPU. This runs on the thread of the element
 * upstream (from the pad's clip function): the Vulkan queues of that element and of the copy are
 * then used by one thread at a time, which the Mesa Intel driver needs (two threads submitting
 * to one device corrupted its heap). The slot travels with the buffer. */
gboolean gst_vmafx_vulkan_stage(GstVmafx *self, guint pad, GstBuffer **buffer, gchar **error)
{
    g_assert(self != NULL && buffer != NULL && *buffer != NULL && error != NULL);

    Pool *p = pool_of(self, pad, *buffer, error);
    if (p == NULL) {
        return FALSE;
    }
    Slot *slot = take_slot(p);
    if (slot == NULL) {
        *error = g_strdup_printf("pad %s: no copy slot became free in 10 s: the library holds "
                                 "every frame",
                                 gst_vmafx_pad_name(pad));
        return FALSE;
    }
    if (!copy_into(p, *buffer, slot, error)) {
        put_slot(slot);
        return FALSE;
    }
    *buffer = gst_buffer_make_writable(*buffer);
    gst_mini_object_set_qdata(GST_MINI_OBJECT(*buffer), staged_quark(), slot, staged_slot_free);
    return TRUE;
}

/* Import the frame the pad's clip function copied; the frame owns the slot from then on. */
VmafxStatus gst_vmafx_vulkan_import(GstVmafx *self, GstVmafxRt *rt, GstBuffer *buffer, guint pad,
                                    VmafxFrame **out, gchar **error)
{
    g_assert(self != NULL && rt != NULL && buffer != NULL && out != NULL && error != NULL);
    Slot *slot = gst_mini_object_steal_qdata(GST_MINI_OBJECT(buffer), staged_quark());
    if (slot == NULL) {
        *error = g_strdup_printf("pad %s: the Vulkan frame was not copied on its way in",
                                 gst_vmafx_pad_name(pad));
        return VMAFX_E_INVALID;
    }
    gboolean consumed = FALSE;
    const VmafxStatus status = import_slot(rt, slot->pool, slot, pad, out, error, &consumed);
    if (!consumed) {
        put_slot(slot);
    }
    return status;
}

/* ---- The Vulkan device of the frames ------------------------------------------------------
 * GStreamer's own Vulkan devices enable no memory-export extension, and the memory of an image
 * cannot be exported afterwards. So the element offers the Vulkan elements upstream of it a device
 * of its own, with VK_KHR_external_memory_fd, as an element that owns the device does (a Vulkan
 * sink): they ask downstream for a "gst.vulkan.instance" and a "gst.vulkan.device" context. The
 * GPU is the one the asking element is made for (vulkanh264device1dec: index 1), else the first.
 */

typedef struct {
    GstVulkanInstance *instance;
    GHashTable *devices; /* GPU index -> GstVulkanDevice */
} VkContext;

static VkContext *vk_context(GstVmafx *self)
{
    if (self->vk_ctx == NULL) {
        VkContext *c = g_new0(VkContext, 1);
        c->devices = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, gst_object_unref);
        self->vk_ctx = c;
    }
    return self->vk_ctx;
}

void gst_vmafx_vulkan_context_free(GstVmafx *self)
{
    VkContext *c = self->vk_ctx;
    if (c != NULL) {
        g_hash_table_destroy(c->devices);
        gst_clear_object(&c->instance);
        g_free(c);
        self->vk_ctx = NULL;
    }
}

/* The GPU index in an element's name: `vulkanh264device1dec` is GPU 1. */
static gboolean index_in_name(const char *name, guint *index)
{
    const char *at = strstr(name, "device");
    gchar *end = NULL;
    if (at == NULL || !g_ascii_isdigit(at[6])) {
        return FALSE;
    }
    *index = (guint)g_ascii_strtoull(at + 6, &end, 10);
    return TRUE;
}

/* The element at the other end of `pad`, or NULL. */
static GstElement *peer_element(GstPad *pad)
{
    GstPad *peer = gst_pad_get_peer(pad);
    GstElement *el = peer ? gst_pad_get_parent_element(peer) : NULL;
    gst_clear_object(&peer);
    return el;
}

/* The sink pad of `el` and, one step further, the pad it is linked to (NULL at the start). */
static GstPad *upstream_pad(GstElement *el)
{
    GstPad *sink = gst_element_get_static_pad(el, "sink");
    GstPad *peer = sink ? gst_pad_get_peer(sink) : NULL;
    if (peer == NULL) {
        gst_clear_object(&sink);
        return NULL;
    }
    gst_object_unref(peer);
    return sink;
}

/* The GPU of the Vulkan element nearest upstream of `pad` (bounded walk), else the first. */
static guint upstream_gpu(GstPad *pad)
{
    GstPad *cur = gst_object_ref(pad);
    guint index = 0;
    for (guint depth = 0; depth < 8 && cur != NULL; depth++) {
        GstElement *el = peer_element(cur);
        GstElementFactory *factory = el ? gst_element_get_factory(el) : NULL;
        const gboolean found = factory && index_in_name(GST_OBJECT_NAME(factory), &index);
        GstPad *next = found || el == NULL ? NULL : upstream_pad(el);
        gst_clear_object(&el);
        gst_object_unref(cur);
        cur = next;
        if (found) {
            break;
        }
    }
    gst_clear_object(&cur);
    return index;
}

static GstVulkanDevice *context_device(VkContext *c, guint gpu, guint key, GError **err)
{
    GstVulkanDevice *dev = g_hash_table_lookup(c->devices, GUINT_TO_POINTER(key));
    if (dev == NULL) {
        dev = gst_vulkan_device_new_with_index(c->instance, gpu);
        if (dev != NULL && gst_vulkan_device_enable_extension(dev, "VK_KHR_external_memory_fd") &&
            gst_vulkan_device_open(dev, err)) {
            g_hash_table_insert(c->devices, GUINT_TO_POINTER(key), dev);
        } else {
            gst_clear_object(&dev);
        }
    }
    return dev;
}

/* Each sink pad gets a device of its own: elements of two branches sharing one VkDevice queue
 * crash the Mesa Intel driver (a double free inside vkQueueSubmit2, found with AddressSanitizer);
 * both are on one GPU, which the frames' PCI location checks. */
static guint pad_key(GstVmafx *self, GstAggregatorPad *pad)
{
    return pad == self->pad[GST_VMAFX_PAD_REFERENCE] ? 0 : 1;
}

/* The element's Vulkan instance, opened on first use. */
static GstVulkanInstance *context_instance(GstVmafx *self, VkContext *c)
{
    GError *err = NULL;
    if (c->instance == NULL) {
        c->instance = gst_vulkan_instance_new();
        if (!gst_vulkan_instance_open(c->instance, &err)) {
            GST_WARNING_OBJECT(self, "no Vulkan instance: %s", err ? err->message : "?");
            g_clear_error(&err);
            gst_clear_object(&c->instance);
        }
    }
    return c->instance;
}

/* Answer a "gst.vulkan.instance" or "gst.vulkan.device" query from upstream with our own. */
gboolean gst_vmafx_vulkan_context_query(GstVmafx *self, GstAggregatorPad *pad, GstQuery *query)
{
    const gchar *type = NULL;
    GError *err = NULL;
    gst_query_parse_context_type(query, &type);
    const gboolean want_device = !g_strcmp0(type, GST_VULKAN_DEVICE_CONTEXT_TYPE_STR);
    if (!want_device && g_strcmp0(type, GST_VULKAN_INSTANCE_CONTEXT_TYPE_STR) != 0) {
        return FALSE;
    }
    VkContext *c = vk_context(self);
    if (context_instance(self, c) == NULL) {
        return FALSE;
    }
    const guint gpu = want_device ? upstream_gpu(GST_PAD(pad)) : 0;
    GstVulkanDevice *dev =
        want_device ? context_device(c, gpu, gpu * 2 + pad_key(self, pad), &err) : NULL;
    if (want_device && dev == NULL) {
        GST_WARNING_OBJECT(self, "no exporting Vulkan device: %s", err ? err->message : "?");
        g_clear_error(&err);
        return FALSE;
    }
    return gst_vulkan_handle_context_query(GST_ELEMENT(self), query, NULL, c->instance, dev);
}

#endif /* HAVE_GST_VULKAN */
