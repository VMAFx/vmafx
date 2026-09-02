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

#ifndef __VMAF_FRAME_SYNC_H__
#define __VMAF_FRAME_SYNC_H__

#include <pthread.h>
/* In C++ translation units <stdatomic.h> conflicts with GCC 14 + Clang-18:
 * GCC 14's C++ stdatomic.h wrapper includes <atomic>, then Clang's own
 * stdatomic.h retypedefs atomic_int as _Atomic(int) which conflicts with
 * the C++ atomic<int> already in scope.  framesync.h uses no atomic_*
 * types in its declarations — the include here is vestigial. */
#if !defined(__cplusplus)
#include <stdatomic.h>
#endif
#include <stdint.h>
#include <stdlib.h>
#include "libvmaf/libvmaf.h"

/**
 * @brief Opaque context that synchronises producer/consumer frame data across
 *        feature-extractor threads.
 *
 * Each frame slot is reference-counted through acquire / submit / retrieve /
 * release.  A slot may be reused once every consumer has called
 * vmaf_framesync_release_buf() for that index.
 */
typedef struct VmafFrameSyncContext VmafFrameSyncContext;

/**
 * @brief Allocate and initialise a new VmafFrameSyncContext.
 *
 * @param[out] fs_ctx  Receives the newly-allocated context on success.
 * @return 0 on success, negative errno on failure.
 */
int vmaf_framesync_init(VmafFrameSyncContext **fs_ctx);

/**
 * @brief Acquire a buffer slot for @p index, blocking until one is free.
 *
 * @param fs_ctx   Frame-sync context.
 * @param[out] data    Set to the allocated buffer on success.
 * @param data_sz  Size in bytes of the buffer to allocate.
 * @param index    Frame index this buffer is associated with.
 * @return 0 on success, negative errno on failure.
 */
int vmaf_framesync_acquire_new_buf(VmafFrameSyncContext *fs_ctx, void **data, unsigned data_sz,
                                   unsigned index);

/**
 * @brief Mark a filled buffer as ready for retrieval.
 *
 * @param fs_ctx  Frame-sync context.
 * @param data    Buffer previously obtained via vmaf_framesync_acquire_new_buf().
 * @param index   Frame index associated with @p data.
 * @return 0 on success, negative errno on failure.
 */
int vmaf_framesync_submit_filled_data(VmafFrameSyncContext *fs_ctx, void *data, unsigned index);

/**
 * @brief Retrieve the buffer that was submitted for @p index, blocking until
 *        it is available.
 *
 * @param fs_ctx    Frame-sync context.
 * @param[out] data Set to the submitted buffer pointer on success.
 * @param index     Frame index to retrieve.
 * @return 0 on success, negative errno on failure.
 */
int vmaf_framesync_retrieve_filled_data(VmafFrameSyncContext *fs_ctx, void **data, unsigned index);

/**
 * @brief Release a buffer slot, allowing it to be reused.
 *
 * @param fs_ctx  Frame-sync context.
 * @param data    Buffer to release (must have been retrieved first).
 * @param index   Frame index associated with @p data.
 * @return 0 on success, negative errno on failure.
 */
int vmaf_framesync_release_buf(VmafFrameSyncContext *fs_ctx, void *data, unsigned index);

/**
 * vmaf_framesync_abort - wake all retrieve_filled_data waiters and make them
 * return -ECANCELED.  Call on any producer error path before joining threads.
 * Idempotent: safe to call more than once or concurrently.
 */
int vmaf_framesync_abort(VmafFrameSyncContext *fs_ctx);

/**
 * @brief Destroy a VmafFrameSyncContext and free all associated resources.
 *
 * @param fs_ctx  Context to destroy.  May be NULL (no-op).
 * @return 0 on success, negative errno on failure.
 */
int vmaf_framesync_destroy(VmafFrameSyncContext *fs_ctx);

#endif /* __VMAF_FRAME_SYNC_H__ */
