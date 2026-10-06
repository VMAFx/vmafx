/*
 * Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * CUDAMemory buffers imported into a CUDA device of the library without a host copy.
 *
 * The planes are the buffer's device pointers and pitches. The producer's stream is ordered
 * before the library's by an acquire fence (a CUDA event recorded on that stream, waited on
 * device-side by the import); the buffer is kept until the library drops the frame, and its
 * producer stream then waits on the frame's release fence before the buffer is reused. The driver
 * calls the element needs beyond GStreamer's are resolved from the driver library at run time, so
 * the plug-in loads on a machine without one.
 */

#include "gstvmafx.h"

#define GST_CAT_DEFAULT gst_vmafx_debug

#ifndef HAVE_GST_CUDA

gboolean gst_vmafx_cuda_available(void)
{
    return FALSE;
}

gboolean gst_vmafx_cuda_probe(GstBuffer *buffer, uintptr_t *context, uintptr_t *stream)
{
    (void)buffer;
    (void)context;
    (void)stream;
    return FALSE;
}

VmafxStatus gst_vmafx_cuda_import(GstVmafx *self, GstVmafxRt *rt, GstBuffer *buffer, guint pad,
                                  VmafxFrame **out, gchar **error)
{
    (void)self;
    (void)rt;
    (void)buffer;
    (void)out;
    *error = g_strdup_printf("pad %s: this build has no GStreamer CUDA support",
                             gst_vmafx_pad_name(pad));
    return VMAFX_E_NOTSUP;
}

#else /* HAVE_GST_CUDA */

#include <gmodule.h>
#include <gst/cuda/gstcuda.h>

typedef CUresult (*EventCreateFn)(CUevent *, unsigned int);
typedef CUresult (*EventRecordFn)(CUevent, CUstream);
typedef CUresult (*EventDestroyFn)(CUevent);
typedef CUresult (*StreamWaitFn)(CUstream, CUevent, unsigned int);

static struct {
    EventCreateFn event_create;
    EventRecordFn event_record;
    EventDestroyFn event_destroy;
    StreamWaitFn stream_wait;
} drv;

#ifdef G_OS_WIN32
#define DRIVER_LIBRARY "nvcuda.dll"
#else
#define DRIVER_LIBRARY "libcuda.so.1"
#endif

static gsize drv_once;

static void load_driver(void)
{
    GModule *module = g_module_open(DRIVER_LIBRARY, G_MODULE_BIND_LAZY);
    gpointer sym[4] = {NULL, NULL, NULL, NULL};
    if (module != NULL && g_module_symbol(module, "cuEventCreate", &sym[0]) &&
        g_module_symbol(module, "cuEventRecord", &sym[1]) &&
        g_module_symbol(module, "cuEventDestroy_v2", &sym[2]) &&
        g_module_symbol(module, "cuStreamWaitEvent", &sym[3])) {
        drv.event_create = (EventCreateFn)sym[0];
        drv.event_record = (EventRecordFn)sym[1];
        drv.event_destroy = (EventDestroyFn)sym[2];
        drv.stream_wait = (StreamWaitFn)sym[3];
    }
}

gboolean gst_vmafx_cuda_available(void)
{
    if (g_once_init_enter(&drv_once)) {
        load_driver();
        g_once_init_leave(&drv_once, 1);
    }
    return drv.event_create != NULL && gst_cuda_load_library();
}

/* The context and stream the buffer's memory lives in. */
gboolean gst_vmafx_cuda_probe(GstBuffer *buffer, uintptr_t *context, uintptr_t *stream)
{
    GstMemory *mem = gst_buffer_n_memory(buffer) > 0 ? gst_buffer_peek_memory(buffer, 0) : NULL;
    if (mem == NULL || !gst_is_cuda_memory(mem)) {
        return FALSE;
    }
    GstCudaMemory *cm = GST_CUDA_MEMORY_CAST(mem);
    GstCudaStream *s = gst_cuda_memory_get_stream(cm);
    *context = (uintptr_t)gst_cuda_context_get_handle(cm->context);
    *stream = s != NULL ? (uintptr_t)gst_cuda_stream_get_handle(s) : 0;
    return TRUE;
}

