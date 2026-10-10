/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * A real FFmpeg Vulkan decode handed to a device of the lane (RC4 WP3 Vulkan
 * lane). The reference and distorted bitstreams are decoded twice: by
 * FFmpeg's Vulkan hwaccel on the device's GPU (AV_PIX_FMT_VULKAN frames) and
 * in software.
 *
 * FFmpeg 9.0.2's Vulkan decoder outputs one 2-plane OPTIMAL image per frame,
 * also on a device made with disable_multiplane=1 (measured on the RTX 4090
 * and the gfx1036), and no device API reads a plane of an opaque multi-plane
 * image (FFmpeg's own CUDA interop refuses it; a CUDA NV12 array over it
 * faults): the import of such a frame must be refused naming
 * desc.plane[1].plane_index, which the first test checks.
 *
 * The second hands each decoded frame over as a producer would: a device
 * copy of its two plane aspects into a frame of an FFmpeg frames context with
 * one exportable image per plane (OPTIMAL for CUDA, LINEAR for the dma-buf
 * lanes), then the barrier FFmpeg's CUDA interop makes
 * (PREP_MODE_EXTERNAL_EXPORT: VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_EXTERNAL)
 * behind the images' timeline semaphores, its memory and semaphores exported
 * and imported as VMAFX_MEMORY_VULKAN. CUDA waits on the semaphores and
 * signals them again at release (FFmpeg's next use waits for that); the
 * dma-buf lanes take the frame after a host wait. The scores of every exact
 * cell of the lane equal the software-decoded frames' bit for bit, with no
 * host copy.
 *
 * Inputs: VMAFX_TEST_VULKAN_REF and VMAFX_TEST_VULKAN_DIST, two H.264 or
 * HEVC files of one size (77 without them, or without a Vulkan decoder on
 * the device's GPU). This is device evidence, not a CI test.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
#include <libavutil/imgutils.h>
#include <vulkan/vulkan.h>

#include "mu_table.h"
#include "test.h"
#include "vmafx/frame_import_hooks.h"
#include "vmafx/vmafx.h"
#include "vmafx_vulkan_test_util.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

#define MAX_FRAMES 48u
/* Attempts at a cell: the gfx1036 drops a run of a stream's commands now and
 * then (T-HIP-GFX1036-DROPPED-DISPATCHES-2026-10-01), so a cell that differs
 * is run again there (printed); a defect differs in every attempt. */
#define CELL_ATTEMPTS (VMAFX_VK_LANE == 4 ? 4u : 1u)

static VkGpu gpu;
static bool have_inputs;
static char vk_index[16];

/* ---- Decoding ------------------------------------------------------------------------ */

typedef struct Decoded {
    AVFrame *vk[MAX_FRAMES];   /* the decoder's Vulkan frames, held for the whole test */
    AVFrame *copy[MAX_FRAMES]; /* each copied into one image per plane */
    uint8_t *host[MAX_FRAMES]; /* the same frames decoded in software, tightly packed */
    unsigned n_vk;
    unsigned n_host;
    VmafxFrameDesc desc;
} Decoded;

static enum AVPixelFormat pick_vulkan(AVCodecContext *c, const enum AVPixelFormat *fmts)
{
    (void)c;
    for (const enum AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == AV_PIX_FMT_VULKAN) {
            return *p;
        }
    }
    return AV_PIX_FMT_NONE;
}

/* A decoder of the best video stream of `fmt`, on the Vulkan device `hw`
 * (NULL: software). */
static AVCodecContext *open_decoder(AVFormatContext *fmt, AVBufferRef *hw, int *stream)
{
    const AVCodec *codec = NULL;
    *stream = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    AVCodecContext *c = *stream >= 0 ? avcodec_alloc_context3(codec) : NULL;
    if (!c || avcodec_parameters_to_context(c, fmt->streams[*stream]->codecpar) < 0) {
        avcodec_free_context(&c);
        return NULL;
    }
    c->thread_count = 1;
    if (hw) {
        c->hw_device_ctx = av_buffer_ref(hw);
        c->get_format = pick_vulkan;
        c->extra_hw_frames = (int)MAX_FRAMES + 8;
    }
    if (avcodec_open2(c, codec, NULL) < 0) {
        avcodec_free_context(&c);
    }
    return c;
}

static AVFrame *device_copy(AVFrame *src);
static bool open_copies(AVBufferRef *hw, int w, int h);
static AVBufferRef *copies; /* NV12 frames, one exportable image per plane */

/* Keep `frame`: a Vulkan frame by reference, a software one packed. */
static bool keep(Decoded *d, const AVFrame *frame, bool vulkan)
{
    unsigned *const n = vulkan ? &d->n_vk : &d->n_host;
    if (*n >= MAX_FRAMES) {
        return true;
    }
    if (vulkan && !copies) {
        AVHWFramesContext *const fc = (AVHWFramesContext *)frame->hw_frames_ctx->data;
        if (!open_copies(fc->device_ref, frame->width, frame->height)) {
            return false;
        }
    }
    if (vulkan) {
        d->vk[*n] = av_frame_clone(frame);
        d->copy[*n] = d->vk[*n] ? device_copy(d->vk[*n]) : NULL;
        return d->copy[(*n)++] != NULL;
    }
    d->desc =
        vt_desc(VMAFX_PIXEL_FORMAT_YUV420P, 8u, (uint32_t)frame->width, (uint32_t)frame->height);
    const int bytes =
        av_image_get_buffer_size((enum AVPixelFormat)frame->format, frame->width, frame->height, 1);
    uint8_t *const buf = bytes > 0 ? malloc((size_t)bytes) : NULL;
    if (!buf) {
        return false;
    }
    d->host[*n] = buf;
    *n += 1u; /* freed with the others from here on */
    return av_image_copy_to_buffer(buf, bytes, (const uint8_t *const *)frame->data, frame->linesize,
                                   (enum AVPixelFormat)frame->format, frame->width, frame->height,
                                   1) == bytes;
}

/* Receive every frame the decoder has; false on an error. */
static bool drain(AVCodecContext *c, AVFrame *frame, Decoded *d, bool vulkan)
{
    for (unsigned guard = 0; guard < 4096u; guard++) {
        const int r = avcodec_receive_frame(c, frame);
        if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) {
            return true;
        }
        const bool ok = r >= 0 && (!vulkan || frame->format == AV_PIX_FMT_VULKAN) &&
                        (vulkan || frame->format == AV_PIX_FMT_YUV420P) && keep(d, frame, vulkan);
        av_frame_unref(frame);
        if (!ok) {
            return false;
        }
    }
    return false;
}

