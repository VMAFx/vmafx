/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 *
 *  integer_adm init() must reject inputs where min(w,h) <= 16 with -EINVAL
 *  instead of SIGSEGV-ing mid-run. The 4-level DWT2 pyramid requires at
 *  least 17 pixels in each dimension so that the coarsest level still has
 *  a non-empty band; frames smaller than 17x17 walk off the end of the
 *  allocated scratch buffers on the first decomposition step.
 */

#include <stdlib.h>

#include "test.h"

#include "feature/feature_extractor.h"

static char *test_integer_adm_is_registered(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("adm");
    mu_assert("integer_adm extractor missing", fex != NULL);
    mu_assert("integer_adm.init must be set", fex->init != NULL);
    mu_assert("integer_adm.close must be set", fex->close != NULL);
    return NULL;
}

/* Helper: call init with the given dimensions and return the result,
 * cleanly freeing the priv buffer on the failure path. */
static int invoke_init(VmafFeatureExtractor *fex, unsigned w, unsigned h)
{
    void *priv = calloc(1, fex->priv_size);
    if (!priv)
        return -1;
    fex->priv = priv;
    int rc = fex->init(fex, VMAF_PIX_FMT_YUV420P, 8u, w, h);
    /* close() tolerates partial state (init may have returned early). */
    (void)fex->close(fex);
    free(priv);
    fex->priv = NULL;
    return rc;
}

static char *test_integer_adm_init_rejects_below_min_dim(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("adm");
    mu_assert("integer_adm extractor missing", fex != NULL);

    /* Both dimensions below the floor. */
    mu_assert("init must reject 8x8 (< 17x17)", invoke_init(fex, 8u, 8u) < 0);

    /* Width at floor, height below. */
    mu_assert("init must reject 17x16 (h just below)", invoke_init(fex, 17u, 16u) < 0);

    /* Height at floor, width below. */
    mu_assert("init must reject 16x17 (w just below)", invoke_init(fex, 16u, 17u) < 0);

    /* Exactly at the excluded boundary (16 is the last rejected value). */
    mu_assert("init must reject 16x16", invoke_init(fex, 16u, 16u) < 0);

    return NULL;
}

static char *test_integer_adm_init_accepts_min_dim(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("adm");
    mu_assert("integer_adm extractor missing", fex != NULL);

    /* Exact boundary — must succeed. */
    int rc = invoke_init(fex, 17u, 17u);
    mu_assert("init must accept 17x17 (exact minimum)", rc == 0);

    /* Standard Netflix test resolution well above the floor. */
    rc = invoke_init(fex, 576u, 324u);
    mu_assert("init must accept 576x324 (well above minimum)", rc == 0);

    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_integer_adm_is_registered);
    mu_run_test(test_integer_adm_init_rejects_below_min_dim);
    mu_run_test(test_integer_adm_init_accepts_min_dim);
    return NULL;
}
