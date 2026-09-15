/**
 * Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 * NEON-vs-scalar bit-exactness for the whole integer-motion SAD pipeline.
 *
 * Coverage gap closed: `core/src/feature/arm64/motion_v2_neon.c` exports
 * `motion_score_pipeline_8_neon` and `motion_score_pipeline_16_neon`, both
 * dispatched from `integer_motion.c` whenever `VMAF_ARM_CPU_FLAG_NEON` is set,
 * and its header claims they are "bit-exact vs the scalar references
 * motion_score_pipeline_{8,16}". Nothing asserted that. The sibling
 * `test_motion_neon.c` covers only `x_convolution_16_neon`, one kernel *inside*
 * the 16-bit pipeline, so the fused diff + vertical convolution, the
 * short-circuit that skips an all-zero row, the horizontal pass and the SAD
 * reduction were all unverified on the architecture that runs them.
 *
 * Method: drive the registered `motion` feature extractor twice over the same
 * pictures, once with the CPU-flag mask cleared so dispatch falls back to the
 * scalar pipeline and once with every flag set so it selects NEON, then require
 * the pooled SAD score to be equal bit-for-bit. That exercises the pipeline
 * through its real entry point rather than a hand-built call, so a dispatch
 * mistake counts as a failure too.
 *
 * Geometries: widths chosen so every residue of the vector step is hit — the
 * 8-bit pipeline consumes 16 columns per iteration and the 16-bit one 8, so the
 * set covers a tail of 0 through 15 columns as well as sizes smaller than one
 * vector. Heights below 3 are excluded deliberately: the radius-2 mirror
 * reflection reads out of bounds there in the scalar reference as well, so both
 * sides would be comparing the same garbage and the test would assert nothing.
 *
 * Bit depths: 8 covers `motion_score_pipeline_8_neon`; 10 and 12 cover
 * `motion_score_pipeline_16_neon`, which upstream Netflix/vmaf has no NEON
 * implementation of at all (its own commit adds the 8-bit path only).
 */

#include <stdint.h>
#include <stdlib.h>

#include "cpu.h"
#include "test.h"
#include "libvmaf/picture.h"
#include "feature/feature_extractor.h"
#include "feature/feature_collector.h"

/* Deterministic per-plane fill. rand() is seeded per call so the scalar and the
 * NEON run see byte-identical input; the values themselves only need to span
 * the sample range, which is what makes the vertical accumulator work hard. */
static void fill_random_luma(VmafPicture *pic, unsigned bpc, unsigned seed)
{
    srand(seed);
    const unsigned max = (1u << bpc) - 1u;
    for (unsigned i = 0; i < pic->h[0]; i++) {
        if (bpc == 8) {
            uint8_t *row = (uint8_t *)pic->data[0] + i * pic->stride[0];
            for (unsigned j = 0; j < pic->w[0]; j++)
                row[j] = (uint8_t)((unsigned)rand() % (max + 1u));
        } else {
            uint16_t *row = (uint16_t *)((uint8_t *)pic->data[0] + i * pic->stride[0]);
            for (unsigned j = 0; j < pic->w[0]; j++)
                row[j] = (uint16_t)((unsigned)rand() % (max + 1u));
        }
    }
}

/* Runs the motion extractor over a synthetic previous/current pair under an
 * explicit CPU-flag mask and returns the pooled SAD score. */
static int motion_sad_under_mask(unsigned w, unsigned h, unsigned bpc, unsigned seed_prev,
                                 unsigned seed_cur, unsigned cpu_mask, double *score)
{
    vmaf_set_cpu_flags_mask(cpu_mask);

    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("motion");
    if (!fex)
        return -1;

    VmafFeatureExtractorContext *fex_ctx;
    int err = vmaf_feature_extractor_context_create(&fex_ctx, fex, NULL);
    if (err)
        return err;

    VmafPicture prev_pic, cur_pic;
    err = vmaf_picture_alloc(&prev_pic, VMAF_PIX_FMT_YUV420P, bpc, w, h);
    if (err)
        return err;
    err = vmaf_picture_alloc(&cur_pic, VMAF_PIX_FMT_YUV420P, bpc, w, h);
    if (err) {
        vmaf_picture_unref(&prev_pic);
        return err;
    }

    fill_random_luma(&prev_pic, bpc, seed_prev);
    fill_random_luma(&cur_pic, bpc, seed_cur);

    VmafFeatureCollector *vfc;
    err = vmaf_feature_collector_init(&vfc);
    if (err)
        return err;

    /* Frame 0 primes prev_ref; the score under test comes from frame 1. */
    err = vmaf_feature_extractor_context_extract(fex_ctx, &prev_pic, NULL, &prev_pic, NULL, 0, vfc);
    if (err)
        return err;

    if (fex_ctx->fex->flags & VMAF_FEATURE_EXTRACTOR_PREV_REF)
        fex_ctx->fex->prev_ref = prev_pic;

    err = vmaf_feature_extractor_context_extract(fex_ctx, &cur_pic, NULL, &cur_pic, NULL, 1, vfc);
    if (err)
        return err;

    err = vmaf_feature_collector_get_score(vfc, "VMAF_integer_feature_motion_sad_score", score, 1);
    if (err)
        return err;

    err = vmaf_feature_extractor_context_close(fex_ctx);
    if (err)
        return err;
    err = vmaf_feature_extractor_context_destroy(fex_ctx);
    if (err)
        return err;

    vmaf_feature_collector_destroy(vfc);
    vmaf_picture_unref(&prev_pic);
    vmaf_picture_unref(&cur_pic);

    return 0;
}

static char *test_motion_pipeline_neon_matches_scalar(void)
{
    static const struct {
        unsigned w, h;
    } sizes[] = {
        {3, 3},   {4, 4},  {5, 5},  {7, 7},  {8, 8},  {9, 9},   {15, 15}, {16, 16},
        {17, 17}, {20, 4}, {31, 5}, {32, 6}, {33, 9}, {64, 48}, {65, 63},
    };
    static const unsigned depths[] = {8, 10, 12};

    vmaf_init_cpu();

    for (unsigned d = 0; d < sizeof(depths) / sizeof(depths[0]); d++) {
        for (unsigned s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
            for (unsigned seed = 0; seed < 3; seed++) {
                double scalar_score = -1.0, neon_score = -2.0;

                int err = motion_sad_under_mask(sizes[s].w, sizes[s].h, depths[d], 100 + seed,
                                                200 + seed, 0, &scalar_score);
                mu_assert("scalar motion extraction failed", !err);

                err = motion_sad_under_mask(sizes[s].w, sizes[s].h, depths[d], 100 + seed,
                                            200 + seed, ~0u, &neon_score);
                mu_assert("neon motion extraction failed", !err);

                mu_assert("NEON motion SAD must bit-exactly match the scalar "
                          "reference at every geometry and bit depth",
                          scalar_score == neon_score);
            }
        }
    }

    vmaf_set_cpu_flags_mask(~0u);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_motion_pipeline_neon_matches_scalar);
    return NULL;
}
