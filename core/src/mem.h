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

#ifndef __VMAF_MEM_H__
#define __VMAF_MEM_H__

#include <stddef.h>

/** @brief Default SIMD alignment boundary in bytes (AVX2 / AVX-512 safe). */
#define MAX_ALIGN 32

/** @brief Round @p x down to the nearest MAX_ALIGN boundary. */
#define ALIGN_FLOOR(x) ((x) - (x) % MAX_ALIGN)
/** @brief Round @p x up to the nearest MAX_ALIGN boundary. */
#define ALIGN_CEIL(x) ((x) + ((x) % MAX_ALIGN ? MAX_ALIGN - (x) % MAX_ALIGN : 0))

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Allocate @p size bytes aligned to @p alignment.
 *
 * @param size       Number of bytes to allocate.
 * @param alignment  Required alignment; must be a power of two.
 * @return Pointer to the allocated memory, or NULL on failure.
 *         Free with aligned_free().
 */
void *aligned_malloc(size_t size, size_t alignment);

/**
 * @brief Free memory allocated by aligned_malloc().
 *
 * @param ptr  Pointer to free.  May be NULL (no-op).
 */
void aligned_free(void *ptr);

#ifdef __cplusplus
}
#endif

#endif /* __VMAF_MEM_H__ */
