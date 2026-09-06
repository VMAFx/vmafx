/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Pixel-format edge coverage for the CPU extractor surface.
 *
 *  Motivation (ADR-0912 / Research-0912): the bulk of the unit-test
 *  suite exercises only YUV420P 8-bit because that is the common case
 *  in the Netflix golden gate and the existing benchmark fixtures.
 *  The chroma-stride and bit-depth-shift code paths that diverge for
 *  4:2:2 / 4:4:4 / 10-bit / 12-bit inputs are therefore largely
 *  unexercised at the unit-test layer — bugs there are caught only by
 *  the (slower) Python harness or the cross-backend parity gate.
 *
 *  This file plugs five focused, fast end-to-end smoke tests through
 *  the public CPU extractor surface (`vmaf_get_feature_extractor_by_name`
 *  + `vmaf_feature_extractor_context_extract`) so that any regression
 *  in `picture_compute_geometry`, the per-extractor `init()` chroma
 *  bookkeeping, or the HBD scoring loops is caught at the libvmaf
 *  unit-test layer rather than at the integration layer.
 *
 *  Coverage matrix added by this file:
 *
 *  | test                             | extractor | pix_fmt | bpc |
 *  | -------------------------------- | --------- | ------- | --- |
 *  | psnr_yuv422p_8bit_identical      | psnr      | 4:2:2   | 8   |
 *  | psnr_yuv444p_10bit_identical     | psnr      | 4:4:4   | 10  |
 *  | psnr_yuv420p_12bit_identical     | psnr      | 4:2:0   | 12  |
 *  | ssim_yuv422p_8bit_identical      | ssim      | 4:2:2   | 8   |
 *  | ciede_yuv422p_8bit_identical     | ciede     | 4:2:2   | 8   |
 *
 *  All five compare identical reference/distorted pictures and
 *  assert that the per-plane PSNR is at its `psnr_max` ceiling
 *  (luma 6*bpc + 12) and that the SSIM / CIEDE2000 numbers match
 *  their identical-input expected values. The point of the tests is
 *  not the numeric expectation — that is enforced elsewhere — but
 *  that init/extract complete without a -EINVAL / -ENOMEM / OOB-read
 *  fault for the non-default pixel-format / bit-depth combinations.
 *
 *  The ciede 4:2:2 case in particular exercises the chroma-upscale
 *  path in `ciede.c::init()` that allocates two scratch 4:4:4
 *  pictures and runs `scale_chroma_planes` (vs the 4:4:4 fast-path
 *  that bypasses the scratch allocation entirely); the SSIM 4:2:2
 *  case exercises the asymmetric `ss_hor=1, ss_ver=0` chroma
 *  geometry that does not appear in any other end-to-end test.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "feature/feature_extractor.h"
#include "feature/feature_collector.h"
#include "libvmaf/picture.h"
#include "test.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe, and this file mirrors
 * the C spelling of the surface it exercises. ADR-1138. */

/* Fill every plane of `pic` with a deterministic, non-trivial 8-bit
 * pattern so the extractor cannot short-circuit on an all-zero input
 * (cambi etc. degenerate; psnr_max would clamp to the ceiling for any
 * identical pair, so the pattern is purely defensive against future
 * extractors that special-case flat input). */
static void fill_pic_pattern_8(VmafPicture *pic)
{
    for (unsigned p = 0; p < 3; p++) {
        uint8_t *data = (uint8_t *)pic->data[p];
        if (!data)
            continue;
        const unsigned stride = pic->stride[p];
        for (unsigned i = 0; i < pic->h[p]; i++) {
            for (unsigned j = 0; j < pic->w[p]; j++) {
                /* Cheap rolling pattern; identical for ref and dist so
                 * the extractor sees a zero-error pair. */
                data[i * stride + j] = (uint8_t)((i * 7u + j * 13u + p * 53u) & 0xFFu);
            }
        }
    }
}

/* HBD variant — fills each plane with a deterministic 16-bit pattern
 * clamped to the bit-depth's peak so that downstream `peak * peak`
 * arithmetic in psnr_hbd stays well-defined. */
static void fill_pic_pattern_hbd(VmafPicture *pic, unsigned bpc)
{
    const uint16_t peak = (uint16_t)((1u << bpc) - 1u);
    for (unsigned p = 0; p < 3; p++) {
        uint16_t *data = (uint16_t *)pic->data[p];
        if (!data)
            continue;
        const unsigned stride = pic->stride[p] / 2u;
        for (unsigned i = 0; i < pic->h[p]; i++) {
            for (unsigned j = 0; j < pic->w[p]; j++) {
                const uint16_t v = (uint16_t)((i * 7u + j * 13u + p * 53u) & 0xFFFFu);
                data[i * stride + j] = v > peak ? peak : v;
            }
        }
    }
}

