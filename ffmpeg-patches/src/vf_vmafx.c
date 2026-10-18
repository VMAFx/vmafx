/*
 * Copyright 2026 Lusoris
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public
 * License for more details.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

/**
 * @file
 * The vmafx filter: VMAF and the other VMAFx metrics of two video streams
 * through the VMAFx API (libvmafx). Every backend through one filter: the
 * backend follows the input frames (software frames on the CPU, CUDA frames
 * on CUDA, DRM PRIME frames on SYCL or HIP, imported without a copy; Vulkan
 * frames copied once on the GPU and imported on the device of their GPU);
 * the options are generated from the library's option groups
 * (vf_vmafx_options.h); n_stats windows run on the library's window clock;
 * the provenance record goes to the log and the report. Source of truth:
 * ffmpeg-patches/src/vf_vmafx.c in the VMAFx tree.
 */

#include "config_components.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include <vmafx/vmafx.h>

#include "libavutil/avstring.h"
#include "libavutil/bprint.h"
#include "libavutil/file_open.h"
#include "libavutil/fifo.h"
#include "libavutil/hwcontext.h"
#include "libavutil/mathematics.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#if CONFIG_CUDA
#include "libavutil/hwcontext_cuda_internal.h"
#endif
#if CONFIG_VULKAN
#include <unistd.h>

#include "libavutil/hwcontext_vulkan.h"
#include "libavutil/internal.h"
#endif
#if CONFIG_LIBDRM
#include <sys/stat.h>
#include <sys/sysmacros.h>

#include "libavutil/hwcontext_drm.h"
#endif
#include "avfilter.h"
#include "filters.h"
#include "formats.h"
#include "framesync.h"
#include "video.h"
#include "vf_vmafx_options.h"

#define VMAFX_MAX_MODELS 16
#define VMAFX_MAX_SPANS 64
#define VMAFX_SPEC_BYTES 4096
#define VMAFX_NS ((AVRational){1, 1000000000})

/* One window of the stream: the clock's span and one window per model. */
typedef struct VMAFXSpan {
    VmafxWindowSpan span;
    VmafxWindow *window[VMAFX_MAX_MODELS];
} VMAFXSpan;

typedef struct VMAFXContext {
    const AVClass *class;
    FFFrameSync fs;
    VMAFX_FILTER_OPTION_FIELDS

    VmafxContext *context;
    VmafxDevice *vdev; /* `device` is the option */
    VmafxModel *models[VMAFX_MAX_MODELS];
    unsigned n_models;
    const struct VMAFXBackendSlot *slot; /* the backend frames go to */
    int hw;                              /* the inputs are hardware frames */
    int vulkan;                          /* ... Vulkan frames, copied for the handover */
    struct VMAFXVulkan *vk;
    enum AVPixelFormat sw_format; /* the planes' layout */
    uint64_t frame_cnt;
    int failed;   /* a frame could not be scored: no score line after it */
    int finished; /* the stream was flushed and every window written */
    int logged_download;
    /* Frames by path: imported on the device without a copy, host frames
     * wrapped without a copy, hardware frames downloaded (import=host). */
    uint64_t n_imported, n_host, n_downloaded;

    VmafxWindowClock *clock;
    VMAFXSpan spans[VMAFX_MAX_SPANS]; /* windows submitted, oldest first */
    unsigned span_head, span_count;
    FILE *stats_file;
    char *window_meta; /* window metadata for the next output frame */

    AVFifo *held; /* output frames waiting for their scores (metadata=1) */

    /* vmafx_tune */
    double recommend_target_vmaf;
    double recommend_crf_min;
    double recommend_crf_max;
    int recommend_passes;
} VMAFXContext;

/* ---- Backend slots ----------------------------------------------------------
 * One entry per VMAFx backend. A slot without functions refuses its frames by
 * name: the SYCL, HIP and Metal imports (DRM PRIME / VA frames mapped with
 * hwmap, macOS hardware frames) fill their slots when their WP3 lanes land. */

typedef struct VMAFXBackendSlot {
    uint32_t backend;             /* VmafxBackend */
    const char *name;             /* the `backend` option's value */
    enum AVPixelFormat hw_format; /* the hardware frames it imports */
    int (*open)(AVFilterContext *ctx, AVBufferRef *frames);
    int (*import)(AVFilterContext *ctx, AVFrame *frame, const char *input, VmafxFrame **out);
} VMAFXBackendSlot;

#if CONFIG_CUDA
static int cuda_open(AVFilterContext *ctx, AVBufferRef *frames);
static int cuda_import(AVFilterContext *ctx, AVFrame *frame, const char *input, VmafxFrame **out);
#endif
#if CONFIG_LIBDRM
static int drm_open(AVFilterContext *ctx, AVBufferRef *frames);
static int drm_import(AVFilterContext *ctx, AVFrame *frame, const char *input, VmafxFrame **out);
#define VMAFX_DRM_OPEN drm_open
#define VMAFX_DRM_IMPORT drm_import
#else
#define VMAFX_DRM_OPEN NULL
#define VMAFX_DRM_IMPORT NULL
#endif

#if CONFIG_VULKAN
static int vk_open(AVFilterContext *ctx, AVBufferRef *frames);
static int vk_import(AVFilterContext *ctx, AVFrame *frame, const char *input, VmafxFrame **out);
static void vk_close(VMAFXContext *s);
#endif

/* The slot of the backend that reads the GPUs of a PCI vendor with
 * backend=auto: CUDA on NVIDIA, SYCL on Intel, HIP on AMD; 0 for another. */
static int vendor_slot(unsigned vendor)
{
    switch (vendor) {
    case 0x10deu:
        return 2;
    case 0x8086u:
        return 3;
    case 0x1002u:
        return 4;
    default:
        return 0;
    }
}

/* Indexed by the `backend` option (auto, cpu, cuda, sycl, hip, metal). */
static const VMAFXBackendSlot backend_slots[] = {
    {VMAFX_BACKEND_CPU, "auto", AV_PIX_FMT_NONE, NULL, NULL},
    {VMAFX_BACKEND_CPU, "cpu", AV_PIX_FMT_NONE, NULL, NULL},
#if CONFIG_CUDA
    {VMAFX_BACKEND_CUDA, "cuda", AV_PIX_FMT_CUDA, cuda_open, cuda_import},
#else
    {VMAFX_BACKEND_CUDA, "cuda", AV_PIX_FMT_CUDA, NULL, NULL},
#endif
    {VMAFX_BACKEND_SYCL, "sycl", AV_PIX_FMT_DRM_PRIME, VMAFX_DRM_OPEN, VMAFX_DRM_IMPORT},
    {VMAFX_BACKEND_HIP, "hip", AV_PIX_FMT_DRM_PRIME, VMAFX_DRM_OPEN, VMAFX_DRM_IMPORT},
    {VMAFX_BACKEND_METAL, "metal", AV_PIX_FMT_VIDEOTOOLBOX, NULL, NULL},
};

#define OFFSET(x) offsetof(VMAFXContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_VIDEO_PARAM

static const AVOption vmafx_options[] = {
    VMAFX_FILTER_OPTIONS(VMAFXContext, FLAGS){NULL},
};

FRAMESYNC_DEFINE_CLASS(vmafx, VMAFXContext, fs);

/* ---- Errors and the library's log ------------------------------------------- */

/* Log a library failure naming the call, then release the error. */
static int vmafx_fail(AVFilterContext *ctx, VmafxStatus status, VmafxError *error, const char *what)
{
    av_log(ctx, AV_LOG_ERROR, "%s: %s: %s [%s]\n", what, vmafx_status_name(status),
           error ? vmafx_error_message(error) : "", error ? vmafx_error_subject(error) : "");
    vmafx_error_free(error);
    return status == VMAFX_E_NOMEM ? AVERROR(ENOMEM) : AVERROR_EXTERNAL;
}

static void log_from_library(uint32_t level, const char *message, void *user)
{
    static const int levels[] = {AV_LOG_QUIET, AV_LOG_ERROR, AV_LOG_WARNING, AV_LOG_INFO,
                                 AV_LOG_DEBUG};
    const int av_level = level < FF_ARRAY_ELEMS(levels) ? levels[level] : AV_LOG_DEBUG;
    const size_t len = strlen(message);
    av_log(user, av_level, len && message[len - 1] == '\n' ? "%s" : "%s\n", message);
}

static uint32_t library_log_level(void)
{
    const int level = av_log_get_level();
    if (level >= AV_LOG_DEBUG)
        return VMAFX_LOG_LEVEL_DEBUG;
    if (level >= AV_LOG_INFO)
        return VMAFX_LOG_LEVEL_INFO;
    return level >= AV_LOG_WARNING ? VMAFX_LOG_LEVEL_WARNING : VMAFX_LOG_LEVEL_ERROR;
}

/* ---- Formats -------------------------------------------------------------- */

static const enum AVPixelFormat pix_fmts[] = {
    AV_PIX_FMT_YUV444P,
    AV_PIX_FMT_YUV422P,
    AV_PIX_FMT_YUV420P,
    AV_PIX_FMT_YUV444P10LE,
    AV_PIX_FMT_YUV422P10LE,
    AV_PIX_FMT_YUV420P10LE,
    AV_PIX_FMT_YUV444P12LE,
    AV_PIX_FMT_YUV422P12LE,
    AV_PIX_FMT_YUV420P12LE,
    AV_PIX_FMT_YUV444P16LE,
    AV_PIX_FMT_YUV422P16LE,
    AV_PIX_FMT_YUV420P16LE,
    AV_PIX_FMT_GRAY8,
    AV_PIX_FMT_GRAY10LE,
    AV_PIX_FMT_GRAY12LE,
    AV_PIX_FMT_GRAY16LE,
    AV_PIX_FMT_NV12,
    AV_PIX_FMT_P010LE,
    AV_PIX_FMT_P016LE,
    /* 4:2:2 and 4:4:4 semi-planar, packed and MSB-aligned layouts: imported
     * and converted to planar by the library (ADR-2133). */
    AV_PIX_FMT_NV16,
    AV_PIX_FMT_P210LE,
    AV_PIX_FMT_P216LE,
    AV_PIX_FMT_NV24,
    AV_PIX_FMT_P410LE,
    AV_PIX_FMT_P416LE,
    AV_PIX_FMT_Y210LE,
    AV_PIX_FMT_Y212LE,
    AV_PIX_FMT_YUYV422,
    AV_PIX_FMT_XV30LE,
    AV_PIX_FMT_XV36LE,
    AV_PIX_FMT_VUYX,
    AV_PIX_FMT_YUV444P10MSBLE,
    AV_PIX_FMT_YUV444P12MSBLE,
#if CONFIG_CUDA
    AV_PIX_FMT_CUDA,
#endif
    /* Hardware frames without an import here: refused by name, or downloaded
     * with import=host (pick_slot()), never converted by an inserted scale. */
    AV_PIX_FMT_DRM_PRIME,
    AV_PIX_FMT_VAAPI,
    AV_PIX_FMT_QSV,
    AV_PIX_FMT_VULKAN,
    AV_PIX_FMT_D3D11,
    AV_PIX_FMT_D3D12,
    AV_PIX_FMT_VIDEOTOOLBOX,
    AV_PIX_FMT_NONE,
};

/* The VMAFx layout of `fmt` and its bit depth; UNKNOWN for another layout. */
static const struct {
    enum AVPixelFormat fmt;
    uint32_t layout;
} layouts[] = {
    {AV_PIX_FMT_NV12, VMAFX_PIXEL_FORMAT_NV12},
    {AV_PIX_FMT_P010LE, VMAFX_PIXEL_FORMAT_P010},
    {AV_PIX_FMT_P016LE, VMAFX_PIXEL_FORMAT_P016},
    {AV_PIX_FMT_NV16, VMAFX_PIXEL_FORMAT_NV16},
    {AV_PIX_FMT_P210LE, VMAFX_PIXEL_FORMAT_P210},
    {AV_PIX_FMT_P216LE, VMAFX_PIXEL_FORMAT_P216},
    {AV_PIX_FMT_NV24, VMAFX_PIXEL_FORMAT_NV24},
    {AV_PIX_FMT_P410LE, VMAFX_PIXEL_FORMAT_P410},
    {AV_PIX_FMT_P416LE, VMAFX_PIXEL_FORMAT_P416},
    {AV_PIX_FMT_Y210LE, VMAFX_PIXEL_FORMAT_Y210},
    {AV_PIX_FMT_Y212LE, VMAFX_PIXEL_FORMAT_Y212},
    {AV_PIX_FMT_YUYV422, VMAFX_PIXEL_FORMAT_YUYV422},
    {AV_PIX_FMT_XV30LE, VMAFX_PIXEL_FORMAT_Y410},
    {AV_PIX_FMT_XV36LE, VMAFX_PIXEL_FORMAT_XV36},
    {AV_PIX_FMT_VUYX, VMAFX_PIXEL_FORMAT_VUYX},
    {AV_PIX_FMT_YUV444P10MSBLE, VMAFX_PIXEL_FORMAT_YUV444P_MSB},
    {AV_PIX_FMT_YUV444P12MSBLE, VMAFX_PIXEL_FORMAT_YUV444P_MSB},
};

/* Planar YUV (or gray) with every sample in the low bits of its own plane:
 * a semi-planar, packed or MSB-aligned layout is not, whatever its
 * component count says. */
static int plain_planar(const AVPixFmtDescriptor *desc)
{
    for (int c = 0; c < desc->nb_components; c++) {
        if (desc->comp[c].plane != c || desc->comp[c].shift != 0 || desc->comp[c].offset != 0 ||
            desc->comp[c].step != (desc->comp[c].depth > 8 ? 2 : 1))
            return 0;
    }
    return 1;
}

