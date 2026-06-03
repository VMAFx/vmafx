/**
 *
 *  Copyright 2026 Lusoris
 *
 *     Licensed under the BSD+Patent License (the "License");
 *     you may not use this file except in compliance with the License.
 *     You may obtain a copy of the License at
 *
 *         https://opensource.org/licenses/BSDplusPatent
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 *
 */

/*
 * Public API coverage test — vmaf_score_at_index and vmaf_model_collection_load.
 *
 * The audit at .workingdir/audit-test-coverage-2026-05-16.md §2 found that
 * three public entry points have zero C-unit-test coverage:
 *
 *   - vmaf_score_at_index()             (libvmaf.h:218)
 *   - vmaf_model_collection_load()      (model.h:99)
 *   - vmaf_write_output()               (libvmaf.h:388)
 *
 * The existing test_predict.c exercises vmaf_predict_score_at_index() (internal)
 * but never the public vmaf_score_at_index() wrapper that takes a VmafContext*.
 * test_output.c exercises the internal writers via output.c include-trick but
 * not the public vmaf_write_output() dispatcher.
 *
 * Strategy: drive the public vmaf_init / vmaf_import_feature_score path to
 * place a pre-computed VMAF score in a VmafContext (no YUV frames needed), then
 * call vmaf_score_at_index() to verify the context-mediated score retrieval works.
 * vmaf_score_at_index() first probes the feature collector for a cached score
 * keyed by the model name (here "vmaf"); if found it is returned directly without
 * invoking vmaf_predict_score_at_index(), so the test stays purely public-API.
 * Similarly, load a model collection via the public vmaf_model_collection_load()
 * and assert basic structure invariants.  vmaf_write_output() is exercised
 * against a tmpfile to confirm the dispatcher routes correctly.
 *
 * VmafModel is an opaque typedef in the public header.  No internal struct
 * fields are accessed; this file includes only libvmaf/libvmaf.h and
 * libvmaf/model.h.
 *
 * These tests do NOT assert exact floating-point scores — they assert the calls
 * succeed and that the retrieved values are finite and within the 0–100 VMAF
 * range.  Score accuracy is governed by the Netflix-golden gate.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "test.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/model.h"

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

/*
 * Build a minimal VmafContext with one model, inject a synthetic VMAF score
 * at frame 0 using the model's name as the feature key.
 *
 * vmaf_score_at_index() first probes the feature-collector for a cached
 * score keyed by model->name (set to "vmaf" via VmafModelConfig.name).
 * Importing that score under the same key short-circuits the SVM regression
 * path and keeps the test purely within the public API (no struct-field
 * access on the opaque VmafModel typedef).
 *
 * Caller is responsible for vmaf_close(vmaf) and vmaf_model_destroy(model).
 */
static int make_vmaf_ctx_with_scores(VmafContext **vmaf_out, VmafModel **model_out)
{
    VmafConfiguration cfg = {0};
    int err = vmaf_init(vmaf_out, cfg);
    if (err)
        return err;

    VmafModelConfig mc = {.name = "vmaf", .flags = VMAF_MODEL_FLAGS_DEFAULT};
    err = vmaf_model_load(model_out, &mc, "vmaf_v0.6.1");
    if (err)
        return err;

    /*
     * Import a synthetic VMAF score of 60.0 under the feature name "vmaf"
     * (the model's name set in VmafModelConfig.name above).
     * vmaf_score_at_index() retrieves this directly from the feature
     * collector without calling the internal SVM prediction path.
     */
    err = vmaf_import_feature_score(*vmaf_out, "vmaf", 60.0, 0);
    if (err)
        return err;

    return 0;
}

/* -------------------------------------------------------------------------
 * Test: vmaf_score_at_index() — public entry point
 * ---------------------------------------------------------------------- */
static char *test_vmaf_score_at_index()
{
    int err;
    VmafContext *vmaf = NULL;
    VmafModel *model = NULL;

    err = make_vmaf_ctx_with_scores(&vmaf, &model);
    mu_assert("setup for vmaf_score_at_index failed", !err);

    double score = -1.0;
    err = vmaf_score_at_index(vmaf, model, &score, 0);
    mu_assert("vmaf_score_at_index returned error", !err);
    mu_assert("vmaf_score_at_index produced NaN", !isnan(score));
    mu_assert("vmaf_score_at_index score below 0", score >= 0.0);
    mu_assert("vmaf_score_at_index score above 100", score <= 100.0);

    /* NULL-pointer guards — must not crash, must return error. */
    err = vmaf_score_at_index(NULL, model, &score, 0);
    mu_assert("vmaf_score_at_index(NULL vmaf) should fail", err);

    err = vmaf_score_at_index(vmaf, NULL, &score, 0);
    mu_assert("vmaf_score_at_index(NULL model) should fail", err);

    (void)vmaf_close(vmaf);
    vmaf_model_destroy(model);
    return NULL;
}

/* -------------------------------------------------------------------------
 * Test: vmaf_model_collection_load() — public entry point
 * ---------------------------------------------------------------------- */
