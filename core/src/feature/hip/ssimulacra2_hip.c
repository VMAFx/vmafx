/**
 *  Copyright (c) the JPEG XL Project Authors.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent AND BSD-3-Clause
 *
 *  ssimulacra2 feature extractor on the HIP backend, device-resident since
 *  ADR-1390: the HIP port of the ADR-1363 chain of
 *  `core/src/feature/sycl/ssimulacra2_sycl.cpp`.
 *
 *  Per frame, one in-order chain on the extractor's private stream, with no
 *  host compute and no host wait between submit() and collect():
 *    1. submit(): the six raw Y/U/V planes are packed into pinned staging and
 *       copied to the device. That copy is the only host-to-device traffic.
 *    2. YUV -> linear RGB (`vmaf_ss2_srgb_eotf`, single-rounded FMAs in the
 *       ADR-0891 / ADR-1205 order).
 *    3. Per scale (up to 6, stops before the first scale below 8x8):
 *       a. linear RGB -> XYB (`vmaf_ss2_cbrtf`, correctly rounded division);
 *       b. five separable 3-pole IIR blurs: a row pass staged through LDS
 *          (coalesced; it forms the products ref^2, dis^2 and ref*dis while
 *          it loads), then a lane-per-column pass;
 *       c. the per-pixel SSIM and edge-difference terms, reduced over each
 *          plane to six sums per channel in a fixed tree of exact fp32 pairs;
 *       d. the 2x2 box downsample of the linear-RGB pyramid.
 *    4. One 864-byte readback of the per-scale sums. collect() waits once,
 *       forms the 108 norms in fp64 and pools the score as ssimulacra2.c
 *       does.
 *
 *  Numerical contract (see ssimulacra2/ssimulacra2_device.hip): stages 2, 3a,
 *  3b and 3d reproduce the CPU extractor bit for bit. The CPU evaluates the
 *  per-pixel terms of 3c in fp64 and sums millions of them one after another;
 *  no parallel reduction can replay that order, so the device evaluates each
 *  term as an fp32 pair and sums in the SYCL twin's tree. The score stays
 *  within about 1e-11 of the CPU and equals ssimulacra2_sycl's for the same
 *  input.
 *
 *  HIP specifics: `hipModuleLoadData` / `hipModuleGetFunction` /
 *  `hipModuleLaunchKernel` on one HSACO module, raw `hipMalloc` device
 *  buffers, `hipHostMalloc` pinned staging, a private non-blocking stream.
 *  Without HAVE_HIPCC there is no device module and the extractor reports
 *  -ENOSYS (ADR-1264).
 */

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <hip/hip_runtime_api.h>

#include "feature_collector.h"
#include "feature_extractor.h"
#include "log.h"

#include "feature/ssimulacra2_score.h"
#include "picture.h"
#include "ssimulacra2_hip.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

#define SS2H_SIGMA 1.5
#define SS2H_PI 3.14159265358979323846

