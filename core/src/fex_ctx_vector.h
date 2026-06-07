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

#include "feature/feature_extractor.h"

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
 * @brief Destroy all contexts in the vector and free the backing array.
 *
 * @param rfe  Vector to destroy.  Safe to call on a zero-initialised struct.
 */
void feature_extractor_vector_destroy(RegisteredFeatureExtractors *rfe);

#endif /* __VMAF_SRC_FEX_CTX_VECTOR_H__ */
