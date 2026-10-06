/*
 * Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * vmafx: scores a distorted video against a reference with the VMAFx C API and passes the
 * distorted buffers on unchanged. An aggregator with two always sink pads; frames pair in arrival
 * order. Properties come from the option table the FFmpeg filter uses (gstvmafx_opts.c).
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <locale.h>
#include <string.h>

#include "gstvmafx.h"

GST_DEBUG_CATEGORY(gst_vmafx_debug);
#define GST_CAT_DEFAULT gst_vmafx_debug

#define GST_VMAFX_VERSION_STRING "1.0.0"

G_DEFINE_TYPE(GstVmafx, gst_vmafx, GST_TYPE_AGGREGATOR)

static void gst_vmafx_set_property(GObject *object, guint id, const GValue *value, GParamSpec *ps)
{
    GstVmafx *self = GST_VMAFX(object);
    g_mutex_lock(&self->lock);
    gchar *error = gst_vmafx_opts_set(&self->opts, id, value);
    if (error != NULL && self->prop_error == NULL) {
        self->prop_error = error; /* reported when the element starts */
    } else {
        g_free(error);
    }
    g_mutex_unlock(&self->lock);
    (void)ps;
}

static void gst_vmafx_get_property(GObject *object, guint id, GValue *value, GParamSpec *ps)
{
    GstVmafx *self = GST_VMAFX(object);
    g_mutex_lock(&self->lock);
    gst_vmafx_opts_get(&self->opts, id, value);
    g_mutex_unlock(&self->lock);
    (void)ps;
}

static void gst_vmafx_finalize(GObject *object)
{
    GstVmafx *self = GST_VMAFX(object);
    gst_vmafx_rt_free(self);
    gst_vmafx_opts_clear(&self->opts);
    g_free(self->prop_error);
#ifdef G_OS_UNIX
    if (self->c_locale) {
        freelocale((locale_t)self->c_locale);
    }
#endif
    g_mutex_clear(&self->lock);
    G_OBJECT_CLASS(gst_vmafx_parent_class)->finalize(object);
}

static gboolean gst_vmafx_start(GstAggregator *agg)
{
    GstVmafx *self = GST_VMAFX(agg);
    gchar *why = NULL;
    g_mutex_lock(&self->lock);
    if (self->prop_error != NULL) {
        why = g_strdup(self->prop_error);
    } else {
        why = gst_vmafx_check_backend(&self->opts);
    }
    g_mutex_unlock(&self->lock);
    self->failed = FALSE;
    if (why != NULL) {
        GST_ELEMENT_ERROR(self, RESOURCE, SETTINGS, ("%s", why), (NULL));
        g_free(why);
        return FALSE;
    }
    return TRUE;
}

static gboolean gst_vmafx_stop(GstAggregator *agg)
{
    GstVmafx *self = GST_VMAFX(agg);
    gst_vmafx_rt_free(self);
    g_mutex_lock(&self->lock);
    self->pad_info[0].valid = self->pad_info[1].valid = FALSE;
    g_mutex_unlock(&self->lock);
    return TRUE;
}

/* A refused layout fails the negotiation of its pad with a message naming the format. */
static GstFlowReturn gst_vmafx_sink_event_pre_queue(GstAggregator *agg, GstAggregatorPad *apad,
                                                    GstEvent *event)
{
    GstVmafx *self = GST_VMAFX(agg);
    GstAggregatorClass *parent = GST_AGGREGATOR_CLASS(gst_vmafx_parent_class);
    if (GST_EVENT_TYPE(event) == GST_EVENT_CAPS) {
        const guint idx = apad == self->pad[GST_VMAFX_PAD_REFERENCE] ? 0 : 1;
        GstCaps *caps = NULL;
        GstVmafxPad info = {0};
        gst_event_parse_caps(event, &caps);
        if (caps == NULL || !gst_video_info_from_caps(&info.info, caps)) {
            GST_ELEMENT_ERROR(self, STREAM, FORMAT,
                              ("pad %s: the caps are not raw video", gst_vmafx_pad_name(idx)),
                              (NULL));
            gst_event_unref(event);
            return GST_FLOW_NOT_NEGOTIATED;
        }
        info.valid = TRUE;
        info.mem = gst_vmafx_caps_is_cuda(caps) ? GST_VMAFX_MEM_CUDA : GST_VMAFX_MEM_SYSTEM;
        g_mutex_lock(&self->lock);
        gchar *why = gst_vmafx_check_caps(&self->opts, idx, &info, &self->pad_info[1 - idx]);
        if (why == NULL) {
            self->pad_info[idx] = info;
        }
        g_mutex_unlock(&self->lock);
        if (why != NULL) {
            GST_ELEMENT_ERROR(self, STREAM, FORMAT, ("%s", why), (NULL));
            g_free(why);
            gst_event_unref(event);
            return GST_FLOW_NOT_NEGOTIATED;
        }
    }
    return parent->sink_event_pre_queue != NULL ? parent->sink_event_pre_queue(agg, apad, event) :
                                                  (gst_event_unref(event), GST_FLOW_OK);
}

