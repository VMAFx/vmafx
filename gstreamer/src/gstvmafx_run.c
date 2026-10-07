/*
 * Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * The scoring session of one element: device, context, models, features, windows. Created at the
 * first pair of buffers, when the caps and the memory the frames live in are known.
 */

#include <errno.h>
#include <string.h>

#include "gstvmafx.h"

#define GST_CAT_DEFAULT gst_vmafx_debug

/* The `tiny_device` option's constants in VmafxDnnDevice's numbering. */
static const uint32_t tiny_devices[] = {
    VMAFX_DNN_DEVICE_AUTO,         VMAFX_DNN_DEVICE_CPU,          VMAFX_DNN_DEVICE_CUDA,
    VMAFX_DNN_DEVICE_OPENVINO,     VMAFX_DNN_DEVICE_OPENVINO_NPU, VMAFX_DNN_DEVICE_OPENVINO_CPU,
    VMAFX_DNN_DEVICE_OPENVINO_GPU, VMAFX_DNN_DEVICE_COREML,       VMAFX_DNN_DEVICE_COREML_ANE,
    VMAFX_DNN_DEVICE_COREML_GPU,   VMAFX_DNN_DEVICE_COREML_CPU,   VMAFX_DNN_DEVICE_ROCM,
};

/* Options the element does not carry out are refused by name, never ignored. */
static gchar *check_unsupported(const GstVmafxOpts *o)
{
    if (o->target_width != 0 || o->target_height != 0 || o->target_scaling != 0) {
        return g_strdup("options target-width, target-height, target-scaling: scoring on a "
                        "target display lands in RC5");
    }
    if (o->perceptual_weight) {
        return g_strdup("option perceptual-weight: GstBuffers carry no perceptual side data");
    }
    if (o->tiny_model == NULL && (o->tiny_device != 0 || o->tiny_threads != 0 || o->tiny_fp16)) {
        return g_strdup("options tiny-device, tiny-threads, tiny-fp16 need tiny-model");
    }
    return NULL;
}

static gchar *check_options(const GstVmafxOpts *o)
{
    if (o->pool == 0) {
        return g_strdup("option pool: no pool method set");
    }
    if (!gst_vmafx_score_fmt_ok(o->score_fmt)) {
        return g_strdup_printf("option score-fmt: '%s' is not one printf conversion of a double "
                               "(for example %%.6f or %%.17g)",
                               o->score_fmt ? o->score_fmt : "");
    }
    if ((o->stats_out & GST_VMAFX_STATS_FILE) && o->stats_path == NULL) {
        return g_strdup("option stats-out has file, and stats-path is not set");
    }
    return check_unsupported(o);
}

/* -1 for `auto`, else a device index. */
static gboolean parse_device(const char *text, int32_t *index, gchar **error)
{
    gchar *end = NULL;
    if (text == NULL || !strcmp(text, "auto")) {
        *index = -1;
        return TRUE;
    }
    const gint64 n = g_ascii_strtoll(text, &end, 10);
    if (text[0] == '\0' || *end != '\0' || n < 0 || n > G_MAXINT32) {
        *error = g_strdup_printf("option device: '%s' is neither auto nor a device index", text);
        return FALSE;
    }
    *index = (int32_t)n;
    return TRUE;
}

/* The backend for Vulkan frames of a GPU of PCI vendor `vendor`: the option's, else the vendor's. */
uint32_t gst_vmafx_backend_for_vulkan(const GstVmafxOpts *opts, uint32_t vendor)
{
    switch (opts->backend) {
    case GST_VMAFX_BACKEND_CUDA:
        return VMAFX_BACKEND_CUDA;
    case GST_VMAFX_BACKEND_SYCL:
        return VMAFX_BACKEND_SYCL;
    case GST_VMAFX_BACKEND_HIP:
        return VMAFX_BACKEND_HIP;
    default:
        break;
    }
    return vendor == 0x10de ? VMAFX_BACKEND_CUDA :
                              (vendor == 0x1002 ? VMAFX_BACKEND_HIP : VMAFX_BACKEND_SYCL);
}

