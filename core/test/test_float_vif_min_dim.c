/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Regression test — float_vif must reject, at init() time and with -EINVAL,
 *  every frame the four-scale VIF ladder cannot process without walking its
 *  reflect-101 mirror out of the plane.
 *
 *  Original bug: float_vif init() allocated VIF scratch buffers
 *  unconditionally.  When min(scaled_w, scaled_h) < 9 the 17-tap Gaussian at
 *  scale 0 walks its mirror index out of the allocated region (half-width = 8,
 *  worst-case mirrored index = h - 9, which underflows for h < 9), triggering
 *  UB (ASan heap-buffer-overflow or double-free on close()).  Fixed by a
 *  `< 9` guard.
 *
 *  Netflix/vmaf#1582 (2026-09-03): that guard covered scale 0 only.
 *  compute_vif() halves the working dimension once per scale and re-convolves
 *  with the scale's own Gaussian ({17, 9, 5, 3} taps), so the real floor is
 *  max over s of `((filter_width_s / 2) + 1) << s` = 16.  Input in 9..15
 *  passed the old guard and reached the scale-3 convolution with a 1px plane;
 *  reproduced under ASan as a heap-buffer-overflow READ in
 *  convolution_internal.h.  The guard is now derived from
 *  vif_get_min_dim(kernelscale) instead of a hard-coded 9.
 *
 *  This file exercises the CPU path only (GPU paths require a live GPU
 *  device and are covered by per-backend smoke tests).
 */

#include <errno.h>
#include <stdlib.h>

#include "libvmaf/picture.h"

#include "opt.h"
#include "test.h"

#include "feature/feature_extractor.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but the Windows
 * MSVC legs compile the test tree with cl.exe, whose documented /std:clatest
 * C23 feature set does not include `nullptr`. Same carve-out and reasoning as
 * core/src/feature/float_motion.c. ADR-1138. */

/* Allocate priv, apply option defaults (so string fields like
 * vif_prescale_method are not NULL), call init(), then call close() and
 * free priv.  Returns the init() return code.
 *
 * float_vif has VMAF_OPT_TYPE_STRING options (vif_prescale_method) that are
 * dereferenced inside init() before the dimension guard fires.  Applying
 * defaults first via vmaf_option_set(opt, priv, NULL) avoids a strcmp(NULL,…)
 * crash for the acceptance tests.  The rejection tests return -EINVAL before
 * reaching the string option access, so they are unaffected. */
static int invoke_init(VmafFeatureExtractor *fex, unsigned w, unsigned h)
{
    void *priv = calloc(1, fex->priv_size);
    if (!priv)
        return -1;
    fex->priv = priv;

    if (fex->options) {
        for (unsigned i = 0; fex->options[i].name; i++) {
            int err = vmaf_option_set(&fex->options[i], priv, NULL);
            if (err) {
                free(priv);
                fex->priv = NULL;
                return err;
            }
        }
    }

    int rc = fex->init(fex, VMAF_PIX_FMT_YUV420P, 8u, w, h);
    if (fex->close)
        (void)fex->close(fex);
    free(priv);
    fex->priv = NULL;
    return rc;
}

/* ------------------------------------------------------------------ */
/* float_vif — reject frames below the four-scale ladder minimum       */
/* ------------------------------------------------------------------ */

static char *test_float_vif_rejects_1x1(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("float_vif");
    mu_assert("float_vif extractor missing", fex != NULL);
    int rc = invoke_init(fex, 1u, 1u);
    mu_assert("float_vif: init(1x1) must return -EINVAL", rc == -EINVAL);
    return NULL;
}

static char *test_float_vif_rejects_below_ladder_minimum(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("float_vif");
    mu_assert("float_vif extractor missing", fex != NULL);
    /* 17-tap filter half-width=8: mirror formula 2*h-ii-2 underflows for h<9 */
    mu_assert("float_vif: init(8x8) must return -EINVAL", invoke_init(fex, 8u, 8u) == -EINVAL);
    /* Netflix/vmaf#1582: 9..15 cleared the old scale-0-only floor of 9 but
     * still walked the scale-3 convolution (3-tap, needs >= 2) off the end of
     * a 1px plane — reproduced as an ASan heap-buffer-overflow at
     * convolution_internal.h. */
    mu_assert("float_vif: init(9x9) must return -EINVAL (scale-3 plane is 1px)",
              invoke_init(fex, 9u, 9u) == -EINVAL);
    mu_assert("float_vif: init(15x15) must return -EINVAL (scale-3 plane is 1px)",
              invoke_init(fex, 15u, 15u) == -EINVAL);
    return NULL;
}

static char *test_float_vif_rejects_Nx8(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("float_vif");
    mu_assert("float_vif extractor missing", fex != NULL);
    /* width above floor but height below */
    int rc = invoke_init(fex, 64u, 8u);
    mu_assert("float_vif: init(64x8) must return -EINVAL (height below minimum)", rc == -EINVAL);
    return NULL;
}

static char *test_float_vif_rejects_8xN(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("float_vif");
    mu_assert("float_vif extractor missing", fex != NULL);
    /* height above floor but width below */
    int rc = invoke_init(fex, 8u, 64u);
    mu_assert("float_vif: init(8x64) must return -EINVAL (width below minimum)", rc == -EINVAL);
    return NULL;
}

static char *test_float_vif_accepts_16x16(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("float_vif");
    mu_assert("float_vif extractor missing", fex != NULL);
    /* 16 is the exact four-scale minimum at kernelscale 1.0:
     * max over s of ((filter_width_s / 2) + 1) << s = max(9, 10, 12, 16). */
    int rc = invoke_init(fex, 16u, 16u);
    mu_assert("float_vif: init(16x16) must succeed (exact ladder minimum)", rc == 0);
    return NULL;
}

static char *test_float_vif_accepts_576x324(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("float_vif");
    mu_assert("float_vif extractor missing", fex != NULL);
    int rc = invoke_init(fex, 576u, 324u);
    mu_assert("float_vif: init(576x324) must succeed (Netflix golden resolution)", rc == 0);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_float_vif_rejects_1x1);
    mu_run_test(test_float_vif_rejects_below_ladder_minimum);
    mu_run_test(test_float_vif_rejects_Nx8);
    mu_run_test(test_float_vif_rejects_8xN);
    mu_run_test(test_float_vif_accepts_16x16);
    mu_run_test(test_float_vif_accepts_576x324);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