enum yuv_matrix_h {
    SS2H_MATRIX_BT709_LIMITED = 0,
    SS2H_MATRIX_BT601_LIMITED = 1,
    SS2H_MATRIX_BT709_FULL = 2,
    SS2H_MATRIX_BT601_FULL = 3,
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

typedef struct Ssimu2StateHip {
    /* Options. */
    int yuv_matrix;

    /* Geometry. */
    unsigned width;
    unsigned height;
    unsigned bpc;
    unsigned scale_w[SS2H_NUM_SCALES];
    unsigned scale_h[SS2H_NUM_SCALES];
    int num_scales;
    unsigned plane_w[SS2H_CHANNELS];
    unsigned plane_h[SS2H_CHANNELS];
    size_t row_bytes[SS2H_CHANNELS];

    /* Recursive Gaussian (sigma = 1.5) and YUV -> RGB constants. */
    struct Ss2hIir iir;
    struct Ss2hYuvCoefficients yuv;

    /* HIP module + kernel handles. */
    hipModule_t module;
    hipFunction_t func_yuv;
    hipFunction_t func_xyb;
    hipFunction_t func_blur_rows;
    hipFunction_t func_blur_rows_product;
    hipFunction_t func_blur_cols;
    hipFunction_t func_combine_partials;
    hipFunction_t func_combine_final;
    hipFunction_t func_downsample;
    hipStream_t str;

    /* Device buffers (raw hipMalloc pointers). Three-plane images are
   * compact at every scale: plane c of a cw x ch scale starts at
   * c * cw * ch. */
    void *d_raw[SS2H_IMAGES][SS2H_CHANNELS];
    /* Linear-RGB pyramid, ping-pong per image: [image][0] holds scales 0, 2
   * and 4, [image][1] (a quarter of the size) scales 1, 3 and 5. */
    float *d_lin[SS2H_IMAGES][2];
    float *d_xyb[SS2H_IMAGES];
    float *d_scratch; /* the row pass of every blur */
    float *d_mu1;
    float *d_mu2;
    float *d_s11;
    float *d_s22;
    float *d_s12;
    float *d_partials; /* [channel][group][sum][pair] */
    float *d_totals;   /* [scale][channel][sum][pair] */

    /* Pinned host buffers (hipHostMalloc). */
    void *h_raw[SS2H_IMAGES][SS2H_CHANNELS];
    float *h_totals;

    /* The frame submit() enqueued and collect() has not read yet. */
    bool has_pending;
    unsigned pending_index;
} Ssimu2StateHip;

static const VmafOption options[] = {
    {
        .name = "yuv_matrix",
        .help = "YUV→RGB matrix: 0=bt709_limited (default), 1=bt601_limited, "
                "2=bt709_full, 3=bt601_full",
        .offset = offsetof(Ssimu2StateHip, yuv_matrix),
        .type = VMAF_OPT_TYPE_INT,
        .default_val.i = SS2H_MATRIX_BT709_LIMITED,
        .min = 0,
        .max = 3,
    },
    {0},
};

static int ss2h_hip_rc(hipError_t rc)
{
    if (rc == hipSuccess)
        return 0;
    switch (rc) {
    case hipErrorInvalidValue:
    case hipErrorInvalidHandle:
        return -EINVAL;
    case hipErrorOutOfMemory:
        return -ENOMEM;
    case hipErrorNoDevice:
    case hipErrorInvalidDevice:
        return -ENODEV;
    case hipErrorNotSupported:
        return -ENOSYS;
    default:
        return -EIO;
    }
}

/* ADR-1324 / ADR-1359: the inputs init rejects (no chroma planes, a side
 * below 8) go to the CPU extractor when the twin was picked for a model or a
 * `--backend hip --feature ssimulacra2` request. */
static int check_context_hip(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                             unsigned w, unsigned h)
{
    (void)fex;
    (void)bpc;
    const bool chroma = pix_fmt != VMAF_PIX_FMT_YUV400P && pix_fmt != VMAF_PIX_FMT_UNKNOWN;
    return (chroma && w >= 8u && h >= 8u) ? 0 : -ENOTSUP;
}

#ifdef HAVE_HIPCC

/* ------------------------------------------------------------------ */
/* Host setup: recursive Gaussian, YUV constants, geometry             */
/* ------------------------------------------------------------------ */

/* Verbatim port of ssimulacra2.c::create_recursive_gaussian; the coefficients
 * are the same floats the CPU extractor blurs with. */
static void ss2h_setup_gaussian(struct Ss2hIir *iir, double sigma)
{
    const double radius = round(3.2795 * sigma + 0.2546);
    const double pi_div_2r = SS2H_PI / (2.0 * radius);
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

    iir->radius = (int)radius;
    for (int i = 0; i < 3; i++) {
        iir->n2[i] = (float)(-beta[i] * cos(omega[i] * (radius + 1.0)));
        iir->d1[i] = (float)(-2.0 * cos(omega[i]));
    }
}

/* Resolves the luma primaries for the configured YUV matrix and reports
 * whether the range is limited. The literals and the fallthrough structure
 * are those of ssimulacra2.c::picture_to_linear_rgb. */
static int ss2h_yuv_primaries(int yuv_matrix, float *kr, float *kg, float *kb)
{
    int limited = 1;
    switch (yuv_matrix) {
    case SS2H_MATRIX_BT709_FULL:
        limited = 0;
        // fallthrough
    case SS2H_MATRIX_BT709_LIMITED:
        *kr = 0.2126f;
        *kg = 0.7152f;
        *kb = 0.0722f;
        break;
    case SS2H_MATRIX_BT601_FULL:
        limited = 0;
        // fallthrough
    case SS2H_MATRIX_BT601_LIMITED:
    default:
        *kr = 0.299f;
        *kg = 0.587f;
        *kb = 0.114f;
        break;
    }
    return limited;
}

/* The constants of ssimulacra2.c::picture_to_linear_rgb, same expressions;
 * the device applies them per pixel. */
static struct Ss2hYuvCoefficients ss2h_yuv_coefficients(int yuv_matrix, unsigned bpc)
{
    float kr;
    float kg;
    float kb;
    const int limited = ss2h_yuv_primaries(yuv_matrix, &kr, &kg, &kb);
    const float peak = (float)((1u << bpc) - 1u);
    struct Ss2hYuvCoefficients k;
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

/* Plane geometry by picture.c's ceil rule. YUV 4:0:0 has no chroma planes to
 * convert; the CPU extractor would read planes that do not exist. */
static int ss2h_configure_planes(Ssimu2StateHip *s, enum VmafPixelFormat pix_fmt)
{
    if (pix_fmt == VMAF_PIX_FMT_YUV400P || pix_fmt == VMAF_PIX_FMT_UNKNOWN) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "ssimulacra2_hip: needs a YUV 4:2:0, 4:2:2 or 4:4:4 input\n");
        return -EINVAL;
    }
    const unsigned ss_hor = (pix_fmt != VMAF_PIX_FMT_YUV444P) ? 1u : 0u;
    const unsigned ss_ver = (pix_fmt == VMAF_PIX_FMT_YUV420P) ? 1u : 0u;
    const size_t bytes_per_sample = (s->bpc > 8u) ? 2u : 1u;
    for (unsigned p = 0; p < SS2H_CHANNELS; p++) {
        const unsigned sh = (p == 0u) ? 0u : ss_hor;
        const unsigned sv = (p == 0u) ? 0u : ss_ver;
        s->plane_w[p] = (s->width + sh) >> sh;
        s->plane_h[p] = (s->height + sv) >> sv;
        s->row_bytes[p] = (size_t)s->plane_w[p] * bytes_per_sample;
    }
    return 0;
}