static bool decode_file(const char *path, AVBufferRef *hw, Decoded *d)
{
    AVFormatContext *fmt = NULL;
    int stream = -1;
    if (avformat_open_input(&fmt, path, NULL, NULL) < 0 ||
        avformat_find_stream_info(fmt, NULL) < 0) {
        avformat_close_input(&fmt);
        return false;
    }
    AVCodecContext *c = open_decoder(fmt, hw, &stream);
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    bool ok = c && pkt && frame;
    for (unsigned guard = 0; ok && guard < 100000u && av_read_frame(fmt, pkt) >= 0; guard++) {
        ok = pkt->stream_index != stream ||
             (avcodec_send_packet(c, pkt) >= 0 && drain(c, frame, d, hw != NULL));
        av_packet_unref(pkt);
    }
    ok = ok && avcodec_send_packet(c, NULL) >= 0 && drain(c, frame, d, hw != NULL);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&c);
    avformat_close_input(&fmt);
    return ok && (hw ? d->n_vk : d->n_host) > 0u;
}

/* ---- Handing a Vulkan frame over -------------------------------------------------------- */

typedef struct Handover {
    AVVulkanDeviceContext *dev;
    VkQueue queue;
    VkCommandPool pool;
    PFN_vkGetMemoryFdKHR get_memory_fd;
    PFN_vkGetSemaphoreFdKHR get_semaphore_fd;
} Handover;

static Handover ho;

/* A queue family of FFmpeg's device that transfers. */
static int handover_family(const AVVulkanDeviceContext *dev)
{
    for (int i = 0; i < dev->nb_qf; i++) {
        if (dev->qf[i].flags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
            return dev->qf[i].idx;
        }
    }
    return -1;
}

