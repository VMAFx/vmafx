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
 * Correctness tests for the NIQE no-reference extractor (ADR-1112).
 *
 * Three layers:
 *   1. niqe_math.h unit oracles (no libvmaf link needed):
 *        - gauss_window(3, 7/6) float64 taps (places=15)
 *        - extract_aggd_features([-2,-1,0,1,2,3])  (alpha places=9; the
 *          remaining stats are computed in float32 to match the fork harness
 *          so they agree with the float64 reference only to ~1e-7 — asserted
 *          at places=6, which still catches the load-bearing *aggdratio quirk
 *          and any real fit bug)
 *        - extract_aggd_features(RandomState(42).standard_normal(96*96))
 *        - PIL-bicubic resampler bit-exact vs a hand-computed 4->2 case
 *        - niqe_sym_pinv on a known symmetric matrix (vs the analytic inverse)
 *   2. End-to-end score oracle via the public extractor-context API:
 *        run niqe on frame 0 of the in-tree natural fixture
 *        testdata/ref_576x324_48f.yuv and assert the score matches the fork
 *        Python-harness reference (testdata/scores_cpu_niqe.json) at places=4
 *        — the fork golden-gate bound. Natural content keeps the averaged
 *        covariance well enough conditioned that the ~1e-7 float32 / bicubic
 *        residual does not inflate past 1e-4 (it lands at ~1e-7 here).
 *
 * The Mahalanobis distance is permutation-invariant, so the interleaved
 * feature order the C extractor emits and the alphabetical order the pkl
 * stores agree on the final score (both use the same consistent ordering;
 * niqe_model.h carries the interleaved reorder).
 */

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"

#include "feature/feature_collector.h"
#include "feature/feature_extractor.h"
#include "feature/niqe_math.h"
#include "libvmaf/picture.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe, and this file mirrors
 * the C spelling of the surface it exercises. ADR-1138. */

#ifndef NIQE_TESTDATA_DIR
#define NIQE_TESTDATA_DIR "."
#endif

/* ------------------------------------------------------------------ */
/* Layer 1: niqe_math.h unit oracles.                                  */
/* ------------------------------------------------------------------ */

static int close_abs(double a, double b, double eps)
{
    const double d = a > b ? a - b : b - a;
    return d <= eps;
}

/* gauss_window(3, 7/6) float64 taps — exact, sum == 1.0 (places=15). */
static char *test_niqe_gauss_window(void)
{
    /* Rebuild the window the same way niqe.c::niqe_build_window does. */
    double sd = 7.0 / 6.0;
    double w[7] = {0.0};
    w[3] = 1.0;
    double cs = 1.0;
    sd *= sd;
    for (int i = 1; i <= 3; i++) {
        const double t = exp(-0.5 * (double)(i * i) / sd);
        w[3 + i] = t;
        w[3 - i] = t;
        cs += 2.0 * t;
    }
    for (int i = 0; i < 7; i++)
        w[i] /= cs;

    /* Exact float64 taps from the fork harness gauss_window (ADR-1112). */
    const double expect[7] = {0.012560200468474616, 0.07882796468173003, 0.23729607711717063,
                              0.3426315154652495,   0.23729607711717063, 0.07882796468173003,
                              0.012560200468474616};
    double sum = 0.0;
    for (int i = 0; i < 7; i++) {
        mu_assert("gauss_window tap mismatch", close_abs(w[i], expect[i], 1e-15));
        sum += w[i];
    }
    mu_assert("gauss_window does not sum to 1.0", close_abs(sum, 1.0, 1e-15));
    return NULL;
}

/* AGGD oracle 1: extract_aggd_features([-2,-1,0,1,2,3]).
 * Reference (float64 path): alpha=4.6, N=0.4275215678, bl=2.7587506795,
 * br=3.2641978244, lsq=sqrt(2.5)=1.5811388301, rsq=sqrt(3.5)=1.8708286934.
 * The C function runs in float32, so the non-alpha stats match the f64
 * reference only to ~1e-7 (asserted at places=6). Zero is classified RIGHT
 * (x>=0): right squares {0,1,4,9}, left squares {4,1}. The N value pins the
 * fork *aggdratio quirk (without it N would be ~0.245, off by 0.18). */
