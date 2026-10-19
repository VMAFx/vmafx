/**
 * Copyright 2026 Dan Trapp.
 * Copyright 2026 Lusoris
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 * Licensed under the BSD+Patent License (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://opensource.org/licenses/BSDplusPatent
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * vif_filter1d_dec16_s() against vif_filter1d_s() followed by vif_dec16_s()
 * (Netflix/vmaf 76ea5f03, ad42c532, 9cb9479f). SpEED's anti-alias step keeps
 * one filtered sample in 256; the extractor computes only those, on every
 * target, and the result has to be the bits the two calls leave there.
 *
 *   test_filter_dec16          the upstream matrix: sizes that are and are
 *                              not multiples of 16, tight and padded strides,
 *                              an unaligned source, five image patterns,
 *                              every filter width from 3 to 37 taps. The
 *                              sizes of upstream's checkasm cases (cea2b4d8,
 *                              9cb9479f; the fork carries no checkasm tree)
 *                              and the planes SpEED meets here (80x80, the
 *                              smallest it accepts, odd planes, the Netflix
 *                              chroma plane) are added.
 *   test_avx_rows (x86, AVX2)  convolution_f32_avx_rows_s(), the AVX2
 *                              vertical pass, against the scalar sum written
 *                              out, on every column of widths 1 to 41: the
 *                              decimated horizontal pass never reads a row's
 *                              last columns, so the masked tail is only seen
 *                              here. A guard after the row catches a store
 *                              past the width.
 *   test_filter_and_downscale  speed_internal_filter_and_downscale(), the
 *                              mirror of speed.c's static
 *                              filter_and_downscale(), against the two calls
 *                              spelled out: the whole frame buffer, so the
 *                              copy back from the scratch plane is covered.
 *
 * Both compare with memcmp. The reference is the scalar two-call path (CPU
 * flags masked to 0). Each case is then run again with every flag the host
 * has: the fused call (its AVX2 vertical pass on x86) and the two-call path
 * x86 ran before ad42c532 (AVX2 convolution), each against the reference.
 * On a host without AVX2 those runs repeat the scalar path.
 */

#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "feature/common/convolution.h"
#include "feature/speed_internal.h"
#include "feature/vif_tools.h"
#include "mem.h"
#include "mu_table.h"
#include "test.h"

/* NOLINTBEGIN(modernize-use-nullptr) -- ADR-1138: retain NULL for Windows C
 * support and upstream-compatible C test conventions. */

#define SF_NUM_SCALES 4 /* NUM_SCALES, speed.c */
#define SF_DST_GUARD (-12345.f)
#define SF_PATTERNS 5u
#define SF_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define SF_AVX_STEP 8 /* convolution_f32_avx_s() rounds its tmp rows up to this */
#define SF_ALIGN 32u  /* the AVX2 convolution's aligned loads and stores */
#define SF_LAYOUTS 3u
#define SF_AVX_LAYOUT 2u

typedef struct SfSize {
    int w;
    int h;
} SfSize;

typedef struct SfBuffers {
    float *allocation; /* src, one float earlier for the unaligned layout */
    float *src;
    float *full;      /* vif_filter1d_s() output */
    float *tmp;       /* w floats: what the fused call is promised */
    float *plane_tmp; /* a whole plane: what the AVX2 convolution needs */
    float *expected;
    float *actual;
    int src_stride; /* floats */
    int full_stride;
    int dst_stride;
    unsigned layout;
    size_t src_count;
    size_t dst_count;
} SfBuffers;

static const float sf_kernelscales[] = {
    0.1f, 0.5f, 1.0f, 1.5f, 2.0f, 3.0f, 360.0f / 97.0f, 4.0f,
};

static void sf_free(SfBuffers *b)
{
    aligned_free(b->allocation);
    aligned_free(b->full);
    free(b->tmp);
    aligned_free(b->plane_tmp);
    free(b->expected);
    free(b->actual);
}

/* layout 0: tight strides. layout 1: padded strides and a source one float
 * past the allocation's alignment. layout 2 (SF_AVX_LAYOUT): SpEED's own
 * buffers, SF_ALIGN-aligned planes with strides rounded up to SF_AVX_STEP
 * floats, the only layout the AVX2 convolution of the two-call path accepts
 * (it loads and stores aligned). The fused call takes all three. */
