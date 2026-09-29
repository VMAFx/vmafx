/**
 *  Copyright (c) the JPEG XL Project Authors.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent AND BSD-3-Clause
 *
 *  ssimulacra2 feature kernel on the SYCL backend (ADR-0206), device-resident
 *  since ADR-1363. SYCL twin of ssimulacra2_cuda / ssimulacra2_hip.
 *
 *  Per frame, one in-order chain with no host round trip:
 *    1. submit(): the six raw Y/U/V planes are packed into pinned staging and
 *       uploaded. That upload is the only host-to-device traffic.
 *    2. YUV -> linear RGB (`vmaf_ss2_srgb_eotf` LUT, single-rounded FMAs in
 *       the ADR-0891 / ADR-1205 order).
 *    3. Per scale (up to 6, stops when a side drops below 8):
 *       a. linear RGB -> XYB (`vmaf_ss2_cbrtf` with correctly rounded
 *          division).
 *       b. the products ref^2, dis^2 and ref*dis, and five separable
 *          3-pole IIR blurs (a lane-per-row horizontal walk, a
 *          lane-per-column vertical walk).
 *       c. the per-pixel SSIM and edge-difference terms of `ssim_map` /
 *          `edge_diff_map`, reduced over each plane to six sums per channel
 *          in a fixed tree of exact fp32 pairs.
 *       d. 2x2 box downsample of the linear-RGB pyramid.
 *    4. One 864-byte readback of the per-scale sums. collect() waits once,
 *       forms the 108 norms in fp64 and pools the score exactly as
 *       ssimulacra2.c does.
 *
 *  Numerical contract (ADR-1363): stages 2, 3a, 3b and 3d reproduce the CPU
 *  extractor bit for bit (contraction off for this TU, correctly rounded
 *  division, products in named temporaries). The CPU evaluates the per-pixel
 *  terms of 3c in fp64 and adds 8.3M of them one after another at 4K; the
 *  device has no fp64 (ADR-0220) and cannot replay that sequence of
 *  roundings in parallel, so it evaluates each term in fp32 pairs (relative
 *  error about 2^-44) and sums in a fixed tree (deterministic, identical on
 *  every device). The pooled score stays within 6.7e-12 of the CPU on the
 *  tested content, against the 5e-3 ADR-0214 tolerance.
 */

#include <sycl/sycl.hpp>

#include <cassert>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <numbers>

#include "config.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "log.h"
#include "picture.h"
#include "sycl/common.h"
#include "sycl_exact_fp.h"

/* Device fp32 `/` is not correctly rounded; the cube root must match the
 * host's (ssimulacra2_math.h, ADR-1363). */
#define VMAF_SS2_FDIV(a, b) vmaf_sycl_exact::div_rn((a), (b))
#include "feature/ssimulacra2_math.h"
#include "feature/ssimulacra2_score.h"

namespace
{

using vmaf_sycl_exact::Ff;
using vmaf_sycl_exact::ff_add;
using vmaf_sycl_exact::ff_div;
using vmaf_sycl_exact::ff_mul;
using vmaf_sycl_exact::ff_neg;
using vmaf_sycl_exact::two_prod;
using vmaf_sycl_exact::two_sum;

constexpr int SS2S_NUM_SCALES = 6;
constexpr unsigned SS2S_CHANNELS = 3;
constexpr unsigned SS2S_IMAGES = 2; /* reference, distorted */
/* Per channel: SSIM L1, SSIM L4, artifact L1, artifact L4, detail L1,
 * detail L4 (the four edge sums of edge_diff_map in its order). */
constexpr unsigned SS2S_SUMS = 6;
constexpr unsigned SS2S_PAIR = 2;                            /* hi, lo */
constexpr unsigned SS2S_LANE_FLOATS = SS2S_SUMS * SS2S_PAIR; /* one work-item's sums */
constexpr size_t SS2S_TOTAL_FLOATS = (size_t)SS2S_NUM_SCALES * SS2S_CHANNELS * SS2S_LANE_FLOATS;
constexpr unsigned SS2S_BLUR_BLOCK = 64;
constexpr size_t SS2S_PIX_BX = 16;
constexpr size_t SS2S_PIX_BY = 8;
constexpr size_t SS2S_ELEM_BLOCK = 256;
constexpr unsigned SS2S_REDUCE_WG = 256;
constexpr unsigned SS2S_MAX_GROUPS = 256;
constexpr unsigned SS2S_PIXELS_PER_ITEM = 16;
constexpr size_t SS2S_LDS_FLOATS = (size_t)SS2S_REDUCE_WG * SS2S_LANE_FLOATS;
constexpr double SS2S_SIGMA = 1.5;
constexpr float SS2S_C2 = 0.0009f; /* ssimulacra2.c kC2 */

/* `yuv_matrix` option values; 1 (bt601_limited) is the default branch of
 * ss2s_yuv_coefficients, as in ssimulacra2.c. */
constexpr int SS2S_MATRIX_BT709_LIMITED = 0;
constexpr int SS2S_MATRIX_BT709_FULL = 2;
constexpr int SS2S_MATRIX_BT601_FULL = 3;

/* Host-computed YUV -> RGB constants, evaluated with the same float
 * expressions as ssimulacra2.c::picture_to_linear_rgb. */
struct Ss2YuvCoefficients {
    float inv_peak;
    float y_off;
    float y_scale;
    float c_off;
    float c_scale;
    float cr_r;
    float cb_g;
    float cr_g;
    float cb_b;
};

struct Ss2Iir {
    float n2[3];
    float d1[3];
    int radius;
};

struct Ssimu2StateSycl {
    int yuv_matrix;

    unsigned width;
    unsigned height;
    unsigned bpc;
    unsigned scale_w[SS2S_NUM_SCALES];
    unsigned scale_h[SS2S_NUM_SCALES];
    int num_scales;
    unsigned plane_w[SS2S_CHANNELS];
    unsigned plane_h[SS2S_CHANNELS];
    size_t row_bytes[SS2S_CHANNELS];

    Ss2Iir iir;
    Ss2YuvCoefficients yuv;

    VmafSyclState *sycl_state;

    /* Raw planes: pinned staging and device copies, [image][plane]. */
    void *h_raw[SS2S_IMAGES][SS2S_CHANNELS];
    void *d_raw[SS2S_IMAGES][SS2S_CHANNELS];
    /* Linear-RGB pyramid, ping-pong per image: [image][0] holds scales 0, 2,
     * 4 (full size), [image][1] scales 1, 3, 5 (quarter size). */
    float *d_lin[SS2S_IMAGES][2];
    float *d_xyb[SS2S_IMAGES];
    float *d_product; /* ref^2, dis^2 or ref*dis for the blur that follows */
    float *d_scratch;
    float *d_mu1;
    float *d_mu2;
    float *d_s11;
    float *d_s22;
    float *d_s12;
    float *d_partials; /* [channel][group][sum][pair] */
    float *d_totals;   /* [scale][channel][sum][pair] */
    float *h_totals;