static bool handover_open(AVBufferRef *hw)
{
    ho.dev = ((AVHWDeviceContext *)hw->data)->hwctx;
    const int family = handover_family(ho.dev);
    const VkCommandPoolCreateInfo pool = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                          .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                          .queueFamilyIndex = (uint32_t)family};
    if (family < 0 || vkCreateCommandPool(ho.dev->act_dev, &pool, NULL, &ho.pool) != VK_SUCCESS) {
        return false;
    }
    vkGetDeviceQueue(ho.dev->act_dev, (uint32_t)family, 0, &ho.queue);
    ho.get_memory_fd =
        (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(ho.dev->act_dev, "vkGetMemoryFdKHR");
    ho.get_semaphore_fd =
        (PFN_vkGetSemaphoreFdKHR)vkGetDeviceProcAddr(ho.dev->act_dev, "vkGetSemaphoreFdKHR");
    return ho.get_memory_fd && ho.get_semaphore_fd;
}

static unsigned images_of(const AVVkFrame *f)
{
    unsigned n = 0;
    while (n < AV_NUM_DATA_POINTERS && f->img[n]) {
        n++;
    }
    return n;
}

/* Record the barrier of image `i` to GENERAL and VK_QUEUE_FAMILY_EXTERNAL. */
static void export_barrier(VkCommandBuffer cmd, AVVkFrame *f, unsigned i)
{
    const VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                     .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                     .srcAccessMask = f->access[i],
                                     .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                     .dstAccessMask =
                                         VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                     .oldLayout = f->layout[i],
                                     .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                     .srcQueueFamilyIndex = f->queue_family[i],
                                     .dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL,
                                     .image = f->img[i],
                                     .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    const VkDependencyInfo dep = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                  .imageMemoryBarrierCount = 1,
                                  .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cmd, &dep);
    f->layout[i] = VK_IMAGE_LAYOUT_GENERAL;
    f->access[i] = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    f->queue_family[i] = VK_QUEUE_FAMILY_EXTERNAL;
}

static int export_semaphore(VkSemaphore s)
{
    const VkSemaphoreGetFdInfoKHR info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
                                          .semaphore = s,
                                          .handleType =
                                              VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT};
    int fd = -1;
    return ho.get_semaphore_fd(ho.dev->act_dev, &info, &fd) == VK_SUCCESS ? fd : -1;
}

/* ---- The producer's device copy into one image per plane ----------------------------- */

static bool open_copies(AVBufferRef *hw, int w, int h)
{
    copies = av_hwframe_ctx_alloc(hw);
    if (!copies) {
        return false;
    }
    AVHWFramesContext *const fc = (AVHWFramesContext *)copies->data;
    AVVulkanFramesContext *const vfc = fc->hwctx;
    fc->format = AV_PIX_FMT_VULKAN;
    fc->sw_format = AV_PIX_FMT_NV12;
    fc->width = w;
    fc->height = h;
    vfc->tiling = VK_DEVICE_SEMAPHORES ? VK_IMAGE_TILING_OPTIMAL : VK_IMAGE_TILING_LINEAR;
    vfc->flags = AV_VK_FRAME_FLAG_DISABLE_MULTIPLANE;
    return av_hwframe_ctx_init(copies) >= 0;
}

/* A barrier of image `img` of `f` (index `i` of its fields) to `layout`. */
static VkImageMemoryBarrier2 layout_barrier(AVVkFrame *f, unsigned i, VkImageLayout layout,
                                            VkAccessFlags2 access)
{
    const VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                     .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                     .srcAccessMask = f->access[i],
                                     .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                     .dstAccessMask = access,
                                     .oldLayout = f->layout[i],
                                     .newLayout = layout,
                                     .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                     .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                     .image = f->img[i],
                                     .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    f->layout[i] = layout;
    f->access[i] = access;
    return b;
}