/* The output is the distorted input, as it is. */
static GstFlowReturn gst_vmafx_update_src_caps(GstAggregator *agg, GstCaps *caps, GstCaps **ret)
{
    GstVmafx *self = GST_VMAFX(agg);
    GstCaps *dist = gst_pad_get_current_caps(GST_PAD(self->pad[GST_VMAFX_PAD_DISTORTED]));
    if (dist == NULL) {
        return GST_AGGREGATOR_FLOW_NEED_DATA;
    }
    *ret = gst_caps_intersect(dist, caps);
    gst_caps_unref(dist);
    if (gst_caps_is_empty(*ret)) {
        gst_caps_replace(ret, NULL);
        return GST_FLOW_NOT_NEGOTIATED;
    }
    return GST_FLOW_OK;
}

/* Upstream may recycle its buffers only after the engine let go of them: ask for enough. */
static gboolean gst_vmafx_propose_allocation(GstAggregator *agg, GstAggregatorPad *apad,
                                             GstQuery *decide_query, GstQuery *query)
{
    GstVmafx *self = GST_VMAFX(agg);
    GstCaps *caps = NULL;
    GstVideoInfo info;
    guint in_flight = 0;
    (void)apad;
    (void)decide_query;
    gst_query_parse_allocation(query, &caps, NULL);
    if (caps == NULL || !gst_video_info_from_caps(&info, caps)) {
        return FALSE;
    }
    g_mutex_lock(&self->lock);
    if (self->rt != NULL) {
        in_flight = vmafx_context_max_in_flight(self->rt->context);
    } else {
        /* No context yet: the largest bound (two earlier reference frames, one device frame). */
        const guint threads = (guint)CLAMP(self->opts.threads, 0, 256);
        in_flight = 2 + 2 * threads * 3 + 1;
    }
    g_mutex_unlock(&self->lock);
    gst_query_add_allocation_pool(query, NULL, GST_VIDEO_INFO_SIZE(&info), in_flight + 1, 0);
    gst_query_add_allocation_meta(query, GST_VIDEO_META_API_TYPE, NULL);
    return TRUE;
}

/* A failure of the stream: posted once, the element scores nothing more. */
static GstFlowReturn fail(GstVmafx *self, gchar *why)
{
    self->failed = TRUE;
    GST_ELEMENT_ERROR(self, STREAM, FAILED, ("%s", why), (NULL));
    g_free(why);
    return GST_FLOW_ERROR;
}

static int64_t buffer_pts_ns(const GstVmafx *self, GstBuffer *dist)
{
    if (GST_BUFFER_PTS_IS_VALID(dist)) {
        return (int64_t)GST_BUFFER_PTS(dist);
    }
    const gint n = GST_VIDEO_INFO_FPS_N(&self->rt->info);
    const gint d = GST_VIDEO_INFO_FPS_D(&self->rt->info);
    return n > 0 ? (int64_t)gst_util_uint64_scale(self->rt->next_index * GST_SECOND, d, n) : 0;
}