    bool has_pending;
    unsigned pending_index;
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

} // namespace

/* ------------------------------------------------------------------ */
/* Host setup: recursive Gaussian, YUV constants, geometry             */
/* ------------------------------------------------------------------ */

namespace
{

/* Verbatim port of ssimulacra2.c::create_recursive_gaussian. */
void ss2s_setup_gaussian(Ss2Iir *iir, double sigma)
{
    assert(iir != nullptr);
    assert(sigma > 0.0);
    const double radius = std::round(3.2795 * sigma + 0.2546);
    const double pi_div_2r = std::numbers::pi / (2.0 * radius);
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
        for (int i = 0; i < 3; i++) {
            for (int j = 0; j < 3; j++)
                M[i][j] = A[i][j];
        }
        for (int i = 0; i < 3; i++)
            M[i][col] = gamma[i];
        beta[col] = (M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1]) -
                     M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0]) +
                     M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0])) *
                    inv_det;
    }

    iir->radius = (int)radius;
    for (int i = 0; i < 3; i++) {
        iir->n2[i] = (float)(-beta[i] * std::cos(omega[i] * (radius + 1.0)));
        iir->d1[i] = (float)(-2.0 * std::cos(omega[i]));
    }
}

/* The constants of ssimulacra2.c::picture_to_linear_rgb, same expressions. */
Ss2YuvCoefficients ss2s_yuv_coefficients(int yuv_matrix, unsigned bpc)
{
    assert(bpc >= 8u && bpc <= 16u);
    assert(yuv_matrix >= SS2S_MATRIX_BT709_LIMITED && yuv_matrix <= SS2S_MATRIX_BT601_FULL);
    const float peak = (float)((1u << bpc) - 1u);
    float kr = 0.299f;
    float kg = 0.587f;
    float kb = 0.114f;
    bool limited = true;
    if (yuv_matrix == SS2S_MATRIX_BT709_FULL || yuv_matrix == SS2S_MATRIX_BT709_LIMITED) {
        kr = 0.2126f;
        kg = 0.7152f;
        kb = 0.0722f;
    }
    if (yuv_matrix == SS2S_MATRIX_BT709_FULL || yuv_matrix == SS2S_MATRIX_BT601_FULL)
        limited = false;
    Ss2YuvCoefficients k = {};
    k.inv_peak = 1.0f / peak;
    k.cr_r = 2.0f * (1.0f - kr);
    k.cb_b = 2.0f * (1.0f - kb);
    k.cb_g = -(2.0f * kb * (1.0f - kb)) / kg;
    k.cr_g = -(2.0f * kr * (1.0f - kr)) / kg;
    k.y_scale = limited ? (255.0f / 219.0f) : 1.0f;
    k.c_scale = limited ? (255.0f / 224.0f) : 1.0f;
    k.y_off = limited ? (16.0f / 255.0f) : 0.0f;
    k.c_off = 0.5f;
    return k;
}

/* Plane geometry by picture.c's ceil rule; YUV400 has no chroma to convert. */
int ss2s_configure_planes(Ssimu2StateSycl *s, enum VmafPixelFormat pix_fmt)
{
    if (pix_fmt == VMAF_PIX_FMT_YUV400P || pix_fmt == VMAF_PIX_FMT_UNKNOWN) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "ssimulacra2_sycl: needs a YUV 4:2:0, 4:2:2 or 4:4:4 input\n");
        return -EINVAL;
    }
    const unsigned ss_hor = (pix_fmt != VMAF_PIX_FMT_YUV444P) ? 1u : 0u;
    const unsigned ss_ver = (pix_fmt == VMAF_PIX_FMT_YUV420P) ? 1u : 0u;
    const size_t bytes_per_sample = (s->bpc > 8u) ? 2u : 1u;
    for (unsigned p = 0; p < SS2S_CHANNELS; p++) {
        const unsigned sh = (p == 0u) ? 0u : ss_hor;
        const unsigned sv = (p == 0u) ? 0u : ss_ver;
        s->plane_w[p] = (s->width + sh) >> sh;
        s->plane_h[p] = (s->height + sv) >> sv;
        s->row_bytes[p] = (size_t)s->plane_w[p] * bytes_per_sample;
    }
    return 0;
}

void ss2s_configure_scales(Ssimu2StateSycl *s)
{
    s->scale_w[0] = s->width;
    s->scale_h[0] = s->height;
    for (int i = 1; i < SS2S_NUM_SCALES; i++) {
        s->scale_w[i] = (s->scale_w[i - 1] + 1) / 2;
        s->scale_h[i] = (s->scale_h[i - 1] + 1) / 2;
    }
    /* ssimulacra2.c::extract stops before the first scale below 8x8. */
    s->num_scales = 0;
    while (s->num_scales < SS2S_NUM_SCALES && s->scale_w[s->num_scales] >= 8u &&
           s->scale_h[s->num_scales] >= 8u)
        s->num_scales++;
}

/* Work-groups per channel for one scale's reduction. A function of the
 * plane size only, so the summation tree is the same on every device. */
unsigned ss2s_reduce_groups(size_t pixels)
{
    const size_t per_group = (size_t)SS2S_REDUCE_WG * SS2S_PIXELS_PER_ITEM;
    size_t groups = (pixels + per_group - 1u) / per_group;
    if (groups < 1u)
        groups = 1u;
    if (groups > SS2S_MAX_GROUPS)
        groups = SS2S_MAX_GROUPS;
    return (unsigned)groups;
}

} // namespace

/* ------------------------------------------------------------------ */
/* Device: YUV -> linear RGB, XYB, downsample                          */
/* ------------------------------------------------------------------ */