/* Record the copy of the two plane aspects of `src` into the images of `dst`. */
static void record_copy(VkCommandBuffer cmd, AVVkFrame *src, AVVkFrame *dst, int w, int h)
{
    const VkImageMemoryBarrier2 bars[3] = {
        layout_barrier(src, 0u, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       VK_ACCESS_2_TRANSFER_READ_BIT),
        layout_barrier(dst, 0u, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_ACCESS_2_TRANSFER_WRITE_BIT),
        layout_barrier(dst, 1u, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_ACCESS_2_TRANSFER_WRITE_BIT)};
    const VkDependencyInfo dep = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                  .imageMemoryBarrierCount = 3,
                                  .pImageMemoryBarriers = bars};
    vkCmdPipelineBarrier2(cmd, &dep);
    const VkImageCopy planes[2] = {
        {.srcSubresource = {VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 0, 1},
         .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
         .extent = {(uint32_t)w, (uint32_t)h, 1u}},
        {.srcSubresource = {VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 0, 1},
         .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
         .extent = {(uint32_t)(w + 1) / 2u, (uint32_t)(h + 1) / 2u, 1u}}};
    for (unsigned i = 0; i < 2u; i++) {
        vkCmdCopyImage(cmd, src->img[0], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst->img[i],
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &planes[i]);
    }
}

/* Submit `cmd` behind the timelines of `f[0..n)` (each image's semaphore at
 * its value, signalled one higher). */
static bool submit_behind(VkCommandBuffer cmd, AVVkFrame *const *f, unsigned n)
{
    VkSemaphoreSubmitInfo waits[4];
    VkSemaphoreSubmitInfo signals[4];
    unsigned k = 0;
    for (unsigned j = 0; j < n; j++) {
        for (unsigned i = 0; i < images_of(f[j]) && k < 4u; i++, k++) {
            const VkSemaphoreSubmitInfo w = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                             .semaphore = f[j]->sem[i],
                                             .value = f[j]->sem_value[i],
                                             .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
            waits[k] = w;
            signals[k] = w;
            signals[k].value = ++f[j]->sem_value[i];
        }
    }
    const VkCommandBufferSubmitInfo cb = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                                          .commandBuffer = cmd};
    const VkSubmitInfo2 si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
                              .waitSemaphoreInfoCount = k,
                              .pWaitSemaphoreInfos = waits,
                              .commandBufferInfoCount = 1,
                              .pCommandBufferInfos = &cb,
                              .signalSemaphoreInfoCount = k,
                              .pSignalSemaphoreInfos = signals};
    return vkEndCommandBuffer(cmd) == VK_SUCCESS &&
           vkQueueSubmit2(ho.queue, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS;
}

static VkCommandBuffer begin_cmd(void)
{
    const VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                            .commandPool = ho.pool,
                                            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                            .commandBufferCount = 1};
    const VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                         .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    const bool ok = vkAllocateCommandBuffers(ho.dev->act_dev, &ai, &cmd) == VK_SUCCESS &&
                    vkBeginCommandBuffer(cmd, &bi) == VK_SUCCESS;
    return ok ? cmd : VK_NULL_HANDLE;
}

/* The device copy of decoded frame `src` into a new frame of `copies`. */
static AVFrame *device_copy(AVFrame *src)
{
    AVFrame *dst = av_frame_alloc();
    if (!dst || av_hwframe_get_buffer(copies, dst, 0) < 0) {
        av_frame_free(&dst);
        return NULL;
    }
    AVVkFrame *const pair[2] = {(AVVkFrame *)src->data[0], (AVVkFrame *)dst->data[0]};
    VkCommandBuffer cmd =
        images_of(pair[0]) == 1u && images_of(pair[1]) == 2u ? begin_cmd() : VK_NULL_HANDLE;
    if (cmd) {
        record_copy(cmd, pair[0], pair[1], src->width, src->height);
    }
    if (!cmd || !submit_behind(cmd, pair, 2u)) {
        av_frame_free(&dst);
    }
    return dst;
}

/* The export transition FFmpeg's CUDA interop makes, behind the images'
 * timeline semaphores (waited at sem_value, signalled at sem_value + 1). */
static bool export_transition(AVVkFrame *f)
{
    VkCommandBuffer cmd = begin_cmd();
    if (!cmd) {
        return false;
    }
    for (unsigned i = 0; i < images_of(f); i++) {
        export_barrier(cmd, f, i);
    }
    AVVkFrame *const one[1] = {f};
    return submit_behind(cmd, one, 1u);
}

