/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Research-0094 regression test — motion feature extractors must reject frames
 *  smaller than 3x3 at init() time with -EINVAL instead of reading
 *  out-of-bounds memory in the reflect-101 mirror-padding formula.
 *
 *  Bug: the 5-tap separable Gaussian uses the formula
 *    mirrored_idx = height - (i_tap - height + 2)
 *  for the bottom edge.  For height < 3 (radius + 1 = 3) the formula
 *  produces a negative index, resulting in a read of uninitialised memory
 *  (UB; ASan SEGV or garbage VMAF scores).
 *
 *  Fix: each of the three CPU motion extractors (motion, motion_v2,
 *  float_motion) now checks h < 3 || w < 3 in init() and returns -EINVAL
 *  with a human-readable message.  The same check is present on every
 *  GPU backend (CUDA, SYCL, Vulkan, HIP) that shares the formula.
 *
 *  This file exercises the CPU paths only (the GPU paths require a live
 *  GPU device; they are validated by the per-backend smoke tests).
 */

#include <errno.h>
#include <stdlib.h>

#include "libvmaf/picture.h"

#include "test.h"

#include "feature/feature_extractor.h"

/* Helper: allocate priv, call init(), then call close() and free priv.
 * Returns the init() return code.  The extractor's close() contract
 * tolerates partially-initialised state (same pattern as
 * test_float_ms_ssim_min_dim.c). */
static int invoke_init(VmafFeatureExtractor *fex, unsigned w, unsigned h)
{
    void *priv = calloc(1, fex->priv_size);
    if (!priv)
        return -1;
    fex->priv = priv;
    int rc = fex->init(fex, VMAF_PIX_FMT_YUV420P, 8u, w, h);
    if (fex->close)
        (void)fex->close(fex);
    free(priv);
    fex->priv = NULL;
    return rc;
}

/* ------------------------------------------------------------------ */
/* motion (integer_motion.c)                                          */
/* ------------------------------------------------------------------ */

static char *test_motion_rejects_1x1(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("motion");
    mu_assert("motion extractor missing", fex != NULL);
    int rc = invoke_init(fex, 1u, 1u);
    mu_assert("motion: init(1x1) must return -EINVAL", rc == -EINVAL);
    return NULL;
}

static char *test_motion_rejects_2x2(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("motion");
    mu_assert("motion extractor missing", fex != NULL);
    int rc = invoke_init(fex, 2u, 2u);
    mu_assert("motion: init(2x2) must return -EINVAL", rc == -EINVAL);
    return NULL;
}

static char *test_motion_rejects_1xN(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("motion");
    mu_assert("motion extractor missing", fex != NULL);
    /* 1-row frame: width above floor but height below */
    int rc = invoke_init(fex, 64u, 1u);
    mu_assert("motion: init(64x1) must return -EINVAL", rc == -EINVAL);
    return NULL;
}

static char *test_motion_accepts_3x3(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("motion");
    mu_assert("motion extractor missing", fex != NULL);
    int rc = invoke_init(fex, 3u, 3u);
    mu_assert("motion: init(3x3) must succeed (exact minimum)", rc == 0);
    return NULL;
}

static char *test_motion_accepts_576x324(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("motion");
    mu_assert("motion extractor missing", fex != NULL);
    int rc = invoke_init(fex, 576u, 324u);
    mu_assert("motion: init(576x324) must succeed (Netflix golden resolution)", rc == 0);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* motion_v2 (integer_motion_v2.c)                                    */
/* ------------------------------------------------------------------ */

static char *test_motion_v2_rejects_1x1(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("motion_v2");
    mu_assert("motion_v2 extractor missing", fex != NULL);
    int rc = invoke_init(fex, 1u, 1u);
    mu_assert("motion_v2: init(1x1) must return -EINVAL", rc == -EINVAL);
    return NULL;
}

static char *test_motion_v2_rejects_2x2(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("motion_v2");
    mu_assert("motion_v2 extractor missing", fex != NULL);
    int rc = invoke_init(fex, 2u, 2u);
    mu_assert("motion_v2: init(2x2) must return -EINVAL", rc == -EINVAL);
    return NULL;
}

static char *test_motion_v2_accepts_3x3(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("motion_v2");
    mu_assert("motion_v2 extractor missing", fex != NULL);
    int rc = invoke_init(fex, 3u, 3u);
    mu_assert("motion_v2: init(3x3) must succeed (exact minimum)", rc == 0);
    return NULL;
}

static char *test_motion_v2_accepts_576x324(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("motion_v2");
    mu_assert("motion_v2 extractor missing", fex != NULL);
    int rc = invoke_init(fex, 576u, 324u);
    mu_assert("motion_v2: init(576x324) must succeed (Netflix golden resolution)", rc == 0);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* float_motion (float_motion.c)                                      */
/* ------------------------------------------------------------------ */

static char *test_float_motion_rejects_1x1(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("float_motion");
    mu_assert("float_motion extractor missing", fex != NULL);
    int rc = invoke_init(fex, 1u, 1u);
    mu_assert("float_motion: init(1x1) must return -EINVAL", rc == -EINVAL);
    return NULL;
}

static char *test_float_motion_rejects_2x2(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("float_motion");
    mu_assert("float_motion extractor missing", fex != NULL);
    int rc = invoke_init(fex, 2u, 2u);
    mu_assert("float_motion: init(2x2) must return -EINVAL", rc == -EINVAL);
    return NULL;
}

static char *test_float_motion_accepts_3x3(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("float_motion");
    mu_assert("float_motion extractor missing", fex != NULL);
    int rc = invoke_init(fex, 3u, 3u);
    mu_assert("float_motion: init(3x3) must succeed (exact minimum)", rc == 0);
    return NULL;
}

static char *test_float_motion_accepts_576x324(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("float_motion");
    mu_assert("float_motion extractor missing", fex != NULL);
    int rc = invoke_init(fex, 576u, 324u);
    mu_assert("float_motion: init(576x324) must succeed (Netflix golden resolution)", rc == 0);
    return NULL;
}

char *run_tests(void)
{
    /* motion */
    mu_run_test(test_motion_rejects_1x1);
    mu_run_test(test_motion_rejects_2x2);
    mu_run_test(test_motion_rejects_1xN);
    mu_run_test(test_motion_accepts_3x3);
    mu_run_test(test_motion_accepts_576x324);
    /* motion_v2 */
    mu_run_test(test_motion_v2_rejects_1x1);
    mu_run_test(test_motion_v2_rejects_2x2);
    mu_run_test(test_motion_v2_accepts_3x3);
    mu_run_test(test_motion_v2_accepts_576x324);
    /* float_motion */
    mu_run_test(test_float_motion_rejects_1x1);
    mu_run_test(test_float_motion_rejects_2x2);
    mu_run_test(test_float_motion_accepts_3x3);
    mu_run_test(test_float_motion_accepts_576x324);
    return NULL;
}
