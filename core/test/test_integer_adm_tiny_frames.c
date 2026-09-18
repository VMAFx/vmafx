/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Integer ADM on the smallest frames the extractor accepts.
 *
 * For any frame dimension from 17 to 32 the fourth DWT level runs on a 3- or
 * 4-sample input, so its subsampled extent is 2. `dwt2_src_indices_1d()` then
 * restarted its mirrored-tail loop at index 0 (`n_half - 2`) and replaced the
 * i == 0 mirror {1, 0, 1, 2} with {-1, 0, 1, 2}: scale 3 read row and column -1,
 * i.e. the tail of the preceding slab band vertically and, horizontally, the
 * int32 in front of the `tmp_ref` allocation. The integer_adm_scale3 score was
 * built from whatever happened to be there, so it could change from one run to
 * the next on identical input.
 *
 * This test runs the extractor through the public API on geometries that put
 * one or both dimensions in that range, twice per geometry with the heap
 * scribbled on between the runs, and requires finite, bit-identical scores. On
 * the ASan CI lane the out-of-bounds read aborts the test outright.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"

#include "libvmaf/libvmaf.h"
#include "libvmaf/picture.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe. ADR-1138. */

#define NUM_FRAMES 2u
#define NUM_SCALES 4u
#define SCRIBBLE_BYTES ((size_t)1 << 20)

typedef struct {
    unsigned w;
    unsigned h;
} Geometry;

/* At least one dimension from 17 to 32 in every row; the first row is the
 * extractor minimum. */
static const Geometry GEOMETRIES[] = {
    {17, 17}, {24, 32}, {32, 24}, {20, 64}, {64, 20}, {31, 48}, {48, 31}, {17, 96}, {96, 17},
};
#define NUM_GEOMETRIES (sizeof(GEOMETRIES) / sizeof(GEOMETRIES[0]))

static const char *const SCALE_KEYS[NUM_SCALES] = {
    "integer_adm_scale0",
    "integer_adm_scale1",
    "integer_adm_scale2",
    "integer_adm_scale3",
};

/* A smooth ramp; the distorted frame adds a small periodic error to it. */
static uint8_t sample(unsigned row, unsigned col, unsigned frame, unsigned plane, int distorted)
{
    unsigned v = (row * 5u + col * 3u + frame * 11u + plane * 29u) & 0xFFu;
    if (distorted) {
        v = (v + ((row * 7u + col + frame) % 17u)) & 0xFFu;
    }
    return (uint8_t)v;
}

static int fill_pic(VmafPicture *pic, Geometry g, unsigned frame, int distorted)
{
    const int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, 8u, g.w, g.h);
    if (err) {
        return err;
    }
    for (unsigned p = 0; p < 3u; p++) {
        uint8_t *plane = (uint8_t *)pic->data[p];
        for (unsigned row = 0; row < pic->h[p]; row++) {
            for (unsigned col = 0; col < pic->w[p]; col++) {
                plane[(row * pic->stride[p]) + col] = sample(row, col, frame, p, distorted);
            }
        }
    }
    return 0;
}

static int feed_frames(VmafContext *vmaf, Geometry g)
{
    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        VmafPicture ref;
        VmafPicture dist;
        int err = fill_pic(&ref, g, i, 0);
        if (err) {
            return err;
        }
        err = fill_pic(&dist, g, i, 1);
        if (err) {
            (void)vmaf_picture_unref(&ref);
            return err;
        }
        err = vmaf_read_pictures(vmaf, &ref, &dist, i);
        if (err) {
            return err;
        }
    }
    return vmaf_read_pictures(vmaf, NULL, NULL, 0);
}

static int read_scores(VmafContext *vmaf, double scores[NUM_FRAMES * NUM_SCALES])
{
    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        for (unsigned s = 0; s < NUM_SCALES; s++) {
            const int err =
                vmaf_feature_score_at_index(vmaf, SCALE_KEYS[s], &scores[(i * NUM_SCALES) + s], i);
            if (err) {
                return err;
            }
        }
    }
    return 0;
}

static int run_adm(Geometry g, double scores[NUM_FRAMES * NUM_SCALES])
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    int err = vmaf_init(&vmaf, cfg);
    if (err) {
        return err;
    }
    err = vmaf_use_feature(vmaf, "adm", NULL);
    if (!err) {
        err = feed_frames(vmaf, g);
    }
    if (!err) {
        err = read_scores(vmaf, scores);
    }
    (void)vmaf_close(vmaf);
    return err;
}

/* Leave a recognisable pattern in freed heap memory so a read of storage the
 * extractor never wrote is likely to see different bytes on the second run. */
static void scribble_heap(void)
{
    unsigned char *p = malloc(SCRIBBLE_BYTES);
    if (p) {
        (void)memset(p, 0x5A, SCRIBBLE_BYTES);
        free(p);
    }
}

static int scores_usable(const double scores[NUM_FRAMES * NUM_SCALES])
{
    for (unsigned k = 0; k < NUM_FRAMES * NUM_SCALES; k++) {
        if (!isfinite(scores[k])) {
            return 0;
        }
    }
    return 1;
}

/* Bit-pattern equality; memcmp() over floating-point operands is what
 * clang-tidy rightly flags, so compare the payloads as integers. */
static int scores_bit_identical(const double a[NUM_FRAMES * NUM_SCALES],
                                const double b[NUM_FRAMES * NUM_SCALES])
{
    for (unsigned k = 0; k < NUM_FRAMES * NUM_SCALES; k++) {
        uint64_t a_bits = 0;
        uint64_t b_bits = 0;
        (void)memcpy(&a_bits, &a[k], sizeof(a_bits));
        (void)memcpy(&b_bits, &b[k], sizeof(b_bits));
        if (a_bits != b_bits) {
            return 0;
        }
    }
    return 1;
}

static char *check_geometry(Geometry g)
{
    double first[NUM_FRAMES * NUM_SCALES] = {0};
    double second[NUM_FRAMES * NUM_SCALES] = {0};

    const int err_first = run_adm(g, first);
    scribble_heap();
    const int err_second = run_adm(g, second);
    if (err_first || err_second) {
        (void)fprintf(stderr, "  %ux%u: extraction failed (%d, %d)\n", g.w, g.h, err_first,
                      err_second);
    }
    mu_assert("integer ADM failed on a tiny frame", !err_first && !err_second);
    mu_assert("integer ADM produced a non-finite score on a tiny frame", scores_usable(first));
    const int identical = scores_bit_identical(first, second);
    if (!identical) {
        (void)fprintf(stderr, "  %ux%u: scale3 %.17g vs %.17g\n", g.w, g.h, first[NUM_SCALES - 1],
                      second[NUM_SCALES - 1]);
    }
    mu_assert("integer ADM scores differ between identical runs on a tiny frame", identical);
    return NULL;
}

static char *test_integer_adm_tiny_frames_deterministic(void)
{
    for (size_t k = 0; k < NUM_GEOMETRIES; k++) {
        char *msg = check_geometry(GEOMETRIES[k]);
        if (msg) {
            return msg;
        }
    }
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_integer_adm_tiny_frames_deterministic);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
