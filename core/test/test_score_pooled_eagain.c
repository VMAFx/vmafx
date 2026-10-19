/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  Netflix#755 / ADR-0154 — vmaf_score_pooled must distinguish
 *  "feature not yet written" (transient, -EAGAIN) from "programmer
 *  error" (fatal, -EINVAL). Several extractors (integer_motion's
 *  motion2/motion3, the five-frame-window variant) write frame N's
 *  score retroactively when frame N+1 or N+2 is extracted, then the
 *  tail on flush. Pre-fix, vmaf_score_pooled returned -EINVAL for
 *  the transient case — indistinguishable from genuine misuse.
 *
 *  This test pins:
 *    (1) transient case returns -EAGAIN (not -EINVAL);
 *    (2) the streaming pattern `score_pooled(i-1, i-1)` after
 *        `read_pictures(i)` returns 0 with a valid score (ADR-2090:
 *        motion2 / motion3 of frame i - 1 are final once frame i is read);
 *    (3) after flush, every in-range index is poolable with rc=0.
 */

#include <errno.h>

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe, and this file mirrors
 * the C spelling of the surface it exercises. ADR-1138. */
#include <string.h>

#include "test.h"

#include "libvmaf/libvmaf.h"
#include "libvmaf/model.h"
#include "libvmaf/picture.h"

static int submit_frame(VmafContext *vmaf, unsigned i, unsigned w, unsigned h)
{
    VmafPicture ref;
    VmafPicture dist;
    int err = vmaf_picture_alloc(&ref, VMAF_PIX_FMT_YUV420P, 8, w, h);
    if (err)
        return err;
    err = vmaf_picture_alloc(&dist, VMAF_PIX_FMT_YUV420P, 8, w, h);
    if (err) {
        vmaf_picture_unref(&ref);
        return err;
    }
    /* Mild per-frame variation so motion SAD is non-zero. */
    memset(ref.data[0], (int)(128 + i), ref.stride[0] * h);
    memset(dist.data[0], (int)(130 + i), dist.stride[0] * h);
    memset(ref.data[1], 128, ref.stride[1] * (h / 2));
    memset(dist.data[1], 128, dist.stride[1] * (h / 2));
    memset(ref.data[2], 128, ref.stride[2] * (h / 2));
    memset(dist.data[2], 128, dist.stride[2] * (h / 2));
    return vmaf_read_pictures(vmaf, &ref, &dist, i);
}

static char *test_score_pooled_returns_eagain_on_pending(void)
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    mu_assert("vmaf_init failed", vmaf_init(&vmaf, cfg) == 0);

    VmafModelConfig mcfg = {0};
    VmafModel *model = NULL;
    mu_assert("model load failed", vmaf_model_load(&model, &mcfg, "vmaf_v0.6.1") == 0);
    mu_assert("use_features_from_model failed", vmaf_use_features_from_model(vmaf, model) == 0);

    /* Submit frame 1 — motion2 for frame 1 is NOT yet written (requires
     * frame 2 or flush). vmaf_v0.6.1 depends on integer_motion2, so
     * score_pooled(1,1) must return -EAGAIN at this point. */
    mu_assert("read(0) failed", submit_frame(vmaf, 0u, 576, 324) == 0);
    mu_assert("read(1) failed", submit_frame(vmaf, 1u, 576, 324) == 0);

    double score = 0.0;
    int rc = vmaf_score_pooled(vmaf, model, VMAF_POOL_METHOD_MEAN, &score, 1u, 1u);
    mu_assert("score_pooled(1,1) must return -EAGAIN, not -EINVAL", rc == -EAGAIN);

    vmaf_close(vmaf);
    vmaf_model_destroy(model);
    return NULL;
}

/* Pool frames [from, to] of `model`; 0 with a finite score, or the error. */
static int pooled(VmafContext *vmaf, VmafModel *model, unsigned from, unsigned to)
{
    double score = -1.0;
    const int rc = vmaf_score_pooled(vmaf, model, VMAF_POOL_METHOD_MEAN, &score, from, to);
    return (rc == 0 && !(score >= 0.0)) ? -ERANGE : rc;
}

/* Frames 0..3, one by one: after read_pictures(i) frame i - 1 pools and frame
 * i is pending; after the flush every frame pools. NULL or the failure. */
static char *check_streaming(VmafContext *vmaf, VmafModel *model)
{
    for (unsigned i = 0; i < 4; i++) {
        mu_assert("read failed", submit_frame(vmaf, i, 576, 324) == 0);
        mu_assert("frame i - 1 does not pool after read_pictures(i)",
                  i == 0 || pooled(vmaf, model, i - 1u, i - 1u) == 0);
        mu_assert("frame i pools before the frame after it is read (motion2 early)",
                  pooled(vmaf, model, i, i) == -EAGAIN);
    }
    mu_assert("frames 0..2 do not pool before the flush", pooled(vmaf, model, 0u, 2u) == 0);
    mu_assert("flush failed", vmaf_read_pictures(vmaf, NULL, NULL, 0) == 0);
    mu_assert("the last frame does not pool after the flush", pooled(vmaf, model, 3u, 3u) == 0);
    mu_assert("the stream does not pool after the flush", pooled(vmaf, model, 0u, 3u) == 0);
    return NULL;
}

static char *test_score_pooled_streaming_pattern(void)
{
    /* ADR-2090: integer_motion derives motion2 / motion3 of frame i once the
     * SAD of frame i + 1 is in, before the flush (the streaming pattern of
     * Netflix#755). */
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    mu_assert("vmaf_init failed", vmaf_init(&vmaf, cfg) == 0);

    VmafModelConfig mcfg = {0};
    VmafModel *model = NULL;
    char *msg = vmaf_model_load(&model, &mcfg, "vmaf_v0.6.1") == 0 ? NULL : "model load failed";
    if (!msg && vmaf_use_features_from_model(vmaf, model) != 0)
        msg = "use_features_from_model failed";
    if (!msg)
        msg = check_streaming(vmaf, model);

    vmaf_close(vmaf);
    vmaf_model_destroy(model);
    return msg;
}

static char *test_score_pooled_still_rejects_bad_range(void)
{
    /* Programmer-error cases must remain -EINVAL (not -EAGAIN). */
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    mu_assert("vmaf_init failed", vmaf_init(&vmaf, cfg) == 0);

    VmafModelConfig mcfg = {0};
    VmafModel *model = NULL;
    mu_assert("model load failed", vmaf_model_load(&model, &mcfg, "vmaf_v0.6.1") == 0);
    mu_assert("use_features_from_model failed", vmaf_use_features_from_model(vmaf, model) == 0);

    double score = 0.0;
    /* index_low > index_high → always -EINVAL. */
    int rc = vmaf_score_pooled(vmaf, model, VMAF_POOL_METHOD_MEAN, &score, 5u, 3u);
    mu_assert("inverted range must return -EINVAL", rc == -EINVAL);

    /* NULL score pointer → always -EINVAL. */
    rc = vmaf_score_pooled(vmaf, model, VMAF_POOL_METHOD_MEAN, NULL, 0u, 0u);
    mu_assert("NULL score pointer must return -EINVAL", rc == -EINVAL);

    vmaf_close(vmaf);
    vmaf_model_destroy(model);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_score_pooled_returns_eagain_on_pending);
    mu_run_test(test_score_pooled_streaming_pattern);
    mu_run_test(test_score_pooled_still_rejects_bad_range);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
