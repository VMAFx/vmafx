/*
 * Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/* The formats the element accepts and the checks of a pad's caps against the options. */

#include <string.h>

#include "gstvmafx.h"

#define GST_CAT_DEFAULT gst_vmafx_debug

/* Planar formats are scored in place; NV12 and P010 are imported (the library de-interleaves). */
static const GstVmafxFormat formats[] = {
    {"I420", VMAFX_PIXEL_FORMAT_YUV420P, 8, FALSE, TRUE},
    {"Y42B", VMAFX_PIXEL_FORMAT_YUV422P, 8, FALSE, FALSE},
    {"Y444", VMAFX_PIXEL_FORMAT_YUV444P, 8, FALSE, TRUE},
    {"GRAY8", VMAFX_PIXEL_FORMAT_YUV400P, 8, FALSE, FALSE},
    {"I420_10LE", VMAFX_PIXEL_FORMAT_YUV420P, 10, FALSE, FALSE},
    {"I420_12LE", VMAFX_PIXEL_FORMAT_YUV420P, 12, FALSE, FALSE},
    {"I422_10LE", VMAFX_PIXEL_FORMAT_YUV422P, 10, FALSE, FALSE},
    {"I422_12LE", VMAFX_PIXEL_FORMAT_YUV422P, 12, FALSE, FALSE},
    {"Y444_10LE", VMAFX_PIXEL_FORMAT_YUV444P, 10, FALSE, FALSE},
    {"Y444_12LE", VMAFX_PIXEL_FORMAT_YUV444P, 12, FALSE, FALSE},
    {"Y444_16LE", VMAFX_PIXEL_FORMAT_YUV444P, 16, FALSE, FALSE},
    {"GRAY16_LE", VMAFX_PIXEL_FORMAT_YUV400P, 16, FALSE, FALSE},
    {"NV12", VMAFX_PIXEL_FORMAT_NV12, 8, TRUE, TRUE},
    {"P010_10LE", VMAFX_PIXEL_FORMAT_P010, 10, TRUE, TRUE},
};

const GstVmafxFormat *gst_vmafx_format_lookup(GstVideoFormat format)
{
    const char *name = gst_video_format_to_string(format);
    for (gsize i = 0; name != NULL && i < G_N_ELEMENTS(formats); i++) {
        if (!strcmp(formats[i].name, name)) {
            return &formats[i];
        }
    }
    return NULL;
}

static void append_structure(GString *text, const char *features, gboolean cuda_only)
{
    gboolean first = TRUE;
    g_string_append_printf(text, "video/x-raw%s, format = (string) { ", features);
    for (gsize i = 0; i < G_N_ELEMENTS(formats); i++) {
        if (!cuda_only || formats[i].cuda) {
            g_string_append_printf(text, "%s%s", first ? "" : ", ", formats[i].name);
            first = FALSE;
        }
    }
    g_string_append(text, " }, width = (int) [ 1, 2147483647 ], height = (int) [ 1, 2147483647 ], "
                          "framerate = (fraction) [ 0/1, 2147483647/1 ]");
}

/* CUDA memory formats (where the plug-in was built with GStreamer's CUDA), then system memory:
 * the order is the preference an upstream element that offers both negotiates by. */
GstCaps *gst_vmafx_template_caps(void)
{
    GString *text = g_string_new(NULL);
#ifdef HAVE_GST_CUDA
    append_structure(text, "(memory:CUDAMemory)", TRUE);
    g_string_append(text, "; ");
#endif
    /* Offered so that a decoder's GL or Vulkan output negotiates to a refusal that names the
     * memory, not to a silent download through a converter. */
    append_structure(text, "(memory:GLMemory)", TRUE);
    g_string_append(text, "; ");
    append_structure(text, "(memory:VulkanImage)", TRUE);
    g_string_append(text, "; ");
    append_structure(text, "", FALSE);
    GstCaps *caps = gst_caps_from_string(text->str);
    g_assert(caps != NULL);
    g_string_free(text, TRUE);
    return caps;
}

GstVmafxMem gst_vmafx_caps_memory(const GstCaps *caps)
{
    const GstCapsFeatures *f = gst_caps_get_features(caps, 0);
    if (f == NULL) {
        return GST_VMAFX_MEM_SYSTEM;
    }
    if (gst_caps_features_contains(f, "memory:CUDAMemory")) {
        return GST_VMAFX_MEM_CUDA;
    }
    if (gst_caps_features_contains(f, "memory:GLMemory")) {
        return GST_VMAFX_MEM_GL;
    }
    return gst_caps_features_contains(f, "memory:VulkanImage") ? GST_VMAFX_MEM_VULKAN :
                                                                 GST_VMAFX_MEM_SYSTEM;
}

gboolean gst_vmafx_caps_is_cuda(const GstCaps *caps)
{
    const GstCapsFeatures *features = gst_caps_get_features(caps, 0);
    return features != NULL && gst_caps_features_contains(features, "memory:CUDAMemory");
}

/* A backend the element cannot run yet, named; NULL for cpu, cuda and auto. */
gchar *gst_vmafx_check_backend(const GstVmafxOpts *opts)
{
    const char *name = gst_vmafx_opts_const_name("backend", opts->backend);
    if (opts->backend >= GST_VMAFX_BACKEND_METAL) {
        return g_strdup_printf(
            "backend %s: this element imports CPU, CUDA and Vulkan memory; Metal comes with its "
            "WP3 lane",
            name ? name : "?");
    }
    return NULL;
}

