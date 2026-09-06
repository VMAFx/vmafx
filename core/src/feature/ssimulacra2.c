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
 * SSIMULACRA 2 — scalar C port of the libjxl reference implementation.
 *
 * Reference: https://github.com/libjxl/libjxl/blob/main/tools/ssimulacra2.cc
 * Algorithm designed by Jon Sneyers (Cloudinary), July 2022 / April 2023.
 *
 * Per-frame pipeline:
 *   YUV (8/10/12-bit, 420/422/444) ──► upsample chroma to luma resolution
 *     └► BT.709 limited-range matrix → non-linear sRGB in [0, 1]
 *        └► sRGB EOTF → linear RGB
 *           └► opsin LMS mixing matrix + cube-root + bias → XYB
 *              └► MakePositiveXYB rescale (B -= Y-0.55, X = 14X+0.42, Y += 0.01)
 *                 └► 6-scale pyramid (2x2 box downsample in linear RGB)
 *                    └► per scale:
 *                         libjxl FastGaussian 3-pole IIR (sigma=1.5, zero-pad)
 *                         → SSIMMap (no-gamma variant, kC2=0.0009)
 *                         → EdgeDiffMap (ringing + blurring error)
 *                    └► 108 weighted norms + polynomial + pow → 0..100
 *
 * SIMD variants ship under ADR-0161 (AVX2), ADR-0162 (AVX-512 + NEON),
 * and ADR-0164 (snapshot gate). GPU twins ship under ADR-0201 (Vulkan)
 * and ADR-0206 (CUDA + SYCL).
 */

// NOLINTNEXTLINE(misc-include-cleaner) — ENOMEM at line ~979 in init() bailouts.
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
#include "feature/ssimulacra2_math.h"
#include "feature/ssimulacra2_simd_common.h"
#include "mem.h"
#include "opt.h"

#if ARCH_X86 || ARCH_AARCH64
#include "cpu.h"
#endif
#if ARCH_X86
#include "feature/x86/ssimulacra2_avx2.h"
#if HAVE_AVX512
#include "feature/x86/ssimulacra2_avx512.h"
#endif
#include "x86/cpu.h"
#endif
#if ARCH_AARCH64
#include "feature/arm64/ssimulacra2_neon.h"
#if HAVE_SVE2
#include "feature/arm64/ssimulacra2_sve2.h"
#endif
#endif

/*
 * ---------------------------------------------------------------
 *  Constants — kept identical to the libjxl tools/ssimulacra2.cc
 *  reference. Any change here must be validated against the Python
 *  port (github.com/Pacidus/ssimulacra2) AND re-snapshotted.
 * ---------------------------------------------------------------
 */

static const int kNumScales = 6;
static const float kC2 = 0.0009f;

/* Opsin absorbance mixing matrix (row-major 3x3, from libjxl
 * opsin_params.h). Converts linear sRGB to mixed LMS. */
static const float kM00 = 0.30f;
static const float kM02 = 0.078f;
static const float kM10 = 0.23f;
static const float kM12 = 0.078f;
static const float kM20 = 0.24342268924547819f;
static const float kM21 = 0.20476744424496821f;

static const float kOpsinBias = 0.0037930732552754493f;

/* BT.709 limited-range YUV → non-linear-RGB matrix, 8-bit reference.
 * Scaled so the code path normalises Y to [16/255, 235/255] and
 * Cb/Cr to [16/255, 240/255] before this matrix is applied. */

/* 108 pooling weights (libjxl tools/ssimulacra2.cc:301). */
static const double kWeights[108] = {
    0.0,
    0.0007376606707406586,
    0.0,
    0.0,
    0.0007793481682867309,
    0.0,
    0.0,
    0.0004371155730107379,
    0.0,
    1.1041726426657346,
    0.00066284834129271,
    0.00015231632783718752,
    0.0,
    0.0016406437456599754,
    0.0,
    1.8422455520539298,
    11.441172603757666,
    0.0,
    0.0007989109436015163,
    0.000176816438078653,
    0.0,
    1.8787594979546387,
    10.94906990605142,
    0.0,
    0.0007289346991508072,
    0.9677937080626833,
    0.0,
    0.00014003424285435884,
    0.9981766977854967,
    0.00031949755934435053,
    0.0004550992113792063,
    0.0,
    0.0,
    0.0013648766163243398,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    7.466890328078848,
    0.0,
    17.445833984131262,
    0.0006235601634041466,
    0.0,
    0.0,
    6.683678146179332,
    0.00037724407979611296,
    1.027889937768264,
    225.20515300849274,
    0.0,
    0.0,
    19.213238186143016,
    0.0011401524586618361,
    0.001237755635509985,
    176.39317598450694,
    0.0,
    0.0,
    24.43300999870476,
    0.28520802612117757,
    0.0004485436923833408,
    0.0,
    0.0,
    0.0,
    34.77906344483772,
    44.835625328877896,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0008680556573291698,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0005313191874358747,
    0.0,
    0.00016533814161379112,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0004179171803251336,
    0.0017290828234722833,
    0.0,
    0.0020827005846636437,
    0.0,
    0.0,
    8.826982764996862,
    23.19243343998926,
    0.0,
    95.1080498811086,
    0.9863978034400682,
    0.9834382792465353,
    0.0012286405048278493,
    171.2667255897307,
    0.9807858872435379,
    0.0,
    0.0,
    0.0,
    0.0005130064588990679,
    0.0,
    0.00010854057858411537,
};

/* Gaussian blur is implemented as libjxl's recursive 3-pole IIR
 * ("truncated cosine" approximation). Bit-close to the libjxl C++
 * reference (tools/ssimulacra2 + lib/jxl/gauss_blur.cc). Sigma=1.5
 * is fixed, matching libjxl's kSigma in tools/ssimulacra2.cc. */
#define SSIMU2_SIGMA 1.5
#define SSIMU2_PI 3.14159265358979323846

