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

#ifndef __VMAF_SRC_FEX_CTX_VECTOR_H__
#define __VMAF_SRC_FEX_CTX_VECTOR_H__

/* Outside the extern "C" block: in C++ mode feature_extractor.h pulls in
 * <atomic>, whose templates cannot take C linkage. The header carries its
 * own extern "C" guard for its declarations. */
#include "feature/feature_extractor.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Dynamic array of registered feature-extractor contexts.
 *
 * Owned by the top-level VmafContext.  All three lifecycle functions must be
 * called from the same thread that owns the VmafContext.
 *
 * @var RegisteredFeatureExtractors::fex_ctx   Heap-allocated context pointer array.
 * @var RegisteredFeatureExtractors::cnt        Number of live entries.
 * @var RegisteredFeatureExtractors::capacity   Allocated capacity of @p fex_ctx.
 */
typedef struct {
    VmafFeatureExtractorContext **fex_ctx;
    unsigned cnt, capacity;
} RegisteredFeatureExtractors;

/**
 * @brief Initialise an empty RegisteredFeatureExtractors vector.
 *
 * @param rfe  Vector to initialise (must not be NULL).
 * @return 0 on success, negative errno on failure.
 */
int feature_extractor_vector_init(RegisteredFeatureExtractors *rfe);

/**
 * @brief Append a feature-extractor context to the vector, growing it if needed.
 *
 * @param rfe      Target vector.
 * @param fex_ctx  Context to append (ownership transfers to the vector).
 * @param flags    Registration flags forwarded to the context.
 * @return 0 on success, negative errno on failure.
 */
int feature_extractor_vector_append(RegisteredFeatureExtractors *rfe,
                                    VmafFeatureExtractorContext *fex_ctx, uint64_t flags);

/**
 * @brief Close every initialized or partially initialized context.
 *
 * Ownership remains in the vector. A failed close is retryable by calling
 * this function again.
 *
 * @param rfe  Vector whose contexts should be prepared for destruction.
 * @return 0 on success, first negative close error otherwise.
 */
int feature_extractor_vector_close(RegisteredFeatureExtractors *rfe);

/**
 * @brief Destroy all closed contexts and free the backing array.
 *
 * This is the commit phase paired with feature_extractor_vector_close(). It
 * returns -EBUSY without releasing the vector when any context still owns
 * close-required state.
 *
 * @param rfe  Vector to destroy. Safe to call on a zero-initialised struct.
 * @return 0 on success, negative errno on failure.
 */
int feature_extractor_vector_destroy(RegisteredFeatureExtractors *rfe);

#ifdef __cplusplus
}
#endif

#endif /* __VMAF_SRC_FEX_CTX_VECTOR_H__ */
