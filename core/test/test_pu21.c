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
 * PU21 correctness test against the verified design dossier's worked oracle
 * (.workingdir2/rc/metrics/pu21.md). All values were independently re-derived
 * in fp64 by the adversarial verifier (encoder to ~1e-11, PU-PSNR exact).
 *
 * Includes pu21.c directly to reach the static encoder + PU-PSNR helpers
 * (mirrors test_ciede.c / test_psnr.c). pu21_ssim.c + the iqa convolve helper
 * are linked via meson (see core/test/meson.build).
 */

#include <math.h>

#include "test.h"
#include "feature/pu21.c"

/* places=4 → tolerance 5e-5 (the fork's non-negotiable golden tolerance).
 * The encoder points are asserted at places=6 (5e-7) per the dossier. */
#define EPS_6 5e-7
#define EPS_4 5e-5

static int almost_equal(double a, double b, double eps)
{
    const double diff = a > b ? a - b : b - a;
    return diff < eps;
}

/* Encoder oracle: banding_glare variant, the 6 worked points from the dossier
 * test vector. Encoding is in fp64 (matches the MATLAB reference). */
static char *test_pu21_encode_banding_glare(void)
{
    const double *p = pu21_params[PU21_VARIANT_BANDING_GLARE];

    struct {
        double y;
        double v;
    } cases[] = {
        {0.005, 0.0000000005},   {0.1, 5.7170738397},      {1.0, 36.5439111394},
        {100.0, 256.3838973127}, {1000.0, 420.0969213492}, {10000.0, 595.3939200201},
    };

    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const double y = pu21_clamp_luminance(cases[i].y);
        const double v = pu21_encode(y, p);
        mu_assert("pu21_encode (banding_glare) mismatch vs dossier oracle",
                  almost_equal(v, cases[i].v, EPS_6));
    }

    /* Documented design anchor: 100 nit -> ~256 (8-bit SDR mimicry). */
    mu_assert("pu21_encode(100) should map to ~256",
              almost_equal(pu21_encode(pu21_clamp_luminance(100.0), p), 256.3838973127, EPS_6));

    return NULL;
}

/* Encoder oracle: a SECOND variant (peaks) at four in-domain luminances, to
 * exercise a coefficient row independent of banding_glare. Expected values
 * were derived from the verified pu21_encode formula and the peaks row of
 * pu21_params (fp64; cross-checked against the matlab/ffmpeg references). */
static char *test_pu21_encode_peaks(void)
{
    const double *p = pu21_params[PU21_VARIANT_PEAKS];

    struct {
        double y;
        double v;
    } cases[] = {
        {1.0, 85.5420149821},
        {100.0, 260.7249826119},
        {1000.0, 335.6947145978},
        {10000.0, 380.9853161220},
    };

    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const double y = pu21_clamp_luminance(cases[i].y);
        const double v = pu21_encode(y, p);
        mu_assert("pu21_encode (peaks) mismatch vs re-derived oracle",
                  almost_equal(v, cases[i].v, EPS_6));
    }

    return NULL;
}

/* End-to-end PU-PSNR oracle: 1x1 luma, V_ref/V_dist from enc(100)/enc(99).
 * MSE = (V_ref - V_dist)^2 = 0.42574156304610455;
 * PU-PSNR = 10*log10(256^2 / MSE) = 51.87333880351542 dB. */
static char *test_pu21_psnr_oracle(void)
{
    const double *p = pu21_params[PU21_VARIANT_BANDING_GLARE];

    /* Encoded planes are stored in double in production (matches the
     * maintainer constraint + the fp64 oracle); the test mirrors that. */
    const double v_ref = pu21_encode(pu21_clamp_luminance(100.0), p);
    const double v_dist = pu21_encode(pu21_clamp_luminance(99.0), p);

    const double score = pu21_compute_psnr(&v_ref, &v_dist, 1, 1);

    mu_assert("PU-PSNR(100,99) should be 51.873338803 dB",
              almost_equal(score, 51.87333880351542, EPS_4));

    return NULL;
}

/* MSE == 0 (identical planes) must yield a large finite dB, not +inf/NaN. */
static char *test_pu21_psnr_identical(void)
{
    const double a[4] = {256.0, 100.0, 50.0, 12.0};
    const double score = pu21_compute_psnr(a, a, 2, 2);
    mu_assert("PU-PSNR for identical planes must be finite", isfinite(score));
    mu_assert("PU-PSNR for identical planes must be large (>100 dB)", score > 100.0);

    return NULL;
}

/* PQ EOTF anchor: the peak code value (Ep=1.0) maps to 10000 cd/m^2. */
static char *test_pu21_pq_eotf(void)
{
    mu_assert("PQ EOTF peak should be 10000 nits",
              almost_equal(pu21_pq_eotf_nits(1.0), 10000.0, EPS_4));
    mu_assert("PQ EOTF at 0 should be 0 nits", almost_equal(pu21_pq_eotf_nits(0.0), 0.0, EPS_4));

    return NULL;
}

