/**
 *
 *  Copyright 2016-2026 Netflix, Inc.
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

#ifndef __VMAF_THREAD_POOL_H__
#define __VMAF_THREAD_POOL_H__

#include <pthread.h>

/** @brief Opaque thread-pool handle. */
typedef struct VmafThreadPool VmafThreadPool;

/**
 * @brief Configuration passed to vmaf_thread_pool_create().
 *
 * @var VmafThreadPoolConfig::n_threads
 *   Number of worker threads to spawn.
 * @var VmafThreadPoolConfig::thread_data_free
 *   Optional destructor called on each thread's private data pointer when the
 *   pool is destroyed.  May be NULL if no per-thread state is allocated.
 */
typedef struct VmafThreadPoolConfig {
    unsigned n_threads;
    void (*thread_data_free)(void *thread_data);
} VmafThreadPoolConfig;

/**
 * @brief Create a thread pool with the given configuration.
 *
 * @param[out] tpool  Receives the newly-created pool on success.
 * @param cfg         Pool configuration (copied by value).
 * @return 0 on success, negative errno on failure.
 */
int vmaf_thread_pool_create(VmafThreadPool **tpool, VmafThreadPoolConfig cfg);

/**
 * @brief Enqueue a work item, waiting while the bounded queue is full.
 *
 * At most one pending job per successfully created worker is retained. A
 * dequeuing worker wakes a producer; running jobs are outside this bound.
 * A registered capacity waiter returns -ECANCELED during coordinated shutdown.
 * Callbacks must not recursively enqueue into this same pool or wait on work
 * whose submission requires the blocked producer to advance.
 *
 * @p data is copied internally (up to @p data_sz bytes), so the caller's
 * buffer may be reused or freed immediately after this call returns.
 *
 * The worker function returns an int error code (0 = success, negative errno =
 * failure).  Non-zero return values are OR-accumulated into the pool's
 * @c last_error field, which is returned by the next call to
 * vmaf_thread_pool_wait().  This allows callers to detect that at least one
 * job failed without needing to share a separate out-parameter.
 *
 * @param pool      Thread pool.
 * @param func      Work function; receives a copy of @p data and a pointer to
 *                  this thread's private state slot (set to NULL on first use).
 *                  Must return 0 on success or a negative errno on failure.
 * @param data      Input data for @p func.
 * @param data_sz   Size of @p data in bytes.
 * @return 0 on success, negative errno on failure.
 */
int vmaf_thread_pool_enqueue(VmafThreadPool *pool, int (*func)(void *data, void **thread_data),
                             const void *data, size_t data_sz);

/**
 * @brief Block until all enqueued work items have completed.
 *
 * @param pool  Thread pool to drain.
 * @return 0 on success, negative errno on failure.
 */
int vmaf_thread_pool_wait(VmafThreadPool *pool);

/**
 * @brief Discard queued jobs, finish active jobs, and free the pool.
 *
 * Call vmaf_thread_pool_wait() first to drain all accepted work. Destruction
 * wakes and waits for registered queue-capacity waiters. The owner must
 * externally serialize destruction against API entry, including callers
 * waiting to acquire the queue mutex. Cancellation is supported only when
 * the owner has established that the concurrent producers are already in
 * the capacity wait. Otherwise stop and join producers before destruction.
 *
 * @param tpool  Pool to destroy. NULL returns -EINVAL.
 * @return 0 on success, negative errno on failure.
 */
int vmaf_thread_pool_destroy(VmafThreadPool *tpool);

#endif /* __VMAF_THREAD_POOL_H__ */
