/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Regression test for the two float-convolution border defects reported
 *  upstream as Netflix/vmaf#1582 (the mirror half is also the subject of
 *  Netflix/vmaf#1581):
 *
 *    1. `convolution_edge_s` / `_sq_s` / `_xy_s` bounced an out-of-range tap
 *       exactly once.  One bounce only lands in range when the plane is at
 *       least `radius + 1` samples across; below that the bounced index falls
 *       out the opposite side and the helper dereferences out of bounds
 *       (heap-buffer-overflow READ under ASan).
 *
 *    2. `convolution_x_c_s` / `convolution_y_c_s` derived the trailing border
 *       bound as `dim - (filter_width - radius)`, which is NEGATIVE for any
 *       plane narrower/shorter than the filter.  The trailing loop then began
 *       at a negative index and wrote `dst[i * dst_stride - 1]` /
 *       `dst[-dst_stride + j]` — a heap underflow WRITE.
 *
 *  Detection without a sanitizer: the source plane is embedded in a buffer
 *  poisoned with NaN everywhere outside the plane, and the destination plane
 *  is embedded in a NaN-poisoned buffer as well.  Any tap that escapes the
 *  plane reads a NaN and taints the output; any write that escapes the plane
 *  replaces a poison NaN with a finite value.  So:
 *
 *    - every in-plane output sample must be finite   (catches defect 1)
 *    - every out-of-plane guard sample must stay NaN (catches defect 2)
 *
 *  Both assertions fail on the pre-fix tree for planes smaller than the
 *  filter and pass after it.  `test_large_plane_bit_identical` pins the other
 *  half of the contract: for any plane at least `radius + 1` across the
 *  iterative fold must reproduce the single-bounce result bit-for-bit, so no
 *  in-contract score moves.
 */

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"

#include "cpu.h"
#include "feature/common/convolution.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but the Windows
 * MSVC legs compile the test tree with cl.exe, whose documented /std:clatest
 * C23 feature set does not include `nullptr`. Same carve-out and reasoning as
 * core/src/feature/float_motion.c. ADR-1138. */

/* Rows of NaN poison above and below the plane inside each buffer. */
#define GUARD_ROWS 4

/* Normalised 5-tap Gaussian, the motion workhorse. */
static const float kFilter5[5] = {0.054488685f, 0.244201342f, 0.402619947f, 0.244201342f,
                                  0.054488685f};

/* Normalised 3-tap Gaussian, the `motion_filter_size=3` option. */
static const float kFilter3[3] = {0.25f, 0.5f, 0.25f};

/* Scale-0 VIF filter width: half-width 8, so every plane below 9 exercised
 * the single-bounce defect.  A delta kernel keeps the expected output equal
 * to the input, which makes a NaN leak unambiguous. */
static const float kFilter17[17] = {
    0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f,
    0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
};

typedef struct {
    float *base;  /* poisoned allocation */
    float *plane; /* base + GUARD_ROWS * stride */
    size_t n;     /* total float count in base */
} PoisonBuf;

static int poison_buf_alloc(PoisonBuf *b, int stride, int height)
{
    b->n = (size_t)(height + 2 * GUARD_ROWS) * (size_t)stride;
    b->base = malloc(b->n * sizeof(float));
    if (!b->base)
        return -1;
    for (size_t i = 0; i < b->n; i++) {
        b->base[i] = NAN;
    }
    b->plane = b->base + (size_t)GUARD_ROWS * (size_t)stride;
    return 0;
}

static void poison_buf_free(PoisonBuf *b)
{
    free(b->base);
    b->base = NULL;
    b->plane = NULL;
}

/* Fill the w x h sub-rectangle with a finite ramp; leave the row tails and
 * the guard rows poisoned. */
static void poison_buf_fill_plane(PoisonBuf *b, int stride, int width, int height)
{
    for (int i = 0; i < height; i++) {
        for (int j = 0; j < width; j++) {
            b->plane[(size_t)i * (size_t)stride + (size_t)j] = (float)(i * width + j) + 1.0f;
        }
    }
}

/* Every sample outside the w x h destination rectangle must still be NaN. */
static int poison_buf_guards_intact(const PoisonBuf *b, int stride, int width, int height)
{
    for (size_t i = 0; i < (size_t)GUARD_ROWS * (size_t)stride; i++) {
        if (!isnan(b->base[i]))
            return 0;
    }
    const size_t after = (size_t)(GUARD_ROWS + height) * (size_t)stride;
    for (size_t i = after; i < b->n; i++) {
        if (!isnan(b->base[i]))
            return 0;
    }
    /* Row tails between `width` and `stride` are never a valid output. */
    for (int i = 0; i < height; i++) {
        for (int j = width; j < stride; j++) {
            if (!isnan(b->plane[(size_t)i * (size_t)stride + (size_t)j]))
                return 0;
        }
    }
    return 1;
}

static int plane_all_finite(const float *plane, int stride, int width, int height)
{
    for (int i = 0; i < height; i++) {
        for (int j = 0; j < width; j++) {
            if (!isfinite(plane[(size_t)i * (size_t)stride + (size_t)j]))
                return 0;
        }
    }
    return 1;
}

