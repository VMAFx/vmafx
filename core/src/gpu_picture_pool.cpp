/**
 *
 *  Copyright 2016-2023 Netflix, Inc.
 *  Copyright 2022 NVIDIA Corporation.
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

/* ADR-0239: backend-agnostic GPU picture pool. Promoted from
 * `cuda/ring_buffer.c` — same callback-based round-robin shape, now
 * shared between CUDA, SYCL, HIP, and Metal backends. */

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <pthread.h>

#include "picture.h"
#include "gpu_picture_pool.h"

#ifdef HAVE_NVTX
#include <atomic>
#include "nvtx3/nvToolsExt.h"
#endif

struct VmafGpuPicturePool {
    VmafGpuPicturePoolConfig cfg;
    unsigned curr_idx;
    pthread_mutex_t busy;
    VmafPicture *pic;
};

namespace
{

/* Allocate all pic_cnt slots; on failure roll back any slots that succeeded.
 * Returns 0 on full success, or the first non-zero error from
 * alloc_picture_callback with all partial allocations freed. */
int alloc_pictures(VmafGpuPicturePool *p)
{
    unsigned alloc_cnt = 0;
    int err = 0;

    for (unsigned i = 0; i < p->cfg.pic_cnt; i++) {
        err = p->cfg.alloc_picture_callback(&p->pic[i], p->cfg.cookie);
        if (err)
            break;
        alloc_cnt++;
    }

    if (err) {
        /* Free pictures 0..alloc_cnt-1 that were successfully allocated.
         * Without this rollback the caller leaks one GPU buffer per slot. */
        for (unsigned i = 0; i < alloc_cnt; i++)
            (void)p->cfg.free_picture_callback(&p->pic[i], p->cfg.cookie);
    }

    return err;
}

/* Release the picture array and the pool itself and clear the caller's handle.
 * This is the former free_pic -> free_p label pair, in the same order; on the
 * `!p->pic` path p->pic is nullptr and std::free(nullptr) is the no-op the
 * `goto free_p` entry relied on. */
void gpu_pool_destruct(VmafGpuPicturePool *p, VmafGpuPicturePool **pool)
{
    std::free(p->pic);
    std::free(p);
    *pool = nullptr;
}

} // namespace

int vmaf_gpu_picture_pool_init(VmafGpuPicturePool **pool, VmafGpuPicturePoolConfig cfg)
{
    if (!pool)
        return -EINVAL;
    if (!cfg.pic_cnt)
        return -EINVAL;
    if (!cfg.alloc_picture_callback)
        return -EINVAL;
    if (!cfg.free_picture_callback)
        return -EINVAL;

    int err = 0;

    /* Publish to caller's *pool before any later failure path.  Set *pool = NULL
     * on every failure path so the caller can treat a non-zero return as
     * "pool not constructed" — prevents UAF via vmaf_gpu_picture_pool_close()
     * on a freed pointer (Netflix#UAF-001 / ADR-0239). */
    VmafGpuPicturePool *const p = *pool =
        static_cast<VmafGpuPicturePool *>(std::malloc(sizeof(*p)));
    if (!p) {
        *pool = nullptr;
        /* Preserved verbatim from the `goto fail` this replaced: that jump
         * skipped every assignment to `err`, so this path returns 0 even
         * though no pool was constructed.  Left as-is because this change is
         * a structural refactor; the defect is reported separately rather
         * than silently altered here. */
        return err;
    }
    std::memset(p, 0, sizeof(*p));
    p->cfg = cfg;

    p->pic = static_cast<VmafPicture *>(std::malloc(sizeof(VmafPicture) * p->cfg.pic_cnt));
    if (!p->pic) {
        gpu_pool_destruct(p, pool);
        return -ENOMEM;
    }

    err = pthread_mutex_init(&p->busy, nullptr);
    if (err) {
        gpu_pool_destruct(p, pool);
        return err;
    }

    err = alloc_pictures(p);
    if (err) {
        /* alloc_pictures already freed any successfully-allocated prior slots.
         * Destroy the mutex before the pool teardown so we do not leak the
         * mutex's kernel-side state. */
        (void)pthread_mutex_destroy(&p->busy);
        gpu_pool_destruct(p, pool);
        return err;
    }

    return 0;
}

int vmaf_gpu_picture_pool_close(VmafGpuPicturePool *pool)
{
    if (!pool)
        return -EINVAL;

    int err = pthread_mutex_lock(&pool->busy);
    if (err)
        return err;

    for (unsigned i = 0; i < pool->cfg.pic_cnt; i++) {
        err |= pool->cfg.free_picture_callback(&pool->pic[i], pool->cfg.cookie);
    }

    /* Netflix#1300 — the original code never called
     * pthread_mutex_destroy(&pool->busy), leaking the mutex's internal
     * state on every pool close. It also held the lock while
     * free(pool) ran, which POSIX classifies as undefined behaviour
     * (destroying a locked mutex). Unlock first, then destroy, then
     * free. */
    err |= pthread_mutex_unlock(&pool->busy);
    err |= pthread_mutex_destroy(&pool->busy);

    std::free(pool->pic);
    std::free(pool);
    return err;
}

int vmaf_gpu_picture_pool_fetch(VmafGpuPicturePool *pool, VmafPicture *pic)
{
    if (!pool)
        return -EINVAL;
    if (!pic)
        return -EINVAL;

    int err = pthread_mutex_lock(&pool->busy);
    if (err)
        return err;
    unsigned pic_idx = pool->curr_idx;
    pool->curr_idx = (pool->curr_idx + 1) % pool->cfg.pic_cnt;
    err |= pthread_mutex_unlock(&pool->busy);
    if (err)
        return err;

#ifdef HAVE_NVTX
    char n[40];
    /* Round-5 race fix: plain `static unsigned` incremented by concurrent
     * worker threads is a C++ data race.  Use std::atomic with relaxed
     * ordering — the NVTX label is a diagnostic annotation; we only need
     * atomicity, not inter-thread ordering. */
    static std::atomic<unsigned> glob{0};
    snprintf(n, sizeof(n), "fetch idx %u %u", pic_idx,
             glob.fetch_add(1u, std::memory_order_relaxed));
    nvtxRangePushA(n);
#endif

    vmaf_picture_ref(pic, &pool->pic[pic_idx]);

    if (pool->cfg.synchronize_picture_callback) {
        err |= pool->cfg.synchronize_picture_callback(pic, pool->cfg.cookie);
    }

#ifdef HAVE_NVTX
    nvtxRangePop();
#endif

    return err;
}