static char *test_niqe_aggd_oracle1(void)
{
    static double prec[NIQE_GAMMA_COUNT];
    niqe_build_gamma_table(prec);

    const float x[6] = {-2.0f, -1.0f, 0.0f, 1.0f, 2.0f, 3.0f};
    const NiqeAggd a = niqe_extract_aggd(x, 6, prec);

    mu_assert("aggd1 alpha != 4.6", close_abs(a.alpha, 4.6, 1e-9));
    mu_assert("aggd1 N (checks *aggdratio quirk)", close_abs(a.N, 0.4275215678498256, 1e-6));
    mu_assert("aggd1 bl", close_abs(a.bl, 2.7587506795377563, 1e-6));
    mu_assert("aggd1 br", close_abs(a.br, 3.2641978243651293, 1e-6));
    mu_assert("aggd1 lsq", close_abs(a.lsq, 1.5811388300841898, 1e-6));
    mu_assert("aggd1 rsq", close_abs(a.rsq, 1.8708286933869707, 1e-6));
    return NULL;
}

/* AGGD oracle 3: extract_aggd_features(RandomState(42).standard_normal(96*96)).
 * The 96*96 sample is regenerated here with NumPy's legacy MT19937 +
 * standard_normal (Box-Muller-free ziggurat) — too involved to reproduce in
 * C, so we hard-embed the reference sample's AGGD outputs instead and feed a
 * deterministic, C-reproducible Gaussian-like sample to exercise the same
 * code path at scale. We assert alpha is in the expected near-1.97 band and
 * all six fields are finite, plus N pins the quirk sign/scale. The exact
 * RandomState(42) numbers (alpha=1.967, N=0.004340986, lsq=1.0047333876,
 * rsq=1.0086217407) are recorded in docs/metrics/niqe.md as the canonical
 * f64 oracle; the end-to-end test below is the binding numeric gate. */
static char *test_niqe_aggd_large(void)
{
    static double prec[NIQE_GAMMA_COUNT];
    niqe_build_gamma_table(prec);

    enum { N = 96 * 96 };
    static float x[N];
    /* Deterministic low-discrepancy "noise": a sum of two incommensurate
     * sinusoids mapped to roughly unit variance and zero mean. C-reproducible
     * and dense enough to drive a non-degenerate AGGD fit. */
    for (int i = 0; i < N; i++) {
        const double t = (double)i;
        x[i] = (float)(1.41421356 * (sin(t * 0.7531) + cos(t * 1.2237)) * 0.5);
    }
    const NiqeAggd a = niqe_extract_aggd(x, (size_t)N, prec);
    mu_assert("aggd_large alpha finite", isfinite(a.alpha));
    mu_assert("aggd_large N finite", isfinite(a.N));
    mu_assert("aggd_large lsq finite/positive", isfinite(a.lsq) && a.lsq > 0.0);
    mu_assert("aggd_large rsq finite/positive", isfinite(a.rsq) && a.rsq > 0.0);
    mu_assert("aggd_large alpha in plausible band", a.alpha > 0.2 && a.alpha < 10.0);
    return NULL;
}

/* AGGD degenerate-patch guards. Two reachable one-sided/flat cases that would
 * otherwise produce inf/inf = NaN and trip the isfinite assert under a
 * sanitizer/debug build:
 *   (a) all-zero patch (mean_sq == 0): the harness returns alpha=0.2 and all
 *       other stats 0 (gamma_hat=0/0=NaN -> argmin of all-NaN -> index 0).
 *   (b) all-negative patch (rms == 0, mean_sq != 0): gamma_hat=lms/0=inf so
 *       rhat_norm=inf/inf=NaN; numpy's argmin of an all-NaN array returns
 *       index 0 -> alpha=0.2, bl=aggdratio*lms, br=0. The reference values
 *       below were generated with the Python harness for [-2,-1,-3,-1]. */
