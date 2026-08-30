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
 * Y-FUNQUE+ wavelet-domain ATOM features (atoms-only, no fused SVR score).
 *
 * This extractor ships the three single-channel (luma) wavelet-domain atom
 * features that the official Y-FUNQUE+ model consumes:
 *   - y_funque_plus_ms_ssim : MS-SSIM with covariance pooling
 *   - y_funque_plus_dlm     : detail-loss measure (DLM, scale 2)
 *   - y_funque_plus_mad     : MAD-Ref temporal atom (scale 2)
 *
 * It deliberately does NOT emit a fused MOS-predicting score: the official
 * FUNQUE+ suite trains a per-dataset ScaledSVR at runtime and ships NO frozen
 * regressor weights, so a fused score would be fork-originated. Shipping a
 * fork-trained SVR is a deferred follow-up. See ADR-1114 and
 * docs/metrics/y-funque-plus.md.
 *
 * The algorithm is a clean-room C reimplementation guided by the published
 * papers (arXiv:2304.03412 "One Transform To Compute Them All";
 * arXiv:2202.11241 FUNQUE; Nadenau HVS-CSF; Wang/Simoncelli/Bovik MS-SSIM)
 * and the verified design dossier .workingdir2/rc/metrics/y-funque-plus.md.
 * The reference funque_plus implementation
 * (github.com/abhinaukumar/funque_plus, MIT License, (c) 2023 Abhinau Kumar)
 * is MIT-licensed and therefore compatible with this fork's
 * BSD-2-Clause-Patent; it was used as an algorithmic cross-check only and no
 * Python source was copied verbatim. See ADR-1114 for the license finding.
 *
 * Per-frame pipeline (all double-precision):
 *   reference + distorted luma (plane 0)
 *     ├─ normalize to [0,1] : pixel / ((1<<bpc)-1)
 *     ├─ 2x bicubic downscale : OpenCV INTER_CUBIC (Keys cubic a=-0.75,
 *     │   src coord 2i+0.5, BORDER_REPLICATE edges) — the dominant parity
 *     │   risk; ported bit-faithfully (verified vs cv2 to 1 ulp)
 *     ├─ crop to a multiple of 2^levels from the ORIGINAL pre-resize size:
 *     │   w_crop = (orig_w >> (levels+1)) << levels
 *     ├─ 2-level Haar DWT (pywt 'periodization' boundary)
 *     ├─ CSF-weight the DETAIL subbands only (nadenau_weight, Y channel);
 *     │   approx subbands are never weighted
 *     ├─ MS-SSIM (cov pooling) over both 2-level pyramids
 *     ├─ DLM on the last detail level (decouple + contrast-mask + cube-root
 *     │   pooling; the num pools rest^3 WITHOUT abs, the den pools ref with
 *     │   abs — replicated exactly per pyr_features.py lines 54/61)
 *     └─ MAD-Ref : mean|approx_last[t] - approx_last[t-1]| (0 on frame 0)
 *
 * Bit-depth: luma read as uint8/uint16, cast to double, divided by
 * ((1<<bpc)-1). Only the Y plane is used (U/V ignored). SDR code-value
 * domain — no EOTF; HDR (PQ/HLG) input is scored as-is (documented
 * limitation, see ADR-1114).
 */

#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "libvmaf/picture.h"

#include "config.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "log.h"
#include "mem.h"

/* MSVC and MinGW (-std=c11 -> __STRICT_ANSI__) do not expose M_PI from
 * <math.h>; provide the portable fallback the tree already uses elsewhere
 * (cf. core/src/feature/adm_tools.c, integer_ssim.c) so the Windows builds
 * resolve psi_diff's M_PI. Order-independent — no reliance on _USE_MATH_DEFINES
 * preceding a transitive math.h include. */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Fixed Y-FUNQUE+ recipe constants (verified, ADR-1114). */
#define YF_LEVELS 2u
#define YF_NUM_ATOMS 3

/* SSIM stabilisers: K1=0.01, K2=0.03, max_val=1 -> C1=1e-4, C2=9e-4. */
static const double YF_C1 = 1e-4;
static const double YF_C2 = 9e-4;

/* MS-SSIM scale exponents (Wang/Simoncelli/Bovik). Only the first
 * YF_LEVELS are used. */
static const double YF_MS_EXP[5] = {0.0448, 0.2856, 0.3001, 0.2363, 0.1333};

/* DLM constants (pyr_features.py / dlm_utils.py). */
static const double YF_DLM_BORDER = 0.2;
static const double YF_DLM_EPS = 1e-4;
static const double YF_DECOUPLE_EPS = 1e-30;
static const double YF_CONTRAST_DIV = 30.0;

