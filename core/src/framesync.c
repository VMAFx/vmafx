/**
 *
 *  Copyright 2016-2023 Netflix, Inc.
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
#include <string.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include "framesync.h"

/* POSIX guarantees that pthread_mutex_lock / pthread_cond_wait always see
 * writes to plain int that were made while the mutex was held, so no
 * C11 _Atomic is required for the aborted flag. */

enum {
    BUF_FREE = 0,
    BUF_ACQUIRED,
    BUF_FILLED,
    BUF_RETRIEVED,
};

typedef struct VmafFrameSyncBuf {
    void *frame_data;
    int buf_status;
    signed long index;
    struct VmafFrameSyncBuf *next;
} VmafFrameSyncBuf;

typedef struct VmafFrameSyncContext {
    VmafFrameSyncBuf *buf_que;
    pthread_mutex_t acquire_lock;
    pthread_mutex_t retrieve_lock;
    pthread_cond_t retrieve;
    unsigned buf_cnt;
    /* Set to 1 by vmaf_framesync_abort(); causes retrieve_filled_data to
     * return -ECANCELED instead of waiting forever when a producer dies. */
    int aborted;
} VmafFrameSyncContext;

int vmaf_framesync_init(VmafFrameSyncContext **fs_ctx)
{
    VmafFrameSyncContext *const ctx = *fs_ctx = malloc(sizeof(VmafFrameSyncContext));
    if (!ctx)
        return -ENOMEM;
    memset(ctx, 0, sizeof(VmafFrameSyncContext));
    ctx->buf_cnt = 1;

    pthread_mutex_init(&(ctx->acquire_lock), NULL);
    pthread_mutex_init(&(ctx->retrieve_lock), NULL);
    pthread_cond_init(&(ctx->retrieve), NULL);

    VmafFrameSyncBuf *buf_que = ctx->buf_que = malloc(sizeof(VmafFrameSyncBuf));
    if (!buf_que) {
        pthread_cond_destroy(&(ctx->retrieve));
        pthread_mutex_destroy(&(ctx->retrieve_lock));
        pthread_mutex_destroy(&(ctx->acquire_lock));
        free(ctx);
        *fs_ctx = NULL;
        return -ENOMEM;
    }

    buf_que->frame_data = NULL;
    buf_que->buf_status = BUF_FREE;
    buf_que->index = -1;
    buf_que->next = NULL;

    return 0;
}

/*
 * Lock ordering — strict M0-before-M1.
 *
 * `acquire_lock` (M0) is the *structural* lock: it protects the
 * `buf_que` linked-list spine — `next` pointers, `buf_cnt`, the FREE
 * <-> ACQUIRED <-> RETRIEVED state transitions on `buf_status` that
 * gate node-allocation/teardown — and the per-node `frame_data`
 * pointer. `retrieve_lock` (M1) is the *signal* lock: it protects the
 * ACQUIRED -> FILLED -> RETRIEVED handshake that pairs producer
 * (`submit_filled_data`) with consumer (`retrieve_filled_data`) and is
 * the lock the `retrieve` condvar binds to.
 *
 * Prior to this fix the four entry points used inconsistent lock
 * domains: producer and consumer paths walked the list under M1 only,
 * while the structural mutators walked it under M0 only. TSan flagged
 * `SAN-FRAMESYNC-MUTEX-DOMAIN` because the spine is mutated under M0
 * but read under M1 in the FILLED/RETRIEVED scan. Per
 * `feedback_no_test_weakening` we fix the implementation, not the
 * gate: every entry point that traverses the spine now takes M0 first
 * (covering the spine), then M1 *only on the producer/consumer paths
 * that need to interact with the condvar*. Strict M0-before-M1
 * ordering precludes deadlock.
 */

