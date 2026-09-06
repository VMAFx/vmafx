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
 * Regression test for finding R2-10 — flush_context terminal-flush ordering.
 *
 * Bug
 * ---
 * flush_context_threaded() used to set vmaf->flushed = true the moment its own
 * (CPU) flush succeeded. flush_context() then ran the CUDA flush and, on a CUDA
 * error, early-returned BEFORE flush_context_sycl() — the only place SYCL
 * extractors' final-frame gpu_pending collect + flush runs. Net effect on a
 * CUDA+SYCL build: a CUDA-flush error dropped the last SYCL frame's scores, and
 * because vmaf->flushed was already true, vmaf_read_pictures(NULL, NULL) rejected
 * any retry (it returns -EINVAL once flushed). The final SYCL frame was lost
 * with no recovery path.
 *
 * Fix
 * ---
 * The terminal-flush decision is centralised in flush_context(): the inner
 * flush_context_threaded() no longer touches vmaf->flushed, and flush_context()
 * accumulates the CUDA result (instead of early-returning) so flush_context_sycl
 * still runs. vmaf->flushed flips true only after EVERY backend flush succeeded.
 *
 * Why this is load-bearing on a CPU-only build
 * --------------------------------------------
 * The white-box test below includes libvmaf.c directly so it can call the static
 * flush_context_threaded() and observe vmaf->flushed. The structural invariant —
 * "the inner threaded flush must NOT set vmaf->flushed; only flush_context()
 * does, and only after all backends ran" — is independent of which GPU backends
 * are compiled in. Pre-fix, flush_context_threaded() set vmaf->flushed = true
 * and the FIRST assertion below fails. Post-fix it leaves it false and
 * flush_context() flips it. Toggling the fix flips this test red/green without
 * any CUDA or SYCL toolchain.
 */

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "test.h"

/* White-box include: pulls in the static flush_context* helpers and the full
 * VmafContext definition. Mirrors the established pattern in
 * test_feature_collector.c. */
#include "feature_collector.c"
#include "libvmaf.c"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe, and this file mirrors
 * the C spelling of the surface it exercises. ADR-1138. */

#define FRAME_W 64u
#define FRAME_H 64u
#define FRAME_BPC 8u
#define NUM_FRAMES 4u

static int alloc_frame(VmafPicture *pic, unsigned seed)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FRAME_BPC, FRAME_W, FRAME_H);
    if (err)
        return err;

    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned r = 0; r < FRAME_H; r++) {
        for (unsigned c = 0; c < FRAME_W; c++)
            y[r * pic->stride[0] + c] = (uint8_t)((r + c + seed * 7u) & 0xFFu);
    }
    for (unsigned p = 1; p < 3; p++) {
        uint8_t *plane = (uint8_t *)pic->data[p];
        for (unsigned r = 0; r < pic->h[p]; r++)
            memset(plane + r * pic->stride[p], 128u, pic->w[p]);
    }
    return 0;
}

/* Register a temporal extractor and push NUM_FRAMES frames through the threaded
 * read path, leaving the context ready for a terminal flush. Caller owns the
 * returned context (must vmaf_close it). */
static char *prep_threaded_context(VmafContext **out)
{
    int err = 0;
    VmafConfiguration cfg = {
        .log_level = VMAF_LOG_LEVEL_NONE,
        .n_threads = 4,
    };

    VmafContext *vmaf = NULL;
    err = vmaf_init(&vmaf, cfg);
    mu_assert("vmaf_init failed", !err);
    mu_assert("thread_pool must be active for the threaded path", vmaf->thread_pool != NULL);

    err = vmaf_use_feature(vmaf, "motion", NULL);
    mu_assert("vmaf_use_feature(motion) failed", !err);

    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        VmafPicture ref, dist;
        err = alloc_frame(&ref, i);
        mu_assert("alloc_frame(ref) failed", !err);
        err = alloc_frame(&dist, i + 1u);
        mu_assert("alloc_frame(dist) failed", !err);
        err = vmaf_read_pictures(vmaf, &ref, &dist, i);
        mu_assert("vmaf_read_pictures failed", !err);
    }

    mu_assert("flushed must be false before any flush", !vmaf->flushed);
    *out = vmaf;
    return NULL;
}

