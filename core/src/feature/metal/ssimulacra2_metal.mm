/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 *
 *  ssimulacra2 feature extractor on the Metal backend (feature
 *  "ssimulacra2"). Metal twin of ssimulacra2_cuda.c / the SYCL twin,
 *  both of which mirror the libjxl scalar reference ported in
 *  core/src/feature/ssimulacra2.c.
 *
 *  ARCHITECTURE (must match ssimulacra2_cuda.c for places=4 parity):
 *  the host does everything except the elementwise multiply and the
 *  separable IIR blur, which run on the GPU (ssimulacra2.metal):
 *
 *    1. Host: YUV -> linear RGB on the full-res frame (deterministic
 *       LUT sRGB EOTF, ADR-0164).
 *    2. Per-scale (up to 6, breaks early when min-dim < 8):
 *       a. Host: linear RGB -> XYB (verbatim port of the CPU
 *          linear_rgb_to_xyb; the GPU float cbrt differs from libm by
 *          tens of ULP and that drift cascades to a ~1.5e-2 pooled-score
 *          drift, so XYB stays on host — ADR-0201).
 *       b. GPU: 3 elementwise 3-plane multiplies (ref^2, dis^2, ref*dis)
 *          via ssimulacra2_mul3.
 *       c. GPU: 5 separable IIR blurs (s11, s22, s12, mu1, mu2) via
 *          ssimulacra2_blur_h3 + ssimulacra2_blur_v3.
 *       d. Host: per-pixel SSIM + EdgeDiff combine in double precision
 *          (verbatim ports of ssim_map + edge_diff_map).
 *       e. Host: 2x2 box downsample of the linear-RGB pyramid.
 *    3. Host: pool 108 weighted norms via the libjxl polynomial. Emit
 *       `ssimulacra2`.
 *
 *  On Apple Silicon's unified memory, the host reads the GPU outputs
 *  directly via [buffer contents] after waitUntilCompleted — there is
 *  no explicit device->host copy (cf. the CUDA twin's pinned-buffer
 *  D2H/H2D dance).
 *
 *  Precision contract (ADR-0214): places=4 (max_abs_diff <= 1e-4) on
 *  the CPU `ssimulacra2` vs Metal `ssimulacra2_metal` pair. The IIR
 *  blur kernel forces its per-pole products through a no-contract
 *  barrier (see ssimulacra2.metal `nc()`) so the Metal compiler's
 *  default -ffast-math does not fuse the IIR recurrence into FMAs;
 *  this is the MSL equivalent of the CUDA fatbin's --fmad=false.
 */

#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

/* feature_extractor.h uses `#if defined(__cplusplus)` to include <atomic>
 * (Xcode 16.4 / macOS 15 libc++ emits "templates must have C++ linkage"
 * when that header is pulled into an extern "C" block — ADR-fix macOS-Metal). */
#include "feature_extractor.h"

extern "C" {
#include "dict.h"
#include "feature_collector.h"
#include "feature_name.h"
#include "log.h"
#include "libvmaf/picture.h"

#include "feature/ssimulacra2_math.h" /* vmaf_ss2_cbrtf / vmaf_ss2_srgb_eotf */

#include "../../metal/common.h"
#include "../../metal/kernel_template.h"
}

extern "C" {
extern const unsigned char libvmaf_metallib_start[] __asm("section$start$__TEXT$__metallib");
extern const unsigned char libvmaf_metallib_end[]   __asm("section$end$__TEXT$__metallib");
}

#define SS2M_NUM_SCALES 6
#define SS2M_MUL_BX     16
#define SS2M_MUL_BY      8
#define SS2M_BLUR_BLOCK 64
#define SS2M_SIGMA      1.5
#define SS2M_PI         3.14159265358979323846

enum ss2m_yuv_matrix {
    SS2M_MATRIX_BT709_LIMITED = 0,
    SS2M_MATRIX_BT601_LIMITED = 1,
    SS2M_MATRIX_BT709_FULL    = 2,
    SS2M_MATRIX_BT601_FULL    = 3,
};

