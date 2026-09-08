/*
 * Copyright 2026 Lusoris
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 * Horizontal SIMD regressions: the final row of every buffer ends at width,
 * without tail padding. ASan catches discarded vector reads as well as writes;
 * NaN row padding catches writes in ordinary builds. Each ISA keeps its own
 * arithmetic; retained old/new probes pin exact values for unchanged regions.
 */
#include <float.h>
#include <math.h>
#include <stddef.h>

#include "config.h"
#include "test.h"
/* clang-format off: test.h must precede the shared SIMD test harness. */
#include "simd_bitexact_test.h"
/* clang-format on */
#include "feature/common/convolution.h"
#include "feature/common/convolution_internal.h"
#include "x86/cpu.h"

/* NOLINTBEGIN(modernize-use-nullptr): ADR-1138 preserves C NULL for MSVC. */

typedef struct {
    float *src1;
    float *src2;
    float *dst;
    float *tmp;
    int width;
    int height;
    int stride;
} ConvolutionFixture;

static void free_fixture(ConvolutionFixture *f)
{
    simd_test_aligned_free(f->src1);
    simd_test_aligned_free(f->src2);
    simd_test_aligned_free(f->dst);
    simd_test_aligned_free(f->tmp);
}

static int init_fixture(ConvolutionFixture *f, int width, int height, int step)
{
    f->width = width;
    f->height = height;
    f->stride = ((width + step - 1) / step) * step;
    const size_t count = (size_t)(height - 1) * (size_t)f->stride + (size_t)width;
    f->src1 = simd_test_aligned_malloc(count * sizeof(float), 32);
    f->src2 = simd_test_aligned_malloc(count * sizeof(float), 32);
    f->dst = simd_test_aligned_malloc(count * sizeof(float), 32);
    f->tmp = simd_test_aligned_malloc(count * sizeof(float), 32);
    if (!f->src1 || !f->src2 || !f->dst || !f->tmp)
        return 0;
    for (size_t i = 0; i < count; i++) {
        f->src1[i] = (float)((i * 37 + 17) % 251) / 251.0f;
        f->src2[i] = (float)((i * 13 + 59) % 239) / 239.0f;
        f->dst[i] = NAN;
        f->tmp[i] = NAN;
    }
    return 1;
}

static void run_avx2(ConvolutionFixture *f, const float *filter, int taps, int mode)
{
    if (mode == 0) {
        convolution_f32_avx_s(filter, taps, f->src1, f->dst, f->tmp, f->width, f->height, f->stride,
                              f->stride);
    } else if (mode == 1) {
        convolution_f32_avx_sq_s(filter, taps, f->src1, f->dst, f->tmp, f->width, f->height,
                                 f->stride, f->stride);
    } else {
        convolution_f32_avx_xy_s(filter, taps, f->src1, f->src2, f->dst, f->tmp, f->width,
                                 f->height, f->stride, f->stride, f->stride);
    }
}

#if HAVE_AVX512
static void run_avx512(ConvolutionFixture *f, const float *filter, int taps, int mode)
{
    if (mode == 0) {
        convolution_f32_avx512_s(filter, taps, f->src1, f->dst, f->tmp, f->width, f->height,
                                 f->stride, f->stride);
    } else if (mode == 1) {
        convolution_f32_avx512_sq_s(filter, taps, f->src1, f->dst, f->tmp, f->width, f->height,
                                    f->stride, f->stride);
    } else {
        convolution_f32_avx512_xy_s(filter, taps, f->src1, f->src2, f->dst, f->tmp, f->width,
                                    f->height, f->stride, f->stride, f->stride);
    }
}
#endif

static int check_plane(const ConvolutionFixture *f, const float *filter, int taps)
{
    for (int i = 0; i < f->height; i++) {
        for (int j = 0; j < f->width; j++) {
            const size_t offset = (size_t)i * (size_t)f->stride + (size_t)j;
            const float expected = convolution_edge_s(true, filter, taps, f->tmp, f->width,
                                                      f->height, f->stride, i, j);
            const float value = f->dst[offset];
            if (!isfinite(value) ||
                fabsf(value - expected) > 16.0f * FLT_EPSILON * fmaxf(1.0f, fabsf(expected)))
                return 0;
        }
        /* The final row has no padding at all; ASan owns its end boundary. */
        for (int j = f->width; i + 1 < f->height && j < f->stride; j++) {
            const size_t offset = (size_t)i * (size_t)f->stride + (size_t)j;
            if (!isnan(f->dst[offset]) || !isnan(f->tmp[offset]))
                return 0;
        }
    }
    return 1;
}

static int check_case(int width, int height, int taps, int step, int mode)
{
    ConvolutionFixture f = {0};
    float filter[MAX_FWIDTH_AVX_CONV];
    for (int k = 0; k < taps; k++) {
        const int mirror = k < taps / 2 ? k : taps - k - 1;
        filter[k] = (float)(mirror + 1) / (float)(taps * taps);
    }
    int ok = init_fixture(&f, width, height, step);
    if (ok) {
        if (step == 8) {
            run_avx2(&f, filter, taps, mode);
        }
#if HAVE_AVX512
        else {
            run_avx512(&f, filter, taps, mode);
        }
#endif
        ok = check_plane(&f, filter, taps);
    }
    free_fixture(&f);
    return ok;
}

static int check_modes(int width, int height, int taps, int step)
{
    for (int mode = 0; mode < 3; mode++) {
        if (!check_case(width, height, taps, step, mode))
            return 0;
    }
    return 1;
}

static int check_heights(int width, int taps, int step)
{
    const int heights[] = {1, 3, 17};
    for (size_t h = 0; h < sizeof(heights) / sizeof(heights[0]); h++) {
        if (!check_modes(width, heights[h], taps, step))
            return 0;
    }
    return 1;
}

static char *check_isa(int step)
{
    for (int taps = 1; taps <= MAX_FWIDTH_AVX_CONV; taps += 2) {
        for (int width = 1; width <= 49; width++) {
            mu_assert("horizontal SIMD escaped its plane or changed valid outputs",
                      check_heights(width, taps, step));
        }
    }
    return NULL;
}

static char *test_avx2_horizontal_bounds(void)
{
    return check_isa(8);
}

#if HAVE_AVX512
static char *test_avx512_horizontal_bounds(void)
{
    return check_isa(16);
}
#endif

char *run_tests(void)
{
    const unsigned flags = vmaf_get_cpu_flags_x86();
    if (flags & VMAF_X86_CPU_FLAG_AVX2) {
        mu_run_test(test_avx2_horizontal_bounds);
    } else {
        (void)fprintf(stderr, "skipping: CPU lacks AVX2\n");
    }
#if HAVE_AVX512
    if (flags & VMAF_X86_CPU_FLAG_AVX512) {
        mu_run_test(test_avx512_horizontal_bounds);
    } else {
        (void)fprintf(stderr, "skipping: CPU lacks AVX512\n");
    }
#endif
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
