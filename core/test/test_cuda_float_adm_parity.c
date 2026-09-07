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

/* ADR-1220 — derived feature keys for the two option variants.
 *
 * Both options are VMAF_OPT_FLAG_FEATURE_PARAM, so setting one changes the key
 * the score is filed under (ADR-1183): the alias base plus `_<alias>_<%g
 * value>`. `adm_p_norm` aliases to `apn`, `adm_bypass_cm` to `bcm`. */
static const char *const ADM_FEATURES_APN[NUM_ADM_FEATURES] = {
    "adm2_apn_2", "adm_scale0_apn_2", "adm_scale1_apn_2", "adm_scale2_apn_2", "adm_scale3_apn_2",
};
static const char *const ADM_FEATURES_BCM[NUM_ADM_FEATURES] = {
    "adm2_bcm_1", "adm_scale0_bcm_1", "adm_scale1_bcm_1", "adm_scale2_bcm_1", "adm_scale3_bcm_1",
};

/* Build the option dictionary for a variant, or leave it NULL for defaults. */
static int adm_opts_build(VmafFeatureDictionary **opts, const char *name, const char *val)
{
    if (!name)
        return 0;
    return vmaf_feature_dictionary_set(opts, name, val);
}

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

static char *run_cpu(const char *opt_name, const char *opt_val, const char *const *keys,
                     double *out_scores)
{
    int err = 0;
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    err = vmaf_init(&vmaf, cfg);
    mu_assert("CPU: vmaf_init failed", !err);

    VmafFeatureDictionary *opts = NULL;
    err = adm_opts_build(&opts, opt_name, opt_val);
    mu_assert("CPU: adm_opts_build failed", !err);
    err = vmaf_use_feature(vmaf, "float_adm", opts);
    if (err)
        (void)vmaf_feature_dictionary_free(&opts);
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
        err = vmaf_feature_score_at_index(vmaf, keys[m], &out_scores[m], 1u);
        mu_assert("CPU: vmaf_feature_score_at_index(adm[i], idx=1) failed", !err);
    }

    err = vmaf_close(vmaf);
    mu_assert("CPU: vmaf_close failed", !err);
    return NULL;
}

static char *run_cuda(const char *opt_name, const char *opt_val, const char *const *keys,
                      double *out_scores, int *skipped)
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

    VmafFeatureDictionary *opts = NULL;
    err = adm_opts_build(&opts, opt_name, opt_val);
    mu_assert("CUDA: adm_opts_build failed", !err);
    err = vmaf_use_feature(vmaf, "float_adm_cuda", opts);
    if (err)
        (void)vmaf_feature_dictionary_free(&opts);
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
        err = vmaf_feature_score_at_index(vmaf, keys[m], &out_scores[m], 1u);
        mu_assert("CUDA: vmaf_feature_score_at_index(adm[i], idx=1) failed", !err);
    }

    err = vmaf_close(vmaf);
    mu_assert("CUDA: vmaf_close failed", !err);
    err = vmaf_cuda_state_free(cu_state);
    mu_assert("CUDA: vmaf_cuda_state_free failed", !err);
    return NULL;
}

static char *test_float_adm_cpu_cuda_parity(void)
{
    double cpu_scores[NUM_ADM_FEATURES] = {0};
    double cuda_scores[NUM_ADM_FEATURES] = {0};
    int skipped = 0;

    char *msg = run_cpu(NULL, NULL, ADM_FEATURES, cpu_scores);
    if (msg)
        return msg;
    msg = run_cuda(NULL, NULL, ADM_FEATURES, cuda_scores, &skipped);
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
                          "\nfloat_adm parity FAIL %s: cpu=%.8f cuda=%.8f delta=%.2e tol=%.2e\n",
                          ADM_FEATURES[m], cpu_scores[m], cuda_scores[m], delta, PARITY_TOL);
        }
        mu_assert("float_adm CPU vs. CUDA delta exceeds places=4 tolerance (1e-4)",
                  delta <= PARITY_TOL);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* ADR-1220 — adm_p_norm and adm_bypass_cm must reach the kernels.     */
/*                                                                     */
/* Both are VMAF_OPT_FLAG_FEATURE_PARAM options the twin declares with */
/* the CPU's names, aliases, defaults and ranges. The kernels          */
/* hardcoded the cube sum and the host pooling hardcoded the 1/3 root, */
/* so `apn` moved only the AIM exponent and produced a hybrid          */
/* quantity, and `bcm` was accepted and then read by nothing at all.   */
/* The default-options test above cannot see either, because p = 3 IS  */
/* the hardcoded exponent and bypass = 0 IS the hardcoded behaviour.   */
/* ------------------------------------------------------------------ */
static char *assert_opt_parity(const char *opt_name, const char *opt_val, const char *const *keys,
                               const char *label)
{
    double cpu_scores[NUM_ADM_FEATURES] = {0};
    double gpu_scores[NUM_ADM_FEATURES] = {0};
    int skipped = 0;

    char *msg = run_cpu(opt_name, opt_val, keys, cpu_scores);
    if (msg)
        return msg;
    msg = run_cuda(opt_name, opt_val, keys, gpu_scores, &skipped);
    if (msg)
        return msg;
    if (skipped)
        return NULL;

    for (unsigned m = 0; m < NUM_ADM_FEATURES; m++) {
        mu_assert("CPU float_adm score is non-finite", isfinite(cpu_scores[m]));
        mu_assert("CUDA float_adm score is non-finite", isfinite(gpu_scores[m]));
        const double delta = fabs(cpu_scores[m] - gpu_scores[m]);
        if (delta > PARITY_TOL) {
            (void)fprintf(stderr,
                          "\nfloat_adm %s parity FAIL key=%s: cpu=%.8f cuda=%.8f delta=%.2e "
                          "tol=%.2e\n",
                          label, keys[m], cpu_scores[m], gpu_scores[m], delta, PARITY_TOL);
        }
        mu_assert("float_adm with a non-default option drifts from the CPU reference",
                  delta <= PARITY_TOL);
    }
    return NULL;
}

static char *test_float_adm_p_norm_reaches_kernel(void)
{
    return assert_opt_parity("adm_p_norm", "2.0", ADM_FEATURES_APN, "apn=2.0");
}

static char *test_float_adm_bypass_cm_reaches_kernel(void)
{
    return assert_opt_parity("adm_bypass_cm", "1", ADM_FEATURES_BCM, "bcm=1");
}

char *run_tests(void)
{
    mu_run_test(test_float_adm_cpu_cuda_parity);
    mu_run_test(test_float_adm_p_norm_reaches_kernel);
    mu_run_test(test_float_adm_bypass_cm_reaches_kernel);
    return NULL;
}