static uint32_t layout_of(enum AVPixelFormat fmt, uint32_t *bpc)
{
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(fmt);
    *bpc = desc ? desc->comp[0].depth : 0;
    if (!desc || (desc->flags & (AV_PIX_FMT_FLAG_BE | AV_PIX_FMT_FLAG_HWACCEL)))
        return VMAFX_PIXEL_FORMAT_UNKNOWN;
    for (size_t i = 0; i < FF_ARRAY_ELEMS(layouts); i++) {
        if (layouts[i].fmt == fmt)
            return layouts[i].layout;
    }
    if (!plain_planar(desc) || (desc->flags & AV_PIX_FMT_FLAG_RGB))
        return VMAFX_PIXEL_FORMAT_UNKNOWN;
    if (desc->nb_components == 1)
        return VMAFX_PIXEL_FORMAT_YUV400P;
    if (desc->nb_components != 3)
        return VMAFX_PIXEL_FORMAT_UNKNOWN;
    if (desc->log2_chroma_w == 1)
        return desc->log2_chroma_h == 1 ? VMAFX_PIXEL_FORMAT_YUV420P : VMAFX_PIXEL_FORMAT_YUV422P;
    return desc->log2_chroma_h == 0 ? VMAFX_PIXEL_FORMAT_YUV444P : VMAFX_PIXEL_FORMAT_UNKNOWN;
}

/* The planar layout the library makes of an imported layout. */
static uint32_t planar_of(uint32_t layout)
{
    switch (layout) {
    case VMAFX_PIXEL_FORMAT_NV12:
    case VMAFX_PIXEL_FORMAT_P010:
    case VMAFX_PIXEL_FORMAT_P016:
        return VMAFX_PIXEL_FORMAT_YUV420P;
    case VMAFX_PIXEL_FORMAT_NV16:
    case VMAFX_PIXEL_FORMAT_P210:
    case VMAFX_PIXEL_FORMAT_P216:
    case VMAFX_PIXEL_FORMAT_Y210:
    case VMAFX_PIXEL_FORMAT_Y212:
    case VMAFX_PIXEL_FORMAT_YUYV422:
        return VMAFX_PIXEL_FORMAT_YUV422P;
    case VMAFX_PIXEL_FORMAT_NV24:
    case VMAFX_PIXEL_FORMAT_P410:
    case VMAFX_PIXEL_FORMAT_P416:
    case VMAFX_PIXEL_FORMAT_Y410:
    case VMAFX_PIXEL_FORMAT_XV36:
    case VMAFX_PIXEL_FORMAT_VUYX:
    case VMAFX_PIXEL_FORMAT_YUV444P_MSB:
        return VMAFX_PIXEL_FORMAT_YUV444P;
    default:
        return layout;
    }
}

/* Whether the library wraps `layout` (planar) or imports and converts it. */
static int converted_layout(uint32_t layout)
{
    return planar_of(layout) != layout;
}

/* The planar layout and depth a frame of `fmt` is scored in, as one number:
 * the semi-planar, packed and MSB layouts are imported as their planar
 * equivalent, so a decoder's NV12 frames and an uploaded YUV420P reference
 * score together. */
static uint32_t scored_layout(enum AVPixelFormat fmt)
{
    uint32_t bpc = 0;
    const uint32_t layout = layout_of(fmt, &bpc);
    return planar_of(layout) << 8 | bpc;
}

/* ---- Frames ---------------------------------------------------------------- */

/* The layout of a hardware frame's planes: its own frames context's (the two
 * inputs may differ, NV12 from a decoder and YUV420P from an upload). */
static enum AVPixelFormat frame_sw_format(const AVFrame *frame)
{
    return ((const AVHWFramesContext *)frame->hw_frames_ctx->data)->sw_format;
}

/* What the library holds while it reads a frame: the AVFrame (and, for a
 * CUDA frame, its device and release fence), released by the frame's
 * release callback. Two references: the callback's and the importing
 * call's. When an import fails, the library may have run the callback
 * (admission refused a frame it had imported) or not (the import itself
 * failed); the call releases the frame only in the second case. */
typedef struct ReleaseBox {
    atomic_int refs;
    AVFrame *frame;
    AVBufferRef *hwdev; /* CUDA: the device the frame lives on */
    VmafxFence fence;   /* CUDA: recorded behind the last reader; SYCL, HIP: HOST */
} ReleaseBox;

static ReleaseBox *box_new(const AVFrame *frame)
{
    ReleaseBox *b = av_mallocz(sizeof(*b));
    if (!b)
        return NULL;
    atomic_init(&b->refs, 2);
    b->fence = (VmafxFence)VMAFX_FENCE_INIT;
    b->frame = av_frame_clone(frame);
    if (!b->frame) {
        av_free(b);
        return NULL;
    }
    return b;
}

static void box_drop(ReleaseBox *b)
{
    if (atomic_fetch_sub(&b->refs, 1) == 1)
        av_free(b);
}

#if CONFIG_CUDA
static void cuda_release_wait(ReleaseBox *b);
#endif

/* Before a device frame goes back to its producer: its HOST release fence,
 * which the device signals after the frame's last reader (SYCL, HIP). The
 * library enqueues that signal before it runs the release callback. */
#define VMAFX_RELEASE_WAIT_NS UINT64_C(10000000000)

static void box_release_resources(ReleaseBox *b)
{
#if CONFIG_CUDA
    if (b->hwdev)
        cuda_release_wait(b);
#endif
    if (b->fence.kind == VMAFX_FENCE_HOST &&
        vmafx_fence_wait(&b->fence, VMAFX_RELEASE_WAIT_NS, NULL) != VMAFX_OK)
        av_log(NULL, AV_LOG_ERROR, "vmafx: a frame's release fence was not signalled in 10 s\n");
    (void)vmafx_fence_destroy(&b->fence, NULL);
    av_frame_free(&b->frame);
    av_buffer_unref(&b->hwdev);
}

/* The library's release callback (VmafxHostPlanes / VmafxFrameImport). */
static void box_release(void *user)
{
    ReleaseBox *b = user;
    box_release_resources(b);
    box_drop(b);
}

/* The importing call is done: release the frame when the library never ran
 * its callback for a frame it did not take. */
static void box_after_import(ReleaseBox *b, VmafxStatus status)
{
    if (status != VMAFX_OK && atomic_load(&b->refs) == 2) {
        box_release_resources(b);
        box_drop(b);
    }
    box_drop(b);
}

static VmafxStatus host_import(VMAFXContext *s, const AVFrame *frame, uint32_t layout, uint32_t bpc,
                               ReleaseBox *b, const char *input, VmafxFrame **out,
                               VmafxError **error)
{
    VmafxFrameImport imp = VMAFX_FRAME_IMPORT_INIT;
    imp.memory = VMAFX_MEMORY_HOST;
    imp.pix_fmt = layout;
    imp.bpc = bpc;
    imp.w = (uint32_t)frame->width;
    imp.h = (uint32_t)frame->height;
    imp.n_planes = (uint32_t)av_pix_fmt_count_planes(frame->format);
    for (unsigned i = 0; i < imp.n_planes && i < 3; i++) {
        imp.plane[i].handle = (uintptr_t)frame->data[i];
        imp.plane[i].pitch = (uint64_t)frame->linesize[i];
    }
    imp.release = box_release;
    imp.user = b;
    return vmafx_context_import_frame(s->context, NULL, &imp, input, out, error); /* the CPU */
}

static VmafxStatus host_wrap(VMAFXContext *s, const AVFrame *frame, uint32_t layout, uint32_t bpc,
                             ReleaseBox *b, VmafxFrame **out, VmafxError **error)
{
    const VmafxFrameDesc desc = {.struct_size = sizeof(desc),
                                 .pix_fmt = layout,
                                 .bpc = bpc,
                                 .w = (uint32_t)frame->width,
                                 .h = (uint32_t)frame->height};
    VmafxHostPlanes planes = VMAFX_HOST_PLANES_INIT;
    for (unsigned i = 0; i < 3 && frame->data[i]; i++) {
        planes.data[i] = frame->data[i];
        planes.stride[i] = (uint64_t)frame->linesize[i];
    }
    planes.release = box_release;
    planes.user = b;
    return vmafx_frame_wrap_host(NULL, &desc, &planes, out, error); /* the CPU device */
}

/* A host frame on the planes of `frame`, without a copy: the library holds a
 * reference to the AVFrame until it reads none of its planes. Planar layouts
 * are wrapped; the semi-planar, packed and MSB layouts are imported from host
 * memory and converted. A host frame
 * lives on the CPU device whatever device the context scores on; a context on
 * a GPU uploads it. */
static int host_frame(AVFilterContext *ctx, AVFrame *frame, const char *input, VmafxFrame **out)
{
    VMAFXContext *s = ctx->priv;
    uint32_t bpc = 0;
    const uint32_t layout = layout_of(frame->format, &bpc);
    ReleaseBox *b = box_new(frame);
    if (!b)
        return AVERROR(ENOMEM);
    VmafxError *error = NULL;
    const VmafxStatus status = converted_layout(layout) ?
                                   host_import(s, frame, layout, bpc, b, input, out, &error) :
                                   host_wrap(s, frame, layout, bpc, b, out, &error);
    box_after_import(b, status);
    if (status != VMAFX_OK) {
        av_log(ctx, AV_LOG_ERROR, "vmafx: the %s input (%s) cannot be scored\n", input,
               av_get_pix_fmt_name(frame->format));
        return vmafx_fail(ctx, status, error, "vmafx_frame");
    }
    return 0;
}

/* import=host on hardware frames: an explicit download, logged once. */
static int download_frame(AVFilterContext *ctx, AVFrame *frame, const char *input, VmafxFrame **out)
{
    VMAFXContext *s = ctx->priv;
    AVFrame *sw = av_frame_alloc();
    if (!sw)
        return AVERROR(ENOMEM);
    sw->format = frame_sw_format(frame); /* each input keeps its own layout */
    int ret = av_hwframe_transfer_data(sw, frame, 0);
    if (ret >= 0) {
        if (!s->logged_download) {
            av_log(ctx, AV_LOG_INFO,
                   "vmafx: import=host: hardware frames are downloaded before scoring\n");
            s->logged_download = 1;
        }
        ret = host_frame(ctx, sw, input, out);
    }
    av_frame_free(&sw);
    return ret;
}

/* One input frame as a VMAFx frame of the context's device. */
static int to_vmafx_frame(AVFilterContext *ctx, AVFrame *frame, const char *input, VmafxFrame **out)
{
    VMAFXContext *s = ctx->priv;
    *out = NULL;
    if (!s->hw) {
        s->n_host++;
        return host_frame(ctx, frame, input, out);
    }
    if (s->import_mode == 2) {
        s->n_downloaded++;
        return download_frame(ctx, frame, input, out);
    }
    s->n_imported++;
#if CONFIG_VULKAN
    if (s->vulkan)
        return vk_import(ctx, frame, input, out);
#endif
    av_assert0(s->slot && s->slot->import);
    return s->slot->import(ctx, frame, input, out);
}

/* ---- CUDA frames ---------------------------------------------------------------- */

#if CONFIG_CUDA
static AVCUDADeviceContext *cuda_device(AVBufferRef *hwdev)
{
    return ((AVHWDeviceContext *)hwdev->data)->hwctx;
}

/* Before a CUDA frame goes back to its producer: the producer's stream waits
 * on the release event (no host wait). */
static void cuda_release_wait(ReleaseBox *b)
{
    AVCUDADeviceContext *hw = cuda_device(b->hwdev);
    CudaFunctions *cu = hw->internal->cuda_dl;
    CUcontext dummy;
    if (b->fence.kind != VMAFX_FENCE_CUDA_EVENT ||
        cu->cuCtxPushCurrent(hw->cuda_ctx) != CUDA_SUCCESS)
        return;
    (void)cu->cuStreamWaitEvent(hw->stream, (CUevent)b->fence.handle, 0);
    (void)cu->cuCtxPopCurrent(&dummy);
}

static int cuda_open(AVFilterContext *ctx, AVBufferRef *frames)
{
    VMAFXContext *s = ctx->priv;
    AVHWFramesContext *fc = (AVHWFramesContext *)frames->data;
    AVCUDADeviceContext *hw = fc->device_ctx->hwctx;
    VmafxDeviceDesc desc = VMAFX_DEVICE_DESC_INIT;
    desc.backend = VMAFX_BACKEND_CUDA;
    desc.flags = s->profile ? VMAFX_DEVICE_PROFILING : 0;
    desc.external[0] = (uintptr_t)hw->cuda_ctx;
    desc.external[1] = (uintptr_t)hw->stream;
    VmafxError *error = NULL;
    const VmafxStatus status = vmafx_device_create(&desc, &s->vdev, &error);
    return status == VMAFX_OK ? 0 : vmafx_fail(ctx, status, error, "vmafx_device_create(cuda)");
}

/* An acquire fence: an event recorded on the frames' stream, behind the
 * producer's writes. */
static int cuda_acquire(AVCUDADeviceContext *hw, VmafxFence *fence, CUevent *event)
{
    CudaFunctions *cu = hw->internal->cuda_dl;
    CUcontext dummy;
    if (cu->cuCtxPushCurrent(hw->cuda_ctx) != CUDA_SUCCESS)
        return AVERROR_EXTERNAL;
    int ok = cu->cuEventCreate(event, CU_EVENT_DISABLE_TIMING) == CUDA_SUCCESS;
    ok = ok && cu->cuEventRecord(*event, hw->stream) == CUDA_SUCCESS;
    (void)cu->cuCtxPopCurrent(&dummy);
    fence->kind = VMAFX_FENCE_CUDA_EVENT;
    fence->handle = (uintptr_t)*event;
    return ok ? 0 : AVERROR_EXTERNAL;
}

static void cuda_event_destroy(AVCUDADeviceContext *hw, CUevent event)
{
    CudaFunctions *cu = hw->internal->cuda_dl;
    CUcontext dummy;
    if (event && cu->cuCtxPushCurrent(hw->cuda_ctx) == CUDA_SUCCESS) {
        (void)cu->cuEventDestroy(event);
        (void)cu->cuCtxPopCurrent(&dummy);
    }
}

static void cuda_planes(const AVFrame *frame, VmafxFrameImport *imp)
{
    uint32_t bpc = 0;
    imp->memory = VMAFX_MEMORY_DEVICE_POINTER;
    imp->pix_fmt = layout_of(frame_sw_format(frame), &bpc);
    imp->bpc = bpc;
    imp->w = (uint32_t)frame->width;
    imp->h = (uint32_t)frame->height;
    imp->n_planes = (uint32_t)av_pix_fmt_count_planes(frame_sw_format(frame));
    for (unsigned i = 0; i < imp->n_planes; i++) {
        imp->plane[i].handle = (uintptr_t)frame->data[i];
        imp->plane[i].pitch = (uint64_t)frame->linesize[i];
    }
}