/*
 * ---------------------------------------------------------------
 *  Enums for options
 * ---------------------------------------------------------------
 */

enum yuv_matrix {
    YUV_MATRIX_BT709_LIMITED = 0,
    YUV_MATRIX_BT601_LIMITED = 1,
    YUV_MATRIX_BT709_FULL = 2,
    YUV_MATRIX_BT601_FULL = 3,
};

/*
 * ---------------------------------------------------------------
 *  State
 * ---------------------------------------------------------------
 */

typedef struct Ssimu2State {
    /* Option: integer (enum yuv_matrix), default BT.709 limited. */
    int yuv_matrix;

    /* Geometry */
    unsigned w, h;
    unsigned bpc;
    float sample_scale; /* 1 / ((1<<bpc) - 1) */

    /* Pre-computed 3-pole recursive Gaussian coefficients (sigma=1.5). */
    float rg_n2[3]; /* k = {1, 3, 5} */
    float rg_d1[3];
    int rg_radius;

    /* Working buffers sized for the full-resolution frame. Per-scale
     * views reuse the same storage with smaller (w, h). Each buffer is
     * planar: R | G | B (or X | Y | B) stored contiguously. */
    float *ref_lin; /* linear RGB, used to derive per-scale XYB + downsample */
    float *dist_lin;
    float *ref_xyb; /* XYB for current scale */
    float *dist_xyb;
    float *mu1;       /* blur(ref_xyb) */
    float *mu2;       /* blur(dist_xyb) */
    float *sigma1_sq; /* blur(ref_xyb^2) */
    float *sigma2_sq; /* blur(dist_xyb^2) */
    float *sigma12;   /* blur(ref_xyb*dist_xyb) */
    float *scratch;   /* horizontal-pass temp for separable IIR (one plane). */
    float *col_state; /* 6 * w floats: per-column IIR state for vertical pass. */
    float *mul_buf;   /* temp product buffer */

    /* Capacities in pixels-per-plane (== w * h at full scale). */
    size_t cap_plane;

    /* SIMD dispatch. Function pointers assigned in init() based on runtime
     * CPU flags. Scalar fallback is always available; SIMD variants live
     * in x86/ssimulacra2_{avx2,avx512}.c and arm64/ssimulacra2_neon.c and
     * must produce byte-identical output to scalar (ADR-0161). */
    void (*mul3_fn)(const float *a, const float *b, float *mul, unsigned w, unsigned h);
    void (*xyb_fn)(const float *lin, float *xyb, unsigned w, unsigned h);
    void (*down_fn)(const float *in, unsigned iw, unsigned ih, float *out, unsigned *ow,
                    unsigned *oh);
    void (*ssim_fn)(const float *m1, const float *m2, const float *s11, const float *s22,
                    const float *s12, unsigned w, unsigned h, double plane_averages[6]);
    void (*edge_fn)(const float *img1, const float *mu1, const float *img2, const float *mu2,
                    unsigned w, unsigned h, double plane_averages[12]);
    /* SIMD blur: NULL => scalar fallback via blur_plane; non-NULL replaces
     * blur_plane + fast_gaussian_1d with one SIMD entry point (ADR-0162). */
    void (*blur_fn)(const float rg_n2[3], const float rg_d1[3], int rg_radius, float *col_state,
                    const float *in, float *out, float *scratch, unsigned w, unsigned h);
    /* SIMD YUV → linear RGB (ADR-0163). NULL => scalar fallback. */
    void (*ptlr_fn)(int yuv_matrix, unsigned bpc, unsigned w, unsigned h,
                    const simd_plane_t planes[3], float *out);
} Ssimu2State;

/*
 * ---------------------------------------------------------------
 *  Options
 * ---------------------------------------------------------------
 */

static const VmafOption options[] = {
    {
        .name = "yuv_matrix",
        .help = "YUV→RGB matrix: 0=bt709_limited (default), 1=bt601_limited, "
                "2=bt709_full, 3=bt601_full",
        .offset = offsetof(Ssimu2State, yuv_matrix),
        .type = VMAF_OPT_TYPE_INT,
        .default_val.i = YUV_MATRIX_BT709_LIMITED,
        .min = 0,
        .max = 3,
    },
    {0}};

/*
 * ---------------------------------------------------------------
 *  Helpers
 * ---------------------------------------------------------------
 */

