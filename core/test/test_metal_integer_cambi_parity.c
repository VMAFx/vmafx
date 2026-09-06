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
 * Metal kernel parity — cambi CPU vs. Metal (ADR-0214 cross-backend gate).
 *
 * CAMBI (Contrast-Aware Multiscale Banding Index) is a JND-tuned banding
 * detector with non-trivial multi-scale filtering. The CPU extractor is
 * registered under the name `cambi` (core/src/feature/cambi.c) and emits
 * the feature key `Cambi_feature_cambi_score`. The Metal twin
 * `integer_cambi_metal` (core/src/feature/metal/integer_cambi_metal.mm +
 *  core/src/feature/metal/integer_cambi.metal) mirrors the CUDA twin's
 * Strategy II hybrid (integer_cambi_cuda.c, ADR-0360): three GPU kernels
 * (spatial mask, 2x decimate, 3-tap mode filter) run the parallel integer
 * stages, while the precision-sensitive sliding-histogram calculate_c_values
 * pass and the top-K spatial pooling run on the host CPU via the shared
 * wrappers in core/src/feature/cambi_internal.h. The emitted score is
 * therefore bit-for-bit identical to the CPU `cambi` extractor; the
 * places=4 (1e-4) ADR-0214 gate bounds any residual.
 *
 * Both backends require >= 216 on at least one dimension — see
 * CAMBI_MIN_WIDTH_HEIGHT in cambi.c. 256x256 is comfortably above the
 * threshold and still cheap to run in CI.
 *
 * Skip behaviour: -ENODEV from `vmaf_metal_state_init` -> clean skip on
 * Linux / Windows / Intel Mac.
 *
 * Cross-references:
 *   - core/test/test_cuda_cambi_parity.c (CUDA twin parity)
 *   - core/test/test_metal_integer_ssim_parity.c (Metal integer sibling)
 *   - core/src/feature/metal/integer_cambi_metal.mm
 *   - core/src/feature/cambi.c
 */

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"

#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_metal.h"
#include "libvmaf/picture.h"
#include "feature/feature_extractor.h"

/* CAMBI requires >= 216 on at least one dimension; 256x256 is safely above. */
#define FIXTURE_W 256u
#define FIXTURE_H 256u
#define FIXTURE_BPC 8u
#define PARITY_TOL 1e-4

static int fill_fixture(VmafPicture *pic, unsigned salt)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;
    /* Step-function quantised gradient — classic banding artefact pattern,
     * so CAMBI emits a non-trivial score in both backends. */
    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            const unsigned base = (col / 32u) * 32u + salt * 4u;
            y[row * pic->stride[0] + col] = (uint8_t)(base & 0xFFu);
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

static char *feed_one_frame(VmafContext *vmaf)
{
    VmafPicture ref;
    VmafPicture dist;
    int err = fill_fixture(&ref, 0u);
    mu_assert("fill_fixture(ref) failed", !err);
    err = fill_fixture(&dist, 1u);
    mu_assert("fill_fixture(dist) failed", !err);
    err = vmaf_read_pictures(vmaf, &ref, &dist, 0u);
    mu_assert("vmaf_read_pictures failed", !err);
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("vmaf_read_pictures(EOS) failed", !err);
    return NULL;
}

/* Exact mirror of the `cambi_high_res_speedup` row in `core/src/feature/cambi.c`
 * and `core/src/feature/cuda/integer_cambi_cuda.c`. Split out of the caller so
 * neither function trips the `readability-function-size` branch threshold. */
static char *assert_hrs_option_mirrors_cpu(const VmafOption *opt)
{
    mu_assert("hrs alias mismatch", opt->alias && !strcmp(opt->alias, "hrs"));
    mu_assert("hrs type mismatch", opt->type == VMAF_OPT_TYPE_INT);
    mu_assert("hrs default mismatch", opt->default_val.i == 0);
    mu_assert("hrs min mismatch", opt->min == 0.0);
    mu_assert("hrs max mismatch", opt->max == 2160.0);
    mu_assert("hrs flags mismatch", (opt->flags & VMAF_OPT_FLAG_FEATURE_PARAM) != 0);
    return NULL;
}

