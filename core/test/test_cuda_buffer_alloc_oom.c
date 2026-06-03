/**
 *
 *  Copyright 2026 Lusoris
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

/*
 * Netflix#1420 reducer — attempt an oversized cuMemAlloc and assert the
 * fork's CHECK_CUDA wholesale rewrite returns -ENOMEM to the caller
 * instead of firing assert(0) and aborting the process.
 *
 * GPU-gated: if vmaf_cuda_state_init fails (no driver / no device) the
 * test skips with a pass so hosts without CUDA hardware still run the
 * suite green. The success criterion is "the process didn't abort and
 * the allocator returned -ENOMEM for an impossible size," not a
 * particular frame-level score.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#include "test.h"

#include "libvmaf/libvmaf_cuda.h"

#include "cuda/common.h"

static char *test_cuda_buffer_alloc_oom_returns_enomem(void)
{
    VmafCudaState *cu_state = NULL;
    VmafCudaConfiguration cfg = {0};

    int err = vmaf_cuda_state_init(&cu_state, cfg);
    if (err != 0 || cu_state == NULL) {
        /* No CUDA runtime / no device — skip. The wholesale rewrite is
         * still exercised on a host where the driver is present. */
        fprintf(stderr, "[skip: no CUDA runtime] ");
        return NULL;
    }

    /* Request 1 TiB from the device allocator. On every GPU the fork
     * actually runs on this has to fail with CUDA_ERROR_OUT_OF_MEMORY,
     * which the mapping table converts to -ENOMEM. Before the rewrite
     * this would assert(0). */
    const size_t huge = (size_t)1 << 40;
    VmafCudaBuffer *buf = NULL;
    int alloc_err = vmaf_cuda_buffer_alloc(cu_state, &buf, huge);

    mu_assert("vmaf_cuda_buffer_alloc must fail for 1 TiB request", alloc_err != 0);
    mu_assert("vmaf_cuda_buffer_alloc must return -ENOMEM on OOM", alloc_err == -ENOMEM);
    mu_assert("vmaf_cuda_buffer_alloc must NULL out the output buffer on failure", buf == NULL);

    (void)vmaf_cuda_release(cu_state);
    (void)vmaf_cuda_state_free(cu_state);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_cuda_buffer_alloc_oom_returns_enomem);
    return NULL;
}