/* Score one pair; the frames are consumed on every path. */
static gchar *score_pair(GstVmafx *self, GstBuffer *ref, GstBuffer *dist)
{
    GstVmafxRt *rt = self->rt;
    VmafxFrame *rf = NULL;
    VmafxFrame *df = NULL;
    VmafxError *err = NULL;
    gchar *why = NULL;
    if (rt->windows != NULL &&
        !gst_vmafx_windows_frame(self, rt->next_index, buffer_pts_ns(self, dist), &why)) {
        return why;
    }
    if (gst_vmafx_frame_make(self, rt, ref, GST_VMAFX_PAD_REFERENCE, &rf, &why) != VMAFX_OK) {
        return why;
    }
    if (gst_vmafx_frame_make(self, rt, dist, GST_VMAFX_PAD_DISTORTED, &df, &why) != VMAFX_OK) {
        vmafx_frame_unref(rf);
        return why;
    }
    const VmafxStatus status = vmafx_submit(rt->context, rf, df, rt->next_index, &err);
    if (status != VMAFX_OK) {
        return gst_vmafx_status_text("submit", status, err);
    }
    rt->next_index++;
    if (rt->windows != NULL && !gst_vmafx_windows_poll(self, &why)) {
        return why;
    }
    gst_vmafx_report_frames(self, FALSE, &why);
    return why;
}

static GstFlowReturn process_pair(GstVmafx *self, GstBuffer *ref, GstBuffer *dist)
{
    gchar *why = NULL;
    if (self->rt == NULL) {
        if (!gst_vmafx_rt_create(self, ref, &why) ||
            (self->opts.provenance & GST_VMAFX_PROVENANCE_LOG &&
             !gst_vmafx_post_provenance(self, &why))) {
            gst_buffer_unref(ref);
            gst_buffer_unref(dist);
            return fail(self, why);
        }
    }
    why = score_pair(self, ref, dist);
    gst_buffer_unref(ref);
    if (why != NULL) {
        gst_buffer_unref(dist);
        return fail(self, why);
    }
    return gst_aggregator_finish_buffer(GST_AGGREGATOR(self), dist);
}

static GstFlowReturn finish(GstVmafx *self)
{
    gchar *why = NULL;
    if (self->rt != NULL && !self->failed && !gst_vmafx_finish_stream(self, &why)) {
        return fail(self, why);
    }
    return GST_FLOW_EOS;
}

/* One input ended: a buffer of the other has no partner and is not scored. */
static void drop_unpaired(GstVmafx *self, GstAggregatorPad *pad)
{
    GST_WARNING_OBJECT(self, "dropping an unpaired buffer: the other input ended");
    gst_buffer_unref(gst_aggregator_pad_pop_buffer(pad));
}

/* No pair this time: end of stream, an input that ended early, or more data needed. */
static GstFlowReturn no_pair(GstVmafx *self, GstBuffer *ref, GstBuffer *dist)
{
    GstAggregatorPad *rp = self->pad[GST_VMAFX_PAD_REFERENCE];
    GstAggregatorPad *dp = self->pad[GST_VMAFX_PAD_DISTORTED];
    const gboolean ref_eos = gst_aggregator_pad_is_eos(rp);
    const gboolean dist_eos = gst_aggregator_pad_is_eos(dp);
    if (self->failed) {
        return GST_FLOW_ERROR;
    }
    if (ref != NULL && dist_eos) {
        drop_unpaired(self, rp);
        return GST_FLOW_OK;
    }
    if (dist != NULL && ref_eos) {
        drop_unpaired(self, dp);
        return GST_FLOW_OK;
    }
    if (ref == NULL && dist == NULL && ref_eos && dist_eos) {
        return finish(self);
    }
    return GST_AGGREGATOR_FLOW_NEED_DATA;
}

static GstFlowReturn aggregate_pair(GstVmafx *self)
{
    GstAggregatorPad *rp = self->pad[GST_VMAFX_PAD_REFERENCE];
    GstAggregatorPad *dp = self->pad[GST_VMAFX_PAD_DISTORTED];
    GstBuffer *ref = gst_aggregator_pad_peek_buffer(rp);
    GstBuffer *dist = gst_aggregator_pad_peek_buffer(dp);
    if (ref != NULL && dist != NULL && !self->failed) {
        gst_buffer_unref(gst_aggregator_pad_pop_buffer(rp));
        gst_buffer_unref(gst_aggregator_pad_pop_buffer(dp));
        return process_pair(self, ref, dist);
    }
    const GstFlowReturn ret = no_pair(self, ref, dist);
    gst_buffer_replace(&ref, NULL);
    gst_buffer_replace(&dist, NULL);
    return ret;
}

