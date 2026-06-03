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
#else
#include <stdatomic.h>
#endif

typedef struct VmafRef {
    atomic_int cnt;
} VmafRef;

#ifdef __cplusplus
extern "C" {
#endif

int vmaf_ref_init(VmafRef **ref);
void vmaf_ref_fetch_increment(VmafRef *ref);
long vmaf_ref_fetch_decrement(VmafRef *ref);
long vmaf_ref_load(VmafRef *ref);
int vmaf_ref_close(VmafRef *ref);

#ifdef __cplusplus
}
#endif

#endif /* __VMAF_SRC_REF_H__ */
