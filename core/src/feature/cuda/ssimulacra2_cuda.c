/**
 *  Copyright (c) the JPEG XL Project Authors.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent AND BSD-3-Clause
 *
 *  ssimulacra2 feature kernel on the CUDA backend (T7-23 / GPU
 *  long-tail batch 3 part 7b — ADR-0192 / ADR-0206). CUDA twin of
 *  ssimulacra2_vulkan (PR #156 / ADR-0201).
 *
 *  Pipeline (per ADR-0201 — same shape as the Vulkan port):
 *    1. Host: YUV → linear RGB on full-res frame (deterministic LUT
 *       sRGB EOTF, ADR-0164).
 *    2. Per-scale (up to 6, breaks early when min-dim < 8):
 *       a. Host: linear RGB → XYB (verbatim port of CPU
 *          `linear_rgb_to_xyb`). Computed on host because the GPU's
 *          float `cbrt` differs from libm by 42 ULP at the bit
 *          level — that drift cascades through the IIR + 108-weight
 *          pool to a ~1.5e-2 pooled-score drift on the Vulkan port,
 *          and the same fix carries over to CUDA / SYCL by
 *          construction. See ADR-0201 §Precision investigation.
 *       b. GPU: 3 elementwise 3-plane multiplies (ref², dis², ref·dis)
 *          via `ssimulacra2_mul3`.
 *       c. GPU: 5 separable IIR blurs (s11, s22, s12, mu1, mu2) via
 *          `ssimulacra2_blur_h` + `ssimulacra2_blur_v`. One thread
 *          per row (H pass) / per column (V pass).
 *       d. Host: per-pixel SSIM + EdgeDiff combine in double precision
 *          (verbatim ports of `ssim_map` + `edge_diff_map`). Reads
 *          from host-mapped pinned buffers; the ssim_map's
 *          `1 - num_m * num_s / denom_s` requires `(double)`
 *          promotion at the divide site, which is why we keep the
 *          combine on the CPU rather than running it as a GPU
 *          reduction.
 *       e. Host: 2×2 box downsample of the linear-RGB pyramid for
 *          the next scale.
 *    3. Host: pool 108 weighted norms via the libjxl polynomial.
 *       Emit `ssimulacra2`.
 *
 *  Precision contract (per ADR-0192 + ADR-0201): places=4
 *  (max_abs_diff ≤ 5e-5) on the Netflix CPU vs CUDA pair. The
 *  ssimulacra2_blur fatbin is compiled with --fmad=false (the
 *  `cuda_cu_extra_flags` map in libvmaf/src/meson.build); without
 *  it, NVCC fuses `n2*sum - d1*prev1` into FMAs and the IIR
 *  pole-tracking error compounds through the 6-scale pyramid.
 */

#include "vmaf_nullptr.h"

#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "log.h"
#include "mem.h"

#include "cuda/ssimulacra2_cuda.h"
#include "cuda_helper.cuh"
#include "feature/ssimulacra2_math.h"
#include "picture.h"
#include "picture_cuda.h"

/* lint rationale: C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `VMAF_NULLPTR` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

#define SS2C_NUM_SCALES 6
#define SS2C_BLUR_BLOCK 64
#define SS2C_TILE 32 /* Transpose tile dimension; mirrors SS2C_TILE in ssimulacra2_blur.cu. */
#define SS2C_MUL_BX 16
#define SS2C_MUL_BY 8
#define SS2C_SIGMA 1.5
#define SS2C_PI 3.14159265358979323846

enum yuv_matrix_c {
    SS2C_MATRIX_BT709_LIMITED = 0,
    SS2C_MATRIX_BT601_LIMITED = 1,
    SS2C_MATRIX_BT709_FULL = 2,
    SS2C_MATRIX_BT601_FULL = 3,
};

/* libjxl 108 pooling weights — bit-identical to ssimulacra2.c::kWeights. */
static const double g_weights[6][18] = {
    {0.0, 0.0007376606707406586, 0.0, 0.0, 0.0007793481682867309, 0.0, 0.0, 0.0004371155730107379,
     0.0, 1.1041726426657346, 0.00066284834129271, 0.00015231632783718752, 0.0,
     0.0016406437456599754, 0.0, 1.8422455520539298, 11.441172603757666, 0.0},
    {0.0007989109436015163, 0.000176816438078653, 0.0, 1.8787594979546387, 10.94906990605142, 0.0,
     0.0007289346991508072, 0.9677937080626833, 0.0, 0.00014003424285435884, 0.9981766977854967,
     0.00031949755934435053, 0.0004550992113792063, 0.0, 0.0, 0.0013648766163243398, 0.0, 0.0},
    {0.0, 0.0, 0.0, 7.466890328078848, 0.0, 17.445833984131262, 0.0006235601634041466, 0.0, 0.0,
     6.683678146179332, 0.00037724407979611296, 1.027889937768264, 225.20515300849274, 0.0, 0.0,
     19.213238186143016, 0.0011401524586618361, 0.001237755635509985},
    {176.39317598450694, 0.0, 0.0, 24.43300999870476, 0.28520802612117757, 0.0004485436923833408,
     0.0, 0.0, 0.0, 34.77906344483772, 44.835625328877896, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0},
    {0.0, 0.0008680556573291698, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0005313191874358747, 0.0,
     0.00016533814161379112, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0004179171803251336, 0.0017290828234722833,
     0.0},
    {0.0020827005846636437, 0.0, 0.0, 8.826982764996862, 23.19243343998926, 0.0, 95.1080498811086,
     0.9863978034400682, 0.9834382792465353, 0.0012286405048278493, 171.2667255897307,
     0.9807858872435379, 0.0, 0.0, 0.0, 0.0005130064588990679, 0.0, 0.00010854057858411537},
};

