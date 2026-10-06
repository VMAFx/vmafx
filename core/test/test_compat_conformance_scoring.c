/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Conformance scenarios of the libvmaf compat library that score frames (RC4
 * WP6): models, a model collection, an option-carrying feature and an
 * imported score on three synthetic frames, every per-frame and pooled score
 * as %a, the extractor and backend queries, the four report formats, and the
 * preallocated-picture path. Synthetic frames keep the test free of
 * fixtures: the comparison is the old libvmaf against the compat library on
 * the same input, not a score against a reference value.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compat_conformance_trace.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

#define FRAME_W 176u
#define FRAME_H 144u
#define N_FRAMES 3u
#define EXTRACTORS_MAX 64u /* HISS-02 bound of the registered-extractor walk */
#define REPORT_LINE 8192
#define REPORT_LINES_MAX 4096u
#define REPORT_PATH "compat_conformance_report.out"

static VmafConfiguration quiet_config(void)
{
    VmafConfiguration cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.log_level = VMAF_LOG_LEVEL_NONE;
    return cfg;
}

/* A deterministic textured frame; the distorted one adds bounded noise. */
static void fill(VmafPicture *pic, unsigned frame, int distorted)
{
    uint32_t state = 0x9e3779b9u * (frame + 1u);
    for (unsigned p = 0; p < 3u; p++) {
        uint8_t *const data = pic->data[p];
        for (unsigned y = 0; y < pic->h[p]; y++) {
            for (unsigned x = 0; x < pic->w[p]; x++) {
                state = state * 1664525u + 1013904223u;
                const unsigned base = (x * 3u + y * 5u + frame * 7u + p * 11u) & 0xffu;
                const unsigned noise = distorted ? (state >> 28) : 0u;
                data[y * (size_t)pic->stride[p] + x] = (uint8_t)((base + noise) & 0xffu);
            }
        }
    }
}

static void submit_frames(const VmafCompatApi *api, Trace *t, VmafContext *vmaf)
{
    for (unsigned i = 0; i < N_FRAMES; i++) {
        VmafPicture ref;
        VmafPicture dist;
        const int a = api->picture_alloc(&ref, VMAF_PIX_FMT_YUV420P, 8, FRAME_W, FRAME_H);
        const int b = api->picture_alloc(&dist, VMAF_PIX_FMT_YUV420P, 8, FRAME_W, FRAME_H);
        fill(&ref, i, 0);
        fill(&dist, i, 1);
        const int err = api->read_pictures(vmaf, &ref, &dist, i);
        trace(t, "frame %u alloc %d %d read %d consumed %d %d", i, a, b, err, ref.ref == NULL,
              dist.ref == NULL);
    }
    VmafPicture lone;
    (void)api->picture_alloc(&lone, VMAF_PIX_FMT_YUV420P, 8, FRAME_W, FRAME_H);
    trace(t, "read one-sided %d", api->read_pictures(vmaf, &lone, NULL, N_FRAMES));
    trace(t, "read NULL context %d", api->read_pictures(NULL, NULL, NULL, 0));
    (void)api->picture_unref(&lone);
    trace(t, "flush %d", api->read_pictures(vmaf, NULL, NULL, 0));
    trace(t, "flush again %d", api->read_pictures(vmaf, NULL, NULL, 0));
}

static void register_features(const VmafCompatApi *api, Trace *t, VmafContext *vmaf)
{
    VmafFeatureDictionary *opts = NULL;
    (void)api->feature_dictionary_set(&opts, "enable_chroma", "1");
    trace(t, "use psnr %d", api->use_feature(vmaf, "psnr", opts));
    VmafFeatureDictionary *kept = NULL;
    (void)api->feature_dictionary_set(&kept, "x", "1");
    trace(t, "use unknown %d", api->use_feature(vmaf, "no_such_extractor", kept));
    trace(t, "unknown kept the options: free %d", api->feature_dictionary_free(&kept));
    trace(t, "use NULL %d", api->use_feature(NULL, "psnr", NULL));
    for (unsigned i = 0; i < N_FRAMES; i++) {
        trace(t, "import %u %d", i, api->import_feature_score(vmaf, "imported", 1.25 + i, i));
    }
    trace(t, "import NULL %d", api->import_feature_score(vmaf, NULL, 1.0, 0));
}