static inline float clampf(float v, float lo, float hi)
{
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

/* sRGB → linear (matches IEC 61966-2-1). Uses the fork's deterministic
 * LUT-based EOTF (ADR-0164) instead of libm `powf` so output is
 * host-independent across glibc / musl / macOS libSystem. */
static inline float srgb_to_linear(float v)
{
    return vmaf_ss2_srgb_eotf(v);
}

/* Port of libjxl CreateRecursiveGaussian (lib/jxl/gauss_blur.cc).
 * Charalampidis [2016] "Recursive Implementation of the Gaussian Filter
 * Using Truncated Cosine Functions", with k={1,3,5}. Writes n2[], d1[],
 * and radius into the state; the full IIR pass uses symmetric-sum
 * recurrence `out_k = n2[k]*sum - d1[k]*prev_k - prev2_k`. */
/* ADR-0130-era scalar, pre-existing pre-touched-file rule. */
// NOLINTNEXTLINE(readability-function-size,google-readability-function-size) — paired SIMD ports (avx2/avx512/neon/sve2) bit-exact-match this scalar; splitting would force matching splits in 4 SIMD files (ADR-0141)
static void create_recursive_gaussian(Ssimu2State *s, double sigma)
{
    const double radius = round(3.2795 * sigma + 0.2546); /* (57), "N" */

    const double pi_div_2r = SSIMU2_PI / (2.0 * radius);
    const double omega[3] = {pi_div_2r, 3.0 * pi_div_2r, 5.0 * pi_div_2r};

    /* (37): poles p_k */
    const double p1 = +1.0 / tan(0.5 * omega[0]);
    const double p3 = -1.0 / tan(0.5 * omega[1]);
    const double p5 = +1.0 / tan(0.5 * omega[2]);

    /* (44): residues r_k */
    const double r1 = +p1 * p1 / sin(omega[0]);
    const double r3 = -p3 * p3 / sin(omega[1]);
    const double r5 = +p5 * p5 / sin(omega[2]);

    /* (50): rho_k */
    const double neg_half_sigma2 = -0.5 * sigma * sigma;
    const double recip_r = 1.0 / radius;
    double rho[3];
    for (int i = 0; i < 3; i++) {
        rho[i] = exp(neg_half_sigma2 * omega[i] * omega[i]) * recip_r;
    }

    /* (52): determinants and zetas */
    const double D13 = p1 * r3 - r1 * p3;
    const double D35 = p3 * r5 - r3 * p5;
    const double D51 = p5 * r1 - r5 * p1;
    const double recip_d13 = 1.0 / D13;
    const double zeta_15 = D35 * recip_d13;
    const double zeta_35 = D51 * recip_d13;

    /* A * beta = gamma; solve via Cramer's rule. */
    const double A[3][3] = {{p1, p3, p5}, {r1, r3, r5}, {zeta_15, zeta_35, 1.0}};
    const double gamma[3] = {1.0, radius * radius - sigma * sigma, /* (55) */
                             zeta_15 * rho[0] + zeta_35 * rho[1] + rho[2]};

    const double det_A = A[0][0] * (A[1][1] * A[2][2] - A[1][2] * A[2][1]) -
                         A[0][1] * (A[1][0] * A[2][2] - A[1][2] * A[2][0]) +
                         A[0][2] * (A[1][0] * A[2][1] - A[1][1] * A[2][0]);
    const double inv_det = 1.0 / det_A;

    double beta[3];
    for (int col = 0; col < 3; col++) {
        double M[3][3];
        for (int i = 0; i < 3; i++) {
            for (int j = 0; j < 3; j++) {
                M[i][j] = A[i][j];
            }
        }
        for (int i = 0; i < 3; i++) {
            M[i][col] = gamma[i];
        }
        beta[col] = (M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1]) -
                     M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0]) +
                     M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0])) *
                    inv_det;
    }

    /* (33): final coefficients */
    s->rg_radius = (int)radius;
    for (int i = 0; i < 3; i++) {
        s->rg_n2[i] = (float)(-beta[i] * cos(omega[i] * (radius + 1.0)));
        s->rg_d1[i] = (float)(-2.0 * cos(omega[i]));
    }
}

/*
 * ---------------------------------------------------------------
 *  YUV (any bpc / subsampling) → linear-RGB (float, planar R|G|B)
 * ---------------------------------------------------------------
 */

/* Read pixel from plane with bpc-aware and subsample-aware access.
 * For 4:2:0 / 4:2:2 the chroma plane is at lower resolution; we
 * nearest-sample by rounding x/y down. */
static inline float read_plane(const VmafPicture *pic, int plane, int x, int y)
{
    unsigned pw = pic->w[plane];
    unsigned ph = pic->h[plane];
    unsigned lw = pic->w[0];
    unsigned lh = pic->h[0];
    int sx;
    int sy;
    if (pw == lw) {
        sx = x;
    } else if (pw * 2 == lw) {
        sx = x >> 1;
    } else {
        sx = (int)((int64_t)x * (int64_t)pw / (int64_t)lw);
    }
    if (ph == lh) {
        sy = y;
    } else if (ph * 2 == lh) {
        sy = y >> 1;
    } else {
        sy = (int)((int64_t)y * (int64_t)ph / (int64_t)lh);
    }
    if (sx < 0)
        sx = 0;
    if (sy < 0)
        sy = 0;
    if ((unsigned)sx >= pw)
        sx = (int)pw - 1;
    if ((unsigned)sy >= ph)
        sy = (int)ph - 1;

    if (pic->bpc > 8) {
        const uint16_t *row =
            (const uint16_t *)((const uint8_t *)pic->data[plane] + (size_t)sy * pic->stride[plane]);
        return (float)row[sx];
    }
    const uint8_t *row = (const uint8_t *)pic->data[plane] + (size_t)sy * pic->stride[plane];
    return (float)row[sx];
}

/* Apply YUV matrix → non-linear sRGB in [0,1], then sRGB EOTF → linear RGB.
 * Writes planar R|G|B, each plane w*h contiguous floats at `out`. */
/* ADR-0141 carve-out: the matmul + per-pixel YUV-matrix dispatch +
 * clamp + per-lane sRGB EOTF is a line-for-line port of the libjxl
 * scalar reference path — splitting would break the scalar-diff
 * audit story. */