int vmaf_framesync_acquire_new_buf(VmafFrameSyncContext *fs_ctx, void **data, unsigned data_sz,
                                   unsigned index)
{
    VmafFrameSyncBuf *buf_que = fs_ctx->buf_que;
    *data = NULL;

    int rc = pthread_mutex_lock(&(fs_ctx->acquire_lock));
    if (rc != 0)
        return -rc;

    // traverse until a free buffer is found
    for (unsigned i = 0; i < fs_ctx->buf_cnt; i++) {
        if (buf_que->buf_status == BUF_FREE) {
            buf_que->frame_data = *data = malloc(data_sz);
            if (!buf_que->frame_data) {
                (void)pthread_mutex_unlock(&(fs_ctx->acquire_lock));
                return -ENOMEM;
            }
            buf_que->buf_status = BUF_ACQUIRED;
            buf_que->index = index;
            break;
        }
        // move to next node
        if (buf_que->next != NULL)
            buf_que = buf_que->next;
    }

    // create a new node if all nodes are occupied in the list and append to the tail
    if (*data == NULL) {
        VmafFrameSyncBuf *new_buf_node = malloc(sizeof(VmafFrameSyncBuf));
        if (!new_buf_node) {
            (void)pthread_mutex_unlock(&(fs_ctx->acquire_lock));
            return -ENOMEM;
        }
        new_buf_node->buf_status = BUF_FREE;
        new_buf_node->index = -1;
        new_buf_node->next = NULL;

        new_buf_node->frame_data = *data = malloc(data_sz);
        if (!new_buf_node->frame_data) {
            free(new_buf_node);
            *data = NULL;
            (void)pthread_mutex_unlock(&(fs_ctx->acquire_lock));
            return -ENOMEM;
        }
        new_buf_node->buf_status = BUF_ACQUIRED;
        new_buf_node->index = index;

        // Publish the new node + bump `buf_cnt` atomically with respect
        // to other M0 holders. The `next` write must happen *before*
        // `buf_cnt` is incremented so a reader walking the spine under
        // M0 with i < buf_cnt always sees a valid `next` pointer.
        buf_que->next = new_buf_node;
        fs_ctx->buf_cnt++;
    }

    (void)pthread_mutex_unlock(&(fs_ctx->acquire_lock));

    return 0;
}

int vmaf_framesync_submit_filled_data(VmafFrameSyncContext *fs_ctx, void *data, unsigned index)
{
    VmafFrameSyncBuf *buf_que = fs_ctx->buf_que;
    const signed long frame_index = (signed long)index;
    int ret = 0;

    // M0-before-M1: take the structural lock first to walk the spine
    // safely, then take the signal lock for the condvar broadcast.
    int rc = pthread_mutex_lock(&(fs_ctx->acquire_lock));
    if (rc != 0)
        return -rc;
    rc = pthread_mutex_lock(&(fs_ctx->retrieve_lock));
    if (rc != 0) {
        (void)pthread_mutex_unlock(&(fs_ctx->acquire_lock));
        return -rc;
    }

    // loop until a matching buffer is found
    for (unsigned i = 0; i < fs_ctx->buf_cnt; i++) {
        if ((buf_que->index == frame_index) && (buf_que->buf_status == BUF_ACQUIRED)) {
            buf_que->buf_status = BUF_FILLED;
            if (data != buf_que->frame_data) {
                ret = -1;
            }
            break;
        }

        // move to next node
        if (NULL != buf_que->next)
            buf_que = buf_que->next;
    }

    (void)pthread_cond_broadcast(&(fs_ctx->retrieve));
    (void)pthread_mutex_unlock(&(fs_ctx->retrieve_lock));
    (void)pthread_mutex_unlock(&(fs_ctx->acquire_lock));

    return ret;
}

int vmaf_framesync_retrieve_filled_data(VmafFrameSyncContext *fs_ctx, void **data, unsigned index)
{
    const signed long frame_index = (signed long)index;
    *data = NULL;

    while (*data == NULL) {
        VmafFrameSyncBuf *buf_que = fs_ctx->buf_que;

        // M0-before-M1: spine first, condvar lock second. The
        // `pthread_cond_wait` below atomically releases M1 and waits;
        // M0 is dropped before the wait so producers can make
        // progress.
        int rc = pthread_mutex_lock(&(fs_ctx->acquire_lock));
        if (rc != 0)
            return -rc;
        rc = pthread_mutex_lock(&(fs_ctx->retrieve_lock));
        if (rc != 0) {
            (void)pthread_mutex_unlock(&(fs_ctx->acquire_lock));
            return -rc;
        }

        // loop until a free buffer is found
        for (unsigned i = 0; i < fs_ctx->buf_cnt; i++) {
            if ((buf_que->index == frame_index) && (buf_que->buf_status == BUF_FILLED)) {
                buf_que->buf_status = BUF_RETRIEVED;
                *data = buf_que->frame_data;
                break;
            }

            // move to next node
            if (NULL != buf_que->next)
                buf_que = buf_que->next;
        }

        if (*data == NULL) {
            // Check abort flag (under M1) before sleeping; a producer that
            // died after calling vmaf_framesync_abort() may have broadcast
            // before we entered cond_wait, but the flag is persistent.
            if (fs_ctx->aborted) {
                (void)pthread_mutex_unlock(&(fs_ctx->retrieve_lock));
                (void)pthread_mutex_unlock(&(fs_ctx->acquire_lock));
                return -ECANCELED;
            }
            // Release M0 before waiting so producers can
            // acquire/append. M1 is released atomically by
            // pthread_cond_wait and re-acquired on wake-up; we then
            // re-take M0 at the top of the loop in canonical order.
            (void)pthread_mutex_unlock(&(fs_ctx->acquire_lock));
            (void)pthread_cond_wait(&(fs_ctx->retrieve), &(fs_ctx->retrieve_lock));
            // Re-check abort flag after waking: the broadcast may have been
            // fired by abort() rather than by a real submit_filled_data().
            if (fs_ctx->aborted) {
                (void)pthread_mutex_unlock(&(fs_ctx->retrieve_lock));
                return -ECANCELED;
            }
            (void)pthread_mutex_unlock(&(fs_ctx->retrieve_lock));
        } else {
            (void)pthread_mutex_unlock(&(fs_ctx->retrieve_lock));
            (void)pthread_mutex_unlock(&(fs_ctx->acquire_lock));
        }
    }

    return 0;
}