static void query_context(const VmafCompatApi *api, Trace *t, VmafContext *vmaf)
{
    const char *name = NULL;
    enum VmafBackend backend = VMAF_BACKEND_UNKNOWN;
    int err = 0;
    for (unsigned i = 0; i < EXTRACTORS_MAX && !err; i++) {
        err = api->registered_feature_extractor(vmaf, i, &name, &backend);
        trace(t, "extractor %u %d %s %d", i, err, err ? "-" : trace_str(name), (int)backend);
    }
    trace(t, "extractor NULL %d", api->registered_feature_extractor(vmaf, 0, NULL, &backend));
    trace(t, "backend %d %d", api->context_get_backend(vmaf, &backend), (int)backend);
    trace(t, "backend NULL %d", api->context_get_backend(NULL, &backend));
    const char *twin = "x";
    const char *option = "x";
    trace(t, "twin psnr %d", api->feature_backend_twin(vmaf, "psnr", NULL, NULL, &twin, &option));
    trace(t, "twin %s option %s", trace_str(twin), trace_str(option));
    trace(t, "twin unknown %d",
          api->feature_backend_twin(vmaf, "no_such", NULL, NULL, &twin, &option));
    trace(t, "twin NULL %d", api->feature_backend_twin(vmaf, "psnr", NULL, NULL, NULL, &option));
}

static void frame_scores(const VmafCompatApi *api, Trace *t, VmafContext *vmaf, VmafModel *model,
                         VmafModelCollection *collection)
{
    static const char *const features[] = {"psnr_y", "psnr_cb", "imported", "no_such_feature"};
    for (unsigned i = 0; i < N_FRAMES; i++) {
        double score = -1.0;
        int err = api->score_at_index(vmaf, model, &score, i);
        trace(t, "score %u %d %a", i, err, score);
        VmafModelCollectionScore set;
        memset(&set, 0, sizeof(set));
        err = api->score_at_index_model_collection(vmaf, collection, &set, i);
        trace(t, "set %u %d %d %a %a %a %a", i, err, (int)set.type, set.bootstrap.bagging_score,
              set.bootstrap.stddev, set.bootstrap.ci.p95.lo, set.bootstrap.ci.p95.hi);
        for (unsigned f = 0; f < 4u; f++) {
            score = -1.0;
            err = api->feature_score_at_index(vmaf, features[f], &score, i);
            trace(t, "feature %s %u %d %a", features[f], i, err, score);
        }
        for (unsigned f = 0; f < api->model_feature_count(model); f++) {
            const char *const name = api->model_feature_name(model, f);
            score = -1.0;
            err = api->feature_score_at_index(vmaf, name, &score, i);
            trace(t, "model feature %s %u %d %a", trace_str(name), i, err, score);
        }
    }
    double score = 0.0;
    trace(t, "score NULL %d", api->score_at_index(vmaf, model, NULL, 0));
    trace(t, "score past end %d", api->score_at_index(vmaf, model, &score, N_FRAMES + 5u));
}