/* A CUDA frame imported without a copy, under the import rule (D8). */
static int cuda_import(AVFilterContext *ctx, AVFrame *frame, const char *input, VmafxFrame **out)
{
    VMAFXContext *s = ctx->priv;
    AVHWFramesContext *fc = (AVHWFramesContext *)frame->hw_frames_ctx->data;
    AVCUDADeviceContext *hw = fc->device_ctx->hwctx;
    ReleaseBox *b = box_new(frame);
    if (!b)
        return AVERROR(ENOMEM);
    b->hwdev = av_buffer_ref(fc->device_ref);
    VmafxFrameImport imp = VMAFX_FRAME_IMPORT_INIT;
    cuda_planes(frame, &imp);
    CUevent event = NULL;
    const int ret = b->hwdev ? cuda_acquire(hw, &imp.acquire, &event) : AVERROR(ENOMEM);
    imp.release = box_release;
    imp.user = b;
    VmafxError *error = NULL;
    VmafxStatus status = VMAFX_E_DEVICE;
    if (ret >= 0)
        status = vmafx_context_import_frame(s->context, s->vdev, &imp, input, out, &error);
    cuda_event_destroy(hw, event); /* the import took what it needs */
    if (status == VMAFX_OK)
        status = vmafx_frame_release_fence(*out, VMAFX_FENCE_CUDA_EVENT, &b->fence, &error);
    if (status != VMAFX_OK && *out) {
        vmafx_frame_unref(*out); /* runs box_release() */
        *out = NULL;
    }
    box_after_import(b, status);
    if (status == VMAFX_OK)
        return 0;
    return ret < 0 ? ret : vmafx_fail(ctx, status, error, "vmafx_context_import_frame");
}
#endif /* CONFIG_CUDA */

/* ---- DRM PRIME frames (SYCL, HIP) --------------------------------------------- */

#if CONFIG_LIBDRM
/* The PCI vendor of the GPU behind the DRM device of `frames`: 0x8086 Intel
 * (SYCL), 0x1002 AMD (HIP); 0 when it cannot be read. */
static unsigned drm_vendor(AVBufferRef *frames)
{
    const AVHWFramesContext *fc = (const AVHWFramesContext *)frames->data;
    const AVDRMDeviceContext *drm = fc->device_ctx->hwctx;
    struct stat st;
    char path[96];
    unsigned vendor = 0;
    if (drm->fd < 0 || fstat(drm->fd, &st) != 0 || !S_ISCHR(st.st_mode))
        return 0;
    snprintf(path, sizeof(path), "/sys/dev/char/%u:%u/device/vendor", major(st.st_rdev),
             minor(st.st_rdev));
    FILE *f = fopen(path, "r");
    if (f) {
        if (fscanf(f, "%x", &vendor) != 1)
            vendor = 0;
        fclose(f);
    }
    return vendor;
}

/* The device of the slot's backend that reads the frames: `device` names it
 * (auto: the backend's first GPU). */
static int drm_open(AVFilterContext *ctx, AVBufferRef *frames)
{
    VMAFXContext *s = ctx->priv;
    VmafxDeviceDesc desc = VMAFX_DEVICE_DESC_INIT;
    desc.backend = s->slot->backend;
    char *end = NULL;
    const long index = !s->device || !strcmp(s->device, "auto") ? -1 : strtol(s->device, &end, 10);
    if (end && (*end || index < 0 || index > INT32_MAX)) {
        av_log(ctx, AV_LOG_ERROR, "vmafx: device=%s is not auto or a device index\n", s->device);
        return AVERROR(EINVAL);
    }
    desc.index = (int32_t)index;
    desc.flags = s->profile ? VMAFX_DEVICE_PROFILING : 0;
    (void)frames;
    VmafxError *error = NULL;
    const VmafxStatus status = vmafx_device_create(&desc, &s->vdev, &error);
    return status == VMAFX_OK ? 0 : vmafx_fail(ctx, status, error, "vmafx_device_create(drm)");
}

/* A DRM PRIME frame imported as dma-bufs without a copy: one plane of the
 * import per plane of the frame's layers, in layer order. The producer's
 * writes are ordered by the dma-buf's implicit fences, which the device
 * import waits for (decision D8 retries a busy one); the frame goes back to
 * its producer after its HOST release fence. */
static int drm_import(AVFilterContext *ctx, AVFrame *frame, const char *input, VmafxFrame **out)
{
    VMAFXContext *s = ctx->priv;
    const AVDRMFrameDescriptor *drm = (const AVDRMFrameDescriptor *)frame->data[0];
    VmafxFrameImport imp = VMAFX_FRAME_IMPORT_INIT;
    uint32_t bpc = 0;
    imp.memory = VMAFX_MEMORY_DMABUF;
    imp.pix_fmt = layout_of(frame_sw_format(frame), &bpc);
    imp.bpc = bpc;
    imp.w = (uint32_t)frame->width;
    imp.h = (uint32_t)frame->height;
    for (int l = 0; l < drm->nb_layers; l++) {
        for (int k = 0; k < drm->layers[l].nb_planes && imp.n_planes < 3; k++) {
            const AVDRMPlaneDescriptor *pl = &drm->layers[l].planes[k];
            const AVDRMObjectDescriptor *obj = &drm->objects[pl->object_index];
            VmafxImportPlane *ip = &imp.plane[imp.n_planes++];
            ip->fd = obj->fd;
            ip->size = obj->size;
            ip->offset = (uint64_t)pl->offset;
            ip->pitch = (uint64_t)pl->pitch;
            ip->modifier = obj->format_modifier;
        }
    }
    if ((int)imp.n_planes != av_pix_fmt_count_planes(frame_sw_format(frame))) {
        av_log(ctx, AV_LOG_ERROR, "vmafx: the %s input's DRM frame has %u planes, %s has %d\n",
               input, imp.n_planes, av_get_pix_fmt_name(frame_sw_format(frame)),
               av_pix_fmt_count_planes(frame_sw_format(frame)));
        return AVERROR(EINVAL);
    }
    ReleaseBox *b = box_new(frame);
    if (!b)
        return AVERROR(ENOMEM);
    imp.release = box_release;
    imp.user = b;
    VmafxError *error = NULL;
    VmafxStatus status = vmafx_context_import_frame(s->context, s->vdev, &imp, input, out, &error);
    /* The surface goes back to its pool (a decoder writes it again) only
     * after the device read it: box_release() waits on this fence. */
    if (status == VMAFX_OK)
        status = vmafx_frame_release_fence(*out, VMAFX_FENCE_HOST, &b->fence, &error);
    if (status != VMAFX_OK && *out) {
        vmafx_frame_unref(*out); /* runs box_release() */
        *out = NULL;
    }
    box_after_import(b, status);
    return status == VMAFX_OK ? 0 : vmafx_fail(ctx, status, error, "vmafx_context_import_frame");
}
#endif /* CONFIG_LIBDRM */

/* ---- Vulkan frames (CUDA, SYCL, HIP; ADR-2152) ------------------------------
 * A Vulkan frame is read by the VMAFx device on the same GPU (PCI location,
 * VK_EXT_pci_bus_info), never across GPUs. The library imports one image per
 * plane whose memory the producer exported; FFmpeg's decoder writes one
 * multi-plane image whose memory is not exportable on every driver, and a
 * frames context does not tell whether its pool exports its memory. So each
 * frame is copied on the GPU into a frame of the filter's own pool of
 * exportable per-plane images (AV_VK_FRAME_FLAG_DISABLE_MULTIPLANE): OPTIMAL
 * (device-local) for CUDA, LINEAR for the dma-buf backends (SYCL, HIP). The
 * copy waits on the frames' timeline semaphores and signals them, as FFmpeg's
 * own submissions do, and releases the images to VK_QUEUE_FAMILY_EXTERNAL in
 * VK_IMAGE_LAYOUT_GENERAL. CUDA waits on the timelines on its stream and
 * signals them at the frame's release; SYCL and HIP take the frame after a
 * host wait and hand it back after its HOST release fence. */

#if CONFIG_VULKAN
#define VMAFX_VK_RING 4 /* command buffers in flight */
#define VMAFX_VK_WAIT_NS UINT64_C(10000000000)

/* The Vulkan functions the handover calls, loaded through the device's
 * get_proc_addr (FFmpeg links no Vulkan loader). */
#define VMAFX_VK_INSTANCE_FNS(X)                                                                   \
    X(GetDeviceProcAddr);                                                                          \
    X(GetPhysicalDeviceProperties2);                                                               \
    X(EnumerateDeviceExtensionProperties);
#define VMAFX_VK_DEVICE_FNS(X)                                                                     \
    X(GetDeviceQueue2);                                                                            \
    X(CreateCommandPool);                                                                          \
    X(DestroyCommandPool);                                                                         \
    X(AllocateCommandBuffers);                                                                     \
    X(BeginCommandBuffer);                                                                         \
    X(EndCommandBuffer);                                                                           \
    X(CmdPipelineBarrier2);                                                                        \
    X(CmdCopyImage);                                                                               \
    X(QueueSubmit2);                                                                               \
    X(CreateFence);                                                                                \
    X(DestroyFence);                                                                               \
    X(WaitForFences);                                                                              \
    X(ResetFences);                                                                                \
    X(GetMemoryFdKHR);                                                                             \
    X(GetSemaphoreFdKHR);                                                                          \
    X(WaitSemaphores);                                                                             \
    X(GetImageSubresourceLayout);                                                                  \
    X(GetImageMemoryRequirements2);

typedef struct VMAFXVulkan {
    AVHWDeviceContext *dev; /* the inputs' Vulkan device */
    AVVulkanDeviceContext *hw;
    uint32_t pci[4];
    uint32_t vendor; /* PCI vendor of the GPU */
    uint32_t qf;     /* queue family of the copies */
    VkQueue queue;
    VkCommandPool pool;
    VkCommandBuffer cmd[VMAFX_VK_RING];
    VkFence fence[VMAFX_VK_RING];
    unsigned next;
    AVBufferRef *copies[2]; /* per input: the exportable per-plane frames */
    int device_waits;       /* the device waits on and signals the timelines (CUDA) */
#define VMAFX_VK_FN(name) PFN_vk##name name
    VMAFX_VK_INSTANCE_FNS(VMAFX_VK_FN)
    VMAFX_VK_DEVICE_FNS(VMAFX_VK_FN)
#undef VMAFX_VK_FN
} VMAFXVulkan;

static int vk_load(VMAFXVulkan *v)
{
    const PFN_vkGetInstanceProcAddr gipa = v->hw->get_proc_addr;
    int ok = gipa != NULL;
#define VMAFX_VK_FN(name)                                                                          \
    v->name = ok ? (PFN_vk##name)gipa(v->hw->inst, "vk" #name) : NULL;                             \
    ok = ok && v->name
    VMAFX_VK_INSTANCE_FNS(VMAFX_VK_FN)
#undef VMAFX_VK_FN
#define VMAFX_VK_FN(name)                                                                          \
    v->name = ok ? (PFN_vk##name)v->GetDeviceProcAddr(v->hw->act_dev, "vk" #name) : NULL;          \
    ok = ok && v->name
    VMAFX_VK_DEVICE_FNS(VMAFX_VK_FN)
#undef VMAFX_VK_FN
    return ok ? 0 : AVERROR(ENOSYS);
}

static int vk_has_pci_info(VMAFXVulkan *v)
{
    uint32_t n = 0;
    int found = 0;
    if (v->EnumerateDeviceExtensionProperties(v->hw->phys_dev, NULL, &n, NULL) != VK_SUCCESS)
        return 0;
    VkExtensionProperties *ext = av_malloc_array(n ? n : 1, sizeof(*ext));
    if (ext && v->EnumerateDeviceExtensionProperties(v->hw->phys_dev, NULL, &n, ext) == VK_SUCCESS)
        for (uint32_t i = 0; i < n && !found; i++)
            found = !strcmp(ext[i].extensionName, VK_EXT_PCI_BUS_INFO_EXTENSION_NAME);
    av_free(ext);
    return found;
}

/* The PCI location of the Vulkan device; ENOSYS when its driver has none. */
static int vk_pci(VMAFXVulkan *v)
{
    VkPhysicalDevicePCIBusInfoPropertiesEXT bus = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT,
    };
    VkPhysicalDeviceProperties2 props = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
        .pNext = &bus,
    };
    if (!vk_has_pci_info(v))
        return AVERROR(ENOSYS);
    v->GetPhysicalDeviceProperties2(v->hw->phys_dev, &props);
    v->vendor = props.properties.vendorID;
    v->pci[0] = bus.pciDomain;
    v->pci[1] = bus.pciBus;
    v->pci[2] = bus.pciDevice;
    v->pci[3] = bus.pciFunction;
    return 0;
}

/* A queue family of the device that copies images (graphics or compute
 * queues transfer too). */
static int vk_queue_family(const AVVulkanDeviceContext *hw)
{
    for (int i = 0; i < hw->nb_qf; i++)
        if (hw->qf[i].flags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))
            return hw->qf[i].idx;
    return -1;
}

static int vk_ring_open(VMAFXVulkan *v)
{
    const int qf = vk_queue_family(v->hw);
    const VkCommandPoolCreateInfo pi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = (uint32_t)qf,
    };
    const VkCommandBufferAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = VMAFX_VK_RING,
    };
    const VkFenceCreateInfo fi = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };
    if (qf < 0 || v->CreateCommandPool(v->hw->act_dev, &pi, v->hw->alloc, &v->pool) != VK_SUCCESS)
        return AVERROR_EXTERNAL;
    v->qf = (uint32_t)qf;
    /* The queue as the device created it (its flags: internally synchronized
     * queues where the driver has them). */
    const VkDeviceQueueInfo2 qi = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2,
        .flags = v->hw->queue_flags,
        .queueFamilyIndex = v->qf,
    };
    v->GetDeviceQueue2(v->hw->act_dev, &qi, &v->queue);
    if (!v->queue)
        return AVERROR_EXTERNAL;
    VkCommandBufferAllocateInfo a = ai;
    a.commandPool = v->pool;
    if (v->AllocateCommandBuffers(v->hw->act_dev, &a, v->cmd) != VK_SUCCESS)
        return AVERROR_EXTERNAL;
    for (int i = 0; i < VMAFX_VK_RING; i++)
        if (v->CreateFence(v->hw->act_dev, &fi, v->hw->alloc, &v->fence[i]) != VK_SUCCESS)
            return AVERROR_EXTERNAL;
    return 0;
}