/* Whether the device of `backend` binds host memory (NV12 and P010 are imported, not wrapped). */
static gboolean device_binds_host(uint32_t backend)
{
    VmafxDeviceInfo info = VMAFX_DEVICE_INFO_INIT;
    VmafxError *error = NULL;
    const VmafxStatus status = vmafx_device_info(backend, 0, &info, &error);
    vmafx_error_free(error);
    return status == VMAFX_OK && (info.memory_kinds & (1u << VMAFX_MEMORY_HOST)) != 0;
}

/* Why Vulkan images cannot be taken with these options, or NULL. */
static gchar *check_vulkan_memory(const GstVmafxOpts *opts, const char *pname,
                                  const GstVmafxFormat *f)
{
    if (gst_vmafx_vulkan_built() && opts->backend != GST_VMAFX_BACKEND_CPU &&
        opts->import_mode == GST_VMAFX_IMPORT_AUTO) {
        return NULL;
    }
    return g_strdup_printf(
        "pad %s: format %s in VulkanImage: %s (the element never downloads device memory)", pname,
        f->name,
        gst_vmafx_vulkan_built() ? "backend cpu and import=host or device do not take it" :
                                   "this build has no GStreamer Vulkan support");
}

/* Why device memory (GL, Vulkan, CUDA) cannot be read with these options, or NULL. */
static gchar *check_device_memory(const GstVmafxOpts *opts, guint pad, const GstVmafxPad *p,
                                  const GstVmafxFormat *f)
{
    const char *pname = gst_vmafx_pad_name(pad);
    if (p->mem == GST_VMAFX_MEM_GL) {
        return g_strdup_printf(
            "pad %s: format %s in GLMemory: not imported yet (GL import needs the WP3 GL interop "
            "lanes; on AMD negotiate DMABuf); the element never downloads a frame to score it",
            pname, f->name);
    }
    if (p->mem == GST_VMAFX_MEM_VULKAN) {
        return check_vulkan_memory(opts, pname, f);
    }
    if (p->mem == GST_VMAFX_MEM_CUDA && opts->backend != GST_VMAFX_BACKEND_CUDA &&
        opts->backend != GST_VMAFX_BACKEND_AUTO) {
        return g_strdup_printf("pad %s: format %s in CUDAMemory: backend %s does not read it "
                               "(use backend cuda or auto)",
                               pname, f->name, gst_vmafx_opts_const_name("backend", opts->backend));
    }
    return NULL;
}

static gchar *check_memory(const GstVmafxOpts *opts, guint pad, const GstVmafxPad *p,
                           const GstVmafxFormat *f)
{
    const char *pname = gst_vmafx_pad_name(pad);
    gchar *why = check_device_memory(opts, pad, p, f);
    if (why != NULL) {
        return why;
    }
    if (p->mem == GST_VMAFX_MEM_SYSTEM && opts->import_mode == GST_VMAFX_IMPORT_DEVICE) {
        return g_strdup_printf(
            "pad %s: format %s in system memory: import=device refuses software frames", pname,
            f->name);
    }
    if (p->mem == GST_VMAFX_MEM_SYSTEM && f->semi_planar) {
        const uint32_t backend =
            opts->backend == GST_VMAFX_BACKEND_CUDA ? VMAFX_BACKEND_CUDA : VMAFX_BACKEND_CPU;
        if (!device_binds_host(backend)) {
            return g_strdup_printf("pad %s: format %s in system memory: the %s device does not "
                                   "bind host memory for semi-planar frames",
                                   pname, f->name, vmafx_backend_name(backend));
        }
    }
    return NULL;
}

static gboolean pads_differ(const GstVmafxPad *a, const GstVmafxPad *b)
{
    return GST_VIDEO_INFO_FORMAT(&a->info) != GST_VIDEO_INFO_FORMAT(&b->info) ||
           GST_VIDEO_INFO_WIDTH(&a->info) != GST_VIDEO_INFO_WIDTH(&b->info) ||
           GST_VIDEO_INFO_HEIGHT(&a->info) != GST_VIDEO_INFO_HEIGHT(&b->info) || a->mem != b->mem;
}

/* NULL when the caps of `pad` suit the options and the other pad; else why not, naming the format. */
gchar *gst_vmafx_check_caps(const GstVmafxOpts *opts, guint pad, const GstVmafxPad *this_pad,
                            const GstVmafxPad *other)
{
    const GstVmafxFormat *f = gst_vmafx_format_lookup(GST_VIDEO_INFO_FORMAT(&this_pad->info));
    const char *pname = gst_vmafx_pad_name(pad);
    if (f == NULL) {
        return g_strdup_printf("pad %s: format %s is not one the element scores", pname,
                               gst_video_format_to_string(GST_VIDEO_INFO_FORMAT(&this_pad->info)));
    }
    gchar *why = gst_vmafx_check_backend(opts);
    if (why == NULL) {
        why = check_memory(opts, pad, this_pad, f);
    }
    if (why == NULL && other != NULL && other->valid && pads_differ(this_pad, other)) {
        why = g_strdup_printf("pad %s: format %s %dx%d differs from the other pad's format %s "
                              "%dx%d (the inputs must match)",
                              pname, f->name, GST_VIDEO_INFO_WIDTH(&this_pad->info),
                              GST_VIDEO_INFO_HEIGHT(&this_pad->info),
                              gst_video_format_to_string(GST_VIDEO_INFO_FORMAT(&other->info)),
                              GST_VIDEO_INFO_WIDTH(&other->info),
                              GST_VIDEO_INFO_HEIGHT(&other->info));
    }
    return why;
}
