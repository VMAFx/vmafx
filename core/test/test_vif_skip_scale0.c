/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Unit test: integer_vif vif_skip_scale0 option (CPU path).
 *
 * Gap closed: GPU backends historically did not expose vif_skip_scale0,
 * meaning their output for the scale0 score diverges from the CPU path when
 * a model requests this option.  The Python golden tests
 * (feature_extractor_test.py) cover the CPU path via bindings but there is
 * no C-level assertion that the option is plumbed correctly.
 *
 * This test closes the C-unit-test gap by verifying via the public API:
 *   1. vif_skip_scale0=true causes the scale0 score to be exactly 0.0
 *      (CPU reference behaviour, integer_vif.c:714).
 *   2. Without the option the scale0 score is a finite positive value,
 *      confirming the default=false path is exercised too.
 *
 * Feature-naming note: when vif_skip_scale0 is set the option system
 * appends the option alias "_ssclz" to each provided feature name.
 * The alias of "VMAF_integer_feature_vif_scale0_score" is
 * "integer_vif_scale0" (alias.c:109), so the score is stored under
 * "integer_vif_scale0_ssclz" when the option is active.
 *
 * The assertions establish the CPU ground truth that GPU backends must
 * match once vif_skip_scale0 is ported to CUDA/SYCL/Vulkan.
 *
 * No GPU hardware required; runs in the fast suite.
 */

#include <math.h>

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe, and this file mirrors
 * the C spelling of the surface it exercises. ADR-1138. */
#include <stdint.h>
#include <string.h>

#include "libvmaf/feature.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/picture.h"

#include "test.h"

/* Minimum frame size that satisfies VIF's 4-level pyramid (2^3 = 8 pixel
 * minimum along each axis after three halvings).  64x64 divides cleanly
 * and is small enough to keep the test fast. */
#define TEST_W 64u
#define TEST_H 64u

/* Fill a YUV420P 8-bpc picture with a flat luma value and neutral chroma. */
static int alloc_flat_pic(VmafPicture *pic, uint8_t luma_val)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, 8, TEST_W, TEST_H);
    if (err)
        return err;
    for (unsigned p = 0; p < 3; p++) {
        unsigned pw = (p == 0) ? TEST_W : TEST_W / 2;
        unsigned ph = (p == 0) ? TEST_H : TEST_H / 2;
        uint8_t val = (p == 0) ? luma_val : 128u;
        uint8_t *plane = (uint8_t *)pic->data[p];
        for (unsigned r = 0; r < ph; r++) {
            memset(plane + r * (size_t)pic->stride[p], val, pw);
        }
    }
    return 0;
}

/* Shared by both tests below: allocate the ref/dis pictures and run them
 * through vmaf_read_pictures (plus the EOS flush). Same messages and order
 * in both callers, so this is not duplicated per-test. */
static char *check_vif_skip_scale0_pictures(VmafContext *vmaf)
{
    VmafPicture ref_pic;
    VmafPicture dis_pic;
    int err = alloc_flat_pic(&ref_pic, 100u);
    mu_assert("ref picture alloc", err == 0);
    err = alloc_flat_pic(&dis_pic, 120u);
    mu_assert("dis picture alloc", err == 0);

    err = vmaf_read_pictures(vmaf, &ref_pic, &dis_pic, 0);
    mu_assert("vmaf_read_pictures should succeed", err == 0);

    /* Flush: pass NULL, NULL to trigger the EOS path in vmaf_read_pictures. */
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("flush should succeed", err == 0);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Test 1: vif_skip_scale0=true -> scale0 score == 0.0                */
/* ------------------------------------------------------------------ */

static char *check_vif_skip_scale0_true_setup(VmafContext **vmaf_out)
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE, .n_threads = 1};
    int err = vmaf_init(vmaf_out, cfg);
    mu_assert("vmaf_init should succeed", err == 0);
    if (err)
        return NULL;

    VmafFeatureDictionary *opts = NULL;
    err = vmaf_feature_dictionary_set(&opts, "vif_skip_scale0", "true");
    mu_assert("dictionary_set vif_skip_scale0 should succeed", err == 0);

    /* vmaf_use_feature() takes ownership of opts and frees it internally,
     * regardless of success or failure.  Do NOT call
     * vmaf_feature_dictionary_free() on opts after this call — that would be a
     * double-free (CWE-415).  See ADR-0806. */
    err = vmaf_use_feature(*vmaf_out, "vif", opts);
    opts = NULL; /* consumed by vmaf_use_feature */
    mu_assert("vmaf_use_feature(vif) with vif_skip_scale0 should succeed", err == 0);
    if (err) {
        (void)vmaf_close(*vmaf_out);
        return NULL;
    }
    return NULL;
}

