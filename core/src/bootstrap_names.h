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

#ifndef VMAF_SRC_BOOTSTRAP_NAMES_H_
#define VMAF_SRC_BOOTSTRAP_NAMES_H_

/**
 * @file bootstrap_names.h
 * @brief Shared suffix constants and name-buffer sizing for bootstrap
 *        model-collection score labels.
 *
 * Both predict.c (bootstrap_append_named_scores) and libvmaf.c
 * (vmaf_score_pooled_model_collection) need to construct four score-name
 * strings of the form "<collection_name><suffix>".  The suffix set and the
 * buffer-size formula were previously duplicated verbatim in both files.
 * Extracted here to close the two TODO-dedupe markers per ADR-0480.
 *
 * Usage:
 *
 *   size_t name_sz = BOOTSTRAP_NAME_BUF_SZ(model_collection->name);
 *   char  *name    = calloc(1u, name_sz);
 *   if (!name) return -ENOMEM;
 *
 *   snprintf(name, name_sz, "%s%s", model_collection->name,
 *            BOOTSTRAP_SUFFIX_BAGGING);
 *   // ... append / pool that name ...
 *
 *   snprintf(name, name_sz, "%s%s", model_collection->name,
 *            BOOTSTRAP_SUFFIX_STDDEV);
 *   // ...
 *
 *   free(name);
 */

#include <string.h>

/** Score-name suffixes appended to the collection name. */
#define BOOTSTRAP_SUFFIX_BAGGING "_bagging"
#define BOOTSTRAP_SUFFIX_STDDEV "_stddev"
#define BOOTSTRAP_SUFFIX_CI_LO "_ci_p95_lo"
#define BOOTSTRAP_SUFFIX_CI_HI "_ci_p95_hi"

/**
 * Compute the allocation size for a score-name buffer.
 *
 * The longest suffix is "_ci_p95_lo" (10 bytes).  The buffer must hold
 * strlen(collection_name) + strlen(longest_suffix) + 1 (NUL terminator).
 *
 * @param collection_name  C string containing the model-collection name.
 * @return                 Number of bytes to allocate (includes NUL).
 */
#define BOOTSTRAP_NAME_BUF_SZ(collection_name)                                                     \
    (strlen(collection_name) + sizeof(BOOTSTRAP_SUFFIX_CI_LO))

#endif /* VMAF_SRC_BOOTSTRAP_NAMES_H_ */
