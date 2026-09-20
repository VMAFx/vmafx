/**
 *
 *  Copyright 2016-2025 Netflix, Inc.
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

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <new>
#include <pthread.h>
#include <type_traits>

#include "picture_pool.h"
#include "libvmaf/picture.h"
#include "mem.h"
#include "picture.h"
#include "ref.h"

/**
 * Extended picture private data that includes pool information.
 * This allows the release callback to return pictures to the pool.
 */
struct PooledPicturePriv {
    VmafPicturePrivate base;
    VmafPicturePool *pool{};
    unsigned pic_idx{};
};

/* vmaf_picture_unref releases pic->priv with free(), so this placement-
 * constructed extension must remain trivially destructible. */
static_assert(std::is_trivially_destructible_v<PooledPicturePriv>);

/**
 * CPU Picture Pool implementation.
 * Maintains a pool of reusable VmafPicture objects with pre-allocated data.
 * Uses a free list (stack) for O(1) allocation instead of O(n) linear scan.
 */
struct VmafPicturePool {
    VmafPicturePoolConfig cfg{};
    pthread_mutex_t lock{};
    pthread_cond_t available{};

    VmafPicture *pictures{}; /* Array of pre-allocated pictures */

    unsigned *free_list{};    /* Stack of available picture indices */
    unsigned free_list_top{}; /* Index of top of stack (# of free pictures) */
};

static_assert(std::is_trivially_destructible_v<VmafPicturePool>);

static VmafPicturePool *picture_pool_create(void)
{
    void *const storage = std::malloc(sizeof(VmafPicturePool));
    if (!storage)
        return nullptr;
    return ::new (storage) VmafPicturePool{};
}

static int default_condition_init(pthread_cond_t *condition)
{
#ifdef _WIN32
    pthread_condattr_t attributes = nullptr;
    return pthread_cond_init(condition, &attributes);
#else
    pthread_condattr_t attributes;
    const int attributes_init_err = pthread_condattr_init(&attributes);
    if (attributes_init_err)
        return attributes_init_err;

    const int condition_init_err = pthread_cond_init(condition, &attributes);
    const int attributes_destroy_err = pthread_condattr_destroy(&attributes);
    if (!condition_init_err && attributes_destroy_err) {
        const int condition_destroy_err = pthread_cond_destroy(condition);
        return condition_destroy_err ? condition_destroy_err : attributes_destroy_err;
    }
    return condition_init_err ? condition_init_err : attributes_destroy_err;
#endif
}

/**
 * Release callback invoked when vmaf_picture_unref() brings refcount to 0.
 * Instead of freeing the data, we return the picture to the pool and signal
 * any waiting threads.
 */
static int pooled_picture_release(VmafPicture *pic, const void *cookie)
{
    (void)cookie;

    /* Extract pool info from priv before it gets freed */
    PooledPicturePriv *priv = reinterpret_cast<PooledPicturePriv *>(pic->priv);
    VmafPicturePool *pool = priv->pool;
    unsigned idx = priv->pic_idx;

    /* DON'T free pic->data[0] - it belongs to the pool picture and will be reused */

    /* Return picture to pool (thread-safe) - O(1) push onto free list */
    pthread_mutex_lock(&pool->lock);
    pool->free_list[pool->free_list_top++] = idx;
    pthread_cond_signal(&pool->available); /* Wake one waiting thread */
    pthread_mutex_unlock(&pool->lock);

    return 0;
}

static VmafPicturePrivate *pooled_picture_priv_create(VmafPicturePool *pool, unsigned pic_idx)
{
    void *const storage = std::malloc(sizeof(PooledPicturePriv));
    if (!storage)
        return nullptr;

    return &(::new (storage) PooledPicturePriv{
                 .base = {},
                 .pool = pool,
                 .pic_idx = pic_idx,
             })
                ->base;
}

static int pool_preallocate_pictures(VmafPicturePool *p, const VmafPicturePoolConfig &cfg)
{
    /* ADR-0778 Fix-E: strip priv/ref only after the full allocation loop
     * succeeds.  The original code stripped immediately after each
     * vmaf_picture_alloc, leaving the unwind-path vmaf_picture_unref calls
     * with pic->ref == nullptr, so they returned -EINVAL and leaked the buffer.
     * Now: allocate all pictures first, then strip priv/ref in a second
     * pass; the error unwind uses vmaf_picture_unref on intact pictures
     * since the strip pass has not run yet. */
    for (unsigned i = 0; i < cfg.pic_cnt; i++) {
        int err = vmaf_picture_alloc(&p->pictures[i], cfg.pix_fmt, cfg.bpc, cfg.w, cfg.h);
        if (err) {
            /* Free any pictures we've already fully allocated (priv/ref still
             * intact on these since the strip pass has not run yet).
             * ADR-0778 Fix-E: two-pass approach; vmaf_picture_unref is safe
             * here because priv/ref have not yet been cleared. */
            for (unsigned j = 0; j < i; j++) {
                (void)vmaf_picture_unref(&p->pictures[j]);
            }
            return err;
        }
    }

    /* All pictures allocated successfully — now strip priv and ref so that
     * pool_fetch can (re-)initialise them on every fetch without leaking the
     * originals. */
    for (unsigned i = 0; i < cfg.pic_cnt; i++) {
        /* Clear priv and ref — we'll recreate them on each fetch */
        std::free(p->pictures[i].priv);
        vmaf_ref_close(p->pictures[i].ref);
        p->pictures[i].priv = nullptr;
        p->pictures[i].ref = nullptr;

        /* Push index onto free list (all pictures start available) */
        p->free_list[i] = i;
    }
    p->free_list_top = cfg.pic_cnt; /* Stack is full initially */
    return 0;
}

