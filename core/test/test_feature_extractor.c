/**
 *
 *  Copyright 2016-2026 Netflix, Inc.
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

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "dict.h"
#include "feature/feature_extractor.h"
#include "feature/feature_collector.h"
#include "fex_ctx_vector.h"
#include "test.h"
#include "picture.h"
#include "libvmaf/picture.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe, and this test mirrors
 * the C spelling of the surface it exercises. ADR-1138. */

static const char *const MOTION2_SCORE = "VMAF_integer_feature_motion2_score";

/* ---------------------------------------------------------------------------
 * Shared extract fixture
 *
 * Several tests need the same four-part setup: an extractor context, a
 * ref/dist picture pair and a feature collector. Keeping it in a helper (a)
 * removes the copy-paste, and (b) keeps the per-test `mu_assert` count inside
 * the `readability-function-size` branch budget the fork enforces (ADR-0141),
 * because each `mu_assert` expands to a branch in its enclosing function.
 * ------------------------------------------------------------------------- */

typedef struct {
    VmafFeatureExtractorContext *fex_ctx;
    VmafPicture ref;
    VmafPicture dist;
    VmafFeatureCollector *vfc;
} FexFixture;

/* Opens the fixture around the extractor registered as `fex_name`, handing
 * `opts` to the context constructor. Returns 0 on success, non-zero on the
 * first failing step. */
static int fex_fixture_open(FexFixture *fixture, const char *fex_name, VmafDictionary *opts)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name(fex_name);
    if (!fex)
        return -1;

    int err = vmaf_feature_extractor_context_create(&fixture->fex_ctx, fex, opts);
    if (err)
        return err;
    err = vmaf_picture_alloc(&fixture->ref, VMAF_PIX_FMT_YUV420P, 8, 1920, 1080);
    if (err)
        return err;
    err = vmaf_picture_alloc(&fixture->dist, VMAF_PIX_FMT_YUV420P, 8, 1920, 1080);
    if (err)
        return err;
    return vmaf_feature_collector_init(&fixture->vfc);
}

/* Closes and destroys the context, then releases the collector and pictures.
 * Returns the context close/destroy status; the frees cannot fail. */
static int fex_fixture_close(FexFixture *fixture)
{
    int err = vmaf_feature_extractor_context_close(fixture->fex_ctx);
    err |= vmaf_feature_extractor_context_destroy(fixture->fex_ctx);

    vmaf_feature_collector_destroy(fixture->vfc);
    vmaf_picture_unref(&fixture->ref);
    vmaf_picture_unref(&fixture->dist);
    return err;
}

/* Feeds the same picture pair through the context as frames 0 and 1, then
 * flushes. A temporal extractor holds frame 1 back until the flush, so this
 * is the minimal sequence that makes both indices retrievable. */
static int fex_fixture_extract_two_frames_and_flush(FexFixture *fixture)
{
    int err = vmaf_feature_extractor_context_extract(fixture->fex_ctx, &fixture->ref, NULL,
                                                     &fixture->dist, NULL, 0, fixture->vfc);
    if (err)
        return err;

    if (fixture->fex_ctx->fex->flags & VMAF_FEATURE_EXTRACTOR_PREV_REF)
        fixture->fex_ctx->fex->prev_ref = fixture->ref;

    err = vmaf_feature_extractor_context_extract(fixture->fex_ctx, &fixture->ref, NULL,
                                                 &fixture->dist, NULL, 1, fixture->vfc);
    if (err)
        return err;

    return vmaf_feature_extractor_context_flush(fixture->fex_ctx, fixture->vfc);
}

/* ---------------------------------------------------------------------------
 * Registry lookups
 * ------------------------------------------------------------------------- */

