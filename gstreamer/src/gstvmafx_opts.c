/*
 * Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/* GObject properties generated from the shared option table (see gstvmafx_opts.h). */

#include "gstvmafx_opts.h"

#include <gst/gst.h>
#include <limits.h>
#include <string.h>

static const GstVmafxOpt opt_table[] = {VMAFX_FILTER_OPTIONS(GstVmafxOpts, 0)};
#define N_OPTS (G_N_ELEMENTS(opt_table))

static gboolean opt_is_const(const GstVmafxOpt *o)
{
    return o->type == AV_OPT_TYPE_CONST;
}

/* An integer option whose values are named by the constants of its unit. */
static gboolean opt_is_named(const GstVmafxOpt *o)
{
    return (o->type == AV_OPT_TYPE_INT || o->type == AV_OPT_TYPE_FLAGS) && o->unit != NULL;
}

static const GstVmafxOpt *opt_by_id(guint id)
{
    g_assert(id >= 1 && id <= N_OPTS);
    return &opt_table[id - 1];
}

static void *opt_field(const GstVmafxOpts *opts, const GstVmafxOpt *o)
{
    return (char *)opts + o->offset;
}

static const GstVmafxOpt *const_by_name(const char *unit, const char *name)
{
    for (gsize i = 0; i < N_OPTS; i++) {
        const GstVmafxOpt *c = &opt_table[i];
        if (opt_is_const(c) && c->unit && !strcmp(c->unit, unit) && !strcmp(c->name, name)) {
            return c;
        }
    }
    return NULL;
}

const char *gst_vmafx_opts_const_name(const char *unit, int64_t value)
{
    for (gsize i = 0; i < N_OPTS; i++) {
        const GstVmafxOpt *c = &opt_table[i];
        if (opt_is_const(c) && c->unit && !strcmp(c->unit, unit) && c->def.i64 == value) {
            return c->name;
        }
    }
    return NULL;
}

static gboolean const_in_unit(const GstVmafxOpt *c, const char *unit)
{
    return opt_is_const(c) && c->unit != NULL && !strcmp(c->unit, unit);
}

/* `a+b` for a set of flags: the constants of the unit set in `value`, the rest as a number. */
static gchar *flags_text(const GstVmafxOpt *o, int value)
{
    GString *text = g_string_new(NULL);
    int left = value;
    for (gsize i = 0; i < N_OPTS; i++) {
        const GstVmafxOpt *c = &opt_table[i];
        if (const_in_unit(c, o->unit) && c->def.i64 != 0 && (value & c->def.i64)) {
            g_string_append_printf(text, "%s%s", text->len ? "+" : "", c->name);
            left &= ~(int)c->def.i64;
        }
    }
    if (left != 0 || text->len == 0) {
        g_string_append_printf(text, "%s%d", text->len ? "+" : "", left);
    }
    return g_string_free(text, FALSE);
}

/* The text of a named value: the constant's name, `a+b` for a set of flags, else the number. */
static gchar *named_text(const GstVmafxOpt *o, int value)
{
    if (o->type == AV_OPT_TYPE_FLAGS) {
        return flags_text(o, value);
    }
    const char *name = gst_vmafx_opts_const_name(o->unit, value);
    return name ? g_strdup(name) : g_strdup_printf("%d", value);
}

/* One token: a constant of the unit, or a whole number. */
static gboolean token_value(const GstVmafxOpt *o, const char *token, int64_t *value)
{
    const GstVmafxOpt *c = const_by_name(o->unit, token);
    if (c) {
        *value = c->def.i64;
        return TRUE;
    }
    gchar *end = NULL;
    const gint64 n = g_ascii_strtoll(token, &end, 0);
    if (token[0] == '\0' || *end != '\0') {
        return FALSE;
    }
    *value = n;
    return TRUE;
}

static gchar *parse_named(const GstVmafxOpt *o, const char *text, int *out)
{
    gchar **tokens = g_strsplit(text ? text : "", o->type == AV_OPT_TYPE_FLAGS ? "+" : "\x01", -1);
    int64_t total = 0;
    gchar *error = NULL;
    for (guint i = 0; tokens[i] && !error; i++) {
        int64_t v = 0;
        gchar *token = g_strstrip(tokens[i]);
        if (!token_value(o, token, &v)) {
            error = g_strdup_printf("option %s: '%s' is not a value of it", o->name, token);
        } else {
            total = o->type == AV_OPT_TYPE_FLAGS ? (total | v) : v;
        }
    }
    g_strfreev(tokens);
    if (!error && (total < o->min || total > o->max)) {
        error = g_strdup_printf("option %s: %" G_GINT64_FORMAT " is outside %g to %g", o->name,
                                (gint64)total, o->min, o->max);
    }
    *out = (int)total;
    return error;
}

