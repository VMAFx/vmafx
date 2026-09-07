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
 * ADR-0956 — float_adm CPU vs. CUDA parity test (round 4).
 *
 * The float-path ADM extractor is implemented independently in
 * core/src/feature/float_adm.c (CPU) and
 * core/src/feature/cuda/float_adm_cuda.c (CUDA). Both emit the
 * `VMAF_feature_adm2_score` family plus the four `adm_scale[0..3]`
 * sub-scores. The integer-path twin (`adm_cuda`) is gated by PR #374
 * (round 2); the float-path was the last uncovered ADM kernel.
 *
 * Without this test, a SIMD pivot on the CPU side or a kernel-grid
 * change on the CUDA side could silently shift the float-path ADM
 * scores away from the CPU reference and only surface weeks later via
 * a CHUG re-extract diff against the float `vmaf_float_*` model
 * lineage.
 *
 * Fixture: 256x144 YUV420P 8-bpc, 3 frames, ref/dist deterministic
 * ramps that differ so the ADM score is non-trivial and finite. We
 * read the score at frame index 1 and assert agreement at
 * `places=4` / 1e-4 (ADR-0214 cross-backend gate).
 *
 * Skip behaviour: if vmaf_cuda_state_init() fails (no driver / no
 * device) the test emits "[skip: no CUDA device]" and passes.
 * Mirrors test_cuda_motion3_parity.c.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test.h"

#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_cuda.h"
#include "libvmaf/picture.h"

#ifndef FIXTURE_W
#define FIXTURE_W 256u
#endif
#ifndef FIXTURE_H
#define FIXTURE_H 144u
#endif
#define FIXTURE_BPC 8u
#define NUM_FRAMES 3u

/* ADR-0214 cross-backend tolerance (places=4 → 1e-4). */
#define PARITY_TOL 1e-4

/* Subset of ADM features that BOTH CPU (`float_adm`) and CUDA
 * (`float_adm_cuda`) emit on the same name. We compare:
 *   - the aggregate score (adm2)
 *   - all four scale sub-scores (drift here is the most common
 *     SIMD-vs-kernel divergence signal). */
#define NUM_ADM_FEATURES 5u
static const char *const ADM_FEATURES[NUM_ADM_FEATURES] = {
    "VMAF_feature_adm2_score",       "VMAF_feature_adm_scale0_score",
    "VMAF_feature_adm_scale1_score", "VMAF_feature_adm_scale2_score",
    "VMAF_feature_adm_scale3_score",
};

static int fill_ref(VmafPicture *pic, unsigned frame_idx)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;

    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            y[row * pic->stride[0] + col] = (uint8_t)((row + col + frame_idx * 7u) & 0xFFu);
        }
    }
    for (unsigned p = 1; p < 3; p++) {
        uint8_t *plane = (uint8_t *)pic->data[p];
        for (unsigned row = 0; row < pic->h[p]; row++) {
            memset(plane + row * pic->stride[p], 128, pic->w[p]);
        }
    }
    return 0;
}

static int fill_dist(VmafPicture *pic, unsigned frame_idx)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;

    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            const unsigned base = (row + col + frame_idx * 7u) & 0xFFu;
            const unsigned noise = ((row * 3u + col * 2u + frame_idx) % 13u);
            y[row * pic->stride[0] + col] = (uint8_t)((base + noise) & 0xFFu);
        }
    }
    for (unsigned p = 1; p < 3; p++) {
        uint8_t *plane = (uint8_t *)pic->data[p];
        for (unsigned row = 0; row < pic->h[p]; row++) {
            memset(plane + row * pic->stride[p], 128, pic->w[p]);
        }
    }
    return 0;
}

/* Derived feature names use the feature's ALIAS as their base once any option
 * is non-default (feature_name.cpp: `vmaf_feature_name_alias(name)` + one
 * `_<alias>_<value>` per option, options sorted alphabetically by name), e.g.
 * "VMAF_feature_adm2_score" -> "adm2_scfd_0.5_scf_2". Strip the fixed
 * prefix/suffix at runtime rather than keeping a parallel alias table. */