static void pooled_scores(const VmafCompatApi *api, Trace *t, VmafContext *vmaf, VmafModel *model,
                          VmafModelCollection *collection)
{
    for (int pool = 0; pool <= (int)VMAF_POOL_METHOD_PERC20 + 1; pool++) {
        const enum VmafPoolingMethod method = (enum VmafPoolingMethod)pool;
        double score = -1.0;
        int err = api->score_pooled(vmaf, model, method, &score, 0, N_FRAMES - 1u);
        trace(t, "pooled %d %d %a", pool, err, score);
        score = -1.0;
        err = api->feature_score_pooled(vmaf, "psnr_y", method, &score, 0, N_FRAMES - 1u);
        trace(t, "pooled psnr %d %d %a", pool, err, score);
        VmafModelCollectionScore set;
        memset(&set, 0, sizeof(set));
        err = api->score_pooled_model_collection(vmaf, collection, method, &set, 0, N_FRAMES - 1u);
        trace(t, "pooled set %d %d %a %a", pool, err, set.bootstrap.bagging_score,
              set.bootstrap.ci.p95.hi);
    }
    double score = 0.0;
    trace(t, "pooled reversed %d",
          api->score_pooled(vmaf, model, VMAF_POOL_METHOD_MEAN, &score, 2, 1));
    trace(t, "pooled NULL %d",
          api->feature_score_pooled(vmaf, NULL, VMAF_POOL_METHOD_MEAN, &score, 0, 1));
}

/* A report line the compat library adds or the clock decides: the run time,
 * and the provenance record with the backend receipt (ADR-2073, ADR-1359),
 * which only a VMAFx context carries. */
static int additive(const char *line)
{
    static const char *const members[] = {"fps", "provenance", "feature_backends", "backend_used"};
    for (size_t i = 0; i < sizeof(members) / sizeof(members[0]); i++) {
        if (strstr(line, members[i])) {
            return 1;
        }
    }
    return 0;
}

/* Lines of a report, except the run time (fps) and the provenance record
 * (ADR-2073): the compat library's context carries one, a context of the old
 * libvmaf none, which is the additive change of design section 2.11. XML
 * spreads the record over an element; JSON keeps it on one line. */
static void trace_report(Trace *t, const char *format)
{
    FILE *const file = fopen(REPORT_PATH, "r");
    if (!file) {
        trace(t, "report %s missing", format);
        return;
    }
    char line[REPORT_LINE];
    int in_record = 0;
    int skipping_rest = 0; /* a skipped line longer than the buffer goes on */
    for (unsigned n = 0; n < REPORT_LINES_MAX && fgets(line, sizeof(line), file); n++) {
        const int opens = strstr(line, "<provenance") != NULL;
        const int closes = strstr(line, "</provenance>") != NULL;
        const int skip = skipping_rest || in_record || opens || additive(line);
        skipping_rest = skip && !strchr(line, '\n');
        const int self_closing = opens && strstr(line, "/>") != NULL;
        in_record = (in_record || opens) && !closes && !self_closing;
        /* A JSON member before the record gains a comma: compare without it. */
        size_t len = strcspn(line, "\n");
        len -= len > 0 && line[len - 1u] == ',';
        if (!skip) {
            trace(t, "report %s %.*s", format, (int)len, line);
        }
    }
    (void)fclose(file);
    (void)remove(REPORT_PATH);
}

static void reports(const VmafCompatApi *api, Trace *t, VmafContext *vmaf)
{
    static const char *const names[] = {"none", "xml", "json", "csv", "sub"};
    for (int fmt = VMAF_OUTPUT_FORMAT_NONE; fmt <= VMAF_OUTPUT_FORMAT_SUB; fmt++) {
        const int err = api->write_output(vmaf, REPORT_PATH, (enum VmafOutputFormat)fmt);
        trace(t, "write %s %d", names[fmt], err);
        /* A refused format: the same -EINVAL, but libvmaf truncated the file
         * first and the compat library does not touch it (documented). */
        if (err) {
            (void)remove(REPORT_PATH);
            continue;
        }
        trace_report(t, names[fmt]);
    }
    trace(t, "write precise %d",
          api->write_output_with_format(vmaf, REPORT_PATH, VMAF_OUTPUT_FORMAT_JSON, "%.17g"));
    trace_report(t, "precise");
    trace(t, "write NULL path %d", api->write_output(vmaf, NULL, VMAF_OUTPUT_FORMAT_JSON));
    trace(t, "write NULL %d",
          api->write_output_with_format(NULL, REPORT_PATH, VMAF_OUTPUT_FORMAT_JSON, NULL));
}