/* What the frame keeps alive: the buffer, its mapping and the way back to the producer's stream. */
typedef struct {
    GstBuffer *buffer;
    GstMemory *memory;
    GstMapInfo map;
    GstCudaContext *context;
    CUstream stream;
    VmafxFence release;
} CudaHold;

/* The library no longer reads the frame: order the producer's stream after its last reader. */
static void cuda_release(void *user)
{
    CudaHold *hold = user;
    VmafxError *error = NULL;
    if (hold->release.kind == VMAFX_FENCE_CUDA_EVENT && gst_cuda_context_push(hold->context)) {
        const CUresult r = drv.stream_wait(hold->stream, (CUevent)hold->release.handle, 0);
        CUcontext popped = NULL;
        gst_cuda_context_pop(&popped);
        if (r != CUDA_SUCCESS) {
            GST_WARNING("cuStreamWaitEvent failed with %d", (int)r);
        }
    }
    (void)vmafx_fence_destroy(&hold->release, &error);
    vmafx_error_free(error);
    gst_memory_unmap(hold->memory, &hold->map);
    gst_memory_unref(hold->memory);
    gst_buffer_unref(hold->buffer);
    gst_object_unref(hold->context);
    g_free(hold);
}

static void hold_free(CudaHold *hold)
{
    gst_memory_unmap(hold->memory, &hold->map);
    gst_memory_unref(hold->memory);
    gst_buffer_unref(hold->buffer);
    gst_object_unref(hold->context);
    g_free(hold);
}

/* An event recording the producer's work so far; 0 on failure. */
static CUevent record_acquire(CudaHold *hold)
{
    CUevent event = NULL;
    CUresult r = CUDA_ERROR_UNKNOWN;
    if (gst_cuda_context_push(hold->context)) {
        r = drv.event_create(&event, CU_EVENT_DISABLE_TIMING);
        if (r == CUDA_SUCCESS) {
            r = drv.event_record(event, hold->stream);
            if (r != CUDA_SUCCESS) {
                drv.event_destroy(event);
                event = NULL;
            }
        }
        CUcontext popped = NULL;
        gst_cuda_context_pop(&popped);
    }
    return event;
}

static void describe(VmafxFrameImport *imp, const GstVmafxFormat *f, const GstCudaMemory *cm,
                     const CudaHold *hold)
{
    const GstVideoInfo *info = &cm->info;
    imp->memory = VMAFX_MEMORY_DEVICE_POINTER;
    imp->pix_fmt = f->pix_fmt;
    imp->bpc = f->bpc;
    imp->w = (uint32_t)GST_VIDEO_INFO_WIDTH(info);
    imp->h = (uint32_t)GST_VIDEO_INFO_HEIGHT(info);
    imp->n_planes = f->semi_planar ? 2 : 3;
    for (guint i = 0; i < imp->n_planes; i++) {
        imp->plane[i].handle = (uintptr_t)hold->map.data;
        imp->plane[i].offset = (uint64_t)GST_VIDEO_INFO_PLANE_OFFSET(info, i);
        imp->plane[i].pitch = (uint64_t)GST_VIDEO_INFO_PLANE_STRIDE(info, i);
    }
    imp->release = cuda_release;
    imp->user = (void *)hold;
}

static CudaHold *hold_new(GstBuffer *buffer, GstMemory *mem, gchar **error, guint pad)
{
    CudaHold *hold = g_new0(CudaHold, 1);
    hold->release = (VmafxFence)VMAFX_FENCE_INIT;
    hold->buffer = gst_buffer_ref(buffer);
    hold->memory = gst_memory_ref(mem);
    hold->context = gst_object_ref(GST_CUDA_MEMORY_CAST(mem)->context);
    GstCudaStream *s = gst_cuda_memory_get_stream(GST_CUDA_MEMORY_CAST(mem));
    hold->stream = s != NULL ? gst_cuda_stream_get_handle(s) : NULL;
    if (!gst_memory_map(mem, &hold->map, (GstMapFlags)(GST_MAP_READ | GST_MAP_CUDA))) {
        *error = g_strdup_printf("pad %s: the CUDA memory cannot be mapped as a device pointer",
                                 gst_vmafx_pad_name(pad));
        gst_memory_unref(hold->memory);
        gst_buffer_unref(hold->buffer);
        gst_object_unref(hold->context);
        g_free(hold);
        return NULL;
    }
    return hold;
}

