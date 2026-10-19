/*
 * Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Per-window statistics (the `n_stats` and `n_stats_frames` options). The library's window clock
 * cuts the stream into windows and the library pools each one; the element asks for a window per
 * model, collects the results and reports them as a log line, an element message and an NDJSON
 * line.
 */

#include <string.h>

#include "gstvmafx.h"

#define GST_CAT_DEFAULT gst_vmafx_debug

/* How long to wait, after the flush, for a window to complete (it is final by then). */
#define WINDOW_FLUSH_WAIT_NS UINT64_C(60000000000)

/* The windows of one span of the stream: one per model. */
typedef struct {
    VmafxWindowSpan span;
    VmafxWindow *window[GST_VMAFX_MAX_MODELS];
    guint n;
} WinSet;

struct GstVmafxWindows {
    VmafxWindowClock *clock; /* NULL without n_stats / n_stats_frames */
    GQueue sets;             /* WinSet *, oldest first: the windows of the clock */
    GQueue frames;           /* WinSet *, oldest first: one frame each (the metadata option) */
};

static void set_free(WinSet *set)
{
    for (guint i = 0; i < set->n; i++) {
        vmafx_window_release(set->window[i]);
    }
    g_free(set);
}

GstVmafxWindows *gst_vmafx_windows_new(GstVmafx *self, gchar **error)
{
    VmafxWindowClockConfig cfg = VMAFX_WINDOW_CLOCK_CONFIG_INIT;
    VmafxError *err = NULL;
    GstVmafxWindows *w = g_new0(GstVmafxWindows, 1);
    cfg.n_stats = self->opts.n_stats;
    cfg.n_stats_frames = (uint64_t)self->opts.n_stats_frames;
    g_queue_init(&w->sets);
    g_queue_init(&w->frames);
    if (self->opts.n_stats <= 0 && self->opts.n_stats_frames <= 0) {
        return w; /* per-frame scores only */
    }
    const VmafxStatus status = vmafx_window_clock_create(&cfg, &w->clock, &err);
    if (status != VMAFX_OK) {
        *error = gst_vmafx_status_text("window clock", status, err);
        g_free(w);
        return NULL;
    }
    return w;
}

void gst_vmafx_windows_free(GstVmafxWindows *windows)
{
    if (windows == NULL) {
        return;
    }
    WinSet *set = NULL;
    while ((set = g_queue_pop_head(&windows->sets)) != NULL) {
        set_free(set);
    }
    while ((set = g_queue_pop_head(&windows->frames)) != NULL) {
        set_free(set);
    }
    vmafx_window_clock_destroy(windows->clock);
    g_free(windows);
}

/* Ask for the window of `span` of every model, pooled with `mask`, into `queue`. */
static gboolean submit_set(GstVmafx *self, const VmafxWindowSpan *span, uint32_t mask,
                           GQueue *queue, gchar **error)
{
    GstVmafxRt *rt = self->rt;
    WinSet *set = g_new0(WinSet, 1);
    set->span = *span;
    for (guint i = 0; i < rt->n_models; i++) {
        VmafxWindowRequest req = VMAFX_WINDOW_REQUEST_INIT;
        VmafxError *err = NULL;
        req.target = VMAFX_WINDOW_TARGET_MODEL;
        req.model = rt->models[i];
        req.pool_mask = mask;
        req.first = span->first;
        req.last = span->last;
        const VmafxStatus status = vmafx_window_submit(rt->context, &req, &set->window[i], &err);
        if (status != VMAFX_OK) {
            *error = gst_vmafx_status_text("window", status, err);
            set_free(set);
            return FALSE;
        }
        set->n++;
    }
    g_queue_push_tail(queue, set);
    return TRUE;
}

static gboolean submit_span(GstVmafx *self, const VmafxWindowSpan *span, gchar **error)
{
    return submit_set(self, span, self->rt->pool_mask, &self->rt->windows->sets, error);
}

/* Tell the clock frame `index`; a window it completes is asked for at once. */
gboolean gst_vmafx_windows_frame(GstVmafx *self, uint64_t index, int64_t pts_ns, gchar **error)
{
    VmafxWindowSpan span = VMAFX_WINDOW_SPAN_INIT;
    VmafxError *err = NULL;
    g_assert(self->rt != NULL && self->rt->windows != NULL);
    if (self->rt->windows->clock == NULL) {
        return TRUE;
    }
    const VmafxStatus status =
        vmafx_window_clock_frame(self->rt->windows->clock, index, pts_ns, &span, &err);
    if (status == VMAFX_OK) {
        return submit_span(self, &span, error);
    }
    if (status != VMAFX_PENDING) {
        *error = gst_vmafx_status_text("window clock", status, err);
        return FALSE;
    }
    vmafx_error_free(err);
    return TRUE;
}