namespace
{

struct Ss2YuvArgs {
    const void *plane[SS2S_CHANNELS];
    unsigned plane_w[SS2S_CHANNELS];
    unsigned plane_h[SS2S_CHANNELS];
    unsigned width;
    unsigned height;
    unsigned wide; /* 16-bit samples */
    Ss2YuvCoefficients k;
    float *out;
};

/* ssimulacra2.c::read_plane coordinate mapping (nearest neighbour). */
inline unsigned ss2s_map(unsigned v, unsigned plane_dim, unsigned luma_dim)
{
    uint64_t s = v;
    if (plane_dim != luma_dim)
        s = (plane_dim * 2u == luma_dim) ? (uint64_t)(v >> 1) : (uint64_t)v * plane_dim / luma_dim;
    return (s >= plane_dim) ? plane_dim - 1u : (unsigned)s;
}

inline float ss2s_sample(const Ss2YuvArgs &a, unsigned p, unsigned x, unsigned y)
{
    const unsigned sx = ss2s_map(x, a.plane_w[p], a.width);
    const unsigned sy = ss2s_map(y, a.plane_h[p], a.height);
    const size_t idx = (size_t)sy * a.plane_w[p] + sx;
    if (a.wide)
        return (float)static_cast<const uint16_t *>(a.plane[p])[idx];
    return (float)static_cast<const uint8_t *>(a.plane[p])[idx];
}

inline float ss2s_clampf(float v)
{
    if (v < 0.0f)
        return 0.0f;
    if (v > 1.0f)
        return 1.0f;
    return v;
}

/* One pixel of ssimulacra2.c::picture_to_linear_rgb. */
inline void ss2s_yuv_pixel(const Ss2YuvArgs &a, unsigned x, unsigned y)
{
    const Ss2YuvCoefficients &k = a.k;
    const float Y = ss2s_sample(a, 0, x, y) * k.inv_peak;
    const float U = ss2s_sample(a, 1, x, y) * k.inv_peak;
    const float V = ss2s_sample(a, 2, x, y) * k.inv_peak;
    const float Yn = (Y - k.y_off) * k.y_scale;
    const float Un = (U - k.c_off) * k.c_scale;
    const float Vn = (V - k.c_off) * k.c_scale;
    /* Single-rounded FMAs in this order (ADR-0891 / ADR-1205). */
    const float R = sycl::fma(k.cr_r, Vn, Yn);
    const float G0 = sycl::fma(k.cb_g, Un, Yn);
    const float G = sycl::fma(k.cr_g, Vn, G0);
    const float B = sycl::fma(k.cb_b, Un, Yn);
    const size_t plane = (size_t)a.width * a.height;
    const size_t idx = (size_t)y * a.width + x;
    a.out[idx] = vmaf_ss2_srgb_eotf(ss2s_clampf(R));
    a.out[plane + idx] = vmaf_ss2_srgb_eotf(ss2s_clampf(G));
    a.out[2u * plane + idx] = vmaf_ss2_srgb_eotf(ss2s_clampf(B));
}

sycl::nd_range<2> ss2s_pixel_range(unsigned width, unsigned height)
{
    const size_t gx = ((size_t)width + SS2S_PIX_BX - 1u) / SS2S_PIX_BX * SS2S_PIX_BX;
    const size_t gy = ((size_t)height + SS2S_PIX_BY - 1u) / SS2S_PIX_BY * SS2S_PIX_BY;
    return {sycl::range<2>(gy, gx), sycl::range<2>(SS2S_PIX_BY, SS2S_PIX_BX)};
}

void launch_yuv_to_linear(sycl::queue &q, const Ss2YuvArgs &args)
{
    q.parallel_for(ss2s_pixel_range(args.width, args.height), [=](sycl::nd_item<2> it) {
        const auto x = (unsigned)it.get_global_id(1);
        const auto y = (unsigned)it.get_global_id(0);
        if (x < args.width && y < args.height)
            ss2s_yuv_pixel(args, x, y);
    });
}

struct Ss2PlanesArgs {
    const float *in[SS2S_IMAGES];
    float *out[SS2S_IMAGES];
    unsigned width; /* input scale */
    unsigned height;
    unsigned out_width; /* downsample only */
    unsigned out_height;
};

/* One pixel of ssimulacra2.c::linear_rgb_to_xyb (MakePositiveXYB folded in). */
inline void ss2s_xyb_pixel(const float *lin, float *xyb, size_t i, size_t plane)
{
    constexpr float kM00 = 0.30f;
    constexpr float kM02 = 0.078f;
    constexpr float kM10 = 0.23f;
    constexpr float kM12 = 0.078f;
    constexpr float kM20 = 0.24342268924547819f;
    constexpr float kM21 = 0.20476744424496821f;
    constexpr float kOpsinBias = 0.0037930732552754493f;
    constexpr float m01 = 1.0f - kM00 - kM02;
    constexpr float m11 = 1.0f - kM10 - kM12;
    constexpr float m22 = 1.0f - kM20 - kM21;
    const float cbrt_bias = vmaf_ss2_cbrtf(kOpsinBias);

    const float r = lin[i];
    const float g = lin[plane + i];
    const float b = lin[2u * plane + i];
    const float l_r = kM00 * r;
    const float l_g = m01 * g;
    const float l_b = kM02 * b;
    const float m_r = kM10 * r;
    const float m_g = m11 * g;
    const float m_b = kM12 * b;
    const float s_r = kM20 * r;
    const float s_g = kM21 * g;
    const float s_b = m22 * b;
    const float l = ((l_r + l_g) + l_b) + kOpsinBias;
    const float m = ((m_r + m_g) + m_b) + kOpsinBias;
    const float s = ((s_r + s_g) + s_b) + kOpsinBias;
    /* `if (l < 0.0f) l = 0.0f;` of the reference, NaN kept (not fmax). */
    const float L = vmaf_ss2_cbrtf((l < 0.0f) ? 0.0f : l) - cbrt_bias;
    const float M = vmaf_ss2_cbrtf((m < 0.0f) ? 0.0f : m) - cbrt_bias;
    const float S = vmaf_ss2_cbrtf((s < 0.0f) ? 0.0f : s) - cbrt_bias;
    const float X = 0.5f * (L - M);
    const float Yv = 0.5f * (L + M);
    const float X14 = X * 14.0f;
    xyb[i] = X14 + 0.42f;
    xyb[plane + i] = Yv + 0.01f;
    xyb[2u * plane + i] = (S - Yv) + 0.55f; /* B uses Y before its offset */
}

void launch_xyb(sycl::queue &q, const Ss2PlanesArgs &args)
{
    const size_t plane = (size_t)args.width * args.height;
    const size_t global = (plane + SS2S_ELEM_BLOCK - 1u) / SS2S_ELEM_BLOCK * SS2S_ELEM_BLOCK;
    q.parallel_for(sycl::nd_range<1>(global, SS2S_ELEM_BLOCK), [=](sycl::nd_item<1> it) {
        const size_t i = it.get_global_id(0);
        if (i >= plane)
            return;
        for (unsigned img = 0; img < SS2S_IMAGES; img++)
            ss2s_xyb_pixel(args.in[img], args.out[img], i, plane);
    });
}

/* One output pixel of ssimulacra2.c::downsample_2x2, all three planes. */
inline void ss2s_down_pixel(const float *in, float *out, const Ss2PlanesArgs &a, unsigned ox,
                            unsigned oy)
{
    const size_t in_plane = (size_t)a.width * a.height;
    const size_t out_plane = (size_t)a.out_width * a.out_height;
    for (unsigned c = 0; c < SS2S_CHANNELS; c++) {
        const float *ip = in + c * in_plane;
        float sum = 0.0f;
        for (unsigned dy = 0; dy < 2u; dy++) {
            for (unsigned dx = 0; dx < 2u; dx++) {
                const unsigned ix = (sycl::min)(ox * 2u + dx, a.width - 1u);
                const unsigned iy = (sycl::min)(oy * 2u + dy, a.height - 1u);
                sum += ip[(size_t)iy * a.width + ix];
            }
        }
        out[c * out_plane + (size_t)oy * a.out_width + ox] = sum * 0.25f;
    }
}

void launch_downsample(sycl::queue &q, const Ss2PlanesArgs &args)
{
    q.parallel_for(ss2s_pixel_range(args.out_width, args.out_height), [=](sycl::nd_item<2> it) {
        const auto ox = (unsigned)it.get_global_id(1);
        const auto oy = (unsigned)it.get_global_id(0);
        if (ox >= args.out_width || oy >= args.out_height)
            return;
        for (unsigned img = 0; img < SS2S_IMAGES; img++)
            ss2s_down_pixel(args.in[img], args.out[img], args, ox, oy);
    });
}

} // namespace

