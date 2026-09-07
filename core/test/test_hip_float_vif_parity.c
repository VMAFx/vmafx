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
 * ADR-1217 — float_vif CPU vs. HIP parity, with and without the NEG options.
 *
 * `float_vif_hip` had no parity test at all: `test_hip_vif_parity.c` covers the
 * *integer* `vif_hip` twin. That gap is why the hardcoded
 * `vif_sigma_nsq = 2.0f` / `vif_enhn_gain_limit = 100.0f` in the HIP compute
 * kernel went unnoticed — both are VMAF_OPT_FLAG_FEATURE_PARAM options that the
 * extractor accepts and folds into the derived feature name, and
 * model/vmaf_float_v0.6.1neg.json sets `vif_enhn_gain_limit = 1.0` on all four
 * VIF scales.
 *
 * Two assertions:
 *   1. default options — CPU vs. HIP at places=4 (ADR-0214);
 *   2. NEG options (`vif_enhn_gain_limit = 1.0`, `vif_sigma_nsq = 1.5`) — the
 *      same gate on the derived `vif_scale0_egl_1_snsq_1.5` key. Assertion 1
 *      cannot see a kernel that ignores its options, because the hardcoded
 *      values *are* the defaults.
 *
 * Skip behaviour: if vmaf_hip_state_init() fails (no HIP runtime / no device)
 * OR any call returns -ENOSYS (scaffold posture under enable_hipcc=false) the
 * test emits a "[skip: ...]" marker and passes — the same skip contract as
 * test_hip_float_psnr_parity.c.
 */

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test.h"

#include "feature/feature_extractor.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_hip.h"
#include "libvmaf/picture.h"

#define FIXTURE_W 256u
#define FIXTURE_H 144u
#define FIXTURE_BPC 8u
#define PARITY_TOL 1e-4

/* The values model/vmaf_float_v0.6.1neg.json actually ships, plus a
 * non-default neural-noise variance. Both are feature params, so the score is
 * filed under a derived key: alias base + `_<alias>_<%g value>` per option,
 * sorted by option NAME (`vif_enhn_gain_limit` before `vif_sigma_nsq`). */
#define NEG_EGL "1.0"
#define NEG_SNSQ "1.5"
#define DEFAULT_SCALE0_KEY "VMAF_feature_vif_scale0_score"
#define NEG_SCALE0_KEY "vif_scale0_egl_1_snsq_1.5"

static int fill_pic(VmafPicture *pic, unsigned salt)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;
    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            y[row * pic->stride[0] + col] = (uint8_t)((row * 3u + col + salt * 29u) & 0xFFu);
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

static int feed_frame(VmafContext *vmaf)
{
    VmafPicture ref;
    VmafPicture dist;
    int err = fill_pic(&ref, 0u);
    if (err)
        return err;
    err = fill_pic(&dist, 1u);
    if (err) {
        vmaf_picture_unref(&ref);
        return err;
    }
    return vmaf_read_pictures(vmaf, &ref, &dist, 0u);
}

/* Build the NEG option dictionary, or NULL for the default-options run.
 * Returns non-zero on allocation failure. */
static int neg_opts_build(VmafFeatureDictionary **opts)
{
    int err = vmaf_feature_dictionary_set(opts, "vif_enhn_gain_limit", NEG_EGL);
    if (err)
        return err;
    return vmaf_feature_dictionary_set(opts, "vif_sigma_nsq", NEG_SNSQ);
}

static char *run_cpu_float_vif(bool neg_opts, const char *key, double *score)
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    int err = vmaf_init(&vmaf, cfg);
    mu_assert("CPU: vmaf_init failed", !err);

    VmafFeatureDictionary *opts = NULL;
    if (neg_opts) {
        err = neg_opts_build(&opts);
        mu_assert("CPU: neg_opts_build failed", !err);
    }
    err = vmaf_use_feature(vmaf, "float_vif", opts);
    if (err)
        (void)vmaf_feature_dictionary_free(&opts);
    mu_assert("CPU: vmaf_use_feature(float_vif) failed", !err);

    err = feed_frame(vmaf);
    mu_assert("CPU: feed_frame failed", !err);
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("CPU: vmaf_read_pictures(EOS) failed", !err);
    err = vmaf_feature_score_at_index(vmaf, key, score, 0u);
    mu_assert("CPU: vif_scale0 score missing", !err);
    err = vmaf_close(vmaf);
    mu_assert("CPU: vmaf_close failed", !err);
    return NULL;
}

/* NOLINTNEXTLINE(readability-function-size): the -ENOSYS scaffold skip contract
 * has to be checked after each of the four HIP entry points, which is what
 * makes this longer than the CPU leg. Mirrors test_hip_float_psnr_parity.c. */
