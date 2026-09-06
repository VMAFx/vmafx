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
 * TSan-eligible thread-safety test for threaded_extract_batch_func (ADR-1072/ADR-1073).
 *
 * Context
 * -------
 * PRs #765 and #769 fixed two bugs in the threaded batch extraction path:
 *
 *   PR #765 (ADR-1072): threaded_extract_batch_func bare-memset'd fex->prev_ref
 *   after extract() without first calling vmaf_picture_unref, leaking one pool
 *   slot per PREV_REF frame and deadlocking the picture pool after ~pool_size
 *   frames.
 *
 *   PR #769 (ADR-1073): flush_context_threaded called flush() on the shared,
 *   never-initialized extractor context. flush() lazily allocated
 *   s->feature_name_dict against the shared instance; vmaf_feature_extractor_
 *   context_close returned early (is_initialized == false) and leaked the dict.
 *   Fix: set fex_ctx->is_initialized = true before the flush loop.
 *
 * Coverage gap
 * ------------
 * test_pic_preallocation.c exercises the same code paths but is excluded from
 * the ASan and TSan sanitizer runs because vmaf_preallocate_pictures() allocates
 * large pool buffers (40 x 1920x1080 ~ 95 MB) that the TSan shadow-memory
 * allocator cannot satisfy, triggering SIGABRT before the test logic runs.
 *
 * This test avoids vmaf_preallocate_pictures() entirely (uses vmaf_picture_alloc
 * instead) and keeps frames small (64x64) so the TSan shadow overhead is trivial.
 * It targets the same code paths with n_threads=4 (exercises the batch path that
 * calls threaded_extract_batch_func) and an integer_motion extractor (PREV_REF
 * flag set, which is the class that triggered ADR-1072).
 *
 * What is verified
 * ----------------
 *   1. threaded_extract_batch_func processes N frames with a PREV_REF extractor
 *      (integer_motion) without deadlocking or leaking pool slots.
 *   2. flush_context_threaded runs after vmaf_thread_pool_wait returns,
 *      sets is_initialized on the shared fex_ctx, and flush() frees
 *      s->feature_name_dict (no ASan/TSan leak report).
 *   3. motion_score at frame 1 is finite and non-negative (sanity check that
 *      the extractor actually produced output).
 *   4. The entire sequence terminates cleanly under TSan with no data-race
 *      reports on fex->prev_ref, td->fex_ctx, or fex_ctx->is_initialized.
 *
 * Sanitizer eligibility
 * ---------------------
 * The test does NOT use vmaf_preallocate_pictures(), RLIMIT_AS, or any
 * intentional huge-allocation pattern. It is therefore eligible for all
 * sanitizer runs: ASan, UBSan, and TSan.
 */

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "test.h"

#include "libvmaf/libvmaf.h"
#include "libvmaf/picture.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe, and this file mirrors
 * the C spelling of the surface it exercises. ADR-1138. */

/* 64x64 satisfies the integer_motion minimum (w >= 3, h >= 3) and keeps
 * TSan shadow overhead well under 1 MB for the entire test run. */
#define FRAME_W 64u
#define FRAME_H 64u
#define FRAME_BPC 8u

/* 8 frames gives: frame 0 is the seed, frames 1-7 each emit a motion_score,
 * and the EOS flush exercises flush_context_threaded with the is_initialized
 * fix (ADR-1073). More frames than the default thread pool queue depth (4)
 * ensures the pool is busy across multiple dispatch cycles. */
#define NUM_FRAMES 8u

static int alloc_frame(VmafPicture *pic, unsigned seed)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FRAME_BPC, FRAME_W, FRAME_H);
    if (err)
        return err;

    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned r = 0; r < FRAME_H; r++) {
        for (unsigned c = 0; c < FRAME_W; c++) {
            /* Per-frame luma variation ensures non-zero motion scores. */
            y[r * pic->stride[0] + c] = (uint8_t)((r + c + seed * 7u) & 0xFFu);
        }
    }

    /* Chroma planes: uniform mid-grey (motion extractor is luma-only). */
    for (unsigned p = 1; p < 3; p++) {
        uint8_t *plane = (uint8_t *)pic->data[p];
        for (unsigned r = 0; r < pic->h[p]; r++)
            memset(plane + r * pic->stride[p], 128u, pic->w[p]);
    }

    return 0;
}