/* Nadenau CSF (Y channel) — verified analytic form regenerates the official
 * lookup table bit-for-bit to 8 dp (see ADR-1114 / dossier). */
static const double YF_CSF_A = 1.0 / 256.0;
static const double YF_CSF_B = -5.4715e-3;
static const double YF_CSF_C = 1.91;
/* factor = pi * 1080 * 3.0 / 180.0. */
static const double YF_CSF_FACTOR = 56.548667764616276;

/* Orientation factors for [A, H, V, D]. */
static const double YF_ORIENT[4] = {1.0 / 0.85, 1.0, 1.0, 1.0 / (0.85 - 0.15)};

/* One subband plane: contiguous double buffer with explicit dims. */
typedef struct {
    double *data;
    unsigned w, h;
} YFPlane;

/* A single DWT level: approx + 3 detail subbands (H, V, D). */
typedef struct {
    YFPlane approx;
    YFPlane detail[3]; /* H, V, D */
} YFLevel;

typedef struct YFunqueState {
    unsigned bpc;
    unsigned in_w, in_h;     /* original input plane dims */
    unsigned ds_w, ds_h;     /* downscaled dims (in_w/2, in_h/2) */
    unsigned crop_w, crop_h; /* cropped-to-2^levels dims */

    /* working buffers (all sized to the cropped/downscaled extent) */
    double *luma_ref, *luma_dist; /* normalized + downscaled + cropped */
    double *ds_tmp;               /* separable-resize intermediate */
    double *src_norm;             /* full-res normalized scratch */

    YFLevel ref_pyr[YF_LEVELS];
    YFLevel dist_pyr[YF_LEVELS];

    /* MAD-Ref temporal state: previous frame's final approx subband. */
    double *prev_approx;
    unsigned prev_approx_w, prev_approx_h;
    bool have_prev;

    /* DLM atom scratch (9 * last-detail-level), pre-allocated in init() so
     * extract() does no per-frame heap (Power-of-10 §1.3). */
    double *dlm_scratch;

    VmafDictionary *feature_name_dict;
} YFunqueState;

/* ------------------------------------------------------------------ */
/* Nadenau CSF weight for (level, subband). sub: 0=A,1=H,2=V,3=D.     */
/* ------------------------------------------------------------------ */
static double yf_nadenau_weight(unsigned level, unsigned sub)
{
    assert(sub < 4u);
    const double lf = YF_CSF_FACTOR / (double)(1u << (level + 1u));
    const double f = lf * YF_ORIENT[sub];
    return (1.0 - YF_CSF_A) * exp(YF_CSF_B * pow(f, YF_CSF_C)) + YF_CSF_A;
}

/* ------------------------------------------------------------------ */
/* OpenCV interpolateCubic coefficients (Keys cubic, A = -0.75).      */
/* taps for src indices [x-1, x, x+1, x+2] at fractional offset t.    */
/* ------------------------------------------------------------------ */
static void yf_cubic_weights(double t, double w[4])
{
    const double A = -0.75;
    const double t1 = t + 1.0;
    w[0] = ((A * t1 - 5.0 * A) * t1 + 8.0 * A) * t1 - 4.0 * A;
    w[1] = ((A + 2.0) * t - (A + 3.0)) * t * t + 1.0;
    const double s = 1.0 - t;
    w[2] = ((A + 2.0) * s - (A + 3.0)) * s * s + 1.0;
    w[3] = 1.0 - w[0] - w[1] - w[2];
}

/* Clamp an index to [0, n-1] (BORDER_REPLICATE). */
static unsigned yf_clamp_idx(int idx, unsigned n)
{
    assert(n > 0u);
    if (idx < 0)
        return 0u;
    if ((unsigned)idx >= n)
        return n - 1u;
    return (unsigned)idx;
}

/* Separable 2x bicubic downscale (OpenCV INTER_CUBIC). src is
 * src_h x src_w row-major; dst is (src_h/2) x (src_w/2). tmp must hold
 * src_h x (src_w/2). */