/* libjxl 108 pooling weights — bit-identical to ssimulacra2.c::kWeights. */
static const double g_weights[108] = {
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

typedef struct Ssimu2StateMetal {
    VmafMetalKernelLifecycle lc;
    VmafMetalContext *ctx;

    /* Option. */
    int yuv_matrix;

    /* Geometry. */
    unsigned width;
    unsigned height;
    unsigned bpc;
    unsigned scale_w[SS2M_NUM_SCALES];
    unsigned scale_h[SS2M_NUM_SCALES];

    /* Recursive Gaussian coefficients (sigma=1.5). */
    float rg_n2[3];
    float rg_d1[3];
    int   rg_radius;

    /* Pipeline state objects (mul + H/V blur). */
    void *pso_mul3;
    void *pso_blur_h;
    void *pso_blur_v;

    /* Device (Shared-storage) buffers. Each 3-plane buffer is contiguous
     * X | Y | B with planes at full-resolution stride, kept constant
     * across pyramid scales for layout consistency with the host. */
    void *buf_ref_xyb;     /* host writes XYB; GPU reads */
    void *buf_dis_xyb;
    void *buf_mul;         /* GPU scratch (mul output / blur input) */
    void *buf_blur_scr;    /* GPU scratch (H-pass output / V-pass input) */
    void *buf_mu1;
    void *buf_mu2;
    void *buf_s11;
    void *buf_s22;
    void *buf_s12;

    size_t plane_full_bytes; /* width*height*sizeof(float) */
    size_t three_plane_bytes;

    /* Host scratch for the linear-RGB pyramid + downsample. */
    float *h_ref_lin;
    float *h_dis_lin;
    float *h_ref_lin_ds;
    float *h_dis_lin_ds;

    /* Per-scale pooled accumulators, populated in submit(), consumed in
     * collect(). avg_ssim[scale][6] (L1/L4 x 3 channels);
     * avg_ed[scale][12] (artifact/detail L1/L4 x 3 channels). */
    double accum_ssim[SS2M_NUM_SCALES][6];
    double accum_ed[SS2M_NUM_SCALES][12];
    int    accum_completed;

    VmafDictionary *feature_name_dict;
} Ssimu2StateMetal;

static const VmafOption options[] = {
    {
        .name        = "yuv_matrix",
        .help        = "YUV→RGB matrix: 0=bt709_limited (default), 1=bt601_limited, "
                       "2=bt709_full, 3=bt601_full",
        .offset      = offsetof(Ssimu2StateMetal, yuv_matrix),
        .type        = VMAF_OPT_TYPE_INT,
        .default_val = {.i = SS2M_MATRIX_BT709_LIMITED},
        .min         = 0,
        .max         = 3,
    },
    {0},
};

/* ------------------------------------------------------------------ */
/* Recursive Gaussian coefficient setup — bit-identical port of
 * ssimulacra2.c::create_recursive_gaussian. Splitting the linear-system
 * solve would obscure the scalar-diff audit trail (ADR-0141).
 * NOLINTNEXTLINE(readability-function-size,google-readability-function-size) */
static void ss2m_setup_gaussian(Ssimu2StateMetal *s, double sigma)
{
    const double radius = round(3.2795 * sigma + 0.2546);
    const double pi_div_2r = SS2M_PI / (2.0 * radius);
    const double omega[3] = {pi_div_2r, 3.0 * pi_div_2r, 5.0 * pi_div_2r};

    const double p1 = +1.0 / tan(0.5 * omega[0]);
    const double p3 = -1.0 / tan(0.5 * omega[1]);
    const double p5 = +1.0 / tan(0.5 * omega[2]);
    const double r1 = +p1 * p1 / sin(omega[0]);
    const double r3 = -p3 * p3 / sin(omega[1]);
    const double r5 = +p5 * p5 / sin(omega[2]);

    const double neg_half_sigma2 = -0.5 * sigma * sigma;
    const double recip_r = 1.0 / radius;
    double rho[3];
    for (int i = 0; i < 3; i++) {
        rho[i] = exp(neg_half_sigma2 * omega[i] * omega[i]) * recip_r;
    }

    const double D13 = p1 * r3 - r1 * p3;
    const double D35 = p3 * r5 - r3 * p5;
    const double D51 = p5 * r1 - r5 * p1;
    const double recip_d13 = 1.0 / D13;
    const double zeta_15 = D35 * recip_d13;
    const double zeta_35 = D51 * recip_d13;

    const double A[3][3] = {{p1, p3, p5}, {r1, r3, r5}, {zeta_15, zeta_35, 1.0}};
    const double gamma[3] = {1.0, radius * radius - sigma * sigma,
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

    s->rg_radius = (int)radius;
    for (int i = 0; i < 3; i++) {
        s->rg_n2[i] = (float)(-beta[i] * cos(omega[i] * (radius + 1.0)));
        s->rg_d1[i] = (float)(-2.0 * cos(omega[i]));
    }
}

/* ------------------------------------------------------------------ */
/* Host-side YUV -> linear RGB + linear RGB -> XYB + 2x2 downsample +
 * SSIM/EdgeDiff combine + final pool. Verbatim ports of the matching
 * ssimulacra2.c scalar paths so host outputs bit-match the CPU
 * extractor (ssimulacra2_cuda.c uses the same ports).
 * ------------------------------------------------------------------ */

static inline float ss2m_clampf(float v, float lo, float hi)
{
    if (v < lo) { return lo; }
    if (v > hi) { return hi; }
    return v;
}

static inline float ss2m_read_plane(const VmafPicture *pic, int plane, int x, int y)
{
    unsigned pw = pic->w[plane];
    unsigned ph = pic->h[plane];
    unsigned lw = pic->w[0];
    unsigned lh = pic->h[0];
    int sx = (pw == lw)     ? x :
             (pw * 2 == lw) ? (x >> 1) :
                              (int)((int64_t)x * (int64_t)pw / (int64_t)lw);
    int sy = (ph == lh)     ? y :
             (ph * 2 == lh) ? (y >> 1) :
                              (int)((int64_t)y * (int64_t)ph / (int64_t)lh);
    if (sx < 0) { sx = 0; }
    if (sy < 0) { sy = 0; }
    if ((unsigned)sx >= pw) { sx = (int)pw - 1; }
    if ((unsigned)sy >= ph) { sy = (int)ph - 1; }
    if (pic->bpc > 8) {
        const uint16_t *row =
            (const uint16_t *)((const uint8_t *)pic->data[plane] + (size_t)sy * pic->stride[plane]);
        return (float)row[sx];
    }
    const uint8_t *row = (const uint8_t *)pic->data[plane] + (size_t)sy * pic->stride[plane];
    return (float)row[sx];
}

/* Verbatim port of ssimulacra2.c::picture_to_linear_rgb (ADR-0141
 * carve-out: line-for-line scalar-diff parity).
 * NOLINTNEXTLINE(readability-function-size,google-readability-function-size) */
static void ss2m_picture_to_linear_rgb(const Ssimu2StateMetal *s, const VmafPicture *pic, float *out)
{
    const unsigned w = s->width;
    const unsigned h = s->height;
    const size_t plane_sz = (size_t)w * (size_t)h;
    float *rp = out;
    float *gp = out + plane_sz;
    float *bp = out + 2 * plane_sz;

    const float peak = (float)((1u << s->bpc) - 1u);
    const float inv_peak = 1.0f / peak;

    float kr;
    float kg;
    float kb;
    int limited = 1;
    switch (s->yuv_matrix) {
    case SS2M_MATRIX_BT709_FULL:
        limited = 0;
        /* fall through */
    case SS2M_MATRIX_BT709_LIMITED:
        kr = 0.2126f;
        kg = 0.7152f;
        kb = 0.0722f;
        break;
    case SS2M_MATRIX_BT601_FULL:
        limited = 0;
        /* fall through */
    case SS2M_MATRIX_BT601_LIMITED:
    default:
        kr = 0.299f;
        kg = 0.587f;
        kb = 0.114f;
        break;
    }
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
            float Y = ss2m_read_plane(pic, 0, (int)x, (int)y) * inv_peak;
            float U = ss2m_read_plane(pic, 1, (int)x, (int)y) * inv_peak;
            float V = ss2m_read_plane(pic, 2, (int)x, (int)y) * inv_peak;
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
            R = ss2m_clampf(R, 0.0f, 1.0f);
            G = ss2m_clampf(G, 0.0f, 1.0f);
            B = ss2m_clampf(B, 0.0f, 1.0f);
            const size_t idx = (size_t)y * w + x;
            rp[idx] = vmaf_ss2_srgb_eotf(R);
            gp[idx] = vmaf_ss2_srgb_eotf(G);
            bp[idx] = vmaf_ss2_srgb_eotf(B);
        }
    }
}