/* ------------------------------------------------------------------ */
/* Device: separable recursive Gaussian                                */
/* ------------------------------------------------------------------ */

namespace
{

struct Ss2BlurArgs {
    const float *in;
    float *out;
    unsigned width;
    unsigned height;
    Ss2Iir iir;
};

struct Ss2IirState {
    float prev1[3];
    float prev2[3];
};

/* One step of ssimulacra2.c::fast_gaussian_1d; returns o0 + o1 + o2. The IIR
 * is a pure recurrence: no running accumulator to compensate (ADR-0985). */
inline float ss2s_iir_step(float sum, Ss2IirState &st, const Ss2Iir &k)
{
    float o[3];
    for (int p = 0; p < 3; p++) {
        const float ns = k.n2[p] * sum;
        const float dp = k.d1[p] * st.prev1[p];
        const float t = ns - dp;
        o[p] = t - st.prev2[p];
        st.prev2[p] = st.prev1[p];
        st.prev1[p] = o[p];
    }
    const float s01 = o[0] + o[1];
    return s01 + o[2];
}

/* Elementwise product of two three-plane images (ssimulacra2.c::
 * multiply_3plane) as its own coalesced pass. Forming the product inside the
 * horizontal pass instead doubles that pass's per-lane loads, which measured
 * 2.3x slower on the UHD 770 (ADR-1363). */
void launch_mul3(sycl::queue &q, const float *a, const float *b, float *out, size_t count)
{
    const size_t global = (count + SS2S_ELEM_BLOCK - 1u) / SS2S_ELEM_BLOCK * SS2S_ELEM_BLOCK;
    q.parallel_for(sycl::nd_range<1>(global, SS2S_ELEM_BLOCK), [=](sycl::nd_item<1> it) {
        const size_t i = it.get_global_id(0);
        if (i < count) {
            const float pa = a[i];
            const float pb = b[i];
            out[i] = pa * pb;
        }
    });
}

/* ssimulacra2.c::fast_gaussian_1d along one row (step 1) or one column
 * (step = width) that starts at `base`. */
inline void ss2s_iir_line(const Ss2BlurArgs &a, size_t base, size_t step, int xsize)
{
    const int radius = a.iir.radius;
    Ss2IirState st = {};
    for (int n = -radius + 1; n < xsize; ++n) {
        const int left = n - radius - 1;
        const int right = n + radius - 1;
        const float lv = (left >= 0) ? a.in[base + (size_t)left * step] : 0.f;
        const float rv = (right < xsize) ? a.in[base + (size_t)right * step] : 0.f;
        const float s = ss2s_iir_step(lv + rv, st, a.iir);
        if (n >= 0)
            a.out[base + (size_t)n * step] = s;
    }
}

/* PASS 0: one work-item per row, PASS 1: one per column; all three planes of
 * the current scale in one launch. Staging the rows through local memory for
 * contiguous loads measured 2x (B580) to 5.6x (UHD 770) slower than this walk
 * (ADR-1363). */
template <int PASS> void launch_blur_pass(sycl::queue &q, const Ss2BlurArgs &args)
{
    const unsigned per_plane = (PASS == 0) ? args.height : args.width;
    const unsigned lines = per_plane * SS2S_CHANNELS;
    const size_t global =
        ((size_t)lines + SS2S_BLUR_BLOCK - 1u) / SS2S_BLUR_BLOCK * SS2S_BLUR_BLOCK;
    q.parallel_for(sycl::nd_range<1>(global, SS2S_BLUR_BLOCK), [=](sycl::nd_item<1> it) {
        const auto line = (unsigned)it.get_global_id(0);
        if (line >= lines)
            return;
        const unsigned c = line / per_plane;
        const unsigned l = line - c * per_plane;
        const size_t plane = (size_t)c * args.width * args.height;
        const size_t base = plane + ((PASS == 0) ? (size_t)l * args.width : (size_t)l);
        const size_t step = (PASS == 0) ? 1u : (size_t)args.width;
        const int xsize = (PASS == 0) ? (int)args.width : (int)args.height;
        ss2s_iir_line(args, base, step, xsize);
    });
}

/* blur(in) -> out through the scratch buffer (rows into scratch, columns out). */
void ss2s_blur(sycl::queue &q, const Ssimu2StateSycl *s, const float *in, float *out, int scale)
{
    const Ss2BlurArgs rows = {.in = in,
                              .out = s->d_scratch,
                              .width = s->scale_w[scale],
                              .height = s->scale_h[scale],
                              .iir = s->iir};
    launch_blur_pass<0>(q, rows);
    Ss2BlurArgs columns = rows;
    columns.in = s->d_scratch;
    columns.out = out;
    launch_blur_pass<1>(q, columns);
}

} // namespace

/* ------------------------------------------------------------------ */
/* Device: SSIM and edge-difference sums                               */
/* ------------------------------------------------------------------ */