/* The backend of the session: one case per backend this element can import for. */
static gboolean resolve_backend(GstVmafx *self, GstVmafxRt *rt, gchar **error)
{
    const GstVmafxOpts *o = &self->opts;
    switch (o->backend) {
    case GST_VMAFX_BACKEND_AUTO:
        rt->backend = rt->mem == GST_VMAFX_MEM_CUDA ? VMAFX_BACKEND_CUDA : VMAFX_BACKEND_CPU;
        if (rt->mem == GST_VMAFX_MEM_VULKAN) {
            rt->backend = gst_vmafx_backend_for_vulkan(o, rt->vk_vendor);
        }
        return TRUE;
    case GST_VMAFX_BACKEND_CPU:
        rt->backend = VMAFX_BACKEND_CPU;
        return TRUE;
    case GST_VMAFX_BACKEND_CUDA:
        rt->backend = VMAFX_BACKEND_CUDA;
        return TRUE;
    case GST_VMAFX_BACKEND_SYCL:
        rt->backend = VMAFX_BACKEND_SYCL;
        return TRUE;
    case GST_VMAFX_BACKEND_HIP:
        rt->backend = VMAFX_BACKEND_HIP;
        return TRUE;
    default:
        *error = gst_vmafx_check_backend(o);
        return FALSE;
    }
}

/* Index of the device of `backend` at PCI location `pci`, or -1. */
static int32_t device_at(uint32_t backend, const uint32_t pci[4])
{
    uint32_t n = 0;
    if (vmafx_device_count(backend, &n, NULL) != VMAFX_OK) {
        return -1;
    }
    for (uint32_t i = 0; i < n && i <= INT32_MAX; i++) {
        VmafxDeviceInfo info = VMAFX_DEVICE_INFO_INIT;
        if (vmafx_device_info(backend, (int32_t)i, &info, NULL) == VMAFX_OK &&
            !memcmp(info.pci, pci, sizeof(info.pci))) {
            return (int32_t)i;
        }
    }
    return -1;
}

/* Vulkan frames: the device of the backend at the Vulkan device's PCI location; never another. */
static gboolean pick_vulkan_device(GstVmafxRt *rt, VmafxDeviceDesc *desc, gchar **error)
{
    const int32_t index = device_at(rt->backend, rt->vk_pci);
    if (index < 0) {
        *error = g_strdup_printf(
            "backend %s has no device on the Vulkan device's GPU (PCI vendor 0x%04x, location "
            "%04x:%02x:%02x.%x): Vulkan frames are never read across GPUs",
            vmafx_backend_name(rt->backend), rt->vk_vendor, rt->vk_pci[0], rt->vk_pci[1],
            rt->vk_pci[2], rt->vk_pci[3]);
        return FALSE;
    }
    desc->index = index;
    return TRUE;
}

static gboolean create_device(GstVmafx *self, GstVmafxRt *rt, GstBuffer *first, gchar **error)
{
    VmafxDeviceDesc desc = VMAFX_DEVICE_DESC_INIT;
    VmafxError *err = NULL;
    desc.backend = rt->backend;
    desc.flags = self->opts.profile ? VMAFX_DEVICE_PROFILING : 0;
    if (!parse_device(self->opts.device, &desc.index, error)) {
        return FALSE;
    }
    if (rt->backend == VMAFX_BACKEND_CPU && desc.index != -1) {
        *error = g_strdup("option device: the cpu backend has one device (use auto)");
        return FALSE;
    }
    if (rt->mem == GST_VMAFX_MEM_VULKAN && !pick_vulkan_device(rt, &desc, error)) {
        return FALSE;
    }
    if (rt->mem == GST_VMAFX_MEM_CUDA &&
        !gst_vmafx_cuda_probe(first, &desc.external[0], &desc.external[1])) {
        *error = g_strdup("pad reference: the first buffer is not CUDA memory");
        return FALSE;
    }
    const VmafxStatus status = vmafx_device_create(&desc, &rt->device, &err);
    if (status != VMAFX_OK) {
        *error = gst_vmafx_status_text("device", status, err);
        return FALSE;
    }
    return TRUE;
}

static VmafxLogLevel log_level(void)
{
    return gst_debug_category_get_threshold(gst_vmafx_debug) >= GST_LEVEL_DEBUG ?
               VMAFX_LOG_LEVEL_DEBUG :
               VMAFX_LOG_LEVEL_INFO;
}

static gboolean create_context(GstVmafx *self, GstVmafxRt *rt, gchar **error)
{
    const GstVmafxOpts *o = &self->opts;
    VmafxContextConfig cfg = VMAFX_CONTEXT_CONFIG_INIT;
    VmafxError *err = NULL;
    cfg.log_level = log_level();
    cfg.n_threads = (uint32_t)CLAMP(o->threads, 0, G_MAXUINT32);
    cfg.n_subsample = o->subsample > 1 ? (uint32_t)CLAMP(o->subsample, 0, G_MAXUINT32) : 0;
    cfg.cpumask = (uint64_t)o->cpumask;
    cfg.gpumask = (uint64_t)o->gpumask;
    cfg.log_callback = gst_vmafx_log_callback;
    cfg.log_user = self;
    VmafxStatus status = vmafx_context_create(&cfg, &rt->context, &err);
    if (status == VMAFX_OK) {
        status = vmafx_context_use_device(rt->context, rt->device, &err);
    }
    if (status != VMAFX_OK) {
        *error = gst_vmafx_status_text("context", status, err);
        return FALSE;
    }
    return TRUE;
}

