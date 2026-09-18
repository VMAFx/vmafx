/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Integer ADM on bright 16-bit input (T-ADM-DWT2-16BIT-INT32-OVERFLOW-2026-09-18).
 *
 * The scale-0 vertical DWT pass forms a four-tap response before normalising
 * it. The low-pass taps (15826, 27411, 7345, -4240) put up to 50582 * 65535
 * into that sum for a bright 16-bit column, past INT32_MAX, and the scalar
 * reference and the AVX2 / AVX-512 16-bit kernels all summed it in int32:
 * signed overflow, undefined behaviour. The normalised value fits in int32,
 * so wrapping hardware scores it correctly and no output comparison can see
 * the defect. The sanitizer lane can: it builds with -fsanitize=undefined and
 * halts on the first report. This test drives every dispatch level through
 * the top of the 16-bit range and requires SIMD and scalar to agree bit for
 * bit, so that lane covers each kernel.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test.h"

#include "libvmaf/libvmaf.h"
#include "libvmaf/picture.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe, and this file mirrors
 * the C spelling of the surface it exercises. ADR-1138. */

#define NUM_FRAMES 2u
#define NUM_SCALES 4u
#define BPC 16u

/* Luma draws from [BRIGHT_FLOOR, 65535]. The first three low-pass taps sum to
 * 50582, so a column whose first three samples all reach 42456 overflows the
 * int32 partial sum (42456 * 50582 > INT32_MAX); every column here does. */
#define BRIGHT_FLOOR 49152u

typedef struct {
    unsigned w;
    unsigned h;
} Geometry;

/* 96x64 keeps the SIMD kernels' vector loops short; 176x144 gives them full
 * iterations and a tail. */
static const Geometry GEOMETRIES[] = {{96u, 64u}, {176u, 144u}};
#define NUM_GEOMETRIES (sizeof(GEOMETRIES) / sizeof(GEOMETRIES[0]))

static const char *const SCALE_KEYS[NUM_SCALES] = {
    "integer_adm_scale0",
    "integer_adm_scale1",
    "integer_adm_scale2",
    "integer_adm_scale3",
};

/* xorshift32: reproducible noise without libc rand(). */
static uint32_t next_noise(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static int fill_bright(VmafPicture *pic, Geometry g, uint32_t seed)
{
    const int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, BPC, g.w, g.h);
    if (err) {
        return err;
    }
    uint32_t state = seed;
    const uint32_t span = 65536u - BRIGHT_FLOOR;
    for (unsigned p = 0; p < 3u; p++) {
        uint16_t *plane = (uint16_t *)pic->data[p];
        const size_t stride = pic->stride[p] / sizeof(uint16_t);
        for (unsigned row = 0; row < pic->h[p]; row++) {
            for (unsigned col = 0; col < pic->w[p]; col++) {
                plane[(row * stride) + col] =
                    (p == 0u) ? (uint16_t)(BRIGHT_FLOOR + (next_noise(&state) % span)) : 32768u;
            }
        }
    }
    return 0;
}

/* Reference and distorted frames draw from independent streams. */
static int feed_frames(VmafContext *vmaf, Geometry g)
{
    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        VmafPicture ref;
        VmafPicture dist;
        int err = fill_bright(&ref, g, 0x9e3779b9u + i);
        if (err) {
            return err;
        }
        err = fill_bright(&dist, g, 0x85ebca6bu + i);
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

/* `cpumask` 0 lets the extractor dispatch every SIMD level the host has; all
 * bits set forces the scalar path. */
static int run_adm(Geometry g, uint64_t cpumask, double scores[NUM_FRAMES * NUM_SCALES])
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE, .cpumask = cpumask};
    VmafContext *vmaf = NULL;
    int err = vmaf_init(&vmaf, cfg);
    if (err) {
        return err;
    }
    err = vmaf_use_feature(vmaf, "adm", NULL);
    if (!err) {
        err = feed_frames(vmaf, g);
    }
    for (unsigned k = 0; !err && k < NUM_FRAMES * NUM_SCALES; k++) {
        err = vmaf_feature_score_at_index(vmaf, SCALE_KEYS[k % NUM_SCALES], &scores[k],
                                          k / NUM_SCALES);
    }
    (void)vmaf_close(vmaf);
    return err;
}

/* Bit-pattern equality; memcmp() over floating-point operands is what
 * clang-tidy rightly flags, so compare the payloads as integers. */
static int bit_identical(double a, double b)
{
    uint64_t a_bits = 0;
    uint64_t b_bits = 0;
    (void)memcpy(&a_bits, &a, sizeof(a_bits));
    (void)memcpy(&b_bits, &b, sizeof(b_bits));
    return a_bits == b_bits;
}

/* Every SIMD level the host has, then AVX2 alone (cpumask bits 16 and 32 turn
 * off AVX-512 and AVX-512 ICL), so an AVX-512 host runs both x86 kernels. */
static const uint64_t SIMD_MASKS[] = {0u, 16u | 32u};
#define NUM_SIMD_MASKS (sizeof(SIMD_MASKS) / sizeof(SIMD_MASKS[0]))

static char *check_scalar_scores(Geometry g, const double scalar[NUM_FRAMES * NUM_SCALES])
{
    for (unsigned k = 0; k < NUM_FRAMES * NUM_SCALES; k++) {
        if (!isfinite(scalar[k]) || scalar[k] < 0.0) {
            (void)fprintf(stderr, "\n  %ux%u frame %u %s: scalar %.17g\n", g.w, g.h, k / NUM_SCALES,
                          SCALE_KEYS[k % NUM_SCALES], scalar[k]);
            return "integer ADM scored bright 16-bit input as non-finite or negative";
        }
    }
    return NULL;
}

static char *check_geometry(Geometry g)
{
    double scalar[NUM_FRAMES * NUM_SCALES] = {0};
    mu_assert("integer ADM failed on bright 16-bit input (scalar)",
              !run_adm(g, ~(uint64_t)0, scalar));
    char *msg = check_scalar_scores(g, scalar);
    if (msg) {
        return msg;
    }
    for (size_t m = 0; m < NUM_SIMD_MASKS; m++) {
        double simd[NUM_FRAMES * NUM_SCALES] = {0};
        mu_assert("integer ADM failed on bright 16-bit input (SIMD)",
                  !run_adm(g, SIMD_MASKS[m], simd));
        for (unsigned k = 0; k < NUM_FRAMES * NUM_SCALES; k++) {
            if (!bit_identical(simd[k], scalar[k])) {
                (void)fprintf(stderr,
                              "\n  %ux%u cpumask %u frame %u %s: SIMD %.17g vs scalar %.17g\n", g.w,
                              g.h, (unsigned)SIMD_MASKS[m], k / NUM_SCALES,
                              SCALE_KEYS[k % NUM_SCALES], simd[k], scalar[k]);
                return "integer ADM SIMD dispatch differs from scalar on bright 16-bit input";
            }
        }
    }
    return NULL;
}

static char *test_integer_adm_bright_16bit(void)
{
    for (size_t i = 0; i < NUM_GEOMETRIES; i++) {
        char *msg = check_geometry(GEOMETRIES[i]);
        if (msg) {
            return msg;
        }
    }
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_integer_adm_bright_16bit);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