static void ss2h_configure_scales(Ssimu2StateHip *s)
{
    s->scale_w[0] = s->width;
    s->scale_h[0] = s->height;
    for (int i = 1; i < SS2H_NUM_SCALES; i++) {
        s->scale_w[i] = (s->scale_w[i - 1] + 1u) / 2u;
        s->scale_h[i] = (s->scale_h[i - 1] + 1u) / 2u;
    }
    /* ssimulacra2.c::extract stops before the first scale below 8x8. */
    s->num_scales = 0;
    while (s->num_scales < SS2H_NUM_SCALES && s->scale_w[s->num_scales] >= 8u &&
           s->scale_h[s->num_scales] >= 8u)
        s->num_scales++;
}

/* Work-groups per channel for one scale's reduction. A function of the plane
 * size only, so the summation tree is the same on every device and the same
 * as ssimulacra2_sycl's (ss2s_reduce_groups). */
static unsigned ss2h_reduce_groups(size_t pixels)
{
    const size_t per_group = (size_t)SS2H_REDUCE_WG * SS2H_PIXELS_PER_ITEM;
    size_t groups = (pixels + per_group - 1u) / per_group;
    if (groups < 1u)
        groups = 1u;
    if (groups > SS2H_MAX_GROUPS)
        groups = SS2H_MAX_GROUPS;
    return (unsigned)groups;
}

/* ------------------------------------------------------------------ */
/* Buffers                                                             */
/* ------------------------------------------------------------------ */

static void ss2h_free_device(void **slot)
{
    if (*slot) {
        (void)hipFree(*slot);
        *slot = NULL;
    }
}

/* Null-guarded, so init's unwind and close share it after a partial
 * allocation. */
static void ss2h_free_device_buffers(Ssimu2StateHip *s)
{
    for (unsigned img = 0; img < SS2H_IMAGES; img++) {
        for (unsigned p = 0; p < SS2H_CHANNELS; p++)
            ss2h_free_device(&s->d_raw[img][p]);
        ss2h_free_device((void **)&s->d_lin[img][0]);
        ss2h_free_device((void **)&s->d_lin[img][1]);
        ss2h_free_device((void **)&s->d_xyb[img]);
    }
    float **const planes[] = {&s->d_scratch, &s->d_mu1, &s->d_mu2,      &s->d_s11,
                              &s->d_s22,     &s->d_s12, &s->d_partials, &s->d_totals};
    for (size_t i = 0; i < sizeof(planes) / sizeof(planes[0]); i++)
        ss2h_free_device((void **)planes[i]);
}

static void ss2h_free_pinned(void **slot)
{
    if (*slot) {
        (void)hipHostFree(*slot);
        *slot = NULL;
    }
}

static void ss2h_free_pinned_buffers(Ssimu2StateHip *s)
{
    for (unsigned img = 0; img < SS2H_IMAGES; img++) {
        for (unsigned p = 0; p < SS2H_CHANNELS; p++)
            ss2h_free_pinned(&s->h_raw[img][p]);
    }
    ss2h_free_pinned((void **)&s->h_totals);
}

/* hipMalloc into `*slot`; on failure every device buffer allocated so far is
 * released and the errno returned. */
static int ss2h_alloc_one_device(Ssimu2StateHip *s, void **slot, size_t bytes)
{
    const hipError_t hip_rc = hipMalloc(slot, bytes);
    if (hip_rc != hipSuccess) {
        *slot = NULL;
        ss2h_free_device_buffers(s);
        return ss2h_hip_rc(hip_rc);
    }
    return 0;
}