/* Items of a `|`-separated option; an empty string is one empty item (the default model). */
static gchar **split_items(const char *text, gboolean empty_is_item)
{
    if (text == NULL) {
        return NULL;
    }
    if (text[0] == '\0' && empty_is_item) {
        gchar **items = g_new0(gchar *, 2);
        items[0] = g_strdup("");
        return items;
    }
    return g_strsplit(text, "|", -1);
}

/* One model specification with the view-distance and display-height options as overrides. */
static gchar *model_spec(const GstVmafxOpts *o, const char *item)
{
    GString *spec = g_string_new(item);
    gchar number[64];
    if (o->view_distance > 0) {
        g_ascii_formatd(number, sizeof(number), "%.17g", o->view_distance);
        g_string_append_printf(spec, "%sadm.adm_norm_view_dist=%s", spec->len ? ":" : "", number);
    }
    if (o->display_height > 0) {
        g_string_append_printf(spec, "%sadm.adm_ref_display_height=%" G_GINT64_FORMAT,
                               spec->len ? ":" : "", (gint64)o->display_height);
    }
    return g_string_free(spec, FALSE);
}

static gboolean load_one_model(GstVmafx *self, GstVmafxRt *rt, const char *item, gchar **error)
{
    VmafxModelConfig mcfg = VMAFX_MODEL_CONFIG_INIT;
    VmafxError *err = NULL;
    gchar *spec = model_spec(&self->opts, item);
    mcfg.log_level = log_level();
    mcfg.log_callback = gst_vmafx_log_callback;
    mcfg.log_user = self;
    VmafxModel *model = NULL;
    VmafxStatus status = vmafx_model_load_spec(&mcfg, spec, &model, &err);
    if (status == VMAFX_OK) {
        status = vmafx_context_use_model(rt->context, model, &err);
        if (status == VMAFX_OK) {
            rt->models[rt->n_models++] = model;
        } else {
            vmafx_model_unref(model);
        }
    }
    if (status != VMAFX_OK) {
        gchar *what = g_strdup_printf("model '%s'", spec);
        *error = gst_vmafx_status_text(what, status, err);
        g_free(what);
    }
    g_free(spec);
    return status == VMAFX_OK;
}

/* The models: `model` split on `|`; unset means the default model unless features are named. */
static gboolean load_models(GstVmafx *self, GstVmafxRt *rt, gchar **error)
{
    const GstVmafxOpts *o = &self->opts;
    const char *text = o->model != NULL ? o->model : (o->feature == NULL ? "" : NULL);
    gchar **items = split_items(text, TRUE);
    gboolean ok = TRUE;
    for (guint i = 0; items != NULL && items[i] && ok; i++) {
        if (rt->n_models >= GST_VMAFX_MAX_MODELS) {
            *error = g_strdup_printf("option model: more than %d models", GST_VMAFX_MAX_MODELS);
            ok = FALSE;
        } else {
            ok = load_one_model(self, rt, g_strstrip(items[i]), error);
        }
    }
    g_strfreev(items);
    return ok;
}

static gboolean load_features(GstVmafxRt *rt, const char *text, gchar **error)
{
    gchar **items = split_items(text, FALSE);
    gboolean ok = TRUE;
    for (guint i = 0; items != NULL && items[i] && ok; i++) {
        VmafxError *err = NULL;
        const VmafxStatus status =
            vmafx_context_use_feature_spec(rt->context, g_strstrip(items[i]), &err);
        if (status != VMAFX_OK) {
            gchar *what = g_strdup_printf("feature '%s'", items[i]);
            *error = gst_vmafx_status_text(what, status, err);
            g_free(what);
            ok = FALSE;
        }
    }
    g_strfreev(items);
    return ok;
}

