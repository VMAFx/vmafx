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
 * NIQE — Natural Image Quality Evaluator (no-reference, opinion-unaware).
 *
 * Mittal, Soundararajan, Bovik, "Making a Completely Blind Image Quality
 * Analyzer," IEEE SPL 20(3):209-212, 2013.
 *
 * This is a no-reference (NR) metric: it scores the DISTORTED picture only,
 * mirroring the CAMBI NR posture already in the fork (ref_pic / *_90 are
 * (void)-discarded). Higher score = lower quality (further from the natural
 * scene statistics of the pristine population).
 *
 * The C pipeline replicates the fork's Python harness
 * (compat/python-vmaf/core/noref_feature_extractor.py::NiqeNorefFeatureExtractor)
 * byte-for-byte, because the in-tree pristine model
 * model/other_models/niqe_v0.1.pkl (embedded as niqe_model.h) was trained
 * against THAT exact pipeline — not the LIVE MATLAB or scikit-video variants.
 * The two load-bearing fork divergences from upstream NIQE are:
 *   1. the trailing *aggdratio factor in the AGGD mean parameter N, and
 *   2. the float32 round-trip of the MSCN maps before patch features.
 * Both are required for pkl parity; see ADR-1112 and docs/metrics/niqe.md.
 *
 * Per-frame pipeline (all in float64 except the float32 MSCN round-trip):
 *   distorted luma (plane 0, uint8/uint16)
 *     ├─ scale-1: MSCN via separable 7-tap Gaussian (sigma=7/6), nearest
 *     │           boundary, C=1 stabiliser  → m_image1 (cast to float32)
 *     └─ scale-2: PIL-bicubic (Catmull-Rom a=-0.5) downscale by 2 → MSCN
 *                 → m_image2 (cast to float32)
 *   for each non-overlapping 96x96 patch (48x48 on the half-res map):
 *     AGGD-fit the MSCN map → (alpha_m, (bl+br)/2);
 *     AGGD-fit the 4 paired products V/H/D1/D2 → (alpha, N, lsq, rsq) each;
 *     concatenate lvl1 (18) || lvl2 (18) → a 36-vector per patch.
 *   pool: sample_mu = column mean, sample_cov = unbiased covariance (ddof=1);
 *   score = sqrt(X^T pinv((cov_pris + sample_cov)/2) X), X = sample_mu - mu_pris.
 */

#include <assert.h>
#include <errno.h>
#include <float.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "libvmaf/picture.h"

#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature/niqe_math.h"
#include "feature/niqe_model.h"
#include "mem.h"

/* Algorithm constants (VERIFIED against the fork harness, scikit-video,
 * guptapraful, and the LIVE MATLAB sources — see ADR-1112). */
#define NIQE_PATCH 96
#define NIQE_HALF_PATCH 48
#define NIQE_SCALENUM 2
#define NIQE_FEATPERSCALE 18
#define NIQE_WIN_LW 3
#define NIQE_WIN_LEN 7 /* 2*lw + 1 */
#define NIQE_WIN_SIGMA (7.0 / 6.0)
#define NIQE_MSCN_C 1.0 /* additive denominator stabiliser */

/* Minimum frame size: the np.cov pooling needs >= 2 patches (one dimension
 * may be a single patch, but the product must be >= 2). With 96x96
 * non-overlapping patches, a 96x192 (or 192x96) frame already yields 2
 * patches; we require width and height each >= NIQE_PATCH and the patch
 * count >= 2, mirroring niqe_train_test_model.py:99 (`shape[0] < 2 -> None`).
 * The conservative >=194 guard cited in early design notes was an arithmetic
 * error (2*96 = 192, not 193) and rejects valid small frames; we use the
 * exact >=2-patch rule instead. */

