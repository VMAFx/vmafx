/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Integer ADM folds a second viewing distance into one context
 * (Netflix/vmaf 33e5f0aca + cffd5b77d, ADR-2795).
 *
 * Two `adm` contexts that differ only in `adm_norm_view_dist` become one:
 * the registry offers the second to the first through the descriptor's
 * merge() callback, which records the distance as `adm_norm_view_dist_extra`
 * in the first's options. extend_name_dict() then maps the second distance's
 * keys to the names its own context would have used. These tests pin the
 * rules of adm_merge_view_dist(), the registry's merge pass in
 * fex_ctx_vector.cpp and the dictionary. test_integer_adm_view_dist.c checks
 * the scores.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "dict.h"
#include "feature/adm_view_dist.h"
#include "feature/feature_extractor.h"
#include "fex_ctx_vector.h"
#include "test.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe. ADR-1138. */

/* An `adm` context at viewing distance `nvd`, with one more option when `key`
 * is not NULL. The context owns the dictionary. */
static int adm_context(const VmafFeatureExtractor *fex, VmafFeatureExtractorContext **ctx,
                       const char *nvd, const char *key, const char *val)
{
    VmafDictionary *opts = NULL;
    int err = vmaf_dictionary_set(&opts, "adm_norm_view_dist", nvd, 0);
    if (!err && key) {
        err = vmaf_dictionary_set(&opts, key, val, 0);
    }
    if (!err) {
        err = vmaf_feature_extractor_context_create(ctx, fex, opts);
    }
    if (err && opts) {
        (void)vmaf_dictionary_free(&opts);
    }
    return err;
}

static const VmafFeatureExtractor *adm_fex(void)
{
    return vmaf_get_feature_extractor_by_name("adm");
}

static double extra_view(const VmafFeatureExtractorContext *ctx)
{
    const VmafDictionaryEntry *e =
        vmaf_dictionary_get((VmafDictionary **)&ctx->opts_dict, "adm_norm_view_dist_extra", 0);
    return e ? strtod(e->val, NULL) : 0.0;
}

/* The hooks integer ADM registers. */
static char *test_adm_has_the_hooks(void)
{
    const VmafFeatureExtractor *fex = adm_fex();
    mu_assert("no adm extractor", fex != NULL);
    mu_assert("adm has a merge hook", fex->merge != NULL);
    mu_assert("adm has a name-dictionary hook", fex->extend_name_dict != NULL);
    return NULL;
}

/* Contexts at nvd 3, 5 and 5 for the merge tests. */
static int three_contexts(VmafFeatureExtractorContext *ctx[3], const char *const nvd[3])
{
    int err = 0;
    for (unsigned i = 0; i < 3u && !err; i++) {
        err = adm_context(adm_fex(), &ctx[i], nvd[i], NULL, NULL);
    }
    return err;
}

static void destroy_contexts(VmafFeatureExtractorContext *ctx[3])
{
    for (unsigned i = 0; i < 3u; i++) {
        (void)vmaf_feature_extractor_context_destroy(ctx[i]);
    }
}

/* positive: a context at another distance merges; the distance lands in the
 * options the worker pool builds instances from; the same distance again is
 * absorbed without a change. */
static char *test_merge_takes_a_second_distance(void)
{
    static const char *const NVD[3] = {"3", "5", "5"};
    VmafFeatureExtractorContext *ctx[3] = {NULL, NULL, NULL};
    mu_assert("contexts nvd 3, 5, 5", !three_contexts(ctx, NVD));
    const int first = ctx[0]->fex->merge(ctx[0], ctx[1]);
    const double extra_after_first = extra_view(ctx[0]);
    const int again = ctx[0]->fex->merge(ctx[0], ctx[2]);
    const double extra_after_again = extra_view(ctx[0]);
    destroy_contexts(ctx);
    mu_assert("nvd 5 merges into nvd 3", first == 1);
    mu_assert("the options record the second distance", extra_after_first == 5.0);
    mu_assert("nvd 5 again is absorbed", again == 1);
    mu_assert("the second distance is unchanged", extra_after_again == 5.0);
    return NULL;
}

