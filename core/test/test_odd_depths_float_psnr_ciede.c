/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * `float_psnr` and `ciede` at every depth the engine reads (RC4 WP13 follow-up,
 * ADR-2145): 9, 11, 13, 14 and 15 bits were refused (`-EINVAL`, the CLI printed
 * "problem reading pictures") although 8 to 16 bits are readable.
 *
 * Positive: both extractors score each of those depths. The oracle is
 * independent of the extractors' per-depth code: a sample of `b` bits and the
 * same sample shifted up to 16 bits are the same point of the 8-bit scale
 * (v / 2^(b - 8) = (v << (16 - b)) / 2^8), so `ciede` returns the same float for
 * both pictures and `float_psnr` the same noise; the `float_psnr` score is then
 * 10 log10(peak^2 / noise) with the peak (2^b - 1) / 2^(b - 8) written out here.
 * Negative: 7 and 17 bits are still refused by picture allocation, not scored.
 * Boundary: identical pictures report the ceiling 6 b + 12 (`float_psnr`) and 0
 * (`ciede` is +inf, 45 - 20 log10(0)).
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "libvmaf/libvmaf.h"
#include "libvmaf/picture.h"
#include "mu_table.h"
#include "test.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. ADR-1138. */

#define W 40u
#define H 24u

static unsigned sample8(unsigned plane, unsigned row, unsigned col, bool distorted)
{
    unsigned x = (row * 73856093u) ^ (col * 19349663u) ^ ((plane + 1u) * 83492791u);
    x ^= x >> 13;
    x *= 0x5bd1e995u;
    x ^= x >> 15;
    unsigned v = 16u + (x % 200u);
    if (distorted) {
        v += (row * 31u + col * 17u + plane) % 9u;
    }
    return v;
}

/* A 4:2:0 picture of `bpc` bits whose samples are the 8-bit sample times 2^(bpc - 8) plus a
 * fraction that only a deeper picture has: `extra` less than 2^(bpc - 8). */
static int make_picture(VmafPicture *pic, unsigned bpc, bool distorted)
{
    const int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, bpc, W, H);
    if (err) {
        return err;
    }
    for (unsigned p = 0; p < 3u; p++) {
        for (unsigned row = 0; row < pic->h[p]; row++) {
            uint16_t *const line =
                (uint16_t *)((uint8_t *)pic->data[p] + (size_t)row * pic->stride[p]);
            uint8_t *const line8 = (uint8_t *)pic->data[p] + (size_t)row * pic->stride[p];
            for (unsigned col = 0; col < pic->w[p]; col++) {
                const unsigned v = sample8(p, row, col, distorted) << (bpc > 8u ? bpc - 8u : 0u);
                if (bpc > 8u) {
                    line[col] = (uint16_t)v;
                } else {
                    line8[col] = (uint8_t)v;
                }
            }
        }
    }
    return 0;
}

/* The picture of `bpc` bits and the same samples as a 16-bit picture. */
static int make_pair_at(VmafPicture *ref, VmafPicture *dist, unsigned bpc)
{
    return make_picture(ref, bpc, false) | make_picture(dist, bpc, true);
}

/* The score of feature `name` of one frame of `bpc` bits; NaN on an error. */
static double score_of(const char *name, unsigned bpc, bool identical)
{
    /* ciede reports `ciede2000`. */
    const char *const score_name = strcmp(name, "ciede") == 0 ? "ciede2000" : name;
    const VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    VmafPicture ref;
    VmafPicture dist;
    double score = NAN;
    if (vmaf_init(&vmaf, cfg) || vmaf_use_feature(vmaf, name, NULL)) {
        return score;
    }
    if (make_pair_at(&ref, &dist, bpc) == 0) {
        if (identical) {
            (void)vmaf_picture_unref(&dist);
            (void)make_picture(&dist, bpc, false);
        }
        const int err = vmaf_read_pictures(vmaf, &ref, &dist, 0) ||
                        vmaf_read_pictures(vmaf, NULL, NULL, 0) ||
                        vmaf_feature_score_at_index(vmaf, score_name, &score, 0);
        if (err) {
            score = NAN;
        }
    }
    (void)vmaf_close(vmaf);
    return score;
}

static const unsigned odd_depths[] = {9u, 11u, 13u, 14u, 15u};

/* ciede: the same samples as a 16-bit picture give the same score, bit for bit. */
static char *test_ciede_odd_depths(void)
{
    const double want = score_of("ciede", 16u, false);
    mu_assert("ciede scores 16 bits", isfinite(want));
    for (size_t k = 0; k < sizeof(odd_depths) / sizeof(odd_depths[0]); k++) {
        const double got = score_of("ciede", odd_depths[k], false);
        mu_assert("ciede scores this depth", isfinite(got));
        mu_assert("ciede equals the same samples at 16 bits", got == want);
        const double same = score_of("ciede", odd_depths[k], true);
        mu_assert("identical pictures: no colour difference (45 - 20 log10(0) = +inf)",
                  isinf(same) && same > 0.0);
    }
    return NULL;
}

/* float_psnr: 10 log10(peak^2 / noise) with the noise of the 16-bit picture, the peak of the depth
 * written out as (2^b - 1) / 2^(b - 8); identical pictures report 6 b + 12. */
static char *test_float_psnr_odd_depths(void)
{
    const double at16 = score_of("float_psnr", 16u, false);
    mu_assert("float_psnr scores 16 bits", isfinite(at16));
    const double peak16 = 65535.0 / 256.0;
    const double noise = peak16 * peak16 / pow(10.0, at16 / 10.0);
    for (size_t k = 0; k < sizeof(odd_depths) / sizeof(odd_depths[0]); k++) {
        const unsigned b = odd_depths[k];
        const double got = score_of("float_psnr", b, false);
        mu_assert("float_psnr scores this depth", isfinite(got));
        const double peak = (double)((1u << b) - 1u) / (double)(1u << (b - 8u));
        const double want = 10.0 * log10(peak * peak / noise);
        if (fabs(got - want) >= 1e-9) {
            (void)fprintf(stderr, "\n  b=%u got %.12f want %.12f", b, got, want);
        }
        mu_assert("float_psnr = 10 log10(peak^2 / noise)", fabs(got - want) < 1e-9);
        mu_assert("identical pictures report the ceiling 6 b + 12",
                  score_of("float_psnr", b, true) == 6.0 * (double)b + 12.0);
    }
    return NULL;
}

/* Depths outside 8 to 16 are no picture at all. */
static char *test_depths_outside_the_engine(void)
{
    VmafPicture pic;
    mu_assert("7 bits", vmaf_picture_alloc(&pic, VMAF_PIX_FMT_YUV420P, 7u, W, H) != 0);
    mu_assert("17 bits", vmaf_picture_alloc(&pic, VMAF_PIX_FMT_YUV420P, 17u, W, H) != 0);
    return NULL;
}

char *run_tests(void)
{
    static const MuTest tests[] = {
        MU_TEST(test_ciede_odd_depths),
        MU_TEST(test_float_psnr_odd_depths),
        MU_TEST(test_depths_outside_the_engine),
    };
    return mu_run_table(tests, MU_TABLE_LEN(tests));
}

/* NOLINTEND(modernize-use-nullptr) */
