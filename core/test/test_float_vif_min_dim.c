/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Regression test — float_vif must reject frames smaller than 7x7 at
 *  init() time with -EINVAL instead of proceeding into the scale-0
 *  Gaussian filter path, which uses a 17-tap kernel and requires at
 *  least 7 pixels in each dimension to avoid a double-free / out-of-
 *  bounds read in the filter scratch buffer.
 *
 *  Bug: float_vif init() allocated VIF scratch buffers unconditionally.
 *  When min(scaled_w, scaled_h) < 9 the 17-tap Gaussian filter at scale 0
 *  walks its reflect-101 mirror index out of the allocated region
 *  (half-width = 8, worst-case mirrored index = h - 9 which underflows for
 *  h < 9), triggering UB (ASan heap-buffer-overflow or double-free on
 *  close()).
 *
 *  Fix: float_vif init() now checks scaled_w < 9 || scaled_h < 9 and
 *  returns -EINVAL with a human-readable log message before any allocation.
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

/* Allocate priv, apply option defaults (so string fields like
 * vif_prescale_method are not NULL), call init(), then call close() and
 * free priv.  Returns the init() return code.
 *
 * float_vif has VMAF_OPT_TYPE_STRING options (vif_prescale_method) that are
 * dereferenced inside init() before the dimension guard fires.  Applying
 * defaults first via vmaf_option_set(opt, priv, NULL) avoids a strcmp(NULL,…)
 * crash for the acceptance tests.  The rejection tests (w/h < 9) return
 * -EINVAL before reaching the string option access, so they are unaffected. */
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
/* float_vif — reject frames below the 7x7 floor                      */
/* ------------------------------------------------------------------ */

static char *test_float_vif_rejects_1x1(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("float_vif");
    mu_assert("float_vif extractor missing", fex != NULL);
    int rc = invoke_init(fex, 1u, 1u);
    mu_assert("float_vif: init(1x1) must return -EINVAL", rc == -EINVAL);
    return NULL;
}

static char *test_float_vif_rejects_8x8(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("float_vif");
    mu_assert("float_vif extractor missing", fex != NULL);
    /* 17-tap filter half-width=8: mirror formula 2*h-ii-2 underflows for h<9 */
    int rc = invoke_init(fex, 8u, 8u);
    mu_assert("float_vif: init(8x8) must return -EINVAL (below 9-pixel floor)", rc == -EINVAL);
    return NULL;
}

static char *test_float_vif_rejects_Nx8(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("float_vif");
    mu_assert("float_vif extractor missing", fex != NULL);
    /* width above floor but height below */
    int rc = invoke_init(fex, 64u, 8u);
    mu_assert("float_vif: init(64x8) must return -EINVAL (height < 9)", rc == -EINVAL);
    return NULL;
}

static char *test_float_vif_rejects_8xN(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("float_vif");
    mu_assert("float_vif extractor missing", fex != NULL);
    /* height above floor but width below */
    int rc = invoke_init(fex, 8u, 64u);
    mu_assert("float_vif: init(8x64) must return -EINVAL (width < 9)", rc == -EINVAL);
    return NULL;
}

static char *test_float_vif_accepts_9x9(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("float_vif");
    mu_assert("float_vif extractor missing", fex != NULL);
    int rc = invoke_init(fex, 9u, 9u);
    mu_assert("float_vif: init(9x9) must succeed (exact minimum)", rc == 0);
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
    mu_run_test(test_float_vif_rejects_8x8);
    mu_run_test(test_float_vif_rejects_Nx8);
    mu_run_test(test_float_vif_rejects_8xN);
    mu_run_test(test_float_vif_accepts_9x9);
    mu_run_test(test_float_vif_accepts_576x324);
    return NULL;
}