/*
 * test_batch_prev_ref_lifecycle
 * ------------------------------
 * Core thread-safety test:
 *
 *   - vmaf_init with n_threads=4 → activates threaded_read_pictures_batch path.
 *   - vmaf_use_feature("motion") registers integer_motion, which carries
 *     VMAF_FEATURE_EXTRACTOR_PREV_REF. This is the flag that caused the
 *     ADR-1072 refcount leak (bare memset without vmaf_picture_unref).
 *   - 8 frames processed; each worker thread in threaded_extract_batch_func
 *     writes td->fex_ctx[i]->fex->prev_ref (thread-private deep copy) and
 *     then vmaf_picture_unref + memset it (the ADR-1072 fix).
 *   - EOS flush (vmaf_read_pictures NULL/NULL) triggers flush_context_threaded:
 *     vmaf_thread_pool_wait() serializes all workers, then the main thread
 *     sets fex_ctx->is_initialized = true and calls fex->flush() (ADR-1073 fix).
 *   - After vmaf_close(), no leaked allocation or data race should be visible.
 */
static char *test_batch_prev_ref_lifecycle(void)
{
    int err = 0;

    VmafConfiguration cfg = {
        .log_level = VMAF_LOG_LEVEL_NONE,
        .n_threads = 4,
    };

    VmafContext *vmaf = NULL;
    err = vmaf_init(&vmaf, cfg);
    mu_assert("vmaf_init failed", !err);

    /* integer_motion carries VMAF_FEATURE_EXTRACTOR_PREV_REF; its flush()
     * path lazily allocates feature_name_dict (the ADR-1073 root cause). */
    err = vmaf_use_feature(vmaf, "motion", NULL);
    mu_assert("vmaf_use_feature(motion) failed", !err);

    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        VmafPicture ref, dist;
        err = alloc_frame(&ref, i);
        mu_assert("alloc_frame(ref) failed", !err);
        err = alloc_frame(&dist, i + 1u); /* distinct to produce non-zero motion */
        mu_assert("alloc_frame(dist) failed", !err);

        err = vmaf_read_pictures(vmaf, &ref, &dist, i);
        mu_assert("vmaf_read_pictures failed", !err);
    }

    /* EOS: triggers flush_context_threaded (pool wait + flush loop). */
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("vmaf_read_pictures(EOS) failed", !err);

    /* motion_sad_score is written unconditionally from frame 1 onward (frame 0
     * has no previous frame to compare against). motion_score is only written
     * when debug=true; use motion_sad_score as the always-present sanity check. */
    for (unsigned i = 1u; i < 4u; i++) {
        double score = -1.0;
        err = vmaf_feature_score_at_index(vmaf, "VMAF_integer_feature_motion_sad_score", &score, i);
        mu_assert("vmaf_feature_score_at_index(motion_sad_score) failed", !err);
        mu_assert("motion_sad_score must be finite", isfinite(score));
        mu_assert("motion_sad_score must be non-negative", score >= 0.0);
    }

    err = vmaf_close(vmaf);
    mu_assert("vmaf_close failed", !err);

    return NULL;
}

/*
 * test_batch_flush_initialized_flag
 * -----------------------------------
 * Isolated regression test for the ADR-1073 is_initialized gate:
 *
 * After flush_context_threaded sets fex_ctx->is_initialized = true and
 * calls fex->flush(), vmaf_close -> feature_extractor_vector_destroy ->
 * vmaf_feature_extractor_context_close must NOT return -EINVAL early.
 * If close() is skipped, fex->close is never called and any state
 * allocated by flush() leaks (detected by ASan with detect_leaks=1).
 *
 * With n_threads=4 the threaded path is taken; with n_threads=0 the
 * serial path (flush_context_serial) is taken. Both must produce a
 * motion_score at frame 1.
 */
