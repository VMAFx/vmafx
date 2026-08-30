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
 * niqe_math.h — header-only numeric kernels for the NIQE no-reference
 * feature extractor (feature/niqe.c). Kept in a separate header so the
 * correctness unit test (test/test_niqe.c) can include and exercise the
 * AGGD fit, the gamma lookup table, the PIL-compatible bicubic resampler,
 * and the symmetric pseudo-inverse directly, mirroring the
 * `#include "feature/ciede.c"` test pattern but without dragging the whole
 * extractor lifecycle in.
 *
 * Every routine here is pure (no I/O, no global mutable state) and operates
 * in IEEE-754 double precision EXCEPT the per-patch AGGD statistics, which
 * are computed in float32 to match the fork harness
 * (NiqeNorefFeatureExtractor.mscn_extract_niqe casts the MSCN maps to
 * float32 before patch feature extraction — load-bearing for pkl parity,
 * see docs/adr/1112-niqe-nr-metric.md).
 */

#ifndef __VMAF_FEATURE_NIQE_MATH_H__
#define __VMAF_FEATURE_NIQE_MATH_H__

#include <assert.h>
#include <math.h>
#include <stddef.h>

/* ------------------------------------------------------------------ */
/* Gamma lookup table for the AGGD/GGD shape fit.                      */
/*                                                                     */
/* gamma_range = arange(0.2, 10, 0.001) -> 9800 entries.               */
/* prec_gammas[i] = gamma(2/g)^2 / (gamma(1/g) * gamma(3/g)).          */
/*                                                                     */
/* The table is monotonic over this range, so the argmin lookup in     */
/* niqe_aggd_alpha() is robust to the <1-ulp host variance of libm     */
/* tgamma() (the dominant parity risk is the bicubic resize + pinv,    */
/* not gamma — see the dossier).                                       */
/* ------------------------------------------------------------------ */

#define NIQE_GAMMA_COUNT 9800
#define NIQE_GAMMA_START 0.2
#define NIQE_GAMMA_STEP 0.001

/* Fill prec[] (NIQE_GAMMA_COUNT doubles) with the precomputed AGGD ratio
 * table. Caller owns the storage. */
static inline void niqe_build_gamma_table(double *prec)
{
    for (int i = 0; i < NIQE_GAMMA_COUNT; i++) {
        const double g = NIQE_GAMMA_START + (double)i * NIQE_GAMMA_STEP;
        const double g2 = tgamma(2.0 / g);
        prec[i] = (g2 * g2) / (tgamma(1.0 / g) * tgamma(3.0 / g));
    }
}

/* Map a table index to its gamma_range value. */
static inline double niqe_gamma_value(int idx)
{
    return NIQE_GAMMA_START + (double)idx * NIQE_GAMMA_STEP;
}

/* ------------------------------------------------------------------ */
/* AGGD (asymmetric generalized Gaussian) fit, float32 statistics.     */
/*                                                                     */
/* Mirrors NiqeNorefFeatureExtractor.extract_aggd_features:            */
/*   lms = sqrt(mean(x[x<0]^2)),  rms = sqrt(mean(x[x>=0]^2))          */
/*   gamma_hat = lms / rms                                             */
/*   r_hat = mean(|x|)^2 / mean(x^2)                                   */
/*   rhat_norm = r_hat * ((gh^3+1)(gh+1)) / ((gh^2+1)^2)               */
/*   alpha = gamma_range[argmin |prec_gammas - rhat_norm|]             */
/*   aggdratio = sqrt(gamma(1/alpha)) / sqrt(gamma(3/alpha))           */
/*   bl = aggdratio*lms,  br = aggdratio*rms                           */
/*   N  = (br-bl) * (gamma(2/alpha)/gamma(1/alpha)) * aggdratio        */
/*                                                                     */
/* NOTE the trailing *aggdratio in N is a FORK-SPECIFIC quirk: upstream */
/* scikit-video / MATLAB OMIT it; the fork pkl was trained WITH it, so  */
/* the C port keeps it (load-bearing, see ADR-1112).                    */
/*                                                                     */
/* The per-element reductions use float32 accumulators to match the     */
/* harness's m_image.astype(float32) round-trip; the gamma() tail runs  */
/* in double.                                                           */
/* ------------------------------------------------------------------ */