/* Host linear-RGB -> XYB. Verbatim port of ssimulacra2.c::linear_rgb_to_xyb.
 * The 3-plane output stride is `plane_stride` (= full_w*full_h, constant
 * across scales). */
static void ss2m_host_linear_rgb_to_xyb(const float *lin, float *xyb, unsigned w, unsigned h,
                                        size_t plane_stride)
{
    const float kM00 = 0.30f;
    const float kM02 = 0.078f;
    const float kM10 = 0.23f;
    const float kM12 = 0.078f;
    const float kM20 = 0.24342268924547819f;
    const float kM21 = 0.20476744424496821f;
    const float kOpsinBias = 0.0037930732552754493f;

    const float m01 = 1.0f - kM00 - kM02;
    const float m11 = 1.0f - kM10 - kM12;
    const float m22 = 1.0f - kM20 - kM21;
    const float cbrt_bias = vmaf_ss2_cbrtf(kOpsinBias);

    const float *rp = lin;
    const float *gp = lin + plane_stride;
    const float *bp = lin + 2u * plane_stride;
    float *xp = xyb;
    float *yp = xyb + plane_stride;
    float *bxp = xyb + 2u * plane_stride;

    const size_t scale_pixels = (size_t)w * (size_t)h;
    for (size_t i = 0; i < scale_pixels; i++) {
        float r = rp[i];
        float g = gp[i];
        float b = bp[i];
        float l = kM00 * r + m01 * g + kM02 * b + kOpsinBias;
        float m = kM10 * r + m11 * g + kM12 * b + kOpsinBias;
        float sv = kM20 * r + kM21 * g + m22 * b + kOpsinBias;
        if (l < 0.0f) { l = 0.0f; }
        if (m < 0.0f) { m = 0.0f; }
        if (sv < 0.0f) { sv = 0.0f; }
        float L = vmaf_ss2_cbrtf(l) - cbrt_bias;
        float M = vmaf_ss2_cbrtf(m) - cbrt_bias;
        float S = vmaf_ss2_cbrtf(sv) - cbrt_bias;
        float X = 0.5f * (L - M);
        float Y = 0.5f * (L + M);
        float B = S;
        B = (B - Y) + 0.55f;
        X = X * 14.0f + 0.42f;
        Y = Y + 0.01f;
        xp[i] = X;
        yp[i] = Y;
        bxp[i] = B;
    }
}

static void ss2m_downsample_2x2(const float *in, unsigned iw, unsigned ih, float *out, unsigned ow,
                                unsigned oh, size_t plane_stride)
{
    for (int c = 0; c < 3; c++) {
        const float *ip = in + (size_t)c * plane_stride;
        float *op = out + (size_t)c * plane_stride;
        for (unsigned oy = 0; oy < oh; oy++) {
            for (unsigned ox = 0; ox < ow; ox++) {
                float sum = 0.0f;
                for (unsigned dy = 0; dy < 2; dy++) {
                    for (unsigned dx = 0; dx < 2; dx++) {
                        unsigned ix = ox * 2 + dx;
                        unsigned iy = oy * 2 + dy;
                        if (ix >= iw) { ix = iw - 1; }
                        if (iy >= ih) { iy = ih - 1; }
                        sum += ip[(size_t)iy * iw + ix];
                    }
                }
                op[(size_t)oy * ow + ox] = sum * 0.25f;
            }
        }
    }
}