void scenario_scoring(const VmafCompatApi *api, Trace *t)
{
    VmafContext *vmaf = NULL;
    trace(t, "init %d", api->init(&vmaf, quiet_config()));
    VmafModelConfig cfg = {.name = "vmaf", .flags = VMAF_MODEL_FLAGS_DEFAULT};
    VmafModel *model = NULL;
    VmafModel *lead = NULL;
    VmafModelCollection *collection = NULL;
    trace(t, "load %d", api->model_load(&model, &cfg, "vmaf_v0.6.1"));
    VmafModelConfig set_cfg = {.name = "vmaf_b", .flags = VMAF_MODEL_FLAGS_DEFAULT};
    trace(t, "set %d", api->model_collection_load(&lead, &collection, &set_cfg, "vmaf_b_v0.6.3"));
    trace(t, "use model %d", api->use_features_from_model(vmaf, model));
    trace(t, "use model NULL %d", api->use_features_from_model(vmaf, NULL));
    trace(t, "use set %d", api->use_features_from_model_collection(vmaf, collection));
    trace(t, "use set NULL %d", api->use_features_from_model_collection(vmaf, NULL));
    /* The context keeps the models it mounted (ADR-1755). */
    api->model_destroy(lead);
    register_features(api, t, vmaf);
    query_context(api, t, vmaf);
    submit_frames(api, t, vmaf);
    frame_scores(api, t, vmaf, model, collection);
    pooled_scores(api, t, vmaf, model, collection);
    reports(api, t, vmaf);
    trace(t, "close %d", api->close(vmaf));
    api->model_destroy(model);
    api->model_collection_destroy(collection);
}

/* ---- Preallocated pictures ----------------------------------------------------- */

static void preallocated_frames(const VmafCompatApi *api, Trace *t, VmafContext *vmaf)
{
    for (unsigned i = 0; i < N_FRAMES; i++) {
        VmafPicture ref;
        VmafPicture dist;
        const int a = api->fetch_preallocated_picture(vmaf, &ref);
        const int b = api->fetch_preallocated_picture(vmaf, &dist);
        trace(t, "fetch %u %d %d %ux%u", i, a, b, ref.w[0], ref.h[0]);
        fill(&ref, i, 0);
        fill(&dist, i, 1);
        trace(t, "read %u %d", i, api->read_pictures(vmaf, &ref, &dist, i));
    }
    trace(t, "fetch NULL %d", api->fetch_preallocated_picture(vmaf, NULL));
    trace(t, "flush %d", api->read_pictures(vmaf, NULL, NULL, 0));
    for (unsigned i = 0; i < N_FRAMES; i++) {
        double score = -1.0;
        const int err = api->feature_score_at_index(vmaf, "psnr_y", &score, i);
        trace(t, "psnr %u %d %a", i, err, score);
    }
}

void scenario_preallocated(const VmafCompatApi *api, Trace *t)
{
    VmafContext *vmaf = NULL;
    VmafContext *bare = NULL;
    (void)api->init(&vmaf, quiet_config());
    (void)api->init(&bare, quiet_config());
    VmafPicture pic;
    trace(t, "fetch without pool %d", api->fetch_preallocated_picture(bare, &pic));
    VmafPictureConfiguration cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.pic_params.w = FRAME_W;
    cfg.pic_params.h = FRAME_H;
    cfg.pic_params.bpc = 8;
    cfg.pic_params.pix_fmt = VMAF_PIX_FMT_YUV420P;
    trace(t, "preallocate none %d", api->preallocate_pictures(bare, cfg));
    trace(t, "preallocate NULL %d", api->preallocate_pictures(NULL, cfg));
    trace(t, "use psnr %d", api->use_feature(vmaf, "psnr", NULL));
    cfg.pic_cnt = 4;
    trace(t, "preallocate %d", api->preallocate_pictures(vmaf, cfg));
    preallocated_frames(api, t, vmaf);
    trace(t, "close %d %d", api->close(vmaf), api->close(bare));
}

/* NOLINTEND(modernize-use-nullptr) */