static char *check_vif_skip_scale0_true_scores(VmafContext *vmaf)
{
    /* When vif_skip_scale0=true the FEATURE_PARAM flag causes the option
     * system to remap feature names: the alias "ssclz" is appended.
     * The alias of "VMAF_integer_feature_vif_scale0_score" is
     * "integer_vif_scale0", so the stored key is "integer_vif_scale0_ssclz". */
    double scale0 = -1.0;
    int err = vmaf_feature_score_at_index(vmaf, "integer_vif_scale0_ssclz", &scale0, 0);
    mu_assert("scale0 score retrieval should succeed", err == 0);
    mu_assert("vif_skip_scale0=true: scale0_score must be exactly 0.0", scale0 == 0.0);

    /* Scales 1-3 must still be finite and non-negative. */
    double scale1 = -1.0;
    err = vmaf_feature_score_at_index(vmaf, "integer_vif_scale1_ssclz", &scale1, 0);
    mu_assert("scale1 score retrieval should succeed", err == 0);
    mu_assert("scale1_score should be finite and non-negative", isfinite(scale1) && scale1 >= 0.0);
    return NULL;
}

static char *test_vif_skip_scale0_true(void)
{
    VmafContext *vmaf = NULL;
    char *msg = check_vif_skip_scale0_true_setup(&vmaf);
    if (msg)
        return msg;

    msg = check_vif_skip_scale0_pictures(vmaf);
    if (msg)
        return msg;

    msg = check_vif_skip_scale0_true_scores(vmaf);
    if (msg)
        return msg;

    (void)vmaf_close(vmaf);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Test 2: default (vif_skip_scale0=false) -> scale0 score > 0        */
/* ------------------------------------------------------------------ */

static char *check_vif_skip_scale0_false_setup(VmafContext **vmaf_out)
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE, .n_threads = 1};
    int err = vmaf_init(vmaf_out, cfg);
    mu_assert("vmaf_init should succeed", err == 0);
    if (err)
        return NULL;

    /* No opts_dict -> vif_skip_scale0 defaults to false; standard feature names. */
    err = vmaf_use_feature(*vmaf_out, "vif", NULL);
    mu_assert("vmaf_use_feature(vif) default should succeed", err == 0);
    if (err) {
        (void)vmaf_close(*vmaf_out);
        return NULL;
    }
    return NULL;
}

static char *check_vif_skip_scale0_false_scores(VmafContext *vmaf)
{
    double scale0 = -1.0;
    int err =
        vmaf_feature_score_at_index(vmaf, "VMAF_integer_feature_vif_scale0_score", &scale0, 0);
    mu_assert("scale0 score retrieval should succeed", err == 0);
    /* Without skip, scale0_score is a finite positive ratio. */
    mu_assert("vif_skip_scale0=false: scale0_score must be finite and > 0",
              isfinite(scale0) && scale0 > 0.0);
    return NULL;
}

static char *test_vif_skip_scale0_false(void)
{
    VmafContext *vmaf = NULL;
    char *msg = check_vif_skip_scale0_false_setup(&vmaf);
    if (msg)
        return msg;

    msg = check_vif_skip_scale0_pictures(vmaf);
    if (msg)
        return msg;

    msg = check_vif_skip_scale0_false_scores(vmaf);
    if (msg)
        return msg;

    (void)vmaf_close(vmaf);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_vif_skip_scale0_true);
    mu_run_test(test_vif_skip_scale0_false);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