int vmaf_framesync_release_buf(VmafFrameSyncContext *fs_ctx, void *data, unsigned index)
{
    VmafFrameSyncBuf *buf_que = fs_ctx->buf_que;
    const signed long frame_index = (signed long)index;
    int ret = 0;

    int rc = pthread_mutex_lock(&(fs_ctx->acquire_lock));
    if (rc != 0)
        return -rc;
    // loop until a matching buffer is found
    for (unsigned i = 0; i < fs_ctx->buf_cnt; i++) {
        if ((buf_que->index == frame_index) && (buf_que->buf_status == BUF_RETRIEVED)) {
            if (data != buf_que->frame_data) {
                ret = -1;
                break;
            }

            free(buf_que->frame_data);
            buf_que->frame_data = NULL;
            buf_que->buf_status = BUF_FREE;
            buf_que->index = -1;
            break;
        }

        // move to next node
        if (NULL != buf_que->next)
            buf_que = buf_que->next;
    }

    (void)pthread_mutex_unlock(&(fs_ctx->acquire_lock));
    return ret;
}

/*
 * vmaf_framesync_abort — signal all consumers to stop waiting and return
 * -ECANCELED.  Call this on any producer error path so that a consumer
 * blocked in retrieve_filled_data does not hang indefinitely.
 *
 * The broadcast is sent while holding retrieve_lock (M1) so it is not
 * lost against a concurrent retrieve_filled_data that holds M1 up to
 * the pthread_cond_wait call.  acquire_lock (M0) is NOT taken here so
 * that a producer that holds M0 during acquire_new_buf can still reach
 * this abort path without deadlocking.
 */
int vmaf_framesync_abort(VmafFrameSyncContext *fs_ctx)
{
    int rc = pthread_mutex_lock(&(fs_ctx->retrieve_lock));
    if (rc != 0)
        return -rc;
    fs_ctx->aborted = 1;
    (void)pthread_cond_broadcast(&(fs_ctx->retrieve));
    (void)pthread_mutex_unlock(&(fs_ctx->retrieve_lock));
    return 0;
}

int vmaf_framesync_destroy(VmafFrameSyncContext *fs_ctx)
{
    VmafFrameSyncBuf *buf_que = fs_ctx->buf_que;

    /* Safety net: abort any consumer still in retrieve_filled_data so the
     * cond_wait has a chance to wake and exit before we destroy the condvar.
     * POSIX defines pthread_cond_destroy while there are waiters as UB.
     * The caller must still drain all threads (e.g. via vmaf_thread_pool_wait)
     * before calling destroy, but this broadcast provides defence-in-depth. */
    (void)vmaf_framesync_abort(fs_ctx);

    pthread_mutex_destroy(&(fs_ctx->acquire_lock));
    pthread_mutex_destroy(&(fs_ctx->retrieve_lock));
    pthread_cond_destroy(&(fs_ctx->retrieve));

    //check for any data buffers which are not freed
    while (buf_que != NULL) {
        VmafFrameSyncBuf *next = buf_que->next;
        if (NULL != buf_que->frame_data) {
            free(buf_que->frame_data);
            buf_que->frame_data = NULL;
        }
        free(buf_que);
        buf_que = next;
    }

    free(fs_ctx);

    return 0;
}
