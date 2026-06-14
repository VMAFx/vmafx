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
 * brisque_math.h — header-only numeric kernels for the BRISQUE no-reference
 * feature extractor (feature/brisque.c). Kept in a separate header so the
 * correctness unit test (test/test_brisque.c) can include and exercise the
 * GGD/AGGD fits, the gamma lookup tables, the 7x7 Gaussian window, the
 * MATLAB-imresize bicubic resampler, and the range-scale directly — mirroring
 * the niqe_math.h / test_niqe.c split.
 *
 * SPEC SOURCE OF TRUTH: the gregfreeman MATLAB pipeline
 * (image_quality_toolbox/+brisque/{brisque_feature,estimateggdparam,
 * estimateaggdparam}.m) — the pipeline that TRAINED the bundled allmodel
 * (model/other_models/brisque_live.model). NOT the krshrimali C++ port, which
 * diverges on three load-bearing points the model was NOT trained with:
 *   (1) krshrimali fits the MSCN field with AGGD; MATLAB uses GGD for f1/f2.
 *   (2) krshrimali truncates the Gaussian sigma to 1.166; MATLAB uses 7/6.
 *   (3) krshrimali downsamples with OpenCV INTER_CUBIC; MATLAB uses antialiased
 *       bicubic (imresize). We port the MATLAB imresize kernel exactly.
 * See docs/adr/1115-brisque-nr-metric.md.
 *
 * Everything here is double precision (the MATLAB/C++ reference and libsvm are
 * all double); pure (no I/O, no global mutable state).
 */

#ifndef __VMAF_FEATURE_BRISQUE_MATH_H__
#define __VMAF_FEATURE_BRISQUE_MATH_H__

#include <assert.h>
#include <math.h>
#include <stddef.h>

/* ------------------------------------------------------------------ */
/* Gamma lookup tables for the GGD / AGGD shape fits.                  */
/*                                                                     */
/* MATLAB grid: gam = 0.2:0.001:10  (INCLUSIVE both ends) -> 9801.     */
/* GGD:  r_gam(g) = (Gamma(1/g)*Gamma(3/g)) / Gamma(2/g)^2             */
/* AGGD: r_gam(g) = Gamma(2/g)^2 / (Gamma(1/g)*Gamma(3/g))             */
/*                                                                     */
/* The argmin lookup is over a fixed 0.001 grid, so alpha is quantized */
/* identically across implementations (the dominant cross-impl parity  */
/* risk is the bicubic downsample, not the gamma table — see ADR-1115).*/
/* ------------------------------------------------------------------ */

#define BRISQUE_GAMMA_COUNT 9801
#define BRISQUE_GAMMA_START 0.2
#define BRISQUE_GAMMA_STEP 0.001

/* Map a table index to its gamma_range value. */
static inline double brisque_gamma_value(int idx)
{
    return BRISQUE_GAMMA_START + (double)idx * BRISQUE_GAMMA_STEP;
}

/* Fill the GGD ratio table (BRISQUE_GAMMA_COUNT doubles). Caller owns it. */
static inline void brisque_build_ggd_table(double *prec)
{
    assert(prec != NULL);
    for (int i = 0; i < BRISQUE_GAMMA_COUNT; i++) {
        const double g = brisque_gamma_value(i);
        const double g1 = tgamma(1.0 / g);
        const double g2 = tgamma(2.0 / g);
        const double g3 = tgamma(3.0 / g);
        prec[i] = (g1 * g3) / (g2 * g2);
    }
}

/* Fill the AGGD ratio table (BRISQUE_GAMMA_COUNT doubles). Caller owns it. */
static inline void brisque_build_aggd_table(double *prec)
{
    assert(prec != NULL);
    for (int i = 0; i < BRISQUE_GAMMA_COUNT; i++) {
        const double g = brisque_gamma_value(i);
        const double g1 = tgamma(1.0 / g);
        const double g2 = tgamma(2.0 / g);
        const double g3 = tgamma(3.0 / g);
        prec[i] = (g2 * g2) / (g1 * g3);
    }
}

/* ------------------------------------------------------------------ */
/* GGD fit of the MSCN field (features f1, f2).                        */
/*                                                                     */
/* Mirrors estimateggdparam.m:                                         */
/*   sigma_sq = mean(x^2);  E = mean(|x|);  rho = sigma_sq / E^2       */
/*   alpha = gam[argmin |rho - r_gam_ggd|]                             */
/* Returns alpha and sigma_sq (the caller pushes f1=alpha, f2=sigma_sq).*/
/* ------------------------------------------------------------------ */

