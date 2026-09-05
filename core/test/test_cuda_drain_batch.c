/**
 * Copyright 2026 Lusoris
 *
 * Licensed under the BSD+Patent License (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://opensource.org/licenses/BSDplusPatent
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* Owner scoping of the CUDA drain batch (ADR-0242 fence batch).
 *
 * The batch is thread-local, but two VmafContexts can share one OS thread.
 * Before docs/state.md T-UPSTREAM-1305-CUDA-DRAIN-BATCH-THREAD-GLOBAL-2026-09-03
 * nothing keyed it to its owner, so context B's flush waited on context A's
 * CUevents and wrote through A's `bool *` flags after A had been closed.
 *
 * These cases exercise only the bookkeeping paths: no CUDA call is reached
 * because a foreign owner short-circuits the flush and the destroy path exits
 * as soon as it sees a NULL drain stream. The test therefore runs on any host,
 * with or without a GPU. */

#include <stdbool.h>
#include <stddef.h>

#include "cuda/drain_batch.h"
#include "libvmaf/libvmaf.h"
#include "test.h"

static char *test_open_claims_the_batch_for_its_owner(void)
{
    VmafCudaState engine_a = {0};
    bool drained_a = false;

    vmaf_cuda_drain_batch_open(&engine_a);
    mu_assert("a fresh batch holds no entries", vmaf_cuda_drain_batch_pending() == 0);
    mu_assert("registering into an open batch succeeds",
              vmaf_cuda_drain_batch_register_event(NULL, &drained_a) == 0);
    mu_assert("the entry is pending", vmaf_cuda_drain_batch_pending() == 1);

    vmaf_cuda_drain_batch_thread_destroy(&engine_a);
    // NOLINTNEXTLINE(modernize-use-nullptr): C TU keeps NULL per ADR-1138 (MSVC /std:clatest has no C nullptr).
    return NULL;
}

static char *test_a_second_engine_never_inherits_the_entries(void)
{
    VmafCudaState engine_a = {0};
    VmafCudaState engine_b = {0};
    bool drained_a = false;

    vmaf_cuda_drain_batch_open(&engine_a);
    (void)vmaf_cuda_drain_batch_register_event(NULL, &drained_a);
    mu_assert("engine A has one entry", vmaf_cuda_drain_batch_pending() == 1);

    /* Engine A is closed and its state (and therefore `drained_a`) goes away. */
    vmaf_cuda_drain_batch_thread_destroy(&engine_a);
    mu_assert("destroy leaves no entry behind", vmaf_cuda_drain_batch_pending() == 0);

    /* Engine B takes the thread over and must start from an empty batch. */
    vmaf_cuda_drain_batch_open(&engine_b);
    mu_assert("engine B starts empty", vmaf_cuda_drain_batch_pending() == 0);

    vmaf_cuda_drain_batch_thread_destroy(&engine_b);
    // NOLINTNEXTLINE(modernize-use-nullptr): C TU keeps NULL per ADR-1138 (MSVC /std:clatest has no C nullptr).
    return NULL;
}

static char *test_flush_refuses_a_foreign_owner(void)
{
    VmafCudaState engine_a = {0};
    VmafCudaState engine_b = {0};
    bool drained_a = false;

    vmaf_cuda_drain_batch_open(&engine_a);
    (void)vmaf_cuda_drain_batch_register_event(NULL, &drained_a);

    /* Engine B flushing must be a no-op: A's events are none of its business,
     * and waiting on them would touch objects B does not own. No CUDA call is
     * made, so a NULL function table in `engine_b` is never dereferenced. */
    mu_assert("a foreign flush is a no-op", vmaf_cuda_drain_batch_flush(&engine_b) == 0);
    mu_assert("the foreign flush did not consume A's entry", vmaf_cuda_drain_batch_pending() == 1);
    mu_assert("the foreign flush did not mark A's flag", drained_a == false);

    vmaf_cuda_drain_batch_thread_destroy(&engine_a);
    // NOLINTNEXTLINE(modernize-use-nullptr): C TU keeps NULL per ADR-1138 (MSVC /std:clatest has no C nullptr).
    return NULL;
}

static char *test_open_after_a_foreign_owner_drops_stale_entries(void)
{
    VmafCudaState engine_a = {0};
    VmafCudaState engine_b = {0};
    bool drained_a = false;

    vmaf_cuda_drain_batch_open(&engine_a);
    (void)vmaf_cuda_drain_batch_register_event(NULL, &drained_a);
    mu_assert("engine A has one entry", vmaf_cuda_drain_batch_pending() == 1);

    /* Engine A did not get to close (a crash, or a caller that never calls
     * vmaf_close). Engine B opening the batch must still start clean. */
    vmaf_cuda_drain_batch_open(&engine_b);
    mu_assert("engine B dropped the stale entry", vmaf_cuda_drain_batch_pending() == 0);

    vmaf_cuda_drain_batch_thread_destroy(&engine_b);
    // NOLINTNEXTLINE(modernize-use-nullptr): C TU keeps NULL per ADR-1138 (MSVC /std:clatest has no C nullptr).
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_open_claims_the_batch_for_its_owner);
    mu_run_test(test_a_second_engine_never_inherits_the_entries);
    mu_run_test(test_flush_refuses_a_foreign_owner);
    mu_run_test(test_open_after_a_foreign_owner_drops_stale_entries);
    // NOLINTNEXTLINE(modernize-use-nullptr): C TU keeps NULL per ADR-1138 (MSVC /std:clatest has no C nullptr).
    return NULL;
}

int tests_run = 0;