static void yf_downscale2(const double *src, unsigned src_w, unsigned src_h, double *tmp,
                          double *dst, unsigned dst_w, unsigned dst_h)
{
    assert(src && tmp && dst);
    assert(dst_w <= src_w && dst_h <= src_h);
    /* Horizontal pass: src_w -> dst_w, per row. */
    for (unsigned y = 0; y < src_h; y++) {
        const double *srow = src + (size_t)y * src_w;
        double *trow = tmp + (size_t)y * dst_w;
        for (unsigned i = 0; i < dst_w; i++) {
            const double fx = ((double)i + 0.5) * 2.0 - 0.5;
            const int sx = (int)floor(fx);
            double w[4];
            yf_cubic_weights(fx - (double)sx, w);
            double acc = 0.0;
            for (int k = 0; k < 4; k++)
                acc += w[k] * srow[yf_clamp_idx(sx - 1 + k, src_w)];
            trow[i] = acc;
        }
    }
    /* Vertical pass: src_h -> dst_h, per column. */
    for (unsigned x = 0; x < dst_w; x++) {
        for (unsigned j = 0; j < dst_h; j++) {
            const double fy = ((double)j + 0.5) * 2.0 - 0.5;
            const int sy = (int)floor(fy);
            double w[4];
            yf_cubic_weights(fy - (double)sy, w);
            double acc = 0.0;
            for (int k = 0; k < 4; k++)
                acc += w[k] * tmp[(size_t)yf_clamp_idx(sy - 1 + k, src_h) * dst_w + x];
            dst[(size_t)j * dst_w + x] = acc;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Single-level orthonormal Haar DWT, pywt 'periodization' convention. */
/* For each 2x2 block [[a,b],[c,d]] (a=TL,b=TR,c=BL,d=BR):             */
/*   cA=(a+b+c+d)/2  cH=(a+b-c-d)/2  cV=(a-b+c-d)/2  cD=(a-b-c+d)/2    */
/* Input dims must be even (guaranteed by the crop to a multiple of    */
/* 2^levels); 'periodization' wrap is therefore the identity here.     */
/* ------------------------------------------------------------------ */
static void yf_haar_level(const double *in, unsigned in_w, unsigned in_h, YFLevel *out)
{
    assert(in && out && out->approx.data);
    /* The crop in init() guarantees even input dims, so the periodization
     * wrap is the identity (no boundary extension needed). */
    assert((in_w % 2u) == 0u && (in_h % 2u) == 0u);
    const unsigned ow = in_w / 2u;
    const unsigned oh = in_h / 2u;
    for (unsigned j = 0; j < oh; j++) {
        const size_t row0 = (size_t)(2u * j) * in_w;
        const size_t row1 = (size_t)(2u * j + 1u) * in_w;
        for (unsigned i = 0; i < ow; i++) {
            const size_t col0 = (size_t)2u * i;
            const double a = in[row0 + col0];
            const double b = in[row0 + col0 + 1u];
            const double c = in[row1 + col0];
            const double d = in[row1 + col0 + 1u];
            const size_t o = (size_t)j * ow + i;
            out->approx.data[o] = (a + b + c + d) * 0.5;
            out->detail[0].data[o] = (a + b - c - d) * 0.5; /* H */
            out->detail[1].data[o] = (a - b + c - d) * 0.5; /* V */
            out->detail[2].data[o] = (a - b - c + d) * 0.5; /* D */
        }
    }
}

/* Apply Nadenau CSF to the detail subbands of one level (in place). */
static void yf_csf_weight_details(YFLevel *lev, unsigned level)
{
    const double wH = yf_nadenau_weight(level, 1u);
    const double wV = yf_nadenau_weight(level, 2u);
    const double wD = yf_nadenau_weight(level, 3u);
    const size_t n = (size_t)lev->detail[0].w * lev->detail[0].h;
    for (size_t k = 0; k < n; k++) {
        lev->detail[0].data[k] *= wH;
        lev->detail[1].data[k] *= wV;
        lev->detail[2].data[k] *= wD;
    }
}

/* Build the full 2-level CSF-weighted pyramid from a cropped plane. */
static void yf_build_pyramid(const double *plane, unsigned w, unsigned h, YFLevel pyr[YF_LEVELS])
{
    const double *cur = plane;
    unsigned cw = w;
    unsigned ch = h;
    for (unsigned lev = 0; lev < YF_LEVELS; lev++) {
        yf_haar_level(cur, cw, ch, &pyr[lev]);
        yf_csf_weight_details(&pyr[lev], lev);
        cur = pyr[lev].approx.data;
        cw = pyr[lev].approx.w;
        ch = pyr[lev].approx.h;
    }
}

/* ------------------------------------------------------------------ */
/* MS-SSIM (covariance pooling). Returns ms_ssim_cov_scales[-1].      */
/* Per scale s: mu from approx subband, var/cov from detail energy.   */
/* cs_cov = std(cs)/mean(cs); ssim_cov = std(ssim_map)/mean(ssim_map).*/
/* Combine: prod over scales 0..L-2 of cs_cov^exp, * ssim_cov^exp[L-1]*/
/* (sign-preserving power).                                           */
/* ------------------------------------------------------------------ */
static double yf_signpow(double x, double e)
{
    const double s = (x < 0.0) ? -1.0 : 1.0;
    return s * pow(fabs(x), e);
}

static double yf_ms_ssim_cov(const YFLevel ref[YF_LEVELS], const YFLevel dist[YF_LEVELS])
{
    double cs_cov[YF_LEVELS];
    double ssim_cov[YF_LEVELS];
    for (unsigned s = 0; s < YF_LEVELS; s++) {
        const unsigned w = ref[s].approx.w;
        const unsigned h = ref[s].approx.h;
        const size_t n = (size_t)w * h;
        assert(n > 0u);

        double sum_cs = 0.0;
        double sum_cs2 = 0.0;
        double sum_ss = 0.0;
        double sum_ss2 = 0.0;
        for (size_t k = 0; k < n; k++) {
            const double mx = ref[s].approx.data[k];
            const double my = dist[s].approx.data[k];
            double var_x = 0.0;
            double var_y = 0.0;
            double cov = 0.0;
            for (unsigned b = 0; b < 3u; b++) {
                const double dx = ref[s].detail[b].data[k];
                const double dy = dist[s].detail[b].data[k];
                var_x += dx * dx;
                var_y += dy * dy;
                cov += dx * dy;
            }
            const double l = (2.0 * mx * my + YF_C1) / (mx * mx + my * my + YF_C1);
            const double cs = (2.0 * cov + YF_C2) / (var_x + var_y + YF_C2);
            const double ssim = l * cs;
            sum_cs += cs;
            sum_cs2 += cs * cs;
            sum_ss += ssim;
            sum_ss2 += ssim * ssim;
        }
        const double mean_cs = sum_cs / (double)n;
        const double mean_ss = sum_ss / (double)n;
        /* population std (numpy default ddof=0). var = E[x^2] - E[x]^2. */
        double var_cs = sum_cs2 / (double)n - mean_cs * mean_cs;
        double var_ss = sum_ss2 / (double)n - mean_ss * mean_ss;
        if (var_cs < 0.0) {
            var_cs = 0.0; /* guard fp rounding below zero */
        }
        if (var_ss < 0.0) {
            var_ss = 0.0;
        }
        /* cov pool = std/mean; guard a zero/near-zero mean (NaN/Inf guard). */
        cs_cov[s] = (fabs(mean_cs) > 0.0) ? sqrt(var_cs) / mean_cs : 0.0;
        ssim_cov[s] = (fabs(mean_ss) > 0.0) ? sqrt(var_ss) / mean_ss : 0.0;
    }

    double acc = 1.0;
    for (unsigned s = 0; s + 1u < YF_LEVELS; s++)
        acc *= yf_signpow(cs_cov[s], YF_MS_EXP[s]);
    acc *= yf_signpow(ssim_cov[YF_LEVELS - 1u], YF_MS_EXP[YF_LEVELS - 1u]);
    return acc;
}

/* ------------------------------------------------------------------ */
/* DLM (scale 2). Operates on the last detail level only.             */
/* ------------------------------------------------------------------ */

/* 3x3 box sum of |signal| with reflect-101 boundary handling
 * (matches the integral-image 3x3 contrast-mask box). */
static double yf_box3_abs(const double *sig, unsigned w, unsigned h, unsigned i, unsigned j)
{
    double acc = 0.0;
    const int i0 = (int)i - 1;
    const int i1 = (int)i + 1;
    const int j0 = (int)j - 1;
    const int j1 = (int)j + 1;
    for (int jj = j0; jj <= j1; jj++) {
        const unsigned yy = yf_clamp_idx(jj, h);
        for (int ii = i0; ii <= i1; ii++) {
            const unsigned xx = yf_clamp_idx(ii, w);
            acc += fabs(sig[(size_t)yy * w + xx]);
        }
    }
    return acc;
}

/* DLM step 1: decouple the distortion into rest (restored) + add (additive)
 * subbands using the psi-angle mask (<1 deg) and the clipped ratio k. */
static void yf_dlm_decouple(const YFLevel *ref_lev, const YFLevel *dist_lev, size_t n, double *rest,
                            double *add)
{
    assert(ref_lev && dist_lev && rest && add);
    assert(n > 0u);
    for (size_t k = 0; k < n; k++) {
        const double Hr = ref_lev->detail[0].data[k];
        const double Vr = ref_lev->detail[1].data[k];
        const double Hd = dist_lev->detail[0].data[k];
        const double Vd = dist_lev->detail[1].data[k];
        const double psi_r = atan(Vr / (Hr + YF_DECOUPLE_EPS)) + ((Hr <= 0.0) ? M_PI : 0.0);
        const double psi_d = atan(Vd / (Hd + YF_DECOUPLE_EPS)) + ((Hd <= 0.0) ? M_PI : 0.0);
        const double psi_diff = 180.0 * fabs(psi_r - psi_d) / M_PI;
        const bool ang_mask = (psi_diff < 1.0);
        for (unsigned b = 0; b < 3u; b++) {
            const double sr = ref_lev->detail[b].data[k];
            const double sd = dist_lev->detail[b].data[k];
            double ratio = sd / (sr + YF_DECOUPLE_EPS);
            if (ratio < 0.0) {
                ratio = 0.0;
            } else if (ratio > 1.0) {
                ratio = 1.0;
            }
            double r = ratio * sr;
            if (ang_mask) {
                r = sd;
            }
            rest[(size_t)b * n + k] = r;
            add[(size_t)b * n + k] = sd - r;
        }
    }
}

/* Per-pixel masking threshold for subband b at linear index k: accumulates
 * the 3x3-box contribution from the OTHER two `add` subbands. */
static double yf_contrast_threshold(const double *add, unsigned b, unsigned w, unsigned h, size_t n,
                                    size_t k)
{
    const unsigned i = (unsigned)(k % w);
    const unsigned j = (unsigned)(k / w);
    double thr = 0.0;
    for (unsigned m = 0; m < 3u; m++) {
        if (m == b) {
            continue;
        }
        const double *am = add + (size_t)m * n;
        const double box = yf_box3_abs(am, w, h, i, j);
        thr += (box + fabs(am[k])) / YF_CONTRAST_DIV;
    }
    return thr;
}

/* DLM step 2: contrast masking. Per subband b, the masked output is
 * clip(|rest| - threshold, 0, +inf). */
static void yf_dlm_contrast_mask(const double *rest, const double *add, unsigned w, unsigned h,
                                 size_t n, double *masked)
{
    for (unsigned b = 0; b < 3u; b++) {
        for (size_t k = 0; k < n; k++) {
            const double thr = yf_contrast_threshold(add, b, w, h, n, k);
            double mv = fabs(rest[(size_t)b * n + k]) - thr;
            if (mv < 0.0) {
                mv = 0.0;
            }
            masked[(size_t)b * n + k] = mv;
        }
    }
}

/* DLM step 3: border-crop + cube-root energy pooling. The numerator pools
 * masked-rest^3 WITHOUT abs (pyr_features.py:54); the denominator pools the
 * CSF-weighted ref detail with abs (pyr_features.py:61). */
static double yf_dlm_pool(const double *masked, const YFLevel *ref_lev, unsigned w, unsigned h,
                          size_t n)
{
    assert(masked && ref_lev);
    assert(n == (size_t)w * h && n > 0u);
    /* Border crop: int(0.2*h) rows top+bottom, int(0.2*w) cols left+right. */
    const unsigned bh = (unsigned)(YF_DLM_BORDER * (double)h);
    const unsigned bw = (unsigned)(YF_DLM_BORDER * (double)w);
    /* Degenerate-crop guard: if the border consumes the whole plane, fall
     * back to the uncropped extent (NaN / empty-sum guard). */
    const bool crop_ok = (2u * bh < h) && (2u * bw < w);
    const unsigned y_lo = crop_ok ? bh : 0u;
    const unsigned y_hi = crop_ok ? (h - bh) : h;
    const unsigned x_lo = crop_ok ? bw : 0u;
    const unsigned x_hi = crop_ok ? (w - bw) : w;

    double num = 0.0;
    double den = 0.0;
    for (unsigned b = 0; b < 3u; b++) {
        double sum_num = 0.0;
        double sum_den = 0.0;
        const double *mb = masked + (size_t)b * n;
        for (unsigned j = y_lo; j < y_hi; j++) {
            for (unsigned i = x_lo; i < x_hi; i++) {
                const double mval = mb[(size_t)j * w + i];
                sum_num += mval * mval * mval;
                const double rd = ref_lev->detail[b].data[(size_t)j * w + i];
                sum_den += fabs(rd * rd * rd);
            }
        }
        num += cbrt(sum_num);
        den += cbrt(sum_den);
    }
    return (num + YF_DLM_EPS) / (den + YF_DLM_EPS);
}

/* Compute the DLM atom on one detail level of both CSF-weighted pyramids.
 * scratch must hold 9*n doubles (rest 3n + add 3n + masked 3n). */
static double yf_dlm(const YFLevel *ref_lev, const YFLevel *dist_lev, double *scratch)
{
    assert(ref_lev && dist_lev && scratch);
    const unsigned w = ref_lev->detail[0].w;
    const unsigned h = ref_lev->detail[0].h;
    const size_t n = (size_t)w * h;
    assert(n > 0u);

    double *rest = scratch;
    double *add = scratch + 3u * n;
    double *masked = scratch + 6u * n;

    yf_dlm_decouple(ref_lev, dist_lev, n, rest, add);
    yf_dlm_contrast_mask(rest, add, w, h, n, masked);
    return yf_dlm_pool(masked, ref_lev, w, h, n);
}

/* ------------------------------------------------------------------ */
/* Picture I/O: read luma plane (uint8/uint16) into normalized double. */
/* ------------------------------------------------------------------ */
static void yf_read_luma_norm(const VmafPicture *pic, double *out, unsigned w, unsigned h)
{
    assert(pic && out && pic->data[0]);
    assert(pic->bpc >= 8u && pic->bpc <= 16u);
    const double inv_range = 1.0 / (double)((1u << pic->bpc) - 1u);
    if (pic->bpc <= 8u) {
        const uint8_t *base = pic->data[0];
        const ptrdiff_t stride = pic->stride[0];
        for (unsigned y = 0; y < h; y++) {
            const uint8_t *row = base + (size_t)y * (size_t)stride;
            for (unsigned x = 0; x < w; x++)
                out[(size_t)y * w + x] = (double)row[x] * inv_range;
        }
    } else {
        const uint8_t *base = pic->data[0];
        const ptrdiff_t stride = pic->stride[0];
        for (unsigned y = 0; y < h; y++) {
            const uint16_t *row = (const uint16_t *)(base + (size_t)y * (size_t)stride);
            for (unsigned x = 0; x < w; x++)
                out[(size_t)y * w + x] = (double)row[x] * inv_range;
        }
    }
}

/* Normalize + downscale + crop one picture into `dst` (crop_w x crop_h). */
static void yf_prepare_plane(YFunqueState *s, const VmafPicture *pic, double *dst)
{
    assert(s && pic && dst);
    assert(s->crop_w <= s->ds_w && s->crop_h <= s->ds_h);
    yf_read_luma_norm(pic, s->src_norm, s->in_w, s->in_h);
    /* Downscale into ds_tmp's row-major full ds extent, then crop. */
    yf_downscale2(s->src_norm, s->in_w, s->in_h, s->ds_tmp, dst /*temp ds*/, s->ds_w, s->ds_h);
    /* yf_downscale2 wrote ds_w x ds_h into dst; now crop in place to
     * crop_w x crop_h (crop is a top-left sub-rectangle, so copy rows). */
    if (s->crop_w != s->ds_w || s->crop_h != s->ds_h) {
        for (unsigned j = 0; j < s->crop_h; j++) {
            memmove(dst + (size_t)j * s->crop_w, dst + (size_t)j * s->ds_w,
                    (size_t)s->crop_w * sizeof(double));
        }
    }
}

/* ------------------------------------------------------------------ */
/* Allocation helpers for the pyramid planes.                          */
/* ------------------------------------------------------------------ */
static int yf_alloc_level(YFLevel *lev, unsigned w, unsigned h)
{
    const unsigned ow = w / 2u;
    const unsigned oh = h / 2u;
    const size_t n = (size_t)ow * oh;
    lev->approx.data = malloc(n * sizeof(double));
    lev->approx.w = ow;
    lev->approx.h = oh;
    if (!lev->approx.data)
        return -ENOMEM;
    for (unsigned b = 0; b < 3u; b++) {
        lev->detail[b].data = malloc(n * sizeof(double));
        lev->detail[b].w = ow;
        lev->detail[b].h = oh;
        if (!lev->detail[b].data)
            return -ENOMEM;
    }
    return 0;
}

static void yf_free_level(YFLevel *lev)
{
    free(lev->approx.data);
    lev->approx.data = NULL;
    for (unsigned b = 0; b < 3u; b++) {
        free(lev->detail[b].data);
        lev->detail[b].data = NULL;
    }
}

static void yf_free_all(YFunqueState *s)
{
    free(s->luma_ref);
    s->luma_ref = NULL;
    free(s->luma_dist);
    s->luma_dist = NULL;
    free(s->ds_tmp);
    s->ds_tmp = NULL;
    free(s->src_norm);
    s->src_norm = NULL;
    free(s->prev_approx);
    s->prev_approx = NULL;
    free(s->dlm_scratch);
    s->dlm_scratch = NULL;
    for (unsigned lev = 0; lev < YF_LEVELS; lev++) {
        yf_free_level(&s->ref_pyr[lev]);
        yf_free_level(&s->dist_pyr[lev]);
    }
}

/* DLM scratch is 9*n doubles where n = cells in the last detail level. */
static double *yf_alloc_dlm_scratch(const YFunqueState *s)
{
    const unsigned w = s->ref_pyr[YF_LEVELS - 1u].detail[0].w;
    const unsigned h = s->ref_pyr[YF_LEVELS - 1u].detail[0].h;
    const size_t n = (size_t)w * h;
    return malloc(9u * n * sizeof(double));
}

/* Allocate every working buffer + both 2-level pyramids sized to the dims
 * already stored in `s`. Returns 0 on success, -ENOMEM on any failure (the
 * caller unwinds via yf_free_all). Pulled out of init() so each function stays
 * within the readability-function-size budget. */
static int yf_alloc_buffers(YFunqueState *s)
{
    assert(s);
    assert(s->ds_w > 0u && s->ds_h > 0u && s->crop_w > 0u && s->crop_h > 0u);
    /* src_norm holds the full-res normalized plane; ds_tmp holds the
     * horizontal-pass intermediate (in_h x ds_w). The luma_* buffers are
     * sized to the downscale target (ds_w x ds_h), which is always >= the
     * final crop, so the in-place crop in yf_prepare_plane fits. */
    s->src_norm = malloc((size_t)s->in_w * s->in_h * sizeof(double));
    s->ds_tmp = malloc((size_t)s->in_h * s->ds_w * sizeof(double));
    s->luma_ref = malloc((size_t)s->ds_w * s->ds_h * sizeof(double));
    s->luma_dist = malloc((size_t)s->ds_w * s->ds_h * sizeof(double));
    if (!s->src_norm || !s->ds_tmp || !s->luma_ref || !s->luma_dist)
        return -ENOMEM;

    for (unsigned lev = 0; lev < YF_LEVELS; lev++) {
        const unsigned lw = (lev == 0u) ? s->crop_w : s->ref_pyr[lev - 1u].approx.w;
        const unsigned lh = (lev == 0u) ? s->crop_h : s->ref_pyr[lev - 1u].approx.h;
        if (yf_alloc_level(&s->ref_pyr[lev], lw, lh))
            return -ENOMEM;
        if (yf_alloc_level(&s->dist_pyr[lev], lw, lh))
            return -ENOMEM;
    }
    /* Pre-allocate the DLM scratch (the last-detail-level dims are set by the
     * loop above) so extract() does zero per-frame heap. */
    s->dlm_scratch = yf_alloc_dlm_scratch(s);
    if (!s->dlm_scratch)
        return -ENOMEM;
    return 0;
}

static int init(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc, unsigned w,
                unsigned h)
{
    (void)pix_fmt;
    assert(fex && fex->priv);
    assert(bpc >= 8u && bpc <= 16u);
    YFunqueState *s = fex->priv;
    s->bpc = bpc;
    s->in_w = w;
    s->in_h = h;

    /* 2x bicubic downscale, then crop to a multiple of 2^levels using the
     * ORIGINAL pre-resize dims (matches the reference crop formula). */
    s->ds_w = w / 2u;
    s->ds_h = h / 2u;
    s->crop_w = (w >> (YF_LEVELS + 1u)) << YF_LEVELS;
    s->crop_h = (h >> (YF_LEVELS + 1u)) << YF_LEVELS;

    /* The cropped extent must be at least 2^levels in each axis so the
     * 2-level DWT yields a non-empty final level. Refuse otherwise. */
    const unsigned min_dim = 1u << YF_LEVELS; /* 4 for levels=2 */
    if (s->crop_w < min_dim || s->crop_h < min_dim || s->ds_w == 0u || s->ds_h == 0u) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "y_funque_plus: frame %ux%u too small; cropped extent %ux%u is below the "
                 "%u-level Haar minimum %ux%u\n",
                 w, h, s->crop_w, s->crop_h, YF_LEVELS, min_dim, min_dim);
        return -EINVAL;
    }

    if (yf_alloc_buffers(s))
        goto fail;

    s->have_prev = false;
    s->prev_approx = NULL;
    s->prev_approx_w = s->ref_pyr[YF_LEVELS - 1u].approx.w;
    s->prev_approx_h = s->ref_pyr[YF_LEVELS - 1u].approx.h;

    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (!s->feature_name_dict)
        goto fail;

    return 0;

fail:
    yf_free_all(s);
    return -ENOMEM;
}

/* MAD-Ref atom: mean|ref_approx_last[t] - ref_approx_last[t-1]|; 0 on the
 * first frame. Caches the current ref approx for the next call. Returns the
 * atom in *out_mad; returns -ENOMEM if the cache cannot be allocated. */
static int yf_mad_ref(YFunqueState *s, const YFPlane *cur_approx, double *out_mad)
{
    assert(s && cur_approx && out_mad && cur_approx->data);
    const size_t na = (size_t)cur_approx->w * cur_approx->h;
    assert(na > 0u);
    double mad = 0.0;
    if (s->have_prev && s->prev_approx) {
        double sum = 0.0;
        for (size_t k = 0; k < na; k++) {
            sum += fabs(cur_approx->data[k] - s->prev_approx[k]);
        }
        mad = sum / (double)na;
    }
    if (!s->prev_approx) {
        s->prev_approx = malloc(na * sizeof(double));
        if (!s->prev_approx) {
            return -ENOMEM;
        }
    }
    memcpy(s->prev_approx, cur_approx->data, na * sizeof(double));
    s->have_prev = true;
    *out_mad = mad;
    return 0;
}

static int extract(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                   VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index,
                   VmafFeatureCollector *feature_collector)
{
    assert(fex && fex->priv && ref_pic && dist_pic && feature_collector);
    YFunqueState *s = fex->priv;
    (void)ref_pic_90;
    (void)dist_pic_90;
    int err = 0;

    /* Defensive: dims must match what init() saw. */
    if (ref_pic->w[0] != s->in_w || ref_pic->h[0] != s->in_h) {
        return -EINVAL;
    }

    /* Normalize + downscale + crop both planes, then build the 2-level
     * CSF-weighted Haar pyramids. */
    yf_prepare_plane(s, ref_pic, s->luma_ref);
    yf_prepare_plane(s, dist_pic, s->luma_dist);
    yf_build_pyramid(s->luma_ref, s->crop_w, s->crop_h, s->ref_pyr);
    yf_build_pyramid(s->luma_dist, s->crop_w, s->crop_h, s->dist_pyr);

    /* Atom 1: MS-SSIM (cov pooling). */
    const double ms_ssim = yf_ms_ssim_cov(s->ref_pyr, s->dist_pyr);

    /* Atom 2: DLM on the last detail level (scratch pre-allocated in init()). */
    const double dlm =
        yf_dlm(&s->ref_pyr[YF_LEVELS - 1u], &s->dist_pyr[YF_LEVELS - 1u], s->dlm_scratch);

    /* Atom 3: MAD-Ref (temporal). */
    double mad = 0.0;
    err = yf_mad_ref(s, &s->ref_pyr[YF_LEVELS - 1u].approx, &mad);
    if (err) {
        return err;
    }

    /* NaN/Inf guard: the divisions above are stabilised, but guard before
     * emitting in case of pathological input. */
    if (!isfinite(ms_ssim) || !isfinite(dlm) || !isfinite(mad)) {
        vmaf_log(VMAF_LOG_LEVEL_WARNING,
                 "y_funque_plus: non-finite atom at frame %u (ms_ssim=%g dlm=%g mad=%g)\n", index,
                 ms_ssim, dlm, mad);
        return -EINVAL;
    }

    err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "y_funque_plus_ms_ssim", ms_ssim, index);
    err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "y_funque_plus_dlm", dlm, index);
    err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "y_funque_plus_mad", mad, index);
    return err;
}