static int ss2h_alloc_device(Ssimu2StateHip *s)
{
    /* The ping-pong pyramid's second buffer holds scale 1 and smaller. */
    const size_t full = (size_t)SS2H_CHANNELS * s->width * s->height * sizeof(float);
    const size_t half = (size_t)SS2H_CHANNELS * s->scale_w[1] * s->scale_h[1] * sizeof(float);
    int err = 0;
    for (unsigned img = 0; img < SS2H_IMAGES && !err; img++) {
        for (unsigned p = 0; p < SS2H_CHANNELS && !err; p++) {
            err = ss2h_alloc_one_device(s, &s->d_raw[img][p], s->row_bytes[p] * s->plane_h[p]);
        }
        if (!err)
            err = ss2h_alloc_one_device(s, (void **)&s->d_lin[img][0], full);
        if (!err)
            err = ss2h_alloc_one_device(s, (void **)&s->d_lin[img][1], half);
        if (!err)
            err = ss2h_alloc_one_device(s, (void **)&s->d_xyb[img], full);
    }
    float **const planes[] = {&s->d_scratch, &s->d_mu1, &s->d_mu2, &s->d_s11, &s->d_s22, &s->d_s12};
    for (size_t i = 0; i < sizeof(planes) / sizeof(planes[0]) && !err; i++)
        err = ss2h_alloc_one_device(s, (void **)planes[i], full);
    const size_t partial_bytes =
        (size_t)SS2H_CHANNELS * SS2H_MAX_GROUPS * SS2H_LANE_FLOATS * sizeof(float);
    if (!err)
        err = ss2h_alloc_one_device(s, (void **)&s->d_partials, partial_bytes);
    if (!err)
        err = ss2h_alloc_one_device(s, (void **)&s->d_totals, SS2H_TOTAL_FLOATS * sizeof(float));
    return err;
}

/* hipHostMalloc into `*slot`; on failure every pinned buffer allocated so far
 * is released and the errno returned. */
static int ss2h_alloc_one_pinned(Ssimu2StateHip *s, void **slot, size_t bytes)
{
    const hipError_t hip_rc = hipHostMalloc(slot, bytes, 0);
    if (hip_rc != hipSuccess) {
        *slot = NULL;
        ss2h_free_pinned_buffers(s);
        return ss2h_hip_rc(hip_rc);
    }
    return 0;
}

static int ss2h_alloc_pinned(Ssimu2StateHip *s)
{
    int err = 0;
    for (unsigned img = 0; img < SS2H_IMAGES && !err; img++) {
        for (unsigned p = 0; p < SS2H_CHANNELS && !err; p++)
            err = ss2h_alloc_one_pinned(s, &s->h_raw[img][p], s->row_bytes[p] * s->plane_h[p]);
    }
    if (!err)
        err = ss2h_alloc_one_pinned(s, (void **)&s->h_totals, SS2H_TOTAL_FLOATS * sizeof(float));
    return err;
}

/* ------------------------------------------------------------------ */
/* init failure unwind — cascading tiers                               */
/*                                                                     */
/* Each tier undoes exactly its own acquisition and then delegates to  */
/* the next-earlier tier (HISS-01): buffers, then the module, then the */
/* stream.                                                             */
/* ------------------------------------------------------------------ */

static int ss2h_init_unwind_stream(Ssimu2StateHip *s, hipError_t rc)
{
    (void)hipStreamDestroy(s->str);
    s->str = NULL;
    return ss2h_hip_rc(rc);
}

/* Also frees any device and pinned allocation that succeeded before the
 * fault: ss2h_alloc_device / ss2h_alloc_pinned release their own partial
 * state, but when one of them succeeded and a later step failed, this tier
 * is the only cleanup. */
static int ss2h_init_unwind_module(Ssimu2StateHip *s, hipError_t rc)
{
    ss2h_free_pinned_buffers(s);
    ss2h_free_device_buffers(s);
    (void)hipModuleUnload(s->module);
    s->module = NULL;
    return ss2h_init_unwind_stream(s, rc);
}

/* Unwind after a failure that carries a negative errno of its own rather than
 * a hipError_t — ss2h_alloc_device() and ss2h_alloc_pinned() both report one.
 * Feeding hipSuccess to the ladder makes its terminal ss2h_hip_rc() return 0,
 * which would announce a successful init over a state whose every buffer,
 * module and stream has just been released
 * (T-HIP-INIT-UNWIND-REPORTS-SUCCESS-2026-09-22). So the ladder's result is
 * checked but the caller's errno is what propagates unless the ladder itself
 * reports a HIP failure. */
static int ss2h_init_unwind_alloc(Ssimu2StateHip *s, int err)
{
    const int unwind_err = ss2h_init_unwind_module(s, hipSuccess);
    return (unwind_err != 0) ? unwind_err : err;
}

typedef struct Ss2hKernelName {
    hipFunction_t *slot;
    const char *name;
} Ss2hKernelName;

/* Loads the HSACO module and resolves the eight kernel handles. On failure it
 * returns through the unwind tier of the last successful acquisition. */
