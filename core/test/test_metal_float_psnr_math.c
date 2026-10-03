/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * The per-sample term of float_psnr_metal (core/src/feature/metal/
 * metal_float_psnr_math.h, ADR-1498; T-METAL-FLOAT-PSNR-FP32-BLOCK-SUMS-
 * 2026-10-02) against float_psnr.c, on the host. The header is the kernel's
 * arithmetic, compiled here as C.
 *
 * float_psnr.c converts both planes with picture_copy() (a raw sample over
 * scaler = 2^(bpc - 8)), forms `float diff = ref[j] - dis[j]` and adds
 * `(double)(diff * diff)`. The twin forms the same value times scaler^2 as an
 * integer and adds integers. The cases:
 *   - every sample difference at 8, 10, 12 and 16 bits, each at the bottom and
 *     at the top of the range, through the CPU's picture_copy(): the twin's
 *     term is the CPU's term times scaler^2, exactly. At 16 bits some terms
 *     are not the integer square (the CPU's float rounds them), and the
 *     header must return the rounded value, not the square;
 *   - a 576x324 frame of full-range noise per bit depth: the CPU's noise
 *     (rows in double, divided by w * h) equals the twin's (16x16 group sums
 *     in uint64, the total divided by scaler^2 and the pixel count), while a
 *     fp32 group sum, the kernel's former reduction, does not at 16 bits.
 *
 * Host-only: no device.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"

#include "feature/metal/metal_float_psnr_math.h"
#include "feature/picture_copy.h"
#include "libvmaf/picture.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy proposes `nullptr`, but the required MSVC C build does
 * not provide that keyword. Preserve the portable C spelling. ADR-1138. */

enum { NOISE_W = 576, NOISE_H = 324, GROUP = 16 };

/* float_psnr.c's term for two converted samples (float_psnr_noise_line_c()). */
static double cpu_term(float ref, float dis)
{
    float diff = ref - dis;
    return (double)(diff * diff);
}

static double scaler_of(unsigned bpc)
{
    return (double)(1u << (bpc - 8u));
}

/* A one-plane picture over caller-owned samples, as picture_copy() reads it. */
static VmafPicture plane_picture(void *data, unsigned bpc, unsigned w, unsigned h)
{
    VmafPicture pic;
    memset(&pic, 0, sizeof(pic));
    pic.pix_fmt = VMAF_PIX_FMT_YUV400P;
    pic.bpc = bpc;
    pic.w[0] = w;
    pic.h[0] = h;
    pic.stride[0] = (ptrdiff_t)w * (bpc > 8u ? 2 : 1);
    pic.data[0] = data;
    return pic;
}

static void put_sample(void *data, unsigned bpc, size_t i, unsigned v)
{
    if (bpc > 8u) {
        ((uint16_t *)data)[i] = (uint16_t)v;
    } else {
        ((uint8_t *)data)[i] = (uint8_t)v;
    }
}

static unsigned get_sample(const void *data, unsigned bpc, size_t i)
{
    return bpc > 8u ? ((const uint16_t *)data)[i] : ((const uint8_t *)data)[i];
}

/* Row 0: difference k at the bottom of the range; row 1: at the top. */
static void fill_differences(void *ref, void *dis, unsigned bpc, unsigned maxv)
{
    const unsigned n = (2u * maxv) + 1u;
    for (unsigned j = 0; j < n; j++) {
        const int k = (int)j - (int)maxv;
        const unsigned low = k >= 0 ? (unsigned)k : 0u;
        const unsigned high = k >= 0 ? maxv : (unsigned)((int)maxv + k);
        put_sample(ref, bpc, j, low);
        put_sample(dis, bpc, j, (unsigned)((int)low - k));
        put_sample(ref, bpc, n + j, high);
        put_sample(dis, bpc, n + j, (unsigned)((int)high - k));
    }
}

/* Terms that are not the CPU's (`bad`) and terms that are not the integer
 * square (`rounded`), over 2 * n samples converted by picture_copy(). */