typedef struct Ssimu2StateCuda {
    /* Options. */
    int yuv_matrix;

    /* Geometry. */
    unsigned width;
    unsigned height;
    unsigned bpc;
    unsigned scale_w[SS2C_NUM_SCALES];
    unsigned scale_h[SS2C_NUM_SCALES];

    /* Recursive Gaussian coefficients (sigma=1.5). */
    float rg_n2[3];
    float rg_d1[3];
    int rg_radius;

    /* CUDA module + kernel handles. */
    CUmodule module_blur;
    CUmodule module_mul;
    CUfunction func_blur_h;    /* single-channel H pass (retained; unused after ADR-0456) */
    CUfunction func_blur_v;    /* single-channel V pass (retained; unused after ADR-0456) */
    CUfunction func_blur_h3;   /* fused 3-channel H pass (ADR-0456 Change 1) */
    CUfunction func_transpose; /* row→col-major transpose (ADR-0456 Change 2) */
    CUfunction
        func_blur_v3_transposed; /* fused 3-channel V pass on col-major input (ADR-0456 Changes 1+2) */
    CUfunction func_mul3;
    CUstream str;

    /* Device buffers. All 3-plane buffers are contiguous (X | Y | B
     * with planes at full-resolution stride, kept constant across
     * pyramid scales for layout consistency). */
    VmafCudaBuffer *d_ref_lin;
    VmafCudaBuffer *d_dis_lin;
    VmafCudaBuffer *d_ref_xyb;
    VmafCudaBuffer *d_dis_xyb;
    VmafCudaBuffer *d_mul_buf;
    VmafCudaBuffer *d_blur_scratch;
    /* Column-major transpose scratch for V-pass coalescing (ADR-0456).
     * Same size as d_blur_scratch — 3 × full-plane floats. */
    VmafCudaBuffer *d_transpose_buf;
    VmafCudaBuffer *d_mu1;
    VmafCudaBuffer *d_mu2;
    VmafCudaBuffer *d_s11;
    VmafCudaBuffer *d_s22;
    VmafCudaBuffer *d_s12;

    /* Pinned host buffers for upload + readback. */
    float *h_ref_lin;
    float *h_dis_lin;
    /* Per-scale 2x2-downsample scratch — pre-allocated once (was a
     * per-scale `malloc(3 * plane_full * sizeof(float))` in the hot
     * path; on 1080p that is 24 MB / scale × up to 5 scales / frame,
     * cheap on warm allocators but a real `mmap`/`brk` cost on
     * memory-pressured systems). Reused for both ref and dis on every
     * scale; the previous pyramid level is consumed before the next
     * is written so a single buffer suffices. */
    float *h_ref_lin_ds;
    float *h_dis_lin_ds;
    float *h_ref_xyb;
    float *h_dis_xyb;
    float *h_mu1;
    float *h_mu2;
    float *h_s11;
    float *h_s22;
    float *h_s12;

    /* Pinned raw-YUV scratch + per-plane row strides used by the
     * synthetic host VmafPicture fed into `ss2c_picture_to_linear_rgb`.
     * The picture_cuda backend hands us VmafPictures whose
     * `data[]` is a CUdeviceptr — direct host reads would segfault.
     * We D2H-copy each plane into these pinned buffers and rebuild
     * a host-side VmafPicture mirror for the YUV→linear-RGB pass. */
    void *h_ref_raw[3];
    void *h_dis_raw[3];
    size_t raw_plane_bytes[3];
    unsigned plane_w[3];
    unsigned plane_h[3];
    ptrdiff_t plane_row_bytes[3];
} Ssimu2StateCuda;

static const VmafOption options[] = {
    {
        .name = "yuv_matrix",
        .help = "YUV→RGB matrix: 0=bt709_limited (default), 1=bt601_limited, "
                "2=bt709_full, 3=bt601_full",
        .offset = offsetof(Ssimu2StateCuda, yuv_matrix),
        .type = VMAF_OPT_TYPE_INT,
        .default_val.i = SS2C_MATRIX_BT709_LIMITED,
        .min = 0,
        .max = 3,
    },
    {0},
};

/* ------------------------------------------------------------------ */
/* Recursive Gaussian coefficient setup — bit-identical port of
 * create_recursive_gaussian in ssimulacra2.c.                        */
/* ------------------------------------------------------------------ */

/* Verbatim port of the libjxl Charalampidis 2016 derivation; per
 * ADR-0141 carve-out, splitting the linear-system solve would
 * obscure the scalar-diff audit trail.
 * lint rationale */
static void ss2c_setup_gaussian(Ssimu2StateCuda *s, double sigma)
{
    const double radius = round(3.2795 * sigma + 0.2546);
    const double pi_div_2r = SS2C_PI / (2.0 * radius);
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
    for (int i = 0; i < 3; i++)
        rho[i] = exp(neg_half_sigma2 * omega[i] * omega[i]) * recip_r;

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

    s->rg_radius = (int)radius;
    for (int i = 0; i < 3; i++) {
        s->rg_n2[i] = (float)(-beta[i] * cos(omega[i] * (radius + 1.0)));
        s->rg_d1[i] = (float)(-2.0 * cos(omega[i]));
    }
}

/* ------------------------------------------------------------------ */
/* Host-side YUV → linear-RGB + linear-RGB → XYB + 2×2 downsample.
 * All match-for-match ports of the corresponding ssimulacra2.c
 * scalar paths so host outputs bit-match the CPU extractor.
 * ------------------------------------------------------------------ */