/* Collect every window of `set`: 1 all complete, 0 one still open, -1 failure. */
static int set_collect(WinSet *set, uint64_t wait_ns, VmafxWindowResult *res, gchar **error)
{
    for (guint i = 0; i < set->n; i++) {
        VmafxError *err = NULL;
        res[i] = (VmafxWindowResult)VMAFX_WINDOW_RESULT_INIT;
        const VmafxStatus status = vmafx_window_wait(set->window[i], wait_ns, &res[i], &err);
        if (status == VMAFX_PENDING) {
            vmafx_error_free(err);
            return 0;
        }
        if (status != VMAFX_OK || res[i].status != VMAFX_OK) {
            gchar *what = g_strdup_printf("window %" G_GUINT64_FORMAT, set->span.window);
            *error = gst_vmafx_status_text(what, status != VMAFX_OK ? status : res[i].status, err);
            g_free(what);
            return -1;
        }
    }
    return 1;
}

static void append_methods(GString *json, const GstVmafx *self, const VmafxWindowResult *r)
{
    const char *fmt = self->opts.score_fmt;
    gchar num[64];
    gboolean first = TRUE;
    g_string_append_c(json, '{');
    for (guint p = VMAFX_POOL_MIN; p <= VMAFX_POOL_PERC20; p++) {
        if (r->pool_mask & (1u << p)) {
            gst_vmafx_fmt_double(fmt, r->value[p], num, sizeof(num));
            g_string_append_printf(json, "%s\"%s\":%s", first ? "" : ",", gst_vmafx_pool_name(p),
                                   num);
            first = FALSE;
        }
    }
    g_string_append_c(json, '}');
}

/* The window as one NDJSON object: the field names of the FFmpeg filter. */
static gchar *window_json(const GstVmafx *self, const WinSet *set, const VmafxWindowResult *res)
{
    GString *json = g_string_new(NULL);
    gchar start[64];
    gchar end[64];
    gst_vmafx_fmt_double("%.9f", (double)set->span.start_ns / 1e9, start, sizeof(start));
    gst_vmafx_fmt_double("%.9f", (double)set->span.end_ns / 1e9, end, sizeof(end));
    g_string_append_printf(
        json,
        "{\"window\":%" G_GUINT64_FORMAT ",\"start\":%s,\"end\":%s,"
        "\"n_frames\":%" G_GUINT64_FORMAT ",\"n_scored\":%" G_GUINT64_FORMAT ",\"partial\":%s",
        set->span.window, start, end, res[0].n_frames, res[0].n_scored,
        ((set->span.flags | res[0].flags) & VMAFX_WINDOW_PARTIAL) ? "true" : "false");
    for (guint i = 0; i < set->n; i++) {
        g_string_append_c(json, ',');
        gst_vmafx_json_string(json, res[i].name);
        g_string_append_c(json, ':');
        append_methods(json, self, &res[i]);
    }
    g_string_append_c(json, '}');
    return g_string_free(json, FALSE);
}

static void post_window_message(GstVmafx *self, const WinSet *set, const VmafxWindowResult *res)
{
    GstStructure *s = gst_structure_new(
        "vmafx-window", "window", G_TYPE_UINT64, set->span.window, "start", G_TYPE_DOUBLE,
        (double)set->span.start_ns / 1e9, "end", G_TYPE_DOUBLE, (double)set->span.end_ns / 1e9,
        "n_frames", G_TYPE_UINT64, res[0].n_frames, "n_scored", G_TYPE_UINT64, res[0].n_scored,
        "partial", G_TYPE_BOOLEAN, ((set->span.flags | res[0].flags) & VMAFX_WINDOW_PARTIAL) != 0,
        NULL);
    for (guint i = 0; i < set->n; i++) {
        for (guint p = VMAFX_POOL_MIN; p <= VMAFX_POOL_PERC20; p++) {
            if (res[i].pool_mask & (1u << p)) {
                gchar *field = g_strdup_printf("%s.%s", res[i].name, gst_vmafx_pool_name(p));
                gst_structure_set(s, field, G_TYPE_DOUBLE, res[i].value[p], NULL);
                g_free(field);
            }
        }
    }
    gst_element_post_message(GST_ELEMENT(self), gst_message_new_element(GST_OBJECT(self), s));
}