namespace
{

constexpr Ff kFfZero = {.hi = 0.0f, .lo = 0.0f};
constexpr Ff kFfOne = {.hi = 1.0f, .lo = 0.0f};
constexpr Ff kFfMinusOne = {.hi = -1.0f, .lo = 0.0f};

struct Ss2Sums {
    Ff v[SS2S_SUMS];
};

/* ssimulacra2.c::ssim_map per pixel: the fp32 part verbatim, then
 * d = 1.0 - (double)num_m * (double)num_s / (double)denom_s in fp32 pairs
 * (the product of two floats is exact as a pair), clamped at zero. */
inline Ff ss2s_ssim_term(float u1, float u2, float s11, float s22, float s12)
{
    const float u11 = u1 * u1;
    const float u22 = u2 * u2;
    const float u12 = u1 * u2;
    const float diff = u1 - u2;
    const float diff_sq = diff * diff;
    const float num_m = 1.0f - diff_sq;
    const float cov = s12 - u12;
    const float cov2 = 2.0f * cov;
    const float num_s = cov2 + SS2S_C2;
    const float var1 = s11 - u11;
    const float var2 = s22 - u22;
    const float var_sum = var1 + var2;
    const float denom_s = var_sum + SS2S_C2;
    const Ff ratio = ff_div(two_prod(num_m, num_s), Ff{.hi = denom_s, .lo = 0.0f});
    const Ff d = ff_add(kFfOne, ff_neg(ratio));
    return (d.hi < 0.0f) ? kFfZero : d; /* NaN is kept, as on the host */
}

/* |(double)a - (double)b| exactly. */
inline Ff ss2s_abs_diff(float a, float b)
{
    const Ff diff = two_sum(a, -b);
    return (diff.hi < 0.0f) ? ff_neg(diff) : diff;
}

/* ssimulacra2.c::edge_diff_map per pixel: (1 + ed2) / (1 + ed1) - 1 and
 * vmaf_ss2_split_edge_difference (a non-finite value goes to both sums). */
inline void ss2s_edge_terms(float r1, float m1, float r2, float m2, Ff &artifact, Ff &detail)
{
    const Ff num = ff_add(kFfOne, ss2s_abs_diff(r2, m2));
    const Ff den = ff_add(kFfOne, ss2s_abs_diff(r1, m1));
    const Ff dd = ff_add(ff_div(num, den), kFfMinusOne);
    if (!sycl::isfinite(dd.hi)) {
        artifact = dd;
        detail = dd;
        return;
    }
    artifact = (dd.hi > 0.0f) ? dd : kFfZero;
    detail = (dd.hi < 0.0f) ? ff_neg(dd) : kFfZero;
}

inline Ff ss2s_quartic(Ff x)
{
    const Ff sq = ff_mul(x, x);
    return ff_mul(sq, sq);
}

struct Ss2CombineArgs {
    const float *mu1;
    const float *mu2;
    const float *s11;
    const float *s22;
    const float *s12;
    const float *img1;
    const float *img2;
    float *partials;
    size_t plane;
    unsigned groups;
};

inline void ss2s_accumulate_pixel(Ss2Sums &acc, const Ss2CombineArgs &a, size_t idx)
{
    const float u1 = a.mu1[idx];
    const float u2 = a.mu2[idx];
    const Ff d = ss2s_ssim_term(u1, u2, a.s11[idx], a.s22[idx], a.s12[idx]);
    Ff artifact = kFfZero;
    Ff detail = kFfZero;
    ss2s_edge_terms(a.img1[idx], u1, a.img2[idx], u2, artifact, detail);
    acc.v[0] = ff_add(acc.v[0], d);
    acc.v[1] = ff_add(acc.v[1], ss2s_quartic(d));
    acc.v[2] = ff_add(acc.v[2], artifact);
    acc.v[3] = ff_add(acc.v[3], ss2s_quartic(artifact));
    acc.v[4] = ff_add(acc.v[4], detail);
    acc.v[5] = ff_add(acc.v[5], ss2s_quartic(detail));
}

/* Fixed-shape tree over one work-group's sums in local memory; lane 0 ends
 * up with the group total. */
template <int D>
inline void ss2s_group_tree(sycl::nd_item<D> it, float *lds, unsigned lane, const Ss2Sums &own)
{
    for (unsigned s = 0; s < SS2S_SUMS; s++) {
        const size_t at = (size_t)lane * SS2S_LANE_FLOATS + (size_t)s * SS2S_PAIR;
        lds[at] = own.v[s].hi;
        lds[at + 1u] = own.v[s].lo;
    }
    sycl::group_barrier(it.get_group());
    for (unsigned stride = SS2S_REDUCE_WG / 2u; stride > 0u; stride >>= 1u) {
        if (lane < stride) {
            float *mine = lds + (size_t)lane * SS2S_LANE_FLOATS;
            const float *other = lds + (size_t)(lane + stride) * SS2S_LANE_FLOATS;
            for (unsigned s = 0; s < SS2S_SUMS; s++) {
                const size_t at = (size_t)s * SS2S_PAIR;
                const Ff merged = ff_add({.hi = mine[at], .lo = mine[at + 1u]},
                                         {.hi = other[at], .lo = other[at + 1u]});
                mine[at] = merged.hi;
                mine[at + 1u] = merged.lo;
            }
        }
        sycl::group_barrier(it.get_group());
    }
}

/* Stage 1: every work-group sums a fixed strided subset of one channel. */
void launch_combine_partials(sycl::queue &q, const Ss2CombineArgs &args)
{
    /* The partials buffer holds SS2S_MAX_GROUPS groups per channel. */
    assert(args.groups >= 1u && args.groups <= SS2S_MAX_GROUPS);
    assert(args.plane > 0u);
    const sycl::nd_range<2> range(
        sycl::range<2>(SS2S_CHANNELS, (size_t)args.groups * SS2S_REDUCE_WG),
        sycl::range<2>(1, SS2S_REDUCE_WG));
    q.submit([&](sycl::handler &h) {
        const sycl::local_accessor<float, 1> lds(sycl::range<1>(SS2S_LDS_FLOATS), h);
        h.parallel_for(range, [=](sycl::nd_item<2> it) {
            const auto c = (unsigned)it.get_global_id(0);
            const auto group = (unsigned)it.get_group(1);
            const auto lane = (unsigned)it.get_local_id(1);
            const size_t stride = (size_t)args.groups * SS2S_REDUCE_WG;
            const size_t offset = (size_t)c * args.plane;
            Ss2Sums acc = {};
            for (size_t i = (size_t)group * SS2S_REDUCE_WG + lane; i < args.plane; i += stride)
                ss2s_accumulate_pixel(acc, args, offset + i);
            float *local = lds.get_multi_ptr<sycl::access::decorated::no>().get();
            ss2s_group_tree(it, local, lane, acc);
            if (lane == 0u) {
                float *dst =
                    args.partials + ((size_t)c * SS2S_MAX_GROUPS + group) * SS2S_LANE_FLOATS;
                for (unsigned f = 0; f < SS2S_LANE_FLOATS; f++)
                    dst[f] = local[f];
            }
        });
    });
}

struct Ss2FinalArgs {
    const float *partials;
    float *totals; /* this scale's [channel][sum][pair] */
    unsigned groups;
};

/* Stage 2: one work-group per channel folds the group partials. */
/* One channel's group: each lane folds a fixed subset of the group partials,
 * then the local tree; lane 0 writes the channel's six sums. */
inline void ss2s_final_group(sycl::nd_item<1> it, const Ss2FinalArgs &args, float *local)
{
    const auto c = (unsigned)it.get_group(0);
    const auto lane = (unsigned)it.get_local_id(0);
    Ss2Sums acc = {};
    for (unsigned g = lane; g < args.groups; g += SS2S_REDUCE_WG) {
        const float *src = args.partials + ((size_t)c * SS2S_MAX_GROUPS + g) * SS2S_LANE_FLOATS;
        for (unsigned s = 0; s < SS2S_SUMS; s++) {
            const size_t at = (size_t)s * SS2S_PAIR;
            acc.v[s] = ff_add(acc.v[s], {.hi = src[at], .lo = src[at + 1u]});
        }
    }
    ss2s_group_tree(it, local, lane, acc);
    if (lane == 0u) {
        float *dst = args.totals + (size_t)c * SS2S_LANE_FLOATS;
        for (unsigned f = 0; f < SS2S_LANE_FLOATS; f++)
            dst[f] = local[f];
    }
}

void launch_combine_final(sycl::queue &q, const Ss2FinalArgs &args)
{
    assert(args.groups >= 1u && args.groups <= SS2S_MAX_GROUPS);
    const sycl::nd_range<1> range((size_t)SS2S_CHANNELS * SS2S_REDUCE_WG, SS2S_REDUCE_WG);
    q.submit([&](sycl::handler &h) {
        const sycl::local_accessor<float, 1> lds(sycl::range<1>(SS2S_LDS_FLOATS), h);
        h.parallel_for(range, [=](sycl::nd_item<1> it) {
            ss2s_final_group(it, args, lds.get_multi_ptr<sycl::access::decorated::no>().get());
        });
    });
}

} // namespace