static int sf_alloc(SfBuffers *b, int w, int h, unsigned layout)
{
    const int padded = (w + SF_AVX_STEP - 1) / SF_AVX_STEP * SF_AVX_STEP;
    const unsigned shift = layout == 1 ? 1u : 0u;

    b->layout = layout;
    b->src_stride = layout == SF_AVX_LAYOUT ? padded : w + (layout ? 3 : 0);
    b->full_stride = layout == SF_AVX_LAYOUT ? padded : w + (layout ? 7 : 0);
    b->dst_stride = w / 16 + (layout ? 5 : 0);
    b->src_count = (size_t)(h - 1) * (size_t)b->src_stride + (size_t)w;
    b->dst_count = (size_t)(h / 16) * (size_t)b->dst_stride;
    b->allocation = aligned_malloc((b->src_count + shift) * sizeof(float), SF_ALIGN);
    b->src = b->allocation ? b->allocation + shift : NULL;
    b->full = aligned_malloc((size_t)h * (size_t)b->full_stride * sizeof(float), SF_ALIGN);
    b->tmp = malloc((size_t)w * sizeof(float));
    b->plane_tmp = aligned_malloc((size_t)padded * (size_t)h * sizeof(float), SF_ALIGN);
    b->expected = malloc(b->dst_count * sizeof(float));
    b->actual = malloc(b->dst_count * sizeof(float));
    return b->src && b->full && b->tmp && b->plane_tmp && b->expected && b->actual;
}

static float sf_pattern_value(unsigned pattern, uint32_t state, int i, int j, SfSize size)
{
    const int corner_row = i == 0 || i == size.h - 1;
    const int corner_col = j == 0 || j == size.w - 1;

    switch (pattern) {
    case 0:
        return (float)((int)(state & 65535u) - 32768) / 128.f;
    case 1:
        return 127.5f;
    case 2:
        return ((i + j) & 1) ? 255.f : 0.f;
    case 3:
        return corner_row && corner_col ? 255.f : 0.f;
    default:
        return (float)(i * size.w + j) / 257.f;
    }
}

/* Padding stays NaN: a pass that reads it poisons its output. */
static void sf_fill(float *src, size_t count, int stride, SfSize size, unsigned pattern,
                    uint32_t *state)
{
    for (size_t i = 0; i < count; i++)
        src[i] = NAN;
    for (int i = 0; i < size.h; i++) {
        for (int j = 0; j < size.w; j++) {
            *state = *state * 1664525u + 1013904223u;
            src[(ptrdiff_t)i * stride + j] = sf_pattern_value(pattern, *state, i, j, size);
        }
    }
}

/* vif_filter1d_s() + vif_dec16_s() into out, under the current CPU mask. */
static void sf_two_calls(SfBuffers *b, SfSize size, const float *filter, int fwidth, float *out)
{
    vif_filter1d_s(filter, b->src, b->full, b->plane_tmp, size.w, size.h,
                   b->src_stride * (int)sizeof(float), b->full_stride * (int)sizeof(float), fwidth);
    vif_dec16_s(b->full, out, size.w, size.h, b->full_stride * (int)sizeof(float),
                b->dst_stride * (int)sizeof(float));
}

/* vif_filter1d_dec16_s() into out, under the current CPU mask. */
static void sf_fused(SfBuffers *b, SfSize size, const float *filter, int fwidth, float *out)
{
    vif_filter1d_dec16_s(filter, b->src, out, b->tmp, size.w, size.h,
                         b->src_stride * (int)sizeof(float), b->dst_stride * (int)sizeof(float),
                         fwidth);
}

static void sf_guard(float *dst, size_t count)
{
    for (size_t i = 0; i < count; i++)
        dst[i] = SF_DST_GUARD;
}

/* Returns 1 (and names the case) when actual differs from expected. */
static int sf_differs(const SfBuffers *b, SfSize size, int fwidth, const char *path)
{
    if (memcmp(b->expected, b->actual, b->dst_count * sizeof(float)) == 0)
        return 0;
    (void)fprintf(stderr, "%s: %dx%d, src stride %d, filter %d\n", path, size.w, size.h,
                  b->src_stride, fwidth);
    return 1;
}