/* One declined merge: `existing` at nvd 3 against `incoming` at `nvd` with
 * one more option. Returns NULL when the merge returned 0 and left no second
 * distance behind. */
static char *declines(const char *nvd, const char *key, const char *val)
{
    const VmafFeatureExtractor *fex = adm_fex();
    VmafFeatureExtractorContext *e = NULL;
    VmafFeatureExtractorContext *n = NULL;
    mu_assert("existing context", !adm_context(fex, &e, "3", NULL, NULL));
    mu_assert("incoming context", !adm_context(fex, &n, nvd, key, val));
    const int merged = e->fex->merge(e, n);
    const double extra = extra_view(e);
    (void)vmaf_feature_extractor_context_destroy(e);
    (void)vmaf_feature_extractor_context_destroy(n);
    mu_assert("the merge declined", merged == 0);
    mu_assert("no second distance recorded", extra == 0.0);
    return NULL;
}

/* negative: every difference other than the distance, and the cases whose
 * scores a merge would lose, keep two contexts. */
static char *test_merge_declines_other_differences(void)
{
    char *msg = declines("5", "adm_csf_mode", "1");
    if (msg)
        return msg;
    msg = declines("5", "adm_dlm_weight", "0.7");
    if (msg)
        return msg;
    msg = declines("3", NULL, NULL); /* the same distance: dedup's case */
    if (msg)
        return msg;
    msg = declines("5", "debug", "true"); /* its debug scores are unsuffixed */
    if (msg)
        return msg;
    msg = declines("5", "adm_skip_aim", "true"); /* not a feature parameter */
    if (msg)
        return msg;
    return declines("5", "adm_norm_view_dist_extra", "7"); /* a second of its own */
}

/* boundary: a context holds at most two distances; a third stays apart. */
static char *test_merge_holds_two_distances(void)
{
    static const char *const NVD[3] = {"3", "5", "7"};
    VmafFeatureExtractorContext *ctx[3] = {NULL, NULL, NULL};
    mu_assert("contexts nvd 3, 5, 7", !three_contexts(ctx, NVD));
    const int second = ctx[0]->fex->merge(ctx[0], ctx[1]);
    const int third = ctx[0]->fex->merge(ctx[0], ctx[2]);
    const double extra = extra_view(ctx[0]);
    destroy_contexts(ctx);
    mu_assert("nvd 5 merges", second == 1);
    mu_assert("nvd 7 does not", third == 0);
    mu_assert("the second distance is still 5", extra == 5.0);
    return NULL;
}

static int append_adm(RegisteredFeatureExtractors *rfe, const VmafFeatureExtractor *fex,
                      const char *nvd)
{
    VmafFeatureExtractorContext *ctx = NULL;
    const int err = adm_context(fex, &ctx, nvd, NULL, NULL);
    return err ? err : feature_extractor_vector_append(rfe, ctx, 0);
}

/* The registry: 3, 5 and 5 make one context; 7 a second. */
static char *test_registry_merges_before_init(void)
{
    const VmafFeatureExtractor *fex = adm_fex();
    RegisteredFeatureExtractors rfe;
    mu_assert("vector init", !feature_extractor_vector_init(&rfe));
    const int err =
        append_adm(&rfe, fex, "3") || append_adm(&rfe, fex, "5") || append_adm(&rfe, fex, "5");
    const unsigned merged_cnt = rfe.cnt;
    const double extra = merged_cnt ? extra_view(rfe.fex_ctx[0]) : 0.0;
    const int err7 = append_adm(&rfe, fex, "7");
    const unsigned cnt = rfe.cnt;
    (void)feature_extractor_vector_destroy(&rfe);
    mu_assert("appends of nvd 3, 5, 5", !err && !err7);
    mu_assert("one context for 3 and 5", merged_cnt == 1u);
    mu_assert("it evaluates 5 as its second distance", extra == 5.0);
    mu_assert("7 is a context of its own", cnt == 2u);
    return NULL;
}