static char *test_vmaf_model_collection_load()
{
    int err;

    VmafModel *model = NULL;
    VmafModelCollection *collection = NULL;
    VmafModelConfig cfg = {.name = "vmaf", .flags = VMAF_MODEL_FLAGS_DEFAULT};

    /*
     * vmaf_b_v0.6.3.json is a bootstrap collection (keys "0".."N" at the
     * top level, each containing a full model sub-dict).
     * vmaf_model_collection_load() parses it and returns both the primary
     * model and the collection.  Both out-pointers must be non-NULL on
     * success.
     *
     * NOTE: plain single-model files (e.g. vmaf_v0.6.1.json) are NOT valid
     * collection input; passing them returns -EINVAL.  The collection loader
     * requires the numeric-keyed bootstrap format.
     */
    err = vmaf_model_collection_load(&model, &collection, &cfg, "vmaf_b_v0.6.3");
    mu_assert("vmaf_model_collection_load returned error", !err);
    mu_assert("vmaf_model_collection_load returned NULL collection", collection != NULL);
    mu_assert("vmaf_model_collection_load returned NULL model", model != NULL);

    vmaf_model_collection_destroy(collection);
    vmaf_model_destroy(model);

    /* Invalid version must return error and leave pointers unchanged. */
    model = NULL;
    collection = NULL;
    err = vmaf_model_collection_load(&model, &collection, &cfg, "no_such_model_xxxx");
    mu_assert("vmaf_model_collection_load should fail for unknown version", err);

    return NULL;
}

/*
 * Create an empty, writable temp file at @p out (size @p out_len).
 *
 * Portable temp-path resolution: MSYS2/MinGW64 does not map a usable
 * "/tmp" inside the GitHub Actions `windows-latest` runner — `mkstemp`
 * against a hardcoded `/tmp/...` template fails with ENOENT and the
 * test aborts with "mkstemp failed". Mirror the pattern used in
 * `libvmaf/test/dnn/test_model_loader.c::test_sidecar_parses` (added
 * for the same reason): on Windows query GetTempPathA() and synthesise
 * a deterministic filename inside it; on POSIX keep mkstemp() on
 * "/tmp" so the test retains O_CREAT|O_EXCL atomicity there.
 *
 * Returns 0 on success, non-zero on failure.
 */
static int make_temp_output_path(char *out, size_t out_len)
{
#ifdef _WIN32
    char tmpdir[MAX_PATH];
    DWORD tmplen = GetTempPathA((DWORD)sizeof tmpdir, tmpdir);
    if (tmplen == 0 || tmplen >= sizeof tmpdir)
        return -1;
    int n = snprintf(out, out_len, "%svmaf_test_output_%lu", tmpdir,
                     (unsigned long)GetCurrentProcessId());
    if (n <= 0 || (size_t)n >= out_len)
        return -1;
    /* Pre-create the file so the later fopen("r") + ftell can read it. */
    FILE *touch = fopen(out, "w");
    if (!touch)
        return -1;
    (void)fclose(touch);
    return 0;
#else
    const char tmpl[] = "/tmp/vmaf_test_output_XXXXXX";
    if (sizeof tmpl > out_len)
        return -1;
    memcpy(out, tmpl, sizeof tmpl);
    int fd = mkstemp(out);
    if (fd < 0)
        return -1;
    (void)close(fd);
    return 0;
#endif
}

/* -------------------------------------------------------------------------
 * Test: vmaf_write_output() dispatcher — public entry point
 * ---------------------------------------------------------------------- */
static char *test_vmaf_write_output()
{
    int err;
    VmafContext *vmaf = NULL;
    VmafModel *model = NULL;

    err = make_vmaf_ctx_with_scores(&vmaf, &model);
    mu_assert("setup for vmaf_write_output failed", !err);

    /* Confirm the score is retrievable before writing (sanity). */
    double score = -1.0;
    err = vmaf_score_at_index(vmaf, model, &score, 0);
    mu_assert("vmaf_score_at_index for write_output setup failed", !err);

    char tmp[260];
    err = make_temp_output_path(tmp, sizeof tmp);
    mu_assert("temp-output-path setup failed", !err);

    err = vmaf_write_output(vmaf, tmp, VMAF_OUTPUT_FORMAT_JSON);
    mu_assert("vmaf_write_output(JSON) returned error", !err);

    FILE *f = fopen(tmp, "r");
    mu_assert("could not open vmaf_write_output temp file", f != NULL);
    (void)fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    (void)fclose(f);
    (void)remove(tmp);

    mu_assert("vmaf_write_output(JSON) produced empty file", sz > 0);

    /* NULL path must return error. */
    err = vmaf_write_output(vmaf, NULL, VMAF_OUTPUT_FORMAT_JSON);
    mu_assert("vmaf_write_output(NULL path) should fail", err);

    (void)vmaf_close(vmaf);
    vmaf_model_destroy(model);
    return NULL;
}

/* -------------------------------------------------------------------------
 * Runner
 * ---------------------------------------------------------------------- */
char *run_tests()
{
    mu_run_test(test_vmaf_score_at_index);
    mu_run_test(test_vmaf_model_collection_load);
    mu_run_test(test_vmaf_write_output);
    return NULL;
}