/* ------------------------------------------------------------------ */
/* Frame chain                                                          */
/* ------------------------------------------------------------------ */

namespace
{

void enqueue_linear_rgb(sycl::queue &q, const Ssimu2StateSycl *s)
{
    for (unsigned img = 0; img < SS2S_IMAGES; img++) {
        Ss2YuvArgs args = {};
        for (unsigned p = 0; p < SS2S_CHANNELS; p++) {
            args.plane[p] = s->d_raw[img][p];
            args.plane_w[p] = s->plane_w[p];
            args.plane_h[p] = s->plane_h[p];
        }
        args.width = s->width;
        args.height = s->height;
        args.wide = (s->bpc > 8u) ? 1u : 0u;
        args.k = s->yuv;
        args.out = s->d_lin[img][0];
        launch_yuv_to_linear(q, args);
    }
}

void enqueue_scale(sycl::queue &q, const Ssimu2StateSycl *s, int scale)
{
    /* Scales below 8x8 are never enqueued (ssimulacra2.c::extract). */
    assert(scale >= 0 && scale < s->num_scales);
    assert(s->scale_w[scale] >= 8u && s->scale_h[scale] >= 8u);
    const unsigned cw = s->scale_w[scale];
    const unsigned ch = s->scale_h[scale];
    const unsigned cur = (unsigned)scale & 1u;
    const Ss2PlanesArgs xyb = {.in = {s->d_lin[0][cur], s->d_lin[1][cur]},
                               .out = {s->d_xyb[0], s->d_xyb[1]},
                               .width = cw,
                               .height = ch,
                               .out_width = cw,
                               .out_height = ch};
    launch_xyb(q, xyb);

    const float *ref = s->d_xyb[0];
    const float *dis = s->d_xyb[1];
    const size_t plane = (size_t)cw * ch;
    const size_t planes3 = SS2S_CHANNELS * plane;
    /* ssimulacra2.c::extract order: s11, s22, s12, mu1, mu2. */
    launch_mul3(q, ref, ref, s->d_product, planes3);
    ss2s_blur(q, s, s->d_product, s->d_s11, scale);
    launch_mul3(q, dis, dis, s->d_product, planes3);
    ss2s_blur(q, s, s->d_product, s->d_s22, scale);
    launch_mul3(q, ref, dis, s->d_product, planes3);
    ss2s_blur(q, s, s->d_product, s->d_s12, scale);
    ss2s_blur(q, s, ref, s->d_mu1, scale);
    ss2s_blur(q, s, dis, s->d_mu2, scale);

    const Ss2CombineArgs combine = {.mu1 = s->d_mu1,
                                    .mu2 = s->d_mu2,
                                    .s11 = s->d_s11,
                                    .s22 = s->d_s22,
                                    .s12 = s->d_s12,
                                    .img1 = ref,
                                    .img2 = dis,
                                    .partials = s->d_partials,
                                    .plane = plane,
                                    .groups = ss2s_reduce_groups(plane)};
    launch_combine_partials(q, combine);
    launch_combine_final(q,
                         {.partials = s->d_partials,
                          .totals = s->d_totals + (size_t)scale * SS2S_CHANNELS * SS2S_LANE_FLOATS,
                          .groups = combine.groups});

    if (scale + 1 < s->num_scales) {
        const Ss2PlanesArgs down = {.in = {s->d_lin[0][cur], s->d_lin[1][cur]},
                                    .out = {s->d_lin[0][cur ^ 1u], s->d_lin[1][cur ^ 1u]},
                                    .width = cw,
                                    .height = ch,
                                    .out_width = s->scale_w[scale + 1],
                                    .out_height = s->scale_h[scale + 1]};
        launch_downsample(q, down);
    }
}

void enqueue_frame(sycl::queue &q, const Ssimu2StateSycl *s)
{
    for (unsigned img = 0; img < SS2S_IMAGES; img++) {
        for (unsigned p = 0; p < SS2S_CHANNELS; p++)
            q.memcpy(s->d_raw[img][p], s->h_raw[img][p], s->row_bytes[p] * s->plane_h[p]);
    }
    enqueue_linear_rgb(q, s);
    for (int scale = 0; scale < s->num_scales; scale++)
        enqueue_scale(q, s, scale);
    q.memcpy(s->h_totals, s->d_totals, SS2S_TOTAL_FLOATS * sizeof(float));
}

} // namespace

/* ------------------------------------------------------------------ */
/* Host: norms and pooling (fp64, outside every kernel)                */
/* ------------------------------------------------------------------ */