static void vk_close(VMAFXContext *s)
{
    VMAFXVulkan *v = s->vk;
    if (!v)
        return;
    for (int i = 0; i < VMAFX_VK_RING; i++) {
        if (!v->fence[i])
            continue;
        (void)v->WaitForFences(v->hw->act_dev, 1, &v->fence[i], VK_TRUE, VMAFX_VK_WAIT_NS);
        v->DestroyFence(v->hw->act_dev, v->fence[i], v->hw->alloc);
    }
    if (v->pool)
        v->DestroyCommandPool(v->hw->act_dev, v->pool, v->hw->alloc); /* frees the buffers */
    av_buffer_unref(&v->copies[0]);
    av_buffer_unref(&v->copies[1]);
    av_freep(&s->vk);
}

/* Device of VMAFx backend `backend` at the Vulkan device's PCI location, or
 * -1 (no such device, or the backend is not in this build). */
static int32_t vk_device_at(uint32_t backend, const uint32_t pci[4])
{
    uint32_t n = 0;
    if (vmafx_device_count(backend, &n, NULL) != VMAFX_OK)
        return -1;
    for (uint32_t i = 0; i < n && i <= INT32_MAX; i++) {
        VmafxDeviceInfo info = VMAFX_DEVICE_INFO_INIT;
        if (vmafx_device_info(backend, (int32_t)i, &info, NULL) == VMAFX_OK &&
            !memcmp(info.pci, pci, sizeof(info.pci)))
            return (int32_t)i;
    }
    return -1;
}

/* The VMAFx device on the Vulkan device's GPU: of the `backend` option's
 * backend, or with backend=auto of the backend of the GPU's vendor. */
static int vk_pick_device(AVFilterContext *ctx)
{
    VMAFXContext *s = ctx->priv;
    const VMAFXVulkan *v = s->vk;
    const int slot = s->backend != 0 ? s->backend : vendor_slot(v->vendor);
    const int32_t index =
        slot >= 2 && slot <= 4 ? vk_device_at(backend_slots[slot].backend, v->pci) : -1;
    if (index < 0) {
        av_log(ctx, AV_LOG_ERROR,
               "vmafx: backend %s has no device on the Vulkan device's GPU (PCI vendor 0x%04x, "
               "location %04x:%02x:%02x.%x); Vulkan frames are never read across GPUs\n",
               slot >= 2 && slot <= 4 ? backend_slots[slot].name : "auto", v->vendor, v->pci[0],
               v->pci[1], v->pci[2], v->pci[3]);
        return AVERROR(ENODEV);
    }
    VmafxDeviceDesc desc = VMAFX_DEVICE_DESC_INIT;
    desc.backend = backend_slots[slot].backend;
    desc.index = index;
    desc.flags = s->profile ? VMAFX_DEVICE_PROFILING : 0;
    s->slot = &backend_slots[slot];
    VmafxError *error = NULL;
    const VmafxStatus st = vmafx_device_create(&desc, &s->vdev, &error);
    return st == VMAFX_OK ? 0 : vmafx_fail(ctx, st, error, "vmafx_device_create(vulkan)");
}

/* The VMAFx layouts a Vulkan frame takes: planar and semi-planar, one image
 * per plane. A packed layout (one plane of several components) is refused. */
static int vk_layout_ok(enum AVPixelFormat sw)
{
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(sw);
    return desc && (av_pix_fmt_count_planes(sw) > 1 || desc->nb_components == 1);
}

static int vk_open(AVFilterContext *ctx, AVBufferRef *frames)
{
    VMAFXContext *s = ctx->priv;
    const AVHWFramesContext *fc = (const AVHWFramesContext *)frames->data;
    if (!vk_layout_ok(s->sw_format)) {
        av_log(ctx, AV_LOG_ERROR, "vmafx: Vulkan frames of layout %s (packed) cannot be scored\n",
               av_get_pix_fmt_name(s->sw_format));
        return AVERROR(EINVAL);
    }
    s->vk = av_mallocz(sizeof(*s->vk));
    if (!s->vk)
        return AVERROR(ENOMEM);
    s->vk->dev = fc->device_ctx;
    s->vk->hw = fc->device_ctx->hwctx;
    int ret = vk_load(s->vk);
    if (ret >= 0 && (ret = vk_pci(s->vk)) < 0)
        av_log(ctx, AV_LOG_ERROR,
               "vmafx: the Vulkan device reports no PCI location "
               "(VK_EXT_pci_bus_info), so its GPU cannot be matched\n");
    if (ret >= 0)
        ret = vk_ring_open(s->vk);
    if (ret >= 0)
        ret = vk_pick_device(ctx);
    if (ret >= 0)
        s->vk->device_waits = s->slot->backend == VMAFX_BACKEND_CUDA;
    return ret;
}

/* The filter's pool of exportable per-plane frames for input `k`. */
static int vk_copies(VMAFXContext *s, int k, const AVFrame *frame)
{
    VMAFXVulkan *v = s->vk;
    if (v->copies[k])
        return 0;
    v->copies[k] =
        av_hwframe_ctx_alloc(((AVHWFramesContext *)frame->hw_frames_ctx->data)->device_ref);
    if (!v->copies[k])
        return AVERROR(ENOMEM);
    AVHWFramesContext *fc = (AVHWFramesContext *)v->copies[k]->data;
    AVVulkanFramesContext *vfc = fc->hwctx;
    fc->format = AV_PIX_FMT_VULKAN;
    fc->sw_format = frame_sw_format(frame);
    fc->width = frame->width;
    fc->height = frame->height;
    vfc->tiling = v->device_waits ? VK_IMAGE_TILING_OPTIMAL : VK_IMAGE_TILING_LINEAR;
    vfc->flags = AV_VK_FRAME_FLAG_DISABLE_MULTIPLANE;
    const int ret = av_hwframe_ctx_init(v->copies[k]);
    if (ret < 0)
        av_buffer_unref(&v->copies[k]);
    return ret;
}

static int vk_images(const AVVkFrame *f)
{
    int n = 0;
    while (n < AV_NUM_DATA_POINTERS && f->img[n])
        n++;
    return n;
}

/* A barrier of image `i` to `layout`; from VK_QUEUE_FAMILY_EXTERNAL (a copy
 * frame the library had) back to the copies' queue family. */
static VkImageMemoryBarrier2 vk_barrier(const VMAFXVulkan *v, AVVkFrame *f, int i,
                                        VkImageLayout layout, VkAccessFlags2 access)
{
    const int external = f->queue_family[i] == VK_QUEUE_FAMILY_EXTERNAL;
    const VkImageMemoryBarrier2 b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .srcAccessMask = f->access[i],
        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = access,
        .oldLayout = f->layout[i],
        .newLayout = layout,
        .srcQueueFamilyIndex = external ? VK_QUEUE_FAMILY_EXTERNAL : VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = external ? v->qf : VK_QUEUE_FAMILY_IGNORED,
        .image = f->img[i],
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    f->layout[i] = layout;
    f->access[i] = access;
    if (external)
        f->queue_family[i] = v->qf;
    return b;
}

/* The handover barrier of image `i` (FFmpeg's PREP_MODE_EXTERNAL_EXPORT):
 * VK_IMAGE_LAYOUT_GENERAL, released to VK_QUEUE_FAMILY_EXTERNAL. */
static VkImageMemoryBarrier2 vk_export_barrier(const VMAFXVulkan *v, AVVkFrame *f, int i)
{
    VkImageMemoryBarrier2 b =
        vk_barrier(v, f, i, VK_IMAGE_LAYOUT_GENERAL,
                   VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    b.srcQueueFamilyIndex = v->qf;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    f->queue_family[i] = VK_QUEUE_FAMILY_EXTERNAL;
    return b;
}

static void vk_barriers(const VMAFXVulkan *v, VkCommandBuffer cmd, const VkImageMemoryBarrier2 *bar,
                        int n)
{
    const VkDependencyInfo dep = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = (uint32_t)n,
        .pImageMemoryBarriers = bar,
    };
    v->CmdPipelineBarrier2(cmd, &dep);
}

/* Plane `p` of `src` (an aspect of its one multi-plane image, or its own
 * image) copied into image `p` of `dst`. */
static void vk_copy_plane(const VMAFXVulkan *v, VkCommandBuffer cmd, const AVFrame *frame,
                          const AVVkFrame *src, const AVVkFrame *dst, int p, int n_planes)
{
    const AVPixFmtDescriptor *d = av_pix_fmt_desc_get(frame_sw_format(frame));
    const int sub = p == 1 || p == 2;
    const int multi = vk_images(src) == 1 && n_planes > 1;
    const VkImageCopy c = {
        .srcSubresource = {multi ? (VkImageAspectFlags)VK_IMAGE_ASPECT_PLANE_0_BIT << p :
                                   VK_IMAGE_ASPECT_COLOR_BIT,
                           0, 0, 1},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .extent = {sub ? AV_CEIL_RSHIFT(frame->width, d->log2_chroma_w) : frame->width,
                   sub ? AV_CEIL_RSHIFT(frame->height, d->log2_chroma_h) : frame->height, 1},
    };
    v->CmdCopyImage(cmd, src->img[multi ? 0 : p], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst->img[p],
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
}

/* The copy of `frame` into `copy` and the handover of `copy`'s images. */
static void vk_record(const VMAFXVulkan *v, VkCommandBuffer cmd, const AVFrame *frame,
                      AVVkFrame *src, AVVkFrame *dst, int n_planes)
{
    VkImageMemoryBarrier2 bar[2 * AV_NUM_DATA_POINTERS];
    int nb = 0;
    for (int i = 0; i < vk_images(src); i++)
        bar[nb++] = vk_barrier(v, src, i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               VK_ACCESS_2_TRANSFER_READ_BIT);
    for (int i = 0; i < n_planes; i++)
        bar[nb++] = vk_barrier(v, dst, i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               VK_ACCESS_2_TRANSFER_WRITE_BIT);
    vk_barriers(v, cmd, bar, nb);
    for (int p = 0; p < n_planes; p++)
        vk_copy_plane(v, cmd, frame, src, dst, p, n_planes);
    nb = 0;
    for (int i = 0; i < n_planes; i++)
        bar[nb++] = vk_export_barrier(v, dst, i);
    vk_barriers(v, cmd, bar, nb);
}

/* Submit `cmd` behind the timelines of the images of `f[0..1]`: each waited
 * at its value and signalled one higher, as FFmpeg's submissions do. */
static int vk_submit(VMAFXVulkan *v, VkCommandBuffer cmd, VkFence fence, AVVkFrame *const f[2])
{
    VkSemaphoreSubmitInfo wait[2 * AV_NUM_DATA_POINTERS], sig[2 * AV_NUM_DATA_POINTERS];
    int k = 0;
    for (int j = 0; j < 2; j++)
        for (int i = 0; i < vk_images(f[j]); i++, k++) {
            wait[k] = (VkSemaphoreSubmitInfo){
                .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                .semaphore = f[j]->sem[i],
                .value = f[j]->sem_value[i],
                .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            };
            sig[k] = wait[k];
            sig[k].value = wait[k].value + 1;
        }
    const VkCommandBufferSubmitInfo cb = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .commandBuffer = cmd,
    };
    const VkSubmitInfo2 si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .waitSemaphoreInfoCount = (uint32_t)k,
        .pWaitSemaphoreInfos = wait,
        .commandBufferInfoCount = 1,
        .pCommandBufferInfos = &cb,
        .signalSemaphoreInfoCount = (uint32_t)k,
        .pSignalSemaphoreInfos = sig,
    };
    VkResult r = v->EndCommandBuffer(cmd);
    if (r == VK_SUCCESS) {
#if FF_API_VULKAN_SYNC_QUEUES
        FF_DISABLE_DEPRECATION_WARNINGS
        v->hw->lock_queue(v->dev, v->qf, 0);
        r = v->QueueSubmit2(v->queue, 1, &si, fence);
        v->hw->unlock_queue(v->dev, v->qf, 0);
        FF_ENABLE_DEPRECATION_WARNINGS
#else
        r = v->QueueSubmit2(v->queue, 1, &si, fence);
#endif
    }
    if (r != VK_SUCCESS)
        return AVERROR_EXTERNAL;
    for (int j = 0; j < 2; j++)
        for (int i = 0; i < vk_images(f[j]); i++)
            f[j]->sem_value[i]++;
    return 0;
}