typedef struct NiqeAggd {
    double alpha;
    double N;
    double bl;
    double br;
    double lsq; /* left_mean_sqrt  (lms) */
    double rsq; /* right_mean_sqrt (rms) */
} NiqeAggd;

/* Fit AGGD parameters of the float32 sample buffer x[0..n). `prec` is the
 * gamma table from niqe_build_gamma_table(). Zeros are classified RIGHT
 * (the x >= 0 boundary, matching the harness). */
static inline NiqeAggd niqe_extract_aggd(const float *x, size_t n, const double *prec)
{
    assert(n > 0 && prec);
    /* float32 reductions (harness parity). */
    float left_sq_sum = 0.0f;
    float right_sq_sum = 0.0f;
    size_t left_cnt = 0;
    size_t right_cnt = 0;
    float abs_sum = 0.0f;
    float sq_sum = 0.0f;

    for (size_t i = 0; i < n; i++) {
        const float v = x[i];
        const float v2 = v * v;
        sq_sum += v2;
        abs_sum += (v < 0.0f) ? -v : v;
        if (v < 0.0f) {
            left_sq_sum += v2;
            left_cnt++;
        } else {
            right_sq_sum += v2;
            right_cnt++;
        }
    }

    const float lms = (left_cnt > 0) ? sqrtf(left_sq_sum / (float)left_cnt) : 0.0f;
    const float rms = (right_cnt > 0) ? sqrtf(right_sq_sum / (float)right_cnt) : 0.0f;
    const float mean_abs = abs_sum / (float)n;
    const float mean_sq = sq_sum / (float)n;

    /* Flat patch (mean_sq == 0): 0/0 would produce NaN.  Return the alpha
     * sentinel 0.2f (the lower bound of gamma_range in the Python harness),
     * which the table argmin would also select for a degenerate ratio. */
    if (mean_sq == 0.0f) {
        NiqeAggd flat = {0.2, 0.0, 0.0, 0.0, 0.0, 0.0};
        return flat;
    }

    /* Degenerate one-sided patch (rms == 0 but mean_sq != 0, i.e. all samples
     * negative).  gamma_hat = lms/0 = +inf, so rhat_norm = r_hat*(inf/inf) =
     * NaN.  The Python harness divides with numpy scalars (lms/0 -> inf, not an
     * exception) and feeds the resulting NaN into np.argmin(np.abs(prec - NaN)),
     * which returns index 0 for an all-NaN array.  Mirror that exactly here:
     * skip the inf/inf arithmetic and the argmin, and select gamma index 0
     * (alpha = 0.2).  The gamma tail below then yields bl = aggdratio*lms,
     * br = aggdratio*rms = 0, N = (br-bl)*(g2/g1)*aggdratio — harness-faithful.
     * This guards a sanitizer/debug abort on a reachable all-negative MSCN
     * patch; the RELEASE output is unchanged (this only fires when rms == 0). */
    int best;
    if (rms == 0.0f) {
        best = 0;
    } else {
        const float gamma_hat = lms / rms;
        const float r_hat = (mean_abs * mean_abs) / mean_sq;
        const float gh2 = gamma_hat * gamma_hat;
        const float gh3 = gh2 * gamma_hat;
        const float denom = (gh2 + 1.0f) * (gh2 + 1.0f);
        const float rhat_norm = r_hat * (((gh3 + 1.0f) * (gamma_hat + 1.0f)) / denom);
        assert(isfinite((double)rhat_norm));

        /* argmin over the gamma table (double comparison, robust to ties). */
        best = 0;
        double best_d = fabs(prec[0] - (double)rhat_norm);
        for (int i = 1; i < NIQE_GAMMA_COUNT; i++) {
            const double d = fabs(prec[i] - (double)rhat_norm);
            if (d < best_d) {
                best_d = d;
                best = i;
            }
        }
    }
    const double alpha = niqe_gamma_value(best);

    const double g1 = tgamma(1.0 / alpha);
    const double g2 = tgamma(2.0 / alpha);
    const double g3 = tgamma(3.0 / alpha);
    assert(g3 > 0.0);
    const double aggdratio = sqrt(g1) / sqrt(g3);
    const double bl = aggdratio * (double)lms;
    const double br = aggdratio * (double)rms;
    const double N = (br - bl) * (g2 / g1) * aggdratio;

    NiqeAggd out = {alpha, N, bl, br, (double)lms, (double)rms};
    return out;
}