static int ss2h_load_module(Ssimu2StateHip *s)
{
    hipError_t hip_rc = hipModuleLoadData(&s->module, ssimulacra2_device_hsaco);
    if (hip_rc != hipSuccess)
        return ss2h_init_unwind_stream(s, hip_rc);
    const Ss2hKernelName kernels[] = {
        {&s->func_yuv, "ssimulacra2_yuv_to_linear"},
        {&s->func_xyb, "ssimulacra2_xyb"},
        {&s->func_blur_rows, "ssimulacra2_blur_rows"},
        {&s->func_blur_rows_product, "ssimulacra2_blur_rows_product"},
        {&s->func_blur_cols, "ssimulacra2_blur_cols"},
        {&s->func_combine_partials, "ssimulacra2_combine_partials"},
        {&s->func_combine_final, "ssimulacra2_combine_final"},
        {&s->func_downsample, "ssimulacra2_downsample"},
    };
    for (size_t i = 0; i < sizeof(kernels) / sizeof(kernels[0]); i++) {
        hip_rc = hipModuleGetFunction(kernels[i].slot, s->module, kernels[i].name);
        if (hip_rc != hipSuccess)
            return ss2h_init_unwind_module(s, hip_rc);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Frame chain                                                         */
/* ------------------------------------------------------------------ */

static unsigned ss2h_blocks(size_t items, unsigned block)
{
    return (unsigned)((items + block - 1u) / block);
}

/* One kernel launch with a single by-value argument struct. */
static int ss2h_launch(const Ssimu2StateHip *s, hipFunction_t func, unsigned gx, unsigned gy,
                       unsigned bx, unsigned by, void *args)
{
    void *params[] = {args};
    const hipError_t hip_rc =
        hipModuleLaunchKernel(func, gx, gy, 1, bx, by, 1, 0, s->str, params, NULL);
    return ss2h_hip_rc(hip_rc);
}

static int ss2h_enqueue_linear_rgb(const Ssimu2StateHip *s)
{
    struct Ss2hYuvArgs args;
    (void)memset(&args, 0, sizeof(args));
    for (unsigned img = 0; img < SS2H_IMAGES; img++) {
        for (unsigned p = 0; p < SS2H_CHANNELS; p++)
            args.plane[img][p] = s->d_raw[img][p];
        args.out[img] = s->d_lin[img][0];
    }
    for (unsigned p = 0; p < SS2H_CHANNELS; p++) {
        args.plane_w[p] = s->plane_w[p];
        args.plane_h[p] = s->plane_h[p];
    }
    args.width = s->width;
    args.height = s->height;
    args.wide = (s->bpc > 8u) ? 1u : 0u;
    args.k = s->yuv;
    return ss2h_launch(s, s->func_yuv, ss2h_blocks(s->width, SS2H_PIX_BX),
                       ss2h_blocks(s->height, SS2H_PIX_BY), SS2H_PIX_BX, SS2H_PIX_BY, &args);
}

/* blur(in * in2) -> out through the scratch buffer (rows into scratch,
 * columns out), all three planes of the scale per launch; in2 == NULL blurs
 * `in` itself. The row pass forms the product while it loads (the single
 * fp32 multiply of ssimulacra2.c::multiply_3plane), so no product buffer is
 * written and read back. */
static int ss2h_blur(const Ssimu2StateHip *s, const float *in, const float *in2, float *out,
                     int scale)
{
    struct Ss2hBlurArgs args = {.in = in,
                                .in2 = in2,
                                .out = s->d_scratch,
                                .width = s->scale_w[scale],
                                .height = s->scale_h[scale],
                                .iir = s->iir};
    const hipFunction_t rows = (in2 != NULL) ? s->func_blur_rows_product : s->func_blur_rows;
    int err = ss2h_launch(s, rows, ss2h_blocks(args.height, SS2H_ROW_TILE), SS2H_CHANNELS,
                          SS2H_ROW_TILE, 1, &args);
    if (err)
        return err;
    args.in = s->d_scratch;
    args.in2 = NULL;
    args.out = out;
    err = ss2h_launch(s, s->func_blur_cols,
                      ss2h_blocks((size_t)args.width * SS2H_CHANNELS, SS2H_BLUR_BLOCK), 1,
                      SS2H_BLUR_BLOCK, 1, &args);
    return err;
}

/* ssimulacra2.c::extract order: s11, s22, s12, mu1, mu2. */
static int ss2h_enqueue_blurs(const Ssimu2StateHip *s, int scale)
{
    const float *ref = s->d_xyb[0];
    const float *dis = s->d_xyb[1];
    int err = ss2h_blur(s, ref, ref, s->d_s11, scale);
    if (!err)
        err = ss2h_blur(s, dis, dis, s->d_s22, scale);
    if (!err)
        err = ss2h_blur(s, ref, dis, s->d_s12, scale);
    if (!err)
        err = ss2h_blur(s, ref, NULL, s->d_mu1, scale);
    if (!err)
        err = ss2h_blur(s, dis, NULL, s->d_mu2, scale);
    return err;
}

/* The six sums per channel of one scale into d_totals. */
static int ss2h_enqueue_sums(const Ssimu2StateHip *s, int scale)
{
    const size_t plane = (size_t)s->scale_w[scale] * s->scale_h[scale];
    struct Ss2hCombineArgs combine = {.mu1 = s->d_mu1,
                                      .mu2 = s->d_mu2,
                                      .s11 = s->d_s11,
                                      .s22 = s->d_s22,
                                      .s12 = s->d_s12,
                                      .img1 = s->d_xyb[0],
                                      .img2 = s->d_xyb[1],
                                      .partials = s->d_partials,
                                      .plane = plane,
                                      .groups = ss2h_reduce_groups(plane)};
    int err = ss2h_launch(s, s->func_combine_partials, combine.groups, SS2H_CHANNELS,
                          SS2H_REDUCE_WG, 1, &combine);
    if (err)
        return err;
    struct Ss2hFinalArgs final = {.partials = s->d_partials,
                                  .totals = s->d_totals +
                                            (size_t)scale * SS2H_CHANNELS * SS2H_LANE_FLOATS,
                                  .groups = combine.groups};
    return ss2h_launch(s, s->func_combine_final, SS2H_CHANNELS, 1, SS2H_REDUCE_WG, 1, &final);
}

static int ss2h_enqueue_scale(const Ssimu2StateHip *s, int scale)
{
    const unsigned cw = s->scale_w[scale];
    const unsigned ch = s->scale_h[scale];
    const unsigned cur = (unsigned)scale & 1u;
    struct Ss2hPlanesArgs xyb = {.in = {s->d_lin[0][cur], s->d_lin[1][cur]},
                                 .out = {s->d_xyb[0], s->d_xyb[1]},
                                 .width = cw,
                                 .height = ch,
                                 .out_width = cw,
                                 .out_height = ch};
    int err = ss2h_launch(s, s->func_xyb, ss2h_blocks((size_t)cw * ch, SS2H_ELEM_BLOCK), 1,
                          SS2H_ELEM_BLOCK, 1, &xyb);
    if (!err)
        err = ss2h_enqueue_blurs(s, scale);
    if (!err)
        err = ss2h_enqueue_sums(s, scale);
    if (err || scale + 1 >= s->num_scales)
        return err;
    struct Ss2hPlanesArgs down = {.in = {s->d_lin[0][cur], s->d_lin[1][cur]},
                                  .out = {s->d_lin[0][cur ^ 1u], s->d_lin[1][cur ^ 1u]},
                                  .width = cw,
                                  .height = ch,
                                  .out_width = s->scale_w[scale + 1],
                                  .out_height = s->scale_h[scale + 1]};
    return ss2h_launch(s, s->func_downsample, ss2h_blocks(down.out_width, SS2H_PIX_BX),
                       ss2h_blocks(down.out_height, SS2H_PIX_BY), SS2H_PIX_BX, SS2H_PIX_BY, &down);
}

/* The frame's whole device chain, from the upload of the staged planes to
 * the copy of the per-scale sums into h_totals. */
static int ss2h_enqueue_frame(const Ssimu2StateHip *s)
{
    for (unsigned img = 0; img < SS2H_IMAGES; img++) {
        for (unsigned p = 0; p < SS2H_CHANNELS; p++) {
            const hipError_t hip_rc =
                hipMemcpyAsync(s->d_raw[img][p], s->h_raw[img][p], s->row_bytes[p] * s->plane_h[p],
                               hipMemcpyHostToDevice, s->str);
            if (hip_rc != hipSuccess)
                return ss2h_hip_rc(hip_rc);
        }
    }
    int err = ss2h_enqueue_linear_rgb(s);
    for (int scale = 0; scale < s->num_scales && !err; scale++)
        err = ss2h_enqueue_scale(s, scale);
    if (err)
        return err;
    const hipError_t hip_rc = hipMemcpyAsync(
        s->h_totals, s->d_totals, SS2H_TOTAL_FLOATS * sizeof(float), hipMemcpyDeviceToHost, s->str);
    return ss2h_hip_rc(hip_rc);
}

/* ------------------------------------------------------------------ */
/* Host: norms and pooling (fp64, after the one readback)              */
/* ------------------------------------------------------------------ */

/* The six sums of one scale -> the ssim_map / edge_diff_map plane averages. */
static void ss2h_scale_norms(const float *totals, unsigned cw, unsigned ch, double avg_ssim[6],
                             double avg_ed[12])
{
    const double one_per_pixels = 1.0 / (double)((size_t)cw * (size_t)ch);
    for (unsigned c = 0; c < SS2H_CHANNELS; c++) {
        double sum[SS2H_SUMS];
        for (unsigned k = 0; k < SS2H_SUMS; k++) {
            const float *pair = totals + (size_t)c * SS2H_LANE_FLOATS + (size_t)k * SS2H_PAIR;
            sum[k] = (double)pair[0] + (double)pair[1];
        }
        avg_ssim[c * 2 + 0] = one_per_pixels * sum[0];
        avg_ssim[c * 2 + 1] = sqrt(sqrt(one_per_pixels * sum[1]));
        avg_ed[c * 4 + 0] = one_per_pixels * sum[2];
        avg_ed[c * 4 + 1] = sqrt(sqrt(one_per_pixels * sum[3]));
        avg_ed[c * 4 + 2] = one_per_pixels * sum[4];
        avg_ed[c * 4 + 3] = sqrt(sqrt(one_per_pixels * sum[5]));
    }
}

/* Verbatim port of ssimulacra2.c::pool_score. */
static double ss2h_pool_score(const double avg_ssim[6][6], const double avg_ed[6][12],
                              int num_scales)
{
    double ssim = 0.0;
    size_t i = 0;
    for (int c = 0; c < 3; c++) {
        for (int scale = 0; scale < 6; scale++) {
            for (int n = 0; n < 2; n++) {
                const double s_term = scale < num_scales ? avg_ssim[scale][c * 2 + n] : 0.0;
                const double r_term = scale < num_scales ? avg_ed[scale][c * 4 + n] : 0.0;
                const double b_term = scale < num_scales ? avg_ed[scale][c * 4 + n + 2] : 0.0;
                ssim += g_weights[i++] * fabs(s_term);
                ssim += g_weights[i++] * fabs(r_term);
                ssim += g_weights[i++] * fabs(b_term);
            }
        }
    }
    ssim *= 0.9562382616834844;
    ssim = 2.326765642916932 * ssim - 0.020884521182843837 * ssim * ssim +
           6.248496625763138e-05 * ssim * ssim * ssim;
    return vmaf_ss2_finalize_score(ssim);
}

static double ss2h_frame_score(const Ssimu2StateHip *s)
{
    double avg_ssim[6][6] = {{0}};
    double avg_ed[6][12] = {{0}};
    for (int scale = 0; scale < s->num_scales; scale++) {
        ss2h_scale_norms(s->h_totals + (size_t)scale * SS2H_CHANNELS * SS2H_LANE_FLOATS,
                         s->scale_w[scale], s->scale_h[scale], avg_ssim[scale], avg_ed[scale]);
    }
    return ss2h_pool_score((const double (*)[6])avg_ssim, (const double (*)[12])avg_ed,
                           s->num_scales);
}

/* ------------------------------------------------------------------ */
/* Picture staging                                                     */
/* ------------------------------------------------------------------ */

static bool ss2h_picture_matches(const Ssimu2StateHip *s, const VmafPicture *pic)
{
    if (!pic || pic->bpc != s->bpc)
        return false;
    for (unsigned p = 0; p < SS2H_CHANNELS; p++) {
        if (!pic->data[p] || pic->w[p] != s->plane_w[p] || pic->h[p] != s->plane_h[p])
            return false;
    }
    return true;
}

/* Pack one plane's rows into pinned staging. The picture goes back to the
 * caller when submit() returns, so the device copy must not read it
 * (core/src/feature/hip/AGENTS.md, "Picture uploads"); the staging buffer is
 * the extractor's until the next submit(), which follows collect(). */
static void ss2h_stage_plane(const VmafPicture *pic, unsigned p, void *dst, size_t row_bytes,
                             unsigned rows)
{
    const uint8_t *src = (const uint8_t *)pic->data[p];
    uint8_t *out = (uint8_t *)dst;
    const size_t stride = (size_t)pic->stride[p];
    if (stride == row_bytes) {
        (void)memcpy(out, src, row_bytes * rows);
        return;
    }
    for (unsigned i = 0; i < rows; i++)
        (void)memcpy(out + (size_t)i * row_bytes, src + (size_t)i * stride, row_bytes);
}

#endif /* HAVE_HIPCC */

/* ------------------------------------------------------------------ */
/* init / submit / collect / close                                     */
/* ------------------------------------------------------------------ */

static int init_fex_hip(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                        unsigned w, unsigned h)
{
#ifndef HAVE_HIPCC
    (void)fex;
    (void)pix_fmt;
    (void)bpc;
    (void)w;
    (void)h;
    return -ENOSYS;
#else
    Ssimu2StateHip *s = fex->priv;

    if (w < 8u || h < 8u) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "ssimulacra2_hip: input %ux%u below 8x8 lower bound\n", w,
                 h);
        return -EINVAL;
    }

    s->width = w;
    s->height = h;
    s->bpc = bpc;
    const int plane_err = ss2h_configure_planes(s, pix_fmt);
    if (plane_err)
        return plane_err;
    ss2h_configure_scales(s);
    ss2h_setup_gaussian(&s->iir, SS2H_SIGMA);
    /* The row pass finds every left input of a tile in the tile before it
     * (ssimulacra2_blur_rows); sigma 1.5 gives radius 5. */
    if (2 * s->iir.radius > (int)SS2H_ROW_COLS) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "ssimulacra2_hip: blur radius %d above %d\n", s->iir.radius,
                 SS2H_ROW_COLS / 2);
        return -EINVAL;
    }
    s->yuv = ss2h_yuv_coefficients(s->yuv_matrix, bpc);
    s->has_pending = false;

    const hipError_t hip_rc = hipStreamCreateWithFlags(&s->str, hipStreamNonBlocking);
    if (hip_rc != hipSuccess)
        return ss2h_hip_rc(hip_rc);

    const int mod_err = ss2h_load_module(s);
    if (mod_err)
        return mod_err;

    /* Both allocators return a negative errno, never a hipError_t; route it
   * through ss2h_init_unwind_alloc() so the failure keeps its error code. */
    int ret = ss2h_alloc_device(s);
    if (ret)
        return ss2h_init_unwind_alloc(s, ret);
    ret = ss2h_alloc_pinned(s);
    if (ret)
        return ss2h_init_unwind_alloc(s, ret);
    return 0;