/* Run one (filter, w, h) case through the scalar y-then-x passes. */
static int run_case(const float *filter, int filter_width, int width, int height)
{
    const int stride = width + 8;
    PoisonBuf src = {0};
    PoisonBuf dst = {0};
    PoisonBuf tmp = {0};
    int ok = 0;

    if (poison_buf_alloc(&src, stride, height))
        goto out;
    if (poison_buf_alloc(&dst, stride, height))
        goto out;
    if (poison_buf_alloc(&tmp, stride, height))
        goto out;

    poison_buf_fill_plane(&src, stride, width, height);

    convolution_y_c_s(filter, filter_width, src.plane, tmp.plane, width, height, stride, stride, 1);
    convolution_x_c_s(filter, filter_width, tmp.plane, dst.plane, width, height, stride, stride, 1);

    ok = plane_all_finite(dst.plane, stride, width, height) &&
         poison_buf_guards_intact(&dst, stride, width, height) &&
         poison_buf_guards_intact(&tmp, stride, width, height);

out:
    poison_buf_free(&src);
    poison_buf_free(&dst);
    poison_buf_free(&tmp);
    return ok;
}

static char *test_scalar_5tap_small_planes(void)
{
    for (int h = 1; h <= 12; h++) {
        for (int w = 1; w <= 12; w++) {
            mu_assert("5-tap scalar convolution escaped the plane", run_case(kFilter5, 5, w, h));
        }
    }
    return NULL;
}

static char *test_scalar_3tap_small_planes(void)
{
    for (int h = 1; h <= 8; h++) {
        for (int w = 1; w <= 8; w++) {
            mu_assert("3-tap scalar convolution escaped the plane", run_case(kFilter3, 3, w, h));
        }
    }
    return NULL;
}

static char *test_scalar_17tap_small_planes(void)
{
    for (int h = 1; h <= 20; h++) {
        for (int w = 1; w <= 20; w++) {
            mu_assert("17-tap scalar convolution escaped the plane", run_case(kFilter17, 17, w, h));
        }
    }
    return NULL;
}

/* Explicit single-bounce reflect-101 index, valid only for size >= radius + 1.
 * This is the pre-fix formula, kept verbatim as the reference the iterative
 * fold has to reproduce bit-for-bit above the minimum. */
static int single_bounce(int idx, int size)
{
    if (idx < 0) {
        return -idx;
    }
    if (idx >= size) {
        return 2 * size - idx - 2;
    }
    return idx;
}

/* Reference y-then-x separable pass built from single_bounce(). */
static void reference_convolve(const float *src, float *scratch, float *dst, int width, int height,
                               int stride)
{
    const int radius = 2;

    for (int i = 0; i < height; i++) {
        for (int j = 0; j < width; j++) {
            float accum = 0.0f;
            for (int k = 0; k < 5; k++) {
                const int it = single_bounce(i - radius + k, height);
                accum += kFilter5[k] * src[it * stride + j];
            }
            scratch[i * stride + j] = accum;
        }
    }
    for (int i = 0; i < height; i++) {
        for (int j = 0; j < width; j++) {
            float accum = 0.0f;
            for (int k = 0; k < 5; k++) {
                const int jt = single_bounce(j - radius + k, width);
                accum += kFilter5[k] * scratch[i * stride + jt];
            }
            dst[i * stride + j] = accum;
        }
    }
}

/* Bit-exact float comparison without memcmp on a float object representation
 * (clang-tidy bugprone-suspicious-memory-comparison): copy both operands into
 * an integer of the same width and compare those. */
static int float_bits_equal(float a, float b)
{
    uint32_t ba = 0;
    uint32_t bb = 0;
    memcpy(&ba, &a, sizeof(ba));
    memcpy(&bb, &b, sizeof(bb));
    return ba == bb;
}

static int planes_bit_identical(const float *a, const float *b, int width, int height, int stride)
{
    for (int i = 0; i < height; i++) {
        for (int j = 0; j < width; j++) {
            if (!float_bits_equal(a[i * stride + j], b[i * stride + j])) {
                return 0;
            }
        }
    }
    return 1;
}

/* The fold must not perturb an in-contract plane: compare a 24x24 run against
 * the explicit single-bounce reference and assert bit-equality. */
static char *test_large_plane_bit_identical(void)
{
    enum { WIDTH = 24, HEIGHT = 24, STRIDE = 32 };
    const size_t n = (size_t)STRIDE * (size_t)HEIGHT;
    float *src = calloc(n, sizeof(float));
    float *dst = calloc(n, sizeof(float));
    float *tmp = calloc(n, sizeof(float));
    float *ref = calloc(n, sizeof(float));
    float *rtmp = calloc(n, sizeof(float));
    int identical = 0;

    if (src && dst && tmp && ref && rtmp) {
        for (int i = 0; i < HEIGHT; i++) {
            for (int j = 0; j < WIDTH; j++) {
                src[i * STRIDE + j] = (float)((i * 37 + j * 11) % 251) / 251.0f;
            }
        }

        convolution_y_c_s(kFilter5, 5, src, tmp, WIDTH, HEIGHT, STRIDE, STRIDE, 1);
        convolution_x_c_s(kFilter5, 5, tmp, dst, WIDTH, HEIGHT, STRIDE, STRIDE, 1);
        reference_convolve(src, rtmp, ref, WIDTH, HEIGHT, STRIDE);

        identical = planes_bit_identical(dst, ref, WIDTH, HEIGHT, STRIDE);
    }

    free(src);
    free(dst);
    free(tmp);
    free(ref);
    free(rtmp);
    mu_assert("iterative fold moved an in-contract result", identical);
    return NULL;
}

char *run_tests(void)
{
    vmaf_init_cpu();
    mu_run_test(test_scalar_5tap_small_planes);
    mu_run_test(test_scalar_3tap_small_planes);
    mu_run_test(test_scalar_17tap_small_planes);
    mu_run_test(test_large_plane_bit_identical);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
