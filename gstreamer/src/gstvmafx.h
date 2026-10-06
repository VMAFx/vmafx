/*
 * Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/* The vmafx element: two video inputs scored with the VMAFx C API, the distorted input passed on. */

#ifndef GST_VMAFX_H
#define GST_VMAFX_H

#include <gst/base/gstaggregator.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#include <stdio.h>
#include <vmafx/vmafx.h>

#include "gstvmafx_opts.h"

G_BEGIN_DECLS

#define GST_TYPE_VMAFX (gst_vmafx_get_type())
G_DECLARE_FINAL_TYPE(GstVmafx, gst_vmafx, GST, VMAFX, GstAggregator)

GST_DEBUG_CATEGORY_EXTERN(gst_vmafx_debug);

/* Models one element scores; the specification string of `model` is split on `|`. */
#define GST_VMAFX_MAX_MODELS 16

/* Where a pad's frames live. */
typedef enum {
    GST_VMAFX_MEM_SYSTEM = 0,
    GST_VMAFX_MEM_CUDA = 1,
    GST_VMAFX_MEM_GL = 2,    /* advertised; refused until the GL import lane */
    GST_VMAFX_MEM_VULKAN = 3 /* advertised; refused until the Vulkan import lane */
} GstVmafxMem;

/* Values of the `backend` and `import` options (the table's constants). */
enum {
    GST_VMAFX_BACKEND_AUTO = 0,
    GST_VMAFX_BACKEND_CPU = 1,
    GST_VMAFX_BACKEND_CUDA = 2,
    GST_VMAFX_BACKEND_SYCL = 3,
    GST_VMAFX_BACKEND_HIP = 4,
    GST_VMAFX_BACKEND_METAL = 5
};

enum { GST_VMAFX_IMPORT_AUTO = 0, GST_VMAFX_IMPORT_DEVICE = 1, GST_VMAFX_IMPORT_HOST = 2 };

/* Bits of the `stats_out` and `provenance` options. */
#define GST_VMAFX_STATS_LOG 1
#define GST_VMAFX_STATS_MESSAGE 2
#define GST_VMAFX_STATS_FILE 4
#define GST_VMAFX_PROVENANCE_LOG 1
#define GST_VMAFX_PROVENANCE_REPORT 2

/* What the caps of one sink pad say. */
typedef struct {
    gboolean valid;
    GstVideoInfo info;
    GstVmafxMem mem;
} GstVmafxPad;

typedef struct GstVmafxWindows GstVmafxWindows;

/* Everything created at the first pair of buffers and released at stop. */
typedef struct {
    VmafxDevice *device;
    VmafxContext *context;
    VmafxModel *models[GST_VMAFX_MAX_MODELS];
    guint n_models;
    uint32_t backend;
    GstVmafxMem mem;
    GstVideoInfo info;
    uint32_t pool_mask;
    uint64_t next_index;    /* index of the next pair */
    uint64_t next_reported; /* first frame whose scores were not posted yet */
    GstVmafxWindows *windows;
    FILE *stats;
    guint64 host_copy_frames; /* device frames the element downloaded (import=host) */
    gboolean host_copy_logged;
    gboolean flushed;
} GstVmafxRt;

struct _GstVmafx {
    GstAggregator parent;
    GstAggregatorPad *pad[2]; /* reference, distorted */
    GstVmafxOpts opts;
    gchar *prop_error;
    GMutex lock;
    GstVmafxPad pad_info[2];
    GstVmafxRt *rt;
    gboolean failed;
};

enum { GST_VMAFX_PAD_REFERENCE = 0, GST_VMAFX_PAD_DISTORTED = 1 };

/* --- gstvmafx_util.c --- */
const char *gst_vmafx_pad_name(guint pad);
gchar *gst_vmafx_status_text(const char *what, VmafxStatus status, VmafxError *error);
gboolean gst_vmafx_score_fmt_ok(const char *fmt);
void gst_vmafx_fmt_double(const char *fmt, double value, gchar *buf, gsize size);
void gst_vmafx_json_string(GString *out, const char *text);
const char *gst_vmafx_pool_name(guint pool);
void gst_vmafx_log_callback(uint32_t level, const char *message, void *user);

/* --- gstvmafx_caps.c --- */
typedef struct {
    const char *name;
    uint32_t pix_fmt;
    uint32_t bpc;
    gboolean semi_planar;
    gboolean cuda;
} GstVmafxFormat;

GstCaps *gst_vmafx_template_caps(void);
const GstVmafxFormat *gst_vmafx_format_lookup(GstVideoFormat format);
gboolean gst_vmafx_caps_is_cuda(const GstCaps *caps);
GstVmafxMem gst_vmafx_caps_memory(const GstCaps *caps);
gchar *gst_vmafx_check_backend(const GstVmafxOpts *opts);
gchar *gst_vmafx_check_caps(const GstVmafxOpts *opts, guint pad, const GstVmafxPad *this_pad,
                            const GstVmafxPad *other);

/* --- gstvmafx_frame.c --- */
VmafxStatus gst_vmafx_frame_make(GstVmafx *self, GstVmafxRt *rt, GstBuffer *buffer, guint pad,
                                 VmafxFrame **out, gchar **error);

/* --- gstvmafx_cuda.c --- */
gboolean gst_vmafx_cuda_available(void);
gboolean gst_vmafx_cuda_probe(GstBuffer *buffer, uintptr_t *context, uintptr_t *stream);
VmafxStatus gst_vmafx_cuda_import(GstVmafx *self, GstVmafxRt *rt, GstBuffer *buffer, guint pad,
                                  VmafxFrame **out, gchar **error);

/* --- gstvmafx_run.c --- */
gboolean gst_vmafx_rt_create(GstVmafx *self, GstBuffer *first_reference, gchar **error);
void gst_vmafx_rt_free(GstVmafx *self);

/* --- gstvmafx_windows.c --- */
GstVmafxWindows *gst_vmafx_windows_new(GstVmafx *self, gchar **error);
void gst_vmafx_windows_free(GstVmafxWindows *windows);
gboolean gst_vmafx_windows_frame(GstVmafx *self, uint64_t index, int64_t pts_ns, gchar **error);
gboolean gst_vmafx_windows_poll(GstVmafx *self, gchar **error);
gboolean gst_vmafx_windows_finish(GstVmafx *self, gchar **error);
gboolean gst_vmafx_windows_score_frame(GstVmafx *self, uint64_t index, gchar **error);
gboolean gst_vmafx_windows_frames_drain(GstVmafx *self, gboolean wait, gchar **error);

/* --- gstvmafx_out.c --- */
gboolean gst_vmafx_post_provenance(GstVmafx *self, gchar **error);
gboolean gst_vmafx_report_frames(GstVmafx *self, gboolean drain, gchar **error);
gboolean gst_vmafx_finish_stream(GstVmafx *self, gchar **error);

G_END_DECLS

#endif /* GST_VMAFX_H */