/* The fields every descriptor of an exported NV12 frame has. */
static void describe_frame(const AVFrame *frame, const AVVkFrame *f, VmafxFrameImport *imp)
{
    const VmafxFrameImport init = VMAFX_FRAME_IMPORT_INIT;
    *imp = init;
    imp->memory = VMAFX_MEMORY_VULKAN;
    imp->pix_fmt = VMAFX_PIXEL_FORMAT_NV12;
    imp->bpc = 8u;
    imp->w = (uint32_t)frame->width;
    imp->h = (uint32_t)frame->height;
    imp->n_planes = 2u;
    imp->vulkan_handle_type = VMAFX_VULKAN_HANDLE_OPAQUE_FD;
    imp->vulkan_tiling = (uint32_t)f->tiling;
    memcpy(imp->vulkan_pci, gpu.pci, sizeof(imp->vulkan_pci));
    imp->acquire.fd = -1;
    imp->acquire_more[0].fd = -1;
    for (unsigned i = 0; i < 2u; i++) {
        imp->plane[i].fd = -1;
    }
}

/* Plane `i` from image `img` of `f` (its memory and its timeline at
 * `acquire`); false when an export fails. */
static bool describe_plane(const AVVkFrame *f, unsigned img, unsigned i, VmafxFrameImport *imp,
                           VmafxFence *acquire)
{
    const VkMemoryGetFdInfoKHR mi = {.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
                                     .memory = f->mem[img],
                                     .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
    const bool ok = ho.get_memory_fd(ho.dev->act_dev, &mi, &imp->plane[i].fd) == VK_SUCCESS;
    imp->plane[i].size = f->size[img];
    imp->plane[i].offset = (uint64_t)f->offset[img];
    imp->plane[i].plane_index = img == i ? 0u : i; /* a plane of a multi-plane image */
    if (f->tiling == VK_IMAGE_TILING_LINEAR) {
        const VkImageSubresource sub = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT};
        VkSubresourceLayout layout;
        vkGetImageSubresourceLayout(ho.dev->act_dev, f->img[img], &sub, &layout);
        imp->plane[i].offset += layout.offset;
        imp->plane[i].pitch = layout.rowPitch;
    }
    if (acquire) {
        acquire->kind = VMAFX_FENCE_VULKAN_SEMAPHORE;
        acquire->fd = export_semaphore(f->sem[img]);
        acquire->value = f->sem_value[img];
    }
    return ok && (!acquire || acquire->fd >= 0);
}

/* The descriptor of an exported NV12 frame of one image per plane. */
static bool describe(const AVFrame *frame, const AVVkFrame *f, VmafxFrameImport *imp)
{
    describe_frame(frame, f, imp);
    return images_of(f) == 2u && describe_plane(f, 0u, 0u, imp, &imp->acquire) &&
           describe_plane(f, 1u, 1u, imp, &imp->acquire_more[0]);
}

/* The descriptor of the decoder's frame: both planes in its one image. */
static bool describe_image(const AVFrame *frame, const AVVkFrame *f, VmafxFrameImport *imp)
{
    describe_frame(frame, f, imp);
    return describe_plane(f, 0u, 0u, imp, &imp->acquire) && describe_plane(f, 0u, 1u, imp, NULL);
}

static void close_descriptor(VmafxFrameImport *imp)
{
    for (unsigned i = 0; i < 2u; i++) {
        if (imp->plane[i].fd >= 0) {
            (void)close(imp->plane[i].fd);
        }
    }
    if (imp->acquire.fd >= 0) {
        (void)close(imp->acquire.fd);
    }
    if (imp->acquire_more[0].fd >= 0) {
        (void)close(imp->acquire_more[0].fd);
    }
}

/* The library's release signals each image's semaphore once more; FFmpeg's
 * next use of the frame waits for that value. */
static VmafxStatus signal_release(VmafxFrame *frame, AVVkFrame *f)
{
    VmafxStatus status = VMAFX_OK;
    for (unsigned i = 0; i < images_of(f) && status == VMAFX_OK; i++) {
        VmafxFence s = VMAFX_FENCE_INIT;
        s.kind = VMAFX_FENCE_VULKAN_SEMAPHORE;
        s.fd = export_semaphore(f->sem[i]);
        s.value = ++f->sem_value[i];
        status = s.fd >= 0 ? vmafx_frame_signal_on_release(frame, &s, NULL) : VMAFX_E_DEVICE;
        if (s.fd >= 0) {
            (void)close(s.fd);
        }
    }
    return status;
}