static char *test_batch_flush_initialized_flag(void)
{
    int err = 0;

    /* Run with n_threads=4 (batch path) and verify motion score. */
    {
        VmafConfiguration cfg = {
            .log_level = VMAF_LOG_LEVEL_NONE,
            .n_threads = 4,
        };

        VmafContext *vmaf = NULL;
        err = vmaf_init(&vmaf, cfg);
        mu_assert("threaded: vmaf_init failed", !err);

        err = vmaf_use_feature(vmaf, "motion", NULL);
        mu_assert("threaded: vmaf_use_feature(motion) failed", !err);

        for (unsigned i = 0; i < 3u; i++) {
            VmafPicture ref, dist;
            err = alloc_frame(&ref, i);
            mu_assert("threaded: alloc_frame(ref) failed", !err);
            err = alloc_frame(&dist, i + 1u);
            mu_assert("threaded: alloc_frame(dist) failed", !err);
            err = vmaf_read_pictures(vmaf, &ref, &dist, i);
            mu_assert("threaded: vmaf_read_pictures failed", !err);
        }

        err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
        mu_assert("threaded: EOS failed", !err);

        double score = -1.0;
        err =
            vmaf_feature_score_at_index(vmaf, "VMAF_integer_feature_motion_sad_score", &score, 1u);
        mu_assert("threaded: motion_score at index 1 failed", !err);
        mu_assert("threaded: motion_score finite", isfinite(score));

        err = vmaf_close(vmaf);
        mu_assert("threaded: vmaf_close failed", !err);
    }

    /* Run with n_threads=0 (serial path) and verify same constraint. */
    {
        VmafConfiguration cfg = {
            .log_level = VMAF_LOG_LEVEL_NONE,
            .n_threads = 0,
        };

        VmafContext *vmaf = NULL;
        err = vmaf_init(&vmaf, cfg);
        mu_assert("serial: vmaf_init failed", !err);

        err = vmaf_use_feature(vmaf, "motion", NULL);
        mu_assert("serial: vmaf_use_feature(motion) failed", !err);

        for (unsigned i = 0; i < 3u; i++) {
            VmafPicture ref, dist;
            err = alloc_frame(&ref, i);
            mu_assert("serial: alloc_frame(ref) failed", !err);
            err = alloc_frame(&dist, i + 1u);
            mu_assert("serial: alloc_frame(dist) failed", !err);
            err = vmaf_read_pictures(vmaf, &ref, &dist, i);
            mu_assert("serial: vmaf_read_pictures failed", !err);
        }

        err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
        mu_assert("serial: EOS failed", !err);

        double score = -1.0;
        err =
            vmaf_feature_score_at_index(vmaf, "VMAF_integer_feature_motion_sad_score", &score, 1u);
        mu_assert("serial: motion_score at index 1 failed", !err);
        mu_assert("serial: motion_score finite", isfinite(score));

        err = vmaf_close(vmaf);
        mu_assert("serial: vmaf_close failed", !err);
    }

    return NULL;
}

/*
 * test_batch_n_threads_stress
 * ----------------------------
 * Stress the thread pool with a larger frame count (16) and a higher thread
 * count (8) to increase the probability that TSan detects latent races on
 * fex->prev_ref or td->fex_ctx[i]. Under TSan this runs single-threaded in
 * terms of progress but with instrumented happens-before tracking, so false
 * data-race reports will surface if any shared write escapes the thread-local
 * BatchThreadData.
 */
static char *test_batch_n_threads_stress(void)
{
    int err = 0;

    VmafConfiguration cfg = {
        .log_level = VMAF_LOG_LEVEL_NONE,
        .n_threads = 8,
    };

    VmafContext *vmaf = NULL;
    err = vmaf_init(&vmaf, cfg);
    mu_assert("stress: vmaf_init failed", !err);

    err = vmaf_use_feature(vmaf, "motion", NULL);
    mu_assert("stress: vmaf_use_feature(motion) failed", !err);

    for (unsigned i = 0; i < 16u; i++) {
        VmafPicture ref, dist;
        err = alloc_frame(&ref, i);
        mu_assert("stress: alloc_frame(ref) failed", !err);
        err = alloc_frame(&dist, i + 1u);
        mu_assert("stress: alloc_frame(dist) failed", !err);
        err = vmaf_read_pictures(vmaf, &ref, &dist, i);
        mu_assert("stress: vmaf_read_pictures failed", !err);
    }

    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("stress: EOS failed", !err);

    /* Verify frames 1, 5, 10, 14 all have finite non-negative motion scores. */
    const unsigned check_indices[] = {1u, 5u, 10u, 14u};
    for (unsigned k = 0; k < 4u; k++) {
        double score = -1.0;
        err = vmaf_feature_score_at_index(vmaf, "VMAF_integer_feature_motion_sad_score", &score,
                                          check_indices[k]);
        mu_assert("stress: motion_score failed", !err);
        mu_assert("stress: motion_score finite", isfinite(score));
        mu_assert("stress: motion_score non-negative", score >= 0.0);
    }

    err = vmaf_close(vmaf);
    mu_assert("stress: vmaf_close failed", !err);

    return NULL;
}