// NOLINTNEXTLINE(readability-function-size,google-readability-function-size) — paired SIMD ports (avx2/avx512/neon/sve2) bit-exact-match this scalar; splitting would force matching splits in 4 SIMD files (ADR-0141)
static void picture_to_linear_rgb(const Ssimu2State *s, const VmafPicture *pic, float *out)
{
    const unsigned w = s->w;
    const unsigned h = s->h;
    const size_t plane_sz = (size_t)w * (size_t)h;
    float *rp = out;
    float *gp = out + plane_sz;
    float *bp = out + 2 * plane_sz;

    const float peak = (float)((1u << s->bpc) - 1u);
    const float inv_peak = 1.0f / peak;

    /* BT.709 limited-range (8-bit reference): Y in [16,235], C in [16,240]. */
    float kr;
    float kg;
    float kb;
    float ky;
    float kcb_r;
    float kcb_g;
    float kcr_g;
    float kcr_r;
    int limited = 1;
    switch (s->yuv_matrix) {
    case YUV_MATRIX_BT709_FULL:
        limited = 0;
        /* fall through */
    case YUV_MATRIX_BT709_LIMITED:
        kr = 0.2126f;
        kg = 0.7152f;
        kb = 0.0722f;
        break;
    case YUV_MATRIX_BT601_FULL:
        limited = 0;
        /* fall through */
    case YUV_MATRIX_BT601_LIMITED:
    default:
        kr = 0.299f;
        kg = 0.587f;
        kb = 0.114f;
        break;
    }
    (void)kg;
    ky = kr;
    kcb_r = kb;
    kcb_g = kg;
    kcr_g = kr;
    kcr_r = kb;
    (void)ky;
    (void)kcb_r;
    (void)kcb_g;
    (void)kcr_g;
    (void)kcr_r;

    /* Derived coefficients for the standard YCbCr → RGB inverse:
     *   R = Y                + 2(1-kr) * V
     *   B = Y + 2(1-kb) * U
     *   G = Y - (2kb(1-kb)/kg) U - (2kr(1-kr)/kg) V
     */
    const float cr_r = 2.0f * (1.0f - kr);
    const float cb_b = 2.0f * (1.0f - kb);
    const float cb_g = -(2.0f * kb * (1.0f - kb)) / kg;
    const float cr_g = -(2.0f * kr * (1.0f - kr)) / kg;

    const float y_scale = limited ? (255.0f / 219.0f) : 1.0f;
    const float c_scale = limited ? (255.0f / 224.0f) : 1.0f;
    const float y_off = limited ? (16.0f / 255.0f) : 0.0f;
    const float c_off = 0.5f;

    for (unsigned y = 0; y < h; y++) {
        for (unsigned x = 0; x < w; x++) {
            float Y = read_plane(pic, 0, (int)x, (int)y) * inv_peak;
            float U = read_plane(pic, 1, (int)x, (int)y) * inv_peak;
            float V = read_plane(pic, 2, (int)x, (int)y) * inv_peak;

            /* Normalise to [0,1] luma / centered-at-0 chroma. */
            float Yn = (Y - y_off) * y_scale;
            float Un = (U - c_off) * c_scale;
            float Vn = (V - c_off) * c_scale;

            /* ADR-0891 FMA unification: the AVX2 / AVX-512 / NEON / SVE2
             * kernels and their scalar tails all use a single-rounded
             * fused multiply-add here. This copy was missed when ADR-0891
             * landed, so it produced a mul-then-add result that differs
             * from the SIMD paths by ~1 ULP. The ssimulacra2 pipeline is
             * ill-conditioned downstream (the edge-diff term takes
             * |img - blur(img)|, a catastrophic cancellation, and the
             * 4-norm pooling amplifies the survivors), so that 1 ULP grew
             * into a 2.6e-3 score delta. Keep these three lines
             * fmaf()-based and in this exact order. See ADR-1205. */
            float R = fmaf(cr_r, Vn, Yn);
            float G = fmaf(cb_g, Un, Yn);
            G = fmaf(cr_g, Vn, G);
            float B = fmaf(cb_b, Un, Yn);

            R = clampf(R, 0.0f, 1.0f);
            G = clampf(G, 0.0f, 1.0f);
            B = clampf(B, 0.0f, 1.0f);

            const size_t idx = (size_t)y * w + x;
            rp[idx] = srgb_to_linear(R);
            gp[idx] = srgb_to_linear(G);
            bp[idx] = srgb_to_linear(B);
        }
    }
}

/*
 * ---------------------------------------------------------------
 *  Linear RGB → XYB (with MakePositiveXYB rescale folded in)
 * ---------------------------------------------------------------
 */

static void linear_rgb_to_xyb(const float *lin, float *xyb, unsigned w, unsigned h)
{
    const size_t plane_sz = (size_t)w * (size_t)h;
    const float *rp = lin;
    const float *gp = lin + plane_sz;
    const float *bp = lin + 2 * plane_sz;
    float *xp = xyb;
    float *yp = xyb + plane_sz;
    float *bxp = xyb + 2 * plane_sz;

    const float m01 = 1.0f - kM00 - kM02;
    const float m11 = 1.0f - kM10 - kM12;
    const float m22 = 1.0f - kM20 - kM21;
    const float cbrt_bias = vmaf_ss2_cbrtf(kOpsinBias);

    for (size_t i = 0; i < plane_sz; i++) {
        float r = rp[i];
        float g = gp[i];
        float b = bp[i];
        float l = kM00 * r + m01 * g + kM02 * b + kOpsinBias;
        float m = kM10 * r + m11 * g + kM12 * b + kOpsinBias;
        float s = kM20 * r + kM21 * g + m22 * b + kOpsinBias;

        if (l < 0.0f)
            l = 0.0f;
        if (m < 0.0f)
            m = 0.0f;
        if (s < 0.0f)
            s = 0.0f;

        float L = vmaf_ss2_cbrtf(l) - cbrt_bias;
        float M = vmaf_ss2_cbrtf(m) - cbrt_bias;
        float S = vmaf_ss2_cbrtf(s) - cbrt_bias;

        float X = 0.5f * (L - M);
        float Y = 0.5f * (L + M);
        float B = S;

        /* MakePositiveXYB rescale, in the libjxl order
         * (B uses Y **before** Y is offset). */
        B = (B - Y) + 0.55f;
        X = X * 14.0f + 0.42f;
        Y = Y + 0.01f;

        xp[i] = X;
        yp[i] = Y;
        bxp[i] = B;
    }
}

/*
 * ---------------------------------------------------------------
 *  2x2 box downsample in linear RGB
 * ---------------------------------------------------------------
 */

/* ADR-0141 carve-out: the 4-deep loop nest (per-plane × per-output-row
 * × per-output-col × 2×2 input sample) matches the libjxl scalar
 * reference one-for-one; splitting would obscure the scalar diff. */