/* Hand Vulkan frame `frame` over and import it; `*refused` gets the status of
 * a refusal (no frame). */
/* A lane that waits on no Vulkan semaphore (SYCL, HIP) takes the frame once
 * its transition finished on the host: the acquire fences become NONE. */
static bool host_acquire(const AVVkFrame *f, VmafxFrameImport *imp)
{
    VmafxFence *const fences[2] = {&imp->acquire, &imp->acquire_more[0]};
    bool ok = true;
    for (unsigned i = 0; i < 2u; i++) {
        const VkSemaphoreWaitInfo wi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
                                        .semaphoreCount = 1,
                                        .pSemaphores = &f->sem[i],
                                        .pValues = &f->sem_value[i]};
        ok = ok && vkWaitSemaphores(ho.dev->act_dev, &wi, UINT64_MAX) == VK_SUCCESS;
        if (fences[i]->fd >= 0) {
            (void)close(fences[i]->fd);
        }
        fences[i]->kind = VMAFX_FENCE_NONE;
        fences[i]->fd = -1;
    }
    return ok;
}

/* Hand Vulkan frame `frame` (one image per plane) over and import it;
 * `*refused` gets the import's status. */
static VmafxFrame *import_avframe(VmafxContext *context, AVFrame *frame, VmafxStatus *refused)
{
    AVHWFramesContext *const hwfc = (AVHWFramesContext *)frame->hw_frames_ctx->data;
    AVVulkanFramesContext *const vkfc = hwfc->hwctx;
    AVVkFrame *const f = (AVVkFrame *)frame->data[0];
    VmafxFrameImport imp;
    VmafxFrame *out = NULL;
    describe_frame(frame, f, &imp);
    vkfc->lock_frame(hwfc, f);
    bool ok = export_transition(f) && describe(frame, f, &imp);
    ok = ok && (VK_DEVICE_SEMAPHORES || host_acquire(f, &imp));
    VmafxStatus status = ok ? vmafx_frame_import(gpu.device, &imp, &out, NULL) : VMAFX_E_DEVICE;
    if (status == VMAFX_OK && VK_DEVICE_SEMAPHORES) {
        status = signal_release(out, f);
    }
    vkfc->unlock_frame(hwfc, f);
    close_descriptor(&imp);
    *refused = status;
    (void)context;
    if (status != VMAFX_OK) {
        vmafx_frame_unref(out);
        out = NULL;
    }
    return out;
}

/* ---- Sessions -------------------------------------------------------------------------- */

static VmafxContext *run_host(const Decoded *ref, const Decoded *dist, const VcCell *cell)
{
    VmafxContext *const context = vc_cell_context(gpu.device, cell);
    bool ok = context != NULL;
    for (unsigned i = 0; i < ref->n_host && ok; i++) {
        ok = vmafx_submit(context, vt_wrap_frame(&ref->desc, ref->host[i], NULL),
                          vt_wrap_frame(&dist->desc, dist->host[i], NULL), i, NULL) == VMAFX_OK;
    }
    return ok && vmafx_flush(context, NULL) == VMAFX_OK ? context : NULL;
}

/* The import session over the held Vulkan frames (each handed over again:
 * the transition waits for the previous session's release). `*status` gets
 * the first refusal. */
static VmafxContext *run_vulkan(Decoded *ref, Decoded *dist, const VcCell *cell,
                                VmafxStatus *status)
{
    VmafxContext *const context = vc_cell_context(gpu.device, cell);
    bool ok = context != NULL;
    *status = VMAFX_OK;
    for (unsigned i = 0; i < ref->n_vk && ok; i++) {
        VmafxFrame *const r = import_avframe(context, ref->copy[i], status);
        VmafxFrame *const d = r ? import_avframe(context, dist->copy[i], status) : NULL;
        ok = r && d && vmafx_submit(context, r, d, i, NULL) == VMAFX_OK;
        if (!ok) {
            vmafx_frame_unref(r);
        }
    }
    ok = ok && vmafx_flush(context, NULL) == VMAFX_OK;
    if (!ok && context) {
        (void)vmafx_context_destroy(context, NULL);
    }
    return ok ? context : NULL;
}

/* ---- Tests -------------------------------------------------------------------------------- */