/*
 * test_batch_two_prev_ref_extractors
 * ----------------------------------
 * Regression test for the multi-PREV_REF starvation bug in
 * threaded_read_pictures_batch (libvmaf.c).
 *
 * The batch loop took a single refcounted snapshot of prev_ref (f->prev_ref)
 * and struct-copied it into the first PREV_REF extractor's thread-private ctx,
 * then ZEROED f->prev_ref after that extractor's PREV_REF swap. With TWO
 * PREV_REF extractors registered (e.g. motion + motion_v2 — and requesting
 * motion_v2 always co-schedules motion), the second extractor saw
 * f->prev_ref.ref == NULL and its extract() returned -EINVAL on every frame
 * ("problem with feature extractor motion_v2"), so vmaf_read_pictures failed
 * and no motion_v2 scores were produced. This made every threaded K150K
 * feature extraction (--threads N, motion + motion_v2) fail 100%.
 *
 * The fix gives each PREV_REF extractor its own counted reference
 * (vmaf_picture_ref instead of a struct-copy) and leaves the shared snapshot
 * live for all extractors until the final unref. This test registers BOTH
 * motion and motion_v2 under n_threads=4 and asserts that every frame reads
 * cleanly AND that motion_v2's own feature (motion_v2_sad_score) is present
 * and finite — which it never was before the fix.
 */
static char *test_batch_two_prev_ref_extractors(void)
{
    int err = 0;

    VmafConfiguration cfg = {
        .log_level = VMAF_LOG_LEVEL_NONE,
        .n_threads = 4,
    };

    VmafContext *vmaf = NULL;
    err = vmaf_init(&vmaf, cfg);
    mu_assert("two-prev-ref: vmaf_init failed", !err);

    /* Two PREV_REF extractors in the same batch — the starvation trigger. */
    err = vmaf_use_feature(vmaf, "motion", NULL);
    mu_assert("two-prev-ref: vmaf_use_feature(motion) failed", !err);
    err = vmaf_use_feature(vmaf, "motion_v2", NULL);
    mu_assert("two-prev-ref: vmaf_use_feature(motion_v2) failed", !err);

    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        VmafPicture ref, dist;
        err = alloc_frame(&ref, i);
        mu_assert("two-prev-ref: alloc_frame(ref) failed", !err);
        err = alloc_frame(&dist, i + 1u);
        mu_assert("two-prev-ref: alloc_frame(dist) failed", !err);

        /* Pre-fix this returned -EINVAL from frame 1 (motion_v2 starved). */
        err = vmaf_read_pictures(vmaf, &ref, &dist, i);
        mu_assert("two-prev-ref: vmaf_read_pictures failed (motion_v2 starved?)", !err);
    }

    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("two-prev-ref: EOS failed", !err);

    /* BOTH extractors must have produced their own per-frame SAD feature.
     * motion_v2_sad_score is the one the bug suppressed entirely. */
    for (unsigned i = 1u; i < 4u; i++) {
        double m1 = -1.0, m2 = -1.0;
        err = vmaf_feature_score_at_index(vmaf, "VMAF_integer_feature_motion_sad_score", &m1, i);
        mu_assert("two-prev-ref: motion_sad_score missing", !err);
        mu_assert("two-prev-ref: motion_sad_score finite", isfinite(m1) && m1 >= 0.0);

        err = vmaf_feature_score_at_index(vmaf, "VMAF_integer_feature_motion_v2_sad_score", &m2, i);
        mu_assert("two-prev-ref: motion_v2_sad_score missing (regression!)", !err);
        mu_assert("two-prev-ref: motion_v2_sad_score finite", isfinite(m2) && m2 >= 0.0);
    }

    err = vmaf_close(vmaf);
    mu_assert("two-prev-ref: vmaf_close failed", !err);

    return NULL;
}

char *run_tests()
{
    mu_run_test(test_batch_prev_ref_lifecycle);
    mu_run_test(test_batch_flush_initialized_flag);
    mu_run_test(test_batch_n_threads_stress);
    mu_run_test(test_batch_two_prev_ref_extractors);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
