/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Regression test for ADR-0982 (round-26 GPU runtime bug audit):
 *  vmaf_gpu_picture_pool_init unwind on partial alloc-callback failure.
 *
 *  Pre-fix: when `alloc_picture_callback` failed for any slot, the loop
 *  OR-aggregated the error and continued. The caller saw a non-zero err
 *  with *pool set, called its own destructor on the wrapper, and leaked
 *  the per-slot resources of the slots that DID succeed, plus the
 *  mutex, plus the slot array.
 *
 *  Post-fix: on first per-slot failure, the pool calls
 *  free_picture_callback for every successfully-allocated prior slot,
 *  destroys the mutex, frees the slot array, frees the pool, and
 *  NULLs *pool.
 *
 *  This test is CPU-only (no GPU runtime needed): both the alloc and
 *  free callbacks are pure-C stubs that count their invocations. ASan
 *  / UBSan / valgrind catches any leaked allocation if the unwind
 *  regresses.
 */

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"

#include "gpu_picture_pool.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but the Windows
 * MSVC legs compile the test tree with cl.exe, whose documented /std:clatest
 * C23 feature set does not include `nullptr`. Same carve-out and reasoning as
 * core/src/feature/float_motion.c. ADR-1138. */

typedef struct {
    unsigned succeed_n;   /* allocate fine for the first N calls; fail after */
    unsigned alloc_calls; /* total alloc calls observed */
    unsigned free_calls;  /* total free calls observed */
} TestCookie;

static int stub_alloc(VmafPicture *pic, void *cookie)
{
    TestCookie *c = (TestCookie *)cookie;
    c->alloc_calls++;
    if (c->alloc_calls > c->succeed_n) {
        return -ENOMEM;
    }
    /* Allocate a 1-byte heap buffer so ASan flags a leak if the unwind
     * misses this slot. The bookkeeping mirrors what a real GPU
     * alloc_picture_callback does (pic->data[0] owns the buffer). */
    memset(pic, 0, sizeof(*pic));
    pic->data[0] = malloc(1);
    if (!pic->data[0]) {
        return -ENOMEM;
    }
    return 0;
}

static int stub_free(VmafPicture *pic, void *cookie)
{
    TestCookie *c = (TestCookie *)cookie;
    c->free_calls++;
    if (pic && pic->data[0]) {
        free(pic->data[0]);
        pic->data[0] = NULL;
    }
    return 0;
}

static char *test_pool_init_unwinds_partial_success(void)
{
    TestCookie cookie = {.succeed_n = 3, .alloc_calls = 0, .free_calls = 0};
    VmafGpuPicturePoolConfig cfg = {
        .pic_cnt = 8,
        .alloc_picture_callback = stub_alloc,
        .free_picture_callback = stub_free,
        .synchronize_picture_callback = NULL,
        .cookie = &cookie,
    };
    VmafGpuPicturePool *pool = NULL;

    int err = vmaf_gpu_picture_pool_init(&pool, cfg);

    /* Pool init must report the first failing alloc's error. */
    mu_assert("pool_init must propagate the alloc-callback error", err == -ENOMEM);
    /* *pool must be NULLed on failure (no half-initialised handle handed back). */
    mu_assert("pool_init must NULL out *pool on failure", pool == NULL);
    /* The allocator must have stopped at the first failure (4th call). */
    mu_assert("alloc_picture_callback must be called 4 times (3 success + 1 fail)",
              cookie.alloc_calls == 4);
    /* Round-26 unwind must call free_picture_callback for each of the
     * 3 successfully-allocated slots. Pre-fix value was 0. */
    mu_assert("free_picture_callback must be called for each successful prior slot",
              cookie.free_calls == 3);
    return NULL;
}

static char *test_pool_init_success_path_unaffected(void)
{
    TestCookie cookie = {.succeed_n = 8, .alloc_calls = 0, .free_calls = 0};
    VmafGpuPicturePoolConfig cfg = {
        .pic_cnt = 8,
        .alloc_picture_callback = stub_alloc,
        .free_picture_callback = stub_free,
        .synchronize_picture_callback = NULL,
        .cookie = &cookie,
    };
    VmafGpuPicturePool *pool = NULL;

    int err = vmaf_gpu_picture_pool_init(&pool, cfg);
    mu_assert("pool_init must succeed when all alloc calls succeed", err == 0);
    mu_assert("pool_init must populate *pool on success", pool != NULL);
    mu_assert("alloc_picture_callback must be called exactly pic_cnt times on success",
              cookie.alloc_calls == 8);
    mu_assert("no free callback on the success path of init", cookie.free_calls == 0);

    int close_err = vmaf_gpu_picture_pool_close(pool);
    mu_assert("pool_close must succeed", close_err == 0);
    mu_assert("pool_close must call free_picture_callback for every slot", cookie.free_calls == 8);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_pool_init_unwinds_partial_success);
    mu_run_test(test_pool_init_success_path_unaffected);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
