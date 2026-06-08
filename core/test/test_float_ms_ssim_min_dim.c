/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Netflix#1414 / ADR-0153 — `float_ms_ssim` init must reject input
 *  resolutions below 176x176 cleanly with -EINVAL. The 5-level 11-tap
 *  MS-SSIM pyramid walks off the kernel footprint at scale 4 for any
 *  w < 176 or h < 176, which previously produced a mid-run "scale
 *  below 1x1!" print + a confusing cascading error. The fix is a
 *  resolution check in init() that refuses small inputs up front with
 *  a helpful message.
 */

#include <stdlib.h>

#include "test.h"

#include "feature/feature_extractor.h"

static char *test_float_ms_ssim_is_registered(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("float_ms_ssim");
    mu_assert("float_ms_ssim extractor missing", fex != NULL);
    mu_assert("float_ms_ssim.init must be set", fex->init != NULL);
    mu_assert("float_ms_ssim.close must be set", fex->close != NULL);
    return NULL;
}

/* Helper: call init with the given dimensions and return the result,
 * cleanly freeing the priv buffer on the failure path. The priv buffer
 * is owned by libvmaf core in production; tests have to free it
 * explicitly because there's no core path tearing it down here. */
static int invoke_init(VmafFeatureExtractor *fex, unsigned w, unsigned h)
{
    void *priv = calloc(1, fex->priv_size);
    if (!priv)
        return -1;
    fex->priv = priv;
    int rc = fex->init(fex, VMAF_PIX_FMT_YUV420P, 8u, w, h);
    /* close() is safe after either successful init or early-rejected
     * init — the close contract tolerates partial state. */
    (void)fex->close(fex);
    free(priv);
    fex->priv = NULL;
    return rc;
}

static char *test_float_ms_ssim_init_rejects_below_min_dim(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("float_ms_ssim");
    mu_assert("float_ms_ssim extractor missing", fex != NULL);

    /* Below the pyramid floor in both dimensions. */
    mu_assert("init must reject 160x144 (< 176x176)", invoke_init(fex, 160u, 144u) < 0);

    /* Below in width only (QCIF-style 160x180). */
    mu_assert("init must reject 160x200 (w < 176)", invoke_init(fex, 160u, 200u) < 0);

    /* Below in height only. */
    mu_assert("init must reject 200x160 (h < 176)", invoke_init(fex, 200u, 160u) < 0);

    /* Just below the boundary. */
    mu_assert("init must reject 175x176 (w just below)", invoke_init(fex, 175u, 176u) < 0);
    mu_assert("init must reject 176x175 (h just below)", invoke_init(fex, 176u, 175u) < 0);

    return NULL;
}

static char *test_float_ms_ssim_init_accepts_min_dim(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("float_ms_ssim");
    mu_assert("float_ms_ssim extractor missing", fex != NULL);

    /* Exact boundary — allocation must succeed. */
    int rc = invoke_init(fex, 176u, 176u);
    mu_assert("init must accept 176x176 (exact minimum)", rc == 0);

    /* Standard test resolution well above the floor. */
    rc = invoke_init(fex, 576u, 324u);
    mu_assert("init must accept 576x324 (well above minimum)", rc == 0);

    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_float_ms_ssim_is_registered);
    mu_run_test(test_float_ms_ssim_init_rejects_below_min_dim);
    mu_run_test(test_float_ms_ssim_init_accepts_min_dim);
    return NULL;
}
