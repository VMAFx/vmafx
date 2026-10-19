/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * A feature score of a frame the context was fed can be read before the flush
 * with worker threads (T-ENGINE-READ-FED-FRAME-EINVAL-2026-10-06).
 *
 * vmaf_feature_score_at_index() fenced (Netflix/vmaf#1305) only when the
 * collector answered -EAGAIN, an existing but unwritten slot. With worker
 * threads the slot of a frame still being extracted may not exist yet: no
 * score of the feature written so far, or the vector not grown to the index.
 * The collector then answers -EINVAL, and the call returned it at once,
 * although the frame had been fed and the documented meaning of -EINVAL is a
 * feature no registered extractor writes. vmaf_feature_score_pooled() reads
 * through the same call. The fix fences on -EINVAL too when the index is at
 * most the last frame fed.
 *
 * Failing first: without the fix, test_read_each_frame_as_fed,
 * test_read_past_the_vector_capacity and test_pooled_before_flush each fail
 * (measured on master 4bbbc1faa, each run alone).
 *
 * The extractor is float_ssim because the worker pool runs it: psnr is
 * temporal and runs on the calling thread, so its score is written before
 * vmaf_read_pictures() returns.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "libvmaf/libvmaf.h"
#include "mu_table.h"
#include "sanitizer_build.h"
#include "test.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

/* Large enough that one frame's extraction outlasts the read that follows its
 * submission; past the collector's initial vector capacity of eight. A
 * sanitizer build extracts many times slower, so 320x180 (float_ssim at scale
 * 1) still outlasts the read there and keeps the test well inside its 30 s;
 * 1080p took 10 s to over 30 s under ASan + UBSan on a shared runner
 * (T-FED-FRAME-TEST-SANITIZER-TIMEOUT-2026-10-09). */
enum {
    W = VMAF_TEST_SANITIZER_BUILD ? 320 : 1920,
    H = VMAF_TEST_SANITIZER_BUILD ? 180 : 1080,
    N_FRAMES = 12,
    N_THREADS = 2
};

static int fill_picture(VmafPicture *pic, unsigned seed)
{
    const int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, 8, W, H);
    if (err)
        return err;
    for (unsigned p = 0; p < 3u; p++) {
        uint8_t *row = pic->data[p];
        for (unsigned y = 0; y < pic->h[p]; y++, row += pic->stride[p]) {
            for (unsigned x = 0; x < pic->w[p]; x++)
                row[x] = (uint8_t)((x * 7u + y * 13u + seed * 29u + p * 3u) & 0xffu);
        }
    }
    return 0;
}

static bool open_context(VmafContext **vmaf, unsigned n_threads)
{
    VmafConfiguration cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.n_threads = n_threads;
    return vmaf_init(vmaf, cfg) == 0 && vmaf_use_feature(*vmaf, "float_ssim", NULL) == 0;
}

static bool feed(VmafContext *vmaf, unsigned index)
{
    VmafPicture ref;
    VmafPicture dist;
    if (fill_picture(&ref, index))
        return false;
    if (fill_picture(&dist, index + 50u)) {
        (void)vmaf_picture_unref(&ref);
        return false;
    }
    return vmaf_read_pictures(vmaf, &ref, &dist, index) == 0;
}

static bool flush_and_close(VmafContext *vmaf)
{
    const bool flushed = vmaf_read_pictures(vmaf, NULL, NULL, 0) == 0;
    return vmaf_close(vmaf) == 0 && flushed;
}

static bool same_bits(double a, double b)
{
    uint64_t x = 0;
    uint64_t y = 0;
    memcpy(&x, &a, sizeof(x));
    memcpy(&y, &b, sizeof(y));
    return x == y;
}

/* float_ssim of every frame from a single-threaded, flushed session. */
static bool reference_scores(double *scores)
{
    VmafContext *vmaf = NULL;
    if (!open_context(&vmaf, 0))
        return false;
    bool ok = true;
    for (unsigned i = 0; ok && i < N_FRAMES; i++)
        ok = feed(vmaf, i);
    ok = ok && vmaf_read_pictures(vmaf, NULL, NULL, 0) == 0;
    for (unsigned i = 0; ok && i < N_FRAMES; i++)
        ok = vmaf_feature_score_at_index(vmaf, "float_ssim", &scores[i], i) == 0;
    return vmaf_close(vmaf) == 0 && ok;
}

