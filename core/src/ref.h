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

#ifndef __VMAF_SRC_REF_H__
#define __VMAF_SRC_REF_H__

/* In C++ mode, mixing GCC 14 system headers with Clang-18 causes a
 * typedef conflict: GCC 14's stdatomic.h wrapper includes <atomic>
 * (giving atomic<int> as atomic_int), then Clang-18's own stdatomic.h
 * fires and tries to typedef _Atomic(int) as atomic_int — a clash.
 * MSVC C++ never exposed atomic_int in the global namespace at all.
 * Use <atomic> + using-declaration in all C++ TUs to avoid the conflict. */
#if defined(__cplusplus)
#include <atomic>
using std::atomic_int;
/* Pull in the memory-order constants so C++ TUs can write
 * memory_order_acq_rel / memory_order_relaxed etc. without a std::
 * prefix — mirroring the bare C11 macros visible in the C branch. */
using std::memory_order_acq_rel;
using std::memory_order_acquire;
using std::memory_order_relaxed;
using std::memory_order_release;
using std::memory_order_seq_cst;
#else
#include <stdatomic.h>
#endif

/**
 * @brief Atomic reference counter used by VmafPicture and similar objects.
 *
 * All operations are sequentially consistent unless noted otherwise.
 */
typedef struct VmafRef {
    atomic_int cnt;
} VmafRef;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Allocate a VmafRef with an initial count of 1.
 *
 * @param[out] ref  Receives the newly-allocated ref on success.
 * @return 0 on success, -ENOMEM on allocation failure.
 */
int vmaf_ref_init(VmafRef **ref);

/**
 * @brief Atomically increment the reference count.
 *
 * @param ref  Reference counter to increment (must not be NULL).
 */
void vmaf_ref_fetch_increment(VmafRef *ref);

/**
 * @brief Atomically decrement the reference count and return the new value.
 *
 * When the returned value reaches 0 the caller is responsible for destroying
 * the owning object.
 *
 * @param ref  Reference counter to decrement (must not be NULL).
 * @return New reference count after decrement.
 */
long vmaf_ref_fetch_decrement(VmafRef *ref);

/**
 * @brief Load the current reference count without modifying it.
 *
 * @param ref  Reference counter to read.
 * @return Current reference count.
 */
long vmaf_ref_load(VmafRef *ref);

/**
 * @brief Free a VmafRef allocated by vmaf_ref_init().
 *
 * The caller must ensure the count has reached 0 before calling this.
 *
 * @param ref  Ref to free.  May be NULL (no-op).
 * @return 0 on success, negative errno on failure.
 */
int vmaf_ref_close(VmafRef *ref);

#ifdef __cplusplus
}
#endif

#endif /* __VMAF_SRC_REF_H__ */
