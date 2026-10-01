/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Integer AIM is not clipped at 1; float AIM is (ADR-1417,
 * T-ADM-INTEGER-AIM-ABOVE-ONE-2026-10-01).
 *
 * AIM is the additive impairment the contrast-masking pass keeps, divided by
 * the DLM denominator, the reference's own detail. The two extractors finish
 * that ratio differently, and both follow upstream Netflix/vmaf:
 *
 *   integer_adm.c   *score_aim = aim_num / den;
 *   adm.c (float)   *score_aim = MIN(aim_num / aim_den, 1.0f);
 *
 * On a reference without detail the denominator is the noise floor alone, so
 * any visible additive impairment gives a ratio above 1. A flat grey 64x64
 * reference against the same picture with isolated patches scores
 * integer_aim 3.1755853876204676 (upstream master prints 3.175585) and
 * float aim exactly 1. Stage by stage the two pipelines agree to four
 * significant digits (numerators 64.66445 and 64.66458 over a common
 * denominator of 20.363002); the clip is the whole difference.
 *
 * adm3 = max(w * adm2 + (1 - w) * (1 - aim), adm_min_val) then differs too:
 * with the default model's weight of 0.7 and floor of 0.5 the integer
 * extractor gives 0.5 on this picture and the float one 0.7.
 *
 * The shipped vmaf_v1.0.16 models read VMAF_integer_feature_adm3_score, so a
 * clip added to the integer extractor would move their scores away from
 * upstream's on such content. No Netflix golden assertion has an integer AIM
 * above 1, so nothing but this test would notice. It pins both behaviours.
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
 * required Windows build compiles this TU with cl.exe. ADR-1138. */

#define FRAME_W 64u
#define FRAME_H 64u

/* What one run reads back. */
typedef struct {
    double adm2;
    double aim;
    double adm3;
} AdmScores;

/* The extractor, its options and the keys its scores are published under. */
typedef struct {
    const char *feature;
    const char *dlm_weight; /* NULL keeps every default */
    const char *min_val;
    const char *adm2_key;
    const char *aim_key;
    const char *adm3_key;
} Variant;

static const Variant INTEGER_DEFAULT = {
    "adm",
    NULL,
    NULL,
    "VMAF_integer_feature_adm2_score",
    "VMAF_integer_feature_aim_score",
    "VMAF_integer_feature_adm3_score",
};

static const Variant FLOAT_DEFAULT = {
    "float_adm",
    NULL,
    NULL,
    "VMAF_feature_adm2_score",
    "VMAF_feature_aim_score",
    "VMAF_feature_adm3_score",
};

/* The weight and the floor the default model vmaf_v1.0.16_3d0h sets. */
static const Variant INTEGER_MODEL = {
    "adm",
    "0.7",
    "0.5",
    "integer_adm2_dlmw_0.7_min_0.5",
    "integer_aim_dlmw_0.7_min_0.5",
    "integer_adm3_dlmw_0.7_min_0.5",
};

static const Variant FLOAT_MODEL = {
    "float_adm",
    "0.7",
    "0.5",
    "adm2_dlmw_0.7_min_0.5",
    "aim_dlmw_0.7_min_0.5",
    "adm3_dlmw_0.7_min_0.5",
};

static int alloc_grey(VmafPicture *pic)
{
    const int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, 8u, FRAME_W, FRAME_H);
    if (err) {
        return err;
    }
    for (unsigned p = 0; p < 3u; p++) {
        uint8_t *plane = (uint8_t *)pic->data[p];
        for (unsigned row = 0; row < pic->h[p]; row++) {
            (void)memset(plane + (row * pic->stride[p]), 128, pic->w[p]);
        }
    }
    return 0;
}

/* One white column next to three black ones, two rows high, every 16 pixels
 * from (1, 1): the picture of test_integer_adm_cm_threshold.c. */
static void draw_isolated_patches(VmafPicture *pic)
{
    static const uint8_t PATCH[4] = {255u, 0u, 0u, 0u};
    uint8_t *luma = (uint8_t *)pic->data[0];
    for (unsigned row = 1u; row + 1u < pic->h[0]; row += 16u) {
        for (unsigned col = 1u; col + 4u <= pic->w[0]; col += 16u) {
            (void)memcpy(luma + (row * pic->stride[0]) + col, PATCH, sizeof(PATCH));
            (void)memcpy(luma + ((row + 1u) * pic->stride[0]) + col, PATCH, sizeof(PATCH));
        }
    }
}

/* A flat reference against itself (`patches` == 0) or against the patches. */
static int feed_frame(VmafContext *vmaf, int patches)
{
    VmafPicture ref;
    VmafPicture dist;
    int err = alloc_grey(&ref);
    if (err) {
        return err;
    }
    err = alloc_grey(&dist);
    if (err) {
        (void)vmaf_picture_unref(&ref);
        return err;
    }
    if (patches) {
        draw_isolated_patches(&dist);
    }
    err = vmaf_read_pictures(vmaf, &ref, &dist, 0u);
    if (err) {
        return err;
    }
    return vmaf_read_pictures(vmaf, NULL, NULL, 0);
}

static int variant_options(const Variant *v, VmafFeatureDictionary **opts)
{
    *opts = NULL;
    if (!v->dlm_weight) {
        return 0;
    }
    int err = vmaf_feature_dictionary_set(opts, "adm_dlm_weight", v->dlm_weight);
    if (!err) {
        err = vmaf_feature_dictionary_set(opts, "adm_min_val", v->min_val);
    }
    if (err) {
        (void)vmaf_feature_dictionary_free(opts);
    }
    return err;
}

/* `cpumask` 0 lets the extractor dispatch every SIMD level the host has; all
 * bits set forces the scalar path. */
