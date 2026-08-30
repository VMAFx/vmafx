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
 * PR #850 regression — libvmaf.c CUDA-only NULL deref guard.
 *
 * Before PR #850, in CUDA-only mode (HW_FLAG_HOST not set), the
 * unconditional `ref = &ref_host` / `dist = &dist_host` reassignment in
 * vmaf_read_pictures passed zero-initialised picture structs to the
 * thread pool → NULL pointer dereference / SIGSEGV.
 *
 * The fix gates those two lines behind `if (hw_flags & HW_FLAG_HOST)`.
 *
 * This test registers a CUDA-only feature extractor (adm_cuda) and calls
 * vmaf_read_pictures, asserting no crash.  GPU-gated: the test skips
 * cleanly when no CUDA device is visible so CI on non-GPU hosts stays
 * green.
 *
 * Regression criterion: "the process did not abort" — any non-crash
 * return (0, -ENOSYS, or -EINVAL) is accepted.  What we are verifying is
 * that the NULL deref is gone; we are not asserting a score.
 */

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test.h"

#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_cuda.h"
#include "libvmaf/picture.h"

#define FIX_W 64u
#define FIX_H 64u
#define FIX_BPC 8u

static int alloc_picture(VmafPicture *pic, uint8_t fill_y)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIX_BPC, FIX_W, FIX_H);
    if (err)
        return err;
    for (unsigned row = 0; row < pic->h[0]; row++)
        memset((uint8_t *)pic->data[0] + row * pic->stride[0], fill_y, pic->w[0]);
    for (unsigned p = 1; p < 3; p++)
        for (unsigned row = 0; row < pic->h[p]; row++)
            memset((uint8_t *)pic->data[p] + row * pic->stride[p], 128, pic->w[p]);
    return 0;
}

static char *test_cuda_only_no_host_no_crash(void)
{
    VmafCudaState *cu_state = NULL;
    VmafCudaConfiguration cuda_cfg = {0};
    int err = vmaf_cuda_state_init(&cu_state, cuda_cfg);
    if (err != 0 || cu_state == NULL) {
        (void)fprintf(stderr, "[skip: no CUDA device] ");
        return NULL;
    }

    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    err = vmaf_init(&vmaf, cfg);
    mu_assert("vmaf_init failed", !err);

    err = vmaf_cuda_import_state(vmaf, cu_state);
    mu_assert("vmaf_cuda_import_state failed", !err);

    /* Register ONLY the CUDA extractor (no CPU host extractor).
     * This sets hw_flags to HW_FLAG_DEVICE only, triggering the pre-fix
     * NULL deref path in vmaf_read_pictures. */
    err = vmaf_use_feature(vmaf, "adm_cuda", NULL);
    mu_assert("vmaf_use_feature(adm_cuda) failed", !err);

    VmafPicture ref, dis;
    err = alloc_picture(&ref, 100);
    mu_assert("alloc ref failed", !err);
    err = alloc_picture(&dis, 90);
    mu_assert("alloc dis failed", !err);

    /* Before the fix this dereferences ref_host.data[0] == NULL → SIGSEGV.
     * After the fix it routes through the device path without touching
     * ref_host/dist_host.  We accept any non-crash return code. */
    err = vmaf_read_pictures(vmaf, &ref, &dis, 0);
    /* -ENOSYS means CUDA kernels not compiled (enable_hipcc=false analog).
     * 0 means success.  Either way: no crash. */
    mu_assert("vmaf_read_pictures must not return a positive code", err <= 0);

    /* Flush */
    (void)vmaf_read_pictures(vmaf, NULL, NULL, 0);
    (void)vmaf_close(vmaf);
    (void)vmaf_cuda_state_free(cu_state);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_cuda_only_no_host_no_crash);
    return NULL;
}
