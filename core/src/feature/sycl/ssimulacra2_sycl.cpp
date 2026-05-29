/**
 *  Copyright 2026 Lusoris and Claude (Anthropic)
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 *
 *  ssimulacra2 feature kernel on the SYCL backend (T7-23 / GPU
 *  long-tail batch 3 part 7c — ADR-0192 / ADR-0204). SYCL twin of
 *  ssimulacra2_vulkan (PR #156 / ADR-0201) and ssimulacra2_cuda.
 *
 *  Pipeline (per ADR-0201 — same shape as Vulkan + CUDA twins):
 *    1. Host: YUV → linear RGB on full-res frame (deterministic LUT
 *       sRGB EOTF, ADR-0164).
 *    2. Per-scale (up to 6, breaks early when min-dim < 8):
 *       a. Host: linear RGB → XYB (verbatim port of CPU
 *          `linear_rgb_to_xyb`). Computed on host because the GPU
 *          float `cbrt` differs from libm by 42 ULP at the bit
 *          level — that drift cascades through the IIR + 108-weight
 *          pool to a ~1.5e-2 pooled-score drift on the Vulkan port,
 *          and the same fix carries over to SYCL by construction.
 *          See ADR-0201 §Precision investigation.
 *       b. GPU: 3 elementwise 3-plane multiplies (ref², dis², ref·dis).
 *       c. GPU: 5 separable IIR blurs (s11, s22, s12, mu1, mu2).
 *          One work-item per row (H pass) / per column (V pass).
 *       d. Host: per-pixel SSIM + EdgeDiff combine in double
 *          precision (verbatim ports of `ssim_map` + `edge_diff_map`).
 *       e. Host: 2×2 box downsample of the linear-RGB pyramid.
 *    3. Host: pool 108 weighted norms via the libjxl polynomial.
 *
 *  Precision: places=4 (max_abs_diff ≤ 5e-5). icpx is invoked with
 *  -fp-model=precise (already on the SYCL feature build line in
 *  libvmaf/src/meson.build), which blocks FMA contraction in the
 *  SYCL kernel lambdas — equivalent to NVCC's --fmad=false.
 */

#include <sycl/sycl.hpp>

#include <cassert>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstring>

extern "C" {
#include "config.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "log.h"
#include "picture.h"
#include "sycl/common.h"

#include "feature/ssimulacra2_math.h"
}

namespace
{

constexpr int SS2S_NUM_SCALES = 6;
constexpr int SS2S_BLUR_BLOCK = 64;
constexpr int SS2S_MUL_BX = 16;
constexpr int SS2S_MUL_BY = 8;
constexpr double SS2S_SIGMA = 1.5;
constexpr double SS2S_PI = 3.14159265358979323846;

enum YuvMatrix : int {
    SS2S_MATRIX_BT709_LIMITED = 0,
    SS2S_MATRIX_BT601_LIMITED = 1,
    SS2S_MATRIX_BT709_FULL = 2,
    SS2S_MATRIX_BT601_FULL = 3,
};

struct Ssimu2StateSycl {
    int yuv_matrix;

    unsigned width;
    unsigned height;
    unsigned bpc;
    unsigned scale_w[SS2S_NUM_SCALES];
    unsigned scale_h[SS2S_NUM_SCALES];

    float rg_n2[3];
    float rg_d1[3];
    int rg_radius;

    VmafSyclState *sycl_state;

    /* Device buffers (USM device allocations) — 3-plane contiguous,
     * full-resolution stride kept across pyramid scales. */
    float *d_ref_xyb;
    float *d_dis_xyb;
    float *d_mul_buf;
    float *d_blur_scratch;
    float *d_mu1;
    float *d_mu2;
    float *d_s11;
    float *d_s22;
    float *d_s12;