/* The library reads the numbers of options and model files with the thread's locale, and an
 * application may have set a decimal comma (gst-launch does): score in the C locale. */
static GstFlowReturn gst_vmafx_aggregate(GstAggregator *agg, gboolean timeout)
{
    GstVmafx *self = GST_VMAFX(agg);
    (void)timeout;
#ifdef G_OS_UNIX
    const locale_t previous = self->c_locale ? uselocale((locale_t)self->c_locale) : (locale_t)0;
    const GstFlowReturn ret = aggregate_pair(self);
    if (self->c_locale) {
        uselocale(previous);
    }
    return ret;
#else
    return aggregate_pair(self);
#endif
}

static void gst_vmafx_class_init(GstVmafxClass *klass)
{
    GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
    GstElementClass *element_class = GST_ELEMENT_CLASS(klass);
    GstAggregatorClass *agg_class = GST_AGGREGATOR_CLASS(klass);
    GstCaps *caps = gst_vmafx_template_caps();

    gobject_class->set_property = gst_vmafx_set_property;
    gobject_class->get_property = gst_vmafx_get_property;
    gobject_class->finalize = gst_vmafx_finalize;
    gst_vmafx_opts_install(gobject_class);

    gst_element_class_set_static_metadata(
        element_class, "VMAFx", "Filter/Analyzer/Video",
        "Scores a distorted video against a reference with VMAF and the VMAFx metrics",
        "Lusoris <vmafx@users.noreply.github.com>");
    gst_element_class_add_pad_template(
        element_class, gst_pad_template_new_with_gtype("reference", GST_PAD_SINK, GST_PAD_ALWAYS,
                                                       caps, GST_TYPE_AGGREGATOR_PAD));
    gst_element_class_add_pad_template(
        element_class, gst_pad_template_new_with_gtype("distorted", GST_PAD_SINK, GST_PAD_ALWAYS,
                                                       caps, GST_TYPE_AGGREGATOR_PAD));
    gst_element_class_add_pad_template(
        element_class, gst_pad_template_new_with_gtype("src", GST_PAD_SRC, GST_PAD_ALWAYS, caps,
                                                       GST_TYPE_AGGREGATOR_PAD));
    gst_caps_unref(caps);

    agg_class->start = gst_vmafx_start;
    agg_class->stop = gst_vmafx_stop;
    agg_class->aggregate = gst_vmafx_aggregate;
    agg_class->update_src_caps = gst_vmafx_update_src_caps;
    agg_class->propose_allocation = gst_vmafx_propose_allocation;
    agg_class->sink_event_pre_queue = gst_vmafx_sink_event_pre_queue;
}

static void gst_vmafx_init(GstVmafx *self)
{
    GstElementClass *klass = GST_ELEMENT_GET_CLASS(self);
    static const char *const names[2] = {"reference", "distorted"};
    g_mutex_init(&self->lock);
#ifdef G_OS_UNIX
    self->c_locale = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
#endif
    gst_vmafx_opts_init(&self->opts);
    for (guint i = 0; i < 2; i++) {
        GstPadTemplate *templ = gst_element_class_get_pad_template(klass, names[i]);
        self->pad[i] = GST_AGGREGATOR_PAD(gst_pad_new_from_template(templ, names[i]));
        gst_element_add_pad(GST_ELEMENT(self), GST_PAD(self->pad[i]));
    }
}

static gboolean plugin_init(GstPlugin *plugin)
{
    GST_DEBUG_CATEGORY_INIT(gst_vmafx_debug, "vmafx", 0, "VMAFx video quality element");
    return gst_element_register(plugin, "vmafx", GST_RANK_NONE, GST_TYPE_VMAFX);
}

/* GST_PLUGIN_DEFINE leaves the reserved members of GstPluginDesc out. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR, vmafx, "VMAFx video quality scoring",
                  plugin_init, GST_VMAFX_VERSION_STRING, GST_LICENSE_PLACEHOLDER, "gst-vmafx",
                  "https://github.com/VMAFx/vmafx")
#pragma GCC diagnostic pop
