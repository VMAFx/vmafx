/**
 *
 *  Copyright 2016-2023 Netflix, Inc.
 *  Copyright 2022 NVIDIA Corporation.
 *  Copyright 2026 Lusoris and Claude (Anthropic)
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

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "gpu_picture_pool.h"

#ifdef HAVE_NVTX
#include "nvtx3/nvToolsExt.h"
#endif

typedef struct VmafGpuPicturePool {
    VmafGpuPicturePoolConfig cfg;
    unsigned curr_idx;
    pthread_mutex_t busy;
    VmafPicture *pic;
} VmafGpuPicturePool;

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

    VmafGpuPicturePool *const p = *pool = malloc(sizeof(*p));
    if (!p) {
        err = -ENOMEM;
        goto fail;
    }
    memset(p, 0, sizeof(*p));
    p->cfg = cfg;

    /* calloc to zero every slot before any callback runs, so partial-
     * init unwind can call free_picture_callback on uninitialised slots
     * (their data[] / priv fields are NULL and the callback short-
     * circuits). Round-26 audit. */
    p->pic = calloc(p->cfg.pic_cnt, sizeof(VmafPicture));
    if (!p->pic) {
        err = -ENOMEM;
        goto free_p;
    }

    err = pthread_mutex_init(&p->busy, NULL);
    if (err)
        goto free_pic;

    /* Allocate every slot. On first failure, unwind the slots that
     * succeeded, destroy the mutex, free the pool, and return the
     * error code. Previously the loop OR-aggregated all callback
     * results then returned mid-initialised state with *pool set —
     * callers (e.g. picture_sycl.cpp) propagated the error code,
     * called `delete wrap`, and leaked the per-slot device memory +
     * mutex + p->pic. Round-26 audit (ADR-0982). */
    for (unsigned i = 0; i < p->cfg.pic_cnt; i++) {
        const int alloc_err = p->cfg.alloc_picture_callback(&p->pic[i], p->cfg.cookie);
        if (alloc_err != 0) {
            err = alloc_err;
            for (unsigned j = 0; j < i; j++) {
                (void)p->cfg.free_picture_callback(&p->pic[j], p->cfg.cookie);
            }
            (void)pthread_mutex_destroy(&p->busy);
            free(p->pic);
            free(p);
            *pool = NULL;
            return err;
        }
    }

    return 0;

free_pic:
    free(p->pic);
free_p:
    free(p);
    *pool = NULL;
fail:
    return err;
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

    free(pool->pic);
    free(pool);
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
    static unsigned glob = 0;
    snprintf(n, sizeof(n), "fetch idx %d %d", pic_idx, glob++);
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
