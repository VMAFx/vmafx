/*
 * Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * The properties of the vmafx element come from the option table the FFmpeg
 * filter uses (ffmpeg-patches/src/vf_vmafx_options.h, generated from
 * core/api/vmafx.toml). The table is written for libavutil's AVOption; this
 * header gives it a local struct and the local type constants it names, so the
 * element has no libavutil dependency and one table serves both consumers.
 */

#ifndef GST_VMAFX_OPTS_H
#define GST_VMAFX_OPTS_H

#include <glib-object.h>
#include <stddef.h>
#include <stdint.h>

/* Option kinds the table uses. */
enum {
    AV_OPT_TYPE_FLAGS = 1,
    AV_OPT_TYPE_INT,
    AV_OPT_TYPE_INT64,
    AV_OPT_TYPE_DOUBLE,
    AV_OPT_TYPE_STRING,
    AV_OPT_TYPE_BOOL,
    AV_OPT_TYPE_CONST
};

/* One row of the table, in the member order of libavutil's AVOption. */
typedef struct GstVmafxOpt {
    const char *name;
    const char *help;
    int offset;
    int type;
    union {
        int64_t i64;
        double dbl;
        const char *str;
    } def;
    double min;
    double max;
    int flags;
    const char *unit;
} GstVmafxOpt;

#include <vf_vmafx_options.h>

/* The values of the options, one field per option (the table's own fields). */
typedef struct GstVmafxOpts {
    VMAFX_FILTER_OPTION_FIELDS
} GstVmafxOpts;

/* Set every field to its default; strings are owned by the struct. */
void gst_vmafx_opts_init(GstVmafxOpts *opts);

/* Free the strings. */
void gst_vmafx_opts_clear(GstVmafxOpts *opts);

/* Install one GObject property per non-constant table row (ids start at 1). */
void gst_vmafx_opts_install(GObjectClass *klass);

/* Store `value` in the field of property `id`; NULL, or a newly allocated message when the text
 * names no value of the option. */
gchar *gst_vmafx_opts_set(GstVmafxOpts *opts, guint id, const GValue *value);

/* Read the field of property `id` into `value`. */
void gst_vmafx_opts_get(const GstVmafxOpts *opts, guint id, GValue *value);

/* Name of option `unit` constant with value `value`, or NULL. */
const char *gst_vmafx_opts_const_name(const char *unit, int64_t value);

#endif /* GST_VMAFX_OPTS_H */
