/*
 * Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/* What the element reports: provenance, per-frame scores, the end-of-stream summary and report. */

#include "gstvmafx.h"

#define GST_CAT_DEFAULT gst_vmafx_debug

static void post(GstVmafx *self, GstStructure *s)
{
    gst_element_post_message(GST_ELEMENT(self), gst_message_new_element(GST_OBJECT(self), s));
}

/* The provenance record of the session, as a message and a log line. */
gboolean gst_vmafx_post_provenance(GstVmafx *self, gchar **error)
{
    const char *json = NULL;
    VmafxError *err = NULL;
    g_assert(self->rt != NULL);
    const VmafxStatus status = vmafx_context_provenance_json(self->rt->context, 0, &json, &err);
    if (status != VMAFX_OK) {
        *error = gst_vmafx_status_text("provenance", status, err);
        return FALSE;
    }
    GST_INFO_OBJECT(self, "provenance %s", json);
    post(self, gst_structure_new("vmafx-provenance", "json", G_TYPE_STRING, json, NULL));
    return TRUE;
}

/* Post the scores of the frames that became final (the `metadata` option). A frame's score is
 * asked for as a window of that one frame: the library says when it is final without blocking
 * the feeding thread, which the per-frame score call does not (docs/state.md). */
gboolean gst_vmafx_report_frames(GstVmafx *self, gboolean drain, gchar **error)
{
    GstVmafxRt *rt = self->rt;
    const uint64_t step = self->opts.subsample > 1 ? (uint64_t)self->opts.subsample : 1;
    if (!self->opts.metadata || rt->windows == NULL) {
        return TRUE;
    }
    for (; rt->next_reported < rt->next_index; rt->next_reported++) {
        if (rt->next_reported % step == 0 &&
            !gst_vmafx_windows_score_frame(self, rt->next_reported, error)) {
            return FALSE;
        }
    }
    return gst_vmafx_windows_frames_drain(self, drain, error);
}

/* Pool every model over the whole stream: the summary message, and the scores the report holds. */
static gboolean post_summary(GstVmafx *self, gchar **error)
{
    GstVmafxRt *rt = self->rt;
    GstStructure *s =
        gst_structure_new("vmafx-summary", "n_frames", G_TYPE_UINT64, rt->next_index,
                          "host-copy-frames", G_TYPE_UINT64, rt->host_copy_frames, NULL);
    for (guint i = 0; i < rt->n_models; i++) {
        for (guint p = VMAFX_POOL_MIN; p <= VMAFX_POOL_PERC20; p++) {
            VmafxPooledScore out = VMAFX_POOLED_SCORE_INIT;
            VmafxError *err = NULL;
            if (!(rt->pool_mask & (1u << p))) {
                continue;
            }
            const VmafxStatus status = vmafx_score_pooled(rt->context, rt->models[i], p, 0,
                                                          rt->next_index - 1, &out, &err);
            if (status != VMAFX_OK) {
                *error = gst_vmafx_status_text("pooled score", status, err);
                gst_structure_free(s);
                return FALSE;
            }
            gchar *field =
                g_strdup_printf("%s.%s", vmafx_model_name(rt->models[i]), gst_vmafx_pool_name(p));
            gst_structure_set(s, field, G_TYPE_DOUBLE, out.value, NULL);
            g_free(field);
        }
    }
    gchar *text = gst_structure_to_string(s);
    GST_INFO_OBJECT(self, "summary %s", text);
    g_free(text);
    post(self, s);
    return TRUE;
}

static gboolean write_report(GstVmafx *self, gchar **error)
{
    static const uint32_t formats[] = {VMAFX_REPORT_FORMAT_JSON, VMAFX_REPORT_FORMAT_XML,
                                       VMAFX_REPORT_FORMAT_CSV, VMAFX_REPORT_FORMAT_SUB};
    const GstVmafxOpts *o = &self->opts;
    VmafxError *err = NULL;
    if (o->output == NULL) {
        return TRUE;
    }
    const uint32_t format = formats[CLAMP(o->output_format, 0, 3)];
    const uint32_t flags =
        ((o->provenance & GST_VMAFX_PROVENANCE_REPORT) &&
         (format == VMAFX_REPORT_FORMAT_CSV || format == VMAFX_REPORT_FORMAT_SUB)) ?
            VMAFX_REPORT_PROVENANCE_SIDECAR :
            0;
    const VmafxStatus status =
        vmafx_report_write(self->rt->context, o->output, format, flags, o->score_fmt, &err);
    if (status != VMAFX_OK) {
        *error = gst_vmafx_status_text("report", status, err);
        return FALSE;
    }
    return TRUE;
}

/* End of stream: flush, close the windows, post what is left, write the report. */
gboolean gst_vmafx_finish_stream(GstVmafx *self, gchar **error)
{
    GstVmafxRt *rt = self->rt;
    VmafxError *err = NULL;
    g_assert(rt != NULL);
    if (rt->next_index == 0 || rt->flushed) {
        GST_WARNING_OBJECT(self, "no frame pair was scored");
        return TRUE;
    }
    const VmafxStatus status = vmafx_flush(rt->context, &err);
    rt->flushed = TRUE;
    if (status != VMAFX_OK) {
        *error = gst_vmafx_status_text("flush", status, err);
        return FALSE;
    }
    return (rt->windows == NULL || gst_vmafx_windows_finish(self, error)) &&
           gst_vmafx_report_frames(self, TRUE, error) && post_summary(self, error) &&
           write_report(self, error);
}