static inline float ss2c_clampf(float v, float lo, float hi)
{
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

static inline float ss2c_read_plane(const VmafPicture *pic, int plane, int x, int y)
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

/* Verbatim port of ssimulacra2.c::picture_to_linear_rgb. Splitting
 * would break the line-for-line scalar-diff audit trail
 * (ADR-0141 §2 upstream-parity load-bearing invariant; T7-5
 * sweep closeout — ADR-0278).
 * lint rationale */
typedef struct Ssimu2YuvTransform {
    float inv_peak;
    float cr_r;
    float cb_b;
    float cb_g;
    float cr_g;
    float y_scale;
    float c_scale;
    float y_offset;
} Ssimu2YuvTransform;

static Ssimu2YuvTransform ss2c_yuv_transform(const Ssimu2StateCuda *s)
{
    float kr;
    float kg;
    float kb;
    int limited = 1;
    switch (s->yuv_matrix) {
    case SS2C_MATRIX_BT709_FULL:
        limited = 0;
        // fallthrough
    case SS2C_MATRIX_BT709_LIMITED:
        kr = 0.2126f;
        kg = 0.7152f;
        kb = 0.0722f;
        break;
    case SS2C_MATRIX_BT601_FULL:
        limited = 0;
        // fallthrough
    case SS2C_MATRIX_BT601_LIMITED:
    default:
        kr = 0.299f;
        kg = 0.587f;
        kb = 0.114f;
        break;
    }
    const float peak = (float)((1u << s->bpc) - 1u);
    return (Ssimu2YuvTransform){
        .inv_peak = 1.0f / peak,
        .cr_r = 2.0f * (1.0f - kr),
        .cb_b = 2.0f * (1.0f - kb),
        .cb_g = -(2.0f * kb * (1.0f - kb)) / kg,
        .cr_g = -(2.0f * kr * (1.0f - kr)) / kg,
        .y_scale = limited ? 255.0f / 219.0f : 1.0f,
        .c_scale = limited ? 255.0f / 224.0f : 1.0f,
        .y_offset = limited ? 16.0f / 255.0f : 0.0f,
    };
}

static void ss2c_convert_pixel(const VmafPicture *pic, const Ssimu2YuvTransform *transform, int x,
                               int y, float *red, float *green, float *blue)
{
    const float y_sample = ss2c_read_plane(pic, 0, x, y) * transform->inv_peak;
    const float u_sample = ss2c_read_plane(pic, 1, x, y) * transform->inv_peak;
    const float v_sample = ss2c_read_plane(pic, 2, x, y) * transform->inv_peak;
    const float yn = (y_sample - transform->y_offset) * transform->y_scale;
    const float un = (u_sample - 0.5f) * transform->c_scale;
    const float vn = (v_sample - 0.5f) * transform->c_scale;
    float r = fmaf(transform->cr_r, vn, yn);
    float g = fmaf(transform->cb_g, un, yn);
    g = fmaf(transform->cr_g, vn, g);
    float b = fmaf(transform->cb_b, un, yn);
    r = ss2c_clampf(r, 0.0f, 1.0f);
    g = ss2c_clampf(g, 0.0f, 1.0f);
    b = ss2c_clampf(b, 0.0f, 1.0f);
    *red = vmaf_ss2_srgb_eotf(r);
    *green = vmaf_ss2_srgb_eotf(g);
    *blue = vmaf_ss2_srgb_eotf(b);
}

static void ss2c_picture_to_linear_rgb(const Ssimu2StateCuda *s, const VmafPicture *pic, float *out)
{
    const size_t plane_size = (size_t)s->width * (size_t)s->height;
    float *red = out;
    float *green = out + plane_size;
    float *blue = out + 2 * plane_size;
    const Ssimu2YuvTransform transform = ss2c_yuv_transform(s);
    for (unsigned y = 0; y < s->height; y++) {
        for (unsigned x = 0; x < s->width; x++) {
            const size_t index = (size_t)y * s->width + x;
            ss2c_convert_pixel(pic, &transform, (int)x, (int)y, &red[index], &green[index],
                               &blue[index]);
        }
    }
}

/* Host linear-RGB → XYB. Verbatim port of
 * ssimulacra2.c::linear_rgb_to_xyb. The 3-plane output stride is
 * `plane_stride` (= full_w * full_h, kept constant across scales). */
static void ss2c_host_linear_rgb_to_xyb(const float *lin, float *xyb, unsigned w, unsigned h,
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
        B = (B - Y) + 0.55f;
        X = X * 14.0f + 0.42f;
        Y = Y + 0.01f;
        xp[i] = X;
        yp[i] = Y;
        bxp[i] = B;
    }
}

