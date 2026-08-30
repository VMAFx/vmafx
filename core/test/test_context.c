/**
 *
 *  Copyright 2016-2026 Netflix, Inc.
 *
 *     Licensed under the BSD+Patent License (the "License");
 *     you may not use this file except in compliance with the License.
 *     You may obtain a copy of the License at
 *
 *         https://opensource.org/licenses/BSDplusPatent
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 *
 */

#include <errno.h>

#include "test.h"
#include "libvmaf/libvmaf.h"

static char *test_context_init_and_close()
{
    int err = 0;
    VmafContext *vmaf = NULL;
    VmafConfiguration cfg = {0};

    err = vmaf_init(&vmaf, cfg);
    mu_assert("problem during vmaf_init", !err);
    err = vmaf_close(vmaf);
    mu_assert("problem during vmaf_close", !err);

    return NULL;
}

static char *test_get_feature_score()
{
    int err = 0;
    VmafContext *vmaf = NULL;
    VmafConfiguration cfg = {0};

    err = vmaf_init(&vmaf, cfg);
    mu_assert("problem during vmaf_init", !err);

    err = vmaf_import_feature_score(vmaf, "feature_a", 100., 0);
    err |= vmaf_import_feature_score(vmaf, "feature_a", 200., 1);
    err |= vmaf_import_feature_score(vmaf, "feature_a", 300., 2);
    mu_assert("problem during vmaf_import_feature_score", !err);

    double score;
    err = vmaf_feature_score_at_index(vmaf, "feature_a", &score, 0);
    mu_assert("problem during vmaf_feature_score_at_index", !err);
    mu_assert("retrieved feature score does not match", score == 100.);
    err = vmaf_feature_score_at_index(vmaf, "feature_a", &score, 1);
    mu_assert("problem during vmaf_feature_score_at_index", !err);
    mu_assert("retrieved feature score does not match", score == 200.);
    err = vmaf_feature_score_at_index(vmaf, "feature_a", &score, 2);
    mu_assert("problem during vmaf_feature_score_at_index", !err);
    mu_assert("retrieved feature score does not match", score == 300.);

    err = vmaf_feature_score_pooled(vmaf, "feature_a", VMAF_POOL_METHOD_MEAN, &score, 0, 2);
    mu_assert("problem during vmaf_feature_score_pooled", !err);
    mu_assert("pooled feature score does not match expected value", score == 200.);

    err = vmaf_close(vmaf);
    mu_assert("problem during vmaf_close", !err);

    return NULL;
}

static char *test_vmaf_init_double_init_guard()
{
    /* vmaf_init on an already-initialised (non-NULL) pointer must return
     * -EINVAL without leaking the existing context.  This exercises the
     * double-init guard added in ADR-1032. */
    VmafContext *vmaf = NULL;
    VmafConfiguration cfg = {0};

    int err = vmaf_init(&vmaf, cfg);
    mu_assert("first vmaf_init failed unexpectedly", !err);
    mu_assert("vmaf pointer still NULL after successful init", vmaf != NULL);

    /* Second call on the same (non-NULL) pointer must fail. */
    int err2 = vmaf_init(&vmaf, cfg);
    mu_assert("vmaf_init on non-NULL pointer must return -EINVAL", err2 == -EINVAL);

    /* The original context must still be valid and closeable. */
    err = vmaf_close(vmaf);
    mu_assert("vmaf_close after rejected double-init failed", !err);

    return NULL;
}

/* vmaf_context_get_backend — CPU-only path: freshly init'd context returns
 * VMAF_BACKEND_UNKNOWN because no GPU import_state was called. */
static char *test_get_backend_cpu_returns_unknown()
{
    VmafContext *vmaf = NULL;
    VmafConfiguration cfg = {0};

    int err = vmaf_init(&vmaf, cfg);
    mu_assert("vmaf_init failed in test_get_backend_cpu_returns_unknown", !err);

    enum VmafBackend backend = VMAF_BACKEND_CUDA; /* intentionally non-zero */
    err = vmaf_context_get_backend(vmaf, &backend);
    mu_assert("vmaf_context_get_backend failed on CPU context", !err);
    mu_assert("CPU-only context must report VMAF_BACKEND_UNKNOWN", backend == VMAF_BACKEND_UNKNOWN);

    err = vmaf_close(vmaf);
    mu_assert("vmaf_close failed in test_get_backend_cpu_returns_unknown", !err);

    return NULL;
}

/* vmaf_context_get_backend — null-pointer guard: both vmaf=NULL and out=NULL
 * must return -EINVAL without crashing. */
static char *test_get_backend_null_guard()
{
    enum VmafBackend backend = VMAF_BACKEND_UNKNOWN;
    int err = vmaf_context_get_backend(NULL, &backend);
    mu_assert("vmaf_context_get_backend(NULL, out) must return -EINVAL", err == -EINVAL);

    VmafContext *vmaf = NULL;
    VmafConfiguration cfg = {0};
    err = vmaf_init(&vmaf, cfg);
    mu_assert("vmaf_init failed in test_get_backend_null_guard", !err);

    err = vmaf_context_get_backend(vmaf, NULL);
    mu_assert("vmaf_context_get_backend(vmaf, NULL) must return -EINVAL", err == -EINVAL);

    err = vmaf_close(vmaf);
    mu_assert("vmaf_close failed in test_get_backend_null_guard", !err);

    return NULL;
}

char *run_tests()
{
    mu_run_test(test_context_init_and_close);
    mu_run_test(test_get_feature_score);
    mu_run_test(test_vmaf_init_double_init_guard);
    mu_run_test(test_get_backend_cpu_returns_unknown);
    mu_run_test(test_get_backend_null_guard);
    return NULL;
}