static inline double ss2m_quartic(double x)
{
    x *= x;
    return x * x;
}

/* Per-pixel SSIM + EdgeDiff combine in double precision. Mirrors
 * ssimulacra2.c::ssim_map + ::edge_diff_map (and ssimulacra2_cuda.c::
 * ss2c_host_combine). Splitting would obscure the scalar-diff audit
 * trail (ADR-0141).
 * NOLINTNEXTLINE(readability-function-size,google-readability-function-size) */
static void ss2m_host_combine(const Ssimu2StateMetal *s, unsigned cw, unsigned ch,
                              const float *h_mu1, const float *h_mu2, const float *h_s11,
                              const float *h_s22, const float *h_s12, const float *h_ref_xyb,
                              const float *h_dis_xyb, double avg_ssim[6], double avg_ed[12])
{
    const size_t full_plane = (size_t)s->width * (size_t)s->height;
    const size_t scale_pixels = (size_t)cw * (size_t)ch;
    const double inv_pixels = 1.0 / (double)scale_pixels;
    const float kC2 = 0.0009f;
    for (int c = 0; c < 3; c++) {
        const float *m1 = h_mu1 + (size_t)c * full_plane;
        const float *m2 = h_mu2 + (size_t)c * full_plane;
        const float *s11p = h_s11 + (size_t)c * full_plane;
        const float *s22p = h_s22 + (size_t)c * full_plane;
        const float *s12p = h_s12 + (size_t)c * full_plane;
        const float *r1 = h_ref_xyb + (size_t)c * full_plane;
        const float *r2 = h_dis_xyb + (size_t)c * full_plane;
        double sum_l1 = 0.0;
        double sum_l4 = 0.0;
        double e_art = 0.0;
        double e_art4 = 0.0;
        double e_det = 0.0;
        double e_det4 = 0.0;
        for (size_t i = 0; i < scale_pixels; i++) {
            const float u1 = m1[i];
            const float u2 = m2[i];
            const float u11 = u1 * u1;
            const float u22 = u2 * u2;
            const float u12 = u1 * u2;
            const float num_m = 1.0f - (u1 - u2) * (u1 - u2);
            const float num_s = 2.0f * (s12p[i] - u12) + kC2;
            const float denom_s = (s11p[i] - u11) + (s22p[i] - u22) + kC2;
            double d = 1.0 - ((double)num_m * (double)num_s / (double)denom_s);
            if (d < 0.0) { d = 0.0; }
            sum_l1 += d;
            sum_l4 += ss2m_quartic(d);
            const double ed1 = fabs((double)r1[i] - (double)u1);
            const double ed2 = fabs((double)r2[i] - (double)u2);
            const double dd = (1.0 + ed2) / (1.0 + ed1) - 1.0;
            const double art = dd > 0.0 ? dd : 0.0;
            const double det = dd < 0.0 ? -dd : 0.0;
            e_art += art;
            e_art4 += ss2m_quartic(art);
            e_det += det;
            e_det4 += ss2m_quartic(det);
        }
        avg_ssim[c * 2 + 0] = inv_pixels * sum_l1;
        avg_ssim[c * 2 + 1] = sqrt(sqrt(inv_pixels * sum_l4));
        avg_ed[c * 4 + 0] = inv_pixels * e_art;
        avg_ed[c * 4 + 1] = sqrt(sqrt(inv_pixels * e_art4));
        avg_ed[c * 4 + 2] = inv_pixels * e_det;
        avg_ed[c * 4 + 3] = sqrt(sqrt(inv_pixels * e_det4));
    }
}

/* Mirror of ssimulacra2.c::pool_score. */
static double ss2m_pool_score(const double avg_ssim[6][6], const double avg_ed[6][12], int num_scales)
{
    double ssim = 0.0;
    size_t i = 0;
    for (int c = 0; c < 3; c++) {
        for (int scale = 0; scale < 6; scale++) {
            for (int n = 0; n < 2; n++) {
                double s_term = scale < num_scales ? avg_ssim[scale][c * 2 + n] : 0.0;
                double r_term = scale < num_scales ? avg_ed[scale][c * 4 + n] : 0.0;
                double b_term = scale < num_scales ? avg_ed[scale][c * 4 + n + 2] : 0.0;
                ssim += g_weights[i++] * fabs(s_term);
                ssim += g_weights[i++] * fabs(r_term);
                ssim += g_weights[i++] * fabs(b_term);
            }
        }
    }
    ssim *= 0.9562382616834844;
    ssim = 2.326765642916932 * ssim - 0.020884521182843837 * ssim * ssim +
           6.248496625763138e-05 * ssim * ssim * ssim;
    if (ssim > 0.0) {
        ssim = 100.0 - 10.0 * pow(ssim, 0.6276336467831387);
    } else {
        ssim = 100.0;
    }
    return ssim;
}

/* ------------------------------------------------------------------ */
/* Pipeline + buffer lifecycle                                         */
/* ------------------------------------------------------------------ */