static void ss2c_downsample_2x2(const float *in, unsigned iw, unsigned ih, float *out, unsigned ow,
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

/* ------------------------------------------------------------------ */
/* Lifecycle                                                          */
/* ------------------------------------------------------------------ */

static int close_fex_cuda(VmafFeatureExtractor *fex);

static int ss2c_alloc_buffers(VmafFeatureExtractor *fex, Ssimu2StateCuda *s)
{
    const size_t bytes = 3u * (size_t)s->width * (size_t)s->height * sizeof(float);
    const size_t raw_bytes = (size_t)s->width * (size_t)s->height * 2u;
    int ret = 0;
    for (int plane = 0; plane < 3; plane++) {
        ret |= vmaf_cuda_buffer_host_alloc(fex->cu_state, &s->h_ref_raw[plane], raw_bytes);
        ret |= vmaf_cuda_buffer_host_alloc(fex->cu_state, &s->h_dis_raw[plane], raw_bytes);
        s->raw_plane_bytes[plane] = raw_bytes;
    }
    VmafCudaBuffer **device[] = {
        &s->d_ref_lin, &s->d_dis_lin,      &s->d_ref_xyb,       &s->d_dis_xyb,
        &s->d_mul_buf, &s->d_blur_scratch, &s->d_transpose_buf, &s->d_mu1,
        &s->d_mu2,     &s->d_s11,          &s->d_s22,           &s->d_s12,
    };
    for (size_t i = 0; i < sizeof(device) / sizeof(device[0]); i++)
        ret |= vmaf_cuda_buffer_alloc(fex->cu_state, device[i], bytes);
    void **host[] = {
        (void **)&s->h_ref_lin,    (void **)&s->h_dis_lin, (void **)&s->h_ref_lin_ds,
        (void **)&s->h_dis_lin_ds, (void **)&s->h_ref_xyb, (void **)&s->h_dis_xyb,
        (void **)&s->h_mu1,        (void **)&s->h_mu2,     (void **)&s->h_s11,
        (void **)&s->h_s22,        (void **)&s->h_s12,
    };
    for (size_t i = 0; i < sizeof(host) / sizeof(host[0]); i++)
        ret |= vmaf_cuda_buffer_host_alloc(fex->cu_state, host[i], bytes);
    return ret ? -ENOMEM : 0;
}

static int ss2c_init_geometry(Ssimu2StateCuda *s, unsigned bpc, unsigned width, unsigned height)
{
    if (width < 8u || height < 8u) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "ssimulacra2_cuda: input %ux%u below 8x8 lower bound\n",
                 width, height);
        return -EINVAL;
    }
    s->width = width;
    s->height = height;
    s->bpc = bpc;
    ss2c_setup_gaussian(s, SS2C_SIGMA);
    s->scale_w[0] = width;
    s->scale_h[0] = height;
    for (int scale = 1; scale < SS2C_NUM_SCALES; scale++) {
        s->scale_w[scale] = (s->scale_w[scale - 1] + 1) / 2;
        s->scale_h[scale] = (s->scale_h[scale - 1] + 1) / 2;
    }
    return 0;
}

static int ss2c_setup_runtime(VmafFeatureExtractor *fex, Ssimu2StateCuda *s)
{
    CudaFunctions *cu_f = fex->cu_state->f;
    CHECK_CUDA_RETURN(cu_f, cuStreamCreateWithPriority(&s->str, CU_STREAM_NON_BLOCKING, 0));
    CHECK_CUDA_RETURN(cu_f, cuModuleLoadData(&s->module_blur, ssimulacra2_blur_ptx));
    CHECK_CUDA_RETURN(cu_f, cuModuleLoadData(&s->module_mul, ssimulacra2_mul_ptx));
    CHECK_CUDA_RETURN(cu_f,
                      cuModuleGetFunction(&s->func_blur_h, s->module_blur, "ssimulacra2_blur_h"));
    CHECK_CUDA_RETURN(cu_f,
                      cuModuleGetFunction(&s->func_blur_v, s->module_blur, "ssimulacra2_blur_v"));
    CHECK_CUDA_RETURN(cu_f,
                      cuModuleGetFunction(&s->func_blur_h3, s->module_blur, "ssimulacra2_blur_h3"));
    CHECK_CUDA_RETURN(
        cu_f, cuModuleGetFunction(&s->func_transpose, s->module_blur, "ssimulacra2_transpose"));
    CHECK_CUDA_RETURN(cu_f, cuModuleGetFunction(&s->func_blur_v3_transposed, s->module_blur,
                                                "ssimulacra2_blur_v3_transposed"));
    CHECK_CUDA_RETURN(cu_f, cuModuleGetFunction(&s->func_mul3, s->module_mul, "ssimulacra2_mul3"));
    return 0;
}

static int ss2c_pop_context(CudaFunctions *cu_f)
{
    CHECK_CUDA_RETURN(cu_f, cuCtxPopCurrent(VMAF_NULLPTR));
    return 0;
}

static int ss2c_sync_stream(Ssimu2StateCuda *s, CudaFunctions *cu_f)
{
    CHECK_CUDA_RETURN(cu_f, cuStreamSynchronize(s->str));
    return 0;
}

static int ss2c_create_runtime(VmafFeatureExtractor *fex, Ssimu2StateCuda *s)
{
    CudaFunctions *cu_f = fex->cu_state->f;
    CHECK_CUDA_RETURN(cu_f, cuCtxPushCurrent(fex->cu_state->ctx));
    const int setup_ret = ss2c_setup_runtime(fex, s);
    if (setup_ret)
        (void)close_fex_cuda(fex);
    const int pop_ret = ss2c_pop_context(cu_f);
    return setup_ret ? setup_ret : pop_ret;
}

static int init_fex_cuda(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                         unsigned width, unsigned height)
{
    (void)pix_fmt;
    Ssimu2StateCuda *s = fex->priv;
    int ret = ss2c_init_geometry(s, bpc, width, height);
    if (!ret)
        ret = ss2c_create_runtime(fex, s);
    if (!ret)
        ret = ss2c_alloc_buffers(fex, s);
    if (ret)
        (void)close_fex_cuda(fex);
    return ret;
}

/* ------------------------------------------------------------------ */
/* Per-frame extract                                                  */
/* ------------------------------------------------------------------ */

