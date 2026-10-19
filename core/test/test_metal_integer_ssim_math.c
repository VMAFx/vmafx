/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * T-GPU-SSIM-FRAME-SUM-ORDER-2026-10-01 (ADR-1498): the arithmetic of
 * integer_ssim_metal, compiled on the host, against integer_ssim.c.
 *
 * feature/metal/metal_integer_ssim_math.h is the kernel's arithmetic, valid
 * as Metal Shading Language and as C: the horizontal and vertical moment
 * taps, the tap ranges at the frame's edges, the window weight, and the
 * per-pixel term, which runs the fp64 operations of
 * integer_ssim.c::ssim_reduce_row_range() on values held in 64-bit integers
 * (feature/metal/metal_soft_signed.h; Metal has no fp64 type). The test
 * checks, with reference_term() below holding the reference's lines
 * verbatim:
 *
 *   - the term at 8, 10, 12 and 16 bits over random windows of every
 *     truncation, flat, textured, full-range, identical and inverted, bit for
 *     bit: at 8 and 10 bits every product of two moments is below 2^52 and
 *     the header's integer path is taken, at 16 bits the rounding path, at
 *     12 bits both;
 *   - the whole twin on the host: the kernel's two passes written with the
 *     header's functions, the term plane added in raster order as the host
 *     (integer_ssim_metal.mm) adds it and divided by the product of the two
 *     line weights, against the CPU `ssim` extractor through libvmaf, at `==`,
 *     on the frames of ssim_twin_parity.h (the cases of
 *     test_metal_integer_ssim_parity: 8 to 16 bits, odd and tiny frames, one
 *     pixel, 1080p, inverted and identical frames).
 *
 * test_metal_integer_ssim_exact_contract.py pins that integer_ssim.c still
 * holds the lines reference_term() copies, and the kernel and host layout.
 * What this cannot show: that the header compiles as MSL and returns these
 * bits on an Apple GPU (test_metal_integer_ssim_parity on the tester's
 * device).
 *
 * A zero has no sign in metal_soft_signed.h (the reference adds its terms
 * into a sum, where the sign of a zero changes nothing), so a zero term is
 * compared by value.
 */

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"
#include "float_bits.h"

#include "ssim_twin_parity.h"

#include "feature/metal/metal_integer_ssim_math.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy proposes `nullptr`, but the required MSVC C build does
 * not provide that keyword. Preserve the portable C spelling. ADR-1138. */

enum {
    TERM_SAMPLES = 300000,
    DEPTH_COUNT = 4,
    KERNEL_TAPS = 9,
    WINDOW_KINDS = 6,
};

/* The bound below which the header takes its integer path. */
#define EXACT_PRODUCT_BOUND (UINT64_C(1) << 52)

static const unsigned DEPTH[DEPTH_COUNT] = {8u, 10u, 12u, 16u};
/* gaussian_filter_init(1.5, 5): integer_ssim.c's kernel. */
static const int64_t KERNEL[KERNEL_TAPS] = {2, 9, 28, 55, 68, 55, 28, 9, 2};

/* integer_ssim.c's ssim_moments, SSIM_K1 and SSIM_K2. */
typedef struct ssim_moments {
    int64_t mux;
    int64_t muy;
    int64_t x2;
    int64_t xy;
    int64_t y2;
    int64_t w;
} ssim_moments;

#define SSIM_K1 (0.01 * 0.01)
#define SSIM_K2 (0.03 * 0.03)

/* integer_ssim.c::ssim_reduce_row_range(), the lines that form one pixel's
 * term, verbatim; the reference adds the returned expression to `*ssim`. */
static double reference_term(ssim_moments m, int samplemax)
{
    const double sm = (double)samplemax;
    double c1;
    double c2;
    double mx2;
    double mxy;
    double my2;
    double w_d;
    w_d = (double)m.w;
    c1 = sm * sm * SSIM_K1 * w_d * w_d;
    c2 = sm * sm * SSIM_K2 * w_d * w_d;
    mx2 = m.mux * (double)m.mux;
    mxy = m.mux * (double)m.muy;
    my2 = m.muy * (double)m.muy;
    return m.w * (2 * mxy + c1) * (c2 + 2 * (m.xy * w_d - mxy)) /
           ((mx2 + my2 + c1) * (m.x2 * w_d - mx2 + m.y2 * w_d - my2 + c2));
}

