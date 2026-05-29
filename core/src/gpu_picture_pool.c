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
 * shared between CUDA, SYCL, and (after PR #264 lands) Vulkan. */

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
    if (!p)
        goto fail;
    memset(p, 0, sizeof(*p));
    p->cfg = cfg;

    p->pic = malloc(sizeof(VmafPicture) * p->cfg.pic_cnt);
    if (!p->pic) {
        err = -ENOMEM;
        goto free_p;
    }

    err = pthread_mutex_init(&p->busy, NULL);
    if (err)
        goto free_pic;

    for (unsigned i = 0; i < p->cfg.pic_cnt; i++)
        err |= p->cfg.alloc_picture_callback(&p->pic[i], p->cfg.cookie);

    return err;

free_pic:
    free(p->pic);
free_p:
    free(p);
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