static int ss2c_launch_mul3(Ssimu2StateCuda *s, CudaFunctions *cu_f, CUdeviceptr a, CUdeviceptr b,
                            CUdeviceptr out, unsigned scale)
{
    unsigned width = s->scale_w[scale];
    unsigned height = s->scale_h[scale];
    unsigned plane_count = 3u;
    unsigned plane_stride = s->width * s->height;
    unsigned grid_x = (width + SS2C_MUL_BX - 1u) / SS2C_MUL_BX;
    unsigned grid_y = (height + SS2C_MUL_BY - 1u) / SS2C_MUL_BY;
    void *args[] = {&a, &b, &out, &width, &height, &plane_count, &plane_stride};
    CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_mul3, grid_x, grid_y, 1, SS2C_MUL_BX,
                                           SS2C_MUL_BY, 1, 0, s->str, args, VMAF_NULLPTR));
    return 0;
}

static int ss2c_launch_blur_h3(Ssimu2StateCuda *s, CudaFunctions *cu_f, CUdeviceptr input,
                               CUdeviceptr output, unsigned width, unsigned height)
{
    float n2_0 = s->rg_n2[0];
    float n2_1 = s->rg_n2[1];
    float n2_2 = s->rg_n2[2];
    float d1_0 = s->rg_d1[0];
    float d1_1 = s->rg_d1[1];
    float d1_2 = s->rg_d1[2];
    int radius = s->rg_radius;
    unsigned stride = s->width * s->height;
    void *args[] = {&input, &output, &width, &height, &n2_0,   &n2_1,
                    &n2_2,  &d1_0,   &d1_1,  &d1_2,   &radius, &stride};
    unsigned grid_x = (height + SS2C_BLUR_BLOCK - 1u) / SS2C_BLUR_BLOCK;
    CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_blur_h3, grid_x, 1, 3u, SS2C_BLUR_BLOCK, 1, 1, 0,
                                           s->str, args, VMAF_NULLPTR));
    return 0;
}

static int ss2c_launch_transpose(Ssimu2StateCuda *s, CudaFunctions *cu_f, CUdeviceptr input,
                                 CUdeviceptr output, unsigned width, unsigned height)
{
    unsigned stride = s->width * s->height;
    void *args[] = {&input, &output, &width, &height, &stride};
    unsigned grid_x = (width + SS2C_TILE - 1u) / SS2C_TILE;
    unsigned grid_y = (height + SS2C_TILE - 1u) / SS2C_TILE;
    CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_transpose, grid_x, grid_y, 3u, SS2C_TILE,
                                           SS2C_TILE, 1, 0, s->str, args, VMAF_NULLPTR));
    return 0;
}

static int ss2c_launch_blur_v3_transposed(Ssimu2StateCuda *s, CudaFunctions *cu_f,
                                          CUdeviceptr input, CUdeviceptr output, unsigned width,
                                          unsigned height)
{
    float n2_0 = s->rg_n2[0];
    float n2_1 = s->rg_n2[1];
    float n2_2 = s->rg_n2[2];
    float d1_0 = s->rg_d1[0];
    float d1_1 = s->rg_d1[1];
    float d1_2 = s->rg_d1[2];
    int radius = s->rg_radius;
    unsigned stride = s->width * s->height;
    void *args[] = {&input, &output, &width, &height, &n2_0,   &n2_1,
                    &n2_2,  &d1_0,   &d1_1,  &d1_2,   &radius, &stride};
    unsigned grid_x = (width + SS2C_BLUR_BLOCK - 1u) / SS2C_BLUR_BLOCK;
    CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_blur_v3_transposed, grid_x, 1, 3u,
                                           SS2C_BLUR_BLOCK, 1, 1, 0, s->str, args, VMAF_NULLPTR));
    return 0;
}

static int ss2c_blur_3plane(Ssimu2StateCuda *s, CudaFunctions *cu_f, CUdeviceptr input,
                            CUdeviceptr output, unsigned scale)
{
    const unsigned width = s->scale_w[scale];
    const unsigned height = s->scale_h[scale];
    CUdeviceptr scratch = (CUdeviceptr)s->d_blur_scratch->data;
    CUdeviceptr transpose = (CUdeviceptr)s->d_transpose_buf->data;
    int ret = ss2c_launch_blur_h3(s, cu_f, input, scratch, width, height);
    if (!ret)
        ret = ss2c_launch_transpose(s, cu_f, scratch, transpose, width, height);
    if (!ret)
        ret = ss2c_launch_blur_v3_transposed(s, cu_f, transpose, output, width, height);
    return ret;
}