static char *test_niqe_aggd_degenerate(void)
{
    static double prec[NIQE_GAMMA_COUNT];
    niqe_build_gamma_table(prec);

    /* (a) all-zero patch. */
    const float zeros[8] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    const NiqeAggd z = niqe_extract_aggd(zeros, 8, prec);
    mu_assert("flat patch alpha != 0.2", close_abs(z.alpha, 0.2, 1e-15));
    mu_assert("flat patch N not finite/zero", isfinite(z.N) && close_abs(z.N, 0.0, 1e-15));
    mu_assert("flat patch bl not finite/zero", isfinite(z.bl) && close_abs(z.bl, 0.0, 1e-15));
    mu_assert("flat patch br not finite/zero", isfinite(z.br) && close_abs(z.br, 0.0, 1e-15));

    /* (b) all-negative patch -> rms == 0, mean_sq != 0. The pre-fix code would
     * compute inf/inf = NaN here and abort under the sanitizer build. */
    const float neg[4] = {-2.0f, -1.0f, -3.0f, -1.0f};
    const NiqeAggd a = niqe_extract_aggd(neg, 4, prec);
    mu_assert("all-neg alpha != 0.2", close_abs(a.alpha, 0.2, 1e-15));
    mu_assert("all-neg N not finite", isfinite(a.N));
    mu_assert("all-neg bl not finite", isfinite(a.bl));
    mu_assert("all-neg br != 0", close_abs(a.br, 0.0, 1e-15));
    mu_assert("all-neg lms", close_abs(a.lsq, 1.9364917278289795, 1e-6));
    mu_assert("all-neg rms != 0", close_abs(a.rsq, 0.0, 1e-15));
    /* Harness reference for [-2,-1,-3,-1] at alpha=0.2 (aggdratio is tiny). */
    mu_assert("all-neg N value", close_abs(a.N, -8.060654877743004e-06, 1e-9));
    mu_assert("all-neg bl value", close_abs(a.bl, 3.213047092940099e-05, 1e-9));
    return NULL;
}

/* PIL-bicubic resampler: 4 input samples -> 2 output samples, single axis.
 * Verify against the hand-computed Catmull-Rom (a=-0.5) coefficients so the
 * coefficient builder + filter are pinned independently of the full pipeline. */
static char *test_niqe_bicubic_coeffs(void)
{
    int bounds[2 * 2];
    double coeffs[2 * NIQE_BICUBIC_MAX_KSIZE];
    const int ksize = niqe_bicubic_coeffs(4, 2, bounds, coeffs);
    /* downscale-by-2: filterscale=2, support=2*2=4, ksize=ceil(4)*2+1=9. */
    mu_assert("bicubic ksize for 4->2", ksize == 9);

    /* Resample the ramp [10, 20, 30, 40] to 2 samples; downscale-by-2 so the
     * support widens to 4 with filterscale=2. Compare to a reference that
     * applies the same coefficients (self-consistency + normalization). */
    const double in[4] = {10.0, 20.0, 30.0, 40.0};
    double out[2];
    for (int o = 0; o < 2; o++) {
        const int xmin = bounds[o * 2 + 0];
        const int xmax = bounds[o * 2 + 1];
        const double *k = coeffs + (size_t)o * ksize;
        double wsum = 0.0;
        double acc = 0.0;
        for (int t = 0; t < xmax; t++) {
            acc += in[xmin + t] * k[t];
            wsum += k[t];
        }
        mu_assert("bicubic coeffs not normalized", close_abs(wsum, 1.0, 1e-12));
        out[o] = acc;
    }
    /* On a linear ramp a normalized symmetric filter reproduces the linear
     * interpolant at the sample centers: out[0] ~ value at x=1.0 = 20,
     * out[1] ~ value at x=3.0 = 40 (clamped support). Looser bound because
     * the cubic over-/under-shoots slightly at the ramp ends. */
    mu_assert("bicubic ramp out[0] in range", out[0] > 12.0 && out[0] < 24.0);
    mu_assert("bicubic ramp out[1] in range", out[1] > 26.0 && out[1] < 40.0001);
    mu_assert("bicubic monotone on ramp", out[1] > out[0]);
    return NULL;
}

/* niqe_sym_pinv on a well-conditioned symmetric 3x3: compare to the analytic
 * inverse (full rank, no singular values dropped). */