/* A one-frame window is the frame's score: the mean of one value is that value. */
static void post_frame_message(GstVmafx *self, const WinSet *set, const VmafxWindowResult *res)
{
    GstStructure *s =
        gst_structure_new("vmafx-frame", "index", G_TYPE_UINT64, set->span.first, NULL);
    for (guint i = 0; i < set->n; i++) {
        gst_structure_set(s, res[i].name, G_TYPE_DOUBLE, res[i].value[VMAFX_POOL_MEAN], NULL);
    }
    gst_element_post_message(GST_ELEMENT(self), gst_message_new_element(GST_OBJECT(self), s));
}

/* Report a completed window as the `stats_out` option says. */
static gboolean emit_set(GstVmafx *self, const WinSet *set, const VmafxWindowResult *res,
                         gchar **error)
{
    const int out = self->opts.stats_out;
    gchar *json = window_json(self, set, res);
    if (out & GST_VMAFX_STATS_LOG) {
        GST_INFO_OBJECT(self, "window %s", json);
    }
    if (out & GST_VMAFX_STATS_MESSAGE) {
        post_window_message(self, set, res);
    }
    if ((out & GST_VMAFX_STATS_FILE) && self->rt->stats != NULL) {
        if (fprintf(self->rt->stats, "%s\n", json) < 0 || fflush(self->rt->stats) != 0) {
            *error = g_strdup_printf("option stats-path: cannot write '%s'", self->opts.stats_path);
            g_free(json);
            return FALSE;
        }
    }
    g_free(json);
    return TRUE;
}

/* Report every window at the head of the queue that has completed; with a wait, all of them. */
static gboolean drain(GstVmafx *self, GQueue *sets, uint64_t wait_ns, gchar **error)
{
    WinSet *set = NULL;
    while ((set = g_queue_peek_head(sets)) != NULL) {
        VmafxWindowResult res[GST_VMAFX_MAX_MODELS];
        const int ready = set_collect(set, wait_ns, res, error);
        if (ready < 0) {
            return FALSE;
        }
        if (ready == 0) {
            if (wait_ns > 0) {
                *error =
                    g_strdup_printf("window %" G_GUINT64_FORMAT " did not complete after the flush",
                                    set->span.window);
                return FALSE;
            }
            return TRUE;
        }
        if (sets == &self->rt->windows->frames) {
            post_frame_message(self, set, res);
        } else if (!emit_set(self, set, res, error)) {
            return FALSE;
        }
        g_queue_pop_head(sets);
        set_free(set);
    }
    return TRUE;
}

/* Report the windows that completed since the last call. */
gboolean gst_vmafx_windows_poll(GstVmafx *self, gchar **error)
{
    g_assert(self->rt != NULL && self->rt->windows != NULL);
    return self->rt->windows->clock == NULL || drain(self, &self->rt->windows->sets, 0, error);
}

/* End of stream, after the flush: the last (partial) window, then every window. */
gboolean gst_vmafx_windows_finish(GstVmafx *self, gchar **error)
{
    VmafxWindowSpan span = VMAFX_WINDOW_SPAN_INIT;
    VmafxError *err = NULL;
    g_assert(self->rt != NULL && self->rt->windows != NULL);
    if (self->rt->windows->clock == NULL) {
        return TRUE;
    }
    const VmafxStatus status = vmafx_window_clock_finish(self->rt->windows->clock, &span, &err);
    if (status == VMAFX_OK) {
        if (!submit_span(self, &span, error)) {
            return FALSE;
        }
    } else if (status != VMAFX_PENDING) {
        *error = gst_vmafx_status_text("window clock", status, err);
        return FALSE;
    } else {
        vmafx_error_free(err);
    }
    return drain(self, &self->rt->windows->sets, WINDOW_FLUSH_WAIT_NS, error);
}

/* Ask for the score of frame `index` of every model (a window of that one frame). */
gboolean gst_vmafx_windows_score_frame(GstVmafx *self, uint64_t index, gchar **error)
{
    VmafxWindowSpan span = VMAFX_WINDOW_SPAN_INIT;
    g_assert(self->rt != NULL && self->rt->windows != NULL);
    span.window = index;
    span.first = index;
    span.last = index;
    span.n_frames = 1;
    return submit_set(self, &span, VMAFX_POOL_MASK_MEAN, &self->rt->windows->frames, error);
}

/* Post the frames that are final; with `wait`, every frame (after the flush). */
gboolean gst_vmafx_windows_frames_drain(GstVmafx *self, gboolean wait, gchar **error)
{
    g_assert(self->rt != NULL && self->rt->windows != NULL);
    return drain(self, &self->rt->windows->frames, wait ? WINDOW_FLUSH_WAIT_NS : 0, error);
}
