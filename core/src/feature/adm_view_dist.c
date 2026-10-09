/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 */

/* The second ADM viewing distance through the option table (ADR-2795); see
 * adm_view_dist.h. */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "adm_view_dist.h"
#include "feature_name.h"
#include "log.h"
#include "opt.h"
#include "thread_locale.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe. ADR-1138. */

const char *const vmaf_adm_view_names[VMAF_ADM_VIEW_SCORE_COUNT] = {
    "VMAF_integer_feature_adm2_score",
    "VMAF_integer_feature_aim_score",
    "VMAF_integer_feature_adm3_score",
    "integer_adm_scale0",
    "integer_adm_scale1",
    "integer_adm_scale2",
    "integer_adm_scale3",
};

const char *const vmaf_adm_extra_view_keys[VMAF_ADM_VIEW_SCORE_COUNT] = {
    "VMAF_integer_feature_adm2_score" VMAF_ADM_EXTRA_VIEW_KEY_SUFFIX,
    "VMAF_integer_feature_aim_score" VMAF_ADM_EXTRA_VIEW_KEY_SUFFIX,
    "VMAF_integer_feature_adm3_score" VMAF_ADM_EXTRA_VIEW_KEY_SUFFIX,
    "integer_adm_scale0" VMAF_ADM_EXTRA_VIEW_KEY_SUFFIX,
    "integer_adm_scale1" VMAF_ADM_EXTRA_VIEW_KEY_SUFFIX,
    "integer_adm_scale2" VMAF_ADM_EXTRA_VIEW_KEY_SUFFIX,
    "integer_adm_scale3" VMAF_ADM_EXTRA_VIEW_KEY_SUFFIX,
};

static const char NVD[] = "adm_norm_view_dist";
static const char NVD_EXTRA[] = "adm_norm_view_dist_extra";

/* The option `name` of `fex` when it has type `type`, else NULL. */
static const VmafOption *adm_option(const VmafFeatureExtractor *fex, const char *name,
                                    enum VmafOptionType type)
{
    if (!fex || !fex->options) {
        return NULL;
    }
    for (unsigned i = 0; fex->options[i].name; i++) {
        if (!strcmp(fex->options[i].name, name)) {
            return fex->options[i].type == type ? &fex->options[i] : NULL;
        }
    }
    return NULL;
}

static double read_double(const void *obj, const VmafOption *opt)
{
    double v = 0.0;
    (void)memcpy(&v, (const char *)obj + opt->offset, sizeof(v));
    return v;
}

static void write_double(void *obj, const VmafOption *opt, double v)
{
    (void)memcpy((char *)obj + opt->offset, &v, sizeof(v));
}

/* A bool option of the parsed options, false when `fex` does not declare it
 * (adm_hip has no `adm_skip_aim`). */
static bool read_flag(const VmafFeatureExtractor *fex, const char *name)
{
    const VmafOption *opt = adm_option(fex, name, VMAF_OPT_TYPE_BOOL);
    bool v = false;
    if (opt && fex->priv) {
        (void)memcpy(&v, (const char *)fex->priv + opt->offset, sizeof(v));
    }
    return v;
}

unsigned vmaf_adm_view_count(const VmafFeatureExtractor *fex)
{
    if (!fex || !fex->priv || !adm_option(fex, NVD, VMAF_OPT_TYPE_DOUBLE)) {
        return 0u;
    }
    const VmafOption *extra = adm_option(fex, NVD_EXTRA, VMAF_OPT_TYPE_DOUBLE);
    return (extra && read_double(fex->priv, extra) > 0.0) ? 2u : 1u;
}

double vmaf_adm_view_dist(const VmafFeatureExtractor *fex, unsigned view)
{
    const VmafOption *opt = adm_option(fex, view ? NVD_EXTRA : NVD, VMAF_OPT_TYPE_DOUBLE);
    return (opt && fex->priv) ? read_double(fex->priv, opt) : 0.0;
}

/* A copy of `fex->priv` with `adm_norm_view_dist` set to `value`, or NULL.
 * Option fields only are read from it: the names are rendered from them. */
static void *priv_at_view(const VmafFeatureExtractor *fex, double value)
{
    const VmafOption *nvd = adm_option(fex, NVD, VMAF_OPT_TYPE_DOUBLE);
    if (!nvd || !fex->priv || fex->priv_size == 0u) {
        return NULL;
    }
    void *copy = malloc(fex->priv_size);
    if (copy) {
        (void)memcpy(copy, fex->priv, fex->priv_size);
        write_double(copy, nvd, value);
    }
    return copy;
}

/* Whether `a` and `b` name their features alike once both viewing distances
 * are the default: the names carry every feature parameter, the basis the
 * registry deduplicates on. 1, 0, or -ENOMEM. */