typedef struct BrisqueGgd {
    double alpha;
    double sigma_sq;
} BrisqueGgd;

static inline BrisqueGgd brisque_fit_ggd(const double *x, size_t n, const double *ggd_table)
{
    assert(x != NULL && ggd_table != NULL && n > 0);
    double sq_sum = 0.0;
    double abs_sum = 0.0;
    for (size_t i = 0; i < n; i++) {
        const double v = x[i];
        sq_sum += v * v;
        abs_sum += fabs(v);
    }
    const double sigma_sq = sq_sum / (double)n;
    const double mean_abs = abs_sum / (double)n;

    BrisqueGgd out;
    out.sigma_sq = sigma_sq;

    /* Flat field (mean_abs == 0): rho = 0/0. Return the alpha lower bound
     * deterministically (a flat patch is degenerate; the trained model only
     * sees natural content, but a NaN here would propagate into the SVR). */
    if (mean_abs == 0.0) {
        out.alpha = BRISQUE_GAMMA_START;
        return out;
    }
    const double rho = sigma_sq / (mean_abs * mean_abs);
    assert(isfinite(rho));

    int best = 0;
    double best_d = fabs(rho - ggd_table[0]);
    for (int i = 1; i < BRISQUE_GAMMA_COUNT; i++) {
        const double d = fabs(rho - ggd_table[i]);
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    out.alpha = brisque_gamma_value(best);
    return out;
}

/* ------------------------------------------------------------------ */
/* AGGD fit of a paired-product field (4 shifts x {alpha, eta, l2, r2}).*/
/*                                                                     */
/* Mirrors estimateaggdparam.m + brisque_feature.m:                   */
/*   leftstd  = sqrt(mean(x[x<0]^2))   (strict <0)                     */
/*   rightstd = sqrt(mean(x[x>0]^2))   (strict >0; ZEROS EXCLUDED)     */
/*   gammahat = leftstd/rightstd                                       */
/*   rhat     = mean(|x|)^2 / mean(x^2)                                */
/*   rhatnorm = rhat*(gh^3+1)*(gh+1)/(gh^2+1)^2                        */
/*   alpha    = gam[argmin (r_gam_aggd - rhatnorm)^2]                  */
/*   const    = sqrt(Gamma(1/alpha))/sqrt(Gamma(3/alpha))             */
/*   eta      = (rightstd-leftstd)*(Gamma(2/alpha)/Gamma(1/alpha))*const*/
/* Pushes {alpha, eta, leftstd^2, rightstd^2}.                         */
/*                                                                     */
/* NOTE: MATLAB's x<0 / x>0 are STRICT, so exact zeros land in NEITHER */
/* the left nor the right set (unlike NIQE, which buckets zeros right). */
/* This is load-bearing for allmodel parity.                           */
/* ------------------------------------------------------------------ */

typedef struct BrisqueAggd {
    double alpha;
    double eta;
    double left_sq;  /* leftstd^2 */
    double right_sq; /* rightstd^2 */
} BrisqueAggd;

static inline BrisqueAggd brisque_fit_aggd(const double *x, size_t n, const double *aggd_table)
{
    assert(x != NULL && aggd_table != NULL && n > 0);
    double left_sq_sum = 0.0;
    double right_sq_sum = 0.0;
    size_t left_cnt = 0;
    size_t right_cnt = 0;
    double abs_sum = 0.0;
    double sq_sum = 0.0;

    for (size_t i = 0; i < n; i++) {
        const double v = x[i];
        sq_sum += v * v;
        abs_sum += fabs(v);
        if (v < 0.0) {
            left_sq_sum += v * v;
            left_cnt++;
        } else if (v > 0.0) {
            right_sq_sum += v * v;
            right_cnt++;
        }
        /* v == 0.0: excluded from both (MATLAB strict <0 / >0). */
    }

    const double leftstd = (left_cnt > 0) ? sqrt(left_sq_sum / (double)left_cnt) : 0.0;
    const double rightstd = (right_cnt > 0) ? sqrt(right_sq_sum / (double)right_cnt) : 0.0;
    const double mean_sq = sq_sum / (double)n;

    BrisqueAggd out;
    out.left_sq = leftstd * leftstd;
    out.right_sq = rightstd * rightstd;

    /* Flat / one-sided degeneracies. rightstd==0 makes gammahat = x/0 = inf,
     * mean_sq==0 makes rhat = 0/0 = NaN. Either way the fit is meaningless;
     * return the alpha lower bound and eta 0 deterministically. The trained
     * model only ever sees natural content (rightstd, mean_sq > 0), so this
     * guard never fires on real frames — it only prevents NaN propagation. */
    if (rightstd == 0.0 || mean_sq == 0.0) {
        out.alpha = BRISQUE_GAMMA_START;
        out.eta = 0.0;
        return out;
    }

    const double gammahat = leftstd / rightstd;
    const double mean_abs = abs_sum / (double)n;
    const double rhat = (mean_abs * mean_abs) / mean_sq;
    const double gh2 = gammahat * gammahat;
    const double gh3 = gh2 * gammahat;
    const double denom = (gh2 + 1.0) * (gh2 + 1.0);
    const double rhatnorm = (rhat * (gh3 + 1.0) * (gammahat + 1.0)) / denom;
    assert(isfinite(rhatnorm));

    int best = 0;
    double diff0 = aggd_table[0] - rhatnorm;
    double best_d = diff0 * diff0;
    for (int i = 1; i < BRISQUE_GAMMA_COUNT; i++) {
        const double diff = aggd_table[i] - rhatnorm;
        const double d = diff * diff;
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    const double alpha = brisque_gamma_value(best);
    out.alpha = alpha;

    const double g1 = tgamma(1.0 / alpha);
    const double g2 = tgamma(2.0 / alpha);
    const double g3 = tgamma(3.0 / alpha);
    assert(g1 > 0.0 && g3 > 0.0);
    const double cst = sqrt(g1) / sqrt(g3);
    out.eta = (rightstd - leftstd) * (g2 / g1) * cst;
    return out;
}

/* ------------------------------------------------------------------ */
/* 7x7 Gaussian window: fspecial('gaussian',7,7/6) then /sum.          */
/* Stored as a separable 7-tap vector (the 2D window is the outer       */
/* product win[i]*win[j], and sum(win)==1 so the 2D window sums to 1).  */
/* MATLAB filter2(win2d, img, 'same') is correlation with zero-padding  */
/* at the borders; we implement that separably with zero (not nearest)  */
/* boundary, matching the MATLAB pipeline that trained the model.        */
/* ------------------------------------------------------------------ */

#define BRISQUE_WIN_LW 3
#define BRISQUE_WIN_LEN 7 /* 2*lw + 1 */
#define BRISQUE_WIN_SIGMA (7.0 / 6.0)

/* fspecial builds the 2D Gaussian h(x,y)=exp(-(x^2+y^2)/(2 sigma^2)) then
 * normalizes by its sum. That 2D window is separable: h(x,y)=g(x)g(y)/S2D
 * with g(t)=exp(-t^2/(2 sigma^2)). After 2D normalization the SEPARABLE 1D
 * factor is g(t)/sum(g), because (g(x)/Sg)*(g(y)/Sg) sums to 1 over the 7x7
 * grid (Sg = sum_t g(t), S2D = Sg^2). So the unit-volume 2D window is the
 * outer product of the unit-sum 1D vector with itself. */
static inline void brisque_build_window(double window[BRISQUE_WIN_LEN])
{
    const double two_sigma_sq = 2.0 * BRISQUE_WIN_SIGMA * BRISQUE_WIN_SIGMA;
    double g[BRISQUE_WIN_LEN];
    double s = 0.0;
    for (int i = 0; i < BRISQUE_WIN_LEN; i++) {
        const double t = (double)(i - BRISQUE_WIN_LW);
        g[i] = exp(-(t * t) / two_sigma_sq);
        s += g[i];
    }
    assert(s > 0.0);
    for (int i = 0; i < BRISQUE_WIN_LEN; i++)
        window[i] = g[i] / s;
}

/* ------------------------------------------------------------------ */
/* MATLAB imresize antialiased bicubic, 0.5x downscale (Catmull-Rom    */
/* a=-0.5). Separable: vertical (rows) pass then horizontal (cols) pass */
/* — MATLAB resizes the smaller-scale dim first; both are 0.5 so the    */
/* row pass runs first. Output size = ceil(in/2) per axis (handles ODD  */
/* dimensions correctly: ceil(97/2)=49). Validated bit-close (max abs   */
/* diff ~1e-13) vs the MATLAB imresize reference on even AND odd        */
/* fixtures — see docs/metrics/brisque.md.                              */
/* ------------------------------------------------------------------ */

/* For scale=0.5 the antialiased kernel width = 4/scale = 8, so the per-output
 * tap count is ceil(8)+2 = 10. Sizing the coefficient buffer to this constant
 * is odd-dimension-safe (the tap count does not grow for odd inputs at a fixed
 * 0.5x scale). */
#define BRISQUE_RESIZE_TAPS 10
#define BRISQUE_RESIZE_SCALE 0.5

static inline double brisque_cubic(double x)
{
    if (x < 0.0)
        x = -x;
    const double x2 = x * x;
    const double x3 = x2 * x;
    if (x <= 1.0)
        return 1.5 * x3 - 2.5 * x2 + 1.0;
    if (x < 2.0)
        return -0.5 * x3 + 2.5 * x2 - 4.0 * x + 2.0;
    return 0.0;
}

/* Output length for a 0.5x downscale: ceil(in/2). */
static inline int brisque_resize_out_len(int in_len)
{
    assert(in_len > 0);
    return (in_len + 1) / 2;
}

/* Symmetric-reflect index clamp into [0, in_len): mirrors MATLAB's
 * aux = [0..in-1, in-1..0] modulo 2*in mapping. */
static inline int brisque_reflect_index(int i, int in_len)
{
    const int period = 2 * in_len;
    int m = i % period;
    if (m < 0)
        m += period;
    return (m < in_len) ? m : (period - 1 - m);
}

/* Precompute the (out_len x BRISQUE_RESIZE_TAPS) index + weight tables for one
 * axis 0.5x downscale. `idx` and `wts` are out_len*BRISQUE_RESIZE_TAPS. */
static inline void brisque_resize_coeffs(int in_len, int out_len, int *idx, double *wts)
{
    assert(idx != NULL && wts != NULL && in_len > 0 && out_len > 0);
    const double scale = BRISQUE_RESIZE_SCALE;
    const double kernel_width = 4.0 / scale; /* = 8 */
    const double inv_scale = 1.0 / scale;    /* = 2 */
    for (int o = 0; o < out_len; o++) {
        /* MATLAB 1-based center, converted carefully:
         *   u = (o+1)/scale + 0.5*(1 - 1/scale)   (1-based)
         *   left = floor(u - kernel_width/2)
         *   input position p (1-based) = left + t,  t = 0..TAPS-1 */
        const double u = ((double)(o + 1)) * inv_scale + 0.5 * (1.0 - inv_scale);
        const double left = floor(u - kernel_width / 2.0);
        double *w = wts + (size_t)o * BRISQUE_RESIZE_TAPS;
        int *ix = idx + (size_t)o * BRISQUE_RESIZE_TAPS;
        double wsum = 0.0;
        for (int t = 0; t < BRISQUE_RESIZE_TAPS; t++) {
            const double p1 = left + (double)t; /* 1-based input position */
            /* antialiased kernel: scale * cubic(scale * (u - p1)) */
            const double wt = scale * brisque_cubic(scale * (u - p1));
            w[t] = wt;
            wsum += wt;
            ix[t] = brisque_reflect_index((int)p1 - 1, in_len); /* to 0-based */
        }
        assert(wsum != 0.0);
        for (int t = 0; t < BRISQUE_RESIZE_TAPS; t++)
            w[t] /= wsum;
    }
}

/* ------------------------------------------------------------------ */
/* libsvm range-scale to [-1, 1] (NO clamp — matches the reference     */
/* prediction path; the OpenCV variant clamps the final score, this    */
/* one does not. See ADR-1115).                                         */
/*   xs[i] = -1 + 2*(feat[i]-min_[i])/(max_[i]-min_[i])                 */
/* ------------------------------------------------------------------ */
static inline double brisque_range_scale(double feat, double lo, double hi)
{
    assert(hi != lo);
    return -1.0 + 2.0 / (hi - lo) * (feat - lo);
}

#endif /* __VMAF_FEATURE_BRISQUE_MATH_H__ */
