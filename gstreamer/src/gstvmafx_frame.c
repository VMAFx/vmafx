/*
 * Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * GstBuffer to VmafxFrame. System memory is mapped read-only and the planes are lent to the
 * library without a copy; the map is released when the library drops the frame. Device memory
 * goes through gstvmafx_cuda.c.
 */

#include "gstvmafx.h"

#define GST_CAT_DEFAULT gst_vmafx_debug

/* Called once, on any thread, when the library no longer reads the planes. */
static void host_release(void *user)
{
    GstVideoFrame *vf = user;
    gst_video_frame_unmap(vf);
    g_free(vf);
}

static void fill_desc(VmafxFrameDesc *desc, const GstVmafxFormat *f, const GstVideoInfo *info)
{
    desc->pix_fmt = f->pix_fmt;
    desc->bpc = f->bpc;
    desc->w = (uint32_t)GST_VIDEO_INFO_WIDTH(info);
    desc->h = (uint32_t)GST_VIDEO_INFO_HEIGHT(info);
}

static VmafxStatus wrap_planar(GstVmafxRt *rt, const GstVmafxFormat *f, GstVideoFrame *vf,
                               VmafxFrame **out, VmafxError **error)
{
    VmafxFrameDesc desc = VMAFX_FRAME_DESC_INIT;
    VmafxHostPlanes planes = VMAFX_HOST_PLANES_INIT;
    fill_desc(&desc, f, &rt->info);
    for (guint i = 0; i < GST_VIDEO_FRAME_N_PLANES(vf) && i < 3; i++) {
        planes.data[i] = GST_VIDEO_FRAME_PLANE_DATA(vf, i);
        planes.stride[i] = (uint64_t)GST_VIDEO_FRAME_PLANE_STRIDE(vf, i);
    }
    planes.release = host_release;
    planes.user = vf;
    return vmafx_frame_wrap_host(NULL, &desc, &planes, out, error); /* the CPU device */
}

static VmafxStatus import_semi_planar(GstVmafxRt *rt, const GstVmafxFormat *f, GstVideoFrame *vf,
                                      guint pad, VmafxFrame **out, VmafxError **error)
{
    VmafxFrameImport imp = VMAFX_FRAME_IMPORT_INIT;
    imp.memory = VMAFX_MEMORY_HOST;
    imp.pix_fmt = f->pix_fmt;
    imp.bpc = f->bpc;
    imp.w = (uint32_t)GST_VIDEO_INFO_WIDTH(&rt->info);
    imp.h = (uint32_t)GST_VIDEO_INFO_HEIGHT(&rt->info);
    imp.n_planes = 2;
    for (guint i = 0; i < 2; i++) {
        imp.plane[i].handle = (uintptr_t)GST_VIDEO_FRAME_PLANE_DATA(vf, i);
        imp.plane[i].pitch = (uint64_t)GST_VIDEO_FRAME_PLANE_STRIDE(vf, i);
    }
    imp.release = host_release;
    imp.user = vf;
    return vmafx_context_import_frame(rt->context, NULL, &imp, gst_vmafx_pad_name(pad), out, error);
}

/* A frame of system memory, or of device memory the user asked to download (import=host). It
 * lives on the CPU device whatever device the context scores on; a context on a GPU uploads it. */
static VmafxStatus make_host(GstVmafx *self, GstVmafxRt *rt, GstBuffer *buffer, guint pad,
                             VmafxFrame **out, gchar **error)
{
    const GstVmafxFormat *f = gst_vmafx_format_lookup(GST_VIDEO_INFO_FORMAT(&rt->info));
    GstVideoFrame *vf = g_new0(GstVideoFrame, 1);
    if (f == NULL || !gst_video_frame_map(vf, &rt->info, buffer, GST_MAP_READ)) {
        *error = g_strdup_printf("pad %s: the buffer cannot be mapped for reading",
                                 gst_vmafx_pad_name(pad));
        g_free(vf);
        return VMAFX_E_INVALID;
    }
    if (rt->mem == GST_VMAFX_MEM_CUDA) {
        rt->host_copy_frames++;
        if (!rt->host_copy_logged) {
            rt->host_copy_logged = TRUE;
            GST_INFO_OBJECT(self, "import=host: device frames are downloaded to system memory");
        }
    }
    VmafxError *err = NULL;
    const VmafxStatus status = f->semi_planar ? import_semi_planar(rt, f, vf, pad, out, &err) :
                                                wrap_planar(rt, f, vf, out, &err);
    if (status != VMAFX_OK) {
        gchar *what = g_strdup_printf("pad %s: format %s", gst_vmafx_pad_name(pad), f->name);
        *error = gst_vmafx_status_text(what, status, err);
        g_free(what);
        gst_video_frame_unmap(vf);
        g_free(vf);
    }
    return status;
}

/* The frame of one buffer; on failure `*error` says why, naming the pad. */
VmafxStatus gst_vmafx_frame_make(GstVmafx *self, GstVmafxRt *rt, GstBuffer *buffer, guint pad,
                                 VmafxFrame **out, gchar **error)
{
    g_assert(self != NULL && rt != NULL && buffer != NULL && out != NULL && error != NULL);
    if (rt->mem == GST_VMAFX_MEM_VULKAN) {
        return gst_vmafx_vulkan_import(self, rt, buffer, pad, out, error);
    }
    if (rt->mem == GST_VMAFX_MEM_CUDA && self->opts.import_mode != GST_VMAFX_IMPORT_HOST) {
        return gst_vmafx_cuda_import(self, rt, buffer, pad, out, error);
    }
    return make_host(self, rt, buffer, pad, out, error);
}