/* Positive and boundary: the frame just fed is the last frame fed. */
static char *test_read_each_frame_as_fed(void)
{
    double expected[N_FRAMES];
    mu_assert("reference scores", reference_scores(expected));
    VmafContext *vmaf = NULL;
    mu_assert("context", open_context(&vmaf, N_THREADS));
    for (unsigned i = 0; i < N_FRAMES; i++) {
        mu_assert("feed", feed(vmaf, i));
        double score = 0.;
        mu_assert("the frame just fed is read, not refused",
                  vmaf_feature_score_at_index(vmaf, "float_ssim", &score, i) == 0);
        mu_assert("the single-threaded score", same_bits(score, expected[i]));
    }
    mu_assert("flush and close", flush_and_close(vmaf));
    return NULL;
}

/* The collector's vectors start with eight slots: frame 8 has no slot until
 * a worker writes it, even after frames 0 to 7 are scored. */
static char *test_read_past_the_vector_capacity(void)
{
    VmafContext *vmaf = NULL;
    mu_assert("context", open_context(&vmaf, N_THREADS));
    for (unsigned i = 0; i < 8u; i++)
        mu_assert("feed", feed(vmaf, i));
    double score = 0.;
    mu_assert("frame 7", vmaf_feature_score_at_index(vmaf, "float_ssim", &score, 7) == 0);
    mu_assert("feed frame 8", feed(vmaf, 8));
    mu_assert("frame 8 is read, not refused",
              vmaf_feature_score_at_index(vmaf, "float_ssim", &score, 8) == 0);
    mu_assert("flush and close", flush_and_close(vmaf));
    return NULL;
}

/* vmaf_feature_score_pooled() reads every frame through the same call. */
static char *test_pooled_before_flush(void)
{
    VmafContext *vmaf = NULL;
    mu_assert("context", open_context(&vmaf, N_THREADS));
    mu_assert("feed", feed(vmaf, 0));
    double mean = 0.;
    mu_assert("pooled over the frame fed",
              vmaf_feature_score_pooled(vmaf, "float_ssim", VMAF_POOL_METHOD_MEAN, &mean, 0, 0) ==
                  0);
    mu_assert("flush and close", flush_and_close(vmaf));
    return NULL;
}

/* Negative: a name no registered extractor writes is still -EINVAL, after the
 * fence, for a frame that was fed. */
static char *test_unknown_feature_still_einval(void)
{
    VmafContext *vmaf = NULL;
    mu_assert("context", open_context(&vmaf, N_THREADS));
    mu_assert("feed", feed(vmaf, 0));
    double score = 0.;
    mu_assert("unknown feature",
              vmaf_feature_score_at_index(vmaf, "no_such_feature", &score, 0) == -EINVAL);
    mu_assert("flush and close", flush_and_close(vmaf));
    return NULL;
}

/* Boundary: one past the last frame fed has no score. The collector answers
 * -EAGAIN when the vector already has an unwritten slot there and -EINVAL when
 * it has none; either way the call returns without a score. */
static char *test_frame_not_fed_has_no_score(void)
{
    VmafContext *vmaf = NULL;
    mu_assert("context", open_context(&vmaf, N_THREADS));
    mu_assert("feed", feed(vmaf, 0));
    double score = 0.;
    const int err = vmaf_feature_score_at_index(vmaf, "float_ssim", &score, 1);
    mu_assert("frame 1 was not fed", err == -EAGAIN || err == -EINVAL);
    mu_assert("flush and close", flush_and_close(vmaf));
    return NULL;
}

char *run_tests(void)
{
    static const MuTest tests[] = {
        MU_TEST(test_read_each_frame_as_fed),     MU_TEST(test_read_past_the_vector_capacity),
        MU_TEST(test_pooled_before_flush),        MU_TEST(test_unknown_feature_still_einval),
        MU_TEST(test_frame_not_fed_has_no_score),
    };
    return mu_run_table(tests, MU_TABLE_LEN(tests));
}

/* NOLINTEND(modernize-use-nullptr) */
