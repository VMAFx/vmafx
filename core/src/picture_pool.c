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

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "picture_pool.h"
#include "libvmaf/picture.h"
#include "mem.h"
#include "picture.h"
#include "ref.h"

/**
 * Extended picture private data that includes pool information.
 * This allows the release callback to return pictures to the pool.
 */
typedef struct PooledPicturePriv {
    VmafPicturePrivate base;
    VmafPicturePool *pool;
    unsigned pic_idx;
} PooledPicturePriv;

/**
 * CPU Picture Pool implementation.
 * Maintains a pool of reusable VmafPicture objects with pre-allocated data.
 * Uses a free list (stack) for O(1) allocation instead of O(n) linear scan.
 */
typedef struct VmafPicturePool {
    VmafPicturePoolConfig cfg;
    pthread_mutex_t lock;
    pthread_cond_t available;

    VmafPicture *pictures; // Array of pre-allocated pictures

    unsigned *free_list;    // Stack of available picture indices
    unsigned free_list_top; // Index of top of stack (# of free pictures)
} VmafPicturePool;

/**
 * Release callback invoked when vmaf_picture_unref() brings refcount to 0.
 * Instead of freeing the data, we return the picture to the pool and signal
 * any waiting threads.
 */
static int pooled_picture_release(VmafPicture *pic, void *cookie)
{
    (void)cookie;

    // Extract pool info from priv before it gets freed
    PooledPicturePriv *priv = (PooledPicturePriv *)pic->priv;
    VmafPicturePool *pool = priv->pool;
    unsigned idx = priv->pic_idx;

    // DON'T free pic->data[0] - it belongs to the pool picture and will be reused

    // Return picture to pool (thread-safe) - O(1) push onto free list
    pthread_mutex_lock(&pool->lock);
    pool->free_list[pool->free_list_top++] = idx;
    pthread_cond_signal(&pool->available); // Wake one waiting thread
    pthread_mutex_unlock(&pool->lock);

    return 0;
}

