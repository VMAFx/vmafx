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
 * ADR-1223 — the CUDA compute-capability floor predicate.
 *
 * The gencode list emits nothing below `sm_80`, so a sub-Ampere device cannot
 * load any kernel. `vmaf_cuda_state_init()` rejects such a device up front with
 * an explicit message instead of letting `cuModuleLoadData` fail with
 * CUDA_ERROR_NO_BINARY_FOR_GPU (222) inside whichever feature extractor loaded
 * first.
 *
 * The decision itself is a pure function of (major, minor), split out of the
 * driver call so it can be pinned WITHOUT a GPU — which matters because the
 * architectures this test cares most about (Turing and older) are exactly the
 * ones no runner in the fleet has.
 */

#include <stdbool.h>

#include "test.h"

#include "cuda/common.h"

static char *test_supported_floor_is_ampere(void)
{
    /* The floor itself. */
    mu_assert("sm_80 (Ampere, A100) must be supported", vmaf_cuda_arch_supported(8, 0));

    /* Everything the fatbin ships a cubin for. */
    mu_assert("sm_86 (Ampere, RTX 30xx) must be supported", vmaf_cuda_arch_supported(8, 6));
    mu_assert("sm_89 (Ada, RTX 40xx) must be supported", vmaf_cuda_arch_supported(8, 9));
    mu_assert("sm_90 (Hopper) must be supported", vmaf_cuda_arch_supported(9, 0));
    mu_assert("sm_100 (Blackwell) must be supported", vmaf_cuda_arch_supported(10, 0));
    mu_assert("sm_120 (Blackwell) must be supported", vmaf_cuda_arch_supported(12, 0));

    /* Future majors JIT from the compute_120 PTX. */
    mu_assert("a future major must be supported", vmaf_cuda_arch_supported(13, 0));
    return NULL;
}

static char *test_turing_and_older_rejected(void)
{
    /* The generation ADR-1223 drops. */
    mu_assert("sm_75 (Turing, RTX 20xx / GTX 16xx / T4) must be rejected",
              !vmaf_cuda_arch_supported(7, 5));
    mu_assert("sm_70 (Volta) must be rejected", !vmaf_cuda_arch_supported(7, 0));

    /* Generations CUDA 13.x had already dropped. */
    mu_assert("sm_61 (Pascal) must be rejected", !vmaf_cuda_arch_supported(6, 1));
    mu_assert("sm_52 (Maxwell) must be rejected", !vmaf_cuda_arch_supported(5, 2));
    mu_assert("sm_35 (Kepler) must be rejected", !vmaf_cuda_arch_supported(3, 5));
    return NULL;
}

/* The predicate compares (major, minor) lexicographically, not `major * 10 +
 * minor`. A naive `major >= 8` would wrongly admit a hypothetical 8.x below the
 * floor, and a naive `minor >= 0` short-circuit would wrongly admit 7.9. Pin
 * both edges. */
static char *test_boundary_is_lexicographic(void)
{
    mu_assert("7.9 is below 8.0 and must be rejected", !vmaf_cuda_arch_supported(7, 9));
    mu_assert("8.0 is exactly the floor and must be supported", vmaf_cuda_arch_supported(8, 0));
    mu_assert("9.0 is above the floor even with minor 0", vmaf_cuda_arch_supported(9, 0));
    return NULL;
}

/* Defensive: the driver returning nonsense must not be read as "supported". */
static char *test_degenerate_capabilities_rejected(void)
{
    mu_assert("0.0 must be rejected", !vmaf_cuda_arch_supported(0, 0));
    mu_assert("a negative major must be rejected", !vmaf_cuda_arch_supported(-1, 0));
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_supported_floor_is_ampere);
    mu_run_test(test_turing_and_older_rejected);
    mu_run_test(test_boundary_is_lexicographic);
    mu_run_test(test_degenerate_capabilities_rejected);
    return NULL;
}