/* The next command buffer of the ring, once its last submission finished. */
static VkCommandBuffer vk_begin(VMAFXVulkan *v, VkFence *fence)
{
    const unsigned n = v->next++ % VMAFX_VK_RING;
    const VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    if (v->WaitForFences(v->hw->act_dev, 1, &v->fence[n], VK_TRUE, VMAFX_VK_WAIT_NS) !=
            VK_SUCCESS ||
        v->ResetFences(v->hw->act_dev, 1, &v->fence[n]) != VK_SUCCESS ||
        v->BeginCommandBuffer(v->cmd[n], &bi) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    *fence = v->fence[n];
    return v->cmd[n];
}

/* `frame` copied on the GPU into a new frame of the input's pool; NULL when
 * the copy cannot be made. Both frames are locked while their timelines and
 * layouts change. */
static AVFrame *vk_device_copy(VMAFXContext *s, int k, const AVFrame *frame)
{
    VMAFXVulkan *v = s->vk;
    AVFrame *copy = av_frame_alloc();
    if (!copy || vk_copies(s, k, frame) < 0 || av_hwframe_get_buffer(v->copies[k], copy, 0) < 0) {
        av_frame_free(&copy);
        return NULL;
    }
    AVHWFramesContext *sfc = (AVHWFramesContext *)frame->hw_frames_ctx->data;
    AVHWFramesContext *dfc = (AVHWFramesContext *)copy->hw_frames_ctx->data;
    AVVulkanFramesContext *svk = sfc->hwctx, *dvk = dfc->hwctx;
    AVVkFrame *const f[2] = {(AVVkFrame *)frame->data[0], (AVVkFrame *)copy->data[0]};
    const int n_planes = av_pix_fmt_count_planes(frame_sw_format(frame));
    int ok = vk_images(f[1]) == n_planes && (vk_images(f[0]) == 1 || vk_images(f[0]) == n_planes);
    svk->lock_frame(sfc, f[0]);
    dvk->lock_frame(dfc, f[1]);
    VkFence fence = VK_NULL_HANDLE;
    VkCommandBuffer cmd = ok ? vk_begin(v, &fence) : VK_NULL_HANDLE;
    if (cmd)
        vk_record(v, cmd, frame, f[0], f[1], n_planes);
    ok = cmd && vk_submit(v, cmd, fence, f) >= 0;
    dvk->unlock_frame(dfc, f[1]);
    svk->unlock_frame(sfc, f[0]);
    if (!ok)
        av_frame_free(&copy);
    return copy;
}

static int vk_semaphore_fd(const VMAFXVulkan *v, VkSemaphore sem)
{
    const VkSemaphoreGetFdInfoKHR info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
        .semaphore = sem,
        .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT,
    };
    int fd = -1;
    return v->GetSemaphoreFdKHR(v->hw->act_dev, &info, &fd) == VK_SUCCESS ? fd : -1;
}

/* Whether image `i` of `f` is a dedicated allocation: FFmpeg allocates one
 * when the driver prefers or requires it. */
static int vk_dedicated(const VMAFXVulkan *v, const AVVkFrame *f, int i)
{
    VkMemoryDedicatedRequirements ded = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    VkMemoryRequirements2 req = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, .pNext = &ded};
    const VkImageMemoryRequirementsInfo2 info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
        .image = f->img[i],
    };
    v->GetImageMemoryRequirements2(v->hw->act_dev, &info, &req);
    return ded.prefersDedicatedAllocation || ded.requiresDedicatedAllocation;
}

/* Plane `i` of the copy: its exported memory, where its rows are, and (on a
 * device that waits on them) its timeline as acquire fence `acq`. */
static int vk_describe_plane(const VMAFXVulkan *v, const AVVkFrame *f, int i, VmafxFrameImport *imp,
                             VmafxFence *acq)
{
    const VkMemoryGetFdInfoKHR mi = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
        .memory = f->mem[i],
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
    };
    VmafxImportPlane *pl = &imp->plane[i];
    int ok = v->GetMemoryFdKHR(v->hw->act_dev, &mi, &pl->fd) == VK_SUCCESS;
    pl->size = f->size[i];
    pl->offset = (uint64_t)f->offset[i];
    if (f->tiling == VK_IMAGE_TILING_LINEAR) {
        const VkImageSubresource sub = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT};
        VkSubresourceLayout layout;
        v->GetImageSubresourceLayout(v->hw->act_dev, f->img[i], &sub, &layout);
        pl->offset += layout.offset;
        pl->pitch = layout.rowPitch;
    }
    if (vk_dedicated(v, f, i))
        imp->vulkan_flags |= VMAFX_VULKAN_DEDICATED;
    if (v->device_waits) {
        acq->kind = VMAFX_FENCE_VULKAN_SEMAPHORE;
        acq->fd = vk_semaphore_fd(v, f->sem[i]);
        acq->value = f->sem_value[i];
        ok = ok && acq->fd >= 0;
    }
    return ok;
}

static VmafxFence *vk_acquire_fence(VmafxFrameImport *imp, int i)
{
    return i == 0 ? &imp->acquire : &imp->acquire_more[i - 1];
}

static void vk_close_descriptors(VmafxFrameImport *imp)
{
    for (unsigned i = 0; i < imp->n_planes && i < 3; i++) {
        if (imp->plane[i].fd >= 0)
            close(imp->plane[i].fd);
        if (vk_acquire_fence(imp, i)->fd >= 0)
            close(vk_acquire_fence(imp, i)->fd);
    }
}

/* The descriptor of copy frame `copy`: one exported image per plane. */
static int vk_describe(const VMAFXVulkan *v, const AVFrame *copy, VmafxFrameImport *imp)
{
    const AVVkFrame *f = (const AVVkFrame *)copy->data[0];
    uint32_t bpc = 0;
    imp->memory = VMAFX_MEMORY_VULKAN;
    imp->pix_fmt = layout_of(frame_sw_format(copy), &bpc);
    imp->bpc = bpc;
    imp->w = (uint32_t)copy->width;
    imp->h = (uint32_t)copy->height;
    imp->n_planes = (uint32_t)av_pix_fmt_count_planes(frame_sw_format(copy));
    imp->vulkan_handle_type = VMAFX_VULKAN_HANDLE_OPAQUE_FD;
    imp->vulkan_tiling = (uint32_t)f->tiling; /* the values equal VkImageTiling */
    memcpy(imp->vulkan_pci, v->pci, sizeof(imp->vulkan_pci));
    for (unsigned i = 0; i < 3; i++) {
        imp->plane[i].fd = -1;
        vk_acquire_fence(imp, (int)i)->fd = -1;
    }
    int ok = imp->n_planes <= 3;
    for (unsigned i = 0; i < imp->n_planes && ok; i++)
        ok = vk_describe_plane(v, f, (int)i, imp, vk_acquire_fence(imp, (int)i));
    return ok ? 0 : AVERROR_EXTERNAL;
}

/* SYCL and HIP wait on no Vulkan semaphore: the copy is waited for on the
 * host, so the frame needs no acquire fence. */
static int vk_host_wait(const VMAFXVulkan *v, const AVFrame *copy)
{
    const AVVkFrame *f = (const AVVkFrame *)copy->data[0];
    const VkSemaphoreWaitInfo wi = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .semaphoreCount = (uint32_t)vk_images(f),
        .pSemaphores = f->sem,
        .pValues = f->sem_value,
    };
    return v->WaitSemaphores(v->hw->act_dev, &wi, VMAFX_VK_WAIT_NS) == VK_SUCCESS ?
               0 :
               AVERROR_EXTERNAL;
}

/* CUDA signals each image's timeline one higher behind the frame's last
 * reader; the next copy into the frame waits for that value. */
static VmafxStatus vk_signal_release(const VMAFXVulkan *v, VmafxFrame *frame, AVVkFrame *f,
                                     VmafxError **error)
{
    VmafxStatus st = VMAFX_OK;
    for (int i = 0; i < vk_images(f) && st == VMAFX_OK; i++) {
        VmafxFence sig = VMAFX_FENCE_INIT;
        sig.kind = VMAFX_FENCE_VULKAN_SEMAPHORE;
        sig.fd = vk_semaphore_fd(v, f->sem[i]);
        sig.value = f->sem_value[i] + 1;
        st = sig.fd >= 0 ? vmafx_frame_signal_on_release(frame, &sig, error) : VMAFX_E_DEVICE;
        if (sig.fd >= 0)
            close(sig.fd);
        if (st == VMAFX_OK)
            f->sem_value[i]++;
    }
    return st;
}

/* The copy imported (the library takes its descriptors' duplicates), with
 * the release the backend needs: the timelines on CUDA, a HOST release fence
 * waited for before the copy goes back to the pool on SYCL and HIP. */
static VmafxStatus vk_import_copy(VMAFXContext *s, AVFrame *copy, ReleaseBox *b, const char *input,
                                  VmafxFrame **out, VmafxError **error)
{
    VMAFXVulkan *v = s->vk;
    VmafxFrameImport imp = VMAFX_FRAME_IMPORT_INIT;
    if (vk_describe(v, copy, &imp) < 0 || (!v->device_waits && vk_host_wait(v, copy) < 0)) {
        vk_close_descriptors(&imp);
        return VMAFX_E_DEVICE;
    }
    if (v->device_waits && planar_of(imp.pix_fmt) == imp.pix_fmt)
        imp.flags |= VMAFX_IMPORT_ALLOW_COPY; /* planar CUDA arrays: the library's
                                             device copy */
    imp.release = box_release;
    imp.user = b;
    VmafxStatus st = vmafx_context_import_frame(s->context, s->vdev, &imp, input, out, error);
    vk_close_descriptors(&imp);
    if (st == VMAFX_OK)
        st = v->device_waits ? vk_signal_release(v, *out, (AVVkFrame *)copy->data[0], error) :
                               vmafx_frame_release_fence(*out, VMAFX_FENCE_HOST, &b->fence, error);
    return st;
}

/* A Vulkan frame: copied on the GPU into an exportable per-plane frame, which
 * the device imports without a host copy. */
static int vk_import(AVFilterContext *ctx, AVFrame *frame, const char *input, VmafxFrame **out)
{
    VMAFXContext *s = ctx->priv;
    AVFrame *copy = vk_device_copy(s, strcmp(input, "main") != 0, frame);
    if (!copy) {
        av_log(ctx, AV_LOG_ERROR,
               "vmafx: the %s input's Vulkan frame cannot be copied for the "
               "handover\n",
               input);
        return AVERROR_EXTERNAL;
    }
    ReleaseBox *b = box_new(copy);
    av_frame_free(&copy);
    if (!b)
        return AVERROR(ENOMEM);
    VmafxError *error = NULL;
    VmafxStatus st = vk_import_copy(s, b->frame, b, input, out, &error);
    if (st != VMAFX_OK && *out) {
        vmafx_frame_unref(*out); /* runs box_release() */
        *out = NULL;
    }
    box_after_import(b, st);
    return st == VMAFX_OK ? 0 : vmafx_fail(ctx, st, error, "vmafx_context_import_frame(vulkan)");
}
#endif /* CONFIG_VULKAN */

/* ---- Windows (n_stats, #2138) ------------------------------------------------------ */

/* Bit p of the `pool` option is VmafxPool p + 1. */
static uint32_t pool_mask(const VMAFXContext *s)
{
    return (uint32_t)s->pool << 1;
}

static const char *const pool_names[] = VMAFX_OPT_POOL_VALUES;

/* One `"name":{"mean":v,...}` member of a window line. */
static void window_model(AVBPrint *bp, const VMAFXContext *s, const VmafxWindowResult *r)
{
    av_bprintf(bp, ",\"%s\":{", r->name);
    unsigned n = 0;
    for (unsigned p = 1; p < FF_ARRAY_ELEMS(r->value) && p <= FF_ARRAY_ELEMS(pool_names); p++) {
        if (!(r->pool_mask & (1u << p)))
            continue;
        av_bprintf(bp, "%s\"%s\":", n++ ? "," : "", pool_names[p - 1]);
        av_bprintf(bp, s->score_fmt, r->value[p]);
    }
    av_bprintf(bp, "}");
}

/* Frame metadata of a window, for the next output frame. */
static void window_metadata(VMAFXContext *s, const VmafxWindowResult *r, AVDictionary **meta)
{
    char key[128];
    char value[64];
    for (unsigned p = 1; p < FF_ARRAY_ELEMS(r->value) && p <= FF_ARRAY_ELEMS(pool_names); p++) {
        if (!(r->pool_mask & (1u << p)))
            continue;
        snprintf(key, sizeof(key), "lavfi.vmafx.window.%s.%s", r->name, pool_names[p - 1]);
        snprintf(value, sizeof(value), s->score_fmt, r->value[p]);
        av_dict_set(meta, key, value, 0);
    }
}

/* Write a completed window: one log line, one NDJSON object, metadata. */
static int window_emit(AVFilterContext *ctx, VMAFXSpan *sp, const VmafxWindowResult *res)
{
    VMAFXContext *s = ctx->priv;
    const VmafxWindowSpan *w = &sp->span;
    const int partial = (w->flags | res[0].flags) & VMAFX_WINDOW_PARTIAL ? 1 : 0;
    AVBPrint bp;
    av_bprint_init(&bp, 0, AV_BPRINT_SIZE_UNLIMITED);
    av_bprintf(&bp,
               "{\"window\":%" PRIu64 ",\"start\":%.9f,\"end\":%.9f,\"first\":%" PRIu64
               ",\"last\":%" PRIu64 ",\"n_frames\":%" PRIu64 ",\"n_scored\":%" PRIu64
               ",\"partial\":%s",
               w->window, w->start_ns / 1e9, w->end_ns / 1e9, res[0].first,
               res[0].first + res[0].n_frames - 1, res[0].n_frames, res[0].n_scored,
               partial ? "true" : "false");
    AVDictionary *meta = NULL;
    for (unsigned m = 0; m < s->n_models; m++) {
        window_model(&bp, s, &res[m]);
        window_metadata(s, &res[m], &meta);
    }
    av_bprintf(&bp, "}");
    if (!av_bprint_is_complete(&bp)) {
        av_dict_free(&meta);
        av_bprint_finalize(&bp, NULL);
        return AVERROR(ENOMEM);
    }
    if (s->stats_out & 1)
        av_log(ctx, AV_LOG_INFO, "vmafx window: %s\n", bp.str);
    if ((s->stats_out & 4) && s->stats_file)
        fprintf(s->stats_file, "%s\n", bp.str);
    if (s->stats_out & 2) {
        av_freep(&s->window_meta);
        av_dict_get_string(meta, &s->window_meta, '=', ',');
    }
    av_dict_free(&meta);
    av_bprint_finalize(&bp, NULL);
    return 0;
}