static void ss2c_host_combine(const Ssimu2StateCuda *s, int scale, double avg_ssim[6],
                              double avg_ed[12])
{
    const size_t full_plane = (size_t)s->width * (size_t)s->height;
    const size_t pixels = (size_t)s->scale_w[scale] * (size_t)s->scale_h[scale];
    const double inv_pixels = 1.0 / (double)pixels;
    for (int channel = 0; channel < 3; channel++) {
        const float *m1 = s->h_mu1 + (size_t)channel * full_plane;
        const float *m2 = s->h_mu2 + (size_t)channel * full_plane;
        const float *s11 = s->h_s11 + (size_t)channel * full_plane;
        const float *s22 = s->h_s22 + (size_t)channel * full_plane;
        const float *s12 = s->h_s12 + (size_t)channel * full_plane;
        const float *r1 = s->h_ref_xyb + (size_t)channel * full_plane;
        const float *r2 = s->h_dis_xyb + (size_t)channel * full_plane;
        double sum_l1 = 0.0;
        double sum_l4 = 0.0;
        double artifact = 0.0;
        double artifact4 = 0.0;
        double detail = 0.0;
        double detail4 = 0.0;
        for (size_t i = 0; i < pixels; i++) {
            const float u1 = m1[i];
            const float u2 = m2[i];
            const float u11 = u1 * u1;
            const float u22 = u2 * u2;
            const float u12 = u1 * u2;
            const float num_m = 1.0f - (u1 - u2) * (u1 - u2);
            const float num_s = 2.0f * (s12[i] - u12) + 0.0009f;
            const float denom_s = (s11[i] - u11) + (s22[i] - u22) + 0.0009f;
            double delta = 1.0 - ((double)num_m * (double)num_s / (double)denom_s);
            if (delta < 0.0)
                delta = 0.0;
            sum_l1 += delta;
            const double delta2 = delta * delta;
            sum_l4 += delta2 * delta2;
            const double edge1 = fabs((double)r1[i] - (double)u1);
            const double edge2 = fabs((double)r2[i] - (double)u2);
            const double edge = (1.0 + edge2) / (1.0 + edge1) - 1.0;
            const double art = edge > 0.0 ? edge : 0.0;
            const double det = edge < 0.0 ? -edge : 0.0;
            artifact += art;
            const double art2 = art * art;
            artifact4 += art2 * art2;
            detail += det;
            const double det2 = det * det;
            detail4 += det2 * det2;
        }
        const size_t ssim_offset = (size_t)channel * 2U;
        const size_t edge_offset = (size_t)channel * 4U;
        avg_ssim[ssim_offset] = inv_pixels * sum_l1;
        avg_ssim[ssim_offset + 1U] = sqrt(sqrt(inv_pixels * sum_l4));
        avg_ed[edge_offset] = inv_pixels * artifact;
        avg_ed[edge_offset + 1U] = sqrt(sqrt(inv_pixels * artifact4));
        avg_ed[edge_offset + 2U] = inv_pixels * detail;
        avg_ed[edge_offset + 3U] = sqrt(sqrt(inv_pixels * detail4));
    }
}

static double ss2c_pool_score(const double avg_ssim[6][6], const double avg_ed[6][12],
                              int num_scales)
{
    double ssim = 0.0;
    size_t i = 0;
    for (int channel = 0; channel < 3; channel++) {
        for (int scale = 0; scale < 6; scale++) {
            for (int norm = 0; norm < 2; norm++) {
                double s = scale < num_scales ? avg_ssim[scale][channel * 2 + norm] : 0.0;
                double artifact = scale < num_scales ? avg_ed[scale][channel * 4 + norm] : 0.0;
                double detail = scale < num_scales ? avg_ed[scale][channel * 4 + norm + 2] : 0.0;
                ssim += g_weights[i / 18][i % 18] * fabs(s);
                i++;
                ssim += g_weights[i / 18][i % 18] * fabs(artifact);
                i++;
                ssim += g_weights[i / 18][i % 18] * fabs(detail);
                i++;
            }
        }
    }
    ssim *= 0.9562382616834844;
    ssim = 2.326765642916932 * ssim - 0.020884521182843837 * ssim * ssim +
           6.248496625763138e-05 * ssim * ssim * ssim;
    return ssim > 0.0 ? 100.0 - 10.0 * pow(ssim, 0.6276336467831387) : 100.0;
}

typedef struct Ssimu2DevicePlanes {
    CUdeviceptr ref_xyb;
    CUdeviceptr dis_xyb;
    CUdeviceptr xyb_product;
    CUdeviceptr mu1;
    CUdeviceptr mu2;
    CUdeviceptr s11;
    CUdeviceptr s22;
    CUdeviceptr s12;
} Ssimu2DevicePlanes;

static Ssimu2DevicePlanes ss2c_device_planes(const Ssimu2StateCuda *s)
{
    return (Ssimu2DevicePlanes){
        .ref_xyb = (CUdeviceptr)s->d_ref_xyb->data,
        .dis_xyb = (CUdeviceptr)s->d_dis_xyb->data,
        .xyb_product = (CUdeviceptr)s->d_mul_buf->data,
        .mu1 = (CUdeviceptr)s->d_mu1->data,
        .mu2 = (CUdeviceptr)s->d_mu2->data,
        .s11 = (CUdeviceptr)s->d_s11->data,
        .s22 = (CUdeviceptr)s->d_s22->data,
        .s12 = (CUdeviceptr)s->d_s12->data,
    };
}

static int ss2c_upload_xyb(Ssimu2StateCuda *s, CudaFunctions *cu_f,
                           const Ssimu2DevicePlanes *planes, size_t bytes)
{
    const size_t plane_bytes = (size_t)s->width * (size_t)s->height * sizeof(float);
    for (size_t channel = 0; channel < 3u; channel++) {
        const size_t offset = channel * plane_bytes;
        CHECK_CUDA_RETURN(cu_f,
                          cuMemcpyHtoDAsync(planes->ref_xyb + offset,
                                            (const uint8_t *)s->h_ref_xyb + offset, bytes, s->str));
        CHECK_CUDA_RETURN(cu_f,
                          cuMemcpyHtoDAsync(planes->dis_xyb + offset,
                                            (const uint8_t *)s->h_dis_xyb + offset, bytes, s->str));
    }
    return 0;
}

