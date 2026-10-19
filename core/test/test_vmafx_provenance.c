/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * The provenance record of a VMAFx context (#2142, ADR-2073, RC4 WP5): every
 * field filled for a CPU run, the model with its source, digest and overrides,
 * the producer of every feature (option-decorated names included), imported
 * scores marked imported, annotations and the encode record with their
 * refusals, and the digest: covering the record and the scores, not the
 * timing.
 *
 * Failing first: none of these functions exists on the WP8 base; a producer
 * left unrecorded fails test_decorated_name_has_its_producer; a digest that
 * covers elapsed_ns fails test_digest_covers_record_not_timing.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "mu_table.h"
#include "test.h"
#include "vmafx/vmafx.h"
#include "vmafx_test_util.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

enum { W = 176, H = 144, N_FRAMES = 3 };

/* The default model reads features that need larger frames than these. */
#define MODEL_VERSION "vmaf_v0.6.1" /* vmaf-model-pin: features fit 176x144 frames */

static const char *const encode_digest =
    "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

static bool submit_frames(VmafxContext *context, unsigned n)
{
    const VmafxFrameDesc desc = vt_desc(VMAFX_PIXEL_FORMAT_YUV420P, 8, W, H);
    uint8_t *data = malloc(vt_frame_bytes(&desc));
    bool ok = data != NULL;
    for (unsigned i = 0; i < n && ok; i++) {
        vt_fill(&desc, data, i);
        VmafxFrame *ref = vt_copy_frame(&desc, data);
        vt_fill(&desc, data, i + 50u);
        VmafxFrame *dist = vt_copy_frame(&desc, data);
        ok = vmafx_submit(context, ref, dist, i, NULL) == VMAFX_OK;
    }
    free(data);
    return ok;
}

/* A context scoring the default model, psnr with enable_mse and an imported
 * score over N_FRAMES; the model is loaded with one override. */
static VmafxContext *scored_context(bool flush)
{
    VmafxContext *context = NULL;
    VmafxModel *model = NULL;
    VmafxOptions *override = NULL;
    VmafxOptions *psnr = NULL;
    const bool ok =
        vmafx_context_create(NULL, &context, NULL) == VMAFX_OK &&
        vmafx_model_load(NULL, MODEL_VERSION, &model, NULL) == VMAFX_OK &&
        vmafx_options_set(&override, "adm_enhn_gain_limit", "1.0", NULL) == VMAFX_OK &&
        vmafx_model_override_feature(model, "adm", override, NULL) == VMAFX_OK &&
        vmafx_context_use_model(context, model, NULL) == VMAFX_OK &&
        vmafx_options_set(&psnr, "enable_mse", "true", NULL) == VMAFX_OK &&
        vmafx_context_use_feature(context, "psnr", psnr, NULL) == VMAFX_OK &&
        submit_frames(context, N_FRAMES) &&
        vmafx_context_import_score(context, "outside_score", 0, 0.25, NULL) == VMAFX_OK &&
        (!flush || vmafx_flush(context, NULL) == VMAFX_OK);
    vmafx_options_free(override);
    vmafx_options_free(psnr);
    vmafx_model_unref(model);
    if (!ok && context) {
        (void)vmafx_context_destroy(context, NULL);
        return NULL;
    }
    return context;
}

static bool filled(const char *text)
{
    return text && text[0] != '\0';
}

static bool digest_form(const char *text)
{
    if (!text || strncmp(text, "sha256:", 7) != 0 || strlen(text) != 71u) {
        return false;
    }
    for (size_t i = 7; i < 71u; i++) {
        const char c = text[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

static char *check_strings(const VmafxProvenance *rec)
{
    const char *const strings[] = {
        rec->version,     rec->commit,         rec->build_id,      rec->compiler,
        rec->build_flags, rec->fp_policy,      rec->backends,      rec->simd,
        rec->device_name, rec->device_runtime, rec->encode_record, rec->scores_digest,
        rec->digest,
    };
    for (size_t i = 0; i < sizeof(strings) / sizeof(strings[0]); i++) {
        mu_assert("a string field of the record is empty", filled(strings[i]));
    }
    mu_assert("build_id form", digest_form(rec->build_id));
    mu_assert("digest form", digest_form(rec->digest));
    mu_assert("scores digest form", digest_form(rec->scores_digest));
    return NULL;
}

typedef char *(*ContextCheck)(VmafxContext *context);

/* Run `check` on a scored context (flushed or not) and release it on every
 * path. */
static char *with_scored(bool flush, ContextCheck check)
{
    VmafxContext *const context = scored_context(flush);
    mu_assert("session", context != NULL);
    char *const failed = check(context);
    (void)vmafx_context_destroy(context, NULL);
    return failed;
}

/* Run `check` on a fresh context without frames. */
static char *with_fresh(ContextCheck check)
{
    VmafxContext *context = NULL;
    mu_assert("context", vmafx_context_create(NULL, &context, NULL) == VMAFX_OK);
    char *const failed = check(context);
    (void)vmafx_context_destroy(context, NULL);
    return failed;
}

/* `status` and an error naming `subject` of `kind`. */
static bool refused_as(VmafxStatus status, VmafxStatus expected, VmafxError **error,
                       const char *subject, uint32_t kind)
{
    return status == expected && vt_failed(error, expected, subject, kind);
}

static bool same(const char *a, const char *b)
{
    return a && b && strcmp(a, b) == 0;
}

/* ---- The record of a CPU run ----------------------------------------------- */

static bool abi_matches(const VmafxProvenance *rec)
{
    return rec->abi_major == VMAFX_ABI_VERSION_MAJOR && rec->abi_minor == VMAFX_ABI_VERSION_MINOR &&
           rec->abi_patch == VMAFX_ABI_VERSION_PATCH;
}

static bool cpu_device(const VmafxProvenance *rec)
{
    return rec->active_backend == VMAFX_BACKEND_CPU && rec->device_index == 0 &&
           same(rec->device_name, "cpu");
}

static bool frames_match(const VmafxProvenance *rec)
{
    return rec->frame_width == W && rec->frame_height == H && rec->bpc == 8u &&
           rec->pix_fmt == VMAFX_PIXEL_FORMAT_YUV420P && rec->n_frames == N_FRAMES;
}

static bool counts_match(const VmafxProvenance *rec)
{
    return rec->n_subsample == 1u && rec->n_threads == 0u && rec->n_models == 1u &&
           rec->n_features > 5u && rec->n_extractors > 0u;
}

static char *record_complete(VmafxContext *context)
{
    mu_assert("encode record",
              vmafx_context_set_encode_record(context, encode_digest, NULL) == VMAFX_OK);
    VmafxProvenance rec = VMAFX_PROVENANCE_INIT;
    mu_assert("record", vmafx_context_provenance(context, &rec, NULL) == VMAFX_OK);
    mu_assert("ABI version", abi_matches(&rec));
    mu_assert("device", cpu_device(&rec));
    mu_assert("frames", frames_match(&rec));
    mu_assert("options and counts", counts_match(&rec));
    mu_assert("encode record kept", same(rec.encode_record, encode_digest));
    return check_strings(&rec);
}

static char *test_record_complete_cpu(void)
{
    return with_scored(true, record_complete);
}

/* ---- Models ------------------------------------------------------------------- */

static char *model_record(VmafxContext *context)
{
    VmafxModelProvenance model = VMAFX_MODEL_PROVENANCE_INIT;
    mu_assert("model", vmafx_context_model_provenance(context, 0, &model, NULL) == VMAFX_OK);
    mu_assert("model name", same(model.name, "vmaf"));
    mu_assert("model version", same(model.version, MODEL_VERSION));
    mu_assert("model digest", strlen(model.sha256) == 64u);
    mu_assert("model override", same(model.overrides, "adm.adm_enhn_gain_limit=1"));
    VmafxError *error = NULL;
    mu_assert("past the last model",
              refused_as(vmafx_context_model_provenance(context, 1, &model, &error),
                         VMAFX_E_NOTFOUND, &error, "index", VMAFX_SUBJECT_PARAMETER));
    return NULL;
}

static char *test_model_record(void)
{
    return with_scored(true, model_record);
}

/* ---- Producers ---------------------------------------------------------------- */

static bool cpu_extractor_record(const VmafxFeatureProvenance *feature)
{
    return feature->source == VMAFX_FEATURE_SOURCE_EXTRACTOR &&
           same(feature->implementation, "c") && same(feature->exactness, "cpu-reference") &&
           same(feature->device, "cpu") && filled(feature->runtime);
}

static bool score_names(VmafxContext *context, const char *feature, const char *extractor)
{
    VmafxScore score = VMAFX_SCORE_INIT;
    return vmafx_feature_score(context, feature, 1, &score, NULL) == VMAFX_OK &&
           same(score.extractor, extractor);
}

static char *decorated_name(VmafxContext *context)
{
    /* psnr does not declare mse_y; the collector recorded who wrote it. */
    VmafxFeatureProvenance feature = VMAFX_FEATURE_PROVENANCE_INIT;
    mu_assert("mse_y", vmafx_feature_provenance(context, "mse_y", &feature, NULL) == VMAFX_OK);
    mu_assert("producer", same(feature.extractor, "psnr"));
    mu_assert("options", same(feature.options, "enable_mse=true"));
    mu_assert("extractor record", cpu_extractor_record(&feature));
    mu_assert("score names its extractor", score_names(context, "mse_y", "psnr"));
    return NULL;
}

static char *test_decorated_name_has_its_producer(void)
{
    return with_scored(true, decorated_name);
}

static bool imported_score_has_no_extractor(VmafxContext *context)
{
    VmafxScore score = VMAFX_SCORE_INIT;
    return vmafx_feature_score(context, "outside_score", 0, &score, NULL) == VMAFX_OK &&
           score.extractor == NULL;
}

static char *imported_scores(VmafxContext *context)
{
    VmafxFeatureProvenance feature = VMAFX_FEATURE_PROVENANCE_INIT;
    mu_assert("imported",
              vmafx_feature_provenance(context, "outside_score", &feature, NULL) == VMAFX_OK);
    mu_assert("imported source", feature.source == VMAFX_FEATURE_SOURCE_IMPORTED);
    mu_assert("imported has no extractor", feature.extractor[0] == '\0');
    mu_assert("imported has no class", feature.exactness[0] == '\0');
    mu_assert("imported score", imported_score_has_no_extractor(context));
    VmafxError *error = NULL;
    mu_assert("unknown feature",
              refused_as(vmafx_feature_provenance(context, "no_such", &feature, &error),
                         VMAFX_E_NOTFOUND, &error, "no_such", VMAFX_SUBJECT_FEATURE));
    return NULL;
}

static char *test_imported_and_model_scores(void)
{
    return with_scored(true, imported_scores);
}

/* Feature `i` follows `previous` in byte order and has a source. */
static char *feature_follows(VmafxContext *context, uint32_t i, char previous[256])
{
    VmafxFeatureProvenance feature = VMAFX_FEATURE_PROVENANCE_INIT;
    mu_assert("feature", vmafx_context_feature_provenance(context, i, &feature, NULL) == VMAFX_OK);
    mu_assert("byte order", strcmp(previous, feature.feature) < 0);
    mu_assert("every score has a source", feature.source != VMAFX_FEATURE_SOURCE_UNKNOWN);
    (void)snprintf(previous, 256u, "%s", feature.feature);
    return NULL;
}

static char *name_order(VmafxContext *context)
{
    VmafxProvenance rec = VMAFX_PROVENANCE_INIT;
    mu_assert("record", vmafx_context_provenance(context, &rec, NULL) == VMAFX_OK);
    char previous[256] = "";
    for (uint32_t i = 0; i < rec.n_features; i++) {
        char *const failed = feature_follows(context, i, previous);
        mu_assert(failed, failed == NULL);
    }
    VmafxFeatureProvenance feature = VMAFX_FEATURE_PROVENANCE_INIT;
    VmafxError *error = NULL;
    mu_assert(
        "past the last feature",
        refused_as(vmafx_context_feature_provenance(context, rec.n_features, &feature, &error),
                   VMAFX_E_NOTFOUND, &error, "index", VMAFX_SUBJECT_PARAMETER));
    return NULL;
}

static char *test_features_in_name_order(void)
{
    return with_scored(true, name_order);
}

/* ---- Annotations and the encode record ---------------------------------------- */

static VmafxStatus annotate_long_value(VmafxContext *context, VmafxError **error)
{
    char *const long_value = calloc(4098, 1);
    if (!long_value) {
        return VMAFX_E_NOMEM;
    }
    memset(long_value, 'v', 4097);
    const VmafxStatus status = vmafx_context_annotate(context, "k", long_value, error);
    free(long_value);
    return status;
}

static bool fill_annotations(VmafxContext *context, unsigned from, unsigned to)
{
    bool ok = true;
    for (unsigned i = from; i < to && ok; i++) {
        ok = vmafx_context_annotate(context, "k", "v", NULL) == VMAFX_OK;
    }
    return ok;
}

static bool annotation_is(VmafxContext *context, uint32_t index, const char *key, const char *value)
{
    VmafxAnnotation annotation = VMAFX_ANNOTATION_INIT;
    return vmafx_context_annotation(context, index, &annotation, NULL) == VMAFX_OK &&
           same(annotation.key, key) && same(annotation.value, value);
}

static char *annotation_refusals(VmafxContext *context)
{
    VmafxError *error = NULL;
    mu_assert("bad key", refused_as(vmafx_context_annotate(context, "Input", "x", &error),
                                    VMAFX_E_INVALID, &error, "key", VMAFX_SUBJECT_PARAMETER));
    mu_assert("long value", refused_as(annotate_long_value(context, &error), VMAFX_E_RANGE, &error,
                                       "value", VMAFX_SUBJECT_PARAMETER));
    mu_assert("fill", fill_annotations(context, 0, 256u));
    mu_assert("257th", refused_as(vmafx_context_annotate(context, "k", "v", &error), VMAFX_E_RANGE,
                                  &error, "key", VMAFX_SUBJECT_PARAMETER));
    VmafxAnnotation annotation = VMAFX_ANNOTATION_INIT;
    mu_assert("past the last",
              vmafx_context_annotation(context, 256, &annotation, NULL) == VMAFX_E_NOTFOUND);
    return NULL;
}

static char *annotations(VmafxContext *context)
{
    mu_assert("first", vmafx_context_annotate(context, "input_ref", "a.yuv", NULL) == VMAFX_OK);
    mu_assert("repeated key", vmafx_context_annotate(context, "input_ref", "b", NULL) == VMAFX_OK);
    mu_assert("read back", annotation_is(context, 1, "input_ref", "b"));
    return NULL;
}

static char *test_annotations(void)
{
    char *const failed = with_fresh(annotations);
    return failed ? failed : with_fresh(annotation_refusals);
}

static bool encode_record_is(VmafxContext *context, const char *expected)
{
    VmafxProvenance rec = VMAFX_PROVENANCE_INIT;
    return vmafx_context_provenance(context, &rec, NULL) == VMAFX_OK &&
           same(rec.encode_record, expected);
}

static char *encode_record(VmafxContext *context)
{
    static const char *const refused[] = {
        "sha256:0123",
        "md5:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
        "sha256:0123456789ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef",
        "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0",
    };
    for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        VmafxError *error = NULL;
        mu_assert("bad encode record",
                  refused_as(vmafx_context_set_encode_record(context, refused[i], &error),
                             VMAFX_E_INVALID, &error, "record", VMAFX_SUBJECT_PARAMETER));
    }
    mu_assert("set", vmafx_context_set_encode_record(context, encode_digest, NULL) == VMAFX_OK);
    mu_assert("recorded", encode_record_is(context, encode_digest));
    mu_assert("clear", vmafx_context_set_encode_record(context, NULL, NULL) == VMAFX_OK);
    mu_assert("cleared", encode_record_is(context, ""));
    return NULL;
}

static char *test_encode_record(void)
{
    return with_fresh(encode_record);
}

/* ---- Digests ------------------------------------------------------------------ */

/* The record's digest and scores digest into `digest` / `scores`. */
static bool digests(VmafxContext *context, char digest[80], char scores[80], uint64_t *elapsed)
{
    VmafxProvenance rec = VMAFX_PROVENANCE_INIT;
    if (vmafx_context_provenance(context, &rec, NULL) != VMAFX_OK) {
        return false;
    }
    (void)snprintf(digest, 80u, "%s", rec.digest);
    (void)snprintf(scores, 80u, "%s", rec.scores_digest);
    *elapsed = rec.elapsed_ns;
    return true;
}

/* Query until elapsed_ns moves (unflushed: it grows with every query). */
static bool timing_moves(VmafxContext *context, char digest[80])
{
    char scores[80];
    uint64_t first = 0;
    uint64_t now = 0;
    bool ok = digests(context, digest, scores, &first);
    for (unsigned spin = 0; ok && now <= first && spin < 1000000u; spin++) {
        ok = digests(context, digest, scores, &now);
    }
    return ok && now > first;
}

static char *digest_ignores_timing(VmafxContext *context)
{
    char first[80];
    char later[80];
    char scores[80];
    uint64_t elapsed = 0;
    mu_assert("record", digests(context, first, scores, &elapsed));
    mu_assert("timing moved", timing_moves(context, later));
    mu_assert("digest covers the timing", strcmp(first, later) == 0);
    return NULL;
}

static char *digest_covers_record(VmafxContext *context)
{
    char first[80];
    char later[80];
    char scores[80];
    char scores_later[80];
    uint64_t elapsed = 0;
    mu_assert("record", digests(context, first, scores, &elapsed));
    mu_assert("annotate", vmafx_context_annotate(context, "note", "x", NULL) == VMAFX_OK);
    mu_assert("record", digests(context, later, scores, &elapsed));
    mu_assert("digest misses an annotation", strcmp(first, later) != 0);
    mu_assert("import",
              vmafx_context_import_score(context, "outside_score", 1, 0.5, NULL) == VMAFX_OK);
    mu_assert("record", digests(context, first, scores_later, &elapsed));
    mu_assert("scores digest misses a score", strcmp(scores, scores_later) != 0);
    return NULL;
}

static char *test_digest_covers_record_not_timing(void)
{
    char *const failed = with_scored(false, digest_ignores_timing);
    return failed ? failed : with_scored(false, digest_covers_record);
}

static bool carries(const char *json, const char *a, const char *b)
{
    return json && strstr(json, a) != NULL && strstr(json, b) != NULL;
}

static char *canonical_json(VmafxContext *context)
{
    const char *json = NULL;
    mu_assert("full", vmafx_context_provenance_json(context, 0, &json, NULL) == VMAFX_OK);
    mu_assert("full carries digest and timing",
              carries(json, "\"digest\":\"sha256:", "\"elapsed_ns\":\""));
    mu_assert("no whitespace after a key", strstr(json, "\": ") == NULL);
    mu_assert("canonical", vmafx_context_provenance_json(context, VMAFX_PROVENANCE_JSON_CANONICAL,
                                                         &json, NULL) == VMAFX_OK);
    mu_assert("canonical leaves out digest and timing",
              !carries(json, "\"digest\":", "\"digest\":") &&
                  !carries(json, "\"elapsed_ns\":", "\"elapsed_ns\":"));
    mu_assert("keys in byte order", strncmp(json, "{\"abi_major\":", 13) == 0);
    VmafxError *error = NULL;
    mu_assert("unknown flag",
              refused_as(vmafx_context_provenance_json(context, 2u, &json, &error), VMAFX_E_INVALID,
                         &error, "flags", VMAFX_SUBJECT_PARAMETER));
    return NULL;
}

static char *test_canonical_json(void)
{
    return with_scored(true, canonical_json);
}

static char *test_null_arguments(void)
{
    VmafxError *error = NULL;
    VmafxProvenance rec = VMAFX_PROVENANCE_INIT;
    mu_assert("NULL context",
              refused_as(vmafx_context_provenance(NULL, &rec, &error), VMAFX_E_INVALID, &error,
                         "context", VMAFX_SUBJECT_PARAMETER));
    mu_assert("NULL annotation context",
              vmafx_context_annotate(NULL, "k", "v", NULL) == VMAFX_E_INVALID);
    mu_assert("NULL json", vmafx_context_provenance_json(NULL, 0, NULL, NULL) == VMAFX_E_INVALID);
    return NULL;
}

char *run_tests(void)
{
    static const MuTest tests[] = {
        MU_TEST(test_record_complete_cpu),
        MU_TEST(test_model_record),
        MU_TEST(test_decorated_name_has_its_producer),
        MU_TEST(test_imported_and_model_scores),
        MU_TEST(test_features_in_name_order),
        MU_TEST(test_annotations),
        MU_TEST(test_encode_record),
        MU_TEST(test_digest_covers_record_not_timing),
        MU_TEST(test_canonical_json),
        MU_TEST(test_null_arguments),
    };
    return mu_run_table(tests, MU_TABLE_LEN(tests));
}

/* NOLINTEND(modernize-use-nullptr) */