// NOLINTNEXTLINE(readability-function-size,google-readability-function-size) — paired SIMD ports (avx2/avx512/neon/sve2) bit-exact-match this scalar; splitting would force matching splits in 4 SIMD files (ADR-0141)
static void downsample_2x2(const float *in, unsigned iw, unsigned ih, float *out, unsigned *ow_out,
                           unsigned *oh_out)
{
    const unsigned ow = (iw + 1) / 2;
    const unsigned oh = (ih + 1) / 2;
    *ow_out = ow;
    *oh_out = oh;

    const size_t in_plane = (size_t)iw * (size_t)ih;
    const size_t out_plane = (size_t)ow * (size_t)oh;
    for (int c = 0; c < 3; c++) {
        const float *ip = in + (size_t)c * in_plane;
        float *op = out + (size_t)c * out_plane;
        for (unsigned oy = 0; oy < oh; oy++) {
            for (unsigned ox = 0; ox < ow; ox++) {
                float sum = 0.0f;
                for (unsigned dy = 0; dy < 2; dy++) {
                    for (unsigned dx = 0; dx < 2; dx++) {
                        unsigned ix = ox * 2 + dx;
                        unsigned iy = oy * 2 + dy;
                        if (ix >= iw)
                            ix = iw - 1;
                        if (iy >= ih)
                            iy = ih - 1;
                        sum += ip[(size_t)iy * iw + ix];
                    }
                }
                op[(size_t)oy * ow + ox] = sum * 0.25f;
            }
        }
    }
}

/*
 * ---------------------------------------------------------------
 *  FastGaussian — separable 3-pole recursive IIR (Charalampidis 2016).
 *  Bit-close port of libjxl's gauss_blur.cc scalar path. Boundaries
 *  are zero-padded (matching libjxl), not reflected — this is the
 *  dominant source of numeric deviation from the prior convolutional
 *  blur and from scipy's gaussian_filter Python reference, and it
 *  matches libjxl's canonical ssimulacra2 pipeline.
 * ---------------------------------------------------------------
 */

/* 1D pass: out[n] = sum_{k in {1,3,5}} prev_k where
 * out_k = n2[k] * (in[n-N-1] + in[n+N-1]) - d1[k] * prev_k - prev2_k. */
static void fast_gaussian_1d(const Ssimu2State *s, const float *restrict in, float *restrict out,
                             ptrdiff_t xsize)
{
    float prev1_0 = 0.f;
    float prev1_1 = 0.f;
    float prev1_2 = 0.f;
    float prev2_0 = 0.f;
    float prev2_1 = 0.f;
    float prev2_2 = 0.f;
    const float n2_0 = s->rg_n2[0];
    const float n2_1 = s->rg_n2[1];
    const float n2_2 = s->rg_n2[2];
    const float d1_0 = s->rg_d1[0];
    const float d1_1 = s->rg_d1[1];
    const float d1_2 = s->rg_d1[2];
    const ptrdiff_t N = (ptrdiff_t)s->rg_radius;

    for (ptrdiff_t n = -N + 1; n < xsize; n++) {
        const ptrdiff_t left = n - N - 1;
        const ptrdiff_t right = n + N - 1;
        const float lv = (left >= 0) ? in[left] : 0.f;
        const float rv = (right < xsize) ? in[right] : 0.f;
        const float sum = lv + rv;

        const float o0 = n2_0 * sum - d1_0 * prev1_0 - prev2_0;
        const float o1 = n2_1 * sum - d1_1 * prev1_1 - prev2_1;
        const float o2 = n2_2 * sum - d1_2 * prev1_2 - prev2_2;
        prev2_0 = prev1_0;
        prev2_1 = prev1_1;
        prev2_2 = prev1_2;
        prev1_0 = o0;
        prev1_1 = o1;
        prev1_2 = o2;

        if (n >= 0) {
            out[n] = o0 + o1 + o2;
        }
    }
}

/* 2D pass: horizontal per row (in → scratch), vertical per column
 * (scratch → out) with per-column IIR state in s->col_state. */
static void blur_plane(const Ssimu2State *s, const float *in, float *out, float *scratch,
                       unsigned w, unsigned h)
{
    /* Horizontal pass */
    for (unsigned y = 0; y < h; y++) {
        fast_gaussian_1d(s, in + (size_t)y * w, scratch + (size_t)y * w, (ptrdiff_t)w);
    }

    /* Vertical pass — interleave all columns' IIR state. */
    const size_t xsize = (size_t)w;
    float *prev1_0 = s->col_state + 0u * xsize;
    float *prev1_1 = s->col_state + 1u * xsize;
    float *prev1_2 = s->col_state + 2u * xsize;
    float *prev2_0 = s->col_state + 3u * xsize;
    float *prev2_1 = s->col_state + 4u * xsize;
    float *prev2_2 = s->col_state + 5u * xsize;
    memset(s->col_state, 0, 6u * xsize * sizeof(float));

    const float n2_0 = s->rg_n2[0];
    const float n2_1 = s->rg_n2[1];
    const float n2_2 = s->rg_n2[2];
    const float d1_0 = s->rg_d1[0];
    const float d1_1 = s->rg_d1[1];
    const float d1_2 = s->rg_d1[2];
    const ptrdiff_t N = (ptrdiff_t)s->rg_radius;
    const ptrdiff_t ysize = (ptrdiff_t)h;

    for (ptrdiff_t n = -N + 1; n < ysize; n++) {
        const ptrdiff_t left = n - N - 1;
        const ptrdiff_t right = n + N - 1;
        const float *lrow = (left >= 0) ? (scratch + (size_t)left * xsize) : NULL;
        const float *rrow = (right < ysize) ? (scratch + (size_t)right * xsize) : NULL;
        float *orow = (n >= 0) ? (out + (size_t)n * xsize) : NULL;

        for (size_t x = 0; x < xsize; x++) {
            const float lv = lrow ? lrow[x] : 0.f;
            const float rv = rrow ? rrow[x] : 0.f;
            const float sum = lv + rv;

            const float o0 = n2_0 * sum - d1_0 * prev1_0[x] - prev2_0[x];
            const float o1 = n2_1 * sum - d1_1 * prev1_1[x] - prev2_1[x];
            const float o2 = n2_2 * sum - d1_2 * prev1_2[x] - prev2_2[x];
            prev2_0[x] = prev1_0[x];
            prev2_1[x] = prev1_1[x];
            prev2_2[x] = prev1_2[x];
            prev1_0[x] = o0;
            prev1_1[x] = o1;
            prev1_2[x] = o2;

            if (orow) {
                orow[x] = o0 + o1 + o2;
            }
        }
    }
}