static gdouble clamp_range(gdouble v, gdouble lo, gdouble hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static GParamSpec *make_pspec(const GstVmafxOpt *o, const gchar *name)
{
    const GParamFlags fl = G_PARAM_READWRITE | GST_PARAM_MUTABLE_READY;
    switch (o->type) {
    case AV_OPT_TYPE_BOOL:
        return g_param_spec_boolean(name, o->name, o->help, o->def.i64 != 0, fl);
    case AV_OPT_TYPE_INT:
    case AV_OPT_TYPE_FLAGS:
        if (opt_is_named(o)) {
            gchar *def = named_text(o, (int)o->def.i64);
            GParamSpec *p = g_param_spec_string(name, o->name, o->help, def, fl);
            g_free(def);
            return p;
        }
        return g_param_spec_int(
            name, o->name, o->help, (gint)clamp_range(o->min, G_MININT, G_MAXINT),
            (gint)clamp_range(o->max, G_MININT, G_MAXINT), (gint)o->def.i64, fl);
    case AV_OPT_TYPE_INT64:
        return g_param_spec_int64(name, o->name, o->help,
                                  o->min <= -9.2e18 ? G_MININT64 : (gint64)o->min,
                                  o->max >= 9.2e18 ? G_MAXINT64 : (gint64)o->max, o->def.i64, fl);
    case AV_OPT_TYPE_DOUBLE:
        return g_param_spec_double(name, o->name, o->help, o->min, o->max, o->def.dbl, fl);
    default:
        return g_param_spec_string(name, o->name, o->help, o->def.str, fl);
    }
}

void gst_vmafx_opts_install(GObjectClass *klass)
{
    g_assert(klass != NULL);
    for (gsize i = 0; i < N_OPTS; i++) {
        const GstVmafxOpt *o = &opt_table[i];
        if (opt_is_const(o)) {
            continue;
        }
        gchar *name = g_strdup(o->name);
        g_strdelimit(name, "_", '-');
        g_object_class_install_property(klass, (guint)i + 1, make_pspec(o, name));
        g_free(name);
    }
}

void gst_vmafx_opts_init(GstVmafxOpts *opts)
{
    memset(opts, 0, sizeof(*opts));
    for (gsize i = 0; i < N_OPTS; i++) {
        const GstVmafxOpt *o = &opt_table[i];
        void *field = opt_field(opts, o);
        if (opt_is_const(o)) {
            continue;
        } else if (o->type == AV_OPT_TYPE_INT64) {
            *(int64_t *)field = o->def.i64;
        } else if (o->type == AV_OPT_TYPE_DOUBLE) {
            *(double *)field = o->def.dbl;
        } else if (o->type == AV_OPT_TYPE_STRING) {
            *(char **)field = g_strdup(o->def.str);
        } else {
            *(int *)field = (int)o->def.i64;
        }
    }
}

void gst_vmafx_opts_clear(GstVmafxOpts *opts)
{
    for (gsize i = 0; i < N_OPTS; i++) {
        const GstVmafxOpt *o = &opt_table[i];
        if (o->type == AV_OPT_TYPE_STRING) {
            g_clear_pointer((char **)opt_field(opts, o), g_free);
        }
    }
}

gchar *gst_vmafx_opts_set(GstVmafxOpts *opts, guint id, const GValue *value)
{
    const GstVmafxOpt *o = opt_by_id(id);
    void *field = opt_field(opts, o);
    if (opt_is_named(o)) {
        return parse_named(o, g_value_get_string(value), (int *)field);
    }
    switch (o->type) {
    case AV_OPT_TYPE_BOOL:
        *(int *)field = g_value_get_boolean(value) ? 1 : 0;
        break;
    case AV_OPT_TYPE_INT:
    case AV_OPT_TYPE_FLAGS:
        *(int *)field = g_value_get_int(value);
        break;
    case AV_OPT_TYPE_INT64:
        *(int64_t *)field = g_value_get_int64(value);
        break;
    case AV_OPT_TYPE_DOUBLE:
        *(double *)field = g_value_get_double(value);
        break;
    default:
        g_free(*(char **)field);
        *(char **)field = g_value_dup_string(value);
        break;
    }
    return NULL;
}

void gst_vmafx_opts_get(const GstVmafxOpts *opts, guint id, GValue *value)
{
    const GstVmafxOpt *o = opt_by_id(id);
    const void *field = opt_field(opts, o);
    if (opt_is_named(o)) {
        gchar *text = named_text(o, *(const int *)field);
        g_value_take_string(value, text);
        return;
    }
    switch (o->type) {
    case AV_OPT_TYPE_BOOL:
        g_value_set_boolean(value, *(const int *)field != 0);
        break;
    case AV_OPT_TYPE_INT:
    case AV_OPT_TYPE_FLAGS:
        g_value_set_int(value, *(const int *)field);
        break;
    case AV_OPT_TYPE_INT64:
        g_value_set_int64(value, *(const int64_t *)field);
        break;
    case AV_OPT_TYPE_DOUBLE:
        g_value_set_double(value, *(const double *)field);
        break;
    default:
        g_value_set_string(value, *(char *const *)field);
        break;
    }
}