static int build_pipelines(Ssimu2StateMetal *s, id<MTLDevice> device)
{
    const size_t blob_size = (size_t)(libvmaf_metallib_end - libvmaf_metallib_start);
    if (blob_size == 0) { return -ENODEV; }

    dispatch_data_t data = dispatch_data_create(
        libvmaf_metallib_start, blob_size,
        dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0),
        DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    if (data == NULL) { return -ENOMEM; }

    NSError *err = nil;
    id<MTLLibrary> lib = [device newLibraryWithData:data error:&err];
    if (lib == nil) { return -ENODEV; }

    id<MTLFunction> fn_mul = [lib newFunctionWithName:@"ssimulacra2_mul3"];
    id<MTLFunction> fn_h   = [lib newFunctionWithName:@"ssimulacra2_blur_h3"];
    id<MTLFunction> fn_v   = [lib newFunctionWithName:@"ssimulacra2_blur_v3"];
    if (fn_mul == nil || fn_h == nil || fn_v == nil) { return -ENODEV; }

    id<MTLComputePipelineState> pso_mul =
        [device newComputePipelineStateWithFunction:fn_mul error:&err];
    id<MTLComputePipelineState> pso_h =
        [device newComputePipelineStateWithFunction:fn_h error:&err];
    id<MTLComputePipelineState> pso_v =
        [device newComputePipelineStateWithFunction:fn_v error:&err];
    if (pso_mul == nil || pso_h == nil || pso_v == nil) { return -ENODEV; }

    s->pso_mul3   = (__bridge_retained void *)pso_mul;
    s->pso_blur_h = (__bridge_retained void *)pso_h;
    s->pso_blur_v = (__bridge_retained void *)pso_v;
    return 0;
}

static int ss2m_alloc_device_buffers(Ssimu2StateMetal *s, id<MTLDevice> device)
{
    const size_t bytes = s->three_plane_bytes;
    void **slots[] = {
        &s->buf_ref_xyb, &s->buf_dis_xyb, &s->buf_mul, &s->buf_blur_scr,
        &s->buf_mu1,     &s->buf_mu2,     &s->buf_s11, &s->buf_s22, &s->buf_s12,
    };
    for (size_t i = 0; i < sizeof(slots) / sizeof(slots[0]); ++i) {
        id<MTLBuffer> b = [device newBufferWithLength:bytes
                                             options:MTLResourceStorageModeShared];
        if (b == nil) { return -ENOMEM; }
        *slots[i] = (__bridge_retained void *)b;
    }
    return 0;
}

static void ss2m_release_device_buffers(Ssimu2StateMetal *s)
{
    void **slots[] = {
        &s->buf_ref_xyb, &s->buf_dis_xyb, &s->buf_mul, &s->buf_blur_scr,
        &s->buf_mu1,     &s->buf_mu2,     &s->buf_s11, &s->buf_s22, &s->buf_s12,
    };
    for (size_t i = 0; i < sizeof(slots) / sizeof(slots[0]); ++i) {
        if (*slots[i]) {
            (void)(__bridge_transfer id<MTLBuffer>)(*slots[i]);
            *slots[i] = NULL;
        }
    }
}

static int init_fex_metal(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                          unsigned w, unsigned h)
{
    (void)pix_fmt;
    Ssimu2StateMetal *s = (Ssimu2StateMetal *)fex->priv;

    if (w < 8u || h < 8u) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "ssimulacra2_metal: input %ux%u below 8x8 lower bound\n", w, h);
        return -EINVAL;
    }

    s->width  = w;
    s->height = h;
    s->bpc    = bpc;
    ss2m_setup_gaussian(s, SS2M_SIGMA);

    s->scale_w[0] = w;
    s->scale_h[0] = h;
    for (int i = 1; i < SS2M_NUM_SCALES; i++) {
        s->scale_w[i] = (s->scale_w[i - 1] + 1) / 2;
        s->scale_h[i] = (s->scale_h[i - 1] + 1) / 2;
    }

    s->plane_full_bytes  = (size_t)w * (size_t)h * sizeof(float);
    s->three_plane_bytes = 3u * s->plane_full_bytes;

    s->h_ref_lin    = (float *)malloc(s->three_plane_bytes);
    s->h_dis_lin    = (float *)malloc(s->three_plane_bytes);
    s->h_ref_lin_ds = (float *)malloc(s->three_plane_bytes);
    s->h_dis_lin_ds = (float *)malloc(s->three_plane_bytes);
    if (s->h_ref_lin == NULL || s->h_dis_lin == NULL || s->h_ref_lin_ds == NULL ||
        s->h_dis_lin_ds == NULL) {
        free(s->h_ref_lin);
        free(s->h_dis_lin);
        free(s->h_ref_lin_ds);
        free(s->h_dis_lin_ds);
        s->h_ref_lin = s->h_dis_lin = s->h_ref_lin_ds = s->h_dis_lin_ds = NULL;
        return -ENOMEM;
    }

    int err = vmaf_metal_context_new(&s->ctx, 0);
    if (err != 0) { goto fail_host; }

    err = vmaf_metal_kernel_lifecycle_init(&s->lc, s->ctx);
    if (err != 0) { goto fail_ctx; }

    {
        void *dh = vmaf_metal_context_device_handle(s->ctx);
        if (dh == NULL) { err = -ENODEV; goto fail_lc; }
        id<MTLDevice> device = (__bridge id<MTLDevice>)dh;

        err = ss2m_alloc_device_buffers(s, device);
        if (err != 0) { goto fail_bufs; }

        err = build_pipelines(s, device);
    }
    if (err != 0) { goto fail_pso; }

    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (s->feature_name_dict == NULL) { err = -ENOMEM; goto fail_pso; }
    return 0;