/* Collect the oldest windows that completed; with `wait`, wait for all. */
static int windows_collect(AVFilterContext *ctx, int wait)
{
    VMAFXContext *s = ctx->priv;
    while (s->span_count) {
        VMAFXSpan *sp = &s->spans[s->span_head];
        VmafxWindowResult res[VMAFX_MAX_MODELS];
        for (unsigned m = 0; m < s->n_models; m++) {
            res[m] = (VmafxWindowResult)VMAFX_WINDOW_RESULT_INIT;
            const VmafxStatus st = wait ?
                                       vmafx_window_wait(sp->window[m], UINT64_MAX, &res[m], NULL) :
                                       vmafx_window_poll(sp->window[m], &res[m], NULL);
            if (st == VMAFX_PENDING)
                return 0;
            if (st != VMAFX_OK || res[m].status != VMAFX_OK)
                return vmafx_fail(ctx, st != VMAFX_OK ? st : res[m].status, NULL, "vmafx window");
        }
        const int ret = window_emit(ctx, sp, res);
        for (unsigned m = 0; m < s->n_models; m++)
            vmafx_window_release(sp->window[m]);
        s->span_head = (s->span_head + 1) % VMAFX_MAX_SPANS;
        s->span_count--;
        if (ret < 0)
            return ret;
    }
    return 0;
}

/* Submit the span the clock cut: one window per model. */
static int windows_submit(AVFilterContext *ctx, const VmafxWindowSpan *span)
{
    VMAFXContext *s = ctx->priv;
    if (s->span_count == VMAFX_MAX_SPANS) {
        int ret = windows_collect(ctx, 1); /* bounded: wait for the oldest */
        if (ret < 0)
            return ret;
    }
    VMAFXSpan *sp = &s->spans[(s->span_head + s->span_count) % VMAFX_MAX_SPANS];
    sp->span = *span;
    for (unsigned m = 0; m < s->n_models; m++) {
        VmafxWindowRequest req = VMAFX_WINDOW_REQUEST_INIT;
        req.target = VMAFX_WINDOW_TARGET_MODEL;
        req.pool_mask = pool_mask(s);
        req.first = span->first;
        req.last = span->last;
        req.model = s->models[m];
        VmafxError *error = NULL;
        const VmafxStatus st = vmafx_window_submit(s->context, &req, &sp->window[m], &error);
        if (st != VMAFX_OK) {
            for (unsigned k = 0; k < m; k++)
                vmafx_window_release(sp->window[k]);
            return vmafx_fail(ctx, st, error, "vmafx_window_submit");
        }
    }
    s->span_count++;
    return 0;
}

/* Tell the clock frame `index`; submit the window it completes (WP9-1). */
static int windows_frame(AVFilterContext *ctx, uint64_t index, const AVFrame *frame)
{
    VMAFXContext *s = ctx->priv;
    if (!s->clock)
        return 0;
    const AVFilterLink *main = ctx->inputs[0];
    if (frame->pts == AV_NOPTS_VALUE && s->n_stats > 0) {
        av_log(ctx, AV_LOG_ERROR, "vmafx: n_stats needs timestamps; frame %" PRIu64 " has none\n",
               index);
        return AVERROR(EINVAL);
    }
    const int64_t pts_ns = frame->pts == AV_NOPTS_VALUE ?
                               (int64_t)index :
                               av_rescale_q(frame->pts, main->time_base, VMAFX_NS);
    VmafxWindowSpan span = VMAFX_WINDOW_SPAN_INIT;
    VmafxError *error = NULL;
    const VmafxStatus st = vmafx_window_clock_frame(s->clock, index, pts_ns, &span, &error);
    if (st == VMAFX_PENDING)
        return 0;
    if (st != VMAFX_OK)
        return vmafx_fail(ctx, st, error, "vmafx_window_clock_frame");
    return windows_submit(ctx, &span);
}

/* ---- Output frames ---------------------------------------------------------------- */

static void frame_metadata(VMAFXContext *s, AVFrame *frame, uint64_t index)
{
    char key[128];
    char value[64];
    for (unsigned m = 0; m < s->n_models; m++) {
        VmafxScore score = VMAFX_SCORE_INIT;
        if (vmafx_score_frame(s->context, s->models[m], index, &score, NULL) != VMAFX_OK)
            continue;
        snprintf(key, sizeof(key), "lavfi.vmafx.%s", vmafx_model_name(s->models[m]));
        snprintf(value, sizeof(value), s->score_fmt, score.value);
        av_dict_set(&frame->metadata, key, value, 0);
    }
}

static int output_frame(AVFilterContext *ctx, AVFrame *frame, uint64_t index)
{
    VMAFXContext *s = ctx->priv;
    if (s->metadata)
        frame_metadata(s, frame, index);
    if (s->window_meta) {
        av_dict_parse_string(&frame->metadata, s->window_meta, "=", ",", 0);
        av_freep(&s->window_meta);
    }
    return ff_filter_frame(ctx->outputs[0], frame);
}

/* Frame `index` is final for every model, or not scored at all. */
static int scores_final(VMAFXContext *s, uint64_t index)
{
    for (unsigned m = 0; m < s->n_models; m++) {
        VmafxScore score = VMAFX_SCORE_INIT;
        if (vmafx_score_frame(s->context, s->models[m], index, &score, NULL) == VMAFX_PENDING)
            return 0;
    }
    return 1;
}

typedef struct HeldFrame {
    AVFrame *frame;
    uint64_t index;
} HeldFrame;

/* metadata=1: send the held frames whose scores are final, in order; with
 * `all`, every held frame (after the flush). */
static int held_drain(AVFilterContext *ctx, int all)
{
    VMAFXContext *s = ctx->priv;
    HeldFrame h;
    while (s->held && av_fifo_peek(s->held, &h, 1, 0) >= 0) {
        if (!all && !scores_final(s, h.index))
            return 0;
        av_fifo_drain2(s->held, 1);
        const int ret = output_frame(ctx, h.frame, h.index);
        if (ret < 0)
            return ret;
    }
    return 0;
}

static int send_or_hold(AVFilterContext *ctx, AVFrame *dist, uint64_t index)
{
    VMAFXContext *s = ctx->priv;
    if (!s->metadata)
        return output_frame(ctx, dist, index);
    const HeldFrame h = {dist, index};
    int ret = av_fifo_write(s->held, &h, 1);
    if (ret < 0) {
        av_frame_free(&dist);
        return ret;
    }
    return held_drain(ctx, 0);
}

/* ---- Scoring ------------------------------------------------------------------------ */

static void perceptual_sidedata(AVFilterContext *ctx, const AVFrame *dist, uint64_t index)
{
    VMAFXContext *s = ctx->priv;
    const AVFrameSideData *sd = av_frame_get_side_data(dist, AV_FRAME_DATA_SEI_UNREGISTERED);
    if (!s->perceptual_weight || !sd || !sd->data || !sd->size)
        return;
    VmafxError *error = NULL;
    if (vmafx_context_attach_sidedata(s->context, index, sd->data, sd->size, &error) != VMAFX_OK) {
        av_log(ctx, AV_LOG_DEBUG, "perceptual_weight: frame %" PRIu64 " scored unweighted: %s\n",
               index, vmafx_error_message(error));
        vmafx_error_free(error);
    }
}

/* Both inputs as VMAFx frames, submitted as frame `index`. */
static int submit_pair(AVFilterContext *ctx, AVFrame *ref, AVFrame *dist, uint64_t index)
{
    VMAFXContext *s = ctx->priv;
    VmafxFrame *r = NULL;
    VmafxFrame *d = NULL;
    int ret = to_vmafx_frame(ctx, ref, "reference", &r);
    if (ret >= 0)
        ret = to_vmafx_frame(ctx, dist, "main", &d);
    if (ret < 0) {
        vmafx_frame_unref(r);
        return ret;
    }
    perceptual_sidedata(ctx, dist, index);
    VmafxError *error = NULL;
    const VmafxStatus st = vmafx_submit(s->context, r, d, index, &error); /* consumes both */
    return st == VMAFX_OK ? 0 : vmafx_fail(ctx, st, error, "vmafx_submit");
}

static int do_vmafx(FFFrameSync *fs)
{
    AVFilterContext *ctx = fs->parent;
    VMAFXContext *s = ctx->priv;
    AVFrame *ref = NULL;
    AVFrame *dist = NULL;
    int ret = ff_framesync_dualinput_get(fs, &dist, &ref);
    if (ret < 0)
        return ret;
    if (ctx->is_disabled || !ref)
        return ff_filter_frame(ctx->outputs[0], dist);
    if (s->failed) {
        av_frame_free(&dist);
        return AVERROR_EXTERNAL;
    }
    const uint64_t index = s->frame_cnt++;
    ret = windows_frame(ctx, index, dist);
    if (ret >= 0)
        ret = submit_pair(ctx, ref, dist, index);
    if (ret >= 0)
        ret = windows_collect(ctx, 0);
    if (ret < 0) {
        s->failed = 1; /* no frame passes unscored, no score line follows */
        av_frame_free(&dist);
        return ret;
    }
    return send_or_hold(ctx, dist, index);
}

/* End of stream: flush, close the last window, write every window, send the
 * held frames. Their output precedes the EOF the framesync set (a link
 * delivers its queued frames before its status). */
static int finish_stream(AVFilterContext *ctx)
{
    VMAFXContext *s = ctx->priv;
    s->finished = 1;
    if (s->failed || !s->context)
        return 0;
    VmafxError *error = NULL;
    VmafxStatus st = vmafx_flush(s->context, &error);
    if (st != VMAFX_OK)
        return vmafx_fail(ctx, st, error, "vmafx_flush");
    if (s->clock) {
        VmafxWindowSpan span = VMAFX_WINDOW_SPAN_INIT;
        st = vmafx_window_clock_finish(s->clock, &span, &error);
        if (st == VMAFX_OK) {
            int ret = windows_submit(ctx, &span);
            if (ret < 0)
                return ret;
        } else if (st != VMAFX_PENDING) {
            return vmafx_fail(ctx, st, error, "vmafx_window_clock_finish");
        }
    }
    int ret = windows_collect(ctx, 1);
    return ret < 0 ? ret : held_drain(ctx, 1);
}

static int activate(AVFilterContext *ctx)
{
    VMAFXContext *s = ctx->priv;
    int ret = ff_framesync_activate(&s->fs);
    if (ret >= 0 && s->fs.eof && !s->finished) {
        ret = finish_stream(ctx);
        if (ret < 0)
            s->failed = 1;
    }
    return ret;
}

/* ---- Set-up ------------------------------------------------------------------ */

/* `spec` with the ADM display options appended (option group `model_suffix`). */
static int model_spec(const VMAFXContext *s, const char *spec, char *out, size_t size)
{
    int n = snprintf(out, size, "%s", spec);
    if (n >= 0 && (size_t)n < size && s->view_distance > 0)
        n += snprintf(out + n, size - (size_t)n, "%sadm.adm_norm_view_dist=%g", *spec ? ":" : "",
                      s->view_distance);
    if (n >= 0 && (size_t)n < size && s->display_height > 0)
        n += snprintf(out + n, size - (size_t)n, "%sadm.adm_ref_display_height=%" PRId64,
                      n ? ":" : "", s->display_height);
    return n >= 0 && (size_t)n < size ? 0 : AVERROR(EINVAL);
}

/* Each `|`-separated model spec loaded and mounted; none: the default. */
static int use_models(AVFilterContext *ctx)
{
    VMAFXContext *s = ctx->priv;
    char *copy = av_strdup(s->model ? s->model : "");
    if (!copy)
        return AVERROR(ENOMEM);
    char *save = NULL;
    char *item = av_strtok(copy, "|", &save);
    int ret = 0;
    for (const char *spec = item ? item : ""; ret >= 0; spec = av_strtok(NULL, "|", &save)) {
        char buf[VMAFX_SPEC_BYTES];
        if (!spec || s->n_models == VMAFX_MAX_MODELS) {
            ret = spec ? AVERROR(E2BIG) : 0;
            break;
        }
        ret = model_spec(s, spec, buf, sizeof(buf));
        VmafxError *error = NULL;
        VmafxStatus st =
            ret < 0 ? VMAFX_OK : vmafx_model_load_spec(NULL, buf, &s->models[s->n_models], &error);
        if (ret >= 0 && st == VMAFX_OK)
            st = vmafx_context_use_model(s->context, s->models[s->n_models++], &error);
        if (st != VMAFX_OK)
            ret = vmafx_fail(ctx, st, error, spec);
        if (!item)
            break;
    }
    av_free(copy);
    return ret;
}

static int use_features(AVFilterContext *ctx)
{
    VMAFXContext *s = ctx->priv;
    if (!s->feature)
        return 0;
    char *copy = av_strdup(s->feature);
    if (!copy)
        return AVERROR(ENOMEM);
    char *save = NULL;
    int ret = 0;
    for (char *spec = av_strtok(copy, "|", &save); spec && ret >= 0;
         spec = av_strtok(NULL, "|", &save)) {
        VmafxError *error = NULL;
        const VmafxStatus st = vmafx_context_use_feature_spec(s->context, spec, &error);
        if (st != VMAFX_OK)
            ret = vmafx_fail(ctx, st, error, spec);
    }
    av_free(copy);
    return ret;
}

static int use_tiny_model(AVFilterContext *ctx)
{
    static const uint32_t devices[] = {
        VMAFX_DNN_DEVICE_AUTO,         VMAFX_DNN_DEVICE_CPU,          VMAFX_DNN_DEVICE_CUDA,
        VMAFX_DNN_DEVICE_OPENVINO,     VMAFX_DNN_DEVICE_OPENVINO_NPU, VMAFX_DNN_DEVICE_OPENVINO_CPU,
        VMAFX_DNN_DEVICE_OPENVINO_GPU, VMAFX_DNN_DEVICE_COREML,       VMAFX_DNN_DEVICE_COREML_ANE,
        VMAFX_DNN_DEVICE_COREML_GPU,   VMAFX_DNN_DEVICE_COREML_CPU,   VMAFX_DNN_DEVICE_ROCM,
    };
    VMAFXContext *s = ctx->priv;
    if (!s->tiny_model)
        return 0;
    VmafxDnnConfig cfg = VMAFX_DNN_CONFIG_INIT;
    cfg.device = devices[av_clip(s->tiny_device, 0, FF_ARRAY_ELEMS(devices) - 1)];
    cfg.threads = (int32_t)FFMIN(s->tiny_threads, INT32_MAX);
    cfg.flags = s->tiny_fp16 ? VMAFX_DNN_FP16_IO : 0;
    VmafxError *error = NULL;
    const VmafxStatus st = vmafx_context_use_tiny_model(s->context, s->tiny_model, &cfg, &error);
    return st == VMAFX_OK ? 0 : vmafx_fail(ctx, st, error, "tiny_model");
}