static char *test_get_feature_extractor_by_name_and_feature_name(void)
{
    VmafFeatureExtractor *fex;
    fex = vmaf_get_feature_extractor_by_name("");
    mu_assert("problem during vmaf_get_feature_extractor_by_name", !fex);
    fex = vmaf_get_feature_extractor_by_name("vif");
    mu_assert("problem vmaf_get_feature_extractor_by_name", !strcmp(fex->name, "vif"));

    fex = vmaf_get_feature_extractor_by_feature_name("VMAF_integer_feature_adm2_score", 0);
    mu_assert("problem during vmaf_get_feature_extractor_by_feature_name",
              fex && !strcmp(fex->name, "adm"));

#if HAVE_CUDA
    unsigned flags = VMAF_FEATURE_EXTRACTOR_CUDA;
    fex = vmaf_get_feature_extractor_by_feature_name("VMAF_integer_feature_adm2_score", flags);
    mu_assert("problem during vmaf_get_feature_extractor_by_feature_name",
              fex && !strcmp(fex->name, "adm_cuda"));
#endif

    return NULL;
}

/* Regression guard for the PR #875 feature_extractor.c -> .cpp split,
 * which orphaned the SpEED GPU twins: the externs + array entries stayed
 * in the now-deleted .c while meson compiled the .cpp, so
 * vmaf_get_feature_extractor_by_name("speed_chroma_cuda") returned NULL
 * and SpEED silently fell back to the CPU path (ADR-0964/0965/0852).
 * Each twin must resolve by name when its backend is compiled in. */
static char *test_speed_gpu_twins_resolve_by_name(void)
{
#if HAVE_CUDA
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("speed_chroma_cuda");
    mu_assert("speed_chroma_cuda must resolve by name",
              fex && !strcmp(fex->name, "speed_chroma_cuda"));
    fex = vmaf_get_feature_extractor_by_name("speed_temporal_cuda");
    mu_assert("speed_temporal_cuda must resolve by name",
              fex && !strcmp(fex->name, "speed_temporal_cuda"));
#endif
#if HAVE_SYCL
    VmafFeatureExtractor *sycl_fex = vmaf_get_feature_extractor_by_name("speed_chroma_sycl");
    mu_assert("speed_chroma_sycl must resolve by name",
              sycl_fex && !strcmp(sycl_fex->name, "speed_chroma_sycl"));
    sycl_fex = vmaf_get_feature_extractor_by_name("speed_temporal_sycl");
    mu_assert("speed_temporal_sycl must resolve by name",
              sycl_fex && !strcmp(sycl_fex->name, "speed_temporal_sycl"));
#endif
#if HAVE_HIP
    VmafFeatureExtractor *hip_fex = vmaf_get_feature_extractor_by_name("speed_chroma_hip");
    mu_assert("speed_chroma_hip must resolve by name",
              hip_fex && !strcmp(hip_fex->name, "speed_chroma_hip"));
    hip_fex = vmaf_get_feature_extractor_by_name("speed_temporal_hip");
    mu_assert("speed_temporal_hip must resolve by name",
              hip_fex && !strcmp(hip_fex->name, "speed_temporal_hip"));
#endif

    return NULL;
}

/* ADR-0544: walk the static `feature_extractor_list[]` and assert that no
 * extractor `name` (or symbol pointer) appears twice.  A duplicate caused
 * the ctx-pool's by-name iterator to allocate one entry per registration
 * and run init/extract/flush 2x-11x per picture; the bug was hidden from
 * the first-match get_by_name() path but trashed iterator-driven dispatch
 * (e.g. vmaf_use_features_from_model).  This test exercises the runtime
 * audit helper that also fires from vmaf_init(). */
static char *test_feature_extractor_list_no_duplicates(void)
{
    int err = vmaf_feature_extractor_list_audit();
    mu_assert("feature_extractor_list[] contains duplicate registrations "
              "(see ADR-0541; check the audit log above for the offending "
              "names/indices)",
              !err);
    return NULL;
}

/* Coverage push: NULL-input and unknown-name guards on the public lookup
 * symbols.  Pre-fix the gcovr report showed these single-line guards as
 * silently dead (lines 403-404 and the full-list walkthrough returning
 * NULL at line 443 in feature_extractor.c). */
