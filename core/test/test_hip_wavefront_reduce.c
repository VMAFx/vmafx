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
 * PR #850 regression — HIP wavefront warpSize fix.
 *
 * Before PR #850 the five HIP kernel files hard-coded WARP_SIZE=64.
 * On RDNA2+ (wave32) devices only 32 of the 64 required XOR-shuffle
 * steps executed, halving VIF accumulators and producing ~25-pt VMAF
 * score error.  The fix replaces the compile-time constant with the
 * HIP device variable `warpSize`.
 *
 * This file tests the *host-side reduction logic* that mirrors what
 * the kernel does: given N values in shared memory, the tree-reduce
 * must produce the correct sum for both wave32 (warp_size=32) and
 * wave64 (warp_size=64) scenarios.  No AMD GPU is required.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include "test.h"

/* Mirrors the host-side warp-partial accumulation pattern used in the
 * HIP kernels after the PR #850 fix.  Each "warp" contributes one
 * partial sum into smem[0..warps_per_block).  The final thread then
 * sums those partials.  warp_size can be 32 or 64. */
static float host_warp_reduce_sum(const float *values, int n, int warp_size)
{
    /* Maximum supported slots: block_size / min_warp_size = 256/32 = 8 */
    float smem[8] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    int warps = (n + warp_size - 1) / warp_size;

    for (int w = 0; w < warps; w++) {
        float warp_sum = 0.0f;
        int lo = w * warp_size;
        int hi = lo + warp_size;
        if (hi > n)
            hi = n;
        for (int i = lo; i < hi; i++)
            warp_sum += values[i];
        smem[w] = warp_sum;
    }

    float total = 0.0f;
    for (int w = 0; w < warps; w++)
        total += smem[w];
    return total;
}

static char *test_wave32_reduce_produces_correct_sum(void)
{
    /* 256 values == one block on wave32 (8 warps of 32). */
    float vals[256];
    for (int i = 0; i < 256; i++)
        vals[i] = 1.0f;

    float got = host_warp_reduce_sum(vals, 256, 32);
    mu_assert("wave32 reduce of 256×1.0 must equal 256.0", fabsf(got - 256.0f) < 1e-4f);
    return NULL;
}

static char *test_wave64_reduce_produces_correct_sum(void)
{
    /* 256 values == one block on wave64 (4 warps of 64). */
    float vals[256];
    for (int i = 0; i < 256; i++)
        vals[i] = 1.0f;

    float got = host_warp_reduce_sum(vals, 256, 64);
    mu_assert("wave64 reduce of 256×1.0 must equal 256.0", fabsf(got - 256.0f) < 1e-4f);
    return NULL;
}

static char *test_wave32_and_wave64_agree_on_identical_input(void)
{
    float vals[256];
    for (int i = 0; i < 256; i++)
        vals[i] = (float)(i + 1) * 0.5f;

    float r32 = host_warp_reduce_sum(vals, 256, 32);
    float r64 = host_warp_reduce_sum(vals, 256, 64);
    mu_assert("wave32 and wave64 reductions of same input must agree to 1e-3",
              fabsf(r32 - r64) < 1e-3f);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_wave32_reduce_produces_correct_sum);
    mu_run_test(test_wave64_reduce_produces_correct_sum);
    mu_run_test(test_wave32_and_wave64_agree_on_identical_input);
    return NULL;
}
