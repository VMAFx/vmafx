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
 * Smoke test for the CAMBI SYCL extractor (T3-15 / ADR-0371).
 *
 * Goals:
 *   1. Verify vmaf_fex_cambi_sycl is discoverable via vmaf_get_feature_extractor_by_name.
 *   2. Verify end-to-end init → submit → collect → close with a synthetic
 *      all-flat frame does not crash and emits a finite non-negative score.
 *
 * This is NOT a numerical-correctness test. Bit-exactness against the CPU
 * scalar extractor is verified by the cross-backend scoring gate
 * (validate-scores / ADR-0138 / ADR-0139). A full-precision numerical
 * assertion belongs in python/test/ alongside the other golden-data tests,
 * not here (per CLAUDE.md §8).
 */

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include "config.h"
#include "test.h"

#if HAVE_SYCL

#include "libvmaf/libvmaf_sycl.h"
#include "feature/feature_extractor.h"

static VmafSyclState *sycl = NULL;
static int sycl_init_failed = 0;

/* ------------------------------------------------------------------ */
/* Setup: initialise SYCL state (skip device tests if no GPU present). */
/* ------------------------------------------------------------------ */
static char *test_cambi_sycl_setup(void)
{
    VmafSyclConfiguration cfg = {.device_index = -1};
    int err = vmaf_sycl_state_init(&sycl, cfg);
    if (err) {
        fprintf(stderr,
                "  [SKIP] SYCL state init failed (err=%d), "
                "no GPU available — skipping cambi_sycl tests\n",
                err);
        sycl_init_failed = 1;
        sycl = NULL;
        return NULL;
    }
    mu_assert("sycl state should be non-NULL", sycl != NULL);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Test 1: registration — vmaf_fex_cambi_sycl is discoverable.         */
/* ------------------------------------------------------------------ */
static char *test_cambi_sycl_registration(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("cambi_sycl");
    mu_assert("vmaf_fex_cambi_sycl should be findable by name", fex != NULL);
    if (fex) {
        mu_assert("fex name should be cambi_sycl", strcmp(fex->name, "cambi_sycl") == 0);
        mu_assert("fex flags should include VMAF_FEATURE_EXTRACTOR_SYCL",
                  (fex->flags & VMAF_FEATURE_EXTRACTOR_SYCL) != 0);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Test 2: end-to-end smoke — init/submit/collect/close without crash.  */
/* ------------------------------------------------------------------ */
static char *test_cambi_sycl_smoke(void)
{
    if (sycl_init_failed) {
        fprintf(stderr, "  [SKIP] test_cambi_sycl_smoke (no GPU)\n");
        return NULL;
    }

    /* Build a minimal VmafContext with the SYCL state imported. */
    VmafConfiguration vmaf_cfg = {
        .log_level = VMAF_LOG_LEVEL_NONE,
        .n_threads = 1,
    };
    VmafContext *vmaf = NULL;
    int err = vmaf_init(&vmaf, vmaf_cfg);
    mu_assert("vmaf_init should succeed", err == 0);
    if (err)
        return NULL;

    err = vmaf_sycl_import_state(vmaf, sycl);
    mu_assert("vmaf_sycl_import_state should succeed", err == 0);
    if (err) {
        (void)vmaf_close(vmaf);
        return NULL;
    }

    /* Allocate a synthetic 576×324 YUV420P 8-bpc picture pair.
     * Flat grey (value 64) — no banding, score should be 0. */
    static const unsigned W = 576u;
    static const unsigned H = 324u;
    VmafPicture ref_pic, dis_pic;
    err = vmaf_picture_alloc(&ref_pic, VMAF_PIX_FMT_YUV420P, 8, W, H);
    mu_assert("ref picture alloc", err == 0);
    err = vmaf_picture_alloc(&dis_pic, VMAF_PIX_FMT_YUV420P, 8, W, H);
    mu_assert("dis picture alloc", err == 0);

    /* Fill luma planes with a flat mid-grey value (128). */
    for (unsigned p = 0; p < 3; p++) {
        unsigned pw = (p == 0) ? W : W / 2;
        unsigned ph = (p == 0) ? H : H / 2;
        uint8_t *plane = (uint8_t *)ref_pic.data[p];
        for (unsigned r = 0; r < ph; r++) {
            memset(plane + r * (size_t)ref_pic.stride[p], 128, pw);
        }
        plane = (uint8_t *)dis_pic.data[p];
        for (unsigned r = 0; r < ph; r++) {
            memset(plane + r * (size_t)dis_pic.stride[p], 128, pw);
        }
    }

    /* Use vmaf_read_pictures to feed the frame through the pipeline.
     * cambi_sycl is registered and will be auto-selected when the SYCL
     * state is active and "Cambi_feature_cambi_score" is requested. */
    err = vmaf_use_feature(vmaf, "cambi_sycl", NULL);
    mu_assert("vmaf_use_feature(cambi_sycl) should succeed", err == 0);
    if (err) {
        (void)vmaf_picture_unref(&ref_pic);
        (void)vmaf_picture_unref(&dis_pic);
        (void)vmaf_close(vmaf);
        return NULL;
    }

    err = vmaf_read_pictures(vmaf, &ref_pic, &dis_pic, 0);
    mu_assert("vmaf_read_pictures should succeed", err == 0);

    /* Flush pipeline. */
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("flush vmaf_read_pictures should succeed", err == 0);

    /* Retrieve the score. */
    double score = -1.0;
    err = vmaf_feature_score_at_index(vmaf, "Cambi_feature_cambi_score", &score, 0);
    mu_assert("feature score retrieval should succeed", err == 0);
    mu_assert("score should be finite and non-negative", isfinite(score) && score >= 0.0);

    (void)vmaf_close(vmaf);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Teardown */
/* ------------------------------------------------------------------ */
static char *test_cambi_sycl_teardown(void)
{
    if (sycl) {
        vmaf_sycl_state_free(&sycl);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Test runner — fork's `test.c` provides `int main()` which calls    */
/* `run_tests()`. Each per-feature test file defines that hook.       */
/* ------------------------------------------------------------------ */
char *run_tests(void)
{
    mu_run_test(test_cambi_sycl_setup);
    mu_run_test(test_cambi_sycl_registration);
    mu_run_test(test_cambi_sycl_smoke);
    mu_run_test(test_cambi_sycl_teardown);
    return NULL;
}

#else /* !HAVE_SYCL */

char *run_tests(void)
{
    return NULL;
}

#endif /* HAVE_SYCL */
