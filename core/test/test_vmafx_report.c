/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Reports with their provenance record (#2142, ADR-2073, RC4 WP5):
 * vmafx_report_write() embeds the record in JSON and XML, leaves CSV and SUB
 * as they were and writes the sidecar when asked; vmafx_report_open() /
 * vmafx_report_field() read a report back; vmafx_report_verify() accepts a
 * report and a re-run of the same configuration and names the first field a
 * planted change touches: the model digest, the backend, a score, the record
 * digest, the score digest of a lossless report.
 *
 * Failing first: none of these functions exists on the WP8 base; a verifier
 * that stops comparing at the provenance object fails test_planted_score; one
 * that skips the digest fails test_planted_digest.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mu_table.h"
#include "owner_only_file.h"
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

#define LOSSLESS "%.17g"

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

/* Score the default model over N_FRAMES and write `path` in `format`. */
static bool write_scored(const char *path, uint32_t format, uint32_t flags, const char *sf)
{
    VmafxContext *context = NULL;
    VmafxModel *model = NULL;
    bool ok = vmafx_context_create(NULL, &context, NULL) == VMAFX_OK &&
              vmafx_model_load(NULL, MODEL_VERSION, &model, NULL) == VMAFX_OK &&
              vmafx_context_use_model(context, model, NULL) == VMAFX_OK &&
              vmafx_context_annotate(context, "input", "synthetic", NULL) == VMAFX_OK &&
              submit_frames(context, N_FRAMES) && vmafx_flush(context, NULL) == VMAFX_OK;
    for (uint64_t i = 0; ok && i < N_FRAMES; i++) {
        VmafxScore score = VMAFX_SCORE_INIT;
        ok = vmafx_score_frame(context, model, i, &score, NULL) == VMAFX_OK;
    }
    ok = ok && vmafx_report_write(context, path, format, flags, sf, NULL) == VMAFX_OK;
    vmafx_model_unref(model);
    if (context) {
        ok = vmafx_context_destroy(context, NULL) == VMAFX_OK && ok;
    }
    return ok;
}

static char *read_text(const char *path)
{
    FILE *const file = fopen(path, "rb");
    if (!file) {
        return NULL;
    }
    char *const text = calloc(1u << 20, 1);
    const size_t n = text ? fread(text, 1, (1u << 20) - 1u, file) : 0u;
    (void)fclose(file);
    if (text) {
        text[n] = '\0';
    }
    return text;
}

/* `text` written to `path`, created owner-only (owner_only_file.h). */
static bool write_text(const char *path, const char *text)
{
    FILE *const file = vmaf_test_open_owner_only(path);
    if (!file) {
        return false;
    }
    const size_t len = strlen(text);
    const bool ok = fwrite(text, 1, len, file) == len;
    return fclose(file) == 0 && ok;
}

/* `path` with the first `from` replaced by `to`, written to `out`. */
static bool plant(const char *path, const char *out, const char *from, const char *to)
{
    char *const text = read_text(path);
    const char *const at = text ? strstr(text, from) : NULL;
    bool ok = at != NULL;
    if (ok) {
        const size_t head = (size_t)(at - text);
        const size_t to_len = strlen(to);
        const char *const tail = at + strlen(from);
        const size_t tail_len = strlen(tail);
        char *const planted = malloc(head + to_len + tail_len + 1u);
        ok = planted != NULL;
        if (ok) {
            memcpy(planted, text, head);
            memcpy(planted + head, to, to_len);
            memcpy(planted + head + to_len, tail, tail_len);
            planted[head + to_len + tail_len] = '\0';
            ok = write_text(out, planted);
        }
        free(planted);
    }
    free(text);
    return ok;
}

static bool exists(const char *path)
{
    FILE *const file = fopen(path, "rb");
    if (file) {
        (void)fclose(file);
    }
    return file != NULL;
}

/* VMAFX_E_MISMATCH naming `field` when `recorded` is checked against
 * `rerun` (NULL: on its own). */
static bool mismatch_names(const char *recorded, const char *rerun, const char *field)
{
    VmafxReportFile *a = NULL;
    VmafxReportFile *b = NULL;
    bool ok = vmafx_report_open(recorded, &a, NULL) == VMAFX_OK &&
              (!rerun || vmafx_report_open(rerun, &b, NULL) == VMAFX_OK);
    VmafxError *error = NULL;
    ok = ok && vmafx_report_verify(a, b, &error) == VMAFX_E_MISMATCH &&
         vt_failed(&error, VMAFX_E_MISMATCH, field, VMAFX_SUBJECT_PARAMETER);
    vmafx_error_free(error);
    vmafx_report_close(a);
    vmafx_report_close(b);
    return ok;
}

static bool verifies(const char *recorded, const char *rerun)
{
    VmafxReportFile *a = NULL;
    VmafxReportFile *b = NULL;
    const bool ok = vmafx_report_open(recorded, &a, NULL) == VMAFX_OK &&
                    (!rerun || vmafx_report_open(rerun, &b, NULL) == VMAFX_OK) &&
                    vmafx_report_verify(a, b, NULL) == VMAFX_OK;
    vmafx_report_close(a);
    vmafx_report_close(b);
    return ok;
}

static bool field_is(const VmafxReportFile *file, const char *path, const char *expected)
{
    const char *const value = vmafx_report_field(file, path);
    return value && strcmp(value, expected) == 0;
}

static bool field_starts(const VmafxReportFile *file, const char *path, const char *prefix)
{
    const char *const value = vmafx_report_field(file, path);
    return value && strncmp(value, prefix, strlen(prefix)) == 0;
}

static bool members_absent(const VmafxReportFile *file)
{
    return vmafx_report_field(file, "provenance") == NULL &&
           vmafx_report_field(file, "provenance.no_such") == NULL;
}

static char *report_fields(const VmafxReportFile *file)
{
    mu_assert("digest", field_starts(file, "provenance.digest", "sha256:"));
    mu_assert("model", vmafx_report_field(file, "provenance.models[0].sha256") != NULL);
    mu_assert("annotation", field_is(file, "provenance.annotations[0].value", "synthetic"));
    mu_assert("backend receipt", field_is(file, "backend_used", "cpu"));
    mu_assert("score format", field_is(file, "score_format", LOSSLESS));
    mu_assert("score", vmafx_report_field(file, "frames[1].metrics.vmaf") != NULL);
    mu_assert("objects and absent members are no field", members_absent(file));
    return NULL;
}

static char *test_json_report_carries_the_record(void)
{
    mu_assert("write", write_scored("wp5_a.json", VMAFX_REPORT_FORMAT_JSON, 0, LOSSLESS));
    VmafxReportFile *file = NULL;
    mu_assert("open", vmafx_report_open("wp5_a.json", &file, NULL) == VMAFX_OK);
    char *const failed = report_fields(file);
    vmafx_report_close(file);
    mu_assert(failed, failed == NULL);
    mu_assert("fresh report verifies on its own", verifies("wp5_a.json", NULL));
    return NULL;
}

static char *test_rerun_matches(void)
{
    mu_assert("write", write_scored("wp5_a.json", VMAFX_REPORT_FORMAT_JSON, 0, LOSSLESS));
    mu_assert("write", write_scored("wp5_b.json", VMAFX_REPORT_FORMAT_JSON, 0, LOSSLESS));
    mu_assert("a re-run of the same configuration verifies", verifies("wp5_a.json", "wp5_b.json"));
    return NULL;
}

static char *test_planted_model_digest(void)
{
    mu_assert("write", write_scored("wp5_a.json", VMAFX_REPORT_FORMAT_JSON, 0, LOSSLESS));
    mu_assert("write", write_scored("wp5_b.json", VMAFX_REPORT_FORMAT_JSON, 0, LOSSLESS));
    mu_assert("plant", plant("wp5_a.json", "wp5_p.json", "\"sha256\":\"", "\"sha256\":\"0"));
    mu_assert("model digest named",
              mismatch_names("wp5_p.json", "wp5_b.json", "provenance.models[0].sha256"));
    mu_assert("record digest named on its own",
              mismatch_names("wp5_p.json", NULL, "provenance.digest"));
    return NULL;
}

static char *test_planted_backend(void)
{
    mu_assert("write", write_scored("wp5_a.json", VMAFX_REPORT_FORMAT_JSON, 0, LOSSLESS));
    mu_assert("write", write_scored("wp5_b.json", VMAFX_REPORT_FORMAT_JSON, 0, LOSSLESS));
    mu_assert("plant", plant("wp5_a.json", "wp5_p.json", "\"active_backend\":\"cpu\"",
                             "\"active_backend\":\"cuda\""));
    mu_assert("backend named",
              mismatch_names("wp5_p.json", "wp5_b.json", "provenance.active_backend"));
    return NULL;
}

static char *test_planted_score(void)
{
    mu_assert("write", write_scored("wp5_a.json", VMAFX_REPORT_FORMAT_JSON, 0, LOSSLESS));
    mu_assert("write", write_scored("wp5_b.json", VMAFX_REPORT_FORMAT_JSON, 0, LOSSLESS));
    mu_assert("plant", plant("wp5_a.json", "wp5_p.json", "\"vmaf\": ", "\"vmaf\": 1"));
    mu_assert("score named", mismatch_names("wp5_p.json", "wp5_b.json", "frames[0].metrics.vmaf"));
    mu_assert("lossless score digest named on its own",
              mismatch_names("wp5_p.json", NULL, "provenance.scores_digest"));
    return NULL;
}

static char *test_planted_digest(void)
{
    mu_assert("write", write_scored("wp5_a.json", VMAFX_REPORT_FORMAT_JSON, 0, LOSSLESS));
    mu_assert("write", write_scored("wp5_b.json", VMAFX_REPORT_FORMAT_JSON, 0, LOSSLESS));
    mu_assert("plant",
              plant("wp5_a.json", "wp5_p.json", "\"digest\":\"sha256:", "\"digest\":\"sha256:0"));
    mu_assert("digest named", mismatch_names("wp5_p.json", "wp5_b.json", "provenance.digest"));
    return NULL;
}

/* The text of `path` passes `check`; the text is freed on every path. */
static bool text_passes(const char *path, bool (*check)(const char *text))
{
    char *const text = read_text(path);
    const bool ok = text && check(text);
    free(text);
    return ok;
}

/* The engine writes the XML, JSON and CSV reports through a text-mode stream
 * (output_file_open() in libvmaf.c, as upstream's fopen(path, "w")), so a line
 * ends in CR LF on Windows; read_text() reads the bytes. */
#ifdef _WIN32
#define REPORT_EOL "\r\n"
#else
#define REPORT_EOL "\n"
#endif

static bool xml_has_record(const char *xml)
{
    return strstr(xml, "<provenance abi_major=") != NULL &&
           strstr(xml, "<model name=\"vmaf\"") != NULL &&
           strstr(xml, "</provenance>" REPORT_EOL "</VMAF>") != NULL;
}

static bool csv_untouched(const char *csv)
{
    return strncmp(csv, "Frame,", 6) == 0 && strstr(csv, "provenance") == NULL;
}

static bool sidecar_record(const char *sidecar)
{
    return strncmp(sidecar, "{\"abi_major\":", 13) == 0 &&
           strstr(sidecar, "\"digest\":\"sha256:") != NULL;
}

static char *test_xml_csv_and_sidecar(void)
{
    mu_assert("xml", write_scored("wp5_a.xml", VMAFX_REPORT_FORMAT_XML, 0, NULL));
    mu_assert("xml provenance", text_passes("wp5_a.xml", xml_has_record));
    (void)remove("wp5_a.csv.provenance.json");
    mu_assert("csv", write_scored("wp5_a.csv", VMAFX_REPORT_FORMAT_CSV, 0, NULL));
    mu_assert("csv untouched", text_passes("wp5_a.csv", csv_untouched));
    mu_assert("no sidecar unasked", !exists("wp5_a.csv.provenance.json"));
    mu_assert("csv + sidecar", write_scored("wp5_a.csv", VMAFX_REPORT_FORMAT_CSV,
                                            VMAFX_REPORT_PROVENANCE_SIDECAR, NULL));
    mu_assert("sidecar record", text_passes("wp5_a.csv.provenance.json", sidecar_record));
    return NULL;
}

static bool write_refused(VmafxContext *context, uint32_t format, uint32_t flags,
                          const char *subject)
{
    VmafxError *error = NULL;
    return vmafx_report_write(context, "x.json", format, flags, NULL, &error) == VMAFX_E_INVALID &&
           vt_failed(&error, VMAFX_E_INVALID, subject, VMAFX_SUBJECT_PARAMETER);
}

static char *test_write_refusals(void)
{
    VmafxContext *context = NULL;
    mu_assert("context", vmafx_context_create(NULL, &context, NULL) == VMAFX_OK);
    const bool format = write_refused(context, 0, 0, "format");
    const bool flags = write_refused(context, VMAFX_REPORT_FORMAT_JSON, 2u, "flags");
    (void)vmafx_context_destroy(context, NULL);
    mu_assert("format 0", format);
    mu_assert("flags", flags);
    return NULL;
}

/* vmafx_report_open() of `path` fails with `status` naming the path. */
static bool open_refused(const char *path, VmafxStatus status)
{
    VmafxReportFile *file = NULL;
    VmafxError *error = NULL;
    const bool refused = vmafx_report_open(path, &file, &error) == status &&
                         vt_failed(&error, status, path, VMAFX_SUBJECT_PATH) && file == NULL;
    vmafx_report_close(file);
    return refused;
}

static char *test_open_refusals(void)
{
    mu_assert("missing", open_refused("no/such.json", VMAFX_E_IO));
    mu_assert("write", write_text("wp5_bad.json", "{\"frames\": []}"));
    mu_assert("no provenance", open_refused("wp5_bad.json", VMAFX_E_INVALID));
    mu_assert("write", write_text("wp5_bad.json", "{\"provenance\": "));
    mu_assert("not JSON", open_refused("wp5_bad.json", VMAFX_E_INVALID));
    mu_assert("NULL recorded", vmafx_report_verify(NULL, NULL, NULL) == VMAFX_E_INVALID);
    vmafx_report_close(NULL);
    return NULL;
}

char *run_tests(void)
{
    static const MuTest tests[] = {
        MU_TEST(test_json_report_carries_the_record),
        MU_TEST(test_rerun_matches),
        MU_TEST(test_planted_model_digest),
        MU_TEST(test_planted_backend),
        MU_TEST(test_planted_score),
        MU_TEST(test_planted_digest),
        MU_TEST(test_xml_csv_and_sidecar),
        MU_TEST(test_write_refusals),
        MU_TEST(test_open_refusals),
    };
    return mu_run_table(tests, MU_TABLE_LEN(tests));
}

/* NOLINTEND(modernize-use-nullptr) */