static void compare_terms(const void *ref, const void *dis, const float *rf, const float *df,
                          unsigned bpc, size_t count, unsigned *bad, unsigned *rounded)
{
    const double s2 = scaler_of(bpc) * scaler_of(bpc);
    for (size_t i = 0; i < count; i++) {
        const int r = (int)get_sample(ref, bpc, i);
        const int d = (int)get_sample(dis, bpc, i);
        const vmaf_mtl_u32 term = vmaf_mtl_fpsnr_term(r, d);
        if ((double)term != cpu_term(rf[i], df[i]) * s2) {
            (*bad)++;
        }
        if ((uint64_t)term != (uint64_t)((int64_t)(r - d) * (int64_t)(r - d))) {
            (*rounded)++;
        }
    }
}

/* Every difference at `bpc`; `rounded` counts the terms the float rounds. */
static char *check_every_difference(unsigned bpc, unsigned *rounded)
{
    const unsigned maxv = (1u << bpc) - 1u;
    const unsigned n = (2u * maxv) + 1u;
    const size_t bytes = (size_t)2u * n * 2u;
    void *ref = malloc(bytes);
    void *dis = malloc(bytes);
    float *rf = malloc((size_t)2u * n * sizeof(float));
    float *df = malloc((size_t)2u * n * sizeof(float));
    unsigned bad = 0u;
    *rounded = 0u;
    if (ref && dis && rf && df) {
        fill_differences(ref, dis, bpc, maxv);
        VmafPicture rp = plane_picture(ref, bpc, n, 2u);
        VmafPicture dp = plane_picture(dis, bpc, n, 2u);
        picture_copy(rf, (ptrdiff_t)(n * sizeof(float)), &rp, 0, bpc, 0);
        picture_copy(df, (ptrdiff_t)(n * sizeof(float)), &dp, 0, bpc, 0);
        compare_terms(ref, dis, rf, df, bpc, (size_t)2u * n, &bad, rounded);
    }
    const int allocated = ref && dis && rf && df;
    free(ref);
    free(dis);
    free(rf);
    free(df);
    mu_assert("allocation failed", allocated);
    if (bad) {
        (void)fprintf(stderr, "\n  %u-bit: %u terms differ from the CPU's\n", bpc, bad);
    }
    mu_assert("a term is not float_psnr.c's float square times scaler^2", bad == 0u);
    return NULL;
}

static char *test_every_difference_8bit(void)
{
    unsigned rounded = 0u;
    mu_assert_msg(check_every_difference(8u, &rounded));
    mu_assert("8-bit squares are exact in float", rounded == 0u);
    return NULL;
}

static char *test_every_difference_10bit(void)
{
    unsigned rounded = 0u;
    mu_assert_msg(check_every_difference(10u, &rounded));
    mu_assert("10-bit squares are exact in float", rounded == 0u);
    return NULL;
}

static char *test_every_difference_12bit(void)
{
    unsigned rounded = 0u;
    mu_assert_msg(check_every_difference(12u, &rounded));
    mu_assert("12-bit squares are exact in float", rounded == 0u);
    return NULL;
}

/* At 16 bits the CPU's float rounds most squares above 2^24; the twin must
 * return those rounded values (an exact integer square is another number). */
static char *test_every_difference_16bit(void)
{
    unsigned rounded = 0u;
    mu_assert_msg(check_every_difference(16u, &rounded));
    mu_assert("16-bit squares above 2^24 are rounded on the CPU", rounded > 0u);
    return NULL;
}

/* lowbias32 hash of a position and a salt. */
static unsigned noise_sample(unsigned row, unsigned col, unsigned salt, unsigned bpc)
{
    uint32_t x = ((uint32_t)row << 16) ^ (uint32_t)col ^ (salt * 0x9E3779B9u);
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x & ((1u << bpc) - 1u);
}

/* float_psnr.c's noise: each row in double, the rows in double, / (w * h). */
static double cpu_noise(const float *rf, const float *df)
{
    double noise = 0.0;
    for (int i = 0; i < NOISE_H; i++) {
        double accum = 0.0;
        for (int j = 0; j < NOISE_W; j++) {
            accum += cpu_term(rf[(i * NOISE_W) + j], df[(i * NOISE_W) + j]);
        }
        noise += accum;
    }
    return noise / (NOISE_W * NOISE_H);
}

/* One 16x16 group's sum of the twin's terms (the kernel's integer sum) and of
 * the CPU's terms in fp32 (the kernel's former reduction, in raster order). */
