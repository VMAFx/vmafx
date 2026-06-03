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

/* Netflix#910 / ADR-0152 — vmaf_read_pictures must reject non-monotonic
 * indices. Motion / motion2 / motion3 extractors keep sliding-window
 * state keyed by `index % N`; submitting frames out of order (or with
 * duplicate indices) silently corrupts that state. The fork enforces a
 * monotonically-increasing index at the API boundary; this test pins
 * the contract. */

#include "test.h"

#include "libvmaf/libvmaf.h"
#include "libvmaf/picture.h"

#include <errno.h>
#include <stdlib.h>

static VmafContext *init_context(void)
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    int err = vmaf_init(&vmaf, cfg);
    return err == 0 ? vmaf : NULL;
}

static int submit_frame(VmafContext *vmaf, unsigned index)
{
    VmafPicture ref;
    VmafPicture dist;
    int err = vmaf_picture_alloc(&ref, VMAF_PIX_FMT_YUV420P, 8, 64, 64);
    if (err)
        return err;
    err = vmaf_picture_alloc(&dist, VMAF_PIX_FMT_YUV420P, 8, 64, 64);
    if (err) {
        vmaf_picture_unref(&ref);
        return err;
    }
    err = vmaf_read_pictures(vmaf, &ref, &dist, index);
    /* On rejection vmaf_read_pictures does NOT consume the pictures —
     * unref them here to avoid leaking. On success the API owns them. */
    if (err) {
        vmaf_picture_unref(&ref);
        vmaf_picture_unref(&dist);
    }
    return err;
}

static char *test_read_pictures_monotonic_accepts_increasing(void)
{
    VmafContext *vmaf = init_context();
    mu_assert("init failed", vmaf != NULL);

    mu_assert("frame 0 rejected", submit_frame(vmaf, 0) == 0);
    mu_assert("frame 1 rejected", submit_frame(vmaf, 1) == 0);
    mu_assert("frame 5 rejected", submit_frame(vmaf, 5) == 0); /* gaps OK */
    mu_assert("frame 10 rejected", submit_frame(vmaf, 10) == 0);

    vmaf_close(vmaf);
    return NULL;
}

static char *test_read_pictures_monotonic_rejects_duplicate(void)
{
    VmafContext *vmaf = init_context();
    mu_assert("init failed", vmaf != NULL);

    mu_assert("frame 0 rejected", submit_frame(vmaf, 0) == 0);
    mu_assert("frame 1 rejected", submit_frame(vmaf, 1) == 0);
    int err = submit_frame(vmaf, 1); /* duplicate */
    mu_assert("duplicate index accepted (expected -EINVAL)", err == -EINVAL);

    vmaf_close(vmaf);
    return NULL;
}

static char *test_read_pictures_monotonic_rejects_out_of_order(void)
{
    VmafContext *vmaf = init_context();
    mu_assert("init failed", vmaf != NULL);

    /* Netflix#910 reproducer sequence: out-of-order submission
     * corrupts integer_motion's 3-frame blur ring. The API must
     * reject the out-of-order frame before extractor state is
     * touched. */
    mu_assert("frame 3970 rejected", submit_frame(vmaf, 3970) == 0);
    mu_assert("frame 3974 rejected", submit_frame(vmaf, 3974) == 0);
    int err_3972 = submit_frame(vmaf, 3972);
    mu_assert("out-of-order frame 3972 accepted (expected -EINVAL)", err_3972 == -EINVAL);
    int err_3973 = submit_frame(vmaf, 3973);
    mu_assert("out-of-order frame 3973 accepted (expected -EINVAL)", err_3973 == -EINVAL);

    /* Continuing with a higher-than-last-accepted index still works. */
    mu_assert("frame 3975 rejected after recovery", submit_frame(vmaf, 3975) == 0);

    vmaf_close(vmaf);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_read_pictures_monotonic_accepts_increasing);
    mu_run_test(test_read_pictures_monotonic_rejects_duplicate);
    mu_run_test(test_read_pictures_monotonic_rejects_out_of_order);
    return NULL;
}
