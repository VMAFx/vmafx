/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * T-GPU-FLOAT-MS-SSIM-CPU-ARITHMETIC-2026-10-01 (ADR-1498): the arithmetic of
 * float_ms_ssim_metal, compiled on the host, against ms_ssim_decimate.c,
 * iqa/ssim_tools.c and ms_ssim.c.
 *
 * feature/metal/metal_ms_ssim_math.h and metal_ssim_terms.h are the kernels'
 * arithmetic, valid as Metal Shading Language and as C: the 9-tap
 * decimation with one fused multiply-add per tap, the window sums as exact
 * fp32 pairs, the CPU's fp32 window values and its fp64 quotients on values
 * held in 64-bit integers, and the host's frame sums and scale combine. The
 * test checks:
 *
 *   - the decimation of odd, even, tiny and large planes, bit for bit, against
 *     both ms_ssim_decimate_scalar() and ms_ssim_decimate() (the SIMD path
 *     of this host): the two passes of ms_ssim_decimate_{h,v} written with the
 *     header's functions;
 *   - the whole twin on the host: the five-level pyramid, both window passes
 *     of every scale, the stored lv / cv / sv terms added in raster order as
 *     float_ms_ssim_metal.mm adds them, each mean rounded to fp32 and the
 *     scales combined as ms_ssim.c does, against compute_ms_ssim() at `==`
 *     for the score and the fifteen l / c / s scale means, on the frames of
 *     test_metal_float_ms_ssim_parity (the order frame, the formula frame,
 *     textured frames at 8 and 10 bits, the minimum 176x176 frame, an odd
 *     frame, identical frames).
 *
 * test_metal_float_ms_ssim_exact_contract.py pins the kernel and host layout.
 * What this cannot show: that the headers compile as MSL and return these bits
 * on an Apple GPU (test_metal_float_ms_ssim_parity on the tester's device).
 */

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"

#include "float_ms_ssim_order_frame.h"
#include "float_bits.h"
#include "ssim_order_noise.h"

#include "feature/metal/metal_ms_ssim_math.h"
#include "feature/metal/metal_ssim_terms.h"
#include "feature/ms_ssim.h"
#include "feature/ms_ssim_decimate.h"
#include "feature/picture_copy.h"
#include "libvmaf/picture.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy proposes `nullptr`, but the required MSVC C build does
 * not provide that keyword. Preserve the portable C spelling. ADR-1138. */

#define SCALES VMAF_MTL_MS_SSIM_SCALES
/* The formula pair of test_metal_float_ms_ssim_parity.c. */
#define FORMULA_FRAME UINT64_C(12376132)

/* ------------------------------------------------------------------ */
/* The decimation                                                       */
/* ------------------------------------------------------------------ */

/* ms_ssim_decimate_h / _v of float_ms_ssim.metal for every output. */
static int twin_decimate(const float *src, unsigned w, unsigned h, float *dst)
{
    const unsigned w_out = vmaf_mtl_msdec_extent(w);
    const unsigned h_out = vmaf_mtl_msdec_extent(h);
    float *tmp = (float *)malloc((size_t)w_out * h * sizeof(float));
    if (tmp == NULL) {
        return -1;
    }
    for (unsigned y = 0u; y < h; y++) {
        for (unsigned x = 0u; x < w_out; x++) {
            float acc = 0.0f;
            for (int tap = 0; tap < VMAF_MTL_MSDEC_TAPS; ++tap) {
                const int xi =
                    vmaf_mtl_msdec_mirror((int)x * 2 + tap - VMAF_MTL_MSDEC_HALF, (int)w);
                acc = vmaf_mtl_msdec_tap(acc, src[(size_t)y * w + (unsigned)xi],
                                         vmaf_mtl_msdec_lpf[tap]);
            }
            tmp[(size_t)y * w_out + x] = acc;
        }
    }
    for (unsigned y = 0u; y < h_out; y++) {
        for (unsigned x = 0u; x < w_out; x++) {
            float acc = 0.0f;
            for (int tap = 0; tap < VMAF_MTL_MSDEC_TAPS; ++tap) {
                const int yi =
                    vmaf_mtl_msdec_mirror((int)y * 2 + tap - VMAF_MTL_MSDEC_HALF, (int)h);
                acc = vmaf_mtl_msdec_tap(acc, tmp[(size_t)(unsigned)yi * w_out + x],
                                         vmaf_mtl_msdec_lpf[tap]);
            }
            dst[(size_t)y * w_out + x] = acc;
        }
    }
    free(tmp);
    return 0;
}

static float noise_sample(uint64_t seed, size_t i)
{
    return (float)(ssim_order_noise_luma(seed, 0u, i)) * 0.9921875f +
           (float)(ssim_order_noise_luma(seed, 1u, i) & 3u) * 0.25f;
}

/* Equal as bit patterns, not as float values. */
static bool same_bits(float a, float b)
{
    uint32_t ua;
    uint32_t ub;
    memcpy(&ua, &a, sizeof(ua));
    memcpy(&ub, &b, sizeof(ub));
    return ua == ub;
}

typedef struct DecimateBufs {
    float *src;
    float *twin;
    float *scalar;
    float *dispatched;
} DecimateBufs;

static char *decimate_compare(const DecimateBufs *b, unsigned w, unsigned h, uint64_t seed)
{
    const unsigned w_out = vmaf_mtl_msdec_extent(w);
    const unsigned h_out = vmaf_mtl_msdec_extent(h);
    const size_t out_count = (size_t)w_out * h_out;
    for (size_t i = 0u; i < (size_t)w * h; i++)
        b->src[i] = noise_sample(seed, i);
    int rw = 0;
    int rh = 0;
    mu_assert("ms_ssim_decimate_scalar failed",
              ms_ssim_decimate_scalar(b->src, (int)w, (int)h, b->scalar, &rw, &rh) == 0);
    mu_assert("the decimated extent differs", (unsigned)rw == w_out && (unsigned)rh == h_out);
    mu_assert("ms_ssim_decimate failed",
              ms_ssim_decimate(b->src, (int)w, (int)h, b->dispatched, NULL, NULL) == 0);
    mu_assert("twin_decimate: allocation failed", twin_decimate(b->src, w, h, b->twin) == 0);
    size_t bad = 0u;
    for (size_t i = 0u; i < out_count; i++) {
        if (!same_bits(b->twin[i], b->scalar[i]) || !same_bits(b->twin[i], b->dispatched[i])) {
            if (bad++ < 3u) {
                (void)fprintf(stderr, "\n%ux%u [%zu]: twin %.9g scalar %.9g dispatch %.9g\n", w, h,
                              i, b->twin[i], b->scalar[i], b->dispatched[i]);
            }
        }
    }
    mu_assert("a decimated sample differs from ms_ssim_decimate.c", bad == 0u);
    return NULL;
}

static char *check_decimate(unsigned w, unsigned h, uint64_t seed)
{
    const size_t out_count = (size_t)vmaf_mtl_msdec_extent(w) * vmaf_mtl_msdec_extent(h);
    DecimateBufs b;
    b.src = (float *)malloc((size_t)w * h * sizeof(float));
    b.twin = (float *)malloc(out_count * sizeof(float));
    b.scalar = (float *)malloc(out_count * sizeof(float));
    b.dispatched = (float *)malloc(out_count * sizeof(float));
    char *msg = (b.src && b.twin && b.scalar && b.dispatched) ? decimate_compare(&b, w, h, seed) :
                                                                "allocation failed";
    free(b.src);
    free(b.twin);
    free(b.scalar);
    free(b.dispatched);
    return msg;
}

static char *test_decimation_is_the_cpu_decimation(void)
{
    static const unsigned sizes[][2] = {{176u, 176u}, {353u, 251u}, {640u, 480u}, {11u, 9u},
                                        {5u, 4u},     {1u, 1u},     {2u, 3u},     {17u, 23u}};
    for (size_t i = 0u; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        char *msg = check_decimate(sizes[i][0], sizes[i][1], 1000u + i);
        if (msg)
            return msg;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* The whole twin on the host                                           */
/* ------------------------------------------------------------------ */

typedef enum Source { SRC_TEXTURE, SRC_HEADER, SRC_FORMULA, SRC_SAME } Source;

typedef struct Case {
    const char *what;
    Source src;
    unsigned w;
    unsigned h;
    unsigned bpc;
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
    case SRC_HEADER:
        return side ? float_ms_ssim_order_dis_luma[i] : float_ms_ssim_order_ref_luma[i];
    case SRC_FORMULA:
        return (unsigned)(ssim_order_mix64(((UINT64_C(2) * FORMULA_FRAME + side) << 32) |
                                           (uint64_t)i) >>
                          56);
    case SRC_SAME:
        return texture_at(row, col, c->bpc, 0u);
    default:
        return texture_at(row, col, c->bpc, side ? 1u : 0u);
    }
}

/* One luma plane as picture_copy() makes it. */
static int float_plane(const Case *c, unsigned side, float *dst)
{
    VmafPicture pic;
    if (vmaf_picture_alloc(&pic, VMAF_PIX_FMT_YUV400P, c->bpc, c->w, c->h))
        return -1;
    for (unsigned row = 0u; row < c->h; row++) {
        uint8_t *line = (uint8_t *)pic.data[0] + (size_t)row * pic.stride[0];
        for (unsigned col = 0u; col < c->w; col++) {
            const unsigned v = sample_of(c, side, row, col);
            if (c->bpc > 8u) {
                ((uint16_t *)line)[col] = (uint16_t)v;
            } else {
                line[col] = (uint8_t)v;
            }
        }
    }
    picture_copy(dst, (ptrdiff_t)((size_t)c->w * sizeof(float)), &pic, 0, pic.bpc, 0);
    return vmaf_picture_unref(&pic);
}

/* ms_ssim_horiz of float_ms_ssim.metal for every output. */
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

/* ms_ssim_vert_lcs of float_ms_ssim.metal for the window at (x, y). */
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

/* One scale's l / c / s means: the stored terms added in raster order. */
static int twin_scale(const float *ref, const float *cmp, unsigned w, unsigned h, double *l,
                      double *c, double *s)
{
    const unsigned w_h = w - 10u;
    const unsigned h_v = h - 10u;
    const size_t windows = (size_t)w_h * h_v;
    VmafMtlSsimMoments *rows = (VmafMtlSsimMoments *)malloc((size_t)w_h * h * sizeof(*rows));
    uint64_t *lum = (uint64_t *)malloc(windows * sizeof(uint64_t));
    uint64_t *con = (uint64_t *)malloc(windows * sizeof(uint64_t));
    float *str = (float *)malloc(windows * sizeof(float));
    if (!rows || !lum || !con || !str) {
        free(rows);
        free(lum);
        free(con);
        free(str);
        return -1;
    }
    const VmafMtlSsimConstants k = vmaf_mtl_ssim_constants();
    const VmafMtlSsimWindowParams p = {w_h, h, w_h, h_v, 0u, 0u, k.c1, k.c2};
    twin_horizontal(ref, cmp, w, h, rows);
    for (unsigned y = 0u; y < h_v; y++) {
        for (unsigned x = 0u; x < w_h; x++) {
            const VmafMtlSsimDoubleTerms t = twin_window(rows, p, x, y);
            const size_t i = (size_t)y * w_h + x;
            lum[i] = vmaf_mtl_signed_bits(t.luminance);
            con[i] = vmaf_mtl_signed_bits(t.contrast);
            str[i] = t.structure;
        }
    }
    const VmafMtlSsimFrameSums sums = vmaf_mtl_ssim_lcs_sums(lum, con, str, windows);
    const double pixels = (double)w_h * (double)h_v;
    *l = vmaf_mtl_ms_ssim_scale_mean(sums.luminance, pixels);
    *c = vmaf_mtl_ms_ssim_scale_mean(sums.contrast, pixels);
    *s = vmaf_mtl_ms_ssim_scale_mean(sums.structure, pixels);
    free(rows);
    free(lum);
    free(con);
    free(str);
    return 0;
}

/* The twin's score and means: the pyramid by the header's decimation, the
 * scales by twin_scale(), the combine of ms_ssim.c. */
static int twin_ms_ssim(const float *ref, const float *cmp, unsigned w, unsigned h, double *score,
                        double l[SCALES], double c[SCALES], double s[SCALES])
{
    float *ref_level[SCALES] = {0};
    float *cmp_level[SCALES] = {0};
    unsigned width[SCALES];
    unsigned height[SCALES];
    int err = 0;
    width[0] = w;
    height[0] = h;
    for (int i = 0; i < SCALES && !err; i++) {
        if (i > 0) {
            width[i] = vmaf_mtl_msdec_extent(width[i - 1]);
            height[i] = vmaf_mtl_msdec_extent(height[i - 1]);
        }
        const size_t n = (size_t)width[i] * height[i];
        ref_level[i] = (float *)malloc(n * sizeof(float));
        cmp_level[i] = (float *)malloc(n * sizeof(float));
        if (!ref_level[i] || !cmp_level[i]) {
            err = -1;
        } else if (i == 0) {
            memcpy(ref_level[0], ref, n * sizeof(float));
            memcpy(cmp_level[0], cmp, n * sizeof(float));
        } else {
            err = twin_decimate(ref_level[i - 1], width[i - 1], height[i - 1], ref_level[i]);
            if (!err) {
                err = twin_decimate(cmp_level[i - 1], width[i - 1], height[i - 1], cmp_level[i]);
            }
        }
    }
    for (int i = 0; i < SCALES && !err; i++)
        err = twin_scale(ref_level[i], cmp_level[i], width[i], height[i], &l[i], &c[i], &s[i]);
    for (int i = 0; i < SCALES; i++) {
        free(ref_level[i]);
        free(cmp_level[i]);
    }
    if (!err)
        *score = vmaf_mtl_ms_ssim_combine(l, c, s);
    return err;
}

static void report_frame(const Case *c, const char *what, int scale, double cpu, double twin)
{
    (void)fprintf(stderr, "\n%s %ux%u %u-bit: %s[%d] cpu=%.17g twin=%.17g\n", c->what, c->w, c->h,
                  c->bpc, what, scale, cpu, twin);
}

typedef struct FrameScores {
    double score;
    double l[SCALES];
    double c[SCALES];
    double s[SCALES];
} FrameScores;

static int frame_mismatches(const Case *c, const FrameScores *cpu, const FrameScores *twin)
{
    int bad = 0;
    for (int i = 0; i < SCALES; i++) {
        if (!vmaf_test_identical_f64(cpu->l[i], twin->l[i])) {
            report_frame(c, "l", i, cpu->l[i], twin->l[i]);
            bad = 1;
        }
        if (!vmaf_test_identical_f64(cpu->c[i], twin->c[i])) {
            report_frame(c, "c", i, cpu->c[i], twin->c[i]);
            bad = 1;
        }
        if (!vmaf_test_identical_f64(cpu->s[i], twin->s[i])) {
            report_frame(c, "s", i, cpu->s[i], twin->s[i]);
            bad = 1;
        }
    }
    if (cpu->score != twin->score) {
        report_frame(c, "score", 0, cpu->score, twin->score);
        bad = 1;
    }
    return bad;
}

static char *frame_compare(const Case *c, const float *ref, const float *cmp)
{
    FrameScores cpu = {0};
    FrameScores twin = {0};
    const int stride = (int)(c->w * sizeof(float));
    const int err = compute_ms_ssim(ref, cmp, (int)c->w, (int)c->h, stride, stride, &cpu.score,
                                    cpu.l, cpu.c, cpu.s);
    const int twin_err = twin_ms_ssim(ref, cmp, c->w, c->h, &twin.score, twin.l, twin.c, twin.s);
    mu_assert("compute_ms_ssim failed", err == 0);
    mu_assert("the twin emulation failed", twin_err == 0);
    mu_assert("float_ms_ssim differs from the CPU", frame_mismatches(c, &cpu, &twin) == 0);
    return NULL;
}

static char *check_frame(const Case *c)
{
    const size_t pixels = (size_t)c->w * c->h;
    float *ref = (float *)malloc(pixels * sizeof(float));
    float *cmp = (float *)malloc(pixels * sizeof(float));
    char *msg = "allocation failed";
    if (ref && cmp) {
        msg = "the reference plane failed";
        if (float_plane(c, 0u, ref) == 0) {
            msg = "the distorted plane failed";
            if (float_plane(c, 1u, cmp) == 0)
                msg = frame_compare(c, ref, cmp);
        }
    }
    free(ref);
    free(cmp);
    return msg;
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
        {"order frame", SRC_HEADER, FLOAT_MS_SSIM_ORDER_W, FLOAT_MS_SSIM_ORDER_H, 8u},
        {"order formula", SRC_FORMULA, FLOAT_MS_SSIM_ORDER_W, FLOAT_MS_SSIM_ORDER_H, 8u},
        {"176x176 minimum", SRC_TEXTURE, 176u, 176u, 8u},
        {"353x251 odd", SRC_TEXTURE, 353u, 251u, 8u},
        {"640x480 8-bit", SRC_TEXTURE, 640u, 480u, 8u},
        {"640x480 10-bit", SRC_TEXTURE, 640u, 480u, 10u},
        {"512x384 12-bit", SRC_TEXTURE, 512u, 384u, 12u},
        {"512x384 identical", SRC_SAME, 512u, 384u, 8u},
    };
    return check_all(cases, sizeof(cases) / sizeof(cases[0]));
}

char *run_tests(void)
{
    mu_run_test(test_decimation_is_the_cpu_decimation);
    mu_run_test(test_twin_frames_are_the_cpu_scores);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