static char *run_hip_float_vif(bool neg_opts, const char *key, double *score, int *skipped)
{
    *score = NAN;
    *skipped = 0;
    VmafHipState *hip_state = NULL;
    VmafHipConfiguration hip_cfg = {.device_index = -1};
    int err = vmaf_hip_state_init(&hip_state, hip_cfg);
    if (err != 0 || hip_state == NULL) {
        (void)fprintf(stderr, "[skip: no HIP device] ");
        *skipped = 1;
        return NULL;
    }
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    err = vmaf_init(&vmaf, cfg);
    mu_assert("HIP: vmaf_init failed", !err);
    err = vmaf_hip_import_state(vmaf, hip_state);
    mu_assert("HIP: vmaf_hip_import_state failed", !err);

    VmafFeatureDictionary *opts = NULL;
    if (neg_opts) {
        err = neg_opts_build(&opts);
        mu_assert("HIP: neg_opts_build failed", !err);
    }
    err = vmaf_use_feature(vmaf, "float_vif_hip", opts);
    if (err == -ENOSYS) {
        (void)fprintf(stderr, "[skip: HIP scaffold ENOSYS] ");
        *skipped = 1;
        (void)vmaf_feature_dictionary_free(&opts);
        (void)vmaf_close(vmaf);
        vmaf_hip_state_free(&hip_state);
        return NULL;
    }
    if (err)
        (void)vmaf_feature_dictionary_free(&opts);
    mu_assert("HIP: vmaf_use_feature(float_vif_hip) failed", !err);

    err = feed_frame(vmaf);
    if (err == -ENOSYS) {
        (void)fprintf(stderr, "[skip: HIP scaffold ENOSYS on feed] ");
        *skipped = 1;
        (void)vmaf_close(vmaf);
        vmaf_hip_state_free(&hip_state);
        return NULL;
    }
    mu_assert("HIP: feed_frame failed", !err);

    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    if (err == -ENOSYS) {
        (void)fprintf(stderr, "[skip: HIP scaffold ENOSYS on EOS] ");
        *skipped = 1;
        (void)vmaf_close(vmaf);
        vmaf_hip_state_free(&hip_state);
        return NULL;
    }
    mu_assert("HIP: vmaf_read_pictures(EOS) failed", !err);

    err = vmaf_feature_score_at_index(vmaf, key, score, 0u);
    mu_assert("HIP: vif_scale0 score missing", !err);
    err = vmaf_close(vmaf);
    mu_assert("HIP: vmaf_close failed", !err);
    vmaf_hip_state_free(&hip_state);
    return NULL;
}

static char *test_float_vif_hip_registered(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("float_vif_hip");
    mu_assert("float_vif_hip extractor must be registered", fex != NULL);
    mu_assert("float_vif_hip name matches", !strcmp(fex->name, "float_vif_hip"));
    return NULL;
}

/* Compare one CPU/HIP pair; `label` names the option set in the failure line. */
static char *assert_parity(bool neg_opts, const char *key, const char *label)
{
    double cpu = 0.0;
    double gpu = NAN;
    int skipped = 0;

    char *msg = run_cpu_float_vif(neg_opts, key, &cpu);
    if (msg)
        return msg;
    msg = run_hip_float_vif(neg_opts, key, &gpu, &skipped);
    if (msg)
        return msg;
    if (skipped)
        return NULL;

    mu_assert("CPU float_vif score is non-finite", isfinite(cpu));
    mu_assert("HIP float_vif score is non-finite", isfinite(gpu));

    const double delta = fabs(cpu - gpu);
    if (delta > PARITY_TOL) {
        (void)fprintf(stderr, "\nfloat_vif %s parity FAIL: cpu=%.8f hip=%.8f delta=%.2e tol=%.2e\n",
                      label, cpu, gpu, delta, PARITY_TOL);
    }
    mu_assert("float_vif CPU vs. HIP delta exceeds places=4 tolerance (1e-4)", delta <= PARITY_TOL);
    return NULL;
}

static char *test_float_vif_cpu_hip_parity(void)
{
    return assert_parity(false, DEFAULT_SCALE0_KEY, "default-options");
}

/* ADR-1217 — vif_enhn_gain_limit / vif_sigma_nsq must reach the kernel. The HIP
 * compute kernel hardcoded both to their defaults, so a non-default value was
 * accepted, folded into the derived feature name, and then silently ignored:
 * the NEG model's vif_enhn_gain_limit = 1.0 published un-clamped scores under
 * NEG feature keys. The default-options test above cannot see this. */
static char *test_float_vif_options_reach_kernel(void)
{
    return assert_parity(true, NEG_SCALE0_KEY, "egl=" NEG_EGL " snsq=" NEG_SNSQ);
}

char *run_tests(void)
{
    mu_run_test(test_float_vif_hip_registered);
    mu_run_test(test_float_vif_cpu_hip_parity);
    mu_run_test(test_float_vif_options_reach_kernel);
    return NULL;
}