/* One filter width on the filled source: the scalar two calls into expected,
 * then the fused call (scalar, then every host flag) and the two calls with
 * every host flag into actual. Returns the number of runs whose bits differ. */
static int sf_compare(SfBuffers *b, SfSize size, float kernelscale)
{
    const int fwidth = vif_get_filter_size(1, kernelscale);
    float filter[128];
    int differs = 0;

    if (fwidth / 2 >= size.w || fwidth / 2 >= size.h)
        return 0; /* the mirror needs the half width inside the plane */

    speed_get_antialias_filter(filter, SF_NUM_SCALES, kernelscale);
    vmaf_set_cpu_flags_mask(0);
    sf_guard(b->expected, b->dst_count);
    sf_two_calls(b, size, filter, fwidth, b->expected);
    sf_guard(b->actual, b->dst_count);
    sf_fused(b, size, filter, fwidth, b->actual);
    differs += sf_differs(b, size, fwidth, "fused, scalar");

    vmaf_set_cpu_flags_mask(UINT_MAX);
    sf_guard(b->actual, b->dst_count);
    sf_fused(b, size, filter, fwidth, b->actual);
    differs += sf_differs(b, size, fwidth, "fused, host flags");
    if (b->layout == SF_AVX_LAYOUT) {
        sf_guard(b->actual, b->dst_count);
        sf_two_calls(b, size, filter, fwidth, b->actual);
        differs += sf_differs(b, size, fwidth, "two calls, host flags");
    }
    vmaf_set_cpu_flags_mask(0);
    return differs;
}

static char *sf_check_layout(SfSize size, unsigned layout, uint32_t *state)
{
    SfBuffers b = {0};
    int mismatches = 0;

    if (!sf_alloc(&b, size.w, size.h, layout)) {
        sf_free(&b);
        return "filter buffer allocation failed";
    }
    for (unsigned pattern = 0; pattern < SF_PATTERNS; pattern++) {
        sf_fill(b.src, b.src_count, b.src_stride, size, pattern, state);
        for (size_t k = 0; k < SF_LEN(sf_kernelscales); k++)
            mismatches += sf_compare(&b, size, sf_kernelscales[k]);
    }
    sf_free(&b);
    mu_assert("decimated filter differs from full filter", mismatches == 0);
    return NULL;
}

static char *test_filter_dec16(void)
{
    static const SfSize sizes[] = {
        /* Netflix/vmaf 76ea5f03, test_speed_filter.c */
        {16, 16},
        {17, 31},
        {31, 17},
        {32, 32},
        {33, 33},
        {63, 65},
        {65, 63},
        {80, 80},
        {81, 95},
        {95, 81},
        {96, 97},
        {97, 96},
        {128, 129},
        {160, 161},
        {320, 180},
        /* Netflix/vmaf cea2b4d8 and 9cb9479f, checkasm check_filter_dec16() */
        {64, 64},
        {79, 48},
        {255, 63},
        {256, 64},
        /* planes SpEED filters in this tree */
        {81, 83},
        {173, 131},
        {288, 162},
    };
    uint32_t state = 123456789;

    vmaf_init_cpu();
    vmaf_set_cpu_flags_mask(0);
    for (size_t s = 0; s < SF_LEN(sizes); s++) {
        for (unsigned layout = 0; layout < SF_LAYOUTS; layout++)
            mu_assert_msg(sf_check_layout(sizes[s], layout, &state));
    }
    return NULL;
}

#if ARCH_X86
#define SF_ROWS_MAX_W 41
#define SF_ROWS_GUARD 8

/* The comparison is of bit patterns on purpose (the contract is the scalar's
 * bits), as in the other memcmp() calls of this file. */
static int sf_bits_differ(const float *a, const float *b, size_t count)
{
    return memcmp(a, b, count * sizeof(*a)) != 0;
}

/* The scalar vertical pass of vif_tools.c for one output row, written out:
 * products added in tap order to a sum that starts at zero. */
static void sf_rows_reference(const float *taps, int fwidth, const float *const *rows, float *dst,
                              int width)
{
    for (int j = 0; j < width; j++) {
        float accum = 0;
        for (int k = 0; k < fwidth; k++)
            accum += taps[k] * rows[k][j];
        dst[j] = accum;
    }
}