namespace
{

/* The six sums of one scale -> ssim_map / edge_diff_map plane averages. */
void ss2s_scale_norms(const float *totals, unsigned cw, unsigned ch, double avg_ssim[6],
                      double avg_ed[12])
{
    const double one_per_pixels = 1.0 / (double)((size_t)cw * (size_t)ch);
    for (unsigned c = 0; c < SS2S_CHANNELS; c++) {
        double sum[SS2S_SUMS];
        for (unsigned k = 0; k < SS2S_SUMS; k++) {
            const float *pair = totals + (size_t)c * SS2S_LANE_FLOATS + (size_t)k * SS2S_PAIR;
            sum[k] = (double)pair[0] + (double)pair[1];
        }
        avg_ssim[c * 2 + 0] = one_per_pixels * sum[0];
        avg_ssim[c * 2 + 1] = std::sqrt(std::sqrt(one_per_pixels * sum[1]));
        avg_ed[c * 4 + 0] = one_per_pixels * sum[2];
        avg_ed[c * 4 + 1] = std::sqrt(std::sqrt(one_per_pixels * sum[3]));
        avg_ed[c * 4 + 2] = one_per_pixels * sum[4];
        avg_ed[c * 4 + 3] = std::sqrt(std::sqrt(one_per_pixels * sum[5]));
    }
}

/* Verbatim port of ssimulacra2.c::pool_score. */
double ss2s_pool_score(const double avg_ssim[6][6], const double avg_ed[6][12], int num_scales)
{
    assert(num_scales >= 1 && num_scales <= 6);
    double ssim = 0.0;
    size_t i = 0;
    for (int c = 0; c < 3; c++) {
        for (int scale = 0; scale < 6; scale++) {
            for (int n = 0; n < 2; n++) {
                double const s_term = scale < num_scales ? avg_ssim[scale][c * 2 + n] : 0.0;
                double const r_term = scale < num_scales ? avg_ed[scale][c * 4 + n] : 0.0;
                double const b_term = scale < num_scales ? avg_ed[scale][c * 4 + n + 2] : 0.0;
                ssim += g_weights[i++] * std::fabs(s_term);
                ssim += g_weights[i++] * std::fabs(r_term);
                ssim += g_weights[i++] * std::fabs(b_term);
            }
        }
    }
    ssim *= 0.9562382616834844;
    ssim = 2.326765642916932 * ssim - 0.020884521182843837 * ssim * ssim +
           6.248496625763138e-05 * ssim * ssim * ssim;
    return vmaf_ss2_finalize_score(ssim);
}

double ss2s_frame_score(const Ssimu2StateSycl *s)
{
    double avg_ssim[6][6] = {{0}};
    double avg_ed[6][12] = {{0}};
    for (int scale = 0; scale < s->num_scales; scale++) {
        ss2s_scale_norms(s->h_totals + (size_t)scale * SS2S_CHANNELS * SS2S_LANE_FLOATS,
                         s->scale_w[scale], s->scale_h[scale], avg_ssim[scale], avg_ed[scale]);
    }
    return ss2s_pool_score(avg_ssim, avg_ed, s->num_scales);
}

} // namespace

/* ------------------------------------------------------------------ */
/* Lifecycle                                                            */
/* ------------------------------------------------------------------ */

namespace
{

const VmafOption options_ssimulacra2_sycl[] = {
    {.name = "yuv_matrix",
     .help = "YUV→RGB matrix: 0=bt709_limited (default), 1=bt601_limited, "
             "2=bt709_full, 3=bt601_full",
     .offset = offsetof(Ssimu2StateSycl, yuv_matrix),
     .type = VMAF_OPT_TYPE_INT,
     .default_val = {.i = SS2S_MATRIX_BT709_LIMITED},
     .min = 0,
     .max = 3},
    {.name = nullptr},
};

template <typename T> T *ss2s_device_alloc(VmafSyclState *state, size_t bytes)
{
    return static_cast<T *>(vmaf_sycl_malloc_device(state, bytes));
}

bool ss2s_allocate(Ssimu2StateSycl *s)
{
    assert(s->sycl_state != nullptr);
    /* The ping-pong pyramid's second buffer holds scale 1 and smaller. */
    assert(s->scale_w[1] <= s->width && s->scale_h[1] <= s->height);
    VmafSyclState *st = s->sycl_state;
    const size_t full = SS2S_CHANNELS * (size_t)s->width * s->height * sizeof(float);
    const size_t half = SS2S_CHANNELS * (size_t)s->scale_w[1] * s->scale_h[1] * sizeof(float);
    bool ok = true;
    for (unsigned img = 0; img < SS2S_IMAGES; img++) {
        for (unsigned p = 0; p < SS2S_CHANNELS; p++) {
            const size_t bytes = s->row_bytes[p] * s->plane_h[p];
            s->h_raw[img][p] = vmaf_sycl_malloc_host(st, bytes);
            s->d_raw[img][p] = vmaf_sycl_malloc_device(st, bytes);
            ok = ok && s->h_raw[img][p] && s->d_raw[img][p];
        }
        s->d_lin[img][0] = ss2s_device_alloc<float>(st, full);
        s->d_lin[img][1] = ss2s_device_alloc<float>(st, half);
        s->d_xyb[img] = ss2s_device_alloc<float>(st, full);
        ok = ok && s->d_lin[img][0] && s->d_lin[img][1] && s->d_xyb[img];
    }
    float **const planes[] = {&s->d_product, &s->d_scratch, &s->d_mu1, &s->d_mu2,
                              &s->d_s11,     &s->d_s22,     &s->d_s12};
    for (float **slot : planes) {
        *slot = ss2s_device_alloc<float>(st, full);
        ok = ok && *slot;
    }
    const size_t partial_bytes =
        (size_t)SS2S_CHANNELS * SS2S_MAX_GROUPS * SS2S_LANE_FLOATS * sizeof(float);
    s->d_partials = ss2s_device_alloc<float>(st, partial_bytes);
    s->d_totals = ss2s_device_alloc<float>(st, SS2S_TOTAL_FLOATS * sizeof(float));
    s->h_totals =
        static_cast<float *>(vmaf_sycl_malloc_host(st, SS2S_TOTAL_FLOATS * sizeof(float)));
    return ok && s->d_partials && s->d_totals && s->h_totals;
}

int close_fex_sycl(VmafFeatureExtractor *fex);

int init_fex_sycl(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc, unsigned w,
                  unsigned h)
{
    auto *s = static_cast<Ssimu2StateSycl *>(fex->priv);
    if (w < 8u || h < 8u) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "ssimulacra2_sycl: input %ux%u below 8x8 lower bound\n", w,
                 h);
        return -EINVAL;
    }
    if (!fex->sycl_state)
        return -EINVAL;
    s->width = w;
    s->height = h;
    s->bpc = bpc;
    const int plane_err = ss2s_configure_planes(s, pix_fmt);
    if (plane_err)
        return plane_err;
    ss2s_configure_scales(s);
    ss2s_setup_gaussian(&s->iir, SS2S_SIGMA);
    s->yuv = ss2s_yuv_coefficients(s->yuv_matrix, bpc);
    s->sycl_state = fex->sycl_state;
    if (!ss2s_allocate(s)) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "ssimulacra2_sycl: USM allocation failed\n");
        (void)close_fex_sycl(fex);
        return -ENOMEM;
    }
    s->has_pending = false;
    return 0;
}

