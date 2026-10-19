/*
 * Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/* Small helpers shared by the element's files: error text, number and JSON formatting, logging. */

#include <math.h>
#include <string.h>

#include "gstvmafx.h"

#define GST_CAT_DEFAULT gst_vmafx_debug

const char *gst_vmafx_pad_name(guint pad)
{
    return pad == GST_VMAFX_PAD_REFERENCE ? "reference" : "distorted";
}

/* "<what>: <message> (<status>, <subject>)"; frees `error`. */
gchar *gst_vmafx_status_text(const char *what, VmafxStatus status, VmafxError *error)
{
    const char *message = vmafx_error_message(error);
    const char *subject = vmafx_error_subject(error);
    gchar *text = g_strdup_printf("%s: %s (%s%s%s)", what, message[0] ? message : "failed",
                                  vmafx_status_name(status), subject[0] ? ", " : "", subject);
    vmafx_error_free(error);
    return text;
}

/* `score_fmt` is `%[flags][width][.precision]` and one of e, E, f, F, g, G: g_ascii_formatd()'s
 * contract, and one printf conversion of one double. */
gboolean gst_vmafx_score_fmt_ok(const char *fmt)
{
    if (fmt == NULL || fmt[0] != '%') {
        return FALSE;
    }
    const char *p = fmt + 1;
    p += strspn(p, "-+ #0");
    p += strspn(p, "0123456789");
    if (*p == '.') {
        p++;
        p += strspn(p, "0123456789");
    }
    return p[0] != '\0' && strchr("eEfFgG", p[0]) != NULL && p[1] == '\0';
}

/* The number as `fmt` prints it, in the C locale; `null` for a value JSON has no spelling of. */
void gst_vmafx_fmt_double(const char *fmt, double value, gchar *buf, gsize size)
{
    g_assert(buf != NULL && size >= G_ASCII_DTOSTR_BUF_SIZE);
    if (!isfinite(value)) {
        g_strlcpy(buf, "null", size);
        return;
    }
    g_ascii_formatd(buf, (gint)size, fmt, value);
}

void gst_vmafx_json_string(GString *out, const char *text)
{
    g_string_append_c(out, '"');
    for (const guchar *p = (const guchar *)text; *p != '\0'; p++) {
        if (*p == '"' || *p == '\\') {
            g_string_append_printf(out, "\\%c", *p);
        } else if (*p < 0x20) {
            g_string_append_printf(out, "\\u%04x", *p);
        } else {
            g_string_append_c(out, (gchar)*p);
        }
    }
    g_string_append_c(out, '"');
}

const char *gst_vmafx_pool_name(guint pool)
{
    static const char *const names[] = {"none",   "min",   "max",    "mean",  "harmonic_mean",
                                        "median", "perc5", "perc10", "perc20"};
    return pool < G_N_ELEMENTS(names) ? names[pool] : "unknown";
}

/* Library messages go to the element's debug category; runs on library threads. */
void gst_vmafx_log_callback(uint32_t level, const char *message, void *user)
{
    GstObject *obj = GST_OBJECT_CAST(user);
    switch (level) {
    case VMAFX_LOG_LEVEL_ERROR:
        GST_ERROR_OBJECT(obj, "libvmafx: %s", message);
        break;
    case VMAFX_LOG_LEVEL_WARNING:
        GST_WARNING_OBJECT(obj, "libvmafx: %s", message);
        break;
    case VMAFX_LOG_LEVEL_INFO:
        GST_INFO_OBJECT(obj, "libvmafx: %s", message);
        break;
    default:
        GST_DEBUG_OBJECT(obj, "libvmafx: %s", message);
        break;
    }
}