static int ss2c_compute_scale(Ssimu2StateCuda *s, CudaFunctions *cu_f, const Ssimu2DevicePlanes *p,
                              int scale)
{
    int ret =
        ss2c_launch_mul3(s, cu_f, p->ref_xyb, p->ref_xyb, p->xyb_product, (unsigned)scale);
    if (!ret)
        ret = ss2c_blur_3plane(s, cu_f, p->xyb_product, p->s11, (unsigned)scale);
    if (!ret)
        ret = ss2c_launch_mul3(s, cu_f, p->dis_xyb, p->dis_xyb, p->xyb_product,
                               (unsigned)scale);
    if (!ret)
        ret = ss2c_blur_3plane(s, cu_f, p->xyb_product, p->s22, (unsigned)scale);
    if (!ret)
        ret = ss2c_launch_mul3(s, cu_f, p->ref_xyb, p->dis_xyb, p->xyb_product,
                               (unsigned)scale);
    if (!ret)
        ret = ss2c_blur_3plane(s, cu_f, p->xyb_product, p->s12, (unsigned)scale);
    if (!ret)
        ret = ss2c_blur_3plane(s, cu_f, p->ref_xyb, p->mu1, (unsigned)scale);
    if (!ret)
        ret = ss2c_blur_3plane(s, cu_f, p->dis_xyb, p->mu2, (unsigned)scale);
    return ret;
}

static int ss2c_download_scale(Ssimu2StateCuda *s, CudaFunctions *cu_f, const Ssimu2DevicePlanes *p,
                               size_t bytes)
{
    const size_t plane_bytes = (size_t)s->width * (size_t)s->height * sizeof(float);
    void *host[] = {s->h_mu1, s->h_mu2, s->h_s11, s->h_s22, s->h_s12};
    const CUdeviceptr device[] = {p->mu1, p->mu2, p->s11, p->s22, p->s12};
    for (size_t channel = 0; channel < 3u; channel++) {
        const size_t offset = channel * plane_bytes;
        for (size_t buffer = 0; buffer < sizeof(host) / sizeof(host[0]); buffer++) {
            CHECK_CUDA_RETURN(cu_f, cuMemcpyDtoHAsync((uint8_t *)host[buffer] + offset,
                                                      device[buffer] + offset, bytes, s->str));
        }
    }
    return 0;
}

static int ss2c_run_scale_gpu(Ssimu2StateCuda *s, CudaFunctions *cu_f, int scale)
{
    const size_t bytes = (size_t)s->scale_w[scale] * (size_t)s->scale_h[scale] * sizeof(float);
    const Ssimu2DevicePlanes planes = ss2c_device_planes(s);
    int ret = ss2c_upload_xyb(s, cu_f, &planes, bytes);
    if (!ret)
        ret = ss2c_compute_scale(s, cu_f, &planes, scale);
    if (!ret)
        ret = ss2c_download_scale(s, cu_f, &planes, bytes);
    if (!ret)
        ret = ss2c_sync_stream(s, cu_f);
    return ret;
}

static int ss2c_copy_raw_planes(Ssimu2StateCuda *s, CudaFunctions *cu_f, const VmafPicture *ref,
                                const VmafPicture *dist, VmafPicture *host_ref,
                                VmafPicture *host_dist)
{
    const size_t bytes_per_pixel = s->bpc <= 8u ? 1u : 2u;
    *host_ref = *ref;
    *host_dist = *dist;
    for (int plane = 0; plane < 3; plane++) {
        const ptrdiff_t row_bytes = (ptrdiff_t)ref->w[plane] * (ptrdiff_t)bytes_per_pixel;
        s->plane_w[plane] = ref->w[plane];
        s->plane_h[plane] = ref->h[plane];
        s->plane_row_bytes[plane] = row_bytes;
        CUDA_MEMCPY2D copy = {
            .srcMemoryType = CU_MEMORYTYPE_DEVICE,
            .srcDevice = (CUdeviceptr)ref->data[plane],
            .srcPitch = ref->stride[plane],
            .dstMemoryType = CU_MEMORYTYPE_HOST,
            .dstHost = s->h_ref_raw[plane],
            .dstPitch = (size_t)row_bytes,
            .WidthInBytes = (size_t)row_bytes,
            .Height = ref->h[plane],
        };
        CHECK_CUDA_RETURN(cu_f, cuMemcpy2DAsync(&copy, s->str));
        copy.srcDevice = (CUdeviceptr)dist->data[plane];
        copy.srcPitch = dist->stride[plane];
        copy.dstHost = s->h_dis_raw[plane];
        CHECK_CUDA_RETURN(cu_f, cuMemcpy2DAsync(&copy, s->str));
        host_ref->data[plane] = s->h_ref_raw[plane];
        host_ref->stride[plane] = row_bytes;
        host_dist->data[plane] = s->h_dis_raw[plane];
        host_dist->stride[plane] = row_bytes;
    }
    return 0;
}

static void ss2c_downsample_pyramid(float *image, float *scratch, unsigned width, unsigned height,
                                    size_t plane_stride)
{
    const unsigned next_width = (width + 1) / 2;
    const unsigned next_height = (height + 1) / 2;
    ss2c_downsample_2x2(image, width, height, scratch, next_width, next_height, plane_stride);
    for (int channel = 0; channel < 3; channel++) {
        memcpy(image + (size_t)channel * plane_stride, scratch + (size_t)channel * plane_stride,
               (size_t)next_width * (size_t)next_height * sizeof(float));
    }
}

static int ss2c_process_scales(Ssimu2StateCuda *s, CudaFunctions *cu_f, double avg_ssim[6][6],
                               double avg_ed[6][12], int *completed)
{
    unsigned width = s->width;
    unsigned height = s->height;
    const size_t plane_stride = (size_t)s->width * (size_t)s->height;
    for (int scale = 0; scale < SS2C_NUM_SCALES && width >= 8u && height >= 8u; scale++) {
        ss2c_host_linear_rgb_to_xyb(s->h_ref_lin, s->h_ref_xyb, width, height, plane_stride);
        ss2c_host_linear_rgb_to_xyb(s->h_dis_lin, s->h_dis_xyb, width, height, plane_stride);
        const int ret = ss2c_run_scale_gpu(s, cu_f, scale);
        if (ret)
            return ret;
        ss2c_host_combine(s, scale, avg_ssim[scale], avg_ed[scale]);
        (*completed)++;
        if (scale + 1 < SS2C_NUM_SCALES) {
            ss2c_downsample_pyramid(s->h_ref_lin, s->h_ref_lin_ds, width, height, plane_stride);
            ss2c_downsample_pyramid(s->h_dis_lin, s->h_dis_lin_ds, width, height, plane_stride);
            width = (width + 1) / 2;
            height = (height + 1) / 2;
        }
    }
    return 0;
}