static void adm_key(char *out, size_t n, const char *full, const char *suffix)
{
    static const char pfx[] = "VMAF_feature_";
    static const char sfx[] = "_score";
    const size_t pl = sizeof(pfx) - 1u;
    const size_t sl = sizeof(sfx) - 1u;
    const size_t fl = strlen(full);
    if (!suffix[0]) {
        (void)snprintf(out, n, "%s", full);
    } else if (strncmp(full, pfx, pl) == 0 && fl > pl + sl && strcmp(full + fl - sl, sfx) == 0) {
        (void)snprintf(out, n, "%.*s%s", (int)(fl - pl - sl), full + pl, suffix);
    } else {
        (void)snprintf(out, n, "%s%s", full, suffix);
    }
}

static char *run_cpu(double *out_scores, VmafFeatureDictionary *opts, const char *suffix)
{
    int err = 0;
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    err = vmaf_init(&vmaf, cfg);
    mu_assert("CPU: vmaf_init failed", !err);

    err = vmaf_use_feature(vmaf, "float_adm", opts);
    mu_assert("CPU: vmaf_use_feature(float_adm) failed", !err);

    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        VmafPicture ref, dist;
        err = fill_ref(&ref, i);
        mu_assert("CPU: fill_ref failed", !err);
        err = fill_dist(&dist, i);
        mu_assert("CPU: fill_dist failed", !err);
        err = vmaf_read_pictures(vmaf, &ref, &dist, i);
        mu_assert("CPU: vmaf_read_pictures failed", !err);
    }
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("CPU: vmaf_read_pictures(EOS) failed", !err);

    for (unsigned m = 0; m < NUM_ADM_FEATURES; m++) {
        char key[128];
        adm_key(key, sizeof(key), ADM_FEATURES[m], suffix);
        err = vmaf_feature_score_at_index(vmaf, key, &out_scores[m], 1u);
        mu_assert("CPU: vmaf_feature_score_at_index(adm[i], idx=1) failed", !err);
    }

    err = vmaf_close(vmaf);
    mu_assert("CPU: vmaf_close failed", !err);
    return NULL;
}

static char *run_cuda(double *out_scores, int *skipped, VmafFeatureDictionary *opts,
                      const char *suffix)
{
    *skipped = 0;
    for (unsigned m = 0; m < NUM_ADM_FEATURES; m++)
        out_scores[m] = NAN;

    int err = 0;
    VmafCudaState *cu_state = NULL;
    VmafCudaConfiguration cuda_cfg = {0};
    err = vmaf_cuda_state_init(&cu_state, cuda_cfg);
    if (err != 0 || cu_state == NULL) {
        (void)fprintf(stderr, "[skip: no CUDA device] ");
        *skipped = 1;
        return NULL;
    }

    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    err = vmaf_init(&vmaf, cfg);
    mu_assert("CUDA: vmaf_init failed", !err);

    err = vmaf_cuda_import_state(vmaf, cu_state);
    mu_assert("CUDA: vmaf_cuda_import_state failed", !err);

    err = vmaf_use_feature(vmaf, "float_adm_cuda", opts);
    mu_assert("CUDA: vmaf_use_feature(float_adm_cuda) failed", !err);

    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        VmafPicture ref, dist;
        err = fill_ref(&ref, i);
        mu_assert("CUDA: fill_ref failed", !err);
        err = fill_dist(&dist, i);
        mu_assert("CUDA: fill_dist failed", !err);
        err = vmaf_read_pictures(vmaf, &ref, &dist, i);
        mu_assert("CUDA: vmaf_read_pictures failed", !err);
    }
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("CUDA: vmaf_read_pictures(EOS) failed", !err);

    for (unsigned m = 0; m < NUM_ADM_FEATURES; m++) {
        char key[128];
        adm_key(key, sizeof(key), ADM_FEATURES[m], suffix);
        err = vmaf_feature_score_at_index(vmaf, key, &out_scores[m], 1u);
        mu_assert("CUDA: vmaf_feature_score_at_index(adm[i], idx=1) failed", !err);
    }

    err = vmaf_close(vmaf);
    mu_assert("CUDA: vmaf_close failed", !err);
    err = vmaf_cuda_state_free(cu_state);
    mu_assert("CUDA: vmaf_cuda_state_free failed", !err);
    return NULL;
}