static char *test_get_feature_extractor_null_and_unknown(void)
{
    VmafFeatureExtractor *fex;

    fex = vmaf_get_feature_extractor_by_name(NULL);
    mu_assert("by_name(NULL) must return NULL", !fex);
    fex = vmaf_get_feature_extractor_by_name("definitely-not-a-real-extractor");
    mu_assert("by_name(unknown) must return NULL", !fex);

    fex = vmaf_get_feature_extractor_by_feature_name(NULL, 0);
    mu_assert("by_feature_name(NULL) must return NULL", !fex);
    fex = vmaf_get_feature_extractor_by_feature_name("VMAF_not_a_provided_feature", 0);
    mu_assert("by_feature_name(unknown) must return NULL", !fex);

    return NULL;
}

/* Coverage push: ADR-0530 fallback path in
 * vmaf_get_feature_extractor_by_feature_name.  When the caller requests
 * a backend flag that no extractor on the registry carries for the named
 * feature, the second pass must drop the flag filter and return the CPU
 * twin.  In the CPU-only build the entire registry has no
 * VMAF_FEATURE_EXTRACTOR_CUDA-flagged motion2 provider, so requesting
 * the CUDA flag must fall back to the CPU "motion" extractor that
 * provides "VMAF_integer_feature_motion2_score". */
static char *test_get_feature_extractor_by_name_cuda_fallback(void)
{
    VmafFeatureExtractor *fex =
        vmaf_get_feature_extractor_by_feature_name(MOTION2_SCORE, VMAF_FEATURE_EXTRACTOR_CUDA);
    mu_assert("by_feature_name(CUDA) must fall back to the CPU twin", fex != NULL);
#if !HAVE_CUDA
    mu_assert("CPU build fallback must resolve to the CPU 'motion' extractor",
              !strcmp(fex->name, "motion"));
#endif
    return NULL;
}

/* Regression test for the missing-symbol bug fixed in
 * `feature_extractor.c`'s registry: `vmaf_fex_ssim` was defined in
 * `integer_ssim.c` but never listed in `feature_extractor_list[]`,
 * so `--feature ssim` could not resolve. This asserts that the
 * extractor is now reachable by name and emits a non-empty `ssim`
 * score for two identical 16x16 pictures. */