static void blur_3plane(const Ssimu2State *s, const float *in, float *out, unsigned w, unsigned h)
{
    const size_t plane = (size_t)w * (size_t)h;
    for (int c = 0; c < 3; c++) {
        if (s->blur_fn) {
            s->blur_fn(s->rg_n2, s->rg_d1, s->rg_radius, s->col_state, in + (size_t)c * plane,
                       out + (size_t)c * plane, s->scratch, w, h);
        } else {
            blur_plane(s, in + (size_t)c * plane, out + (size_t)c * plane, s->scratch, w, h);
        }
    }
}

/* Elementwise multiply (3 planes): mul = a * b */
static void multiply_3plane(const float *a, const float *b, float *mul, unsigned w, unsigned h)
{
    const size_t n = 3u * (size_t)w * (size_t)h;
    for (size_t i = 0; i < n; i++) {
        mul[i] = a[i] * b[i];
    }
}

/*
 * ---------------------------------------------------------------
 *  SSIMMap & EdgeDiffMap — return per-plane (L1, L4) averages
 *  (mirrors libjxl tools/ssimulacra2.cc:SSIMMap / EdgeDiffMap)
 * ---------------------------------------------------------------
 */

static inline double quartic(double x)
{
    x *= x;
    return x * x;
}

static void ssim_map(const float *m1, const float *m2, const float *s11, const float *s22,
                     const float *s12, unsigned w, unsigned h, double plane_averages[6])
{
    const size_t plane = (size_t)w * (size_t)h;
    const double one_per_pixels = 1.0 / (double)plane;
    for (int c = 0; c < 3; c++) {
        double sum_l1 = 0.0;
        double sum_l4 = 0.0;
        const float *rm1 = m1 + (size_t)c * plane;
        const float *rm2 = m2 + (size_t)c * plane;
        const float *rs11 = s11 + (size_t)c * plane;
        const float *rs22 = s22 + (size_t)c * plane;
        const float *rs12 = s12 + (size_t)c * plane;
        for (size_t i = 0; i < plane; i++) {
            float mu1 = rm1[i];
            float mu2 = rm2[i];
            float mu11 = mu1 * mu1;
            float mu22 = mu2 * mu2;
            float mu12 = mu1 * mu2;
            float num_m = 1.0f - (mu1 - mu2) * (mu1 - mu2);
            float num_s = 2.0f * (rs12[i] - mu12) + kC2;
            float denom_s = (rs11[i] - mu11) + (rs22[i] - mu22) + kC2;
            double d = 1.0 - ((double)num_m * (double)num_s / (double)denom_s);
            if (d < 0.0)
                d = 0.0;
            sum_l1 += d;
            sum_l4 += quartic(d);
        }
        plane_averages[c * 2 + 0] = one_per_pixels * sum_l1;
        plane_averages[c * 2 + 1] = sqrt(sqrt(one_per_pixels * sum_l4));
    }
}

static void edge_diff_map(const float *img1, const float *mu1, const float *img2, const float *mu2,
                          unsigned w, unsigned h, double plane_averages[12])
{
    const size_t plane = (size_t)w * (size_t)h;
    const double one_per_pixels = 1.0 / (double)plane;
    for (int c = 0; c < 3; c++) {
        double s0 = 0.0;
        double s1 = 0.0;
        double s2 = 0.0;
        double s3 = 0.0;
        const float *r1 = img1 + (size_t)c * plane;
        const float *rm1 = mu1 + (size_t)c * plane;
        const float *r2 = img2 + (size_t)c * plane;
        const float *rm2 = mu2 + (size_t)c * plane;
        for (size_t i = 0; i < plane; i++) {
            double ed1 = fabs((double)r1[i] - (double)rm1[i]);
            double ed2 = fabs((double)r2[i] - (double)rm2[i]);
            double d1 = (1.0 + ed2) / (1.0 + ed1) - 1.0;
            double art = d1 > 0.0 ? d1 : 0.0;
            double det = d1 < 0.0 ? -d1 : 0.0;
            s0 += art;
            s1 += quartic(art);
            s2 += det;
            s3 += quartic(det);
        }
        plane_averages[c * 4 + 0] = one_per_pixels * s0;
        plane_averages[c * 4 + 1] = sqrt(sqrt(one_per_pixels * s1));
        plane_averages[c * 4 + 2] = one_per_pixels * s2;
        plane_averages[c * 4 + 3] = sqrt(sqrt(one_per_pixels * s3));
    }
}

/*
 * ---------------------------------------------------------------
 *  Final polynomial pool
 * ---------------------------------------------------------------
 */

static double pool_score(double avg_ssim[6][6], double avg_ed[6][12], int num_scales)
{
    double ssim = 0.0;
    size_t i = 0;
    for (int c = 0; c < 3; c++) {
        for (int scale = 0; scale < 6; scale++) {
            for (int n = 0; n < 2; n++) {
                double s_term = scale < num_scales ? avg_ssim[scale][c * 2 + n] : 0.0;
                double r_term = scale < num_scales ? avg_ed[scale][c * 4 + n] : 0.0;
                double b_term = scale < num_scales ? avg_ed[scale][c * 4 + n + 2] : 0.0;
                ssim += kWeights[i++] * fabs(s_term);
                ssim += kWeights[i++] * fabs(r_term);
                ssim += kWeights[i++] * fabs(b_term);
            }
        }
    }

    ssim = ssim * 0.9562382616834844;
    ssim = 2.326765642916932 * ssim - 0.020884521182843837 * ssim * ssim +
           6.248496625763138e-05 * ssim * ssim * ssim;
    if (ssim > 0.0) {
        ssim = 100.0 - 10.0 * pow(ssim, 0.6276336467831387);
    } else {
        ssim = 100.0;
    }
    return ssim;
}

