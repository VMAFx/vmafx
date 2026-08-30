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
 * Regression test for Fable finding libvmaf-bpc-validation-and-or.
 *
 * validate_pic_params() used && instead of || for the bpc check:
 *
 *   WRONG:  if ((ref->bpc != dist->bpc) && (ref->bpc != vmaf->pic_params.bpc))
 *   FIXED:  if ((ref->bpc != dist->bpc) || (ref->bpc != vmaf->pic_params.bpc))
 *
 * With the && form, on frame 0 pic_params.bpc is set from ref->bpc immediately
 * before the check, so the second term (ref->bpc != vmaf->pic_params.bpc) is
 * always false — the whole && is false — and a ref=8bpc/dist=10bpc pair is
 * silently accepted.  The fix changes && to || (matching the sibling w, h, and
 * pix_fmt checks) so any bpc mismatch is rejected with -EINVAL.
 *
 * This test pins that contract: submitting mismatched-bpc pictures via
 * vmaf_read_pictures must return -EINVAL on the very first frame.
 */

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

/*
 * Submit a pair of pictures with the given bpc values.
 * On API rejection the caller is responsible for unref-ing both pictures;
 * on success the API consumes them.
 */
static int submit_frame_bpc(VmafContext *vmaf, unsigned ref_bpc, unsigned dist_bpc, unsigned index)
{
    VmafPicture ref;
    VmafPicture dist;
    int err = vmaf_picture_alloc(&ref, VMAF_PIX_FMT_YUV420P, ref_bpc, 64, 64);
    if (err)
        return err;
    err = vmaf_picture_alloc(&dist, VMAF_PIX_FMT_YUV420P, dist_bpc, 64, 64);
    if (err) {
        (void)vmaf_picture_unref(&ref);
        return err;
    }
    err = vmaf_read_pictures(vmaf, &ref, &dist, index);
    if (err) {
        /* API rejected — caller must unref. */
        (void)vmaf_picture_unref(&ref);
        (void)vmaf_picture_unref(&dist);
    }
    return err;
}

/*
 * Matched bpc (8bpc ref + 8bpc dist) must be accepted on frame 0.
 * Sanity-check that the fix did not break the happy path.
 */
static char *test_matched_bpc_accepted(void)
{
    VmafContext *vmaf = init_context();
    mu_assert("init failed", vmaf != NULL);

    int err = submit_frame_bpc(vmaf, 8, 8, 0);
    mu_assert("matched 8bpc ref+dist must be accepted", err == 0);

    (void)vmaf_close(vmaf);
    return NULL;
}

/*
 * Mismatched bpc (8bpc ref, 10bpc dist) must be rejected on frame 0.
 *
 * Before the fix, validate_pic_params() used &&:
 *   if ((ref->bpc != dist->bpc) && (ref->bpc != vmaf->pic_params.bpc))
 * On frame 0, pic_params.bpc is assigned from ref->bpc immediately before this
 * check, so the second term is always false and the whole && is false — the
 * mismatch was silently accepted.  The || fix catches it.
 */
static char *test_mismatched_bpc_rejected_on_frame_0(void)
{
    VmafContext *vmaf = init_context();
    mu_assert("init failed", vmaf != NULL);

    int err = submit_frame_bpc(vmaf, 8, 10, 0);
    mu_assert("8bpc ref + 10bpc dist must be rejected with -EINVAL on frame 0", err == -EINVAL);

    (void)vmaf_close(vmaf);
    return NULL;
}

/*
 * Mismatched bpc (10bpc ref, 8bpc dist) must also be rejected.
 * Exercises the ref->bpc != dist->bpc arm of the ||.
 */
static char *test_mismatched_bpc_reversed_rejected(void)
{
    VmafContext *vmaf = init_context();
    mu_assert("init failed", vmaf != NULL);

    int err = submit_frame_bpc(vmaf, 10, 8, 0);
    mu_assert("10bpc ref + 8bpc dist must be rejected with -EINVAL", err == -EINVAL);

    (void)vmaf_close(vmaf);
    return NULL;
}

/*
 * Consistent bpc across frames must be accepted.
 * 10bpc frame 0 followed by 10bpc frame 1 — both match each other and the
 * pic_params recorded on frame 0.
 */
static char *test_consistent_10bpc_across_frames(void)
{
    VmafContext *vmaf = init_context();
    mu_assert("init failed", vmaf != NULL);

    int err = submit_frame_bpc(vmaf, 10, 10, 0);
    mu_assert("10bpc matched pair on frame 0 must be accepted", err == 0);
    err = submit_frame_bpc(vmaf, 10, 10, 1);
    mu_assert("10bpc matched pair on frame 1 must be accepted", err == 0);

    (void)vmaf_close(vmaf);
    return NULL;
}

/*
 * Bpc change mid-stream (frame 0 = 8bpc, frame 1 = 10bpc) must be rejected.
 * Exercises the ref->bpc != vmaf->pic_params.bpc arm of the || on frame 1.
 */
static char *test_bpc_change_mid_stream_rejected(void)
{
    VmafContext *vmaf = init_context();
    mu_assert("init failed", vmaf != NULL);

    int err = submit_frame_bpc(vmaf, 8, 8, 0);
    mu_assert("8bpc matched pair on frame 0 must be accepted", err == 0);
    err = submit_frame_bpc(vmaf, 10, 10, 1);
    mu_assert("bpc change from 8 to 10 on frame 1 must be rejected with -EINVAL", err == -EINVAL);

    (void)vmaf_close(vmaf);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_matched_bpc_accepted);
    mu_run_test(test_mismatched_bpc_rejected_on_frame_0);
    mu_run_test(test_mismatched_bpc_reversed_rejected);
    mu_run_test(test_consistent_10bpc_across_frames);
    mu_run_test(test_bpc_change_mid_stream_rejected);
    return NULL;
}
