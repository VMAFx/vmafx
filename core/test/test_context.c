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
#include <stdbool.h>
#include <string.h>

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

/* ADR-1396: `*vmaf` is output-only, as in upstream. Upstream's own tests and
 * CLI declare `VmafContext *vmaf;` without an initialiser, and the ADR-1032
 * guard that read it returned -EINVAL whenever that stack slot was non-zero:
 * upstream's test_context_init_and_close failed 3 of 3 runs against the
 * fork. A handle holding an open context is overwritten, not rejected. */
static char *test_vmaf_init_output_only()
{
    VmafContext *vmaf = NULL;
    char dummy = 0;
    vmaf = (VmafContext *)&dummy;
    VmafConfiguration cfg = {0};

    int err = vmaf_init(&vmaf, cfg);
    mu_assert("vmaf_init must not read the incoming handle", !err);
    mu_assert("vmaf_init must store the new context", vmaf != NULL);

    VmafContext *const first = vmaf;
    err = vmaf_init(&vmaf, cfg);
    const bool distinct = !err && vmaf != NULL && vmaf != first;
    const int close_first = vmaf_close(first);
    const int close_second = err ? 0 : vmaf_close(vmaf);
    mu_assert("a second vmaf_init on an open handle succeeds", !err);
    mu_assert("it stores a second, distinct context", distinct);
    mu_assert("both contexts close", !close_first && !close_second);
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
    mu_run_test(test_vmaf_init_output_only);
    mu_run_test(test_get_backend_cpu_returns_unknown);
    mu_run_test(test_get_backend_null_guard);
    return NULL;
}