/*
 * ---------------------------------------------------------------
 *  Extractor lifecycle
 * ---------------------------------------------------------------
 */

/* Pick SIMD kernels based on runtime CPU flags. Scalar is the default
 * fallback; AVX2 / AVX-512 override on x86, NEON on aarch64. Kept as a
 * separate helper so `init` stays under the ADR-0141
 * readability-function-size threshold. */
static void init_simd_dispatch(Ssimu2State *s)
{
    s->mul3_fn = multiply_3plane;
    s->xyb_fn = linear_rgb_to_xyb;
    s->down_fn = downsample_2x2;
    s->ssim_fn = ssim_map;
    s->edge_fn = edge_diff_map;
    s->blur_fn = NULL;
    s->ptlr_fn = NULL;

#if ARCH_X86 || ARCH_AARCH64
    const unsigned flags = vmaf_get_cpu_flags();
#endif
#if ARCH_X86
    if (flags & VMAF_X86_CPU_FLAG_AVX2) {
        s->mul3_fn = ssimulacra2_multiply_3plane_avx2;
        s->xyb_fn = ssimulacra2_linear_rgb_to_xyb_avx2;
        s->down_fn = ssimulacra2_downsample_2x2_avx2;
        s->ssim_fn = ssimulacra2_ssim_map_avx2;
        s->edge_fn = ssimulacra2_edge_diff_map_avx2;
        s->blur_fn = ssimulacra2_blur_plane_avx2;
        s->ptlr_fn = ssimulacra2_picture_to_linear_rgb_avx2;
    }
#if HAVE_AVX512
    if (flags & VMAF_X86_CPU_FLAG_AVX512) {
        s->mul3_fn = ssimulacra2_multiply_3plane_avx512;
        s->xyb_fn = ssimulacra2_linear_rgb_to_xyb_avx512;
        s->down_fn = ssimulacra2_downsample_2x2_avx512;
        s->ssim_fn = ssimulacra2_ssim_map_avx512;
        s->edge_fn = ssimulacra2_edge_diff_map_avx512;
        s->blur_fn = ssimulacra2_blur_plane_avx512;
        s->ptlr_fn = ssimulacra2_picture_to_linear_rgb_avx512;
    }
#endif
#endif
#if ARCH_AARCH64
    if (flags & VMAF_ARM_CPU_FLAG_NEON) {
        s->mul3_fn = ssimulacra2_multiply_3plane_neon;
        s->xyb_fn = ssimulacra2_linear_rgb_to_xyb_neon;
        s->down_fn = ssimulacra2_downsample_2x2_neon;
        s->ssim_fn = ssimulacra2_ssim_map_neon;
        s->edge_fn = ssimulacra2_edge_diff_map_neon;
        s->blur_fn = ssimulacra2_blur_plane_neon;
        s->ptlr_fn = ssimulacra2_picture_to_linear_rgb_neon;
    }
#if HAVE_SVE2
    /* T7-38: SVE2 overrides NEON when available. Each kernel produces
     * byte-identical output to the NEON sibling — the dispatch is
     * additive, not perturbative (ADR-NNNN). */
    if (flags & VMAF_ARM_CPU_FLAG_SVE2) {
        s->mul3_fn = ssimulacra2_multiply_3plane_sve2;
        s->xyb_fn = ssimulacra2_linear_rgb_to_xyb_sve2;
        s->down_fn = ssimulacra2_downsample_2x2_sve2;
        s->ssim_fn = ssimulacra2_ssim_map_sve2;
        s->edge_fn = ssimulacra2_edge_diff_map_sve2;
        s->blur_fn = ssimulacra2_blur_plane_sve2;
        s->ptlr_fn = ssimulacra2_picture_to_linear_rgb_sve2;
    }
#endif
#endif
}

static int init(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc, unsigned w,
                unsigned h)
{
    (void)pix_fmt;
    Ssimu2State *s = fex->priv;

    s->w = w;
    s->h = h;
    s->bpc = bpc;
    s->sample_scale = 1.0f / (float)((1u << bpc) - 1u);

    create_recursive_gaussian(s, SSIMU2_SIGMA);

    s->cap_plane = (size_t)w * (size_t)h;
    const size_t rgb_bytes = 3u * s->cap_plane * sizeof(float);
    const size_t plane_bytes = s->cap_plane * sizeof(float);
    const size_t col_state_bytes = 6u * (size_t)w * sizeof(float);

    s->ref_lin = aligned_malloc(rgb_bytes, 32);
    s->dist_lin = aligned_malloc(rgb_bytes, 32);
    s->ref_xyb = aligned_malloc(rgb_bytes, 32);
    s->dist_xyb = aligned_malloc(rgb_bytes, 32);
    s->mu1 = aligned_malloc(rgb_bytes, 32);
    s->mu2 = aligned_malloc(rgb_bytes, 32);
    s->sigma1_sq = aligned_malloc(rgb_bytes, 32);
    s->sigma2_sq = aligned_malloc(rgb_bytes, 32);
    s->sigma12 = aligned_malloc(rgb_bytes, 32);
    s->mul_buf = aligned_malloc(rgb_bytes, 32);
    s->scratch = aligned_malloc(plane_bytes, 32);
    s->col_state = aligned_malloc(col_state_bytes, 32);

    if (!s->ref_lin || !s->dist_lin || !s->ref_xyb || !s->dist_xyb || !s->mu1 || !s->mu2 ||
        !s->sigma1_sq || !s->sigma2_sq || !s->sigma12 || !s->mul_buf || !s->scratch ||
        !s->col_state) {
        goto fail;
    }

    init_simd_dispatch(s);
    return 0;

fail:
    /* Partial OOM: any of the 12 allocations above may have succeeded while a
     * later one failed. Free every non-NULL buffer (aligned_free(NULL) is a
     * no-op) so the partially-initialised state does not leak 1-11 buffers. */
    aligned_free(s->ref_lin);
    aligned_free(s->dist_lin);
    aligned_free(s->ref_xyb);
    aligned_free(s->dist_xyb);
    aligned_free(s->mu1);
    aligned_free(s->mu2);
    aligned_free(s->sigma1_sq);
    aligned_free(s->sigma2_sq);
    aligned_free(s->sigma12);
    aligned_free(s->mul_buf);
    aligned_free(s->scratch);
    aligned_free(s->col_state);
    return -ENOMEM;
}

