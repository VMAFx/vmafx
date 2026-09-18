/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Integer ADM SIMD against scalar on full-range noise
 * (T-ADM-CM-SIMD-NOISE-NOT-BIT-EXACT-2026-09-18).
 *
 * The scale-0 contrast-masking threshold adds a centre tap of
 * (int16_t)(((ONE_BY_15 * |a|) + 2048) >> 12). For |a| above about 15360 the
 * shifted value no longer fits int16 and the scalar reference wraps it. The
 * AVX2 and AVX-512 kernels kept the 32-bit value, so they drifted from scalar
 * wherever the CSF-weighted band is that large: independent full-range noise
 * gets there, smooth content does not. This test scores such noise with the
 * default dispatch and with the scalar path and requires identical results.
 */

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

typedef struct {
    unsigned w;
    unsigned h;
} Geometry;

/* 576x324 is the size the divergence was measured at; the smaller two keep
 * the interior vector path to a few iterations per row. */
static const Geometry GEOMETRIES[] = {{96u, 64u}, {176u, 144u}, {576u, 324u}};
#define NUM_GEOMETRIES (sizeof(GEOMETRIES) / sizeof(GEOMETRIES[0]))

static const char *const SCALE_KEYS[NUM_SCALES] = {
    "integer_adm_scale0",
    "integer_adm_scale1",
    "integer_adm_scale2",
    "integer_adm_scale3",
};

/* xorshift32: reproducible full-range 8-bit noise without libc rand(). */
static uint32_t next_noise(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static int fill_noise(VmafPicture *pic, Geometry g, uint32_t seed)
{
    const int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, 8u, g.w, g.h);
    if (err) {
        return err;
    }
    uint32_t state = seed;
    uint8_t *luma = (uint8_t *)pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            luma[(row * pic->stride[0]) + col] = (uint8_t)(next_noise(&state) >> 24);
        }
    }
    for (unsigned p = 1; p < 3u; p++) {
        uint8_t *plane = (uint8_t *)pic->data[p];
        for (unsigned row = 0; row < pic->h[p]; row++) {
            (void)memset(plane + (row * pic->stride[p]), 128, pic->w[p]);
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
        int err = fill_noise(&ref, g, 0x9e3779b9u + i);
        if (err) {
            return err;
        }
        err = fill_noise(&dist, g, 0x85ebca6bu + i);
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
 * off AVX-512 and AVX-512 ICL), so an AVX-512 host checks both x86 kernels. */
static const uint64_t SIMD_MASKS[] = {0u, 16u | 32u};
#define NUM_SIMD_MASKS (sizeof(SIMD_MASKS) / sizeof(SIMD_MASKS[0]))

static char *check_geometry(Geometry g)
{
    double scalar[NUM_FRAMES * NUM_SCALES] = {0};
    mu_assert("integer ADM failed on noise (scalar)", !run_adm(g, ~(uint64_t)0, scalar));
    for (size_t m = 0; m < NUM_SIMD_MASKS; m++) {
        double simd[NUM_FRAMES * NUM_SCALES] = {0};
        mu_assert("integer ADM failed on noise (SIMD)", !run_adm(g, SIMD_MASKS[m], simd));
        for (unsigned k = 0; k < NUM_FRAMES * NUM_SCALES; k++) {
            if (!bit_identical(simd[k], scalar[k])) {
                (void)fprintf(stderr,
                              "\n  %ux%u cpumask %u frame %u %s: SIMD %.17g vs scalar %.17g\n", g.w,
                              g.h, (unsigned)SIMD_MASKS[m], k / NUM_SCALES,
                              SCALE_KEYS[k % NUM_SCALES], simd[k], scalar[k]);
                return "integer ADM SIMD dispatch differs from scalar on full-range noise";
            }
        }
    }
    return NULL;
}

static char *test_integer_adm_simd_matches_scalar_on_noise(void)
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
    mu_run_test(test_integer_adm_simd_matches_scalar_on_noise);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