static int same_except_view(const VmafFeatureExtractor *a, const VmafFeatureExtractor *b)
{
    const double neutral = adm_option(a, NVD, VMAF_OPT_TYPE_DOUBLE)->default_val.d;
    void *va = priv_at_view(a, neutral);
    void *vb = priv_at_view(b, neutral);
    char *name_a = va ? vmaf_feature_name_from_options(a->name, a->options, va) : NULL;
    char *name_b = vb ? vmaf_feature_name_from_options(b->name, b->options, vb) : NULL;
    const int same = (name_a && name_b) ? (strcmp(name_a, name_b) == 0) : -ENOMEM;
    free(name_a);
    free(name_b);
    free(va);
    free(vb);
    return same;
}

/* Record the absorbed distance in the context's options, which the worker
 * pool builds its instances from; `%.17g` in the C locale reads back exactly. */
static int record_extra_view(VmafFeatureExtractorContext *existing, double nvd)
{
    char text[32];
    VmafThreadLocaleState *const locale = vmaf_thread_locale_push_c();
    const int len = snprintf(text, sizeof(text), "%.17g", nvd);
    vmaf_thread_locale_pop(locale);
    if (len < 0 || (size_t)len >= sizeof(text)) {
        return -EINVAL;
    }
    return vmaf_dictionary_set(&existing->opts_dict, NVD_EXTRA, text, 0);
}

/* The merge rules that need no name rendering: 1 to go on, 0 to decline. */
static int mergeable(const VmafFeatureExtractor *e, const VmafFeatureExtractor *n)
{
    if (vmaf_adm_view_count(e) == 0u || vmaf_adm_view_count(n) != 1u ||
        !adm_option(e, NVD_EXTRA, VMAF_OPT_TYPE_DOUBLE)) {
        return 0;
    }
    if (read_flag(n, "debug") || read_flag(e, "adm_skip_aim") != read_flag(n, "adm_skip_aim")) {
        return 0;
    }
    return vmaf_adm_view_dist(n, 0u) != vmaf_adm_view_dist(e, 0u);
}

int vmaf_adm_merge_view_dist(VmafFeatureExtractorContext *existing,
                             VmafFeatureExtractorContext *incoming)
{
    if (!existing || !incoming || !mergeable(existing->fex, incoming->fex)) {
        return 0;
    }
    const int same = same_except_view(existing->fex, incoming->fex);
    if (same <= 0) {
        return same;
    }
    const double nvd = vmaf_adm_view_dist(incoming->fex, 0u);
    if (vmaf_adm_view_count(existing->fex) > 1u) {
        return nvd == vmaf_adm_view_dist(existing->fex, 1u);
    }
    const int err = record_extra_view(existing, nvd);
    if (err) {
        return err;
    }
    write_double(existing->fex->priv, adm_option(existing->fex, NVD_EXTRA, VMAF_OPT_TYPE_DOUBLE),
                 nvd);
    return 1;
}

/* -EINVAL when `view`'s first name equals the instance's own: the collector
 * would refuse the second distance's scores at the first frame. */
static int named_apart(const VmafFeatureExtractor *fex, const void *view)
{
    char *a = vmaf_feature_name_from_options(vmaf_adm_view_names[0], fex->options, fex->priv);
    char *b = vmaf_feature_name_from_options(vmaf_adm_view_names[0], fex->options, view);
    const int err = (a && b) ? (strcmp(a, b) == 0 ? -EINVAL : 0) : -ENOMEM;
    free(a);
    free(b);
    if (err == -EINVAL) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "%s: adm_norm_view_dist_extra (%.17g) gives the feature names of "
                 "adm_norm_view_dist (%.17g)\n",
                 fex->name, vmaf_adm_view_dist(fex, 1u), vmaf_adm_view_dist(fex, 0u));
    }
    return err;
}

/* Map each second-distance key to the name `view` renders. */
static int add_view_names(const VmafFeatureExtractor *fex, const void *view, VmafDictionary **dict)
{
    for (size_t i = 0u; i < VMAF_ADM_VIEW_SCORE_COUNT; ++i) {
        char *name = vmaf_feature_name_from_options(vmaf_adm_view_names[i], fex->options, view);
        if (!name) {
            return -ENOMEM;
        }
        const int err = vmaf_dictionary_set(dict, vmaf_adm_extra_view_keys[i], name, 0);
        free(name);
        if (err) {
            return err;
        }
    }
    return 0;
}

int vmaf_adm_extend_name_dict(const VmafFeatureExtractor *fex, VmafDictionary **dict)
{
    if (!fex || !fex->priv || !dict) {
        return -EINVAL;
    }
    if (vmaf_adm_view_count(fex) < 2u) {
        return 0;
    }
    void *view = priv_at_view(fex, vmaf_adm_view_dist(fex, 1u));
    if (!view) {
        return -ENOMEM;
    }
    int err = named_apart(fex, view);
    if (!err) {
        err = add_view_names(fex, view, dict);
    }
    free(view);
    return err;
}

/* NOLINTEND(modernize-use-nullptr) */