/* Once a context is initialized, nothing merges into it. */
static char *test_registry_skips_initialized_contexts(void)
{
    const VmafFeatureExtractor *fex = adm_fex();
    RegisteredFeatureExtractors rfe;
    mu_assert("vector init", !feature_extractor_vector_init(&rfe));
    int err = append_adm(&rfe, fex, "3");
    if (!err) {
        rfe.fex_ctx[0]->is_initialized = true;
        err = append_adm(&rfe, fex, "5");
        rfe.fex_ctx[0]->is_initialized = false;
    }
    const unsigned cnt = rfe.cnt;
    const double extra = cnt ? extra_view(rfe.fex_ctx[0]) : -1.0;
    (void)feature_extractor_vector_destroy(&rfe);
    mu_assert("appends of nvd 3 and 5", !err);
    mu_assert("nothing merges into an initialized context", cnt == 2u && extra == 0.0);
    return NULL;
}

/* A merge callback that takes everything it is offered. */
static int merge_everything(VmafFeatureExtractorContext *existing,
                            VmafFeatureExtractorContext *incoming)
{
    (void)existing;
    (void)incoming;
    return 1;
}

/* The registry offers a context only to contexts of the same extractor name,
 * whatever the callback would accept: two descriptors sharing one callback
 * (a C extractor and its Rust twin) stay apart. */
static char *test_registry_offers_same_name_only(void)
{
    VmafFeatureExtractor a = {.name = "mock_merge_a", .merge = merge_everything};
    VmafFeatureExtractor b = {.name = "mock_merge_b", .merge = merge_everything};
    RegisteredFeatureExtractors rfe;
    mu_assert("vector init", !feature_extractor_vector_init(&rfe));
    VmafFeatureExtractorContext *ctx = NULL;
    mu_assert("context a", !vmaf_feature_extractor_context_create(&ctx, &a, NULL));
    mu_assert("append a", !feature_extractor_vector_append(&rfe, ctx, 0));
    mu_assert("context b", !vmaf_feature_extractor_context_create(&ctx, &b, NULL));
    mu_assert("append b", !feature_extractor_vector_append(&rfe, ctx, 0));
    mu_assert("b is not offered to a", rfe.cnt == 2u);
    (void)feature_extractor_vector_destroy(&rfe);
    return NULL;
}

/* The registry keeps a C extractor and a copy of its descriptor under another
 * name apart, as it keeps `adm` and its Rust twin `adm_rust`, which share the
 * merge callback. */
static char *test_registry_keeps_twins_apart(void)
{
    const VmafFeatureExtractor *fex = adm_fex();
    VmafFeatureExtractor twin = *fex;
    twin.name = "adm_twin_under_test";
    RegisteredFeatureExtractors rfe;
    mu_assert("vector init", !feature_extractor_vector_init(&rfe));
    mu_assert("append adm nvd 3", !append_adm(&rfe, fex, "3"));
    mu_assert("append twin nvd 5", !append_adm(&rfe, &twin, "5"));
    mu_assert("two contexts", rfe.cnt == 2u);
    mu_assert("no second distance on adm", extra_view(rfe.fex_ctx[0]) == 0.0);
    (void)feature_extractor_vector_destroy(&rfe);
    return NULL;
}

/* Every ADM descriptor this build registers (the device twins only with
 * their backend) merges through the shared helpers on its own option
 * layout: the second distance lands in its options and its view count. */
typedef struct {
    const char *name;
    bool merges; /* ADR-2795 stack: a twin's pull request sets it */
} AdmDescriptor;