fail_pso:
    if (s->pso_blur_v) { (void)(__bridge_transfer id<MTLComputePipelineState>)s->pso_blur_v; s->pso_blur_v = NULL; }
    if (s->pso_blur_h) { (void)(__bridge_transfer id<MTLComputePipelineState>)s->pso_blur_h; s->pso_blur_h = NULL; }
    if (s->pso_mul3)   { (void)(__bridge_transfer id<MTLComputePipelineState>)s->pso_mul3;   s->pso_mul3   = NULL; }
fail_bufs:
    ss2m_release_device_buffers(s);
fail_lc:
    (void)vmaf_metal_kernel_lifecycle_close(&s->lc, s->ctx);
fail_ctx:
    vmaf_metal_context_destroy(s->ctx);
    s->ctx = NULL;
fail_host:
    free(s->h_ref_lin);
    free(s->h_dis_lin);
    free(s->h_ref_lin_ds);
    free(s->h_dis_lin_ds);
    s->h_ref_lin = s->h_dis_lin = s->h_ref_lin_ds = s->h_dis_lin_ds = NULL;
    return err;
}

/* Dispatch one ssimulacra2_mul3 (a*b -> out) for the current scale. */
static void ss2m_encode_mul3(Ssimu2StateMetal *s, id<MTLCommandBuffer> cmd, id<MTLBuffer> a,
                             id<MTLBuffer> b, id<MTLBuffer> out, unsigned cw, unsigned ch)
{
    id<MTLComputePipelineState> pso = (__bridge id<MTLComputePipelineState>)s->pso_mul3;
    const uint32_t plane_stride = s->width * s->height;
    const uint32_t params[4] = {(uint32_t)cw, (uint32_t)ch, 3u, plane_stride};
    const size_t gx = (cw + SS2M_MUL_BX - 1u) / SS2M_MUL_BX;
    const size_t gy = (ch + SS2M_MUL_BY - 1u) / SS2M_MUL_BY;

    id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
    [enc setComputePipelineState:pso];
    [enc setBuffer:a   offset:0 atIndex:0];
    [enc setBuffer:b   offset:0 atIndex:1];
    [enc setBuffer:out offset:0 atIndex:2];
    [enc setBytes:params length:sizeof(params) atIndex:3];
    MTLSize tg   = MTLSizeMake(SS2M_MUL_BX, SS2M_MUL_BY, 1);
    MTLSize grid = MTLSizeMake(gx, gy, 1);
    [enc dispatchThreadgroups:grid threadsPerThreadgroup:tg];
    [enc endEncoding];
}

/* Dispatch one separable 3-plane blur (in -> scratch -> out). */
static void ss2m_encode_blur3(Ssimu2StateMetal *s, id<MTLCommandBuffer> cmd, id<MTLBuffer> in_buf,
                              id<MTLBuffer> out_buf, id<MTLBuffer> scratch, unsigned cw, unsigned ch)
{
    id<MTLComputePipelineState> pso_h = (__bridge id<MTLComputePipelineState>)s->pso_blur_h;
    id<MTLComputePipelineState> pso_v = (__bridge id<MTLComputePipelineState>)s->pso_blur_v;
    const uint32_t plane_stride = s->width * s->height;
    const uint32_t params[4] = {(uint32_t)cw, (uint32_t)ch, (uint32_t)s->rg_radius, plane_stride};
    const float n2v[4] = {s->rg_n2[0], s->rg_n2[1], s->rg_n2[2], 0.0f};
    const float d1v[4] = {s->rg_d1[0], s->rg_d1[1], s->rg_d1[2], 0.0f};

    /* H pass: in -> scratch, one thread per row, z=3 fuses XYB planes. */
    {
        const size_t gx = (ch + SS2M_BLUR_BLOCK - 1u) / SS2M_BLUR_BLOCK;
        id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
        [enc setComputePipelineState:pso_h];
        [enc setBuffer:in_buf  offset:0 atIndex:0];
        [enc setBuffer:scratch offset:0 atIndex:1];
        [enc setBytes:params length:sizeof(params) atIndex:2];
        [enc setBytes:n2v    length:sizeof(n2v)    atIndex:3];
        [enc setBytes:d1v    length:sizeof(d1v)    atIndex:4];
        MTLSize tg   = MTLSizeMake(SS2M_BLUR_BLOCK, 1, 1);
        MTLSize grid = MTLSizeMake(gx, 1, 3);
        [enc dispatchThreadgroups:grid threadsPerThreadgroup:tg];
        [enc endEncoding];
    }
    /* V pass: scratch -> out, one thread per column, z=3 fuses XYB planes. */
    {
        const size_t gx = (cw + SS2M_BLUR_BLOCK - 1u) / SS2M_BLUR_BLOCK;
        id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
        [enc setComputePipelineState:pso_v];
        [enc setBuffer:scratch offset:0 atIndex:0];
        [enc setBuffer:out_buf offset:0 atIndex:1];
        [enc setBytes:params length:sizeof(params) atIndex:2];
        [enc setBytes:n2v    length:sizeof(n2v)    atIndex:3];
        [enc setBytes:d1v    length:sizeof(d1v)    atIndex:4];
        MTLSize tg   = MTLSizeMake(SS2M_BLUR_BLOCK, 1, 1);
        MTLSize grid = MTLSizeMake(gx, 1, 3);
        [enc dispatchThreadgroups:grid threadsPerThreadgroup:tg];
        [enc endEncoding];
    }
}

