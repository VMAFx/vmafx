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

/* feature_collector_internal.h
 *
 * Internal helpers extracted from feature_collector.cpp so that both the
 * implementation TU and the C unit-test (test_feature_collector.c) can reach
 * aggregate_vector_* / feature_vector_* without #including the whole .cpp.
 *
 * The six functions are declared here and defined in feature_collector.cpp.
 * The extern "C" wrapper lets C test code include this header directly.
 */

#ifndef VMAF_FEATURE_COLLECTOR_INTERNAL_INCLUDED
#define VMAF_FEATURE_COLLECTOR_INTERNAL_INCLUDED

#include "feature_collector.h"

#ifdef __cplusplus
extern "C" {
#endif

int aggregate_vector_init(AggregateVector *aggregate_vector);
int aggregate_vector_append(AggregateVector *aggregate_vector, const char *feature_name,
                            double score);
void aggregate_vector_destroy(AggregateVector *aggregate_vector);

int feature_vector_init(FeatureVector **feature_vector, const char *name);
int feature_vector_append(FeatureVector *feature_vector, unsigned index, double score);
void feature_vector_destroy(FeatureVector *feature_vector);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* VMAF_FEATURE_COLLECTOR_INTERNAL_INCLUDED */