static char *test_ssim_extractor_registered_and_extracts(void)
{
    int err = 0;
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("ssim");
    mu_assert("ssim extractor must be registered in feature_extractor_list[]",
              fex && !strcmp(fex->name, "ssim"));

    VmafFeatureExtractorContext *fex_ctx;
    err = vmaf_feature_extractor_context_create(&fex_ctx, fex, NULL);
    VmafPicture ref;
    VmafPicture dist;
    err |= vmaf_picture_alloc(&ref, VMAF_PIX_FMT_YUV420P, 8, 16, 16);
    err |= vmaf_picture_alloc(&dist, VMAF_PIX_FMT_YUV420P, 8, 16, 16);
    VmafFeatureCollector *vfc;
    err |= vmaf_feature_collector_init(&vfc);
    err |= vmaf_feature_extractor_context_extract(fex_ctx, &ref, NULL, &dist, NULL, 0, vfc);
    mu_assert("problem during ssim setup/extract", !err);

    double score = -1.0;
    err = vmaf_feature_collector_get_score(vfc, "ssim", &score, 0);
    mu_assert("ssim score must be retrievable from collector", !err);

    err = vmaf_feature_extractor_context_close(fex_ctx);
    err |= vmaf_feature_extractor_context_destroy(fex_ctx);
    mu_assert("problem during ssim teardown", !err);

    vmaf_feature_collector_destroy(vfc);
    vmaf_picture_unref(&ref);
    vmaf_picture_unref(&dist);
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Extractor contexts and the context pool
 * ------------------------------------------------------------------------- */

static char *test_feature_extractor_context_pool(void)
{
    int err = 0;

    /* Enum rather than `const unsigned` so MSVC accepts the array
     * extent as a constant-expression (C `const` is runtime-bounded). */
    enum { n_threads = 8 };
    VmafFeatureExtractorContextPool *pool;
    err = vmaf_fex_ctx_pool_create(&pool, n_threads);
    mu_assert("problem during vmaf_fex_ctx_pool_create", !err);

    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("float_ssim");
    mu_assert("problem during vmaf_get_feature_extractor_by_name", fex);

    VmafFeatureExtractorContext *fex_ctx[n_threads];
    for (unsigned i = 0; i < n_threads; i++) {
        err = vmaf_fex_ctx_pool_aquire(pool, fex, NULL, &fex_ctx[i]);
        mu_assert("problem during vmaf_fex_ctx_pool_aquire", !err);
        mu_assert("fex_ctx[i] should be float_ssim feature extractor",
                  !strcmp(fex_ctx[i]->fex->name, "float_ssim"));
    }

    for (unsigned i = 0; i < n_threads; i++) {
        err = vmaf_fex_ctx_pool_release(pool, fex_ctx[i]);
        mu_assert("problem during vmaf_fex_ctx_pool_release", !err);
    }

    err = vmaf_fex_ctx_pool_destroy(pool);
    mu_assert("problem during vmaf_fex_ctx_pool_destroy", !err);

    return NULL;
}

/* A temporal extractor ("motion") withholds the last frame until the context
 * is flushed. Both picture indices must be retrievable from the collector
 * once the flush has run. */
static char *test_feature_extractor_flush(void)
{
    FexFixture fixture;
    mu_assert("problem during motion fixture setup", !fex_fixture_open(&fixture, "motion", NULL));
    mu_assert("problem vmaf_get_feature_extractor_by_name",
              !strcmp(fixture.fex_ctx->fex->name, "motion"));
    mu_assert("problem during two-frame extract + flush",
              !fex_fixture_extract_two_frames_and_flush(&fixture));

    double score;
    int err = vmaf_feature_collector_get_score(fixture.vfc, MOTION2_SCORE, &score, 0);
    err |= vmaf_feature_collector_get_score(fixture.vfc, MOTION2_SCORE, &score, 1);
    mu_assert("problem during vmaf_feature_collector_get_score", !err);

    mu_assert("problem during motion fixture teardown", !fex_fixture_close(&fixture));
    return NULL;
}

/* An option handed to the context constructor must reach the extractor:
 * `enable_chroma=false` on "psnr" suppresses the chroma sub-scores, so
 * "psnr_cb" must be absent from the collector afterwards. */
static char *test_feature_extractor_initialization_options(void)
{
    VmafDictionary *opts_dict = NULL;
    int err = vmaf_dictionary_set(&opts_dict, "enable_chroma", "false", 0);
    mu_assert("problem during vmaf_dictionary_set", !err);

    FexFixture fixture;
    mu_assert("problem during psnr fixture setup", !fex_fixture_open(&fixture, "psnr", opts_dict));
    mu_assert("problem vmaf_get_feature_extractor_by_name",
              !strcmp(fixture.fex_ctx->fex->name, "psnr"));

    err = vmaf_feature_extractor_context_extract(fixture.fex_ctx, &fixture.ref, NULL, &fixture.dist,
                                                 NULL, 0, fixture.vfc);
    mu_assert("problem during vmaf_feature_extractor_context_extract", !err);

    double score;
    err = vmaf_feature_collector_get_score(fixture.vfc, "psnr_cb", &score, 0);
    mu_assert("chroma PSNR was not disabled via option", err);

    mu_assert("problem during psnr fixture teardown", !fex_fixture_close(&fixture));
    return NULL;
}

/* Coverage push: NULL-input guards on the public extract / submit /
 * collect / flush / close / destroy entry points. */
static char *test_feature_extractor_context_null_guards(void)
{
    VmafPicture pic;
    VmafFeatureCollector *vfc = NULL;

    int err = vmaf_feature_extractor_context_extract(NULL, &pic, NULL, &pic, NULL, 0, vfc);
    mu_assert("extract(NULL ctx) must return -EINVAL", err == -EINVAL);

    err = vmaf_feature_extractor_context_submit(NULL, &pic, NULL, &pic, NULL, 0);
    mu_assert("submit(NULL ctx) must return -EINVAL", err == -EINVAL);

    err = vmaf_feature_extractor_context_submit_nocopy(NULL, 0);
    mu_assert("submit_nocopy(NULL ctx) must return -EINVAL", err == -EINVAL);

    err = vmaf_feature_extractor_context_collect(NULL, 0, vfc);
    mu_assert("collect(NULL ctx) must return -EINVAL", err == -EINVAL);

    err = vmaf_feature_extractor_context_flush(NULL, vfc);
    mu_assert("flush(NULL ctx) must return -EINVAL", err == -EINVAL);

    err = vmaf_feature_extractor_context_close(NULL);
    mu_assert("close(NULL ctx) must return -EINVAL", err == -EINVAL);

    err = vmaf_feature_extractor_context_destroy(NULL);
    mu_assert("destroy(NULL ctx) must return -EINVAL", err == -EINVAL);

    return NULL;
}

/* Coverage push: vmaf_fex_ctx_pool entry-point guards.  Exercises the
 * (!pool / !fex / !fex_ctx) -EINVAL guards on create / aquire / release
 * / flush / destroy. */
static char *test_fex_ctx_pool_null_guards(void)
{
    int err = vmaf_fex_ctx_pool_create(NULL, 1);
    mu_assert("pool_create(NULL pool) must return -EINVAL", err == -EINVAL);

    VmafFeatureExtractorContextPool *pool = NULL;
    err = vmaf_fex_ctx_pool_create(&pool, 0);
    mu_assert("pool_create(n_threads=0) must return -EINVAL", err == -EINVAL);

    err = vmaf_fex_ctx_pool_aquire(NULL, NULL, NULL, NULL);
    mu_assert("pool_aquire(NULL pool) must return -EINVAL", err == -EINVAL);

    err = vmaf_fex_ctx_pool_release(NULL, NULL);
    mu_assert("pool_release(NULL pool) must return -EINVAL", err == -EINVAL);

    err = vmaf_fex_ctx_pool_flush(NULL, NULL);
    mu_assert("pool_flush(NULL pool) must return -EINVAL", err == -EINVAL);

    err = vmaf_fex_ctx_pool_destroy(NULL);
    mu_assert("pool_destroy(NULL pool) must return -EINVAL", err == -EINVAL);

    return NULL;
}

/* ---------------------------------------------------------------------------
 * Registration vector dedup and option validation
 * ------------------------------------------------------------------------- */

/* Creates a context for `fex` and appends it to `rfe`. Returns the append()
 * status, which is 0 both when the entry was stored and when it was deduped
 * away (the dedup path destroys the incoming context and reports success). */
static int fex_vector_create_and_append(RegisteredFeatureExtractors *rfe, VmafFeatureExtractor *fex)
{
    VmafFeatureExtractorContext *ctx = NULL;
    int err = vmaf_feature_extractor_context_create(&ctx, fex, NULL);
    if (err)
        return err;
    return feature_extractor_vector_append(rfe, ctx, 0);
}

/* Regression test for T-CUDA-FEATURE-EXTRACTOR-DOUBLE-WRITE.
 *
 * When the CLI auto-loads a default VMAF model and the user also passes
 * --feature <name>, both vmaf_use_features_from_model() (registering the
 * GPU twin, e.g. "adm_cuda") and the explicit --feature path (registering
 * the CPU extractor "adm") call feature_extractor_vector_append().  The old
 * dedup key was the vmaf_feature_name_from_options()-derived string, which
 * compares the extractor *name* ("adm" vs "adm_cuda") and therefore misses
 * the twin.  Both extractors ran and wrote to the same feature-collector
 * slot, producing 750+ "cannot be overwritten" warnings per scoring run.
 *
 * The fix deduplicates by provided-feature names: if any entry in
 * provided_features[] matches between the already-registered extractor and
 * the incoming one, the incoming context is destroyed and the registration
 * is silently skipped.  This test exercises that path using two synthetic
 * VmafFeatureExtractor descriptors that share one provided-feature name,
 * simulating a CPU/GPU twin pair without requiring CUDA to be compiled in.
 */
static char *test_fex_vector_dedup_by_provided_feature_name(void)
{
    /* Two synthetic provided-feature lists that share "mock_feature_score". */
    static const char *pf_a[] = {"mock_feature_score", "mock_extra_a", NULL};
    static const char *pf_b[] = {"mock_feature_score", "mock_extra_b", NULL};

    VmafFeatureExtractor fex_a = {
        .name = "mock_cpu",
        .provided_features = pf_a,
    };
    VmafFeatureExtractor fex_b = {
        .name = "mock_gpu",
        .provided_features = pf_b,
    };

    RegisteredFeatureExtractors rfe;
    mu_assert("feature_extractor_vector_init failed", !feature_extractor_vector_init(&rfe));

    /* Append the first (CPU) extractor - must succeed and cnt becomes 1. */
    mu_assert("first append (mock_cpu) failed", !fex_vector_create_and_append(&rfe, &fex_a));
    mu_assert("cnt should be 1 after first append", rfe.cnt == 1);

    /* Append the second (GPU twin) extractor - dedup must fire, cnt stays 1,
     * and append must return 0 (the context is destroyed, not an error). */
    mu_assert("second append (mock_gpu twin) must not return an error",
              !fex_vector_create_and_append(&rfe, &fex_b));
    mu_assert("cnt must still be 1: GPU twin must be deduped by provided-feature name",
              rfe.cnt == 1);

    /* Verify the surviving entry is the first-registered (CPU) one. */
    mu_assert("surviving extractor must be mock_cpu",
              !strcmp(rfe.fex_ctx[0]->fex->name, "mock_cpu"));

    feature_extractor_vector_destroy(&rfe);
    return NULL;
}

/* supports_options() with nothing to validate: a NULL dictionary and an
 * empty (never-set) dictionary must both be accepted and must leave the
 * missing-key out-parameter untouched. */
static char *test_supports_options_empty_dict(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("psnr");
    mu_assert("psnr extractor missing", fex != NULL);

    const char *missing = NULL;
    bool ok = vmaf_feature_extractor_supports_options(fex, NULL, &missing);
    mu_assert("supports_options(NULL dict) must return true", ok && missing == NULL);

    VmafDictionary *opts = NULL;
    ok = vmaf_feature_extractor_supports_options(fex, opts, &missing);
    mu_assert("supports_options(empty dict) must return true", ok && missing == NULL);

    return NULL;
}

/* A key the extractor declares in its option table is accepted. */
static char *test_supports_options_known_key(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("psnr");
    mu_assert("psnr extractor missing", fex != NULL);

    VmafDictionary *opts = NULL;
    mu_assert("dictionary_set enable_chroma",
              vmaf_dictionary_set(&opts, "enable_chroma", "true", 0) == 0);

    const char *missing = NULL;
    bool ok = vmaf_feature_extractor_supports_options(fex, opts, &missing);
    mu_assert("supports_options(enable_chroma) must return true", ok && missing == NULL);

    (void)vmaf_dictionary_free(&opts);
    return NULL;
}

/* An undeclared key is rejected and named through the missing-key
 * out-parameter, which is what the CLI prints back to the user. */
static char *test_supports_options_unknown_key_reports_missing(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("psnr");
    mu_assert("psnr extractor missing", fex != NULL);

    VmafDictionary *opts = NULL;
    mu_assert("dictionary_set unknown_opt_xyz",
              vmaf_dictionary_set(&opts, "unknown_opt_xyz", "42", 0) == 0);

    const char *missing = NULL;
    bool ok = vmaf_feature_extractor_supports_options(fex, opts, &missing);
    mu_assert("supports_options(unknown) must return false", !ok);
    mu_assert("missing key must match unknown_opt_xyz",
              missing && !strcmp(missing, "unknown_opt_xyz"));

    (void)vmaf_dictionary_free(&opts);
    return NULL;
}

/* An option spelled with its registered alias ("mmxv" for motion's
 * motion_force_zero threshold) resolves like the canonical name. */
static char *test_supports_options_alias_key(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("motion");
    mu_assert("motion extractor missing", fex != NULL);

    VmafDictionary *opts = NULL;
    mu_assert("dictionary_set mmxv alias", vmaf_dictionary_set(&opts, "mmxv", "18", 0) == 0);

    const char *missing = NULL;
    bool ok = vmaf_feature_extractor_supports_options(fex, opts, &missing);
    mu_assert("supports_options(alias mmxv) must return true", ok && missing == NULL);

    (void)vmaf_dictionary_free(&opts);
    return NULL;
}

/* With no extractor to consult, every supplied key is unsupported: the
 * helper must report false and name the first offending key rather than
 * dereference the NULL descriptor. */
static char *test_supports_options_null_extractor(void)
{
    VmafDictionary *opts = NULL;
    mu_assert("dictionary_set some_key", vmaf_dictionary_set(&opts, "some_key", "1", 0) == 0);

    const char *missing = NULL;
    bool ok = vmaf_feature_extractor_supports_options(NULL, opts, &missing);
    mu_assert("supports_options(NULL fex) must return false", !ok);
    mu_assert("missing key must match some_key", missing && !strcmp(missing, "some_key"));

    (void)vmaf_dictionary_free(&opts);
    return NULL;
}

/* The context constructor rejects an unknown option outright and resets the
 * caller's handle, so a caller that ignores the status cannot go on to use a
 * stale pointer. */
static char *test_feature_extractor_unknown_option_rejected(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("psnr");
    mu_assert("psnr extractor missing", fex != NULL);

    VmafDictionary *opts = NULL;
    int err = vmaf_dictionary_set(&opts, "bogus_option_name", "123", 0);
    mu_assert("dictionary_set", err == 0);

    VmafFeatureExtractorContext *ctx = (VmafFeatureExtractorContext *)0x1234;
    err = vmaf_feature_extractor_context_create(&ctx, fex, opts);
    mu_assert("context_create with unknown option must return -EINVAL", err == -EINVAL);
    mu_assert("context handle must be reset to NULL on failure", ctx == NULL);

    (void)vmaf_dictionary_free(&opts);
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Registration
 *
 * The tests are grouped into per-area runners. `mu_run_test` expands to a
 * branch, so a single flat `run_tests` would exceed the fork's
 * readability-function-size branch budget; the grouping is the same pattern
 * test_cli_parse.c and test_iqa_helpers.c use.
 * ------------------------------------------------------------------------- */

static char *run_registry_tests(void)
{
    mu_run_test(test_get_feature_extractor_by_name_and_feature_name);
    mu_run_test(test_speed_gpu_twins_resolve_by_name);
    mu_run_test(test_feature_extractor_list_no_duplicates);
    mu_run_test(test_get_feature_extractor_null_and_unknown);
    mu_run_test(test_get_feature_extractor_by_name_cuda_fallback);
    mu_run_test(test_ssim_extractor_registered_and_extracts);
    return NULL;
}

static char *run_context_tests(void)
{
    mu_run_test(test_feature_extractor_context_pool);
    mu_run_test(test_feature_extractor_flush);
    mu_run_test(test_feature_extractor_initialization_options);
    mu_run_test(test_feature_extractor_context_null_guards);
    mu_run_test(test_fex_ctx_pool_null_guards);
    return NULL;
}

static char *run_option_tests(void)
{
    mu_run_test(test_fex_vector_dedup_by_provided_feature_name);
    mu_run_test(test_supports_options_empty_dict);
    mu_run_test(test_supports_options_known_key);
    mu_run_test(test_supports_options_unknown_key_reports_missing);
    mu_run_test(test_supports_options_alias_key);
    mu_run_test(test_supports_options_null_extractor);
    mu_run_test(test_feature_extractor_unknown_option_rejected);
    return NULL;
}

char *run_tests(void)
{
    char *result = run_registry_tests();
    if (result)
        return result;
    result = run_context_tests();
    if (result)
        return result;
    return run_option_tests();
}

/* NOLINTEND(modernize-use-nullptr) */