/* Per-scale GPU work: upload XYB, run 3 mul + 5 blur, in one command
 * buffer. Mirrors ssimulacra2_cuda.c::ss2c_run_scale_gpu step-by-step;
 * splitting would obscure the dispatch ordering required for parity
 * audit (ADR-0141).
 * NOLINTNEXTLINE(readability-function-size,google-readability-function-size) */
static int ss2m_run_scale_gpu(Ssimu2StateMetal *s, id<MTLCommandQueue> queue, unsigned cw,
                              unsigned ch)
{
    id<MTLBuffer> ref_xyb = (__bridge id<MTLBuffer>)s->buf_ref_xyb;
    id<MTLBuffer> dis_xyb = (__bridge id<MTLBuffer>)s->buf_dis_xyb;
    id<MTLBuffer> mul     = (__bridge id<MTLBuffer>)s->buf_mul;
    id<MTLBuffer> scratch = (__bridge id<MTLBuffer>)s->buf_blur_scr;
    id<MTLBuffer> mu1     = (__bridge id<MTLBuffer>)s->buf_mu1;
    id<MTLBuffer> mu2     = (__bridge id<MTLBuffer>)s->buf_mu2;
    id<MTLBuffer> s11     = (__bridge id<MTLBuffer>)s->buf_s11;
    id<MTLBuffer> s22     = (__bridge id<MTLBuffer>)s->buf_s22;
    id<MTLBuffer> s12     = (__bridge id<MTLBuffer>)s->buf_s12;

    id<MTLCommandBuffer> cmd = [queue commandBuffer];
    if (cmd == nil) { return -ENOMEM; }

    /* 1) ref^2 -> mul -> blur into s11. */
    ss2m_encode_mul3(s, cmd, ref_xyb, ref_xyb, mul, cw, ch);
    ss2m_encode_blur3(s, cmd, mul, s11, scratch, cw, ch);
    /* 2) dis^2 -> mul -> blur into s22. */
    ss2m_encode_mul3(s, cmd, dis_xyb, dis_xyb, mul, cw, ch);
    ss2m_encode_blur3(s, cmd, mul, s22, scratch, cw, ch);
    /* 3) ref*dis -> mul -> blur into s12. */
    ss2m_encode_mul3(s, cmd, ref_xyb, dis_xyb, mul, cw, ch);
    ss2m_encode_blur3(s, cmd, mul, s12, scratch, cw, ch);
    /* 4) blur ref_xyb -> mu1. */
    ss2m_encode_blur3(s, cmd, ref_xyb, mu1, scratch, cw, ch);
    /* 5) blur dis_xyb -> mu2. */
    ss2m_encode_blur3(s, cmd, dis_xyb, mu2, scratch, cw, ch);

    [cmd commit];
    [cmd waitUntilCompleted];
    return 0;
}

/* submit: orchestrate the per-scale pyramid. Mirrors the CPU/CUDA
 * extract loop step-by-step (ADR-0141).
 * NOLINTNEXTLINE(readability-function-size,google-readability-function-size) */