/* ------------------------------------------------------------------ */
/* PIL-compatible bicubic (Catmull-Rom, a = -0.5) separable resampler. */
/*                                                                     */
/* Port of Pillow's ImagingResample 32-bpc (float) path, used because  */
/* the harness feeds float64 luma to Image.fromarray (mode 'F') before  */
/* Image.Resampling.BICUBIC — so PIL takes the FLOAT resample path (no  */
/* uint8 fixed-point quantization). Validated bit-close (max abs diff   */
/* 1.5e-5, float32 rounding only) vs Pillow on a 194x194 fixture; see   */
/* docs/metrics/niqe.md.                                                */
/* ------------------------------------------------------------------ */

#define NIQE_BICUBIC_A (-0.5)
#define NIQE_BICUBIC_SUPPORT 2.0
/* Max kernel taps: for odd input widths/heights the rational scale w/floor(w/2)
 * slightly exceeds 2.0, giving ceil(2*scale)*2+1 = 11 (not 9).  The minimum
 * guarded frame dimension is 96, and floor(97/2)=48 → scale=97/48≈2.021 →
 * ceil(2*2.021)=5 → ksize=5*2+1=11.  Set the bound to 11 so the coefficient
 * array is always large enough even for odd-dimensioned frames. */
#define NIQE_BICUBIC_MAX_KSIZE 11

static inline double niqe_bicubic_filter(double x)
{
    const double a = NIQE_BICUBIC_A;
    if (x < 0.0)
        x = -x;
    if (x < 1.0)
        return ((a + 2.0) * x - (a + 3.0)) * x * x + 1.0;
    if (x < 2.0)
        return (((x - 5.0) * x + 8.0) * x - 4.0) * a;
    return 0.0;
}

/* Precompute Pillow resample coefficients for one axis.
 *   bounds[o*2+0] = xmin (first input sample), bounds[o*2+1] = xmax (count)
 *   coeffs[o*ksize + k] = normalized tap weight
 * Returns the actual kernel size used (<= NIQE_BICUBIC_MAX_KSIZE). */
static inline int niqe_bicubic_coeffs(int in_size, int out_size, int *bounds, double *coeffs)
{
    const double scale = (double)in_size / (double)out_size;
    double filterscale = scale;
    if (filterscale < 1.0)
        filterscale = 1.0;
    const double support = NIQE_BICUBIC_SUPPORT * filterscale;
    const int ksize = (int)ceil(support) * 2 + 1;
    const double inv_fs = 1.0 / filterscale;

    for (int o = 0; o < out_size; o++) {
        const double center = (o + 0.5) * scale;
        int xmin = (int)(center - support + 0.5);
        if (xmin < 0)
            xmin = 0;
        int xmax = (int)(center + support + 0.5);
        if (xmax > in_size)
            xmax = in_size;
        xmax -= xmin;

        double *k = coeffs + (size_t)o * ksize;
        double ww = 0.0;
        for (int x = 0; x < xmax; x++) {
            const double w = niqe_bicubic_filter(((double)(x + xmin) - center + 0.5) * inv_fs);
            k[x] = w;
            ww += w;
        }
        for (int x = 0; x < xmax; x++) {
            if (ww != 0.0)
                k[x] /= ww;
        }
        /* Zero the unused tail so the resample loop can read a fixed ksize. */
        for (int x = xmax; x < ksize; x++)
            k[x] = 0.0;

        bounds[o * 2 + 0] = xmin;
        bounds[o * 2 + 1] = xmax;
    }
    return ksize;
}

/* ------------------------------------------------------------------ */
/* Symmetric pseudo-inverse via cyclic Jacobi eigen-decomposition.     */
/*                                                                     */
/* The averaged covariance M = (cov_pris + sample_cov)/2 is symmetric.  */
/* We diagonalize M = V diag(e) V^T, then pinv(M) = V diag(e+) V^T with  */
/* e+_i = 1/e_i for |e_i| > cutoff else 0, cutoff = rtol*max|e|,         */
/* rtol = NIQE_FEAT_DIM * DBL_EPSILON (scipy.linalg.pinv default).      */
/* Validated to 7e-14 vs scipy on the fixture (see docs/metrics/niqe.md).*/
/* ------------------------------------------------------------------ */