/* The slot of the backend frames go to: the frames' for hardware frames
 * (`backend` must agree), the `backend` option's for software frames. */
/* With backend=auto, DRM PRIME frames go to the backend of their GPU's
 * vendor (SYCL on Intel, HIP on AMD); 0 keeps the first matching slot. */
static int drm_auto_slot(AVBufferRef *frames)
{
#if CONFIG_LIBDRM
    const int slot = vendor_slot(frames ? drm_vendor(frames) : 0u);
    if (slot == 3 || slot == 4) /* DRM PRIME frames go to SYCL or HIP */
        return slot;
#else
    (void)frames;
#endif
    return 0;
}

static int pick_slot(AVFilterContext *ctx, enum AVPixelFormat format, AVBufferRef *frames)
{
    VMAFXContext *s = ctx->priv;
    const VMAFXBackendSlot *opt =
        &backend_slots[av_clip(s->backend, 0, FF_ARRAY_ELEMS(backend_slots) - 1)];
    s->hw = (av_pix_fmt_desc_get(format)->flags & AV_PIX_FMT_FLAG_HWACCEL) != 0;
    s->slot = opt;
    if (!s->hw) {
        if (s->import_mode == 1) {
            av_log(ctx, AV_LOG_ERROR, "vmafx: import=device refuses software frames (%s)\n",
                   av_get_pix_fmt_name(format));
            return AVERROR(EINVAL);
        }
        return 0;
    }
#if CONFIG_VULKAN
    if (format == AV_PIX_FMT_VULKAN && s->import_mode != 2) {
        if (s->backend == 1 || s->backend == 5) {
            av_log(ctx, AV_LOG_ERROR,
                   "vmafx: backend %s cannot score Vulkan frames (CUDA, SYCL or HIP on the "
                   "frames' GPU can); use import=host or hwdownload\n",
                   opt->name);
            return AVERROR(EINVAL);
        }
        s->vulkan = 1; /* the device is the one on the frames' GPU (vk_open()) */
        return 0;
    }
#endif
    const int vendor_slot =
        s->backend == 0 && format == AV_PIX_FMT_DRM_PRIME ? drm_auto_slot(frames) : 0;
    for (unsigned i = 2; i < FF_ARRAY_ELEMS(backend_slots); i++) {
        const int wanted = s->backend != 0 ? s->backend : vendor_slot;
        if (backend_slots[i].hw_format == format && (wanted == 0 || wanted == (int)i)) {
            s->slot = &backend_slots[i];
            break;
        }
    }
    if (s->slot->hw_format != format && s->import_mode != 2) {
        av_log(ctx, AV_LOG_ERROR,
               "vmafx: backend %s cannot score %s frames; use import=host or hwdownload\n",
               opt->name, av_get_pix_fmt_name(format));
        return AVERROR(EINVAL);
    }
    return 0;
}

/* The device the context scores on: none for the CPU, the frames' device for
 * hardware frames, the `device` index for software frames on a GPU backend. */
static int open_device(AVFilterContext *ctx, AVBufferRef *frames)
{
    VMAFXContext *s = ctx->priv;
#if CONFIG_VULKAN
    if (s->vulkan)
        return vk_open(ctx, frames);
#endif
    if (s->slot->backend == VMAFX_BACKEND_CPU || (s->hw && s->import_mode == 2))
        return 0;
    if (s->hw) {
        if (!s->slot->open) {
            av_log(ctx, AV_LOG_ERROR,
                   "vmafx: backend %s: %s frames are imported once its WP3 lane lands; use "
                   "import=host or hwdownload\n",
                   s->slot->name, av_get_pix_fmt_name(s->slot->hw_format));
            return AVERROR(ENOSYS);
        }
        return s->slot->open(ctx, frames);
    }
    VmafxDeviceDesc desc = VMAFX_DEVICE_DESC_INIT;
    desc.backend = s->slot->backend;
    char *end = NULL;
    const long index = !s->device || !strcmp(s->device, "auto") ? -1 : strtol(s->device, &end, 10);
    if (end && (*end || index < 0 || index > INT32_MAX)) {
        av_log(ctx, AV_LOG_ERROR, "vmafx: device=%s is not auto or a device index\n", s->device);
        return AVERROR(EINVAL);
    }
    desc.index = (int32_t)index;
    desc.flags = s->profile ? VMAFX_DEVICE_PROFILING : 0;
    VmafxError *error = NULL;
    const VmafxStatus st = vmafx_device_create(&desc, &s->vdev, &error);
    return st == VMAFX_OK ? 0 : vmafx_fail(ctx, st, error, "vmafx_device_create");
}

static int open_windows(AVFilterContext *ctx)
{
    VMAFXContext *s = ctx->priv;
    if (s->n_stats <= 0 && s->n_stats_frames <= 0)
        return 0;
    VmafxWindowClockConfig cfg = VMAFX_WINDOW_CLOCK_CONFIG_INIT;
    cfg.n_stats = s->n_stats;
    cfg.n_stats_frames = (uint64_t)s->n_stats_frames;
    VmafxError *error = NULL;
    const VmafxStatus st = vmafx_window_clock_create(&cfg, &s->clock, &error);
    if (st != VMAFX_OK)
        return vmafx_fail(ctx, st, error, "vmafx_window_clock_create");
    if ((s->stats_out & 4) && s->stats_path) {
        s->stats_file = avpriv_fopen_utf8(s->stats_path, "w");
        if (!s->stats_file) {
            av_log(ctx, AV_LOG_ERROR, "vmafx: cannot open stats_path %s\n", s->stats_path);
            return AVERROR(errno);
        }
    }
    return 0;
}

/* The provenance record at init (`provenance=log`). */
static void log_provenance(AVFilterContext *ctx)
{
    VMAFXContext *s = ctx->priv;
    if (!(s->provenance & 1))
        return;
    const char *json = NULL;
    VmafxError *error = NULL;
    if (vmafx_context_provenance_json(s->context, 0, &json, &error) == VMAFX_OK)
        av_log(ctx, AV_LOG_INFO, "vmafx provenance: %s\n", json);
    else
        vmafx_fail(ctx, VMAFX_E_INVALID, error, "vmafx_context_provenance_json");
}

static int create_context(AVFilterContext *ctx)
{
    VMAFXContext *s = ctx->priv;
    VmafxContextConfig cfg = VMAFX_CONTEXT_CONFIG_INIT;
    cfg.log_level = library_log_level();
    cfg.log_callback = log_from_library;
    cfg.log_user = ctx;
    cfg.n_threads = (uint32_t)FFMIN(s->threads, UINT32_MAX);
    cfg.n_subsample = (uint32_t)FFMIN(s->subsample, UINT32_MAX);
    cfg.cpumask = (uint64_t)s->cpumask;
    cfg.gpumask = (uint64_t)s->gpumask;
    VmafxError *error = NULL;
    VmafxStatus st = vmafx_context_create(&cfg, &s->context, &error);
    if (st == VMAFX_OK && s->vdev)
        st = vmafx_context_use_device(s->context, s->vdev, &error);
    if (st == VMAFX_OK && s->perceptual_weight)
        st = vmafx_context_set_option(s->context, "perceptual_weight", "1", &error);
    return st == VMAFX_OK ? 0 : vmafx_fail(ctx, st, error, "vmafx_context_create");
}

/* Everything that scores: the context, its models, features and windows.
 * One context per filter in this release; RC5 device targets add contexts
 * on the same frames (device-targeted scoring) without a change to the
 * frame path. */
static int setup_scoring(AVFilterContext *ctx)
{
    VMAFXContext *s = ctx->priv;
    int ret = create_context(ctx);
    if (ret >= 0)
        ret = use_models(ctx);
    if (ret >= 0)
        ret = use_features(ctx);
    if (ret >= 0)
        ret = use_tiny_model(ctx);
    if (ret >= 0)
        ret = open_windows(ctx);
    if (ret >= 0 && s->metadata) {
        s->held = av_fifo_alloc2(vmafx_context_max_in_flight(s->context) + 2u, sizeof(HeldFrame),
                                 AV_FIFO_FLAG_AUTO_GROW);
        ret = s->held ? 0 : AVERROR(ENOMEM);
    }
    if (ret >= 0)
        log_provenance(ctx);
    return ret;
}

static av_cold int init(AVFilterContext *ctx)
{
    VMAFXContext *s = ctx->priv;
    s->fs.on_event = do_vmafx;
    if (s->n_stats > 0 && s->n_stats_frames > 0) {
        av_log(ctx, AV_LOG_ERROR, "vmafx: give n_stats or n_stats_frames, not both\n");
        return AVERROR(EINVAL);
    }
    if (s->target_width || s->target_height || s->target_scaling) {
        av_log(ctx, AV_LOG_ERROR,
               "vmafx: target_width, target_height and target_scaling are reserved: "
               "device-targeted scoring lands in RC5\n");
        return AVERROR(ENOSYS);
    }
    if ((s->stats_out & 4) && !s->stats_path) {
        av_log(ctx, AV_LOG_ERROR, "vmafx: stats_out=file needs stats_path\n");
        return AVERROR(EINVAL);
    }
    return 0;
}

static int config_input_ref(AVFilterLink *inlink)
{
    AVFilterContext *ctx = inlink->dst;
    const AVFilterLink *main = ctx->inputs[0];
    if (main->w != inlink->w || main->h != inlink->h || main->format != inlink->format) {
        av_log(ctx, AV_LOG_ERROR,
               "vmafx: main (%dx%d %s) and reference (%dx%d %s) must match in size and format\n",
               main->w, main->h, av_get_pix_fmt_name(main->format), inlink->w, inlink->h,
               av_get_pix_fmt_name(inlink->format));
        return AVERROR(EINVAL);
    }
    return 0;
}

/* The hardware frames of an input, and their software layout. */
static int input_frames(AVFilterContext *ctx, AVBufferRef **frames)
{
    VMAFXContext *s = ctx->priv;
    FilterLink *main = ff_filter_link(ctx->inputs[0]);
    FilterLink *ref = ff_filter_link(ctx->inputs[1]);
    *frames = main->hw_frames_ctx;
    s->sw_format = ctx->inputs[0]->format;
    if (!s->hw)
        return 0;
    if (!main->hw_frames_ctx || !ref->hw_frames_ctx) {
        av_log(ctx, AV_LOG_ERROR, "vmafx: a %s input has no hardware frames context\n",
               av_get_pix_fmt_name(ctx->inputs[0]->format));
        return AVERROR(EINVAL);
    }
    const AVHWFramesContext *fm = (AVHWFramesContext *)main->hw_frames_ctx->data;
    const AVHWFramesContext *fr = (AVHWFramesContext *)ref->hw_frames_ctx->data;
    s->sw_format = fm->sw_format;
    const int same_layout = scored_layout(fm->sw_format) == scored_layout(fr->sw_format);
    if (fm->device_ref->data != fr->device_ref->data || !same_layout) {
        av_log(ctx, AV_LOG_ERROR,
               "vmafx: main (%s) and reference (%s) frames %s; bridge them with hwdownload / "
               "hwupload on one device or import=host\n",
               av_get_pix_fmt_name(fm->sw_format), av_get_pix_fmt_name(fr->sw_format),
               same_layout ? "live on different devices" : "differ in layout");
        return AVERROR(EINVAL);
    }
    uint32_t bpc = 0;
    if (layout_of(s->sw_format, &bpc) == VMAFX_PIXEL_FORMAT_UNKNOWN) {
        av_log(ctx, AV_LOG_ERROR, "vmafx: backend %s: frames of layout %s cannot be scored\n",
               s->slot->name, av_get_pix_fmt_name(s->sw_format));
        return AVERROR(EINVAL);
    }
    return 0;
}

/* ---- Frame pools -----------------------------------------------------------
 * The hardware frames vmafx holds of an input: an imported frame stays with
 * the context until it releases it, and metadata=1 holds the main frames
 * until their scores are final; either way at most what the context keeps
 * (vmafx_context_max_in_flight()) plus the pair being submitted. A frame
 * downloaded with import=host is let go at once. A pool of fixed size cannot
 * grow past what its creator asked for: one smaller than that never feeds the
 * filter and is refused by name. */

static int pool_is_fixed(const AVHWFramesContext *fc)
{
    switch (fc->device_ctx->type) {
    case AV_HWDEVICE_TYPE_VAAPI:
    case AV_HWDEVICE_TYPE_QSV:
    case AV_HWDEVICE_TYPE_D3D11VA:
    case AV_HWDEVICE_TYPE_D3D12VA:
    case AV_HWDEVICE_TYPE_DXVA2:
        return fc->initial_pool_size > 0;
    default:
        return 0; /* CUDA, Vulkan, OpenCL, VideoToolbox: the pool grows */
    }
}

static int hw_pool_check(AVFilterContext *ctx)
{
    VMAFXContext *s = ctx->priv;
    static const char *const names[2] = {"main", "reference"};
    const uint32_t held = vmafx_context_max_in_flight(s->context) + 1u;
    av_log(ctx, AV_LOG_VERBOSE,
           "vmafx: holds up to %u hardware frames of each input "
           "(vmafx_context_max_in_flight() + 1)\n",
           held);
    for (int i = 0; i < 2; i++) {
        const FilterLink *l = ff_filter_link(ctx->inputs[i]);
        const AVHWFramesContext *fc = (const AVHWFramesContext *)l->hw_frames_ctx->data;
        const uint32_t need = s->import_mode != 2 || (i == 0 && s->metadata) ? held : 1u;
        if (pool_is_fixed(fc) && (uint32_t)fc->initial_pool_size < need) {
            av_log(ctx, AV_LOG_ERROR,
                   "vmafx: the %s frame pool of the %s input has %d frames and vmafx holds up to "
                   "%u of them; give it at least %u (-extra_hw_frames on the decoder, "
                   "extra_hw_frames on hwupload)\n",
                   av_hwdevice_get_type_name(fc->device_ctx->type), names[i], fc->initial_pool_size,
                   need, need);
            return AVERROR(EINVAL);
        }
    }
    return 0;
}

