/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 */

/*
 * The decimation float_ssim_hip runs on the device, held against the CPU's
 * iqa_decimate() on the host (ADR-1405).
 *
 * float_ssim/ssim_decimate.h is plain C as well as HIP C++: the kernels
 * calculate_ssim_hip_decimate_{8,16}bpc compile the same lines this test
 * does. For every case the test builds the plane the CPU decimates
 * (picture_copy(), then iqa_decimate() in place with the low-pass kernel of
 * ssim.c::ssim_low_pass_alloc()) and the plane the device arithmetic
 * produces from the raw samples (an int64 sum in units of 2^-52, rounded to
 * fp32 once), and requires them to be equal byte for byte.
 *
 * Positive: every scale from 1 to 10 on a 576x324 plane, 10-, 12- and 16-bit
 * samples, odd widths and heights, odd scales on odd sizes. Boundary: the
 * largest exact scale (128), a plane narrower than the window (the
 * period-2n mirror wraps more than once), and the index helper at the edges
 * of its period. Negative: an fp32 running sum, the simplest kernel, is not
 * the CPU's value for a tap that is not a power of two, so the comparison
 * can tell the two apart.
 *
 * No AMD device is needed.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mu_table.h"
#include "test.h"

#include "feature/hip/float_ssim/ssim_decimate.h"
#include "feature/iqa/convolve.h"
#include "feature/iqa/decimate.h"
#include "feature/picture_copy.h"
#include "libvmaf/picture.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe. ADR-1138. */

typedef struct {
    unsigned w;
    unsigned h;
    unsigned bpc;
    int scale;
} Case;

/* lowbias32 hash: full-range noise, the worst case for the window sum. */
static uint32_t sample_hash(unsigned row, unsigned col, unsigned salt)
{
    uint32_t x = ((uint32_t)row << 16) ^ (uint32_t)col ^ (salt * 0x9E3779B9u);
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

/* The bit pattern of an fp32 value: the planes must agree byte for byte, and
 * comparing floats with == would call +0 and -0 equal. */
static uint32_t float_bits(float value)
{
    uint32_t bits = 0u;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static int fill_picture(VmafPicture *pic, const Case *c)
{
    const int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV400P, c->bpc, c->w, c->h);
    if (err) {
        return err;
    }
    for (unsigned row = 0; row < c->h; row++) {
        uint8_t *line = (uint8_t *)pic->data[0] + (size_t)row * (size_t)pic->stride[0];
        for (unsigned col = 0; col < c->w; col++) {
            const uint32_t v = sample_hash(row, col, c->bpc + (unsigned)c->scale) >> (32u - c->bpc);
            if (c->bpc > 8u) {
                const uint16_t sample = (uint16_t)v;
                memcpy(line + (2u * (size_t)col), &sample, sizeof(sample));
            } else {
                line[col] = (uint8_t)v;
            }
        }
    }
    return 0;
}

/* The CPU plane: picture_copy(), then ssim.c's decimation in place. */
static float *cpu_decimated(VmafPicture *pic, const Case *c, int *out_w, int *out_h)
{
    const size_t count = (size_t)c->w * c->h;
    float *plane = malloc(count * sizeof(*plane));
    float *taps = malloc((size_t)c->scale * (size_t)c->scale * sizeof(*taps));
    if (!plane || !taps) {
        free(plane);
        free(taps);
        return NULL;
    }
    picture_copy(plane, (ptrdiff_t)(c->w * sizeof(float)), pic, 0, c->bpc, 0);
    *out_w = (int)c->w;
    *out_h = (int)c->h;
    if (c->scale > 1) {
        const float inv2 = 1.0f / (float)(c->scale * c->scale);
        for (int i = 0; i < c->scale * c->scale; i++) {
            taps[i] = inv2;
        }
        const struct iqa_kernel low_pass = {.kernel = taps,
                                            .w = c->scale,
                                            .h = c->scale,
                                            .normalized = 0,
                                            .bnd_opt = KBND_SYMMETRIC};
        (void)iqa_decimate(plane, (int)c->w, (int)c->h, c->scale, &low_pass, NULL, out_w, out_h);
    }
    free(taps);
    return plane;
}

/* The raw luma plane packed as the twin stages it: `w` samples per row. */
static uint8_t *packed_plane(const VmafPicture *pic, const Case *c, unsigned sample_bytes)
{
    const size_t row_bytes = (size_t)c->w * sample_bytes;
    uint8_t *packed = malloc(row_bytes * c->h);
    if (!packed) {
        return NULL;
    }
    for (unsigned row = 0; row < c->h; row++) {
        memcpy(packed + (size_t)row * row_bytes,
               (const uint8_t *)pic->data[0] + (size_t)row * (size_t)pic->stride[0], row_bytes);
    }
    return packed;
}

/* picture_copy()'s divisor as the reciprocal the twin passes to the kernel. */
static float sample_scale(unsigned bpc)
{
    if (bpc == 10u) {
        return 1.0f / 4.0f;
    }
    if (bpc == 12u) {
        return 1.0f / 16.0f;
    }
    return (bpc == 16u) ? 1.0f / 256.0f : 1.0f;
}

/* Number of device outputs that differ from the CPU plane; -1 on a setup
 * failure or a size mismatch. */
static long mismatches(const Case *c)
{
    VmafPicture pic;
    if (fill_picture(&pic, c)) {
        return -1;
    }
    const unsigned sample_bytes = (c->bpc > 8u) ? 2u : 1u;
    int cpu_w = 0;
    int cpu_h = 0;
    float *cpu = cpu_decimated(&pic, c, &cpu_w, &cpu_h);
    uint8_t *packed = packed_plane(&pic, c, sample_bytes);
    long bad = -1;
    if (cpu && packed && cpu_w == iqa_decimate_dim((int)c->w, c->scale) &&
        cpu_h == iqa_decimate_dim((int)c->h, c->scale)) {
        const VmafHipSsimDecimate d = {packed,
                                       sample_bytes,
                                       c->w,
                                       c->h,
                                       c->scale,
                                       sample_scale(c->bpc),
                                       1.0f / (float)(c->scale * c->scale)};
        bad = 0;
        for (int y = 0; y < cpu_h; y++) {
            for (int x = 0; x < cpu_w; x++) {
                const float device = vmaf_hip_ssim_decimate_sample(&d, x * c->scale, y * c->scale);
                bad += float_bits(device) != float_bits(cpu[(size_t)y * (size_t)cpu_w + (size_t)x]);
            }
        }
    }
    free(packed);
    free(cpu);
    (void)vmaf_picture_unref(&pic);
    return bad;
}

static char *expect_identical(const Case *cases, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        const long bad = mismatches(&cases[i]);
        if (bad != 0) {
            (void)fprintf(stderr, "\n  %ux%u %u-bit scale %d: %ld samples differ\n", cases[i].w,
                          cases[i].h, cases[i].bpc, cases[i].scale, bad);
            return "the device decimation differs from iqa_decimate()";
        }
    }
    return NULL;
}

