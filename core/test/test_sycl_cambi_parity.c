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
 * SYCL parity round 5 — CAMBI CPU vs. SYCL parity (ADR-1001).
 *
 * CAMBI (Contrast-Aware Multiscale Banding Index) is implemented by
 * cambi.c (CPU scalar / AVX2 / AVX-512 / NEON) and by
 * integer_cambi_sycl.cpp::vmaf_fex_cambi_sycl (SYCL GPU port of the
 * multi-scale banding detector: luminance masking, topk
 * accumulation, temporal integration).  ADR-0049 § "bit-for-bit" notes
 * the SYCL kernel is designed to match the CPU integer path.
 *
 * Prior state: test_integer_cambi_sycl.c was a smoke test only (the
 * file itself says "This is NOT a numerical-correctness test").  ADR-0957
 * claimed 16/18 registered SYCL extractors had active parity tests,
 * correctly excluding the two dormant SpEED twins but incorrectly
 * treating the smoke test as equivalent to a parity gate.  This test
 * fills that gap.
 *
 * Fixture: 256×256 — the CAMBI minimum is 216×216 (both dimensions;
 * see CAMBI_MIN_WIDTH_HEIGHT in cambi.c).  A quantised gradient pattern
 * (8-step / 32-pixel bands) is used so that CAMBI sees a non-trivial
 * banding score on both backends rather than a degenerate all-zero path.
 * Using identical ref and dist salts would collapse CAMBI to zero; using
 * different salts ensures a non-zero headline score.
 *
 * Tolerance: 1e-4 (places=4) — matches the ADR-0214 default for integer
 * kernels on Arc A380.  The SYCL kernel uses 32-bit integer accumulation
 * rather than 64-bit, consistent with the CUDA twin; single-frame
 * rounding stays within the places=4 bound empirically.
 *
 * Skip behaviour: if vmaf_sycl_state_init() fails (no oneAPI runtime or
 * no device visible) the test emits "[skip: no SYCL device]" and passes,
 * mirroring test_sycl_motion3_parity.c and all subsequent round tests.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test.h"

#include "feature/feature_extractor.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_sycl.h"
#include "libvmaf/picture.h"

/* CAMBI minimum: 216×216.  Use 256×256 so both dimensions clear the
 * threshold and the cost stays small for CI. */
#define FIXTURE_W 256u
#define FIXTURE_H 256u
#define FIXTURE_BPC 8u
/* places=4 — ADR-0214 default for integer kernels. */
#define PARITY_TOL 1e-4

static int fill_pic(VmafPicture *pic, unsigned salt)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;
    /* 8-step quantised gradient (32-px bands) — a textbook banding
     * artefact that drives CAMBI to a non-zero score. */
    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned row = 0u; row < pic->h[0]; row++) {
        for (unsigned col = 0u; col < pic->w[0]; col++) {
            const unsigned base = (col / 32u) * 32u + salt * 4u;
            y[row * pic->stride[0] + col] = (uint8_t)(base & 0xFFu);
        }
    }
    /* Flat neutral chroma — CAMBI is luma-only. */
    for (unsigned p = 1u; p < 3u; p++) {
        uint8_t *plane = (uint8_t *)pic->data[p];
        for (unsigned row = 0u; row < pic->h[p]; row++) {
            memset(plane + row * pic->stride[p], 128, pic->w[p]);
        }
    }
    return 0;
}

/* Feed one ref/dist pair then flush with an EOS marker. */
static char *feed_one_frame(VmafContext *vmaf, unsigned ref_salt, unsigned dist_salt)
{
    VmafPicture ref;
    VmafPicture dist;
    int err = fill_pic(&ref, ref_salt);
    mu_assert("fill_pic(ref) failed", !err);
    err = fill_pic(&dist, dist_salt);
    if (err) {
        vmaf_picture_unref(&ref);
        mu_assert("fill_pic(dist) failed", !err);
    }
    err = vmaf_read_pictures(vmaf, &ref, &dist, 0u);
    mu_assert("vmaf_read_pictures failed", !err);
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("vmaf_read_pictures(EOS) failed", !err);
    return NULL;
}

static char *run_cpu_cambi(double *score)
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    int err = vmaf_init(&vmaf, cfg);
    mu_assert("CPU: vmaf_init failed", !err);
    err = vmaf_use_feature(vmaf, "cambi", NULL);
    mu_assert("CPU: vmaf_use_feature(cambi) failed", !err);
    char *msg = feed_one_frame(vmaf, 0u, 1u);
    if (msg)
        return msg;
    err = vmaf_feature_score_at_index(vmaf, "Cambi_feature_cambi_score", score, 0u);
    mu_assert("CPU: Cambi_feature_cambi_score missing", !err);
    err = vmaf_close(vmaf);
    mu_assert("CPU: vmaf_close failed", !err);
    return NULL;
}

static char *run_sycl_cambi(double *score, int *device_present)
{
    *score = NAN;
    *device_present = 0;
    VmafSyclState *sycl_state = NULL;
    VmafSyclConfiguration sycl_cfg = {.device_index = -1};
    int err = vmaf_sycl_state_init(&sycl_state, sycl_cfg);
    if (err != 0 || sycl_state == NULL) {
        (void)fprintf(stderr, "[skip: no SYCL device] ");
        return NULL;
    }
    *device_present = 1;
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    err = vmaf_init(&vmaf, cfg);
    mu_assert("SYCL: vmaf_init failed", !err);
    err = vmaf_sycl_import_state(vmaf, sycl_state);
    mu_assert("SYCL: vmaf_sycl_import_state failed", !err);
    err = vmaf_use_feature(vmaf, "cambi_sycl", NULL);
    mu_assert("SYCL: vmaf_use_feature(cambi_sycl) failed", !err);
    char *msg = feed_one_frame(vmaf, 0u, 1u);
    if (msg) {
        (void)vmaf_close(vmaf);
        vmaf_sycl_state_free(&sycl_state);
        return msg;
    }
    err = vmaf_feature_score_at_index(vmaf, "Cambi_feature_cambi_score", score, 0u);
    mu_assert("SYCL: Cambi_feature_cambi_score missing", !err);
    err = vmaf_close(vmaf);
    mu_assert("SYCL: vmaf_close failed", !err);
    vmaf_sycl_state_free(&sycl_state);
    return NULL;
}

static char *test_cambi_sycl_registered(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("cambi_sycl");
    mu_assert("cambi_sycl extractor must be registered", fex != NULL);
    mu_assert("cambi_sycl name matches", !strcmp(fex->name, "cambi_sycl"));
    return NULL;
}

static char *test_cambi_cpu_sycl_parity(void)
{
    double cpu_score = 0.0;
    double sycl_score = NAN;
    int device_present = 0;

    char *msg = run_cpu_cambi(&cpu_score);
    if (msg)
        return msg;
    msg = run_sycl_cambi(&sycl_score, &device_present);
    if (msg)
        return msg;
    if (!device_present)
        return NULL;

    const double delta = fabs(cpu_score - sycl_score);
    if (delta > PARITY_TOL) {
        (void)fprintf(stderr, "\ncambi parity FAIL: cpu=%.8f sycl=%.8f delta=%.2e tol=%.2e\n",
                      cpu_score, sycl_score, delta, PARITY_TOL);
    }
    mu_assert("cambi CPU vs. SYCL delta exceeds ADR-0214 places=4 tolerance (1e-4)",
              delta <= PARITY_TOL);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_cambi_sycl_registered);
    mu_run_test(test_cambi_cpu_sycl_parity);
    return NULL;
}