typedef struct NiqeState {
    unsigned w;
    unsigned h;
    unsigned bpc;

    /* Half-resolution dimensions (PIL int(w/2.0) / int(h/2.0)). */
    unsigned w2;
    unsigned h2;

    /* Patch grid (number of 96x96 patches that fit in each dimension). */
    unsigned npx; /* patches across width  */
    unsigned npy; /* patches down  height  */
    unsigned npatches;

    /* Separable Gaussian window (float64). */
    double window[NIQE_WIN_LEN];

    /* AGGD gamma lookup table (NIQE_GAMMA_COUNT doubles). */
    double *gamma_table;

    /* Working buffers (float64) sized for the full plane. */
    double *luma; /* w*h   distorted luma as double */
    double *tmp;  /* w*h   convolution scratch / squared image */
    double *mu;   /* w*h   local mean (then reused) */
    double *var;  /* w*h   local variance accumulator */

    /* Half-resolution working buffers. */
    double *luma2; /* w2*h2 */
    double *tmp2;  /* w2*h2 */
    double *mu2;   /* w2*h2 */
    double *var2;  /* w2*h2 */

    /* MSCN maps cast to float32 (harness parity). */
    float *mscn1; /* w*h   */
    float *mscn2; /* w2*h2 */

    /* Per-patch feature matrix: npatches rows of NIQE_FEAT_DIM (36). */
    double *feat; /* npatches * NIQE_FEAT_DIM */

    /* Per-patch scratch for AGGD inputs (the MSCN patch + 4 paired products),
     * float32 to match the harness float32 patch arithmetic. */
    float *patch_buf;  /* NIQE_PATCH * NIQE_PATCH (largest patch = 96x96) */
    float *paired_buf; /* NIQE_PATCH * NIQE_PATCH */

    /* Bicubic resample coefficient tables (allocated for the larger axis). */
    int *bicubic_bounds;    /* 2 * max(w2,h2) */
    double *bicubic_coeffs; /* max(w2,h2) * NIQE_BICUBIC_MAX_KSIZE */
    double *resize_tmp;     /* w2 * h (intermediate after horizontal pass) */

    /* Pooling scratch (float64). */
    double *sample_mu;  /* NIQE_FEAT_DIM */
    double *sample_cov; /* NIQE_FEAT_DIM * NIQE_FEAT_DIM */
    double *avgcov;     /* NIQE_FEAT_DIM * NIQE_FEAT_DIM */
    double *pinv;       /* NIQE_FEAT_DIM * NIQE_FEAT_DIM */
    double *pinv_work;  /* 2 * NIQE_FEAT_DIM * NIQE_FEAT_DIM */
} NiqeState;

/* ------------------------------------------------------------------ */
/* Separable Gaussian window: gauss_window(3, 7/6) in float64.         */
/* Mirrors NiqeNorefFeatureExtractor.gauss_window exactly.             */
/* ------------------------------------------------------------------ */
static void niqe_build_window(double window[NIQE_WIN_LEN])
{
    double sd = NIQE_WIN_SIGMA;
    double weights[NIQE_WIN_LEN] = {0.0};
    const int lw = NIQE_WIN_LW;
    weights[lw] = 1.0;
    double curr_sum = 1.0;
    sd *= sd;
    for (int ii = 1; ii <= lw; ii++) {
        const double tmp = exp(-0.5 * (double)(ii * ii) / sd);
        weights[lw + ii] = tmp;
        weights[lw - ii] = tmp;
        curr_sum += 2.0 * tmp;
    }
    for (int ii = 0; ii < NIQE_WIN_LEN; ii++)
        window[ii] = weights[ii] / curr_sum;
}

/* ------------------------------------------------------------------ */
/* Separable correlate1d with 'nearest' (clamp-to-edge) boundary, as   */
/* used by scipy.ndimage.correlate1d(mode='nearest'). Applied along    */
/* axis 0 (rows / vertical) then axis 1 (cols / horizontal), matching   */
/* calc_image's correlate1d(..., 0, ...) then correlate1d(..., 1, ...). */
/* `src` and `dst` may NOT alias (caller uses separate buffers).        */
/* ------------------------------------------------------------------ */
static void niqe_correlate_axis0(const double *src, double *dst, unsigned w, unsigned h,
                                 const double window[NIQE_WIN_LEN])
{
    const int lw = NIQE_WIN_LW;
    for (unsigned y = 0; y < h; y++) {
        for (unsigned x = 0; x < w; x++) {
            double acc = 0.0;
            for (int k = -lw; k <= lw; k++) {
                long yy = (long)y + k;
                if (yy < 0)
                    yy = 0;
                if (yy >= (long)h)
                    yy = (long)h - 1;
                acc += src[(size_t)yy * w + x] * window[k + lw];
            }
            dst[(size_t)y * w + x] = acc;
        }
    }
}