/*
 * test_threaded_flush_does_not_set_flushed
 * ----------------------------------------
 * Calls the static flush_context_threaded() DIRECTLY on a fresh threaded
 * context and asserts it succeeds but does NOT flip vmaf->flushed. This is the
 * core R2-10 invariant: the inner CPU flush must not mark the context terminally
 * flushed, otherwise a subsequent CUDA-flush error in flush_context() would skip
 * flush_context_sycl() with no retry path (vmaf->flushed already true). Pre-fix,
 * flush_context_threaded() set the flag itself and the second mu_assert fails.
 *
 * A separate context is used for the flush_context() check below because calling
 * flush_context_threaded() and then flush_context() on the SAME context would
 * double-flush the motion extractor (duplicate-write -EINVAL).
 */
static char *test_threaded_flush_does_not_set_flushed(void)
{
    int err = 0;
    VmafContext *vmaf = NULL;
    char *prep = prep_threaded_context(&vmaf);
    if (prep)
        return prep;

    err = flush_context_threaded(vmaf);
    mu_assert("flush_context_threaded must succeed", !err);
    mu_assert("flush_context_threaded must NOT set vmaf->flushed (R2-10)", !vmaf->flushed);

    err = vmaf_close(vmaf);
    mu_assert("vmaf_close failed", !err);

    return NULL;
}

/*
 * test_central_flush_sets_flushed
 * -------------------------------
 * The centralised flush_context() is the only place vmaf->flushed flips true,
 * and only after every backend flush ran. On a CPU-only build (no CUDA / SYCL
 * compiled) flush_context() runs the threaded flush and must end with
 * flushed == true.
 */
static char *test_central_flush_sets_flushed(void)
{
    int err = 0;
    VmafContext *vmaf = NULL;
    char *prep = prep_threaded_context(&vmaf);
    if (prep)
        return prep;

    err = flush_context(vmaf);
    mu_assert("flush_context must succeed", !err);
    mu_assert("flush_context must set vmaf->flushed once all backends ran", vmaf->flushed);

    err = vmaf_close(vmaf);
    mu_assert("vmaf_close failed", !err);

    return NULL;
}

/*
 * test_flush_via_public_api_sets_flushed
 * --------------------------------------
 * End-to-end check through the public API: vmaf_read_pictures(NULL, NULL) routes
 * to flush_context(), which must set vmaf->flushed and reject a second flush
 * with -EINVAL. Guards against a regression where the centralised flag-set is
 * dropped entirely.
 */
static char *test_flush_via_public_api_sets_flushed(void)
{
    int err = 0;

    VmafConfiguration cfg = {
        .log_level = VMAF_LOG_LEVEL_NONE,
        .n_threads = 0, /* serial path */
    };

    VmafContext *vmaf = NULL;
    err = vmaf_init(&vmaf, cfg);
    mu_assert("serial: vmaf_init failed", !err);

    err = vmaf_use_feature(vmaf, "motion", NULL);
    mu_assert("serial: vmaf_use_feature(motion) failed", !err);

    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        VmafPicture ref, dist;
        err = alloc_frame(&ref, i);
        mu_assert("serial: alloc_frame(ref) failed", !err);
        err = alloc_frame(&dist, i + 1u);
        mu_assert("serial: alloc_frame(dist) failed", !err);
        err = vmaf_read_pictures(vmaf, &ref, &dist, i);
        mu_assert("serial: vmaf_read_pictures failed", !err);
    }

    mu_assert("serial: flushed false before EOS", !vmaf->flushed);

    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("serial: EOS flush failed", !err);
    mu_assert("serial: flushed must be true after EOS", vmaf->flushed);

    /* A second flush must be rejected — proves the no-retry-once-flushed
     * contract the fix's ordering depends on. */
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("serial: second flush must return -EINVAL", err == -EINVAL);

    err = vmaf_close(vmaf);
    mu_assert("serial: vmaf_close failed", !err);

    return NULL;
}

char *run_tests()
{
    mu_run_test(test_threaded_flush_does_not_set_flushed);
    mu_run_test(test_central_flush_sets_flushed);
    mu_run_test(test_flush_via_public_api_sets_flushed);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