static uint64_t bits_of(double value)
{
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static double value_of(uint64_t bits)
{
    double value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

/* The stabilisers the host hands the kernel: integer_ssim_metal.mm's
 * issim_stabiliser_bits(), `sm * sm * k` in fp64. */
static VmafMtlIssimStabilisers host_stabilisers(unsigned bpc)
{
    const double sm = (double)((1 << bpc) - 1);
    return vmaf_mtl_issim_stabilisers(bits_of(sm * sm * SSIM_K1), bits_of(sm * sm * SSIM_K2));
}

/* Deterministic generator: the same inputs on every host. */
static uint64_t rng_state = UINT64_C(0x9e3779b97f4a7c15);

static uint64_t rng_next(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

/* The sample of one pixel of a window. `kind`: 0 and 1 a flat window, 2 a
 * textured one, 3 the full range; the comparison is the reference itself
 * (4), its negative (5) or its own sample. */
static void window_samples(unsigned kind, int samplemax, int base, int64_t *s, int64_t *d)
{
    const int span = kind < 2u ? 4 : (kind == 2u ? samplemax / 8 + 1 : samplemax + 1);
    const int low = kind == 3u ? 0 : base;
    int64_t reference = low + (int64_t)(rng_next() % (uint64_t)span);
    int64_t comparison = low + (int64_t)(rng_next() % (uint64_t)span);
    reference = reference > samplemax ? samplemax : reference;
    comparison = comparison > samplemax ? samplemax : comparison;
    if (kind == 4u)
        comparison = reference;
    if (kind == 5u)
        comparison = samplemax - reference;
    *s = reference;
    *d = comparison;
}

/* The moments of one random window, as ssim_accumulate_row() and
 * ssim_reduce_row_range() accumulate them. One window in four is truncated
 * as at a frame's edge. */
static ssim_moments random_window(unsigned index, int samplemax)
{
    const unsigned kind = index % WINDOW_KINDS;
    const int truncated = (index % 4u) == 0u;
    const int first_row = truncated ? (int)(rng_next() % 5u) : 0;
    const int last_row = KERNEL_TAPS - (truncated ? (int)(rng_next() % 5u) : 0);
    const int first_col = truncated ? (int)(rng_next() % 5u) : 0;
    const int last_col = KERNEL_TAPS - (truncated ? (int)(rng_next() % 5u) : 0);
    const int base = (int)(rng_next() % (uint64_t)(samplemax + 1));
    ssim_moments m = {0};
    for (int row = first_row; row < last_row; row++) {
        for (int col = first_col; col < last_col; col++) {
            int64_t s = 0;
            int64_t d = 0;
            window_samples(kind, samplemax, base, &s, &d);
            const int64_t window = KERNEL[row] * KERNEL[col];
            m.mux += window * s;
            m.muy += window * d;
            m.x2 += window * s * s;
            m.xy += window * s * d;
            m.y2 += window * d * d;
            m.w += window;
        }
    }
    return m;
}

static int products_exact(ssim_moments m)
{
    const uint64_t all = ((uint64_t)m.mux * (uint64_t)m.mux) | ((uint64_t)m.mux * (uint64_t)m.muy) |
                         ((uint64_t)m.muy * (uint64_t)m.muy) | ((uint64_t)m.x2 * (uint64_t)m.w) |
                         ((uint64_t)m.xy * (uint64_t)m.w) | ((uint64_t)m.y2 * (uint64_t)m.w);
    return all < EXACT_PRODUCT_BOUND;
}

/* Equal bits, or two zeros. */
static int same_double(uint64_t got, uint64_t ref)
{
    return got == ref || (value_of(got) == 0.0 && value_of(ref) == 0.0);
}

/* What the samples of one bit depth cover and how many terms differ. */
typedef struct TermRun {
    unsigned exact;    /* every product of two moments below 2^52 */
    unsigned negative; /* the term is below zero */
    unsigned wrong;
} TermRun;

static void report_term(unsigned bpc, unsigned i, ssim_moments m, uint64_t got, uint64_t ref)
{
    (void)fprintf(stderr,
                  "\n%u-bit sample %u mux=%lld muy=%lld x2=%lld xy=%lld y2=%lld w=%lld: %a, "
                  "reference %a",
                  bpc, i, (long long)m.mux, (long long)m.muy, (long long)m.x2, (long long)m.xy,
                  (long long)m.y2, (long long)m.w, value_of(got), value_of(ref));
}

static TermRun run_terms(unsigned bpc)
{
    const int samplemax = (1 << bpc) - 1;
    const VmafMtlIssimStabilisers k = host_stabilisers(bpc);
    TermRun run = {0u, 0u, 0u};
    rng_state = UINT64_C(0x9e3779b97f4a7c15);
    for (unsigned i = 0u; i < TERM_SAMPLES; i++) {
        const ssim_moments m = random_window(i, samplemax);
        const double term = reference_term(m, samplemax);
        const VmafMtlIssimMoments mm = {(uint64_t)m.mux, (uint64_t)m.muy, (uint64_t)m.x2,
                                        (uint64_t)m.xy,  (uint64_t)m.y2,  (uint64_t)m.w};
        const uint64_t got = vmaf_mtl_issim_term_bits(mm, k);
        if (!same_double(got, bits_of(term))) {
            if (run.wrong < 5u)
                report_term(bpc, i, m, got, bits_of(term));
            run.wrong++;
        }
        run.exact += products_exact(m) ? 1u : 0u;
        run.negative += term < 0.0 ? 1u : 0u;
    }
    return run;
}

static char *test_terms_are_the_fp64_expression(void)
{
    for (unsigned depth = 0u; depth < DEPTH_COUNT; depth++) {
        const unsigned bpc = DEPTH[depth];
        const TermRun run = run_terms(bpc);
        if (run.wrong != 0u) {
            (void)fprintf(stderr, "\n%u-bit: %u of %d terms differ\n", bpc, run.wrong,
                          TERM_SAMPLES);
        }
        mu_assert("the term is not the reference's fp64 expression", run.wrong == 0u);
        mu_assert("no term is negative: the inverted windows are not reached", run.negative > 0u);
        if (bpc <= 10u) {
            mu_assert("an 8- or 10-bit window leaves the integer path",
                      run.exact == (unsigned)TERM_SAMPLES);
        }
        if (bpc == 12u) {
            mu_assert("the 12-bit windows do not reach both paths",
                      run.exact > 0u && run.exact < (unsigned)TERM_SAMPLES);
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* The whole twin on the host                                           */
/* ------------------------------------------------------------------ */

static int64_t sample_at(const VmafPicture *pic, unsigned row, unsigned col)
{
    const uint8_t *line = (const uint8_t *)pic->data[0] + (size_t)row * (size_t)pic->stride[0];
    return pic->bpc <= 8u ? (int64_t)line[col] : (int64_t)((const uint16_t *)line)[col];
}

/* Pass 0 of integer_ssim.metal for every pixel: five planes of horizontal
 * sums. */
static void horizontal_pass(const VmafPicture *ref, const VmafPicture *dis, VmafMtlIssimSums *rows)
{
    const unsigned w = ref->w[0];
    for (unsigned y = 0u; y < ref->h[0]; y++) {
        for (unsigned x = 0u; x < w; x++) {
            const VmafMtlIssimTaps taps = vmaf_mtl_issim_tap_range(x, w);
            VmafMtlIssimSums sums = vmaf_mtl_issim_sums_make(0, 0, 0, 0, 0);
            for (int tap = taps.first; tap < taps.last; ++tap) {
                const unsigned source = (unsigned)((int)x - VMAF_MTL_ISSIM_HALF + tap);
                sums = vmaf_mtl_issim_horizontal_tap(sums, vmaf_mtl_issim_kernel[tap],
                                                     sample_at(ref, y, source),
                                                     sample_at(dis, y, source));
            }
            rows[(size_t)y * w + x] = sums;
        }
    }
}

/* Pass 1 of integer_ssim.metal for the pixel at (x, y): its term's bits. */
static uint64_t pixel_term(const VmafMtlIssimSums *rows, VmafMtlIssimParams p, unsigned x,
                           unsigned y)
{
    const VmafMtlIssimTaps taps = vmaf_mtl_issim_tap_range(y, p.height);
    VmafMtlIssimSums sums = vmaf_mtl_issim_sums_make(0, 0, 0, 0, 0);
    for (int tap = taps.first; tap < taps.last; ++tap) {
        const size_t source = (size_t)((int)y - VMAF_MTL_ISSIM_HALF + tap) * p.width + x;
        sums = vmaf_mtl_issim_vertical_tap(sums, vmaf_mtl_issim_kernel[tap], rows[source]);
    }
    const VmafMtlIssimMoments m =
        vmaf_mtl_issim_moments(sums, vmaf_mtl_issim_tap_weight(taps),
                               vmaf_mtl_issim_tap_weight(vmaf_mtl_issim_tap_range(x, p.width)));
    return vmaf_mtl_issim_term_bits(m, vmaf_mtl_issim_stabilisers(p.k1_bits, p.k2_bits));
}

/* integer_ssim_metal.mm::issim_line_weight(). */
static int64_t line_weight(unsigned extent)
{
    int64_t weight = 0;
    for (unsigned position = 0u; position < extent; position++)
        weight += vmaf_mtl_issim_tap_weight(vmaf_mtl_issim_tap_range(position, extent));
    return weight;
}

/* The twin's ssim / ssimw: both passes, the raster-order sum of the term
 * plane and the product of the line weights, as the kernels and the host do. */
static int twin_ratio(const VmafPicture *ref, const VmafPicture *dis, double *ratio)
{
    const unsigned w = ref->w[0];
    const unsigned h = ref->h[0];
    VmafMtlIssimSums *rows = (VmafMtlIssimSums *)malloc((size_t)w * h * sizeof(*rows));
    if (!rows)
        return -1;
    const double sm = (double)((1 << ref->bpc) - 1);
    const VmafMtlIssimParams p = {w, h, bits_of(sm * sm * SSIM_K1), bits_of(sm * sm * SSIM_K2)};
    horizontal_pass(ref, dis, rows);
    double sum = 0.0;
    for (unsigned y = 0u; y < h; y++) {
        for (unsigned x = 0u; x < w; x++)
            sum += value_of(pixel_term(rows, p, x, y));
    }
    free(rows);
    *ratio = sum / (double)(line_weight(w) * line_weight(h));
    return 0;
}

/* The CPU extractor's score and the twin's ratio for one case, at `==`. */
static mu_message_t check_frame(const SsimTwinCase *c)
{
    static const SsimTwin cpu_only = {"ssim", "host", NULL, NULL, NULL};
    double cpu = 0.0;
    mu_message_t msg = ssim_twin_score(&cpu_only, NULL, c, &cpu);
    if (msg)
        return msg;
    VmafPicture ref;
    VmafPicture dis;
    mu_assert("fill reference failed", !ssim_twin_fill_picture(&ref, c, false));
    mu_assert("fill distorted failed", !ssim_twin_fill_picture(&dis, c, true));
    double twin = NAN;
    const int err = twin_ratio(&ref, &dis, &twin);
    const int unref_ref = vmaf_picture_unref(&ref);
    const int unref_dis = vmaf_picture_unref(&dis);
    mu_assert("twin emulation failed", err == 0);
    mu_assert("vmaf_picture_unref failed", unref_ref == 0 && unref_dis == 0);
    if (!vmaf_test_identical_f64(cpu, twin)) {
        (void)fprintf(stderr, "\n%s %ux%u %u-bit: cpu=%.17g twin=%.17g\n", c->what, c->w, c->h,
                      c->bpc, cpu, twin);
    }
    mu_assert("the twin's ssim on the host differs from the CPU extractor",
              vmaf_test_identical_f64(cpu, twin));
    return NULL;
}

static char *test_twin_frames_are_the_cpu_scores(void)
{
    static const SsimTwinCase cases[] = {
        {"ssim", FIXTURE_W, FIXTURE_H, 8u, SSIM_TWIN_NOISE, NULL, NULL},
        {"ssim", FIXTURE_W, FIXTURE_H, 10u, SSIM_TWIN_NOISE, NULL, NULL},
        {"ssim", FIXTURE_W, FIXTURE_H, 12u, SSIM_TWIN_NOISE, NULL, NULL},
        {"ssim", FIXTURE_W, FIXTURE_H, 16u, SSIM_TWIN_NOISE, NULL, NULL},
        {"ssim odd", 323u, 181u, 8u, SSIM_TWIN_NOISE, NULL, NULL},
        {"ssim tiny", 7u, 5u, 8u, SSIM_TWIN_NOISE, NULL, NULL},
        {"ssim tiny", 7u, 5u, 16u, SSIM_TWIN_NOISE, NULL, NULL},
        {"ssim 1x1", 1u, 1u, 8u, SSIM_TWIN_NOISE, NULL, NULL},
        {"ssim 1080p", 1920u, 1080u, 8u, SSIM_TWIN_NOISE, NULL, NULL},
        {"ssim inverted", FIXTURE_W, FIXTURE_H, 8u, SSIM_TWIN_INVERTED, NULL, NULL},
        {"ssim inverted", FIXTURE_W, FIXTURE_H, 16u, SSIM_TWIN_INVERTED, NULL, NULL},
        {"ssim identical", 323u, 181u, 8u, SSIM_TWIN_SAME, NULL, NULL},
        {"ssim identical", 3u, 3u, 8u, SSIM_TWIN_SAME, NULL, NULL},
    };
    for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); i++)
        mu_assert_msg(check_frame(&cases[i]));
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_terms_are_the_fp64_expression);
    mu_run_test(test_twin_frames_are_the_cpu_scores);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