static void niqe_correlate_axis1(const double *src, double *dst, unsigned w, unsigned h,
                                 const double window[NIQE_WIN_LEN])
{
    const int lw = NIQE_WIN_LW;
    for (unsigned y = 0; y < h; y++) {
        for (unsigned x = 0; x < w; x++) {
            double acc = 0.0;
            for (int k = -lw; k <= lw; k++) {
                long xx = (long)x + k;
                if (xx < 0)
                    xx = 0;
                if (xx >= (long)w)
                    xx = (long)w - 1;
                acc += src[(size_t)y * w + (size_t)xx] * window[k + lw];
            }
            dst[(size_t)y * w + x] = acc;
        }
    }
}

/* Build the MSCN map for a plane: mu = sepfilter(image),
 * var = sepfilter(image^2), sigma = sqrt(|var - mu^2|),
 * mscn = (image - mu) / (sigma + C). Result is cast to float32 (harness
 * parity). `image`, `tmp`, `mu`, `var` are float64 scratch of size w*h;
 * `out` is the float32 MSCN map of size w*h. */
static void niqe_compute_mscn(const double *image, double *tmp, double *mu, double *var, float *out,
                              unsigned w, unsigned h, const double window[NIQE_WIN_LEN])
{
    /* mu = correlate(axis0) then correlate(axis1). */
    niqe_correlate_axis0(image, tmp, w, h, window);
    niqe_correlate_axis1(tmp, mu, w, h, window);

    /* var = correlate(image^2, axis0) then correlate(axis1). Reuse `tmp`
     * for image^2, then `var` for the running result. */
    for (size_t i = 0; i < (size_t)w * h; i++)
        tmp[i] = image[i] * image[i];
    niqe_correlate_axis0(tmp, var, w, h, window);
    /* axis1 in place via a second scratch pass: reuse tmp as destination. */
    niqe_correlate_axis1(var, tmp, w, h, window);

    for (size_t i = 0; i < (size_t)w * h; i++) {
        const double sigma = sqrt(fabs(tmp[i] - mu[i] * mu[i]));
        const double mscn = (image[i] - mu[i]) / (sigma + NIQE_MSCN_C);
        out[i] = (float)mscn;
    }
}

