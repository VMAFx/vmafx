/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/* The float extractors that normalise samples like picture_copy() refuse picture bit
 * depths they do not scale. picture_copy() and the device twins that mirror it handled
 * 10, 12 and 16 bits only and read every other depth above 8 as 8-bit bytes, so
 * float_ssim, float_ms_ssim, float_adm, float_vif and float_motion returned wrong scores
 * for 9, 11, 13, 14 and 15-bit pictures without an error
 * (T-ODD-BIT-DEPTHS-SILENT-WRONG-FLOAT-SCORES-2026-10-07). This test pins the refusal
 * for every family at every unsupported depth, the accepted depths, and that an
 * extractor which scores odd depths correctly (psnr_hvs at 9 and 11 bits) still does. */

#include "test.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe, and this file mirrors
 * the C spelling of the surface it exercises. ADR-1138. */

#include "libvmaf/libvmaf.h"
#include "libvmaf/picture.h"

#include <errno.h>
#include <stdlib.h>

#define FRAME_SIZE 256U

/* Makes a context with `feature` registered; returns 0 and sets *out, or the first
 * error (the context is closed again on a registration failure). */
static int init_context(const char *feature, VmafContext **out)
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    *out = NULL;
    int err = vmaf_init(&vmaf, cfg);
    if (err)
        return err;
    err = vmaf_use_feature(vmaf, feature, NULL);
    if (err) {
        const int close_err = vmaf_close(vmaf);
        return close_err ? close_err : err;
    }
    *out = vmaf;
    return 0;
}

/* Registers `feature` on a fresh context, submits one frame pair at `bpc` and returns
 * the result of vmaf_read_pictures(); the context owns both pictures whatever that
 * result is (ADR-1431). A setup or teardown failure returns 1, 2 or 3, which matches
 * neither 0 nor -EINVAL, so the calling test fails on it. */
static int submit_at_bpc(const char *feature, unsigned bpc)
{
    VmafPicture ref;
    VmafPicture dist;
    if (vmaf_picture_alloc(&ref, VMAF_PIX_FMT_YUV420P, bpc, FRAME_SIZE, FRAME_SIZE) != 0)
        return 1;
    if (vmaf_picture_alloc(&dist, VMAF_PIX_FMT_YUV420P, bpc, FRAME_SIZE, FRAME_SIZE) != 0)
        return vmaf_picture_unref(&ref) != 0 ? 2 : 1;
    VmafContext *vmaf = NULL;
    if (init_context(feature, &vmaf) != 0) {
        const int unref_err = vmaf_picture_unref(&ref) | vmaf_picture_unref(&dist);
        return unref_err != 0 ? 2 : 1;
    }
    const int err = vmaf_read_pictures(vmaf, &ref, &dist, 0);
    return vmaf_close(vmaf) != 0 ? 3 : err;
}

static const char *const kFamilies[] = {"float_ssim", "float_ms_ssim", "float_adm", "float_vif",
                                        "float_motion"};

static char *test_float_families_refuse_unscaled_depths(void)
{
    static const unsigned kRefused[] = {9U, 11U, 13U, 14U, 15U};
    for (size_t f = 0; f < sizeof(kFamilies) / sizeof(kFamilies[0]); f++) {
        for (size_t i = 0; i < sizeof(kRefused) / sizeof(kRefused[0]); i++) {
            mu_assert("a float extractor accepted a depth it does not scale",
                      submit_at_bpc(kFamilies[f], kRefused[i]) == -EINVAL);
        }
    }
    return NULL;
}

static char *test_float_families_accept_scaled_depths(void)
{
    static const unsigned kAccepted[] = {8U, 10U, 12U, 16U};
    for (size_t f = 0; f < sizeof(kFamilies) / sizeof(kFamilies[0]); f++) {
        for (size_t i = 0; i < sizeof(kAccepted) / sizeof(kAccepted[0]); i++) {
            mu_assert("a float extractor refused a depth it scales",
                      submit_at_bpc(kFamilies[f], kAccepted[i]) == 0);
        }
    }
    return NULL;
}

static char *test_other_extractors_keep_odd_depths(void)
{
    mu_assert("psnr_hvs refused 9 bits", submit_at_bpc("psnr_hvs", 9U) == 0);
    mu_assert("psnr_hvs refused 11 bits", submit_at_bpc("psnr_hvs", 11U) == 0);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_float_families_refuse_unscaled_depths);
    mu_run_test(test_float_families_accept_scaled_depths);
    mu_run_test(test_other_extractors_keep_odd_depths);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