static int run(const Variant *v, int patches, uint64_t cpumask, AdmScores *out)
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE, .cpumask = cpumask};
    VmafContext *vmaf = NULL;
    VmafFeatureDictionary *opts = NULL;
    int err = vmaf_init(&vmaf, cfg);
    if (err) {
        return err;
    }
    err = variant_options(v, &opts);
    if (!err) {
        err = vmaf_use_feature(vmaf, v->feature, opts);
    }
    if (!err) {
        err = feed_frame(vmaf, patches);
    }
    if (!err) {
        err = vmaf_feature_score_at_index(vmaf, v->adm2_key, &out->adm2, 0u);
    }
    if (!err) {
        err = vmaf_feature_score_at_index(vmaf, v->aim_key, &out->aim, 0u);
    }
    if (!err) {
        err = vmaf_feature_score_at_index(vmaf, v->adm3_key, &out->adm3, 0u);
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

/* positive: the integer extractor reports the unclipped ratio, the value
 * upstream master prints as 3.175585. */
static char *test_integer_aim_is_the_unclipped_ratio(void)
{
    AdmScores s = {0.0, 0.0, 0.0};
    mu_assert("integer ADM failed on the isolated patches",
              !run(&INTEGER_DEFAULT, 1, ~(uint64_t)0, &s));
    if (!(fabs(s.aim - 3.175585) < 1e-6)) {
        (void)fprintf(stderr, "\n  integer_aim %.17g, upstream master 3.175585\n", s.aim);
    }
    mu_assert("integer AIM on the isolated patches is no longer upstream's 3.175585",
              fabs(s.aim - 3.175585) < 1e-6);
    mu_assert("integer adm2 on a flat reference is 1", bit_identical(s.adm2, 1.0));
    /* 0.5 * 1 + 0.5 * (1 - 3.1756) is below the default floor of 0. */
    mu_assert("integer adm3 with the default weight and floor is 0", bit_identical(s.adm3, 0.0));
    return NULL;
}

/* negative: the float extractor clips the same ratio to exactly 1, and a
 * picture without an impairment has an AIM of 0 in both. */
static char *test_float_aim_is_clipped_at_one(void)
{
    AdmScores s = {0.0, 0.0, 0.0};
    mu_assert("float ADM failed on the isolated patches", !run(&FLOAT_DEFAULT, 1, 0u, &s));
    mu_assert("float AIM on the isolated patches is clipped to 1", bit_identical(s.aim, 1.0));
    mu_assert("float adm3 with the default weight is 0.5", bit_identical(s.adm3, 0.5));

    AdmScores same = {0.0, 1.0, 0.0};
    mu_assert("integer ADM failed on identical pictures",
              !run(&INTEGER_DEFAULT, 0, ~(uint64_t)0, &same));
    mu_assert("integer AIM of identical pictures is 0", bit_identical(same.aim, 0.0));
    same.aim = 1.0;
    mu_assert("float ADM failed on identical pictures", !run(&FLOAT_DEFAULT, 0, 0u, &same));
    mu_assert("float AIM of identical pictures is 0", bit_identical(same.aim, 0.0));
    return NULL;
}

/* boundary: with the default model's weight and floor the unclipped AIM takes
 * integer adm3 to the floor, where the clipped one stops at the weight. */
static char *test_adm3_reaches_the_floor_only_unclipped(void)
{
    AdmScores integer = {0.0, 0.0, 0.0};
    AdmScores flt = {0.0, 0.0, 0.0};
    mu_assert("integer ADM failed with the model's weight and floor",
              !run(&INTEGER_MODEL, 1, ~(uint64_t)0, &integer));
    mu_assert("float ADM failed with the model's weight and floor",
              !run(&FLOAT_MODEL, 1, 0u, &flt));
    /* 0.7 * 1 + 0.3 * (1 - 3.1756) = 0.047, below the floor of 0.5. */
    mu_assert("integer adm3 sits on adm_min_val", bit_identical(integer.adm3, 0.5));
    /* 0.7 * 1 + 0.3 * (1 - 1). */
    mu_assert("float adm3 stops at adm_dlm_weight", bit_identical(flt.adm3, 0.7));
    return NULL;
}

/* Every SIMD level the host has, and AVX2 alone on an AVX-512 host (cpumask
 * bits 16 and 32), return the scalar path's AIM and adm3 bit for bit. */
static char *test_integer_aim_is_the_same_on_every_dispatch_level(void)
{
    static const uint64_t masks[] = {0u, 16u | 32u};
    AdmScores scalar = {0.0, 0.0, 0.0};
    mu_assert("integer ADM failed on the scalar path",
              !run(&INTEGER_DEFAULT, 1, ~(uint64_t)0, &scalar));
    for (size_t m = 0; m < sizeof(masks) / sizeof(masks[0]); m++) {
        AdmScores s = {0.0, 0.0, 0.0};
        mu_assert("integer ADM failed on a SIMD level", !run(&INTEGER_DEFAULT, 1, masks[m], &s));
        if (!bit_identical(s.aim, scalar.aim)) {
            (void)fprintf(stderr, "\n  cpumask %u: integer_aim %.17g, scalar %.17g\n",
                          (unsigned)masks[m], s.aim, scalar.aim);
        }
        mu_assert("integer AIM differs between a SIMD level and the scalar path",
                  bit_identical(s.aim, scalar.aim));
        mu_assert("integer adm3 differs between a SIMD level and the scalar path",
                  bit_identical(s.adm3, scalar.adm3));
    }
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_integer_aim_is_the_unclipped_ratio);
    mu_run_test(test_float_aim_is_clipped_at_one);
    mu_run_test(test_adm3_reaches_the_floor_only_unclipped);
    mu_run_test(test_integer_aim_is_the_same_on_every_dispatch_level);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