static int close_fex(VmafFeatureExtractor *fex)
{
    YFunqueState *s = fex->priv;
    yf_free_all(s);
    vmaf_dictionary_free(&s->feature_name_dict);
    return 0;
}

static const char *provided_features[] = {"y_funque_plus_ms_ssim", "y_funque_plus_dlm",
                                          "y_funque_plus_mad", NULL};

/* Registry symbol: must have external linkage so feature_extractor.cpp can
 * resolve it via `extern VmafFeatureExtractor vmaf_fex_y_funque_plus`. The
 * mutable-global is the established extractor-registration pattern shared by
 * every extractor in the tree (e.g. ssimulacra2.c, niqe.c); making it static
 * would break the registry. Suppressed per the same load-bearing-invariant
 * carve-out those files use (ADR-0141 / ADR-1114). */
// NOLINTNEXTLINE(misc-use-internal-linkage,cppcoreguidelines-avoid-non-const-global-variables)
VmafFeatureExtractor vmaf_fex_y_funque_plus = {
    .name = "y_funque_plus",
    .init = init,
    .extract = extract,
    .close = close_fex,
    .priv_size = sizeof(YFunqueState),
    .provided_features = provided_features,
    /* TEMPORAL: the MAD-Ref atom needs ordered frames + a previous-frame
     * approx cache (frame 0 reports 0). PREV_REF is not used because the
     * cached state is the wavelet approx subband, not the raw picture. */
    .flags = VMAF_FEATURE_EXTRACTOR_TEMPORAL,
};