static char *test_every_adm_descriptor_merges(void)
{
    static const AdmDescriptor DESCRIPTORS[] = {
        {"adm", true},      {"adm_cuda", true},           {"adm_sycl", false},
        {"adm_hip", false}, {"integer_adm_metal", false},
    };
    for (size_t i = 0; i < sizeof(DESCRIPTORS) / sizeof(DESCRIPTORS[0]); i++) {
        const VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name(DESCRIPTORS[i].name);
        if (!fex)
            continue; /* not built into this configuration */
        mu_assert("a registered ADM descriptor merges exactly when its row says so",
                  (fex->merge != NULL) == DESCRIPTORS[i].merges);
        if (!fex->merge)
            continue;
        mu_assert("descriptor merges through the shared helper",
                  fex->merge == vmaf_adm_merge_view_dist &&
                      fex->extend_name_dict == vmaf_adm_extend_name_dict);
        VmafFeatureExtractorContext *a = NULL;
        VmafFeatureExtractorContext *b = NULL;
        mu_assert("contexts nvd 3 and 5",
                  !adm_context(fex, &a, "3", NULL, NULL) && !adm_context(fex, &b, "5", NULL, NULL));
        const int merged = a->fex->merge(a, b);
        const unsigned views = vmaf_adm_view_count(a->fex);
        const double extra = vmaf_adm_view_dist(a->fex, 1u);
        (void)vmaf_feature_extractor_context_destroy(a);
        (void)vmaf_feature_extractor_context_destroy(b);
        mu_assert("nvd 5 merges", merged == 1 && views == 2u && extra == 5.0);
    }
    return NULL;
}

/* extend_name_dict(): the second distance's keys name that distance's
 * features. */
static char *test_name_dictionary(void)
{
    const VmafFeatureExtractor *fex = adm_fex();
    VmafFeatureExtractorContext *ctx = NULL;
    mu_assert("context nvd 3 + 5", !adm_context(fex, &ctx, "3", "adm_norm_view_dist_extra", "5"));
    VmafDictionary *dict = NULL;
    const int err = ctx->fex->extend_name_dict(ctx->fex, &dict);
    const VmafDictionaryEntry *adm2 =
        vmaf_dictionary_get(&dict, "VMAF_integer_feature_adm2_score:nvde", 0);
    const VmafDictionaryEntry *s3 = vmaf_dictionary_get(&dict, "integer_adm_scale3:nvde", 0);
    const int adm2_ok = adm2 && !strcmp(adm2->val, "integer_adm2_nvd_5");
    const int s3_ok = s3 && !strcmp(s3->val, "integer_adm_scale3_nvd_5");
    const unsigned cnt = dict ? dict->cnt : 0u;
    (void)vmaf_dictionary_free(&dict);
    (void)vmaf_feature_extractor_context_destroy(ctx);
    mu_assert("extend", !err);
    mu_assert("adm2 of the second distance", adm2_ok);
    mu_assert("scale 3 of the second distance", s3_ok);
    mu_assert("seven keys", cnt == 7u);
    return NULL;
}

/* Without a second distance nothing is added; bad arguments are refused. */
static char *test_name_dictionary_without_second_distance(void)
{
    VmafFeatureExtractorContext *ctx = NULL;
    mu_assert("context nvd 3", !adm_context(adm_fex(), &ctx, "3", NULL, NULL));
    VmafDictionary *dict = NULL;
    const int err = ctx->fex->extend_name_dict(ctx->fex, &dict);
    const int null_err = ctx->fex->extend_name_dict(ctx->fex, NULL);
    (void)vmaf_feature_extractor_context_destroy(ctx);
    mu_assert("extend without a second distance", !err);
    mu_assert("nothing added", dict == NULL);
    mu_assert("NULL dictionary refused", null_err == -EINVAL);
    return NULL;
}

static char *run_merge_tests(void)
{
    mu_run_test(test_adm_has_the_hooks);
    mu_run_test(test_merge_takes_a_second_distance);
    mu_run_test(test_merge_declines_other_differences);
    mu_run_test(test_merge_holds_two_distances);
    return NULL;
}

static char *run_registry_tests(void)
{
    mu_run_test(test_registry_merges_before_init);
    mu_run_test(test_registry_skips_initialized_contexts);
    mu_run_test(test_registry_offers_same_name_only);
    mu_run_test(test_registry_keeps_twins_apart);
    mu_run_test(test_name_dictionary);
    mu_run_test(test_name_dictionary_without_second_distance);
    mu_run_test(test_every_adm_descriptor_merges);
    return NULL;
}

char *run_tests(void)
{
    char *result = run_merge_tests();
    return result ? result : run_registry_tests();
}

/* NOLINTEND(modernize-use-nullptr) */