/* The import itself. `*consumed` says the frame owns the hold from now on (its release callback
 * frees it, also when the frame is dropped here); until then the hold is the caller's. */
static VmafxStatus do_import(GstVmafxRt *rt, const GstVmafxFormat *f, GstCudaMemory *cm,
                             CudaHold *hold, guint pad, VmafxFrame **out, gchar **error,
                             gboolean *consumed)
{
    VmafxFrameImport imp = VMAFX_FRAME_IMPORT_INIT;
    VmafxError *err = NULL;
    CUevent event = record_acquire(hold);
    if (event == NULL) {
        *error = g_strdup_printf("pad %s: cannot record the acquire event on the buffer's stream",
                                 gst_vmafx_pad_name(pad));
        return VMAFX_E_DEVICE;
    }
    describe(&imp, f, cm, hold);
    imp.acquire.kind = VMAFX_FENCE_CUDA_EVENT;
    imp.acquire.handle = (uintptr_t)event;
    VmafxStatus status = vmafx_context_import_frame(rt->context, rt->device, &imp,
                                                    gst_vmafx_pad_name(pad), out, &err);
    if (status == VMAFX_OK) {
        *consumed = TRUE;
        status = vmafx_frame_release_fence(*out, VMAFX_FENCE_CUDA_EVENT, &hold->release, &err);
        if (status != VMAFX_OK) {
            vmafx_frame_unref(*out);
            *out = NULL;
        }
    }
    if (gst_cuda_context_push(hold->context)) {
        CUcontext popped = NULL;
        drv.event_destroy(event);
        gst_cuda_context_pop(&popped);
    }
    if (status != VMAFX_OK) {
        gchar *what = g_strdup_printf("pad %s: format %s", gst_vmafx_pad_name(pad), f->name);
        *error = gst_vmafx_status_text(what, status, err);
        g_free(what);
    }
    return status;
}

/* The one CUDA memory a buffer holds, else NULL. */
static GstMemory *single_cuda_memory(GstBuffer *buffer)
{
    GstMemory *mem = gst_buffer_n_memory(buffer) == 1 ? gst_buffer_peek_memory(buffer, 0) : NULL;
    return mem != NULL && gst_is_cuda_memory(mem) ? mem : NULL;
}

VmafxStatus gst_vmafx_cuda_import(GstVmafx *self, GstVmafxRt *rt, GstBuffer *buffer, guint pad,
                                  VmafxFrame **out, gchar **error)
{
    g_assert(self != NULL && rt != NULL && buffer != NULL && out != NULL && error != NULL);
    const GstVmafxFormat *f = gst_vmafx_format_lookup(GST_VIDEO_INFO_FORMAT(&rt->info));
    GstMemory *mem = single_cuda_memory(buffer);
    if (f == NULL || mem == NULL || !gst_vmafx_cuda_available()) {
        *error = g_strdup_printf("pad %s: the buffer is not one CUDA memory of a format the "
                                 "element imports",
                                 gst_vmafx_pad_name(pad));
        return VMAFX_E_NOTSUP;
    }
    CudaHold *hold = hold_new(buffer, mem, error, pad);
    if (hold == NULL) {
        return VMAFX_E_INVALID;
    }
    gboolean consumed = FALSE;
    const VmafxStatus status =
        do_import(rt, f, GST_CUDA_MEMORY_CAST(mem), hold, pad, out, error, &consumed);
    if (!consumed) {
        hold_free(hold);
    }
    return status;
}

#endif /* HAVE_GST_CUDA */
