/**
 *
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
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
 * Regression test for finding R2-2: the CUDA HOST_PINNED picture allocator
 * (vmaf_cuda_picture_alloc_pinned) lacked the per-side dimension upper-bound
 * guard its host twin vmaf_picture_alloc enforces.  The stride/pic_size
 * compute runs in 32-bit unsigned arithmetic:
 *
 *     aligned_y = (w + DATA_ALIGN_PINNED - 1u) & ~(DATA_ALIGN_PINNED - 1u);
 *     stride[0] = aligned_y << hbd;
 *     y_sz      = stride[0] * h;
 *
 * For w near UINT32_MAX the (w + 31u) addition wraps, aligned_y -> 0,
 * stride -> 0, pic_size -> ~0, and cuMemHostAlloc under-allocates, so the
 * first frame copy writes out of bounds.  CERT INT30-C.
 *
 * The fix adds, at the very top of vmaf_cuda_picture_alloc_pinned (before any
 * CUDA-state dereference):
 *
 *     if (w == 0 || w > VMAF_CUDA_PIC_DIM_MAX ||
 *         h == 0 || h > VMAF_CUDA_PIC_DIM_MAX) return -EINVAL;
 *
 * Because the guard returns before `cuda_state->f` is touched, the rejection
 * paths are exercised with cuda_state == NULL — no live CUDA device is
 * required.  Pre-fix, the oversized-dimension call would fall through the
 * guard and dereference the NULL cuda_state (or under-allocate on a real
 * device); post-fix it returns -EINVAL cleanly.  This makes the test
 * load-bearing: it FAILS (NULL deref / wrong rc) without the fix and PASSES
 * with it, while remaining runnable on hosts with nvcc but no GPU.
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "test.h"

#include "libvmaf/picture.h"
#include "cuda/picture_cuda.h"

static char *test_pinned_alloc_rejects_overflow_dimensions()
{
    int err;
    VmafPicture pic;

    /* w == 0 must be rejected before any CUDA-state access. */
    err = vmaf_cuda_picture_alloc_pinned(&pic, VMAF_PIX_FMT_YUV420P, 8, 0, 1080, NULL);
    mu_assert("pinned alloc must reject w=0 with -EINVAL", err == -EINVAL);

    /* h == 0 must be rejected. */
    err = vmaf_cuda_picture_alloc_pinned(&pic, VMAF_PIX_FMT_YUV420P, 8, 1920, 0, NULL);
    mu_assert("pinned alloc must reject h=0 with -EINVAL", err == -EINVAL);

    /* w just past the cap must be rejected. */
    err = vmaf_cuda_picture_alloc_pinned(&pic, VMAF_PIX_FMT_YUV420P, 8, 32769, 1080, NULL);
    mu_assert("pinned alloc must reject w=32769 with -EINVAL", err == -EINVAL);

    /* h just past the cap must be rejected. */
    err = vmaf_cuda_picture_alloc_pinned(&pic, VMAF_PIX_FMT_YUV420P, 8, 1920, 32769, NULL);
    mu_assert("pinned alloc must reject h=32769 with -EINVAL", err == -EINVAL);

    /* Near-UINT32_MAX width — the exact 32-bit wrap that under-allocated. */
    err = vmaf_cuda_picture_alloc_pinned(&pic, VMAF_PIX_FMT_YUV420P, 8, 0xFFFFFFE1u, 1080, NULL);
    mu_assert("pinned alloc must reject near-UINT32_MAX width with -EINVAL", err == -EINVAL);

    return NULL;
}

char *run_tests()
{
    mu_run_test(test_pinned_alloc_rejects_overflow_dimensions);
    return NULL;
}