/* Shared comparison body: run both sides with `opts` (consumed by
 * vmaf_use_feature; build a fresh dictionary per side) and compare every
 * feature whose name is ADM_FEATURES[m] + suffix. */
static char *compare_cpu_cuda(VmafFeatureDictionary *cpu_opts, VmafFeatureDictionary *cuda_opts,
                              const char *suffix, const char *label)
{
    double cpu_scores[NUM_ADM_FEATURES] = {0};
    double cuda_scores[NUM_ADM_FEATURES] = {0};
    int skipped = 0;

    char *msg = run_cpu(cpu_scores, cpu_opts, suffix);
    if (msg)
        return msg;
    msg = run_cuda(cuda_scores, &skipped, cuda_opts, suffix);
    if (msg)
        return msg;
    if (skipped)
        return NULL;

    for (unsigned m = 0; m < NUM_ADM_FEATURES; m++) {
        mu_assert("CPU float_adm score is non-finite", isfinite(cpu_scores[m]));
        mu_assert("CUDA float_adm score is non-finite", isfinite(cuda_scores[m]));

        const double delta = fabs(cpu_scores[m] - cuda_scores[m]);
        if (delta > PARITY_TOL) {
            (void)fprintf(stderr,
                          "\nfloat_adm parity FAIL [%s] %s%s: cpu=%.8f cuda=%.8f delta=%.2e "
                          "tol=%.2e\n",
                          label, ADM_FEATURES[m], suffix, cpu_scores[m], cuda_scores[m], delta,
                          PARITY_TOL);
        }
        mu_assert("float_adm CPU vs. CUDA delta exceeds places=4 tolerance (1e-4)",
                  delta <= PARITY_TOL);
    }
    return NULL;
}

static char *test_float_adm_cpu_cuda_parity(void)
{
    return compare_cpu_cuda(NULL, NULL, "", "default");
}

/* ADR-1214: adm_csf_scale / adm_csf_diag_scale are Barten-mode (mode 1)
 * options. In the Watson-97 mode this twin supports, the CPU ignores them;
 * the twin used to multiply them into every CSF rfactor. The derived feature
 * names must also agree — the CPU aliases them "scf" / "scfd", and the twin
 * used to say "cs" / "cds", so the same request produced different keys.
 * Options are sorted alphabetically by NAME when the suffix is built
 * (adm_csf_diag_scale before adm_csf_scale), and numeric values print with
 * %g, hence "_scfd_0.5_scf_2". */
static VmafFeatureDictionary *csf_scale_opts(void)
{
    VmafFeatureDictionary *d = NULL;
    if (vmaf_feature_dictionary_set(&d, "adm_csf_scale", "2.0"))
        return NULL;
    if (vmaf_feature_dictionary_set(&d, "adm_csf_diag_scale", "0.5"))
        return NULL;
    return d;
}

static char *test_float_adm_cpu_cuda_parity_csf_scale(void)
{
    VmafFeatureDictionary *cpu_opts = csf_scale_opts();
    VmafFeatureDictionary *cuda_opts = csf_scale_opts();
    mu_assert("csf_scale_opts: dictionary build failed", cpu_opts && cuda_opts);
    return compare_cpu_cuda(cpu_opts, cuda_opts, "_scfd_0.5_scf_2", "adm_csf_scale=2");
}

char *run_tests(void)
{
    mu_run_test(test_float_adm_cpu_cuda_parity);
    mu_run_test(test_float_adm_cpu_cuda_parity_csf_scale);
    return NULL;
}
