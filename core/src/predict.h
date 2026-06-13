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

#ifndef __VMAF_PREDICT_H__
#define __VMAF_PREDICT_H__

#include "feature/feature_collector.h"
#include "model.h"

/**
 * @brief Run the SVM model for a single frame and optionally store the result.
 *
 * @param model               Model to evaluate.
 * @param feature_collector   Per-frame feature store populated by extractors.
 * @param index               Zero-based frame index to predict.
 * @param[out] vmaf_score     Receives the raw VMAF score for this frame.
 * @param write_prediction    If true, write the score back into @p feature_collector.
 * @param propagate_metadata  If true, fire registered metadata callbacks.
 * @param flags               Combination of VmafModelFlags (e.g. clip / transform).
 * @return 0 on success, negative errno on failure.
 */
int vmaf_predict_score_at_index(VmafModel *model, VmafFeatureCollector *feature_collector,
                                unsigned index, double *vmaf_score, bool write_prediction,
                                bool propagate_metadata, enum VmafModelFlags flags);

/**
 * @brief Run all models in a collection for a single frame.
 *
 * Iterates over every VmafModel in @p model_collection and populates the
 * per-model score fields inside @p score.
 *
 * @param model_collection  Collection of models to evaluate.
 * @param feature_collector Per-frame feature store.
 * @param index             Zero-based frame index to predict.
 * @param[out] score        Receives scores for each model in the collection.
 * @return 0 on success, negative errno on failure.
 */
int vmaf_predict_score_at_index_model_collection(VmafModelCollection *model_collection,
                                                 VmafFeatureCollector *feature_collector,
                                                 unsigned index, VmafModelCollectionScore *score);

#endif /* __VMAF_PREDICT_H__ */