static Decoded ref_dec;
static Decoded dist_dec;

static bool decode_inputs(AVBufferRef *hw, const char *ref, const char *dist)
{
    return decode_file(ref, hw, &ref_dec) && decode_file(dist, hw, &dist_dec) &&
           decode_file(ref, NULL, &ref_dec) && decode_file(dist, NULL, &dist_dec) &&
           ref_dec.n_vk == ref_dec.n_host && dist_dec.n_vk == dist_dec.n_host &&
           ref_dec.n_vk == dist_dec.n_vk;
}

/* One attempt at a cell: the host session against the Vulkan one;
 * `*refused` gets the status of a refused import. */
static bool attempt_cell(const VcCell *cell, unsigned long *compared, unsigned long *differing,
                         VmafxStatus *refused)
{
    VmafxContext *const host = run_host(&ref_dec, &dist_dec, cell);
    VmafxContext *const vk = host ? run_vulkan(&ref_dec, &dist_dec, cell, refused) : NULL;
    const bool same = host && vk && vc_compare(host, vk, ref_dec.n_vk, compared, differing);
    if (host) {
        (void)vmafx_context_destroy(host, NULL);
    }
    if (vk) {
        (void)vmafx_context_destroy(vk, NULL);
    }
    return same;
}

static unsigned repeated;

/* A cell, repeated on the gfx1036 while it differs. */
static bool score_cell(const VcCell *cell, unsigned long *compared, unsigned long *differing,
                       VmafxStatus *refused)
{
    bool same = false;
    for (unsigned a = 0; a < CELL_ATTEMPTS; a++) {
        unsigned long n = 0;
        unsigned long bad = 0;
        same = attempt_cell(cell, &n, &bad, refused);
        if (same && bad == 0u) {
            *compared += n;
            return true;
        }
        (void)fprintf(stderr, "\n  cell %s, attempt %u: %lu differing\n", cell->name, a + 1u, bad);
        repeated += a + 1u < CELL_ATTEMPTS ? 1u : 0u;
        if (a + 1u == CELL_ATTEMPTS) {
            *compared += n;
            *differing += bad;
        }
    }
    return same;
}

/* The decoder's own frame, one 2-plane image, described plane by plane:
 * refused naming the chroma plane's index (no device reads a plane of an
 * opaque multi-plane image). */
static char *test_multiplane_refused(void)
{
    if (!have_inputs) {
        return NULL;
    }
    AVHWFramesContext *const hwfc = (AVHWFramesContext *)ref_dec.vk[0]->hw_frames_ctx->data;
    AVVulkanFramesContext *const vkfc = hwfc->hwctx;
    AVVkFrame *const f = (AVVkFrame *)ref_dec.vk[0]->data[0];
    vkfc->lock_frame(hwfc, f);
    VmafxFrameImport imp;
    describe_frame(ref_dec.vk[0], f, &imp);
    const bool moved = images_of(f) == 1u && export_transition(f);
    const bool ok = moved && describe_image(ref_dec.vk[0], f, &imp);
    VmafxError *error = NULL;
    VmafxFrame *frame = NULL;
    const VmafxStatus status =
        ok ? vmafx_frame_import(gpu.device, &imp, &frame, &error) : VMAFX_E_DEVICE;
    vkfc->unlock_frame(hwfc, f);
    if (moved && !ok) {
        /* The producer could not export the decoder's memory at all (measured:
         * FFmpeg 8.0's decoder pool on RADV): nothing to hand over. */
        (void)fprintf(stderr, "[the decoder's frames are not exportable on this producer] ");
        close_descriptor(&imp);
        return NULL;
    }
    (void)fprintf(stderr, "[decoder frame: %u image(s), tiling %d] ", images_of(f), (int)f->tiling);
    const bool named =
        error && strcmp(vmafx_error_subject(error), "desc.plane[1].plane_index") == 0;
    if (!named) {
        (void)fprintf(stderr, "[import %s: status %d, %s] ", ok ? "made" : "not made", (int)status,
                      error ? vmafx_error_message(error) : "no error");
    }
    vmafx_error_free(error);
    vmafx_frame_unref(frame);
    close_descriptor(&imp);
    mu_assert("a 2-plane image refused naming desc.plane[1].plane_index",
              status == VMAFX_E_NOTSUP && named);
    return NULL;
}

