/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * T-GPU-TWIN-PARITY-GAPS-OUTSIDE-CUDA-2026-09-30 items (1) and (4) and
 * T-GPU-FLOAT-MS-SSIM-CPU-ARITHMETIC-2026-10-01 (ADR-1498): the window
 * arithmetic of float_ssim_metal and float_ms_ssim_metal, compiled on the
 * host, against iqa/ssim_tools.c.
 *
 * feature/metal/metal_ssim_terms.h is the kernels' arithmetic, valid as Metal
 * Shading Language and as C: the two eleven-tap passes as exact fp32 pairs,
 * the CPU's fp32 window values and its two fp64 quotients, which Metal forms
 * on values held in 64-bit integers (metal_soft_signed.h; Metal has no fp64
 * type). The test checks, with reference_terms() holding
 * ssim_variance_scalar() and ssim_accumulate_default_scalar() verbatim:
 *
 *   - the terms of random windows (flat, identical, textured, zero variance,
 *     negative covariance, tiny and large values): lv, cv and `lv * cv * sv`
 *     bit for bit and sv as the fp32 quotient;
 *   - the whole float_ssim twin on the host: both passes written with the
 *     header's functions, the stored terms added in raster order as
 *     float_ssim_metal.mm adds them, the mean rounded to fp32 as iqa_ssim()
 *     returns it, against compute_ssim() at `==` for score, l, c and s, on
 *     the frames of test_metal_float_ssim_parity (8 to 12 bits, odd frames,
 *     the order frame, seeded noise, 960x540, identical flat frames);
 *   - identical flat 64x64 frames with enable_db score 72.247198959355487 dB
 *     on the CPU, not +inf: the twin has no forced 1 on a flat window.
 *
 * test_metal_float_ssim_exact_contract.py pins the kernel and host layout.
 * What this cannot show: that the header compiles as MSL and returns these
 * bits on an Apple GPU (test_metal_float_ssim_parity on the tester's device).
 */

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"

#include "float_ssim_order_frame.h"
#include "ssim_order_noise.h"

#include "feature/metal/metal_ssim_terms.h"
#include "feature/picture_copy.h"
#include "feature/ssim.h"
#include "libvmaf/picture.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy proposes `nullptr`, but the required MSVC C build does
 * not provide that keyword. Preserve the portable C spelling. ADR-1138. */

enum {
    TERM_SAMPLES = 200000,
    WINDOW_KINDS = 6,
    HEAVY_W = 960,
    HEAVY_H = 540,
};

/* FLAT_DB_CPU: float_ssim of identical flat frames with enable_db on the CPU. */
#define FLAT_DB_CPU 72.247198959355487

/* ------------------------------------------------------------------ */
/* The reference: iqa/ssim_tools.c, verbatim                            */
/* ------------------------------------------------------------------ */

#define MAX(x, y) (((x) > (y)) ? (x) : (y))

/* One window's ssim_moments as the CPU holds them after both convolution
 * passes. */
typedef struct window_moments {
    float ref_mu;
    float cmp_mu;
    float ref_sq;
    float cmp_sq;
    float both;
} window_moments;

/* ssim_variance_scalar() and ssim_accumulate_default_scalar() for one pixel;
 * `ssim` is the term the reference adds to *ssim_sum. */
static void reference_terms(window_moments m, float C1, float C2, double *l_out, double *c_out,
                            double *s_out, double *ssim_out)
{
    const float C3 = C2 / 2.0f;
    float ref_sigma_sqd = m.ref_sq;
    float cmp_sigma_sqd = m.cmp_sq;
    float sigma_both = m.both;
    ref_sigma_sqd -= m.ref_mu * m.ref_mu;
    cmp_sigma_sqd -= m.cmp_mu * m.cmp_mu;
    ref_sigma_sqd = MAX(0.0, ref_sigma_sqd);
    cmp_sigma_sqd = MAX(0.0, cmp_sigma_sqd);
    sigma_both -= m.ref_mu * m.cmp_mu;

    const float sigma_ref_sigma_cmp = sqrtf(ref_sigma_sqd * cmp_sigma_sqd);
    const double l =
        (2.0 * m.ref_mu * m.cmp_mu + C1) / (m.ref_mu * m.ref_mu + m.cmp_mu * m.cmp_mu + C1);
    const double c = (2.0 * sigma_ref_sigma_cmp + C2) / (ref_sigma_sqd + cmp_sigma_sqd + C2);
    const float clamped_sigma_both =
        (sigma_both < 0.0f && sigma_ref_sigma_cmp <= 0.0f) ? 0.0f : sigma_both;
    const double s = (clamped_sigma_both + C3) / (sigma_ref_sigma_cmp + C3);
    *l_out = l;
    *c_out = c;
    *s_out = s;
    *ssim_out = l * c * s;
}

static uint64_t bits_of(double value)
{
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

/* Equal bits, or two zeros (metal_soft_signed.h keeps no sign on a zero). */
static int same_double(uint64_t got, double want)
{
    return got == bits_of(want) || (want == 0.0 && vmaf_mtl_ssim_double_of(got) == 0.0);
}

/* ------------------------------------------------------------------ */
/* Deterministic windows                                                */
/* ------------------------------------------------------------------ */

static uint64_t rng_state = UINT64_C(0x9e3779b97f4a7c15);

static uint64_t rng_next(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

/* A float in [0, scale). */
static float rng_float(float scale)
{
    return (float)(rng_next() >> 40) / 16777216.0f * scale;
}

/* A random window's moments. `kind`: 0 random, 1 flat identical, 2 flat
 * identical with the rounding of the squares, 3 zero comparison variance, 4
 * negative covariance, 5 a tiny window. */
static window_moments random_window(unsigned kind)
{
    window_moments m;
    m.ref_mu = rng_float(255.0f);
    m.cmp_mu = kind == 1u || kind == 2u ? m.ref_mu : rng_float(255.0f);
    m.ref_sq = m.ref_mu * m.ref_mu + rng_float(4000.0f);
    m.cmp_sq = m.cmp_mu * m.cmp_mu + rng_float(4000.0f);
    m.both = m.ref_mu * m.cmp_mu + rng_float(3000.0f) - 1000.0f;
    if (kind == 1u) {
        m.ref_sq = m.ref_mu * m.ref_mu;
        m.cmp_sq = m.ref_sq;
        m.both = m.ref_sq;
    }
    if (kind == 2u) {
        m.ref_sq = m.ref_mu * m.ref_mu - rng_float(0.01f);
        m.cmp_sq = m.ref_sq;
        m.both = m.ref_sq;
    }
    if (kind == 3u) {
        m.cmp_sq = m.cmp_mu * m.cmp_mu;
    }
    if (kind == 4u) {
        m.both = m.ref_mu * m.cmp_mu - rng_float(2000.0f);
    }
    if (kind == 5u) {
        m.ref_mu = rng_float(0.5f);
        m.cmp_mu = rng_float(0.5f);
        m.ref_sq = m.ref_mu * m.ref_mu + rng_float(0.01f);
        m.cmp_sq = m.cmp_mu * m.cmp_mu + rng_float(0.01f);
        m.both = m.ref_mu * m.cmp_mu;
    }
    return m;
}

static void report_term(unsigned i, window_moments m, double l, double c, double s, double ssim,
                        const VmafMtlSsimDoubleTerms *got, uint64_t got_bits)
{
    (void)fprintf(stderr,
                  "\nwindow %u mu=(%.9g %.9g) sq=(%.9g %.9g) both=%.9g\n"
                  "  l  want %.17g got %.17g\n  c  want %.17g got %.17g\n"
                  "  s  want %.17g got %.17g\n  l*c*s want %.17g got %.17g\n",
                  i, m.ref_mu, m.cmp_mu, m.ref_sq, m.cmp_sq, m.both, l,
                  vmaf_mtl_ssim_double_of(vmaf_mtl_signed_bits(got->luminance)), c,
                  vmaf_mtl_ssim_double_of(vmaf_mtl_signed_bits(got->contrast)), s,
                  (double)got->structure, ssim, vmaf_mtl_ssim_double_of(got_bits));
}

static char *test_terms_are_the_cpu_expressions(void)
{
    const VmafMtlSsimConstants k = vmaf_mtl_ssim_constants();
    unsigned bad = 0u;
    for (unsigned i = 0u; i < TERM_SAMPLES; i++) {
        const window_moments m = random_window(i % WINDOW_KINDS);
        double l;
        double c;
        double s;
        double ssim;
        reference_terms(m, k.c1, k.c2, &l, &c, &s, &ssim);
        const VmafMtlSsimMoments in =
            vmaf_mtl_ssim_moments_make(m.ref_mu, m.cmp_mu, m.ref_sq, m.cmp_sq, m.both);
        /* The kernel receives the moments the passes made, and forms the
         * variances from them, as the CPU does after its two passes. */
        const VmafMtlSsimDoubleTerms got =
            vmaf_mtl_ssim_double_terms(vmaf_mtl_ssim_float_parts(in, k.c1, k.c2), k.c1, k.c2);
        const uint64_t got_bits = vmaf_mtl_ssim_product_bits(got);
        const int ok = same_double(vmaf_mtl_signed_bits(got.luminance), l) &&
                       same_double(vmaf_mtl_signed_bits(got.contrast), c) &&
                       (double)got.structure == (double)(float)s && (double)got.structure == s &&
                       same_double(got_bits, ssim);
        if (!ok && bad++ < 5u) {
            report_term(i, m, l, c, s, ssim, &got, got_bits);
        }
    }
    mu_assert("a window term differs from the CPU's expression", bad == 0u);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* The whole twin on the host                                           */
/* ------------------------------------------------------------------ */

typedef enum Source { SRC_TEXTURE, SRC_FLAT, SRC_HEADER, SRC_NOISE, SRC_SAME } Source;

typedef struct Case {
    const char *what;
    Source src;
    unsigned w;
    unsigned h;
    unsigned bpc;
    unsigned flat;
    uint64_t seed;
    double expect_db; /* the CPU's enable_db score, or 0 = not asserted */
} Case;

static unsigned texture_at(unsigned row, unsigned col, unsigned bpc, unsigned salt)
{
    const unsigned max = (1u << bpc) - 1u;
    const unsigned base = (((row ^ col) * 3u + row / 3u) << (bpc - 8u)) & max;
    if (!salt)
        return base;
    const unsigned hash = (row * 2654435761u) ^ (col * 40503u) ^ (salt * 97u);
    const int noise = (int)((hash >> 7) % 33u) - 16;
    const int value = (int)base + noise * (int)(1u << (bpc - 8u));
    return value < 0 ? 0u : ((unsigned)value > max ? max : (unsigned)value);
}

static unsigned sample_of(const Case *c, unsigned side, unsigned row, unsigned col)
{
    const size_t i = (size_t)row * c->w + col;
    switch (c->src) {
    case SRC_FLAT:
        return c->flat;
    case SRC_HEADER:
        return side ? float_ssim_order_frame_dis[i] : float_ssim_order_frame_ref[i];
    case SRC_NOISE:
        return ssim_order_noise_luma(c->seed, side, i);
    case SRC_SAME:
        return texture_at(row, col, c->bpc, 0u);
    default:
        return texture_at(row, col, c->bpc, side ? 1u : 0u);
    }
}

/* One luma plane as picture_copy() makes it: the CPU's own normalisation. */
static int float_plane(const Case *c, unsigned side, float *dst)
{
    VmafPicture pic;
    if (vmaf_picture_alloc(&pic, VMAF_PIX_FMT_YUV400P, c->bpc, c->w, c->h))
        return -1;
    for (unsigned row = 0u; row < c->h; row++) {
        uint8_t *line = (uint8_t *)pic.data[0] + (size_t)row * pic.stride[0];
        for (unsigned col = 0u; col < c->w; col++) {
            const unsigned v = sample_of(c, side, row, col);
            if (c->bpc > 8u)
                ((uint16_t *)line)[col] = (uint16_t)v;
            else
                line[col] = (uint8_t)v;
        }
    }
    picture_copy(dst, (ptrdiff_t)((size_t)c->w * sizeof(float)), &pic, 0, pic.bpc, 0);
    return vmaf_picture_unref(&pic);
}

/* Pass 0 of float_ssim.metal for every output: the horizontal moments of the
 * (w - 10) x h plane. */
static void twin_horizontal(const float *ref, const float *cmp, unsigned w, unsigned h,
                            VmafMtlSsimMoments *rows)
{
    const unsigned w_h = w - 10u;
    for (unsigned y = 0u; y < h; y++) {
        for (unsigned x = 0u; x < w_h; x++) {
            VmafMtlSsimPairs sums = vmaf_mtl_ssim_pairs_zero();
            for (int tap = 0; tap < VMAF_MTL_SSIM_TAPS; ++tap) {
                const size_t index = (size_t)y * w + x + (size_t)tap;
                sums = vmaf_mtl_ssim_add_horizontal_tap(sums, ref[index], cmp[index],
                                                        vmaf_mtl_ssim_gauss[tap]);
            }
            rows[(size_t)y * w_h + x] = vmaf_mtl_ssim_round_moments(sums);
        }
    }
}

/* Pass 1 of float_ssim.metal for the window at (x, y). */
static VmafMtlSsimDoubleTerms twin_window(const VmafMtlSsimMoments *rows, VmafMtlSsimWindowParams p,
                                          unsigned x, unsigned y)
{
    VmafMtlSsimPairs sums = vmaf_mtl_ssim_pairs_zero();
    for (int tap = 0; tap < VMAF_MTL_SSIM_TAPS; ++tap) {
        const size_t index = (size_t)(y + (unsigned)tap) * p.horizontal_width + x;
        sums = vmaf_mtl_ssim_add_vertical_tap(sums, rows[index], vmaf_mtl_ssim_gauss[tap]);
    }
    return vmaf_mtl_ssim_double_terms(
        vmaf_mtl_ssim_float_parts(vmaf_mtl_ssim_round_moments(sums), p.c1, p.c2), p.c1, p.c2);
}

/* float_ssim_metal.mm::frame_mean(): `(float)(sum / n)`. */
static double frame_mean(double sum, double n)
{
    return (double)(float)(sum / n);
}

/* The twin's four frame means: the stored terms added in raster order, the
 * product bits (enable_lcs off) or lv, cv and sv (on). */
static int twin_means(const float *ref, const float *cmp, unsigned w, unsigned h, int lcs,
                      double means[4])
{
    const unsigned w_h = w - 10u;
    const unsigned h_v = h - 10u;
    const size_t windows = (size_t)w_h * h_v;
    VmafMtlSsimMoments *rows = (VmafMtlSsimMoments *)malloc((size_t)w_h * h * sizeof(*rows));
    uint64_t *term = (uint64_t *)malloc(windows * sizeof(uint64_t));
    uint64_t *contrast = (uint64_t *)malloc(windows * sizeof(uint64_t));
    float *structure = (float *)malloc(windows * sizeof(float));
    if (!rows || !term || !contrast || !structure) {
        free(rows);
        free(term);
        free(contrast);
        free(structure);
        return -1;
    }
    const VmafMtlSsimConstants k = vmaf_mtl_ssim_constants();
    const VmafMtlSsimWindowParams p = {w_h, h, w_h, h_v, 0u, 0u, k.c1, k.c2};
    twin_horizontal(ref, cmp, w, h, rows);
    for (unsigned y = 0u; y < h_v; y++) {
        for (unsigned x = 0u; x < w_h; x++) {
            const VmafMtlSsimDoubleTerms t = twin_window(rows, p, x, y);
            const size_t i = (size_t)y * w_h + x;
            term[i] = lcs ? vmaf_mtl_signed_bits(t.luminance) : vmaf_mtl_ssim_product_bits(t);
            contrast[i] = vmaf_mtl_signed_bits(t.contrast);
            structure[i] = t.structure;
        }
    }
    const double n = (double)w_h * (double)h_v;
    if (lcs) {
        const VmafMtlSsimFrameSums sums =
            vmaf_mtl_ssim_frame_sums(term, contrast, structure, windows);
        means[0] = frame_mean(sums.ssim, n);
        means[1] = frame_mean(sums.luminance, n);
        means[2] = frame_mean(sums.contrast, n);
        means[3] = frame_mean(sums.structure, n);
    } else {
        means[0] = frame_mean(vmaf_mtl_ssim_product_sum(term, windows), n);
    }
    free(rows);
    free(term);
    free(contrast);
    free(structure);
    return 0;
}

/* compute_ssim()'s -10 * log10(1 - ssim), vmaf_ssim_prepare_score(). */
static double to_db(double ssim)
{
    return ssim >= 1.0 ? INFINITY : -10.0 * log10(1.0 - ssim);
}

static void report_frame(const Case *c, int lcs, const double cpu[4], const double twin[4])
{
    (void)fprintf(
        stderr, "\n%s %ux%u %u-bit lcs=%d: cpu=(%.9g %.9g %.9g %.9g) twin=(%.9g %.9g %.9g %.9g)\n",
        c->what, c->w, c->h, c->bpc, lcs, cpu[0], cpu[1], cpu[2], cpu[3], twin[0], twin[1], twin[2],
        twin[3]);
}

/* One frame: the CPU's means (scale 1) and the twin's, with and without
 * enable_lcs, at `==`. */
static char *check_frame(const Case *c)
{
    const size_t pixels = (size_t)c->w * c->h;
    float *ref = (float *)malloc(pixels * sizeof(float));
    float *cmp = (float *)malloc(pixels * sizeof(float));
    mu_assert("allocation failed", ref && cmp);
    mu_assert("the reference plane failed", float_plane(c, 0u, ref) == 0);
    mu_assert("the distorted plane failed", float_plane(c, 1u, cmp) == 0);

    double cpu[4];
    const int stride = (int)(c->w * sizeof(float));
    const int err = compute_ssim(ref, cmp, (int)c->w, (int)c->h, stride, stride, &cpu[0], &cpu[1],
                                 &cpu[2], &cpu[3], 1);
    double plain[4] = {0.0, 0.0, 0.0, 0.0};
    double lcs[4] = {0.0, 0.0, 0.0, 0.0};
    const int plain_err = twin_means(ref, cmp, c->w, c->h, 0, plain);
    const int lcs_err = twin_means(ref, cmp, c->w, c->h, 1, lcs);
    free(ref);
    free(cmp);
    mu_assert("compute_ssim failed", err == 0);
    mu_assert("the twin emulation failed", plain_err == 0 && lcs_err == 0);
    if (plain[0] != cpu[0]) {
        report_frame(c, 0, cpu, plain);
    }
    if (lcs[0] != cpu[0] || lcs[1] != cpu[1] || lcs[2] != cpu[2] || lcs[3] != cpu[3]) {
        report_frame(c, 1, cpu, lcs);
    }
    mu_assert("float_ssim differs from the CPU", plain[0] == cpu[0]);
    mu_assert("float_ssim under enable_lcs differs from the CPU", lcs[0] == cpu[0]);
    mu_assert("float_ssim_l differs from the CPU", lcs[1] == cpu[1]);
    mu_assert("float_ssim_c differs from the CPU", lcs[2] == cpu[2]);
    mu_assert("float_ssim_s differs from the CPU", lcs[3] == cpu[3]);
    if (c->expect_db != 0.0) {
        const double db = to_db(plain[0]);
        if (db != c->expect_db) {
            (void)fprintf(stderr, "\n%s: enable_db twin %.17g cpu %.17g\n", c->what, db,
                          c->expect_db);
        }
        mu_assert("enable_db on a flat identical frame is not the CPU's value",
                  db == c->expect_db && to_db(cpu[0]) == c->expect_db);
        mu_assert("a flat identical frame must not score exactly 1", plain[0] < 1.0);
    }
    return NULL;
}

static char *check_all(const Case *cases, size_t n)
{
    for (size_t i = 0u; i < n; i++) {
        char *msg = check_frame(&cases[i]);
        if (msg)
            return msg;
    }
    return NULL;
}

static char *test_twin_frames_are_the_cpu_scores(void)
{
    static const Case cases[] = {
        {"texture", SRC_TEXTURE, 256u, 144u, 8u, 0u, 0u, 0.0},
        {"texture", SRC_TEXTURE, 256u, 144u, 10u, 0u, 0u, 0.0},
        {"texture", SRC_TEXTURE, 256u, 144u, 12u, 0u, 0u, 0.0},
        {"texture", SRC_TEXTURE, 128u, 96u, 16u, 0u, 0u, 0.0},
        {"odd frame", SRC_TEXTURE, 323u, 181u, 8u, 0u, 0u, 0.0},
        {"minimal 11x11", SRC_TEXTURE, 11u, 11u, 8u, 0u, 0u, 0.0},
        {"order frame", SRC_HEADER, FLOAT_SSIM_ORDER_FRAME_W, FLOAT_SSIM_ORDER_FRAME_H, 8u, 0u, 0u,
         0.0},
        {"noise 64x64 a", SRC_NOISE, 64u, 64u, 8u, 0u, 17217594u, 0.0},
        {"noise 64x64 b", SRC_NOISE, 64u, 64u, 8u, 0u, 19119610u, 0.0},
        {"noise 176x176", SRC_NOISE, 176u, 176u, 8u, 0u, 138433u, 0.0},
        {"identical texture", SRC_SAME, 323u, 181u, 8u, 0u, 0u, 0.0},
        {"960x540", SRC_TEXTURE, HEAVY_W, HEAVY_H, 8u, 0u, 0u, 0.0},
    };
    return check_all(cases, sizeof(cases) / sizeof(cases[0]));
}

/* Identical flat frames: the CPU divides double numerators by fp32
 * denominators and scores 72.247198959355487 dB, not +inf. */
static char *test_flat_identical_frames_score_the_cpu_db(void)
{
    static const Case cases[] = {
        {"flat 8-bit", SRC_FLAT, 64u, 64u, 8u, 128u, 0u, FLAT_DB_CPU},
        {"flat 10-bit", SRC_FLAT, 64u, 64u, 10u, 512u, 0u, FLAT_DB_CPU},
    };
    return check_all(cases, sizeof(cases) / sizeof(cases[0]));
}

char *run_tests(void)
{
    mu_run_test(test_terms_are_the_cpu_expressions);
    mu_run_test(test_twin_frames_are_the_cpu_scores);
    mu_run_test(test_flat_identical_frames_score_the_cpu_db);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