static char *test_cambi_metal_registered(void)
{
    const VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("integer_cambi_metal");
    mu_assert("integer_cambi_metal extractor missing", fex != NULL);
    mu_assert("integer_cambi_metal has no option table", fex->options != NULL);

    const VmafOption *hrs = NULL;
    for (unsigned i = 0; fex->options[i].name; i++) {
        if (!strcmp(fex->options[i].name, "cambi_high_res_speedup")) {
            hrs = &fex->options[i];
            break;
        }
    }
    mu_assert("cambi_high_res_speedup option missing from integer_cambi_metal", hrs != NULL);
    return assert_hrs_option_mirrors_cpu(hrs);
}

/* `vmaf_use_feature()` takes ownership of the dictionary on every path except
 * the argument-validation guards (see the contract in <libvmaf/libvmaf.h>), so
 * each runner builds its own — handing the same dictionary to both backends
 * would be a use-after-free on the second call, and freeing it afterwards a
 * double free. `hrs_val == NULL` means "no options". */
static int build_hrs_opts(VmafFeatureDictionary **opts, const char *hrs_val)
{
    *opts = NULL;
    if (hrs_val == NULL)
        return 0;
    return vmaf_feature_dictionary_set(opts, "cambi_high_res_speedup", hrs_val);
}

static char *run_cpu_cambi_opts(const char *hrs_val, const char *feature_score_name,
                                double *out_score)
{
    VmafFeatureDictionary *opts = NULL;
    int err = build_hrs_opts(&opts, hrs_val);
    mu_assert("CPU: vmaf_feature_dictionary_set(cambi_high_res_speedup) failed", !err);

    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    err = vmaf_init(&vmaf, cfg);
    mu_assert("CPU: vmaf_init failed", !err);

    err = vmaf_use_feature(vmaf, "cambi", opts);
    mu_assert("CPU: vmaf_use_feature(cambi) failed", !err);

    char *msg = feed_one_frame(vmaf);
    if (msg) {
        (void)vmaf_close(vmaf);
        return msg;
    }

    err = vmaf_feature_score_at_index(vmaf, feature_score_name, out_score, 0u);
    mu_assert("CPU: vmaf_feature_score_at_index(cambi) failed", !err);
    err = vmaf_close(vmaf);
    mu_assert("CPU: vmaf_close failed", !err);
    return NULL;
}

static char *run_metal_cambi_opts(const char *hrs_val, const char *feature_score_name,
                                  double *out_score, int *skipped)
{
    *skipped = 0;
    *out_score = NAN;

    VmafMetalConfiguration mcfg = {.device_index = -1, .flags = 0};
    VmafMetalState *mstate = NULL;
    int err = vmaf_metal_state_init(&mstate, mcfg);
    if (err != 0 || mstate == NULL) {
        (void)fprintf(stderr, "[skip: no Metal device] ");
        *skipped = 1;
        return NULL;
    }

    VmafFeatureDictionary *opts = NULL;
    err = build_hrs_opts(&opts, hrs_val);
    if (err) {
        vmaf_metal_state_free(&mstate);
        return "Metal: vmaf_feature_dictionary_set(cambi_high_res_speedup) failed";
    }

    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    err = vmaf_init(&vmaf, cfg);
    mu_assert("Metal: vmaf_init failed", !err);

    err = vmaf_metal_import_state(vmaf, mstate);
    mu_assert("Metal: vmaf_metal_import_state failed", !err);

    err = vmaf_use_feature(vmaf, "integer_cambi_metal", opts);
    mu_assert("Metal: vmaf_use_feature(integer_cambi_metal) failed", !err);

    char *msg = feed_one_frame(vmaf);
    if (msg) {
        (void)vmaf_close(vmaf);
        vmaf_metal_state_free(&mstate);
        return msg;
    }

    err = vmaf_feature_score_at_index(vmaf, feature_score_name, out_score, 0u);
    mu_assert("Metal: vmaf_feature_score_at_index(cambi) failed", !err);
    err = vmaf_close(vmaf);
    mu_assert("Metal: vmaf_close failed", !err);

    vmaf_metal_state_free(&mstate);
    return NULL;
}