/* PU-SSIM self-consistency: identical planes score exactly 1.0.
 *
 * pu21_compute_ssim now takes a caller-owned, pre-allocated workspace (the
 * production hot-path discipline: no per-frame malloc); the tests allocate one
 * locally to mirror init(). */
static char *test_pu21_ssim_identical(void)
{
    const int w = 16;
    const int h = 16;
    double plane[16 * 16];
    for (int i = 0; i < w * h; i++)
        plane[i] = (double)(100 + (i % 7));

    struct pu21_ssim_workspace ws = {0};
    mu_assert("workspace alloc failed", pu21_ssim_workspace_alloc(&ws, (size_t)w * (size_t)h) == 0);

    double score = 0.0;
    int err = pu21_compute_ssim(&ws, plane, plane, w, h, PU21_SSIM_L, &score);
    pu21_ssim_workspace_free(&ws);
    mu_assert("pu21_compute_ssim returned error", err == 0);
    mu_assert("PU-SSIM of identical planes should be 1.0", almost_equal(score, 1.0, EPS_4));

    return NULL;
}

/* Minimum valid plane: w = h = GAUSSIAN_LEN (11). The valid interior region is
 * exactly 1x1, so SSIM must still return 0 and a finite score. */
static char *test_pu21_ssim_min_valid(void)
{
    /* 11 == GAUSSIAN_LEN, the 11x11 Gaussian window width; this is the
     * smallest plane with a non-empty (1x1) valid interior region. The literal
     * is used directly because pu21.c does not expose ssim_tools.h's
     * GAUSSIAN_LEN to this translation unit. */
    const int w = 11;
    const int h = 11;
    double ref[11 * 11];
    double dist[11 * 11];
    for (int i = 0; i < w * h; i++) {
        ref[i] = (double)(80 + (i % 13));
        dist[i] = (double)(80 + ((i + 1) % 13));
    }

    struct pu21_ssim_workspace ws = {0};
    mu_assert("workspace alloc failed", pu21_ssim_workspace_alloc(&ws, (size_t)w * (size_t)h) == 0);

    double score = -1.0;
    int err = pu21_compute_ssim(&ws, ref, dist, w, h, PU21_SSIM_L, &score);
    pu21_ssim_workspace_free(&ws);
    mu_assert("pu21_compute_ssim (11x11 minimum) should succeed", err == 0);
    mu_assert("PU-SSIM (11x11) must be finite", isfinite(score));

    return NULL;
}

/* Below-minimum plane: w = h = 10 (< GAUSSIAN_LEN). Must be rejected with
 * -EINVAL (no valid interior region exists for the 11x11 window). */
static char *test_pu21_ssim_reject_small(void)
{
    const int w = 10;
    const int h = 10;
    double plane[10 * 10];
    for (int i = 0; i < w * h; i++)
        plane[i] = (double)(100 + (i % 5));

    struct pu21_ssim_workspace ws = {0};
    mu_assert("workspace alloc failed", pu21_ssim_workspace_alloc(&ws, (size_t)w * (size_t)h) == 0);

    double score = 0.0;
    int err = pu21_compute_ssim(&ws, plane, plane, w, h, PU21_SSIM_L, &score);
    pu21_ssim_workspace_free(&ws);
    mu_assert("pu21_compute_ssim (10x10) must reject with -EINVAL", err == -EINVAL);

    return NULL;
}

/* Non-square plane (13x17): exercises the asymmetric reduced-stride geometry
 * (cw != ch) and the contiguous-packing reduction. Identical planes -> 1.0. */
static char *test_pu21_ssim_nonsquare(void)
{
    const int w = 13;
    const int h = 17;
    double plane[13 * 17];
    for (int i = 0; i < w * h; i++)
        plane[i] = (double)(120 + (i % 11));

    struct pu21_ssim_workspace ws = {0};
    mu_assert("workspace alloc failed", pu21_ssim_workspace_alloc(&ws, (size_t)w * (size_t)h) == 0);

    double score = 0.0;
    int err = pu21_compute_ssim(&ws, plane, plane, w, h, PU21_SSIM_L, &score);
    pu21_ssim_workspace_free(&ws);
    mu_assert("pu21_compute_ssim (13x17) should succeed", err == 0);
    mu_assert("PU-SSIM (13x17) must be finite", isfinite(score));
    mu_assert("PU-SSIM (13x17) of identical planes should be 1.0", almost_equal(score, 1.0, EPS_4));

    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_pu21_encode_banding_glare);
    mu_run_test(test_pu21_encode_peaks);
    mu_run_test(test_pu21_psnr_oracle);
    mu_run_test(test_pu21_psnr_identical);
    mu_run_test(test_pu21_pq_eotf);
    mu_run_test(test_pu21_ssim_identical);
    mu_run_test(test_pu21_ssim_min_valid);
    mu_run_test(test_pu21_ssim_reject_small);
    mu_run_test(test_pu21_ssim_nonsquare);

    return NULL;
}