/* ---- Jacobi helper 1: copy A <- in, V <- identity, initialise D. ---- */
static inline void niqe_jacobi_init(int n, const double *in, double *A, double *V)
{
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            A[(size_t)i * n + j] = in[(size_t)i * n + j];
            V[(size_t)i * n + j] = (i == j) ? 1.0 : 0.0;
        }
    }
}

/* ---- Jacobi helper 2: cyclic Jacobi sweeps (the rotation inner loop). ---- */
static inline void niqe_jacobi_sweep_pass(int n, double *A, double *V)
{
    /* Cyclic Jacobi sweeps. n=36 converges well within this bound. */
    for (int sweep = 0; sweep < 100; sweep++) {
        double off = 0.0;
        for (int p = 0; p < n; p++) {
            for (int q = p + 1; q < n; q++) {
                const double apq = A[(size_t)p * n + q];
                off += apq * apq;
            }
        }
        if (off <= 1e-300)
            break;

        for (int p = 0; p < n; p++) {
            for (int q = p + 1; q < n; q++) {
                const double apq = A[(size_t)p * n + q];
                if (fabs(apq) <= 1e-300)
                    continue;
                const double app = A[(size_t)p * n + p];
                const double aqq = A[(size_t)q * n + q];
                const double theta = (aqq - app) / (2.0 * apq);
                double t;
                if (theta == 0.0) {
                    t = 1.0;
                } else {
                    const double sgn = (theta > 0.0) ? 1.0 : -1.0;
                    t = sgn / (fabs(theta) + sqrt(theta * theta + 1.0));
                }
                const double c = 1.0 / sqrt(t * t + 1.0);
                const double s = t * c;

                for (int k = 0; k < n; k++) {
                    const double akp = A[(size_t)k * n + p];
                    const double akq = A[(size_t)k * n + q];
                    A[(size_t)k * n + p] = c * akp - s * akq;
                    A[(size_t)k * n + q] = s * akp + c * akq;
                }
                for (int k = 0; k < n; k++) {
                    const double apk = A[(size_t)p * n + k];
                    const double aqk = A[(size_t)q * n + k];
                    A[(size_t)p * n + k] = c * apk - s * aqk;
                    A[(size_t)q * n + k] = s * apk + c * aqk;
                }
                for (int k = 0; k < n; k++) {
                    const double vkp = V[(size_t)k * n + p];
                    const double vkq = V[(size_t)k * n + q];
                    V[(size_t)k * n + p] = c * vkp - s * vkq;
                    V[(size_t)k * n + q] = s * vkp + c * vkq;
                }
            }
        }
    }
}

/* ---- Jacobi helper 3: eigenvalue cutoff + V diag(e+) V^T reconstruction. */
static inline void niqe_pinv_reconstruct(int n, const double *A, const double *V, double *out,
                                         double rtol)
{
    /* Eigenvalues on the diagonal of A; cutoff relative to max magnitude. */
    double max_e = 0.0;
    for (int i = 0; i < n; i++) {
        const double e = fabs(A[(size_t)i * n + i]);
        if (e > max_e)
            max_e = e;
    }
    const double cutoff = rtol * max_e;

    /* out = V diag(e+) V^T. */
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            double acc = 0.0;
            for (int k = 0; k < n; k++) {
                const double e = A[(size_t)k * n + k];
                if (fabs(e) > cutoff) {
                    acc += V[(size_t)i * n + k] * (1.0 / e) * V[(size_t)j * n + k];
                }
            }
            out[(size_t)i * n + j] = acc;
        }
    }
}

/* Jacobi pinv of the n x n symmetric matrix `in` (row-major) into `out`.
 * `work` must hold 2*n*n doubles of scratch (A then V). rtol selects the
 * singular-value cutoff. */
static inline void niqe_sym_pinv(const double *in, double *out, int n, double rtol, double *work)
{
    assert(n > 0 && in && out && work);
    double *A = work;                 /* n*n */
    double *V = work + (size_t)n * n; /* n*n */
    niqe_jacobi_init(n, in, A, V);
    niqe_jacobi_sweep_pass(n, A, V);
    niqe_pinv_reconstruct(n, A, V, out, rtol);
}

#endif /* __VMAF_FEATURE_NIQE_MATH_H__ */