static int extract_fex_cuda(VmafFeatureExtractor *fex, const VmafPicture *ref_pic,
                            const VmafPicture *ref_pic_90, const VmafPicture *dist_pic,
                            const VmafPicture *dist_pic_90, unsigned index,
                            VmafFeatureCollector *feature_collector)
{
    (void)ref_pic_90;
    (void)dist_pic_90;
    Ssimu2StateCuda *s = fex->priv;
    CudaFunctions *cu_f = fex->cu_state->f;
    CHECK_CUDA_RETURN(cu_f, cuCtxPushCurrent(fex->cu_state->ctx));
    VmafPicture host_ref;
    VmafPicture host_dist;
    int ret = ss2c_copy_raw_planes(s, cu_f, ref_pic, dist_pic, &host_ref, &host_dist);
    if (!ret)
        ret = ss2c_sync_stream(s, cu_f);
    if (!ret) {
        ss2c_picture_to_linear_rgb(s, &host_ref, s->h_ref_lin);
        ss2c_picture_to_linear_rgb(s, &host_dist, s->h_dis_lin);
    }
    double avg_ssim[6][6] = {{0}};
    double avg_ed[6][12] = {{0}};
    int completed = 0;
    if (!ret)
        ret = ss2c_process_scales(s, cu_f, avg_ssim, avg_ed, &completed);
    const int pop_ret = ss2c_pop_context(cu_f);
    if (ret)
        return ret;
    if (pop_ret)
        return pop_ret;
    const double score = ss2c_pool_score(avg_ssim, avg_ed, completed);
    return vmaf_feature_collector_append(feature_collector, "ssimulacra2", score, index);
}

static int ss2c_free_device(VmafFeatureExtractor *fex, VmafCudaBuffer **buffer)
{
    if (!*buffer)
        return 0;
    const int ret = vmaf_cuda_buffer_free(fex->cu_state, *buffer);
    free(*buffer);
    *buffer = VMAF_NULLPTR;
    return ret;
}

static int ss2c_free_host(VmafFeatureExtractor *fex, void **buffer)
{
    if (!*buffer)
        return 0;
    const int ret = vmaf_cuda_buffer_host_free(fex->cu_state, *buffer);
    *buffer = VMAF_NULLPTR;
    return ret;
}

static int close_fex_cuda(VmafFeatureExtractor *fex)
{
    Ssimu2StateCuda *s = fex->priv;
    if (!s)
        return 0;
    CudaFunctions *cu_f = fex->cu_state ? fex->cu_state->f : VMAF_NULLPTR;
    if (cu_f && s->str) {
        (void)cu_f->cuStreamSynchronize(s->str);
        (void)cu_f->cuStreamDestroy(s->str);
        s->str = 0;
    }
    if (cu_f && s->module_blur) {
        (void)cu_f->cuModuleUnload(s->module_blur);
        s->module_blur = VMAF_NULLPTR;
    }
    if (cu_f && s->module_mul) {
        (void)cu_f->cuModuleUnload(s->module_mul);
        s->module_mul = VMAF_NULLPTR;
    }
    VmafCudaBuffer **device[] = {
        &s->d_ref_lin, &s->d_dis_lin,      &s->d_ref_xyb,       &s->d_dis_xyb,
        &s->d_mul_buf, &s->d_blur_scratch, &s->d_transpose_buf, &s->d_mu1,
        &s->d_mu2,     &s->d_s11,          &s->d_s22,           &s->d_s12,
    };
    int ret = 0;
    for (size_t i = 0; i < sizeof(device) / sizeof(device[0]); i++)
        ret |= ss2c_free_device(fex, device[i]);
    void **host[] = {
        (void **)&s->h_ref_lin,    (void **)&s->h_dis_lin, (void **)&s->h_ref_lin_ds,
        (void **)&s->h_dis_lin_ds, (void **)&s->h_ref_xyb, (void **)&s->h_dis_xyb,
        (void **)&s->h_mu1,        (void **)&s->h_mu2,     (void **)&s->h_s11,
        (void **)&s->h_s22,        (void **)&s->h_s12,
    };
    for (size_t i = 0; i < sizeof(host) / sizeof(host[0]); i++)
        ret |= ss2c_free_host(fex, host[i]);
    for (int plane = 0; plane < 3; plane++) {
        ret |= ss2c_free_host(fex, &s->h_ref_raw[plane]);
        ret |= ss2c_free_host(fex, &s->h_dis_raw[plane]);
    }
    return ret;
}

static const char *provided_features[] = {"ssimulacra2", VMAF_NULLPTR};

VmafFeatureExtractor vmaf_fex_ssimulacra2_cuda = {
    .name = "ssimulacra2_cuda",
    .init = init_fex_cuda,
    .extract = extract_fex_cuda,
    .close = close_fex_cuda,
    .options = options,
    .priv_size = sizeof(Ssimu2StateCuda),
    .flags = VMAF_FEATURE_EXTRACTOR_CUDA,
    .provided_features = provided_features,
};