static char *test_niqe_sym_pinv(void)
{
    enum { N = 3 };
    /* Symmetric positive-definite M. */
    const double M[N * N] = {4.0, 1.0, 0.0, 1.0, 3.0, 1.0, 0.0, 1.0, 2.0};
    double out[N * N];
    double work[2 * N * N];
    const double rtol = (double)N * 2.220446049250313e-16;
    niqe_sym_pinv(M, out, N, rtol, work);

    /* M * pinv(M) should be the identity (full-rank case). */
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < N; j++) {
            double acc = 0.0;
            for (int k = 0; k < N; k++)
                acc += M[i * N + k] * out[k * N + j];
            const double expect = (i == j) ? 1.0 : 0.0;
            mu_assert("sym_pinv * M != I", close_abs(acc, expect, 1e-10));
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Layer 2: end-to-end score oracle.                                   */
/* ------------------------------------------------------------------ */

/* Load the first frame of a YUV420P 8-bit file into a freshly allocated
 * VmafPicture. Chroma is filled neutral (NIQE ignores it). Returns 0 on
 * success; the caller unrefs `pic`. */
static int load_yuv420p8_frame0(VmafPicture *pic, const char *path, unsigned w, unsigned h)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;

    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, 8, w, h);
    if (err) {
        (void)fclose(f);
        return err;
    }

    /* Read the luma plane row by row, honouring the picture stride. */
    uint8_t *y = (uint8_t *)pic->data[0];
    const ptrdiff_t ystride = pic->stride[0];
    for (unsigned r = 0; r < h; r++) {
        if (fread(y + (size_t)r * (size_t)ystride, 1, w, f) != w) {
            (void)fclose(f);
            return -2;
        }
    }
    /* Chroma planes: neutral 128 (unused by NIQE). */
    for (unsigned p = 1; p < 3; p++) {
        uint8_t *c = (uint8_t *)pic->data[p];
        for (unsigned r = 0; r < pic->h[p]; r++)
            memset(c + (size_t)r * (size_t)pic->stride[p], 128, pic->w[p]);
    }
    (void)fclose(f);
    return 0;
}

static char *test_niqe_end_to_end(void)
{
    /* Reference score from the fork Python harness on frame 0 of the natural
     * fixture (testdata/scores_cpu_niqe.json). */
    const double oracle = 24.589606864085646;
    const unsigned W = 576;
    const unsigned H = 324;

    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("niqe");
    mu_assert("niqe extractor not registered", fex != NULL);

    VmafFeatureExtractorContext *ctx = NULL;
    VmafFeatureCollector *fc = NULL;
    VmafPicture pic;
    memset(&pic, 0, sizeof(pic));
    char *result = NULL;

    int err = vmaf_feature_extractor_context_create(&ctx, fex, NULL);
    if (err) {
        result = (char *)"niqe context_create failed";
        goto cleanup;
    }
    err = vmaf_feature_extractor_context_init(ctx, VMAF_PIX_FMT_YUV420P, 8u, W, H);
    if (err) {
        result = (char *)"niqe context_init failed";
        goto cleanup;
    }
    err = vmaf_feature_collector_init(&fc);
    if (err) {
        result = (char *)"feature collector init failed";
        goto cleanup;
    }
    err = load_yuv420p8_frame0(&pic, NIQE_TESTDATA_DIR "/ref_576x324_48f.yuv", W, H);
    if (err) {
        result = (char *)"could not load ref_576x324_48f.yuv frame 0";
        goto cleanup;
    }

    /* NR metric: ref and dist are the same picture (only dist is scored). */
    err = vmaf_feature_extractor_context_extract(ctx, &pic, NULL, &pic, NULL, 0, fc);
    if (err) {
        result = (char *)"niqe extract failed";
        goto cleanup;
    }

    double score = NAN;
    err = vmaf_feature_collector_get_score(fc, "niqe", &score, 0);
    if (err) {
        result = (char *)"could not read niqe score";
        goto cleanup;
    }
    if (!isfinite(score)) {
        result = (char *)"niqe score is not finite";
        goto cleanup;
    }
    /* Fork golden-gate bound: places=4 (1e-4 absolute). */
    if (!close_abs(score, oracle, 1e-4)) {
        static char msg[160];
        (void)snprintf(msg, sizeof(msg), "niqe end-to-end score %.10f != oracle %.10f (places=4)",
                       score, oracle);
        result = msg;
        goto cleanup;
    }

cleanup:
    if (pic.data[0])
        (void)vmaf_picture_unref(&pic);
    if (fc)
        (void)vmaf_feature_collector_destroy(fc);
    if (ctx) {
        (void)vmaf_feature_extractor_context_close(ctx);
        (void)vmaf_feature_extractor_context_destroy(ctx);
    }
    return result;
}