/* End-to-end harness used by every test below: allocates an identical
 * ref/dist pair at (pix_fmt, bpc, w, h), runs one extract() pass
 * through the public extractor surface, and asserts the per-plane
 * PSNR reaches its psnr_max ceiling (i.e. mse=0). Returns NULL on
 * success or a mu_assert-compatible failure message. */
static char *run_identical_psnr(enum VmafPixelFormat pix_fmt, unsigned bpc, unsigned w, unsigned h)
{
    int err = 0;
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("psnr");
    mu_assert("psnr extractor must be registered", fex && !strcmp(fex->name, "psnr"));

    VmafFeatureExtractorContext *fex_ctx = NULL;
    err = vmaf_feature_extractor_context_create(&fex_ctx, fex, NULL);
    mu_assert("psnr ctx create failed", !err);

    VmafPicture ref;
    VmafPicture dist;
    err |= vmaf_picture_alloc(&ref, pix_fmt, bpc, w, h);
    err |= vmaf_picture_alloc(&dist, pix_fmt, bpc, w, h);
    mu_assert("picture_alloc failed for (pix_fmt, bpc, w, h)", !err);

    if (bpc == 8u) {
        fill_pic_pattern_8(&ref);
        fill_pic_pattern_8(&dist);
    } else {
        fill_pic_pattern_hbd(&ref, bpc);
        fill_pic_pattern_hbd(&dist, bpc);
    }

    VmafFeatureCollector *vfc = NULL;
    err = vmaf_feature_collector_init(&vfc);
    mu_assert("feature_collector_init failed", !err);

    err = vmaf_feature_extractor_context_extract(fex_ctx, &ref, NULL, &dist, NULL, 0, vfc);
    mu_assert("extract failed (pix_fmt/bpc combination rejected?)", !err);

    /* Default psnr_max for mse=0 is (6 * bpc + 12) per the integer_psnr
     * init() fallback. Identical pictures must hit the ceiling. */
    const double expected_max = 6.0 * (double)bpc + 12.0;

    double psnr_y = -1.0;
    double psnr_cb = -1.0;
    double psnr_cr = -1.0;
    err |= vmaf_feature_collector_get_score(vfc, "psnr_y", &psnr_y, 0);
    err |= vmaf_feature_collector_get_score(vfc, "psnr_cb", &psnr_cb, 0);
    err |= vmaf_feature_collector_get_score(vfc, "psnr_cr", &psnr_cr, 0);
    mu_assert("collector_get_score failed (missing per-plane PSNR)", !err);

    mu_assert("psnr_y must equal psnr_max ceiling for identical pair",
              psnr_y >= expected_max - 1e-9 && psnr_y <= expected_max + 1e-9);
    mu_assert("psnr_cb must equal psnr_max ceiling for identical pair",
              psnr_cb >= expected_max - 1e-9 && psnr_cb <= expected_max + 1e-9);
    mu_assert("psnr_cr must equal psnr_max ceiling for identical pair",
              psnr_cr >= expected_max - 1e-9 && psnr_cr <= expected_max + 1e-9);

    err = vmaf_feature_extractor_context_close(fex_ctx);
    err |= vmaf_feature_extractor_context_destroy(fex_ctx);
    mu_assert("psnr ctx teardown failed", !err);

    vmaf_feature_collector_destroy(vfc);
    vmaf_picture_unref(&ref);
    vmaf_picture_unref(&dist);
    return NULL;
}

static char *test_psnr_yuv422p_8bit_identical(void)
{
    /* 4:2:2 8-bit, 320x240 — small enough for sub-millisecond extract,
     * large enough to exercise the chroma stride alignment path. */
    return run_identical_psnr(VMAF_PIX_FMT_YUV422P, 8u, 320u, 240u);
}

static char *test_psnr_yuv444p_10bit_identical(void)
{
    /* 4:4:4 10-bit — exercises the HBD scoring loop with chroma plane
     * dimensions equal to luma (ss_hor=ss_ver=0), the only case where
     * psnr_hbd loops over a full-resolution chroma plane. */
    return run_identical_psnr(VMAF_PIX_FMT_YUV444P, 10u, 320u, 240u);
}

static char *test_psnr_yuv420p_12bit_identical(void)
{
    /* 4:2:0 12-bit — the most common HDR pipeline depth (HDR10) and
     * the only one of {8, 10, 12, 16} that had zero CPU-extractor
     * coverage prior to this file. */
    return run_identical_psnr(VMAF_PIX_FMT_YUV420P, 12u, 320u, 240u);
}