/* Every width from 1 to SF_ROWS_MAX_W for one filter width; returns the
 * number of widths whose row (or guard) differs. */
static int sf_rows_one_fwidth(const float *taps, int fwidth, const float *const *rows)
{
    float expected[SF_ROWS_MAX_W + SF_ROWS_GUARD];
    float actual[SF_ROWS_MAX_W + SF_ROWS_GUARD];
    int mismatches = 0;

    for (int w = 1; w <= SF_ROWS_MAX_W; w++) {
        for (size_t i = 0; i < SF_LEN(expected); i++) {
            expected[i] = SF_DST_GUARD;
            actual[i] = SF_DST_GUARD;
        }
        sf_rows_reference(taps, fwidth, rows, expected, w);
        convolution_f32_avx_rows_s(taps, fwidth, rows, actual, w);
        if (sf_bits_differ(expected, actual, SF_LEN(actual))) {
            (void)fprintf(stderr, "avx rows: width %d, filter %d\n", w, fwidth);
            mismatches++;
        }
    }
    return mismatches;
}

static char *test_avx_rows(void)
{
    static const int fwidths[] = {1, 3, 5, 9, 15, MAX_FWIDTH_AVX_CONV};
    float rows_data[MAX_FWIDTH_AVX_CONV][SF_ROWS_MAX_W];
    const float *rows[MAX_FWIDTH_AVX_CONV];
    float taps[MAX_FWIDTH_AVX_CONV];
    uint32_t state = 24680u;
    int mismatches = 0;

    for (int k = 0; k < MAX_FWIDTH_AVX_CONV; k++) {
        rows[k] = rows_data[k];
        for (int j = 0; j < SF_ROWS_MAX_W; j++) {
            state = state * 1664525u + 1013904223u;
            rows_data[k][j] = (float)((int)(state >> 8) - (1 << 23)) / 4096.f;
        }
    }
    for (size_t f = 0; f < SF_LEN(fwidths); f++) {
        /* Every tap is drawn, the first fwidths[f] are used: the table is
         * whole before it is passed, whatever the width. */
        for (int k = 0; k < MAX_FWIDTH_AVX_CONV; k++) {
            state = state * 1664525u + 1013904223u;
            taps[k] = (float)(state >> 8) / (float)(1 << 24);
        }
        mismatches += sf_rows_one_fwidth(taps, fwidths[f], rows);
    }
    mu_assert("AVX2 vertical pass differs from the scalar sum", mismatches == 0);
    return NULL;
}
#endif /* ARCH_X86 */

/* filter_and_downscale() with the two calls spelled out, prescale 1. */
static void sf_reference_downscale(const SpeedInternalDimensions *dim, float kernelscale,
                                   float *frame, float *scratch, size_t float_stride)
{
    const size_t stride_px = float_stride / sizeof(float);
    float *curr_scale = scratch;
    float *tmpbuf = scratch + stride_px * dim->alloc_height;
    const int w = (int)dim->scaled_width;
    const int h = (int)dim->scaled_height;
    const int down_w = w >> SF_NUM_SCALES;
    const int down_h = h >> SF_NUM_SCALES;
    float taps[128];

    speed_get_antialias_filter(taps, SF_NUM_SCALES, kernelscale);
    vif_filter1d_s(taps, frame, curr_scale, tmpbuf, w, h, (int)float_stride, (int)float_stride,
                   vif_get_filter_size(1, kernelscale));
    vif_dec16_s(curr_scale, frame, w, h, (int)float_stride, (int)float_stride);

    vif_get_filter(taps, SF_NUM_SCALES, kernelscale);
    vif_filter1d_s(taps, frame, curr_scale, tmpbuf, down_w, down_h, (int)float_stride,
                   (int)float_stride, vif_get_filter_size(SF_NUM_SCALES, kernelscale));
    for (int i = 0; i < down_h; i++) {
        for (int j = 0; j < down_w; j++)
            frame[(size_t)i * stride_px + (size_t)j] -= curr_scale[(size_t)i * stride_px + j];
    }
}

typedef struct SfFrame {
    float *source;   /* the filled frame buffer, never filtered */
    float *expected; /* the two calls, scalar */
    float *actual;
    float *scratch;
    size_t float_stride; /* bytes */
    size_t floats;
} SfFrame;

