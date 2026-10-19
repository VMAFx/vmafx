/* SPDX-License-Identifier: BSD-2-Clause-Patent */
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

#ifndef VMAF_SRC_PREDICT_H_
#define VMAF_SRC_PREDICT_H_

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

/**
 * @brief Whether every feature @p model reads is written at @p index.
 *
 * Reads the collector only (no prediction, no log line), so a caller can ask
 * before vmaf_predict_score_at_index() whether a prediction at @p index would
 * find its inputs, without waiting for work in flight (the window scores of
 * the VMAFx API, ADR-2074).
 *
 * @return 0 when all are written, -EAGAIN when one is not written yet or not
 *         in the collector yet, another negative errno on failure.
 */
int vmaf_predict_inputs_written(VmafModel *model, VmafFeatureCollector *feature_collector,
                                unsigned index);

struct svm_node;

/**
 * @brief Test entry for the chroma-from-luma feature correction.
 *
 * Runs the file-static post_process_feature_from_another() of predict.c on a
 * caller-built SVM node vector, so a unit test can check the correction
 * without a full prediction (Netflix/vmaf 314f14b22 includes predict.c into
 * its test instead; the fork links one predictor implementation, see
 * Research-2096).
 *
 * @param model                  Model whose feature slopes / intercepts apply.
 * @param node                   Normalised feature values, one per model feature.
 * @param correction_parameter   Correction strength.
 * @param value_to_be_corrected  Guided-feature value that triggers the correction.
 * @param guiding_feature_substr Substring naming the guiding feature.
 * @param guided_feature_substr  Substring naming the guided feature.
 * @return 0 on success (also when no correction applies), negative errno on failure.
 */
int vmaf_predict_post_process_feature_from_another_for_test(const VmafModel *model,
                                                            struct svm_node *node,
                                                            double correction_parameter,
                                                            double value_to_be_corrected,
                                                            const char *guiding_feature_substr,
                                                            const char *guided_feature_substr);

/**
 * @brief Entry points of the Rust predictor (ADR-1713, RC4 lane P).
 *
 * predict.c reaches the Rust predictor only through the table installed with
 * vmaf_predict_install_rust_ops(); vmaf_rust_predict_install()
 * (core/src/rust/shim/rust_predict.c, built only with
 * -Denable_rust_features=true) installs it at vmaf_init(). Code that test
 * binaries link without libvmaf (predict.c, model.c) never references a Rust
 * symbol. With no table installed the C predictor runs.
 */
struct VmafRustPredictOps {
    /** Build the predictor of @p model into @p handle; -ENOTSUP = a model it
     *  does not implement (the C predictor runs and a WARNING says so). */
    int (*create)(const VmafModel *model, void **handle);
    /** Score one frame from the raw feature scores in model order; @p nodes
     *  is scratch of n_features + 1 entries. */
    int (*predict)(const VmafModel *model, void *handle, const double *scores,
                   enum VmafModelFlags flags, struct svm_node *nodes, unsigned index,
                   double *prediction);
    /** Free a handle create() returned. */
    void (*destroy)(void *handle);
};

/**
 * @brief Install the Rust predictor's table; NULL uninstalls it.
 *
 * @param ops  Table with static storage duration, or NULL (C predictor).
 */
void vmaf_predict_install_rust_ops(const struct VmafRustPredictOps *ops);

/**
 * @brief Free the Rust predictor handle @p model holds, through the installed
 *        table. No-op when the model holds none or no table is installed.
 *
 * @param model  Model being destroyed; NULL is a no-op.
 */
void vmaf_rust_predict_destroy(VmafModel *model);

#endif /* VMAF_SRC_PREDICT_H_ */