static int config_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    VMAFXContext *s = ctx->priv;
    AVFilterLink *mainlink = ctx->inputs[0];
    FilterLink *il = ff_filter_link(mainlink);
    FilterLink *ol = ff_filter_link(outlink);
    AVBufferRef *frames = NULL;
    int ret = pick_slot(ctx, mainlink->format, il->hw_frames_ctx);
    if (ret >= 0)
        ret = input_frames(ctx, &frames);
    if (ret >= 0 && !s->context)
        ret = open_device(ctx, frames);
    if (ret >= 0 && !s->context)
        ret = setup_scoring(ctx);
    if (ret >= 0 && s->hw)
        ret = hw_pool_check(ctx);
    if (ret < 0) {
        s->failed = 1; /* nothing scored: uninit flushes, logs and writes nothing */
        return ret;
    }
    if (il->hw_frames_ctx) {
        av_buffer_unref(&ol->hw_frames_ctx);
        ol->hw_frames_ctx = av_buffer_ref(il->hw_frames_ctx);
        if (!ol->hw_frames_ctx)
            return AVERROR(ENOMEM);
    }
    if ((ret = ff_framesync_init_dualinput(&s->fs, ctx)) < 0)
        return ret;
    outlink->w = mainlink->w;
    outlink->h = mainlink->h;
    outlink->time_base = mainlink->time_base;
    outlink->sample_aspect_ratio = mainlink->sample_aspect_ratio;
    ol->frame_rate = il->frame_rate;
    return ff_framesync_configure(&s->fs);
}

/* ---- Teardown --------------------------------------------------------------- */

static void write_report(AVFilterContext *ctx)
{
    static const uint32_t formats[] = {VMAFX_REPORT_FORMAT_JSON, VMAFX_REPORT_FORMAT_XML,
                                       VMAFX_REPORT_FORMAT_CSV, VMAFX_REPORT_FORMAT_SUB};
    VMAFXContext *s = ctx->priv;
    if (!s->output)
        return;
    const uint32_t format = formats[av_clip(s->output_format, 0, FF_ARRAY_ELEMS(formats) - 1)];
    const uint32_t flags = (s->provenance & 2) && format >= VMAFX_REPORT_FORMAT_CSV ?
                               VMAFX_REPORT_PROVENANCE_SIDECAR :
                               0;
    VmafxError *error = NULL;
    const VmafxStatus st =
        vmafx_report_write(s->context, s->output, format, flags, s->score_fmt, &error);
    if (st != VMAFX_OK)
        vmafx_fail(ctx, st, error, "vmafx_report_write");
}

/* The pooled scores: one line per model and method, and upstream's
 * `VMAF score:` line for the first model's mean. */
static void log_pooled(AVFilterContext *ctx)
{
    VMAFXContext *s = ctx->priv;
    char value[64];
    for (unsigned m = 0; m < s->n_models && s->frame_cnt; m++) {
        for (unsigned p = 1; p <= FF_ARRAY_ELEMS(pool_names); p++) {
            VmafxPooledScore pooled = VMAFX_POOLED_SCORE_INIT;
            if (!(pool_mask(s) & (1u << p)) ||
                vmafx_score_pooled(s->context, s->models[m], p, 0, s->frame_cnt - 1, &pooled,
                                   NULL) != VMAFX_OK)
                continue;
            snprintf(value, sizeof(value), s->score_fmt, pooled.value);
            av_log(ctx, AV_LOG_INFO, "vmafx %s %s: %s\n", vmafx_model_name(s->models[m]),
                   pool_names[p - 1], value);
            if (m == 0 && p == VMAFX_POOL_MEAN)
                av_log(ctx, AV_LOG_INFO, "VMAF score: %s\n", value);
        }
    }
}

static void log_profile(AVFilterContext *ctx)
{
    VMAFXContext *s = ctx->priv;
    const char *text = NULL;
    if (s->profile && s->vdev && vmafx_device_profile(s->vdev, &text, NULL) == VMAFX_OK)
        av_log(ctx, AV_LOG_INFO, "vmafx device profile:\n%s\n", text);
}

/* Where the frames went: a hardware run downloads none (import=auto). */
static void log_frame_paths(AVFilterContext *ctx)
{
    const VMAFXContext *s = ctx->priv;
    av_log(ctx, AV_LOG_INFO,
           "vmafx frames: %" PRIu64 " imported on the device, %" PRIu64 " host, %" PRIu64
           " downloaded\n",
           s->n_imported, s->n_host, s->n_downloaded);
}

static void release_windows(VMAFXContext *s)
{
    for (; s->span_count; s->span_count--, s->span_head = (s->span_head + 1) % VMAFX_MAX_SPANS)
        for (unsigned m = 0; m < s->n_models; m++)
            vmafx_window_release(s->spans[s->span_head].window[m]);
    vmafx_window_clock_destroy(s->clock);
    s->clock = NULL;
}

/* Everything the filter holds, after its final output. */
static void release_all(AVFilterContext *ctx)
{
    VMAFXContext *s = ctx->priv;
    HeldFrame h;
    while (s->held && av_fifo_read(s->held, &h, 1) >= 0)
        av_frame_free(&h.frame);
    av_fifo_freep2(&s->held);
    release_windows(s);
    if (s->stats_file)
        fclose(s->stats_file);
    s->stats_file = NULL;
    av_freep(&s->window_meta);
    for (unsigned m = 0; m < s->n_models; m++)
        vmafx_model_unref(s->models[m]); /* the context holds its own */
    s->n_models = 0;
    if (s->context && vmafx_context_destroy(s->context, NULL) != VMAFX_OK &&
        vmafx_context_destroy(s->context, NULL) != VMAFX_OK)
        av_log(ctx, AV_LOG_ERROR, "vmafx: the context could not be closed (ADR-1336)\n");
    s->context = NULL;
    vmafx_device_unref(s->vdev);
    s->vdev = NULL;
#if CONFIG_VULKAN
    vk_close(s); /* after the context: it released every copy */
#endif
}

static av_cold void uninit(AVFilterContext *ctx)
{
    VMAFXContext *s = ctx->priv;
    ff_framesync_uninit(&s->fs);
    if (s->context && !s->failed) {
        if (!s->finished)
            (void)vmafx_flush(s->context, NULL);
        log_pooled(ctx); /* predicts every frame: the report then lists the model scores */
        write_report(ctx);
        log_profile(ctx);
        log_frame_paths(ctx);
    }
    release_all(ctx);
}

static const AVFilterPad vmafx_inputs[] = {
    {
        .name = "main",
        .type = AVMEDIA_TYPE_VIDEO,
    },
    {
        .name = "reference",
        .type = AVMEDIA_TYPE_VIDEO,
        .config_props = config_input_ref,
    },
};

static const AVFilterPad vmafx_outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
        .config_props = config_output,
    },
};

const FFFilter ff_vf_vmafx = {
    .p.name = "vmafx",
    .p.description = NULL_IF_CONFIG_SMALL("Calculate VMAF and other VMAFx metrics of two video "
                                          "streams on any VMAFx backend."),
    .p.priv_class = &vmafx_class,
    .p.flags = AVFILTER_FLAG_SUPPORT_TIMELINE_INTERNAL,
    .preinit = vmafx_framesync_preinit,
    .init = init,
    .uninit = uninit,
    .activate = activate,
    .priv_size = sizeof(VMAFXContext),
    FILTER_INPUTS(vmafx_inputs),
    FILTER_OUTPUTS(vmafx_outputs),
    FILTER_PIXFMTS_ARRAY(pix_fmts),
    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
};

/* ---- vmafx_tune -------------------------------------------------------------
 * The libvmaf_tune filter's capability on the VMAFx API: score the pass the
 * graph carries and recommend a CRF for the next one from its mean VMAF. The
 * same scoring path as vmafx; the default model is the library's. */

static const AVOption vmafx_tune_options[] = {
    {"model",
     "Model spec; default: the library's default model.",
     OFFSET(model),
     AV_OPT_TYPE_STRING,
     {.str = NULL},
     0,
     0,
     FLAGS},
    {"feature",
     "Additional feature extractors, | separated.",
     OFFSET(feature),
     AV_OPT_TYPE_STRING,
     {.str = NULL},
     0,
     0,
     FLAGS},
    {"threads",
     "Worker threads of the extractors.",
     OFFSET(threads),
     AV_OPT_TYPE_INT64,
     {.i64 = 0},
     0,
     1024,
     FLAGS},
    {"n_threads",
     "alias of threads",
     OFFSET(threads),
     AV_OPT_TYPE_INT64,
     {.i64 = 0},
     0,
     1024,
     FLAGS},
    {"backend",
     "Backend of software frames.",
     OFFSET(backend),
     AV_OPT_TYPE_INT,
     {.i64 = 0},
     0,
     5,
     FLAGS,
     "backend"},
    {"auto", "auto", 0, AV_OPT_TYPE_CONST, {.i64 = 0}, 0, 0, FLAGS, "backend"},
    {"cpu", "cpu", 0, AV_OPT_TYPE_CONST, {.i64 = 1}, 0, 0, FLAGS, "backend"},
    {"cuda", "cuda", 0, AV_OPT_TYPE_CONST, {.i64 = 2}, 0, 0, FLAGS, "backend"},
    {"sycl", "sycl", 0, AV_OPT_TYPE_CONST, {.i64 = 3}, 0, 0, FLAGS, "backend"},
    {"hip", "hip", 0, AV_OPT_TYPE_CONST, {.i64 = 4}, 0, 0, FLAGS, "backend"},
    {"metal", "metal", 0, AV_OPT_TYPE_CONST, {.i64 = 5}, 0, 0, FLAGS, "backend"},
    {"device",
     "GPU of the backend: auto or an index.",
     OFFSET(device),
     AV_OPT_TYPE_STRING,
     {.str = "auto"},
     0,
     0,
     FLAGS},
    {"recommend_target_vmaf",
     "Target VMAF score to recommend a CRF for.",
     OFFSET(recommend_target_vmaf),
     AV_OPT_TYPE_DOUBLE,
     {.dbl = 95.0},
     0.0,
     100.0,
     FLAGS},
    {"recommend_crf_min",
     "Lower CRF bound considered.",
     OFFSET(recommend_crf_min),
     AV_OPT_TYPE_DOUBLE,
     {.dbl = 18.0},
     0.0,
     51.0,
     FLAGS},
    {"recommend_crf_max",
     "Upper CRF bound considered.",
     OFFSET(recommend_crf_max),
     AV_OPT_TYPE_DOUBLE,
     {.dbl = 51.0},
     0.0,
     51.0,
     FLAGS},
    {"recommend_passes",
     "Number of probe passes (advisory; one pass is scored).",
     OFFSET(recommend_passes),
     AV_OPT_TYPE_INT,
     {.i64 = 1},
     1,
     8,
     FLAGS},
    {NULL},
};

FRAMESYNC_DEFINE_CLASS(vmafx_tune, VMAFXContext, fs);

static av_cold int tune_init(AVFilterContext *ctx)
{
    VMAFXContext *s = ctx->priv;
    s->fs.on_event = do_vmafx;
    s->subsample = 1;
    if (s->recommend_crf_min >= s->recommend_crf_max) {
        av_log(ctx, AV_LOG_ERROR,
               "vmafx_tune: recommend_crf_min (%.1f) must be < recommend_crf_max (%.1f).\n",
               s->recommend_crf_min, s->recommend_crf_max);
        return AVERROR(EINVAL);
    }
    av_log(ctx, AV_LOG_INFO, "vmafx_tune: model=%s target_vmaf=%.1f crf_range=[%.1f, %.1f]\n",
           s->model ? s->model : "(default)", s->recommend_target_vmaf, s->recommend_crf_min,
           s->recommend_crf_max);
    return 0;
}

/* The observed mean VMAF mapped to a CRF in [crf_min, crf_max]: the
 * libvmaf_tune filter's piece-wise linear curve, 0.40 CRF per VMAF point
 * around the middle of the range (tools/vmaf-tune sweeps real encodes for
 * per-clip precision). */
static double observed_to_crf(double observed, double target, double crf_min, double crf_max)
{
    const double crf_per_vmaf_pt = 0.40;
    const double rec = (crf_min + crf_max) * 0.5 + (observed - target) * crf_per_vmaf_pt;
    return FFMIN(FFMAX(rec, crf_min), crf_max);
}

static av_cold void tune_uninit(AVFilterContext *ctx)
{
    VMAFXContext *s = ctx->priv;
    ff_framesync_uninit(&s->fs);
    if (s->context && !s->failed && s->frame_cnt && s->n_models) {
        VmafxPooledScore pooled = VMAFX_POOLED_SCORE_INIT;
        VmafxError *error = NULL;
        VmafxStatus st = s->finished ? VMAFX_OK : vmafx_flush(s->context, &error);
        if (st == VMAFX_OK)
            st = vmafx_score_pooled(s->context, s->models[0], VMAFX_POOL_MEAN, 0, s->frame_cnt - 1,
                                    &pooled, &error);
        if (st == VMAFX_OK)
            av_log(ctx, AV_LOG_INFO,
                   "recommended_crf=%.1f (target_vmaf=%.1f, observed_vmaf=%.2f, n_frames=%" PRIu64
                   ")\n",
                   observed_to_crf(pooled.value, s->recommend_target_vmaf, s->recommend_crf_min,
                                   s->recommend_crf_max),
                   s->recommend_target_vmaf, pooled.value, s->frame_cnt);
        else
            vmafx_fail(ctx, st, error, "vmafx_tune");
    }
    release_all(ctx);
}

const FFFilter ff_vf_vmafx_tune = {
    .p.name = "vmafx_tune",
    .p.description = NULL_IF_CONFIG_SMALL("Recommend a CRF for the next pass from its VMAF."),
    .p.priv_class = &vmafx_tune_class,
    .preinit = vmafx_tune_framesync_preinit,
    .init = tune_init,
    .uninit = tune_uninit,
    .activate = activate,
    .priv_size = sizeof(VMAFXContext),
    FILTER_INPUTS(vmafx_inputs),
    FILTER_OUTPUTS(vmafx_outputs),
    FILTER_PIXFMTS_ARRAY(pix_fmts),
    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
};