/* speed_internal_filter_and_downscale() on a copy of the source under CPU
 * mask `mask`; 1 when the frame buffer differs from the reference. */
static int sf_downscale_differs(const SpeedInternalDimensions *dim, SpeedInternalOptions *opt,
                                const SfFrame *f, unsigned mask)
{
    (void)memcpy(f->actual, f->source, f->floats * sizeof(float));
    vmaf_set_cpu_flags_mask(mask);
    speed_internal_filter_and_downscale(dim, opt, f->actual, f->scratch, f->float_stride);
    vmaf_set_cpu_flags_mask(0);
    return memcmp(f->expected, f->actual, f->floats * sizeof(float)) != 0;
}

static char *sf_check_downscale(SfSize size, float kernelscale, uint32_t *state)
{
    SpeedInternalDimensions dim = {0};
    char method[] = "nearest";
    SpeedInternalOptions opt = {
        .speed_kernelscale = (double)kernelscale,
        .speed_prescale = 1.0,
        .speed_prescale_method = method,
    };
    mu_assert("speed_internal_init_dimensions refused the plane",
              speed_internal_init_dimensions(&dim, size.w, size.h, 1.0) == 0);
    SfFrame f = {.float_stride = speed_internal_float_stride(dim.alloc_width)};
    f.floats = (f.float_stride / sizeof(float)) * dim.alloc_height;
    /* SpEED's buffers are aligned_malloc()ed: the AVX2 convolution of the
     * second filter loads and stores aligned. */
    f.source = aligned_malloc(f.floats * sizeof(float), SF_ALIGN);
    f.expected = aligned_malloc(f.floats * sizeof(float), SF_ALIGN);
    f.actual = aligned_malloc(f.floats * sizeof(float), SF_ALIGN);
    f.scratch = aligned_malloc(2u * f.floats * sizeof(float), SF_ALIGN);
    int differs = 1;

    if (f.source && f.expected && f.actual && f.scratch) {
        sf_fill(f.source, f.floats, (int)(f.float_stride / sizeof(float)), size, 0, state);
        (void)memcpy(f.expected, f.source, f.floats * sizeof(float));
        vmaf_set_cpu_flags_mask(0);
        sf_reference_downscale(&dim, kernelscale, f.expected, f.scratch, f.float_stride);
        differs = sf_downscale_differs(&dim, &opt, &f, 0) +
                  sf_downscale_differs(&dim, &opt, &f, UINT_MAX);
    }
    aligned_free(f.source);
    aligned_free(f.expected);
    aligned_free(f.actual);
    aligned_free(f.scratch);
    if (differs)
        (void)fprintf(stderr, "%dx%d, kernelscale %g\n", size.w, size.h, (double)kernelscale);
    mu_assert("speed_internal_filter_and_downscale differs from the two calls", !differs);
    return NULL;
}

static char *test_filter_and_downscale(void)
{
    static const SfSize sizes[] = {{80, 80}, {81, 83}, {173, 131}, {288, 162}, {576, 324}};
    /* speed_kernelscale values the extractors accept (valid_kernelscales). */
    static const float kernelscales[] = {1.0f, 0.5f, 2.0f, 360.0f / 97.0f};
    uint32_t state = 987654321;

    vmaf_init_cpu();
    for (size_t s = 0; s < SF_LEN(sizes); s++) {
        for (size_t k = 0; k < SF_LEN(kernelscales); k++)
            mu_assert_msg(sf_check_downscale(sizes[s], kernelscales[k], &state));
    }
    vmaf_set_cpu_flags_mask(0);
    return NULL;
}

char *run_tests(void)
{
    static const MuTest tests[] = {
        MU_TEST(test_filter_dec16),
        MU_TEST(test_filter_and_downscale),
    };
    char *msg = mu_run_table(tests, MU_TABLE_LEN(tests));
#if ARCH_X86
    if (!msg && (vmaf_get_cpu_flags_x86() & VMAF_X86_CPU_FLAG_AVX2)) {
        static const MuTest avx2_tests[] = {
            MU_TEST(test_avx_rows),
        };
        msg = mu_run_table(avx2_tests, MU_TABLE_LEN(avx2_tests));
    }
#endif
    return msg;
}

/* NOLINTEND(modernize-use-nullptr) */