static int submit_fex_metal(VmafFeatureExtractor *fex, VmafPicture *ref_pic,
                            VmafPicture *ref_pic_90, VmafPicture *dist_pic,
                            VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic_90;
    (void)dist_pic_90;
    (void)index;
    Ssimu2StateMetal *s = (Ssimu2StateMetal *)fex->priv;

    void *dh = vmaf_metal_context_device_handle(s->ctx);
    void *qh = vmaf_metal_context_queue_handle(s->ctx);
    if (dh == NULL || qh == NULL) { return -ENODEV; }
    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)qh;

    /* Stage 1: host YUV -> linear RGB on the full-res frame. */
    ss2m_picture_to_linear_rgb(s, ref_pic, s->h_ref_lin);
    ss2m_picture_to_linear_rgb(s, dist_pic, s->h_dis_lin);

    /* Per-scale loop. */
    s->accum_completed = 0;
    memset(s->accum_ssim, 0, sizeof(s->accum_ssim));
    memset(s->accum_ed, 0, sizeof(s->accum_ed));

    unsigned cw = s->width;
    unsigned ch = s->height;
    const size_t plane_full = (size_t)s->width * (size_t)s->height;

    float *ref_xyb_host = (float *)[(__bridge id<MTLBuffer>)s->buf_ref_xyb contents];
    float *dis_xyb_host = (float *)[(__bridge id<MTLBuffer>)s->buf_dis_xyb contents];

    for (int scale = 0; scale < SS2M_NUM_SCALES; scale++) {
        if (cw < 8u || ch < 8u) { break; }

        /* Host XYB into the GPU-visible (Shared) XYB buffers. */
        ss2m_host_linear_rgb_to_xyb(s->h_ref_lin, ref_xyb_host, cw, ch, plane_full);
        ss2m_host_linear_rgb_to_xyb(s->h_dis_lin, dis_xyb_host, cw, ch, plane_full);

        int err = ss2m_run_scale_gpu(s, queue, cw, ch);
        if (err != 0) { return err; }

        /* Host combine — reads GPU outputs directly (unified memory). */
        const float *h_mu1 = (const float *)[(__bridge id<MTLBuffer>)s->buf_mu1 contents];
        const float *h_mu2 = (const float *)[(__bridge id<MTLBuffer>)s->buf_mu2 contents];
        const float *h_s11 = (const float *)[(__bridge id<MTLBuffer>)s->buf_s11 contents];
        const float *h_s22 = (const float *)[(__bridge id<MTLBuffer>)s->buf_s22 contents];
        const float *h_s12 = (const float *)[(__bridge id<MTLBuffer>)s->buf_s12 contents];
        ss2m_host_combine(s, cw, ch, h_mu1, h_mu2, h_s11, h_s22, h_s12, ref_xyb_host, dis_xyb_host,
                          s->accum_ssim[scale], s->accum_ed[scale]);
        s->accum_completed++;

        if (scale + 1 < SS2M_NUM_SCALES) {
            const unsigned nw = (cw + 1) / 2;
            const unsigned nh = (ch + 1) / 2;
            ss2m_downsample_2x2(s->h_ref_lin, cw, ch, s->h_ref_lin_ds, nw, nh, plane_full);
            for (int c = 0; c < 3; c++) {
                memcpy(s->h_ref_lin + (size_t)c * plane_full,
                       s->h_ref_lin_ds + (size_t)c * plane_full,
                       (size_t)nw * (size_t)nh * sizeof(float));
            }
            ss2m_downsample_2x2(s->h_dis_lin, cw, ch, s->h_dis_lin_ds, nw, nh, plane_full);
            for (int c = 0; c < 3; c++) {
                memcpy(s->h_dis_lin + (size_t)c * plane_full,
                       s->h_dis_lin_ds + (size_t)c * plane_full,
                       (size_t)nw * (size_t)nh * sizeof(float));
            }
            cw = nw;
            ch = nh;
        }
    }
    return 0;
}

static int collect_fex_metal(VmafFeatureExtractor *fex, unsigned index,
                             VmafFeatureCollector *feature_collector)
{
    Ssimu2StateMetal *s = (Ssimu2StateMetal *)fex->priv;
    const double score = ss2m_pool_score(s->accum_ssim, s->accum_ed, s->accum_completed);
    return vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "ssimulacra2", score, index);
}

static int close_fex_metal(VmafFeatureExtractor *fex)
{
    Ssimu2StateMetal *s = (Ssimu2StateMetal *)fex->priv;
    int rc = vmaf_metal_kernel_lifecycle_close(&s->lc, s->ctx);

    if (s->pso_blur_v) { (void)(__bridge_transfer id<MTLComputePipelineState>)s->pso_blur_v; s->pso_blur_v = NULL; }
    if (s->pso_blur_h) { (void)(__bridge_transfer id<MTLComputePipelineState>)s->pso_blur_h; s->pso_blur_h = NULL; }
    if (s->pso_mul3)   { (void)(__bridge_transfer id<MTLComputePipelineState>)s->pso_mul3;   s->pso_mul3   = NULL; }

    ss2m_release_device_buffers(s);

    free(s->h_ref_lin);
    free(s->h_dis_lin);
    free(s->h_ref_lin_ds);
    free(s->h_dis_lin_ds);
    s->h_ref_lin = s->h_dis_lin = s->h_ref_lin_ds = s->h_dis_lin_ds = NULL;

    if (s->feature_name_dict) {
        int err = vmaf_dictionary_free(&s->feature_name_dict);
        if (err != 0 && rc == 0) { rc = err; }
    }
    if (s->ctx) {
        vmaf_metal_context_destroy(s->ctx);
        s->ctx = NULL;
    }
    return rc;
}

static const char *provided_features[] = {"ssimulacra2", NULL};

extern "C" {
/* Registered via extern in feature_extractor.c's feature_extractor_list[];
 * making this static would unlink the extractor from the registry — same
 * pattern every CUDA / HIP / SYCL feature extractor uses (ADR-0361 Metal
 * backend; ADR-0278 cite form). */
// NOLINTNEXTLINE(misc-use-internal-linkage) — ADR-0361 / ADR-0278
VmafFeatureExtractor vmaf_fex_ssimulacra2_metal = {
    .name              = "ssimulacra2_metal",
    .init              = init_fex_metal,
    .submit            = submit_fex_metal,
    .collect           = collect_fex_metal,
    .flush             = NULL,
    .close             = close_fex_metal,
    .options           = options,
    .priv_size         = sizeof(Ssimu2StateMetal),
    .provided_features = provided_features,
    .flags             = VMAF_FEATURE_EXTRACTOR_METAL,
    .chars = {
        /* Per scale: 3 mul + 5 blur*(H+V) = 3 + 10 = 13 dispatches;
         * worst-case 6 scales = 78. */
        .n_dispatches_per_frame = 13 * SS2M_NUM_SCALES,
        .is_reduction_only      = false,
        .min_useful_frame_area  = 1920U * 1080U,
        .dispatch_hint          = VMAF_FEATURE_DISPATCH_AUTO,
    },
};
} /* extern "C" */