    /* Host buffers (USM host allocations — pinned). */
    float *h_ref_lin;
    float *h_dis_lin;
    float *h_ref_xyb;
    float *h_dis_xyb;
    float *h_mu1;
    float *h_mu2;
    float *h_s11;
    float *h_s22;
    float *h_s12;
};

/* libjxl 108 pooling weights — bit-identical to ssimulacra2.c::kWeights. */
const double g_weights[108] = {
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

/* ---------------------------------------------------------------- */
/* Recursive Gaussian coefficient setup — verbatim port.            */
/* ---------------------------------------------------------------- */

void ss2s_setup_gaussian(Ssimu2StateSycl *s, double sigma)
{
    assert(s != nullptr);
    assert(sigma > 0.0);
    const double radius = std::round(3.2795 * sigma + 0.2546);
    const double pi_div_2r = SS2S_PI / (2.0 * radius);
    const double omega[3] = {pi_div_2r, 3.0 * pi_div_2r, 5.0 * pi_div_2r};

    const double p1 = +1.0 / std::tan(0.5 * omega[0]);
    const double p3 = -1.0 / std::tan(0.5 * omega[1]);
    const double p5 = +1.0 / std::tan(0.5 * omega[2]);
    const double r1 = +p1 * p1 / std::sin(omega[0]);
    const double r3 = -p3 * p3 / std::sin(omega[1]);
    const double r5 = +p5 * p5 / std::sin(omega[2]);

    const double neg_half_sigma2 = -0.5 * sigma * sigma;
    const double recip_r = 1.0 / radius;
    double rho[3];
    for (int i = 0; i < 3; i++)
        rho[i] = std::exp(neg_half_sigma2 * omega[i] * omega[i]) * recip_r;

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
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++)
                M[i][j] = A[i][j];
        for (int i = 0; i < 3; i++)
            M[i][col] = gamma[i];
        beta[col] = (M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1]) -
                     M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0]) +
                     M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0])) *
                    inv_det;
    }

    s->rg_radius = (int)radius;
    for (int i = 0; i < 3; i++) {
        s->rg_n2[i] = (float)(-beta[i] * std::cos(omega[i] * (radius + 1.0)));
        s->rg_d1[i] = (float)(-2.0 * std::cos(omega[i]));
    }
}

/* ---------------------------------------------------------------- */
/* Host-side YUV → linear-RGB + linear-RGB → XYB + 2×2 downsample.   */
/* ---------------------------------------------------------------- */

inline float ss2s_clampf(float v, float lo, float hi)
{
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

inline float ss2s_read_plane(const VmafPicture *pic, int plane, int x, int y)
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

void ss2s_picture_to_linear_rgb(const Ssimu2StateSycl *s, const VmafPicture *pic, float *out)
{
    assert(s != nullptr);
    assert(pic != nullptr);
    assert(out != nullptr);
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
    case SS2S_MATRIX_BT709_FULL:
        limited = 0;
        [[fallthrough]];
    case SS2S_MATRIX_BT709_LIMITED:
        kr = 0.2126f;
        kg = 0.7152f;
        kb = 0.0722f;
        break;
    case SS2S_MATRIX_BT601_FULL:
        limited = 0;
        [[fallthrough]];
    case SS2S_MATRIX_BT601_LIMITED:
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
            float Y = ss2s_read_plane(pic, 0, (int)x, (int)y) * inv_peak;
            float U = ss2s_read_plane(pic, 1, (int)x, (int)y) * inv_peak;
            float V = ss2s_read_plane(pic, 2, (int)x, (int)y) * inv_peak;
            float Yn = (Y - y_off) * y_scale;
            float Un = (U - c_off) * c_scale;
            float Vn = (V - c_off) * c_scale;
            float R = Yn + cr_r * Vn;
            float G = Yn + cb_g * Un + cr_g * Vn;
            float B = Yn + cb_b * Un;
            R = ss2s_clampf(R, 0.0f, 1.0f);
            G = ss2s_clampf(G, 0.0f, 1.0f);
            B = ss2s_clampf(B, 0.0f, 1.0f);
            const size_t idx = (size_t)y * w + x;
            rp[idx] = vmaf_ss2_srgb_eotf(R);
            gp[idx] = vmaf_ss2_srgb_eotf(G);
            bp[idx] = vmaf_ss2_srgb_eotf(B);
        }
    }
}