static gboolean load_tiny(GstVmafx *self, GstVmafxRt *rt, gchar **error)
{
    const GstVmafxOpts *o = &self->opts;
    VmafxDnnConfig dnn = VMAFX_DNN_CONFIG_INIT;
    VmafxError *err = NULL;
    if (o->tiny_model == NULL) {
        return TRUE;
    }
    dnn.device = tiny_devices[CLAMP(o->tiny_device, 0, (int)G_N_ELEMENTS(tiny_devices) - 1)];
    dnn.threads = (int32_t)CLAMP(o->tiny_threads, 0, G_MAXINT32);
    dnn.flags = o->tiny_fp16 ? VMAFX_DNN_FP16_IO : 0;
    const VmafxStatus status = vmafx_context_use_tiny_model(rt->context, o->tiny_model, &dnn, &err);
    if (status != VMAFX_OK) {
        *error = gst_vmafx_status_text("tiny-model", status, err);
        return FALSE;
    }
    return TRUE;
}

static gboolean open_outputs(GstVmafx *self, GstVmafxRt *rt, gchar **error)
{
    const GstVmafxOpts *o = &self->opts;
    if (o->n_stats > 0 || o->n_stats_frames > 0 || o->metadata) {
        if (rt->n_models == 0) {
            *error = g_strdup("options n-stats, n-stats-frames, metadata pool the scores of a "
                              "model: set model");
            return FALSE;
        }
        rt->windows = gst_vmafx_windows_new(self, error);
        if (rt->windows == NULL) {
            return FALSE;
        }
    }
    if ((o->stats_out & GST_VMAFX_STATS_FILE) && rt->windows != NULL) {
        rt->stats = fopen(o->stats_path, "w");
        if (rt->stats == NULL) {
            *error = g_strdup_printf("option stats-path: cannot open '%s' for writing: %s",
                                     o->stats_path, g_strerror(errno));
            return FALSE;
        }
    }
    return TRUE;
}

static void rt_destroy(GstVmafxRt *rt)
{
    VmafxError *err = NULL;
    gst_vmafx_windows_free(rt->windows);
    rt->windows = NULL;
    if (rt->context != NULL) {
        const VmafxStatus status = vmafx_context_destroy(rt->context, &err);
        if (status != VMAFX_OK) {
            GST_WARNING("context destroy: %s", vmafx_error_message(err));
        }
        vmafx_error_free(err);
    }
    for (guint i = 0; i < rt->n_models; i++) {
        vmafx_model_unref(rt->models[i]);
    }
    vmafx_device_unref(rt->device);
    if (rt->stats != NULL) {
        fclose(rt->stats);
    }
    g_free(rt);
}

void gst_vmafx_rt_free(GstVmafx *self)
{
    if (self->rt != NULL) {
        GstVmafxRt *rt = self->rt;
        self->rt = NULL;
        rt_destroy(rt);
    }
}

/* The steps that need the options and the first buffer, in order. */
static gboolean setup(GstVmafx *self, GstVmafxRt *rt, GstBuffer *first, gchar **error)
{
    if (rt->mem == GST_VMAFX_MEM_VULKAN &&
        !gst_vmafx_vulkan_probe(first, rt->vk_pci, &rt->vk_vendor)) {
        *error = g_strdup("pad reference: the Vulkan device reports no PCI location "
                          "(VK_EXT_pci_bus_info), so its GPU cannot be matched");
        return FALSE;
    }
    if (!resolve_backend(self, rt, error) || !create_device(self, rt, first, error)) {
        return FALSE;
    }
    if (!create_context(self, rt, error) || !load_models(self, rt, error)) {
        return FALSE;
    }
    return load_features(rt, self->opts.feature, error) && load_tiny(self, rt, error) &&
           open_outputs(self, rt, error);
}

/* Create the session; on failure nothing stays allocated and `*error` says why. */
gboolean gst_vmafx_rt_create(GstVmafx *self, GstBuffer *first_reference, gchar **error)
{
    g_assert(self != NULL && self->rt == NULL && error != NULL);
    *error = check_options(&self->opts);
    if (*error != NULL) {
        return FALSE;
    }
    GstVmafxRt *rt = g_new0(GstVmafxRt, 1);
    rt->info = self->pad_info[GST_VMAFX_PAD_REFERENCE].info;
    rt->mem = self->pad_info[GST_VMAFX_PAD_REFERENCE].mem;
    rt->pool_mask = (uint32_t)self->opts.pool << 1U;
    self->rt = rt;
    GST_INFO_OBJECT(self, "first pair: memory kind %d, %s", (int)rt->mem,
                    gst_video_format_to_string(GST_VIDEO_INFO_FORMAT(&rt->info)));
    if (!setup(self, rt, first_reference, error)) {
        gst_vmafx_rt_free(self);
        return FALSE;
    }
    return TRUE;
}