/* ADR-1324 / ADR-1359: the inputs init rejects (no chroma planes, a side
 * below 8) go to the CPU extractor when the twin was picked for a model or a
 * `--backend sycl --feature ssimulacra2` request. */
int check_context_sycl(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                       unsigned w, unsigned h)
{
    (void)fex;
    (void)bpc;
    const bool chroma = pix_fmt != VMAF_PIX_FMT_YUV400P && pix_fmt != VMAF_PIX_FMT_UNKNOWN;
    return (chroma && w >= 8u && h >= 8u) ? 0 : -ENOTSUP;
}

bool ss2s_picture_matches(const Ssimu2StateSycl *s, const VmafPicture *pic)
{
    if (!pic || pic->bpc != s->bpc)
        return false;
    for (unsigned p = 0; p < SS2S_CHANNELS; p++) {
        if (!pic->data[p] || pic->w[p] != s->plane_w[p] || pic->h[p] != s->plane_h[p])
            return false;
    }
    return true;
}

/* Pack one plane's rows into pinned staging (the picture is released when
 * submit returns, so the upload cannot read it directly). */
void ss2s_stage_plane(const VmafPicture *pic, unsigned p, void *dst, size_t row_bytes,
                      unsigned rows)
{
    const auto *src = static_cast<const uint8_t *>(pic->data[p]);
    auto *out = static_cast<uint8_t *>(dst);
    const auto stride = static_cast<size_t>(pic->stride[p]);
    if (stride == row_bytes) {
        std::memcpy(out, src, row_bytes * rows);
        return;
    }
    for (unsigned i = 0; i < rows; i++)
        std::memcpy(out + (size_t)i * row_bytes, src + (size_t)i * stride, row_bytes);
}

int submit_fex_sycl(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                    VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic_90;
    (void)dist_pic_90;
    auto *s = static_cast<Ssimu2StateSycl *>(fex->priv);
    auto *qptr = static_cast<sycl::queue *>(vmaf_sycl_get_queue_ptr(s->sycl_state));
    if (!qptr)
        return -EINVAL;
    if (!ss2s_picture_matches(s, ref_pic) || !ss2s_picture_matches(s, dist_pic))
        return -EINVAL;
    const VmafPicture *const pics[SS2S_IMAGES] = {ref_pic, dist_pic};
    for (unsigned img = 0; img < SS2S_IMAGES; img++) {
        for (unsigned p = 0; p < SS2S_CHANNELS; p++)
            ss2s_stage_plane(pics[img], p, s->h_raw[img][p], s->row_bytes[p], s->plane_h[p]);
    }
    try {
        enqueue_frame(*qptr, s);
    } catch (const sycl::exception &e) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "ssimulacra2_sycl: submit failed: %s\n", e.what());
        return -EIO;
    }
    s->pending_index = index;
    s->has_pending = true;
    return 0;
}

int collect_fex_sycl(VmafFeatureExtractor *fex, unsigned index,
                     VmafFeatureCollector *feature_collector)
{
    auto *s = static_cast<Ssimu2StateSycl *>(fex->priv);
    auto *qptr = static_cast<sycl::queue *>(vmaf_sycl_get_queue_ptr(s->sycl_state));
    if (!qptr)
        return -EINVAL;
    /* The one host wait per frame: h_totals is written by the last copy of
     * the chain submit() enqueued. */
    try {
        qptr->wait();
    } catch (const sycl::exception &e) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "ssimulacra2_sycl: device chain failed: %s\n", e.what());
        return -EIO;
    }
    s->has_pending = false;
    const double score = ss2s_frame_score(s);
    if (!vmaf_ss2_score_is_finite(score)) {
        vmaf_log(VMAF_LOG_LEVEL_WARNING,
                 "ssimulacra2_sycl: non-finite score at frame %u (score=%g), failing frame\n",
                 index, score);
        return -EINVAL;
    }
    return vmaf_feature_collector_append(feature_collector, "ssimulacra2", score, index);
}

void ss2s_free(VmafSyclState *state, void *&pointer)
{
    if (pointer) {
        vmaf_sycl_free(state, pointer);
        pointer = nullptr;
    }
}

int close_fex_sycl(VmafFeatureExtractor *fex)
{
    assert(fex != nullptr);
    auto *s = static_cast<Ssimu2StateSycl *>(fex->priv);
    if (!s || !s->sycl_state)
        return 0;
    VmafSyclState *st = s->sycl_state;
    for (unsigned img = 0; img < SS2S_IMAGES; img++) {
        for (unsigned p = 0; p < SS2S_CHANNELS; p++) {
            ss2s_free(st, s->h_raw[img][p]);
            ss2s_free(st, s->d_raw[img][p]);
        }
    }
    float **const buffers[] = {&s->d_lin[0][0], &s->d_lin[0][1], &s->d_lin[1][0], &s->d_lin[1][1],
                               &s->d_xyb[0],    &s->d_xyb[1],    &s->d_product,   &s->d_scratch,
                               &s->d_mu1,       &s->d_mu2,       &s->d_s11,       &s->d_s22,
                               &s->d_s12,       &s->d_partials,  &s->d_totals,    &s->h_totals};
    for (float **slot : buffers) {
        if (*slot) {
            vmaf_sycl_free(st, *slot);
            *slot = nullptr;
        }
    }
    return 0;
}

const char *provided_features_ssimulacra2_sycl[] = {"ssimulacra2", nullptr};

} // namespace

extern "C" VmafFeatureExtractor vmaf_fex_ssimulacra2_sycl = {
    .name = "ssimulacra2_sycl",
    .init = init_fex_sycl,
    .extract = nullptr,
    .flush = nullptr,
    .close = close_fex_sycl,
    .submit = submit_fex_sycl,
    .collect = collect_fex_sycl,
    .options = options_ssimulacra2_sycl,
    .priv_size = sizeof(Ssimu2StateSycl),
    .flags = VMAF_FEATURE_EXTRACTOR_SYCL,
    .provided_features = provided_features_ssimulacra2_sycl,
    .context_check = check_context_sycl,
    .context_fallback_name = "ssimulacra2",
};