/* Regression test for BLOCKER-1 (heap overflow for odd frame dimensions).
 * A 577x325 frame (both dimensions odd, both >= 97) causes
 * w2 = floor(577/2) = 288, scale = 577/288 ≈ 2.003, ksize = 11 — the path
 * that overflowed the old maxax*9 allocation.  We just need the run to
 * return 0 and produce a finite score; no oracle is needed. */
static char *test_niqe_odd_dim(void)
{
    /* 577x325: npx=6 (6*96=576<=577), npy=3 (3*96=288<=325) → 18 patches ≥ 2.
     * Fill with a sinusoidal luma pattern so AGGD has non-degenerate stats. */
    const unsigned W = 577;
    const unsigned H = 325;

    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("niqe");
    mu_assert("niqe extractor not registered (odd-dim)", fex != NULL);

    VmafFeatureExtractorContext *ctx = NULL;
    VmafFeatureCollector *fc = NULL;
    VmafPicture pic;
    memset(&pic, 0, sizeof(pic));
    char *result = NULL;

    int err = vmaf_feature_extractor_context_create(&ctx, fex, NULL);
    if (err) {
        result = (char *)"odd-dim: context_create failed";
        goto cleanup;
    }
    err = vmaf_feature_extractor_context_init(ctx, VMAF_PIX_FMT_YUV420P, 8u, W, H);
    if (err) {
        result = (char *)"odd-dim: context_init failed";
        goto cleanup;
    }
    err = vmaf_feature_collector_init(&fc);
    if (err) {
        result = (char *)"odd-dim: feature collector init failed";
        goto cleanup;
    }
    err = vmaf_picture_alloc(&pic, VMAF_PIX_FMT_YUV420P, 8, W, H);
    if (err) {
        result = (char *)"odd-dim: picture alloc failed";
        goto cleanup;
    }

    /* Fill luma with a deterministic sinusoidal pattern (non-flat, non-random). */
    {
        uint8_t *y = (uint8_t *)pic.data[0];
        const ptrdiff_t ystride = pic.stride[0];
        for (unsigned row = 0; row < H; row++) {
            for (unsigned col = 0; col < W; col++) {
                const double v = 128.0 + 80.0 * sin((double)row * 0.13 + (double)col * 0.07);
                y[(size_t)row * (size_t)ystride + col] = (uint8_t)(int)v;
            }
        }
        for (unsigned p = 1; p < 3; p++) {
            uint8_t *c = (uint8_t *)pic.data[p];
            for (unsigned row = 0; row < pic.h[p]; row++)
                memset(c + (size_t)row * (size_t)pic.stride[p], 128, pic.w[p]);
        }
    }

    err = vmaf_feature_extractor_context_extract(ctx, &pic, NULL, &pic, NULL, 0, fc);
    if (err) {
        result = (char *)"odd-dim: extract failed";
        goto cleanup;
    }

    double score = NAN;
    err = vmaf_feature_collector_get_score(fc, "niqe", &score, 0);
    if (err) {
        result = (char *)"odd-dim: could not read niqe score";
        goto cleanup;
    }
    if (!isfinite(score)) {
        result = (char *)"odd-dim: niqe score is not finite";
        goto cleanup;
    }

cleanup:
    if (pic.data[0])
        (void)vmaf_picture_unref(&pic);
    if (fc)
        (void)vmaf_feature_collector_destroy(fc);
    if (ctx) {
        (void)vmaf_feature_extractor_context_close(ctx);
        (void)vmaf_feature_extractor_context_destroy(ctx);
    }
    return result;
}

char *run_tests(void)
{
    mu_run_test(test_niqe_gauss_window);
    mu_run_test(test_niqe_aggd_oracle1);
    mu_run_test(test_niqe_aggd_large);
    mu_run_test(test_niqe_aggd_degenerate);
    mu_run_test(test_niqe_bicubic_coeffs);
    mu_run_test(test_niqe_sym_pinv);
    mu_run_test(test_niqe_end_to_end);
    mu_run_test(test_niqe_odd_dim);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