/* Dispatch YUV → linear RGB through the SIMD function pointer when set,
 * otherwise fall back to the scalar `picture_to_linear_rgb`. Kept as a
 * helper so `extract` stays under the ADR-0141 function-size threshold. */
static void convert_picture_to_linear_rgb(const Ssimu2State *s, const VmafPicture *pic, float *out)
{
    if (s->ptlr_fn) {
        const simd_plane_t planes[3] = {
            {pic->data[0], (ptrdiff_t)pic->stride[0], pic->w[0], pic->h[0]},
            {pic->data[1], (ptrdiff_t)pic->stride[1], pic->w[1], pic->h[1]},
            {pic->data[2], (ptrdiff_t)pic->stride[2], pic->w[2], pic->h[2]},
        };
        s->ptlr_fn(s->yuv_matrix, s->bpc, s->w, s->h, planes, out);
    } else {
        picture_to_linear_rgb(s, pic, out);
    }
}

static int extract(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                   VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index,
                   VmafFeatureCollector *feature_collector)
{
    (void)ref_pic_90;
    (void)dist_pic_90;
    Ssimu2State *s = fex->priv;

    /* Stage 1: YUV → linear RGB for both frames (full resolution). */
    convert_picture_to_linear_rgb(s, ref_pic, s->ref_lin);
    convert_picture_to_linear_rgb(s, dist_pic, s->dist_lin);

    /* Multi-scale loop. Per-scale working buffers are reused; the
     * linear-RGB buffers are down-sampled in-place between scales. */
    double avg_ssim[6][6] = {{0}};
    double avg_ed[6][12] = {{0}};

    unsigned cw = s->w;
    unsigned ch = s->h;
    int completed = 0;

    for (int scale = 0; scale < kNumScales; scale++) {
        if (cw < 8u || ch < 8u)
            break;

        s->xyb_fn(s->ref_lin, s->ref_xyb, cw, ch);
        s->xyb_fn(s->dist_lin, s->dist_xyb, cw, ch);

        s->mul3_fn(s->ref_xyb, s->ref_xyb, s->mul_buf, cw, ch);
        blur_3plane(s, s->mul_buf, s->sigma1_sq, cw, ch);

        s->mul3_fn(s->dist_xyb, s->dist_xyb, s->mul_buf, cw, ch);
        blur_3plane(s, s->mul_buf, s->sigma2_sq, cw, ch);

        s->mul3_fn(s->ref_xyb, s->dist_xyb, s->mul_buf, cw, ch);
        blur_3plane(s, s->mul_buf, s->sigma12, cw, ch);

        blur_3plane(s, s->ref_xyb, s->mu1, cw, ch);
        blur_3plane(s, s->dist_xyb, s->mu2, cw, ch);

        s->ssim_fn(s->mu1, s->mu2, s->sigma1_sq, s->sigma2_sq, s->sigma12, cw, ch, avg_ssim[scale]);
        s->edge_fn(s->ref_xyb, s->mu1, s->dist_xyb, s->mu2, cw, ch, avg_ed[scale]);
        completed++;

        if (scale + 1 < kNumScales) {
            unsigned nw = 0;
            unsigned nh = 0;
            s->down_fn(s->ref_lin, cw, ch, s->mul_buf, &nw, &nh);
            /* swap: mul now holds downsampled ref_lin */
            float *tmp = s->ref_lin;
            s->ref_lin = s->mul_buf;
            s->mul_buf = tmp;
            s->down_fn(s->dist_lin, cw, ch, s->mul_buf, &nw, &nh);
            tmp = s->dist_lin;
            s->dist_lin = s->mul_buf;
            s->mul_buf = tmp;
            cw = nw;
            ch = nh;
        }
    }

    const double score = pool_score(avg_ssim, avg_ed, completed);
    return vmaf_feature_collector_append(feature_collector, "ssimulacra2", score, index);
}

static int close(VmafFeatureExtractor *fex)
{
    Ssimu2State *s = fex->priv;
    if (!s)
        return 0;
    aligned_free(s->ref_lin);
    aligned_free(s->dist_lin);
    aligned_free(s->ref_xyb);
    aligned_free(s->dist_xyb);
    aligned_free(s->mu1);
    aligned_free(s->mu2);
    aligned_free(s->sigma1_sq);
    aligned_free(s->sigma2_sq);
    aligned_free(s->sigma12);
    aligned_free(s->mul_buf);
    aligned_free(s->scratch);
    aligned_free(s->col_state);
    return 0;
}

static const char *provided_features[] = {"ssimulacra2", NULL};

/* External linkage is required — the extractor registry iterates over
 * `vmaf_fex_*` externs in libvmaf/src/feature/feature_extractor.c. */
// NOLINTNEXTLINE(misc-use-internal-linkage,cppcoreguidelines-avoid-non-const-global-variables)
VmafFeatureExtractor vmaf_fex_ssimulacra2 = {
    .name = "ssimulacra2",
    .init = init,
    .extract = extract,
    .options = options,
    .close = close,
    .priv_size = sizeof(Ssimu2State),
    .provided_features = provided_features,
    /* Explicit zero-init so GCC LTO sees the full struct layout
     * across TUs (silences -Wlto-type-mismatch after the chars
     * field landed; see ADR-0181). */
    .chars = {0},
};