void ss2s_host_linear_rgb_to_xyb(const float *lin, float *xyb, unsigned w, unsigned h,
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
        float s2 = kM20 * r + kM21 * g + m22 * b + kOpsinBias;
        if (l < 0.0f)
            l = 0.0f;
        if (m < 0.0f)
            m = 0.0f;
        if (s2 < 0.0f)
            s2 = 0.0f;
        float L = vmaf_ss2_cbrtf(l) - cbrt_bias;
        float M = vmaf_ss2_cbrtf(m) - cbrt_bias;
        float S = vmaf_ss2_cbrtf(s2) - cbrt_bias;
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

void ss2s_downsample_2x2(const float *in, unsigned iw, unsigned ih, float *out, unsigned ow,
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

/* ---------------------------------------------------------------- */
/* GPU kernels                                                       */
/* ---------------------------------------------------------------- */

/* Elementwise 3-plane multiply. Mirrors ssimulacra2_mul3 (CUDA) and
 * ssimulacra2_mul.comp (Vulkan). */
sycl::event launch_mul3(sycl::queue &q, const float *a, const float *b, float *out, unsigned width,
                        unsigned height, unsigned plane_stride)
{
    const size_t global_x = ((width + SS2S_MUL_BX - 1u) / SS2S_MUL_BX) * SS2S_MUL_BX;
    const size_t global_y = ((height + SS2S_MUL_BY - 1u) / SS2S_MUL_BY) * SS2S_MUL_BY;
    return q.submit([&](sycl::handler &h) {
        const unsigned e_w = width;
        const unsigned e_h = height;
        const unsigned e_stride = plane_stride;
        h.parallel_for(sycl::nd_range<2>(sycl::range<2>(global_y, global_x),
                                         sycl::range<2>(SS2S_MUL_BY, SS2S_MUL_BX)),
                       [=](sycl::nd_item<2> it) {
                           const unsigned x = (unsigned)it.get_global_id(1);
                           const unsigned y = (unsigned)it.get_global_id(0);
                           if (x >= e_w || y >= e_h)
                               return;
                           const unsigned base = y * e_w + x;
                           for (unsigned c = 0u; c < 3u; ++c) {
                               const unsigned idx = base + c * e_stride;
                               out[idx] = a[idx] * b[idx];
                           }
                       });
    });
}

/* IIR blur — separable 3-pole recursive Gaussian. One work-item per
 * row (PASS=0) or per column (PASS=1). */
template <int PASS>
sycl::event launch_blur(sycl::queue &q, const float *in_buf, float *out_buf, unsigned width,
                        unsigned height, float n2_0, float n2_1, float n2_2, float d1_0, float d1_1,
                        float d1_2, int radius, unsigned in_offset, unsigned out_offset,
                        unsigned lines)
{
    const size_t global = ((lines + SS2S_BLUR_BLOCK - 1u) / SS2S_BLUR_BLOCK) * SS2S_BLUR_BLOCK;
    return q.submit([&](sycl::handler &h) {
        const unsigned e_w = width;
        const unsigned e_h = height;
        const unsigned e_in = in_offset;
        const unsigned e_out = out_offset;
        const unsigned e_lines = lines;
        const float c_n2_0 = n2_0;
        const float c_n2_1 = n2_1;
        const float c_n2_2 = n2_2;
        const float c_d1_0 = d1_0;
        const float c_d1_1 = d1_1;
        const float c_d1_2 = d1_2;
        const int c_N = radius;
        h.parallel_for(sycl::nd_range<1>(sycl::range<1>(global), sycl::range<1>(SS2S_BLUR_BLOCK)),
                       [=](sycl::nd_item<1> it) {
                           const unsigned line = (unsigned)it.get_global_id(0);
                           if (line >= e_lines)
                               return;

                           float prev1_0 = 0.f, prev1_1 = 0.f, prev1_2 = 0.f;
                           float prev2_0 = 0.f, prev2_1 = 0.f, prev2_2 = 0.f;

                           const int xsize = (PASS == 0) ? (int)e_w : (int)e_h;
                           /* base addresses depend on pass */
                           const unsigned width_v = e_w;
                           const unsigned in_off = e_in;
                           const unsigned out_off = e_out;
                           const unsigned col = line; /* used only when PASS=1 */
                           const unsigned row = line; /* used only when PASS=0 */

                           for (int n = -c_N + 1; n < xsize; ++n) {
                               const int left = n - c_N - 1;
                               const int right = n + c_N - 1;
                               float lv = 0.f;
                               float rv = 0.f;
                               if (PASS == 0) {
                                   const unsigned base = in_off + row * width_v;
                                   if (left >= 0)
                                       lv = in_buf[base + (unsigned)left];
                                   if (right < xsize)
                                       rv = in_buf[base + (unsigned)right];
                               } else {
                                   if (left >= 0)
                                       lv = in_buf[in_off + (unsigned)left * width_v + col];
                                   if (right < xsize)
                                       rv = in_buf[in_off + (unsigned)right * width_v + col];
                               }
                               const float sum = lv + rv;

                               /* Bit-identical CPU expression order. The icpx
                     * -fp-model=precise flag (set in
                     * libvmaf/src/meson.build) blocks contraction. */
                               const float ns0 = c_n2_0 * sum;
                               const float dp0 = c_d1_0 * prev1_0;
                               const float t0 = ns0 - dp0;
                               const float o0 = t0 - prev2_0;
                               const float ns1 = c_n2_1 * sum;
                               const float dp1 = c_d1_1 * prev1_1;
                               const float t1 = ns1 - dp1;
                               const float o1 = t1 - prev2_1;
                               const float ns2 = c_n2_2 * sum;
                               const float dp2 = c_d1_2 * prev1_2;
                               const float t2 = ns2 - dp2;
                               const float o2 = t2 - prev2_2;
                               prev2_0 = prev1_0;
                               prev2_1 = prev1_1;
                               prev2_2 = prev1_2;
                               prev1_0 = o0;
                               prev1_1 = o1;
                               prev1_2 = o2;

                               if (n >= 0) {
                                   const float s01 = o0 + o1;
                                   const float s_total = s01 + o2;
                                   if (PASS == 0)
                                       out_buf[out_off + row * width_v + (unsigned)n] = s_total;
                                   else
                                       out_buf[out_off + (unsigned)n * width_v + col] = s_total;
                               }
                           }
                       });
    });
}

void blur_3plane(sycl::queue &q, Ssimu2StateSycl *s, const float *in_buf, float *out_buf, int scale)
{
    const unsigned cw = s->scale_w[scale];
    const unsigned ch = s->scale_h[scale];
    const unsigned full_plane = s->width * s->height;
    for (int c = 0; c < 3; c++) {
        const unsigned off = (unsigned)c * full_plane;
        launch_blur<0>(q, in_buf, s->d_blur_scratch, cw, ch, s->rg_n2[0], s->rg_n2[1], s->rg_n2[2],
                       s->rg_d1[0], s->rg_d1[1], s->rg_d1[2], s->rg_radius, off, off, ch);
        launch_blur<1>(q, s->d_blur_scratch, out_buf, cw, ch, s->rg_n2[0], s->rg_n2[1], s->rg_n2[2],
                       s->rg_d1[0], s->rg_d1[1], s->rg_d1[2], s->rg_radius, off, off, cw);
    }
}

/* Per-pixel SSIM + EdgeDiff combine in double precision. Verbatim
 * ports of ssim_map + edge_diff_map in ssimulacra2.c. */
void ss2s_host_combine(const Ssimu2StateSycl *s, int scale, double avg_ssim[6], double avg_ed[12])
{
    assert(s != nullptr);
    assert(avg_ssim != nullptr && avg_ed != nullptr);
    assert(scale >= 0 && scale < 6);
    const unsigned cw = s->scale_w[scale];
    const unsigned ch = s->scale_h[scale];
    const size_t full_plane = (size_t)s->width * (size_t)s->height;
    const size_t scale_pixels = (size_t)cw * (size_t)ch;
    const double inv_pixels = 1.0 / (double)scale_pixels;
    for (int c = 0; c < 3; c++) {
        const float *m1 = s->h_mu1 + (size_t)c * full_plane;
        const float *m2 = s->h_mu2 + (size_t)c * full_plane;
        const float *s11p = s->h_s11 + (size_t)c * full_plane;
        const float *s22p = s->h_s22 + (size_t)c * full_plane;
        const float *s12p = s->h_s12 + (size_t)c * full_plane;
        const float *r1 = s->h_ref_xyb + (size_t)c * full_plane;
        const float *r2 = s->h_dis_xyb + (size_t)c * full_plane;
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
            const float num_s = 2.0f * (s12p[i] - u12) + 0.0009f;
            const float denom_s = (s11p[i] - u11) + (s22p[i] - u22) + 0.0009f;
            double d = 1.0 - ((double)num_m * (double)num_s / (double)denom_s);
            if (d < 0.0)
                d = 0.0;
            sum_l1 += d;
            const double d2 = d * d;
            sum_l4 += d2 * d2;
            const double ed1 = std::fabs((double)r1[i] - (double)u1);
            const double ed2 = std::fabs((double)r2[i] - (double)u2);
            const double dd = (1.0 + ed2) / (1.0 + ed1) - 1.0;
            const double art = dd > 0.0 ? dd : 0.0;
            const double det = dd < 0.0 ? -dd : 0.0;
            e_art += art;
            const double a2 = art * art;
            e_art4 += a2 * a2;
            e_det += det;
            const double d2e = det * det;
            e_det4 += d2e * d2e;
        }
        avg_ssim[c * 2 + 0] = inv_pixels * sum_l1;
        avg_ssim[c * 2 + 1] = std::sqrt(std::sqrt(inv_pixels * sum_l4));
        avg_ed[c * 4 + 0] = inv_pixels * e_art;
        avg_ed[c * 4 + 1] = std::sqrt(std::sqrt(inv_pixels * e_art4));
        avg_ed[c * 4 + 2] = inv_pixels * e_det;
        avg_ed[c * 4 + 3] = std::sqrt(std::sqrt(inv_pixels * e_det4));
    }
}

double ss2s_pool_score(const double avg_ssim[6][6], const double avg_ed[6][12], int num_scales)
{
    assert(avg_ssim != nullptr && avg_ed != nullptr);
    assert(num_scales >= 1 && num_scales <= 6);
    double ssim = 0.0;
    size_t i = 0;
    for (int c = 0; c < 3; c++) {
        for (int scale = 0; scale < 6; scale++) {
            for (int n = 0; n < 2; n++) {
                double s_term = scale < num_scales ? avg_ssim[scale][c * 2 + n] : 0.0;
                double r_term = scale < num_scales ? avg_ed[scale][c * 4 + n] : 0.0;
                double b_term = scale < num_scales ? avg_ed[scale][c * 4 + n + 2] : 0.0;
                ssim += g_weights[i++] * std::fabs(s_term);
                ssim += g_weights[i++] * std::fabs(r_term);
                ssim += g_weights[i++] * std::fabs(b_term);
            }
        }
    }
    ssim *= 0.9562382616834844;
    ssim = 2.326765642916932 * ssim - 0.020884521182843837 * ssim * ssim +
           6.248496625763138e-05 * ssim * ssim * ssim;
    if (ssim > 0.0) {
        ssim = 100.0 - 10.0 * std::pow(ssim, 0.6276336467831387);
    } else {
        ssim = 100.0;
    }
    return ssim;
}

/* ---------------------------------------------------------------- */
/* Lifecycle                                                         */
/* ---------------------------------------------------------------- */

const VmafOption options_ssimulacra2_sycl[] = {
    {.name = "yuv_matrix",
     .help = "YUV→RGB matrix: 0=bt709_limited (default), 1=bt601_limited, "
             "2=bt709_full, 3=bt601_full",
     .offset = offsetof(Ssimu2StateSycl, yuv_matrix),
     .type = VMAF_OPT_TYPE_INT,
     .default_val = {.i = SS2S_MATRIX_BT709_LIMITED},
     .min = 0,
     .max = 3},
    {0},
};

int init_fex_sycl(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc, unsigned w,
                  unsigned h)
{
    (void)pix_fmt;
    auto *s = static_cast<Ssimu2StateSycl *>(fex->priv);

    if (w < 8u || h < 8u) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "ssimulacra2_sycl: input %ux%u below 8x8 lower bound\n", w,
                 h);
        return -EINVAL;
    }

    s->width = w;
    s->height = h;
    s->bpc = bpc;
    ss2s_setup_gaussian(s, SS2S_SIGMA);

    s->scale_w[0] = w;
    s->scale_h[0] = h;
    for (int i = 1; i < SS2S_NUM_SCALES; i++) {
        s->scale_w[i] = (s->scale_w[i - 1] + 1) / 2;
        s->scale_h[i] = (s->scale_h[i - 1] + 1) / 2;
    }

    if (!fex->sycl_state)
        return -EINVAL;
    s->sycl_state = fex->sycl_state;

    const size_t three_plane_bytes = 3u * (size_t)w * (size_t)h * sizeof(float);

    s->d_ref_xyb = static_cast<float *>(vmaf_sycl_malloc_device(s->sycl_state, three_plane_bytes));
    s->d_dis_xyb = static_cast<float *>(vmaf_sycl_malloc_device(s->sycl_state, three_plane_bytes));
    s->d_mul_buf = static_cast<float *>(vmaf_sycl_malloc_device(s->sycl_state, three_plane_bytes));
    s->d_blur_scratch =
        static_cast<float *>(vmaf_sycl_malloc_device(s->sycl_state, three_plane_bytes));
    s->d_mu1 = static_cast<float *>(vmaf_sycl_malloc_device(s->sycl_state, three_plane_bytes));
    s->d_mu2 = static_cast<float *>(vmaf_sycl_malloc_device(s->sycl_state, three_plane_bytes));
    s->d_s11 = static_cast<float *>(vmaf_sycl_malloc_device(s->sycl_state, three_plane_bytes));
    s->d_s22 = static_cast<float *>(vmaf_sycl_malloc_device(s->sycl_state, three_plane_bytes));
    s->d_s12 = static_cast<float *>(vmaf_sycl_malloc_device(s->sycl_state, three_plane_bytes));
    s->h_ref_lin = static_cast<float *>(vmaf_sycl_malloc_host(s->sycl_state, three_plane_bytes));
    s->h_dis_lin = static_cast<float *>(vmaf_sycl_malloc_host(s->sycl_state, three_plane_bytes));
    s->h_ref_xyb = static_cast<float *>(vmaf_sycl_malloc_host(s->sycl_state, three_plane_bytes));
    s->h_dis_xyb = static_cast<float *>(vmaf_sycl_malloc_host(s->sycl_state, three_plane_bytes));
    s->h_mu1 = static_cast<float *>(vmaf_sycl_malloc_host(s->sycl_state, three_plane_bytes));
    s->h_mu2 = static_cast<float *>(vmaf_sycl_malloc_host(s->sycl_state, three_plane_bytes));
    s->h_s11 = static_cast<float *>(vmaf_sycl_malloc_host(s->sycl_state, three_plane_bytes));
    s->h_s22 = static_cast<float *>(vmaf_sycl_malloc_host(s->sycl_state, three_plane_bytes));
    s->h_s12 = static_cast<float *>(vmaf_sycl_malloc_host(s->sycl_state, three_plane_bytes));

    if (!s->d_ref_xyb || !s->d_dis_xyb || !s->d_mul_buf || !s->d_blur_scratch || !s->d_mu1 ||
        !s->d_mu2 || !s->d_s11 || !s->d_s22 || !s->d_s12 || !s->h_ref_lin || !s->h_dis_lin ||
        !s->h_ref_xyb || !s->h_dis_xyb || !s->h_mu1 || !s->h_mu2 || !s->h_s11 || !s->h_s22 ||
        !s->h_s12)
        return -ENOMEM;
    return 0;
}

