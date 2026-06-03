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

/* Regression test for the use-after-free that lurked in
 * vmaf_gpu_picture_pool_init() before this PR.
 *
 * The bug: the combined assignment
 *     VmafGpuPicturePool *const p = *pool = malloc(sizeof(*p));
 * published `p` to the caller's `*pool` argument *before* any of the
 * later failure paths ran. If the pic-array malloc or pthread_mutex_init
 * failed, the function freed `p` via the `goto free_p` chain but left
 * `*pool` holding the dangling pointer. The caller (libvmaf.c) then
 * stored that dangling pointer in the long-lived VmafContext, and the
 * natural vmaf_close() teardown double-freed it via
 * vmaf_gpu_picture_pool_close().
 *
 * The fix: clear *pool to NULL at each failure label. This test
 * exercises the `goto free_p` failure path by passing a `pic_cnt` so
 * large that `malloc(sizeof(VmafPicture) * pic_cnt)` is virtually
 * guaranteed to return NULL on any sane host, then asserts that
 * `*pool` is NULL on return. Pure-CPU build — no GPU backend needed.
 *
 * Run cost: microseconds. Lives in suite=fast so the CPU-only
 * meson_setup that every CI matrix entry runs catches a future
 * regression. */

#include <errno.h>
#include <stdlib.h>

#include "test.h"

#include "gpu_picture_pool.h"

/* Stub callbacks. We never expect alloc to be reached on the
 * free_p path under test, but the init pre-checks require both
 * pointers to be non-NULL. */
static int stub_alloc_unreached(VmafPicture *pic, void *cookie)
{
    (void)pic;
    (void)cookie;
    return -ENOMEM;
}

static int stub_free_noop(VmafPicture *pic, void *cookie)
{
    (void)pic;
    (void)cookie;
    return 0;
}

/* Sentinel non-NULL pointer used to detect that
 * vmaf_gpu_picture_pool_init cleared the handle on failure. We take
 * the address of a static byte rather than casting an integer literal
 * to a pointer: same effect (distinct from NULL, easy to spot in a
 * debugger) without the clang-tidy `performance-no-int-to-ptr` violation
 * the int-to-ptr cast would trip. */
static char sentinel_storage;
#define SENTINEL ((VmafGpuPicturePool *)&sentinel_storage)

/* Triggers the `goto free_p` arm: malloc(sizeof(VmafPicture) * pic_cnt)
 * is asked for ~2^31 elements, which on every realistic host fails. If
 * the host happens to fulfil the request, the test reports skip rather
 * than spuriously fail — the goal is to assert the *failure-cleanup
 * shape*, not to brute-force OOM. */
static char *test_pool_handle_cleared_on_pic_array_alloc_failure(void)
{
    VmafGpuPicturePoolConfig cfg = {
        /* UINT_MAX/2 entries * sizeof(VmafPicture) overflows or
         * exhausts virtual memory on any commodity x86_64/arm64
         * host. Pure C — no GPU dependency. */
        .pic_cnt = 0x7FFFFFFFu,
        .cookie = NULL,
        .alloc_picture_callback = stub_alloc_unreached,
        .free_picture_callback = stub_free_noop,
        .synchronize_picture_callback = NULL,
    };

    /* Seed with a non-NULL sentinel so a regression that fails to
     * clear *pool at the free_p label is detectable. The pre-fix
     * code left this pointing at the now-freed pool struct. */
    VmafGpuPicturePool *pool = SENTINEL;
    int err = vmaf_gpu_picture_pool_init(&pool, cfg);

    if (err == 0) {
        /* Host gave us 32 GiB. Free the live pool and skip. */
        (void)vmaf_gpu_picture_pool_close(pool);
        (void)fprintf(stderr, "[skip: host fulfilled implausibly large alloc] ");
        return NULL;
    }

    /* The actual UAF gate: the caller's handle must be NULL on the
     * failure path. Otherwise a downstream vmaf_close() would call
     * vmaf_gpu_picture_pool_close() on freed memory. */
    mu_assert("pool handle must be NULL after init failure (UAF regression)", pool == NULL);

    return NULL;
}

/* The -EINVAL early-return path runs before malloc(), so *pool is
 * untouched. The contract is "caller only reads *pool on success",
 * which we pin here. */
static char *test_pool_handle_not_clobbered_on_einval_early_return(void)
{
    VmafGpuPicturePoolConfig cfg = {
        .pic_cnt = 0, /* triggers the early EINVAL */
        .cookie = NULL,
        .alloc_picture_callback = stub_alloc_unreached,
        .free_picture_callback = stub_free_noop,
        .synchronize_picture_callback = NULL,
    };

    VmafGpuPicturePool *pool = SENTINEL;
    int err = vmaf_gpu_picture_pool_init(&pool, cfg);

    mu_assert("init must reject pic_cnt=0 with -EINVAL", err == -EINVAL);
    mu_assert("early-return EINVAL must not touch *pool", pool == SENTINEL);

    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_pool_handle_cleared_on_pic_array_alloc_failure);
    mu_run_test(test_pool_handle_not_clobbered_on_einval_early_return);
    return NULL;
}