static char *test_ssim_yuv422p_8bit_identical(void)
{
    int err = 0;
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("ssim");
    mu_assert("ssim extractor must be registered", fex && !strcmp(fex->name, "ssim"));

    VmafFeatureExtractorContext *fex_ctx = NULL;
    err = vmaf_feature_extractor_context_create(&fex_ctx, fex, NULL);
    mu_assert("ssim ctx create failed", !err);

    /* 4:2:2 — ss_hor=1, ss_ver=0 chroma geometry not exercised
     * end-to-end by any other test. 320x240 fits the integer_ssim
     * decimation / window constraints (>=8 px on every plane). */
    VmafPicture ref;
    VmafPicture dist;
    err |= vmaf_picture_alloc(&ref, VMAF_PIX_FMT_YUV422P, 8u, 320u, 240u);
    err |= vmaf_picture_alloc(&dist, VMAF_PIX_FMT_YUV422P, 8u, 320u, 240u);
    mu_assert("ssim picture_alloc failed for YUV422P 8-bit", !err);

    fill_pic_pattern_8(&ref);
    fill_pic_pattern_8(&dist);

    VmafFeatureCollector *vfc = NULL;
    err = vmaf_feature_collector_init(&vfc);
    mu_assert("ssim feature_collector_init failed", !err);

    err = vmaf_feature_extractor_context_extract(fex_ctx, &ref, NULL, &dist, NULL, 0, vfc);
    mu_assert("ssim extract failed on YUV422P 8-bit", !err);

    double score = -1.0;
    err = vmaf_feature_collector_get_score(vfc, "ssim", &score, 0);
    mu_assert("ssim score must be retrievable on YUV422P 8-bit", !err);
    /* Identical input must score 1.0 within float-rounding tolerance. */
    mu_assert("ssim must be 1.0 for identical YUV422P pair",
              score >= 1.0 - 1e-6 && score <= 1.0 + 1e-6);

    err = vmaf_feature_extractor_context_close(fex_ctx);
    err |= vmaf_feature_extractor_context_destroy(fex_ctx);
    mu_assert("ssim ctx teardown failed", !err);
    vmaf_feature_collector_destroy(vfc);
    vmaf_picture_unref(&ref);
    vmaf_picture_unref(&dist);
    return NULL;
}

static char *test_ciede_yuv422p_8bit_identical(void)
{
    int err = 0;
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("ciede");
    mu_assert("ciede extractor must be registered", fex && !strcmp(fex->name, "ciede"));

    VmafFeatureExtractorContext *fex_ctx = NULL;
    err = vmaf_feature_extractor_context_create(&fex_ctx, fex, NULL);
    mu_assert("ciede ctx create failed", !err);

    /* 4:2:2 forces ciede::init() to allocate the YUV444 scratch
     * pictures and dispatch `scale_chroma_planes` on every extract;
     * 4:4:4 input takes a fast-path that bypasses both. The two paths
     * had no end-to-end smoke before this test. */
    VmafPicture ref;
    VmafPicture dist;
    err |= vmaf_picture_alloc(&ref, VMAF_PIX_FMT_YUV422P, 8u, 320u, 240u);
    err |= vmaf_picture_alloc(&dist, VMAF_PIX_FMT_YUV422P, 8u, 320u, 240u);
    mu_assert("ciede picture_alloc failed for YUV422P 8-bit", !err);

    fill_pic_pattern_8(&ref);
    fill_pic_pattern_8(&dist);

    VmafFeatureCollector *vfc = NULL;
    err = vmaf_feature_collector_init(&vfc);
    mu_assert("ciede feature_collector_init failed", !err);

    err = vmaf_feature_extractor_context_extract(fex_ctx, &ref, NULL, &dist, NULL, 0, vfc);
    mu_assert("ciede extract failed on YUV422P 8-bit", !err);

    double score = -1.0;
    err = vmaf_feature_collector_get_score(vfc, "ciede2000", &score, 0);
    mu_assert("ciede2000 score must be retrievable on YUV422P 8-bit", !err);
    /* ciede2000 emits `45 - 20 * log10(mean_de00)`; identical pairs
     * have de00_sum = 0, so the formula evaluates to +inf. Any non-NaN
     * positive score is acceptable — chroma upscaling can introduce
     * sub-ULP roundtrip noise depending on the compiler / libm. What
     * we are guarding against is the YUV422P-specific init/extract
     * failure (-EINVAL / -ENOMEM / scratch-buffer OOB) that would
     * propagate as a negative or NaN score. */
    mu_assert("ciede2000 score must not be NaN", score == score);
    mu_assert("ciede2000 score must be positive for identical pair", score > 0.0);

    err = vmaf_feature_extractor_context_close(fex_ctx);
    err |= vmaf_feature_extractor_context_destroy(fex_ctx);
    mu_assert("ciede ctx teardown failed", !err);
    vmaf_feature_collector_destroy(vfc);
    vmaf_picture_unref(&ref);
    vmaf_picture_unref(&dist);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_psnr_yuv422p_8bit_identical);
    mu_run_test(test_psnr_yuv444p_10bit_identical);
    mu_run_test(test_psnr_yuv420p_12bit_identical);
    mu_run_test(test_ssim_yuv422p_8bit_identical);
    mu_run_test(test_ciede_yuv422p_8bit_identical);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