static char *test_decode_scores(void)
{
    if (!have_inputs) {
        return NULL;
    }
    unsigned long compared = 0;
    unsigned long differing = 0;
    unsigned cells = 0;
    VmafxStatus refused = VMAFX_OK;
    const unsigned ch = (ref_dec.desc.h + 1u) / 2u;
    const unsigned cw = (ref_dec.desc.w + 1u) / 2u;
    for (size_t c = 0; c < VK_N_CELLS && refused == VMAFX_OK; c++) {
        const VcCell *const cell = &VK_CELLS[c];
        if (cell->min_chroma > 0u && (cw < cell->min_chroma || ch < cell->min_chroma)) {
            continue;
        }
        const bool same = score_cell(cell, &compared, &differing, &refused);
        cells += same ? 1u : 0u;
        mu_assert("cell sessions", same || refused != VMAFX_OK);
    }
    (void)fprintf(stderr,
                  "[%u frames, %u cells, %lu values, %lu differing, %u repeated attempts, import "
                  "status %d] ",
                  ref_dec.n_vk, cells, compared, differing, repeated, (int)refused);
    mu_assert("bit-identical", refused == VMAFX_OK && compared > 0u && differing == 0u);
    mu_assert("no host copy", vmafx_test_host_copies() == 0u);
    return NULL;
}

static void free_decoded(Decoded *d)
{
    for (unsigned i = 0; i < MAX_FRAMES; i++) {
        av_frame_free(&d->copy[i]);
        av_frame_free(&d->vk[i]);
        free(d->host[i]);
    }
    memset(d, 0, sizeof(*d));
}

char *run_tests(void)
{
    /* NOLINTNEXTLINE(concurrency-mt-unsafe): single-thread test setup (ADR-0141 / ADR-0278). */
    const char *const ref = getenv("VMAFX_TEST_VULKAN_REF");
    /* NOLINTNEXTLINE(concurrency-mt-unsafe): single-thread test setup (ADR-0141 / ADR-0278). */
    const char *const dist = getenv("VMAFX_TEST_VULKAN_DIST");
    AVBufferRef *hw = NULL;
    AVDictionary *opts = NULL;
    bool ok = ref && dist && vk_open(&gpu);
    const int index = ok ? vkp_index_of_pci(gpu.pci) : -1;
    (void)snprintf(vk_index, sizeof(vk_index), "%d", index);
    (void)av_dict_set(&opts, "disable_multiplane", "1", 0);
    ok = ok && index >= 0 &&
         av_hwdevice_ctx_create(&hw, AV_HWDEVICE_TYPE_VULKAN, vk_index, opts, 0) >= 0 &&
         handover_open(hw);
    av_dict_free(&opts);
    have_inputs = ok && decode_inputs(hw, ref, dist);
    /* vk_open() states its own reason; the others are named here. */
    if (!(ref && dist)) {
        (void)fprintf(stderr, "skipped: VMAFX_TEST_VULKAN_REF / VMAFX_TEST_VULKAN_DIST not set\n");
    } else if (!have_inputs && (access(ref, R_OK) != 0 || access(dist, R_OK) != 0)) {
        (void)fprintf(stderr, "skipped: the inputs are not readable (%s, %s)\n", ref, dist);
    } else if (!have_inputs && gpu.vk) {
        (void)fprintf(stderr, "skipped: no Vulkan decode of the inputs on this GPU\n");
    }
    mu_skipped = !have_inputs; /* no case runs, so none reports a pass (Q-346) */
    static const MuTest tests[] = {MU_TEST(test_multiplane_refused), MU_TEST(test_decode_scores)};
    char *const msg = have_inputs ? mu_run_table(tests, MU_TABLE_LEN(tests)) : NULL;
    free_decoded(&ref_dec);
    free_decoded(&dist_dec);
    if (ho.pool) {
        (void)vkDeviceWaitIdle(ho.dev->act_dev);
        vkDestroyCommandPool(ho.dev->act_dev, ho.pool, NULL);
    }
    av_buffer_unref(&copies);
    av_buffer_unref(&hw);
    vk_close(&gpu);
    return msg;
}

/* NOLINTEND(modernize-use-nullptr) */