#endif
}

static int submit_fex_hip(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                          VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic_90;
    (void)dist_pic_90;
#ifndef HAVE_HIPCC
    (void)fex;
    (void)ref_pic;
    (void)dist_pic;
    (void)index;
    return -ENOSYS;
#else
    Ssimu2StateHip *s = fex->priv;
    if (!ss2h_picture_matches(s, ref_pic) || !ss2h_picture_matches(s, dist_pic))
        return -EINVAL;
    const VmafPicture *const pics[SS2H_IMAGES] = {ref_pic, dist_pic};
    for (unsigned img = 0; img < SS2H_IMAGES; img++) {
        for (unsigned p = 0; p < SS2H_CHANNELS; p++)
            ss2h_stage_plane(pics[img], p, s->h_raw[img][p], s->row_bytes[p], s->plane_h[p]);
    }
    const int err = ss2h_enqueue_frame(s);
    if (err)
        return err;
    s->pending_index = index;
    s->has_pending = true;
    return 0;
#endif
}

static int collect_fex_hip(VmafFeatureExtractor *fex, unsigned index,
                           VmafFeatureCollector *feature_collector)
{
#ifndef HAVE_HIPCC
    (void)fex;
    (void)index;
    (void)feature_collector;
    return -ENOSYS;
#else
    Ssimu2StateHip *s = fex->priv;
    if (!s->has_pending || s->pending_index != index)
        return -EINVAL;
    /* The one host wait per frame: h_totals is written by the last copy of
   * the chain submit() enqueued. */
    const hipError_t hip_rc = hipStreamSynchronize(s->str);
    s->has_pending = false;
    if (hip_rc != hipSuccess)
        return ss2h_hip_rc(hip_rc);
    const double score = ss2h_frame_score(s);
    if (!vmaf_ss2_score_is_finite(score)) {
        vmaf_log(VMAF_LOG_LEVEL_WARNING,
                 "ssimulacra2_hip: non-finite score at frame %u (score=%g), "
                 "failing frame\n",
                 index, score);
        return -EINVAL;
    }
    return vmaf_feature_collector_append(feature_collector, "ssimulacra2", score, index);
#endif
}