/* ------------------------------------------------------------------ */
/* PIL-bicubic downscale of the integer luma to (w2, h2), float64.     */
/* Separable: horizontal pass into resize_tmp (h rows x w2 cols), then  */
/* vertical pass into out (h2 rows x w2 cols).                          */
/* ------------------------------------------------------------------ */
static void niqe_bicubic_resize(const double *src, double *out, unsigned w, unsigned h, unsigned w2,
                                unsigned h2, int *bounds, double *coeffs, double *resize_tmp)
{
    /* Horizontal pass: w -> w2. */
    const int ksx = niqe_bicubic_coeffs((int)w, (int)w2, bounds, coeffs);
    for (unsigned o = 0; o < w2; o++) {
        const int xmin = bounds[o * 2 + 0];
        const int xmax = bounds[o * 2 + 1];
        const double *kx = coeffs + (size_t)o * ksx;
        for (unsigned y = 0; y < h; y++) {
            double acc = 0.0;
            for (int k = 0; k < xmax; k++)
                acc += src[(size_t)y * w + (size_t)(xmin + k)] * kx[k];
            resize_tmp[(size_t)y * w2 + o] = acc;
        }
    }

    /* Vertical pass: h -> h2 (operating on resize_tmp, w2 columns).
     *
     * The final sample is rounded through float32 because PIL returns a
     * 32-bit float ('F' mode) image from BICUBIC resize, and the fork
     * harness feeds that float32 array into calc_image (which then promotes
     * to float64). The float32 quantization is load-bearing: without it the
     * scale-2 MSCN map differs by ~4e-7, which flips the near-zero sign
     * classification in the AGGD paired products and (amplified by the
     * ill-conditioned averaged covariance) shifts the final score by ~1e-4.
     * With the float32 round the C resampler is bit-exact vs PIL. See
     * ADR-1112 / docs/metrics/niqe.md. */
    const int ksy = niqe_bicubic_coeffs((int)h, (int)h2, bounds, coeffs);
    for (unsigned o = 0; o < h2; o++) {
        const int ymin = bounds[o * 2 + 0];
        const int ymax = bounds[o * 2 + 1];
        const double *ky = coeffs + (size_t)o * ksy;
        for (unsigned x = 0; x < w2; x++) {
            double acc = 0.0;
            for (int k = 0; k < ymax; k++)
                acc += resize_tmp[(size_t)(ymin + k) * w2 + x] * ky[k];
            out[(size_t)o * w2 + x] = (double)(float)acc;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Paired-product subbands (V, H, D1, D2) of a square float32 patch.    */
/* Mirrors paired_p: roll convention is wraparound on the PATCH.        */
/*   v_img  = roll(im, +1, axis0) * im                                  */
/*   h_img  = roll(im, +1, axis1) * im                                  */
/*   d1_img = roll(roll(im,+1,ax1),+1,ax0) * im                         */
/*   D2_img = roll(roll(im,-1,ax1),+1,ax0) * im                         */
/* `which` selects 0=V, 1=H, 2=D1, 3=D2. `n` = patch side length.       */
/* ------------------------------------------------------------------ */
static void niqe_paired_product(const float *patch, float *out, unsigned n, int which)
{
    for (unsigned y = 0; y < n; y++) {
        for (unsigned x = 0; x < n; x++) {
            unsigned sy = y;
            unsigned sx = x;
            switch (which) {
            case 0: /* V: roll +1 axis0 -> source row y-1 (mod n) */
                sy = (y + n - 1) % n;
                break;
            case 1: /* H: roll +1 axis1 -> source col x-1 (mod n) */
                sx = (x + n - 1) % n;
                break;
            case 2: /* D1: roll +1 axis1 then +1 axis0 */
                sy = (y + n - 1) % n;
                sx = (x + n - 1) % n;
                break;
            case 3: /* D2: roll -1 axis1 then +1 axis0 */
            default:
                sy = (y + n - 1) % n;
                sx = (x + 1) % n;
                break;
            }
            out[(size_t)y * n + x] = patch[(size_t)sy * n + sx] * patch[(size_t)y * n + x];
        }
    }
}

/* Copy a square sub-patch out of a float32 MSCN map into a contiguous
 * buffer. `map` has row stride `map_w`; the patch origin is (px, py) of
 * side `n`. */
static void niqe_copy_patch(const float *map, unsigned map_w, unsigned px, unsigned py, unsigned n,
                            float *out)
{
    for (unsigned y = 0; y < n; y++)
        memcpy(out + (size_t)y * n, map + (size_t)(py + y) * map_w + px, (size_t)n * sizeof(float));
}

/* Build the 18-feature vector for one scale of one patch into out[0..17].
 * `patch` is the float32 MSCN sub-patch of side `n`; `paired` is scratch of
 * size n*n. Layout: [alpha_m, (bl_m+br_m)/2, (alpha,N,lsq,rsq) x4]. */
static void niqe_scale_features(const float *patch, float *paired, unsigned n, const double *prec,
                                double *out)
{
    const size_t cnt = (size_t)n * n;
    const NiqeAggd am = niqe_extract_aggd(patch, cnt, prec);
    out[0] = am.alpha;
    out[1] = (am.bl + am.br) / 2.0;

    for (int p = 0; p < 4; p++) {
        niqe_paired_product(patch, paired, n, p);
        const NiqeAggd a = niqe_extract_aggd(paired, cnt, prec);
        out[2 + (size_t)p * 4 + 0] = a.alpha;
        out[2 + (size_t)p * 4 + 1] = a.N;
        out[2 + (size_t)p * 4 + 2] = a.lsq;
        out[2 + (size_t)p * 4 + 3] = a.rsq;
    }
}

/* ------------------------------------------------------------------ */
/* Pooling: sample_mu (column mean), sample_cov (unbiased, ddof=1).     */
/* feat is a P x D row-major matrix (P patches, D = NIQE_FEAT_DIM).     */
/* ------------------------------------------------------------------ */
static void niqe_pool(const double *feat, unsigned P, double *sample_mu, double *sample_cov)
{
    const unsigned D = NIQE_FEAT_DIM;
    for (unsigned d = 0; d < D; d++) {
        double acc = 0.0;
        for (unsigned p = 0; p < P; p++)
            acc += feat[(size_t)p * D + d];
        sample_mu[d] = acc / (double)P;
    }

    /* Unbiased covariance: cov[i][j] = sum_p (x_pi - mu_i)(x_pj - mu_j)/(P-1).
     * np.cov(feats.T) with ddof=1. P >= 2 is guaranteed by the frame guard. */
    const double inv = 1.0 / (double)(P - 1);
    for (unsigned i = 0; i < D; i++) {
        for (unsigned j = i; j < D; j++) {
            double acc = 0.0;
            for (unsigned p = 0; p < P; p++) {
                const double a = feat[(size_t)p * D + i] - sample_mu[i];
                const double b = feat[(size_t)p * D + j] - sample_mu[j];
                acc += a * b;
            }
            const double c = acc * inv;
            sample_cov[(size_t)i * D + j] = c;
            sample_cov[(size_t)j * D + i] = c;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Read the distorted luma plane (uint8 / uint16) into a float64 array. */
/* The harness fed 8-bit luma as double; for >8bpc we feed the raw      */
/* high-bit-depth value (see ADR-1112 high-bit-depth open question — the */
/* MSCN C=1 stabiliser is NOT scale-invariant, so >8bpc parity is a     */
/* documented limitation, not a guarantee).                             */
/* ------------------------------------------------------------------ */
static void niqe_read_luma(const VmafPicture *pic, double *out, unsigned w, unsigned h)
{
    if (pic->bpc <= 8) {
        const uint8_t *src = pic->data[0];
        const ptrdiff_t stride = pic->stride[0];
        for (unsigned y = 0; y < h; y++) {
            const uint8_t *row = src + (size_t)y * (size_t)stride;
            for (unsigned x = 0; x < w; x++)
                out[(size_t)y * w + x] = (double)row[x];
        }
    } else {
        const uint8_t *base = pic->data[0];
        const ptrdiff_t stride = pic->stride[0];
        for (unsigned y = 0; y < h; y++) {
            const uint16_t *row = (const uint16_t *)(base + (size_t)y * (size_t)stride);
            for (unsigned x = 0; x < w; x++)
                out[(size_t)y * w + x] = (double)row[x];
        }
    }
}

/* Build the per-patch (npatches x NIQE_FEAT_DIM) feature matrix into s->feat.
 * Requires s->mscn1 / s->mscn2 already computed. Patch grid is non-overlapping
 * 96x96 on scale 1 and the integer-halved 48x48 sub-patch on scale 2
 * (m_image2[j//2:(j+96)//2, i//2:(i+96)//2]). */
static void niqe_build_feature_matrix(NiqeState *s)
{
    unsigned p = 0;
    for (unsigned jy = 0; jy < s->npy; jy++) {
        const unsigned j = jy * NIQE_PATCH; /* row origin */
        for (unsigned ix = 0; ix < s->npx; ix++) {
            const unsigned i = ix * NIQE_PATCH; /* col origin */
            double *row = s->feat + (size_t)p * NIQE_FEAT_DIM;

            /* Scale 1: 96x96 patch out of mscn1. */
            niqe_copy_patch(s->mscn1, s->w, i, j, NIQE_PATCH, s->patch_buf);
            niqe_scale_features(s->patch_buf, s->paired_buf, NIQE_PATCH, s->gamma_table, row);

            /* Scale 2: 48x48 patch out of mscn2 at integer-halved origin. */
            niqe_copy_patch(s->mscn2, s->w2, i / 2, j / 2, NIQE_HALF_PATCH, s->patch_buf);
            niqe_scale_features(s->patch_buf, s->paired_buf, NIQE_HALF_PATCH, s->gamma_table,
                                row + NIQE_FEATPERSCALE);
            p++;
        }
    }
}

/* Pool s->feat into sample_mu / sample_cov, average with the pristine
 * covariance, pseudo-invert, and return the Mahalanobis distance
 * sqrt(X^T pinv X) with X = sample_mu - mu_pris. Uses scipy.linalg.pinv's
 * default cutoff rtol = NIQE_FEAT_DIM * eps. */
static double niqe_mahalanobis_score(NiqeState *s)
{
    niqe_pool(s->feat, s->npatches, s->sample_mu, s->sample_cov);

    const double *cov_pris = (const double *)niqe_cov_prisparam;
    for (size_t k = 0; k < (size_t)NIQE_FEAT_DIM * NIQE_FEAT_DIM; k++)
        s->avgcov[k] = (cov_pris[k] + s->sample_cov[k]) / 2.0;

    const double rtol = (double)NIQE_FEAT_DIM * DBL_EPSILON; /* scipy.linalg.pinv default */
    (void)niqe_sym_pinv(s->avgcov, s->pinv, NIQE_FEAT_DIM, rtol, s->pinv_work);

    double quad = 0.0;
    for (int i = 0; i < NIQE_FEAT_DIM; i++) {
        const double xi = s->sample_mu[i] - niqe_mu_prisparam[i];
        double acc = 0.0;
        for (int jj = 0; jj < NIQE_FEAT_DIM; jj++) {
            const double xj = s->sample_mu[jj] - niqe_mu_prisparam[jj];
            acc += s->pinv[(size_t)i * NIQE_FEAT_DIM + jj] * xj;
        }
        quad += xi * acc;
    }
    return sqrt(fabs(quad));
}

/* ------------------------------------------------------------------ */
/* Lifecycle: init / extract / close.                                  */
/* ------------------------------------------------------------------ */
/* Allocate every working buffer into `s`. Returns 0 on success, -ENOMEM on
 * the first failed allocation. The caller's close() frees whatever was
 * acquired (aligned_free(NULL) is a no-op), so a partial failure unwinds
 * cleanly. Split out of init() to keep both functions under the ADR-0141
 * function-size threshold. */
static int niqe_alloc_buffers(NiqeState *s, unsigned w, unsigned h)
{
    const size_t plane = (size_t)w * h;
    const size_t plane2 = (size_t)s->w2 * s->h2;
    const unsigned maxax = (s->w2 > s->h2) ? s->w2 : s->h2;
    const size_t d = sizeof(double);
    const size_t fdim = NIQE_FEAT_DIM;

    s->gamma_table = aligned_malloc(NIQE_GAMMA_COUNT * d, 32);
    s->luma = aligned_malloc(plane * d, 32);
    s->tmp = aligned_malloc(plane * d, 32);
    s->mu = aligned_malloc(plane * d, 32);
    s->var = aligned_malloc(plane * d, 32);
    s->luma2 = aligned_malloc(plane2 * d, 32);
    s->tmp2 = aligned_malloc(plane2 * d, 32);
    s->mu2 = aligned_malloc(plane2 * d, 32);
    s->var2 = aligned_malloc(plane2 * d, 32);
    s->mscn1 = aligned_malloc(plane * sizeof(float), 32);
    s->mscn2 = aligned_malloc(plane2 * sizeof(float), 32);
    s->feat = aligned_malloc((size_t)s->npatches * fdim * d, 32);
    s->patch_buf = aligned_malloc((size_t)NIQE_PATCH * NIQE_PATCH * sizeof(float), 32);
    s->paired_buf = aligned_malloc((size_t)NIQE_PATCH * NIQE_PATCH * sizeof(float), 32);
    s->bicubic_bounds = aligned_malloc((size_t)2 * maxax * sizeof(int), 32);
    s->bicubic_coeffs = aligned_malloc((size_t)maxax * NIQE_BICUBIC_MAX_KSIZE * d, 32);
    s->resize_tmp = aligned_malloc((size_t)s->w2 * h * d, 32);
    s->sample_mu = aligned_malloc(fdim * d, 32);
    s->sample_cov = aligned_malloc(fdim * fdim * d, 32);
    s->avgcov = aligned_malloc(fdim * fdim * d, 32);
    s->pinv = aligned_malloc(fdim * fdim * d, 32);
    s->pinv_work = aligned_malloc(2 * fdim * fdim * d, 32);

    if (!s->gamma_table || !s->luma || !s->tmp || !s->mu || !s->var || !s->luma2 || !s->tmp2 ||
        !s->mu2 || !s->var2 || !s->mscn1 || !s->mscn2 || !s->feat || !s->patch_buf ||
        !s->paired_buf || !s->bicubic_bounds || !s->bicubic_coeffs || !s->resize_tmp ||
        !s->sample_mu || !s->sample_cov || !s->avgcov || !s->pinv || !s->pinv_work) {
        return -ENOMEM;
    }
    return 0;
}

static int init(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc, unsigned w,
                unsigned h)
{
    (void)pix_fmt;
    assert(fex != NULL);
    assert(fex->priv != NULL);
    assert(w > 0 && h > 0);
    NiqeState *s = fex->priv;

    /* Frame guard: need >= NIQE_PATCH in each dim and >= 2 total patches
     * (np.cov needs >= 2 samples; see niqe_train_test_model.py:99). */
    if (w < NIQE_PATCH || h < NIQE_PATCH)
        return -EINVAL;
    const unsigned npx = w / NIQE_PATCH;
    const unsigned npy = h / NIQE_PATCH;
    if ((size_t)npx * npy < 2u)
        return -EINVAL;

    s->w = w;
    s->h = h;
    s->bpc = bpc;
    s->npx = npx;
    s->npy = npy;
    s->npatches = npx * npy;
    s->w2 = (unsigned)((double)w / 2.0); /* PIL int(w/2.0) = floor for w>=0 */
    s->h2 = (unsigned)((double)h / 2.0);

    niqe_build_window(s->window);

    const int rc = niqe_alloc_buffers(s, w, h);
    if (rc != 0)
        return rc;

    niqe_build_gamma_table(s->gamma_table);
    return 0;
}

static int extract(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                   VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index,
                   VmafFeatureCollector *feature_collector)
{
    (void)ref_pic;
    (void)ref_pic_90;
    (void)dist_pic_90;
    assert(fex != NULL);
    assert(fex->priv != NULL);
    assert(dist_pic != NULL);
    NiqeState *s = fex->priv;
    assert(feature_collector != NULL);

    /* Stage 1: distorted luma -> float64. */
    niqe_read_luma(dist_pic, s->luma, s->w, s->h);

    /* Stage 2: half-res via PIL-bicubic, then MSCN on both scales. */
    niqe_bicubic_resize(s->luma, s->luma2, s->w, s->h, s->w2, s->h2, s->bicubic_bounds,
                        s->bicubic_coeffs, s->resize_tmp);
    niqe_compute_mscn(s->luma, s->tmp, s->mu, s->var, s->mscn1, s->w, s->h, s->window);
    niqe_compute_mscn(s->luma2, s->tmp2, s->mu2, s->var2, s->mscn2, s->w2, s->h2, s->window);

    /* Stage 3: per-patch 36-feature vectors. */
    niqe_build_feature_matrix(s);

    /* Stage 4: pool + Mahalanobis distance. */
    const double score = niqe_mahalanobis_score(s);

    return vmaf_feature_collector_append(feature_collector, "niqe", score, index);
}

static int close(VmafFeatureExtractor *fex)
{
    NiqeState *s = fex->priv;
    if (!s)
        return 0;
    aligned_free(s->gamma_table);
    aligned_free(s->luma);
    aligned_free(s->tmp);
    aligned_free(s->mu);
    aligned_free(s->var);
    aligned_free(s->luma2);
    aligned_free(s->tmp2);
    aligned_free(s->mu2);
    aligned_free(s->var2);
    aligned_free(s->mscn1);
    aligned_free(s->mscn2);
    aligned_free(s->feat);
    aligned_free(s->patch_buf);
    aligned_free(s->paired_buf);
    aligned_free(s->bicubic_bounds);
    aligned_free(s->bicubic_coeffs);
    aligned_free(s->resize_tmp);
    aligned_free(s->sample_mu);
    aligned_free(s->sample_cov);
    aligned_free(s->avgcov);
    aligned_free(s->pinv);
    aligned_free(s->pinv_work);
    return 0;
}

static const char *provided_features[] = {"niqe", NULL};

/* External linkage is required — the extractor registry iterates over
 * `vmaf_fex_*` externs in core/src/feature/feature_extractor.cpp. */
// NOLINTNEXTLINE(misc-use-internal-linkage,cppcoreguidelines-avoid-non-const-global-variables)
VmafFeatureExtractor vmaf_fex_niqe = {
    .name = "niqe",
    .init = init,
    .extract = extract,
    .close = close,
    .priv_size = sizeof(NiqeState),
    .provided_features = provided_features,
};