int extract_fex_sycl(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                     VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index,
                     VmafFeatureCollector *feature_collector)
{
    (void)ref_pic_90;
    (void)dist_pic_90;
    auto *s = static_cast<Ssimu2StateSycl *>(fex->priv);
    auto *qptr = static_cast<sycl::queue *>(vmaf_sycl_get_queue_ptr(s->sycl_state));
    if (!qptr)
        return -EINVAL;
    sycl::queue &q = *qptr;

    /* Stage 1: host YUV → linear RGB on the pinned host buffers. */
    ss2s_picture_to_linear_rgb(s, ref_pic, s->h_ref_lin);
    ss2s_picture_to_linear_rgb(s, dist_pic, s->h_dis_lin);

    /* Stage 2: per-scale loop. */
    double avg_ssim[6][6] = {{0}};
    double avg_ed[6][12] = {{0}};
    int completed = 0;
    unsigned cw = s->width;
    unsigned ch = s->height;
    const size_t plane_full = (size_t)s->width * (size_t)s->height;
    const size_t three_plane_bytes = 3u * plane_full * sizeof(float);

    for (int scale = 0; scale < SS2S_NUM_SCALES; scale++) {
        if (cw < 8u || ch < 8u)
            break;

        ss2s_host_linear_rgb_to_xyb(s->h_ref_lin, s->h_ref_xyb, cw, ch, plane_full);
        ss2s_host_linear_rgb_to_xyb(s->h_dis_lin, s->h_dis_xyb, cw, ch, plane_full);

        q.memcpy(s->d_ref_xyb, s->h_ref_xyb, three_plane_bytes);
        q.memcpy(s->d_dis_xyb, s->h_dis_xyb, three_plane_bytes);

        /* 1) ref² → mul_buf → blur into s11. */
        launch_mul3(q, s->d_ref_xyb, s->d_ref_xyb, s->d_mul_buf, cw, ch, (unsigned)plane_full);
        blur_3plane(q, s, s->d_mul_buf, s->d_s11, scale);
        /* 2) dis² → mul_buf → blur into s22. */
        launch_mul3(q, s->d_dis_xyb, s->d_dis_xyb, s->d_mul_buf, cw, ch, (unsigned)plane_full);
        blur_3plane(q, s, s->d_mul_buf, s->d_s22, scale);
        /* 3) ref·dis → mul_buf → blur into s12. */
        launch_mul3(q, s->d_ref_xyb, s->d_dis_xyb, s->d_mul_buf, cw, ch, (unsigned)plane_full);
        blur_3plane(q, s, s->d_mul_buf, s->d_s12, scale);
        /* 4) blur ref_xyb → mu1. */
        blur_3plane(q, s, s->d_ref_xyb, s->d_mu1, scale);
        /* 5) blur dis_xyb → mu2. */
        blur_3plane(q, s, s->d_dis_xyb, s->d_mu2, scale);

        q.memcpy(s->h_mu1, s->d_mu1, three_plane_bytes);
        q.memcpy(s->h_mu2, s->d_mu2, three_plane_bytes);
        q.memcpy(s->h_s11, s->d_s11, three_plane_bytes);
        q.memcpy(s->h_s22, s->d_s22, three_plane_bytes);
        q.memcpy(s->h_s12, s->d_s12, three_plane_bytes);
        q.wait();

        ss2s_host_combine(s, scale, avg_ssim[scale], avg_ed[scale]);
        completed++;

        if (scale + 1 < SS2S_NUM_SCALES) {
            const unsigned nw = (cw + 1) / 2;
            const unsigned nh = (ch + 1) / 2;
            float *scratch = static_cast<float *>(std::malloc(3u * plane_full * sizeof(float)));
            if (!scratch)
                return -ENOMEM;
            ss2s_downsample_2x2(s->h_ref_lin, cw, ch, scratch, nw, nh, plane_full);
            for (int c = 0; c < 3; c++)
                std::memcpy(s->h_ref_lin + (size_t)c * plane_full, scratch + (size_t)c * plane_full,
                            (size_t)nw * (size_t)nh * sizeof(float));
            ss2s_downsample_2x2(s->h_dis_lin, cw, ch, scratch, nw, nh, plane_full);
            for (int c = 0; c < 3; c++)
                std::memcpy(s->h_dis_lin + (size_t)c * plane_full, scratch + (size_t)c * plane_full,
                            (size_t)nw * (size_t)nh * sizeof(float));
            std::free(scratch);
            cw = nw;
            ch = nh;
        }
    }

    const double score = ss2s_pool_score(avg_ssim, avg_ed, completed);
    return vmaf_feature_collector_append(feature_collector, "ssimulacra2", score, index);
}