static int close_fex_hip(VmafFeatureExtractor *fex)
{
    Ssimu2StateHip *s = fex->priv;
    if (!s)
        return 0;
#ifdef HAVE_HIPCC
    if (s->str)
        (void)hipStreamSynchronize(s->str);
    if (s->module)
        (void)hipModuleUnload(s->module);
    if (s->str)
        (void)hipStreamDestroy(s->str);
    s->module = NULL;
    s->str = NULL;

    /* Shared with init's unwind tiers: null-guarded, so a partial init and a
   * full one release through the same sequence. */
    ss2h_free_device_buffers(s);
    ss2h_free_pinned_buffers(s);
#endif
    return 0;
}

static const char *provided_features[] = {"ssimulacra2", NULL};

/* Load-bearing: registered in feature_extractor.cpp's feature_extractor_list[].
 * Making this static would unlink the extractor from the registry. Same
 * pattern as every other HIP consumer (see integer_psnr_hip.c). */
// NOLINTNEXTLINE(misc-use-internal-linkage): cross-TU registry pattern — external linkage required (ADR-0278).
VmafFeatureExtractor vmaf_fex_ssimulacra2_hip = {
    .name = "ssimulacra2_hip",
    .init = init_fex_hip,
    .submit = submit_fex_hip,
    .collect = collect_fex_hip,
    .close = close_fex_hip,
    .options = options,
    .priv_size = sizeof(Ssimu2StateHip),
    .flags = VMAF_FEATURE_EXTRACTOR_HIP,
    .provided_features = provided_features,
    .context_check = check_context_hip,
    .context_fallback_name = "ssimulacra2",
};

/* NOLINTEND(modernize-use-nullptr) */