static char *run_cpu_cambi(double *out_score)
{
    return run_cpu_cambi_opts(NULL, "Cambi_feature_cambi_score", out_score);
}

static char *run_metal_cambi(double *out_score, int *skipped)
{
    return run_metal_cambi_opts(NULL, "Cambi_feature_cambi_score", out_score, skipped);
}

static char *test_cambi_cpu_metal_parity(void)
{
    double cpu_score = 0.0;
    double metal_score = NAN;
    int skipped = 0;

    char *msg = run_cpu_cambi(&cpu_score);
    if (msg)
        return msg;
    msg = run_metal_cambi(&metal_score, &skipped);
    if (msg)
        return msg;
    if (skipped)
        return NULL;

    const double delta = fabs(cpu_score - metal_score);
    if (delta > PARITY_TOL) {
        (void)fprintf(stderr, "\ncambi parity FAIL: cpu=%.8f metal=%.8f delta=%.2e tol=%.2e\n",
                      cpu_score, metal_score, delta, PARITY_TOL);
    }
    mu_assert("cambi CPU vs. Metal delta exceeds places=4 tolerance (1e-4)", delta <= PARITY_TOL);
    return NULL;
}

/* Option-table plumbing test for `cambi_high_res_speedup`.
 *
 * What this covers: `cambi_high_res_speedup` is a VMAF_OPT_FLAG_FEATURE_PARAM,
 * so setting it renames the collector key from `cambi` to `cambi_hrs_1080`.
 * That key is exactly what `vmaf_use_features_from_model()` looks up for the
 * default model, and it is what the Metal twin could not emit before this
 * change. Both backends must accept the option and land on the same key and
 * the same score.
 *
 * What this does NOT cover: the fixture is 256x256, far below the 1920*1080
 * pixel threshold, so `hrs=1080` resolves to *inactive* in both `cambi.c` and
 * the Metal twin — the halved window and the extra pre-scale-0 decimation are
 * not exercised here. The sibling CUDA/SYCL parity tests have the same limit
 * (`core/test/test_cuda_cambi_parity.c`); real >= 1080p hrs parity is covered
 * by the cross-backend sweep, not by this unit test. */
static char *test_cambi_cpu_metal_hrs_parity(void)
{
    double cpu_score = 0.0;
    double metal_score = NAN;
    int skipped = 0;

    const char *feature_score_name = "cambi_hrs_1080";
    char *msg = run_cpu_cambi_opts("1080", feature_score_name, &cpu_score);
    if (msg)
        return msg;
    msg = run_metal_cambi_opts("1080", feature_score_name, &metal_score, &skipped);
    if (msg)
        return msg;
    if (skipped)
        return NULL;

    const double delta = fabs(cpu_score - metal_score);
    if (delta > PARITY_TOL) {
        (void)fprintf(stderr, "\ncambi hrs parity FAIL: cpu=%.8f metal=%.8f delta=%.2e tol=%.2e\n",
                      cpu_score, metal_score, delta, PARITY_TOL);
    }
    mu_assert("cambi hrs CPU vs. Metal delta exceeds places=4 tolerance (1e-4)",
              delta <= PARITY_TOL);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_cambi_metal_registered);
    mu_run_test(test_cambi_cpu_metal_parity);
    mu_run_test(test_cambi_cpu_metal_hrs_parity);
    return NULL;
}