int close_fex_sycl(VmafFeatureExtractor *fex)
{
    assert(fex != nullptr);
    auto *s = static_cast<Ssimu2StateSycl *>(fex->priv);
    if (!s || !s->sycl_state)
        return 0;
#define SS2S_FREE(p)                                                                               \
    do {                                                                                           \
        if (s->p) {                                                                                \
            vmaf_sycl_free(s->sycl_state, s->p);                                                   \
            s->p = nullptr;                                                                        \
        }                                                                                          \
    } while (0)
    SS2S_FREE(d_ref_xyb);
    SS2S_FREE(d_dis_xyb);
    SS2S_FREE(d_mul_buf);
    SS2S_FREE(d_blur_scratch);
    SS2S_FREE(d_mu1);
    SS2S_FREE(d_mu2);
    SS2S_FREE(d_s11);
    SS2S_FREE(d_s22);
    SS2S_FREE(d_s12);
    SS2S_FREE(h_ref_lin);
    SS2S_FREE(h_dis_lin);
    SS2S_FREE(h_ref_xyb);
    SS2S_FREE(h_dis_xyb);
    SS2S_FREE(h_mu1);
    SS2S_FREE(h_mu2);
    SS2S_FREE(h_s11);
    SS2S_FREE(h_s22);
    SS2S_FREE(h_s12);
#undef SS2S_FREE
    return 0;
}

const char *provided_features_ssimulacra2_sycl[] = {"ssimulacra2", nullptr};

} /* namespace */

extern "C" VmafFeatureExtractor vmaf_fex_ssimulacra2_sycl = {
    .name = "ssimulacra2_sycl",
    .init = init_fex_sycl,
    .extract = extract_fex_sycl,
    .close = close_fex_sycl,
    .options = options_ssimulacra2_sycl,
    .priv_size = sizeof(Ssimu2StateSycl),
    .flags = VMAF_FEATURE_EXTRACTOR_SYCL,
    .provided_features = provided_features_ssimulacra2_sycl,
};