static char *test_every_scale_matches_cpu(void)
{
    static const Case cases[] = {
        {576u, 324u, 8u, 1}, {576u, 324u, 8u, 2},  {576u, 324u, 8u, 3}, {576u, 324u, 8u, 4},
        {576u, 324u, 8u, 5}, {576u, 324u, 8u, 6},  {576u, 324u, 8u, 7}, {576u, 324u, 8u, 8},
        {576u, 324u, 8u, 9}, {576u, 324u, 8u, 10},
    };
    return expect_identical(cases, sizeof(cases) / sizeof(cases[0]));
}

static char *test_high_bit_depths_match_cpu(void)
{
    static const Case cases[] = {
        {400u, 224u, 10u, 3}, {400u, 224u, 10u, 5}, {400u, 224u, 10u, 6},
        {400u, 224u, 12u, 2}, {400u, 224u, 12u, 5}, {400u, 224u, 12u, 10},
        {400u, 224u, 16u, 3}, {400u, 224u, 16u, 7}, {400u, 224u, 16u, 10},
    };
    return expect_identical(cases, sizeof(cases) / sizeof(cases[0]));
}

static char *test_odd_sizes_match_cpu(void)
{
    static const Case cases[] = {
        {853u, 481u, 8u, 2}, {853u, 481u, 8u, 3},  {853u, 481u, 8u, 6}, {853u, 481u, 8u, 9},
        {321u, 181u, 8u, 5}, {321u, 181u, 10u, 7}, {33u, 17u, 8u, 3},   {1u, 1u, 8u, 2},
    };
    return expect_identical(cases, sizeof(cases) / sizeof(cases[0]));
}