static int pool_preallocate_pictures(VmafPicturePool *p, VmafPicturePoolConfig cfg)
{
    /* ADR-0778 Fix-E: strip priv/ref only after the full allocation loop
     * succeeds.  The original code stripped immediately after each
     * vmaf_picture_alloc, leaving the unwind-path vmaf_picture_unref calls
     * with pic->ref == NULL, so they returned -EINVAL and leaked the buffer.
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
        // Clear priv and ref - we'll recreate them on each fetch
        free(p->pictures[i].priv);
        vmaf_ref_close(p->pictures[i].ref);
        p->pictures[i].priv = NULL;
        p->pictures[i].ref = NULL;

        // Push index onto free list (all pictures start available)
        p->free_list[i] = i;
    }
    p->free_list_top = cfg.pic_cnt; // Stack is full initially
    return 0;
}

/* Construction stages in acquisition order.  pool_destruct_partial() releases
 * exactly the resources a stage owns, in the reverse of this order, and is a
 * one-for-one replacement of the former
 * free_cond -> free_mutex -> free_free_list -> free_pictures -> free_pool
 * goto ladder: stage N frees what label N used to free, in the same order. */
enum {
    POOL_STAGE_NONE = 0,
    POOL_STAGE_POOL = 1,
    POOL_STAGE_PICTURES = 2,
    POOL_STAGE_FREE_LIST = 3,
    POOL_STAGE_MUTEX = 4,
    POOL_STAGE_COND = 5,
};

static void pool_destruct_partial(VmafPicturePool *p, unsigned stage)
{
    if (stage >= POOL_STAGE_COND)
        pthread_cond_destroy(&p->available);
    if (stage >= POOL_STAGE_MUTEX)
        pthread_mutex_destroy(&p->lock);
    if (stage >= POOL_STAGE_FREE_LIST)
        free(p->free_list);
    if (stage >= POOL_STAGE_PICTURES)
        free(p->pictures);
    if (stage >= POOL_STAGE_POOL)
        free(p);
}

/* Acquire every pool resource in order, recording how far it got in *stage so
 * the caller can unwind exactly that much.  Returns 0 or a negative errno --
 * the same values the goto ladder's `err ? err : -ENOMEM` produced. */
static int pool_construct(VmafPicturePool **pool, VmafPicturePoolConfig cfg, unsigned *stage)
{
    VmafPicturePool *const p = *pool = malloc(sizeof(*p));
    if (!p)
        return -ENOMEM;
    *stage = POOL_STAGE_POOL;
    memset(p, 0, sizeof(*p));
    p->cfg = cfg;

    p->pictures = malloc(sizeof(*p->pictures) * cfg.pic_cnt);
    if (!p->pictures)
        return -ENOMEM;
    *stage = POOL_STAGE_PICTURES;
    memset(p->pictures, 0, sizeof(*p->pictures) * cfg.pic_cnt);

    // Allocate free list (stack of available picture indices)
    p->free_list = malloc(sizeof(*p->free_list) * cfg.pic_cnt);
    if (!p->free_list)
        return -ENOMEM;
    *stage = POOL_STAGE_FREE_LIST;

    int err = pthread_mutex_init(&p->lock, NULL);
    if (err)
        return err;
    *stage = POOL_STAGE_MUTEX;

    err = pthread_cond_init(&p->available, NULL);
    if (err)
        return err;
    *stage = POOL_STAGE_COND;

    return pool_preallocate_pictures(p, cfg);
}

int vmaf_picture_pool_init(VmafPicturePool **pool, VmafPicturePoolConfig cfg)
{
    if (!pool)
        return -EINVAL;
    if (!cfg.pic_cnt)
        return -EINVAL;
    if (!cfg.w || !cfg.h)
        return -EINVAL;

    unsigned stage = POOL_STAGE_NONE;
    const int err = pool_construct(pool, cfg, &stage);
    if (!err)
        return 0;

    if (stage != POOL_STAGE_NONE)
        pool_destruct_partial(*pool, stage);
    *pool = NULL;
    return err;
}

int vmaf_picture_pool_close(VmafPicturePool *pool)
{
    if (!pool)
        return -EINVAL;

    pthread_mutex_lock(&pool->lock);

    // Wait for all pictures to be returned to the pool
    while (pool->free_list_top < pool->cfg.pic_cnt) {
        pthread_cond_wait(&pool->available, &pool->lock);
    }

    // Free all pictures (including their data buffers)
    for (unsigned i = 0; i < pool->cfg.pic_cnt; i++) {
        // Data pointers are in the picture, just free them directly
        aligned_free(pool->pictures[i].data[0]);
    }

    pthread_mutex_unlock(&pool->lock);
    pthread_cond_destroy(&pool->available);
    pthread_mutex_destroy(&pool->lock);

    free(pool->free_list);
    free(pool->pictures);
    free(pool);
    return 0;
}

/* Push `idx` back onto the free list and wake one waiter.  This is the former
 * `return_to_pool` label moved verbatim: same lock, same push, same signal,
 * same unlock, in the same order. */
static void pool_return_index(VmafPicturePool *pool, unsigned idx)
{
    // If we failed after popping from free list, return the picture
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
}

/* Block until a slot is free, pop its index, and copy the slot out while the
 * lock is still held.  Returns 0, or the pthread error that aborted the wait
 * (the lock is released on every error path, as before). */
static int pool_pop_slot(VmafPicturePool *pool, unsigned *idx, VmafPicture *snapshot)
{
    int err = pthread_mutex_lock(&pool->lock);
    if (err)
        return err;

    // Wait while no pictures are available (event-driven, no polling)
    while (pool->free_list_top == 0) {
        err = pthread_cond_wait(&pool->available, &pool->lock);
        if (err) {
            pthread_mutex_unlock(&pool->lock);
            return err;
        }
    }

    // Pop picture index from free list - O(1) operation
    *idx = pool->free_list[--pool->free_list_top];

    /* Copy the pre-allocated picture slot to a local while still holding the
     * lock.  Although a popped index cannot be re-issued (it is off the free
     * list until explicitly pushed back), pool->pictures[idx] is a shared
     * array element: another thread calling pool_close could free the array
     * between our unlock and the read.  Copying under the lock makes the
     * read race-free and keeps the critical section small (one struct copy). */
    *snapshot = pool->pictures[*idx];

    pthread_mutex_unlock(&pool->lock);
    return 0;
}

/* Attach the pool bookkeeping to a freshly fetched picture.  Each failure frees
 * exactly what it had allocated and leaves pic->priv NULL, matching the three
 * former `goto return_to_pool` sites; pushing the index back is the caller's
 * job, so the free-list handling stays in one place. */
static int pool_attach_priv(VmafPicturePool *pool, VmafPicture *pic, unsigned idx)
{
    // Set up extended priv with pool information
    PooledPicturePriv *priv = malloc(sizeof(*priv));
    if (!priv)
        return -ENOMEM;
    memset(priv, 0, sizeof(*priv));
    priv->pool = pool;
    priv->pic_idx = idx;
    pic->priv = (VmafPicturePrivate *)priv;

    // Set custom release callback to return picture to pool
    int err = vmaf_picture_set_release_callback(pic, NULL, pooled_picture_release);
    if (err) {
        free(priv);
        /* ADR-0960 (round-25 audit A.3) — null pic->priv after free so
         * any caller that inspects it after a failed fetch does not read
         * freed memory. */
        pic->priv = NULL;
        return err;
    }

    // Initialize refcount to 1
    err = vmaf_ref_init(&pic->ref);
    if (err) {
        free(priv);
        /* ADR-0960 (round-25 audit A.3) — same dangling-priv guard. */
        pic->priv = NULL;
        return err;
    }

    return 0;
}

int vmaf_picture_pool_fetch(VmafPicturePool *pool, VmafPicture *pic)
{
    if (!pool)
        return -EINVAL;
    if (!pic)
        return -EINVAL;

    unsigned idx = 0;
    VmafPicture pic_snapshot;
    int err = pool_pop_slot(pool, &idx, &pic_snapshot);
    if (err)
        return err;

    // Apply the pre-allocated picture snapshot (all metadata + data pointers)
    *pic = pic_snapshot;

    err = pool_attach_priv(pool, pic, idx);
    if (err)
        pool_return_index(pool, idx);

    return err;
}