int vmaf_picture_pool_init(VmafPicturePool **pool, VmafPicturePoolConfig cfg)
{
    if (!pool)
        return -EINVAL;
    if (!cfg.pic_cnt)
        return -EINVAL;
    if (!cfg.w || !cfg.h)
        return -EINVAL;

    VmafPicturePool *const p = picture_pool_create();
    if (!p) {
        *pool = nullptr;
        return -ENOMEM;
    }
    *pool = p;
    p->cfg = cfg;

    p->pictures = static_cast<VmafPicture *>(std::malloc(sizeof(*p->pictures) * cfg.pic_cnt));
    if (!p->pictures) {
        p->~VmafPicturePool();
        std::free(p);
        *pool = nullptr;
        return -ENOMEM;
    }
    std::memset(p->pictures, 0, sizeof(*p->pictures) * cfg.pic_cnt);

    /* Allocate free list (stack of available picture indices) */
    p->free_list = static_cast<unsigned *>(std::malloc(sizeof(*p->free_list) * cfg.pic_cnt));
    if (!p->free_list) {
        std::free(p->pictures);
        p->~VmafPicturePool();
        std::free(p);
        *pool = nullptr;
        return -ENOMEM;
    }

    int err = pthread_mutex_init(&p->lock, nullptr);
    if (err)
        goto free_free_list;

    err = default_condition_init(&p->available);
    if (err)
        goto free_mutex;

    err = pool_preallocate_pictures(p, cfg);
    if (err)
        goto free_cond;

    return 0;

free_cond:
    pthread_cond_destroy(&p->available);
free_mutex:
    pthread_mutex_destroy(&p->lock);
free_free_list:
    std::free(p->free_list);
    std::free(p->pictures);
    p->~VmafPicturePool();
    std::free(p);
    *pool = nullptr;
    return err;
}

int vmaf_picture_pool_close(VmafPicturePool *pool)
{
    if (!pool)
        return -EINVAL;

    pthread_mutex_lock(&pool->lock);

    /* Wait for all pictures to be returned to the pool */
    while (pool->free_list_top < pool->cfg.pic_cnt) {
        pthread_cond_wait(&pool->available, &pool->lock);
    }

    /* Free all pictures (including their data buffers) */
    for (unsigned i = 0; i < pool->cfg.pic_cnt; i++) {
        aligned_free(pool->pictures[i].data[0]);
    }

    pthread_mutex_unlock(&pool->lock);
    pthread_cond_destroy(&pool->available);
    pthread_mutex_destroy(&pool->lock);

    std::free(pool->free_list);
    std::free(pool->pictures);
    pool->~VmafPicturePool();
    std::free(pool);
    return 0;
}

int vmaf_picture_pool_fetch(VmafPicturePool *pool, VmafPicture *pic)
{
    if (!pool)
        return -EINVAL;
    if (!pic)
        return -EINVAL;

    int err = pthread_mutex_lock(&pool->lock);
    if (err)
        return err;

    /* Wait while no pictures are available (event-driven, no polling) */
    while (pool->free_list_top == 0) {
        err = pthread_cond_wait(&pool->available, &pool->lock);
        if (err) {
            pthread_mutex_unlock(&pool->lock);
            return err;
        }
    }

    /* Pop picture index from free list - O(1) operation */
    unsigned idx = pool->free_list[--pool->free_list_top];

    /* Copy the pre-allocated picture slot to a local while still holding the
     * lock.  Although a popped index cannot be re-issued (it is off the free
     * list until explicitly pushed back), pool->pictures[idx] is a shared
     * array element: another thread calling pool_close could free the array
     * between our unlock and the read.  Copying under the lock makes the
     * read race-free and keeps the critical section small (one struct copy). */
    const VmafPicture pic_snapshot = pool->pictures[idx];

    pthread_mutex_unlock(&pool->lock);

    /* Apply the pre-allocated picture snapshot (all metadata + data pointers) */
    *pic = pic_snapshot;

    /* Set up extended priv with pool information */
    pic->priv = pooled_picture_priv_create(pool, idx);
    if (!pic->priv) {
        err = -ENOMEM;
        goto return_to_pool;
    }

    /* Set custom release callback to return picture to pool */
    err = vmaf_picture_set_release_callback(pic, nullptr, pooled_picture_release);
    if (err) {
        std::free(pic->priv);
        /* ADR-0960 (round-25 audit A.3) — null pic->priv after free so
         * any caller that inspects it after a failed fetch does not read
         * freed memory. */
        pic->priv = nullptr;
        goto return_to_pool;
    }

    /* Initialize refcount to 1 */
    err = vmaf_ref_init(&pic->ref);
    if (err) {
        std::free(pic->priv);
        /* ADR-0960 (round-25 audit A.3) — same dangling-priv guard. */
        pic->priv = nullptr;
        goto return_to_pool;
    }

    return 0;

return_to_pool:
    /* If we failed after popping from free list, return the picture */
    pthread_mutex_lock(&pool->lock);
    pool->free_list[pool->free_list_top++] = idx;
    /* ADR-0960 (round-25 audit A.2) — signal waiters; without this a
     * thread blocked in pthread_cond_wait (pool exhausted) would not
     * wake after an index is pushed back on a fetch-error path.
     * See feedback_shared_resource_outlive_worker_scope (PR #1415,
     * ADR-0607) for the canonical "shared resource must outlive its
     * owner" pattern this mirrors. */
    pthread_cond_signal(&pool->available);
    pthread_mutex_unlock(&pool->lock);
    return err;
}