/* The largest exact scale, and windows wider than the plane (5x3 at scale
 * 16: the mirror wraps more than once). */
static char *test_boundary_scales_match_cpu(void)
{
    static const Case cases[] = {
        {1410u, 1410u, 8u, VMAF_HIP_SSIM_MAX_EXACT_SCALE},
        {1281u, 300u, 16u, VMAF_HIP_SSIM_MAX_EXACT_SCALE},
        {5u, 3u, 8u, 16},
        {7u, 3u, 12u, 9},
    };
    return expect_identical(cases, sizeof(cases) / sizeof(cases[0]));
}

static char *test_symmetric_index_follows_the_cpu_mirror(void)
{
    /* Period 2n, edge sample repeated: KBND_SYMMETRIC. */
    static const int expected[] = {2, 1, 0, 0, 1, 2, 3, 3, 2, 1, 0, 0, 1};
    for (int i = 0; i < 13; i++) {
        mu_assert("vmaf_hip_ssim_symmetric_index() is not the period-2n mirror",
                  vmaf_hip_ssim_symmetric_index(i - 3, 4) == expected[i]);
    }
    mu_assert("an index inside the plane must be left alone",
              vmaf_hip_ssim_symmetric_index(1919, 1920) == 1919);
    mu_assert("a one-sample axis mirrors onto itself",
              vmaf_hip_ssim_symmetric_index(-7, 1) == 0 &&
                  vmaf_hip_ssim_symmetric_index(9, 1) == 0);
    return NULL;
}

/* The 3x3 window of output (x, y) at scale 3 as an fp32 running sum. */
static float fp32_window_sum(const uint8_t *packed, unsigned w, int x, int y)
{
    const float tap = 1.0f / 9.0f;
    float sum = 0.0f;
    for (int r = -1; r <= 1; r++) {
        for (int k = -1; k <= 1; k++) {
            sum += (float)packed[(size_t)(y * 3 + r) * w + (size_t)(x * 3 + k)] * tap;
        }
    }
    return sum;
}

/* Number of interior outputs of a 576x324 8-bit plane at scale 3 where an
 * fp32 running sum of the nine products differs from `cpu`. */
static long fp32_sum_mismatches(const uint8_t *packed, const float *cpu, unsigned w, int cpu_w,
                                int cpu_h)
{
    long differing = 0;
    for (int y = 1; y + 1 < cpu_h; y++) {
        for (int x = 1; x + 1 < cpu_w; x++) {
            const float sum = fp32_window_sum(packed, w, x, y);
            differing += float_bits(sum) != float_bits(cpu[(size_t)y * (size_t)cpu_w + (size_t)x]);
        }
    }
    return differing;
}

/* An fp32 running sum of the same products, the kernel one would write
 * first. For a tap that is not a power of two it must miss the CPU's value
 * on some sample, or the comparison above proves nothing. */
static char *test_fp32_sum_is_not_the_cpu_value(void)
{
    const Case c = {576u, 324u, 8u, 3};
    VmafPicture pic;
    mu_assert("picture allocation failed", !fill_picture(&pic, &c));
    int cpu_w = 0;
    int cpu_h = 0;
    float *cpu = cpu_decimated(&pic, &c, &cpu_w, &cpu_h);
    uint8_t *packed = packed_plane(&pic, &c, 1u);
    const long differing =
        (cpu && packed) ? fp32_sum_mismatches(packed, cpu, c.w, cpu_w, cpu_h) : -1;
    free(packed);
    free(cpu);
    (void)vmaf_picture_unref(&pic);
    mu_assert("fixture setup failed", differing >= 0);
    mu_assert("an fp32 running sum matched the CPU on every sample: the fixture is too easy",
              differing > 0);
    return NULL;
}

char *run_tests(void)
{
    static const MuTest tests[] = {
        MU_TEST(test_symmetric_index_follows_the_cpu_mirror),
        MU_TEST(test_every_scale_matches_cpu),
        MU_TEST(test_high_bit_depths_match_cpu),
        MU_TEST(test_odd_sizes_match_cpu),
        MU_TEST(test_boundary_scales_match_cpu),
        MU_TEST(test_fp32_sum_is_not_the_cpu_value),
    };
    return mu_run_table(tests, MU_TABLE_LEN(tests));
}

/* NOLINTEND(modernize-use-nullptr) */