static void group_sums(const void *ref, const void *dis, const float *rf, const float *df,
                       unsigned bpc, unsigned gx, unsigned gy, uint64_t *exact, float *fp32)
{
    *exact = 0u;
    *fp32 = 0.0f;
    for (unsigned y = gy * GROUP; y < (gy + 1u) * GROUP && y < (unsigned)NOISE_H; y++) {
        for (unsigned x = gx * GROUP; x < (gx + 1u) * GROUP && x < (unsigned)NOISE_W; x++) {
            const size_t i = ((size_t)y * NOISE_W) + x;
            *exact +=
                vmaf_mtl_fpsnr_term((int)get_sample(ref, bpc, i), (int)get_sample(dis, bpc, i));
            const float diff = rf[i] - df[i];
            *fp32 += diff * diff;
        }
    }
}

/* The twin's noise from its group sums (float_psnr_metal.mm::float_psnr_noise())
 * and the former fp32 group sums added in double. */
static void twin_noise(const void *ref, const void *dis, const float *rf, const float *df,
                       unsigned bpc, double *exact, double *fp32)
{
    uint64_t total = 0u;
    double fp32_total = 0.0;
    for (unsigned gy = 0; gy < ((unsigned)NOISE_H + GROUP - 1u) / GROUP; gy++) {
        for (unsigned gx = 0; gx < ((unsigned)NOISE_W + GROUP - 1u) / GROUP; gx++) {
            uint64_t e = 0u;
            float f = 0.0f;
            group_sums(ref, dis, rf, df, bpc, gx, gy, &e, &f);
            total += e;
            fp32_total += (double)f;
        }
    }
    const double scaler = scaler_of(bpc);
    const double n_pix = (double)NOISE_W * (double)NOISE_H;
    *exact = ((double)total / (scaler * scaler)) / n_pix;
    *fp32 = fp32_total / n_pix;
}

/* Full-range noise at `bpc`: the twin's noise is the CPU's; `fp32_differs`
 * says whether the former fp32 group sums are another number. */
static char *check_noise_frame(unsigned bpc, int *fp32_differs)
{
    const size_t n = (size_t)NOISE_W * NOISE_H;
    void *ref = malloc(n * 2u);
    void *dis = malloc(n * 2u);
    float *rf = malloc(n * sizeof(float));
    float *df = malloc(n * sizeof(float));
    const int allocated = ref && dis && rf && df;
    double cpu = 0.0;
    double exact = -1.0;
    double fp32 = 0.0;
    if (allocated) {
        for (size_t i = 0; i < n; i++) {
            put_sample(ref, bpc, i, noise_sample((unsigned)(i / NOISE_W), i % NOISE_W, 1u, bpc));
            put_sample(dis, bpc, i, noise_sample((unsigned)(i / NOISE_W), i % NOISE_W, 2u, bpc));
        }
        VmafPicture rp = plane_picture(ref, bpc, NOISE_W, NOISE_H);
        VmafPicture dp = plane_picture(dis, bpc, NOISE_W, NOISE_H);
        picture_copy(rf, (ptrdiff_t)(NOISE_W * sizeof(float)), &rp, 0, bpc, 0);
        picture_copy(df, (ptrdiff_t)(NOISE_W * sizeof(float)), &dp, 0, bpc, 0);
        cpu = cpu_noise(rf, df);
        twin_noise(ref, dis, rf, df, bpc, &exact, &fp32);
    }
    free(ref);
    free(dis);
    free(rf);
    free(df);
    mu_assert("allocation failed", allocated);
    *fp32_differs = fp32 != cpu;
    mu_assert("the twin's noise is not float_psnr.c's", exact == cpu);
    return NULL;
}

static char *test_noise_frames(void)
{
    static const unsigned depths[4] = {8u, 10u, 12u, 16u};
    int fp32_differs[4] = {0, 0, 0, 0};
    for (unsigned i = 0; i < 4u; i++) {
        mu_assert_msg(check_noise_frame(depths[i], &fp32_differs[i]));
    }
    mu_assert("8-bit fp32 group sums are exact", !fp32_differs[0]);
    mu_assert("the 16-bit frame shows the fp32 group sum's rounding", fp32_differs[3]);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_every_difference_8bit);
    mu_run_test(test_every_difference_10bit);
    mu_run_test(test_every_difference_12bit);
    mu_run_test(test_every_difference_16bit);
    mu_run_test(test_noise_frames);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
