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

#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dict.h"
#include "feature/alias.h"
#include "feature/feature_collector.h"
#include "feature/feature_extractor.h"
#include "feature/feature_name.h"
#include "log.h"
#include "model.h"
#include "pooling_percentile.h"
#include "predict.h"
#include "svm.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is an
 * upstream-mirror file whose Netflix source spells the null pointer constant
 * `NULL` (every upstream sync would re-conflict against a keyword rewrite) and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

static int normalize(const VmafModel *model, double slope, double intercept, double *feature_score)
{
    switch (model->norm_type) {
    case (VMAF_MODEL_NORMALIZATION_TYPE_NONE):
        break;
    case (VMAF_MODEL_NORMALIZATION_TYPE_LINEAR_RESCALE):
        *feature_score = slope * (*feature_score) + intercept;
        break;
    default:
        return -EINVAL;
    }

    return 0;
}

static int denormalize_feature(const VmafModel *model, double slope, double intercept,
                               double *feature_score)
{
    switch (model->norm_type) {
    case (VMAF_MODEL_NORMALIZATION_TYPE_NONE):
        break;
    case (VMAF_MODEL_NORMALIZATION_TYPE_LINEAR_RESCALE):
        *feature_score = (*feature_score - intercept) / slope;
        break;
    default:
        return -EINVAL;
    }

    return 0;
}

static int denormalize(const VmafModel *model, double *prediction)
{
    switch (model->norm_type) {
    case (VMAF_MODEL_NORMALIZATION_TYPE_NONE):
        break;
    case (VMAF_MODEL_NORMALIZATION_TYPE_LINEAR_RESCALE):
        *prediction = (*prediction - model->intercept) / model->slope;
        break;
    default:
        return -EINVAL;
    }

    return 0;
}

static int find_linear_function_parameters(VmafPoint p1, VmafPoint p2, double *alpha, double *beta)
{

    if (!(p1.x <= p2.x && p1.y <= p2.y))
        return -EINVAL; // first_point coordinates need to be smaller or equal to second_point coordinates

    if (p2.x - p1.x == 0 || p2.y - p1.y == 0) {
        if (!(p1.x == p2.x && p1.y == p2.y))
            return -EINVAL; // first_point and second_point cannot lie on a horizontal or vertical line
        *alpha = 1.0;       // both points are the same
        *beta = 0.0;
    } else if (p1.x == 0) {
        *beta = p1.y;
        *alpha = (p2.y - *beta) / p2.x;
    } else {
        *alpha = (p2.y - p1.y) / (p2.x - p1.x);
        *beta = p1.y - (p1.x * (*alpha));
    }

    return 0;
}

static int piecewise_segment_apply(double x, VmafPoint *knots, unsigned idx, unsigned n_seg,
                                   double *y)
{
    /* Errno values are positive; libvmaf's convention is to return their
     * negation so callers can distinguish them from a successful 0.  Returning
     * positive EINVAL here surfaced as a truthy error to local callers but
     * inverted the sign on any caller that propagated `err` upward (e.g.
     * vmaf_predict_score_at_index downstream).  Adversarial audit 2026-05-31,
     * fix/core-lifecycle-memory-audit. */
    if (!(knots[idx].x < knots[idx + 1].x && knots[idx].y <= knots[idx + 1].y))
        return -EINVAL;

    const bool cond0 = knots[idx].x <= x;
    const bool cond1 = x <= knots[idx + 1].x;

    if (knots[idx].y == knots[idx + 1].y) { // the segment is horizontal
        if (cond0 && cond1)
            *y = knots[idx].y;
        if (idx == 0 && x < knots[idx].x)
            *y = knots[idx].y;
        if (idx == n_seg - 1 && x > knots[idx + 1].x)
            *y = knots[idx].y;
        return 0;
    }

    double slope = 0.0;
    double offset = 0.0;
    /* Unreachable failure for a well-ordered, non-horizontal segment (the
     * guard above already enforces x strictly increasing and y
     * non-decreasing), but propagate it rather than silently mapping onto
     * the zero line. CERT ERR33-C / Power-of-10 rule 7. */
    const int err = find_linear_function_parameters(knots[idx], knots[idx + 1], &slope, &offset);
    if (err)
        return err;

    if (cond0 && cond1)
        *y = slope * x + offset;
    if (idx == 0 && x < knots[idx].x)
        *y = slope * x + offset;
    if (idx == n_seg - 1 && x > knots[idx + 1].x)
        *y = slope * x + offset;
    return 0;
}

static int piecewise_linear_mapping(double x, VmafPoint *knots, unsigned n_knots, double *y)
{
    /* See piecewise_segment_apply: -EINVAL not +EINVAL. */
    if (n_knots <= 1)
        return -EINVAL;
    unsigned n_seg = n_knots - 1;

    *y = 0.0;

    // construct the function
    for (unsigned idx = 0; idx < n_seg; idx++) {
        int err = piecewise_segment_apply(x, knots, idx, n_seg, y);
        if (err)
            return err;
    }

    return 0;
}

/*  Reproducing the logic in quality_runner.VmafQualityRunner.transform_score().
    Transform final quality score in the following optional steps (in this
    order):
    1) polynomial mapping. e.g. {'p0': 1, 'p1': 1, 'p2': 0.5} means
    transform through 1 + x + 0.5 * x^2. For now, only support polynomail
    up to 2nd-order.
    2) piecewise-linear mapping, where the change points are defined in
    'knots', in the form of [[x0, y0], [x1, y1], ...].
    3) rectification, supporting 'out_lte_in' (output is less than or equal
    to input) and 'out_gte_in' (output is greater than or equal to input).
 */
static int transform(const VmafModel *model, double *y_in, enum VmafModelFlags flags)
{
    if (!model->score_transform.enabled)
        return 0;
    if (flags & VMAF_MODEL_FLAG_DISABLE_TRANSFORM)
        return 0;

    double y_stage;
    double y_out;

    // polynomial mapping
    y_stage = *y_in;
    if (model->score_transform.p0.enabled || model->score_transform.p1.enabled ||
        model->score_transform.p2.enabled) {
        y_out = 0.;
        if (model->score_transform.p0.enabled)
            y_out += model->score_transform.p0.value;
        if (model->score_transform.p1.enabled)
            y_out += model->score_transform.p1.value * y_stage;
        if (model->score_transform.p2.enabled)
            y_out += model->score_transform.p2.value * y_stage * y_stage;
    } else {
        y_out = y_stage;
    }

    // piecewise-linear mapping
    y_stage = y_out;
    if (model->score_transform.knots.enabled) {
        /* Propagate error rather than silently overwriting y_in with 0 (the
         * out-param defaults to 0.0 on the early-error path inside
         * piecewise_linear_mapping).  Adversarial audit 2026-05-31. */
        const int err = piecewise_linear_mapping(y_stage, model->score_transform.knots.list,
                                                 model->score_transform.knots.n_knots, &y_out);
        if (err)
            return err;
    }

    // rectification
    if (model->score_transform.out_lte_in)
        y_out = (y_out > *y_in) ? *y_in : y_out;
    if (model->score_transform.out_gte_in)
        y_out = (y_out < *y_in) ? *y_in : y_out;

    *y_in = y_out;
    return 0;
}

/* Clamp `prediction` into the model's score_clip range. Cannot fail: a
 * disabled clip (by model or by flag) is a no-op. */
static void clip(const VmafModel *model, double *prediction, enum VmafModelFlags flags)
{
    if (!model->score_clip.enabled)
        return;
    if (flags & VMAF_MODEL_FLAG_DISABLE_CLIP)
        return;

    *prediction = (*prediction < model->score_clip.min) ? model->score_clip.min : *prediction;
    *prediction = (*prediction > model->score_clip.max) ? model->score_clip.max : *prediction;
}

/* Scan state for post_process_feature_from_another(): the *guiding* feature
 * drives the correction, the *guided* feature receives it. */
typedef struct GuidedFeatureScan {
    const char *guiding_substr;
    const char *guided_substr;
    double sentinel; /* guided-feature value that triggers the correction */
    double guiding_score;
    double guided_score;
    bool found_guiding;
    bool found_guided;
    unsigned guided_idx;
} GuidedFeatureScan;

/* Record feature `i` as the match for `substr`, denormalising its SVM node
 * value into *score. A second match for the same substring means the model
 * is ambiguous and is rejected with -EINVAL. */
static int scan_match_feature(const VmafModel *model, const struct svm_node *node, unsigned i,
                              const char *substr, bool *found, double *score)
{
    if (*found) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "post_process_feature_from_another(): Substring '%s' "
                 "corresponds to more than one feature\n",
                 substr);
        return -EINVAL;
    }
    *score = node[i].value;
    *found = true;
    const int err =
        denormalize_feature(model, model->feature[i].slope, model->feature[i].intercept, score);
    return err ? -EINVAL : 0;
}

/* One step of the feature scan. Returns a negative errno on error, 1 when the
 * guided feature does not carry the sentinel (no correction is needed and the
 * caller returns 0 with `node` untouched), and 0 to keep scanning. */
static int scan_feature(const VmafModel *model, const struct svm_node *node, unsigned i,
                        GuidedFeatureScan *st)
{
    const char *name = model->feature[i].name;
    if (strstr(name, st->guiding_substr) != NULL) {
        const int err = scan_match_feature(model, node, i, st->guiding_substr, &st->found_guiding,
                                           &st->guiding_score);
        if (err)
            return err;
    }
    if (strstr(name, st->guided_substr) != NULL) {
        const int err = scan_match_feature(model, node, i, st->guided_substr, &st->found_guided,
                                           &st->guided_score);
        if (err)
            return err;
        /* Exact sentinel comparison: caller always passes value_to_be_corrected=0.0
         * (see vmaf_predict_score_at_index). A normalised feature that is exactly
         * zero is the only case that needs chroma correction; any other value
         * exits early. An epsilon band here would incorrectly correct near-zero
         * but non-zero features and change scores. */
        if (st->guided_score != st->sentinel) /* sentinel, not computed equality */
            return 1;
        st->guided_idx = i;
    }
    return 0;
}

static int post_process_feature_from_another(const VmafModel *model, struct svm_node *node,
                                             double correction_parameter,
                                             double value_to_be_corrected,
                                             const char *guiding_feature_substr,
                                             const char *guided_feature_substr)
{
    GuidedFeatureScan st = {
        .guiding_substr = guiding_feature_substr,
        .guided_substr = guided_feature_substr,
        .sentinel = value_to_be_corrected,
    };

    for (unsigned i = 0; i < model->n_features; i++) {
        const int r = scan_feature(model, node, i, &st);
        if (r < 0)
            return r;
        if (r > 0)
            return 0;
    }

    if (!st.found_guiding || !st.found_guided)
        return 0;

    double corrected_guided_score =
        (-correction_parameter * st.guiding_score) + correction_parameter;
    const int err = normalize(model, model->feature[st.guided_idx].slope,
                              model->feature[st.guided_idx].intercept, &corrected_guided_score);
    if (err)
        return -EINVAL;
    node[st.guided_idx].value = corrected_guided_score;
    return 0;
}

static int predict_resolve_feature_name(VmafModel *model, unsigned i)
{
    VmafFeatureExtractor *fex =
        vmaf_get_feature_extractor_by_feature_name(model->feature[i].name, 0);
    if (!fex) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "vmaf_predict_score_at_index(): no feature extractor "
                 "providing feature '%s'\n",
                 model->feature[i].name);
        return -EINVAL;
    }

    VmafDictionary *opts_dict = NULL;
    if (model->feature[i].opts_dict) {
        int err = vmaf_dictionary_copy(&model->feature[i].opts_dict, &opts_dict);
        if (err)
            return err;
    }

    VmafFeatureExtractorContext *fex_ctx = NULL;
    int err = vmaf_feature_extractor_context_create(&fex_ctx, fex, opts_dict);
    if (err) {
        (void)vmaf_dictionary_free(&opts_dict);
        return err;
    }

    model->predict_feature_names[i] = vmaf_feature_name_from_options(
        model->feature[i].name, fex_ctx->fex->options, fex_ctx->fex->priv);
    (void)vmaf_feature_extractor_context_destroy(fex_ctx);

    if (!model->predict_feature_names[i])
        return -ENOMEM;
    return 0;
}

/* Helpers called under model->predict_cache_lock — no lock operations inside. */

static int predict_init_feature_names(VmafModel *model)
{
    assert(model->n_features > 0);
    char **names = (char **)calloc(model->n_features, sizeof(*names));
    if (!names)
        return -ENOMEM;
    model->predict_feature_names = names;
    for (unsigned i = 0; i < model->n_features; i++) {
        const int err = predict_resolve_feature_name(model, i);
        if (err) {
            /* Roll back partial table so next call can retry from scratch.
             * Without rollback, a non-NULL pointer with NULL holes bypasses the
             * re-init branch and later dereferences NULL. Adversarial audit
             * 2026-05-31. */
            for (unsigned k = 0; k < i; k++)
                free(model->predict_feature_names[k]);
            free((void *)model->predict_feature_names);
            model->predict_feature_names = NULL;
            return err;
        }
    }
    return 0;
}

static int predict_init_feature_vectors(VmafModel *model, VmafFeatureCollector *fc)
{
    assert(model->n_features > 0);
    model->predict_feature_vectors = (void **)calloc(model->n_features, sizeof(void *));
    if (!model->predict_feature_vectors)
        return -ENOMEM;
    for (unsigned i = 0; i < model->n_features; i++) {
        model->predict_feature_vectors[i] =
            vmaf_feature_collector_find(fc, model->predict_feature_names[i]);
    }
    return 0;
}

/* Round-5 race fix (finding #3): serialise the three lazy-init blocks with
 * model->predict_cache_lock (initialised in vmaf_read_json_model()).  The lock
 * is held only during the one-time init, not during SVM inference. */
static int predict_ensure_caches(VmafModel *model, VmafFeatureCollector *feature_collector)
{
    pthread_mutex_lock(&model->predict_cache_lock);
    int err = 0;

    if (!model->predict_feature_names) {
        err = predict_init_feature_names(model);
        if (err)
            goto unlock;
    }

    if (!model->predict_nodes) {
        model->predict_nodes = malloc(sizeof(struct svm_node) * (model->n_features + 1));
        if (!model->predict_nodes) {
            err = -ENOMEM;
            goto unlock;
        }
    }

    if (!model->predict_feature_vectors)
        err = predict_init_feature_vectors(model, feature_collector);

unlock:
    pthread_mutex_unlock(&model->predict_cache_lock);
    return err;
}

static int predict_load_feature_score(VmafModel *model, VmafFeatureCollector *feature_collector,
                                      unsigned i, unsigned index, bool propagate_metadata,
                                      double *feature_score)
{
    /* Round-5 race fix (finding #4): this function is called from
     * predict_build_svm_nodes -> vmaf_predict_score_at_index, which runs
     * outside the feature_collector lock.  The previous direct calls to the
     * unlocked vmaf_feature_vector_get_score inline raced with concurrent
     * vmaf_feature_collector_append writes to .written / .value.
     *
     * Fix: use vmaf_feature_collector_get_score for all score reads here.
     * It acquires the collector lock internally, so reads are always
     * protected.  The cached FeatureVector* pointer in predict_feature_vectors
     * is still used as a hint via vmaf_feature_collector_find on first miss,
     * but score reads always go through the lock-safe public API.
     *
     * Netflix#755 / ADR-0154 semantics preserved: -EAGAIN is returned for a
     * valid feature whose score has not yet been written (retroactive-write
     * extractors), which is distinct from -EINVAL (feature not found at all). */
    int err = vmaf_feature_collector_get_score(feature_collector, model->predict_feature_names[i],
                                               feature_score, index);

    if (err == -EINVAL && !model->predict_feature_vectors[i]) {
        /* Feature vector may not be registered yet — transient absence for
         * retroactive-write extractors.  Probe whether it now exists so the
         * pointer cache can be populated for future calls. */
        FeatureVector *found =
            vmaf_feature_collector_find(feature_collector, model->predict_feature_names[i]);
        if (found) {
            model->predict_feature_vectors[i] = found;
            /* Re-try through the lock-safe path. */
            err = vmaf_feature_collector_get_score(
                feature_collector, model->predict_feature_names[i], feature_score, index);
        } else {
            err = -EAGAIN;
        }
    }

    if (err && err != -EAGAIN) {
        if (!propagate_metadata) {
            vmaf_log(VMAF_LOG_LEVEL_ERROR,
                     "vmaf_predict_score_at_index(): no feature '%s' "
                     "at index %d\n",
                     model->predict_feature_names[i], index);
        }
    }
    return err;
}

static int predict_build_svm_nodes(VmafModel *model, VmafFeatureCollector *feature_collector,
                                   unsigned index, bool propagate_metadata)
{
    struct svm_node *node = model->predict_nodes;
    for (unsigned i = 0; i < model->n_features; i++) {
        double feature_score;
        int err = predict_load_feature_score(model, feature_collector, i, index, propagate_metadata,
                                             &feature_score);
        if (err)
            return err;

        err =
            normalize(model, model->feature[i].slope, model->feature[i].intercept, &feature_score);
        if (err)
            return err;

        node[i].index = i + 1;
        node[i].value = feature_score;
    }
    if (model->chroma_from_luma.enabled && model->chroma_from_luma.chroma_correction_parameter) {
        int err = post_process_feature_from_another(
            model, node, model->chroma_from_luma.chroma_correction_parameter, 0.0, "adm3",
            "speed_chroma");
        if (err)
            return err;
    }

    node[model->n_features].index = -1;
    return 0;
}

int vmaf_predict_score_at_index(VmafModel *model, VmafFeatureCollector *feature_collector,
                                unsigned index, double *vmaf_score, bool write_prediction,
                                bool propagate_metadata, enum VmafModelFlags flags)
{
    if (!model)
        return -EINVAL;
    if (!feature_collector)
        return -EINVAL;
    if (!vmaf_score)
        return -EINVAL;

    int err = predict_ensure_caches(model, feature_collector);
    if (err)
        return err;

    err = predict_build_svm_nodes(model, feature_collector, index, propagate_metadata);
    if (err)
        return err;

    double prediction = svm_predict(model->svm, model->predict_nodes);

    err = denormalize(model, &prediction);
    if (err)
        return err;

    err = transform(model, &prediction, flags);
    if (err)
        return err;

    clip(model, &prediction, flags);

    if (write_prediction) {
        err = vmaf_feature_collector_append(feature_collector, model->name, prediction, index);
        if (err)
            return err;
    }

    *vmaf_score = prediction;

    return 0;
}

static int bootstrap_gather_scores(VmafModelCollection *model_collection,
                                   VmafFeatureCollector *feature_collector, unsigned index,
                                   double *scores)
{
    for (unsigned i = 0; i < model_collection->cnt; i++) {
        // mean, stddev, etc. are calculated on untransformed/unclipped scores
        // gather the unclipped scores, for the purposes of these calculations
        // but do not write them to the feature collector
        const unsigned flags = VMAF_MODEL_FLAG_DISABLE_CLIP | VMAF_MODEL_FLAG_DISABLE_TRANSFORM;
        /* Bitmask of VmafModelFlags values is not itself a named enumerator;
         * the cast is intentional and the value space is closed by the union
         * of DISABLE_CLIP | DISABLE_TRANSFORM here. Per ADR-0278 cite-form. */
        // NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange) — ADR-0278
        int err = vmaf_predict_score_at_index(model_collection->model[i], feature_collector, index,
                                              &scores[i], false, false, flags);
        // NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange)
        if (err)
            return err;

        // do not override the model's transform/clip behavior
        // write the scores to the feature collector
        double score;
        err = vmaf_predict_score_at_index(model_collection->model[i], feature_collector, index,
                                          &score, true, false, 0);
        if (err)
            return err;
    }
    return 0;
}

static void bootstrap_compute_statistics(const VmafModelCollection *model_collection,
                                         double *scores, VmafModelCollectionScore *score,
                                         double *score_plus_delta, double *score_minus_delta)
{
    double sum = 0.;
    for (unsigned i = 0; i < model_collection->cnt; i++) {
        sum += scores[i];
    }
    const double mean = sum / model_collection->cnt;
    score->bootstrap.bagging_score = mean;

    const double delta = 0.01;
    *score_plus_delta = mean + delta;
    *score_minus_delta = mean - delta;

    double ssd = 0.;
    for (unsigned i = 0; i < model_collection->cnt; i++) {
        ssd += pow(scores[i] - mean, 2);
    }
    score->bootstrap.stddev = sqrt(ssd / model_collection->cnt);

    qsort(scores, model_collection->cnt, sizeof(double), score_compare);
    score->bootstrap.ci.p95.lo = percentile(scores, model_collection->cnt, 2.5);
    score->bootstrap.ci.p95.hi = percentile(scores, model_collection->cnt, 97.5);
}

/* Apply the model's score transform, then its clip, to one value. Propagates
 * the first failure (a malformed piecewise-linear knot list) instead of
 * discarding it. CERT ERR33-C / Power-of-10 rule 7. */
static int transform_and_clip(const VmafModel *model, double *value)
{
    const int err = transform(model, value, 0);
    if (err)
        return err;
    clip(model, value, 0);
    return 0;
}

/* Transform-then-clip the bagging score, both CI bounds and the two
 * finite-difference probes in the original upstream order; the first
 * failure short-circuits the rest. */
static int bootstrap_transform_and_clip(const VmafModel *model, VmafModelCollectionScore *score,
                                        double *score_plus_delta, double *score_minus_delta)
{
    int err = transform_and_clip(model, &score->bootstrap.bagging_score);
    if (!err)
        err = transform_and_clip(model, &score->bootstrap.ci.p95.lo);
    if (!err)
        err = transform_and_clip(model, &score->bootstrap.ci.p95.hi);
    if (!err)
        err = transform_and_clip(model, score_plus_delta);
    if (!err)
        err = transform_and_clip(model, score_minus_delta);
    return err;
}

static int bootstrap_append_named_scores(const VmafModelCollection *model_collection,
                                         VmafFeatureCollector *feature_collector, unsigned index,
                                         const VmafModelCollectionScore *score)
{
    const char *suffix_lo = "_ci_p95_lo";
    const char *suffix_hi = "_ci_p95_hi";
    const char *suffix_bagging = "_bagging";
    const char *suffix_stddev = "_stddev";
    const size_t name_sz = strlen(model_collection->name) + strlen(suffix_lo) + 1;
    /* Heap-allocated for MSVC portability (no VLAs). */
    char *name = (char *)calloc(1u, name_sz);
    if (!name)
        return -ENOMEM;

    int err = 0;
    (void)snprintf(name, name_sz, "%s%s", model_collection->name, suffix_bagging);
    err |= vmaf_feature_collector_append(feature_collector, name, score->bootstrap.bagging_score,
                                         index);

    (void)snprintf(name, name_sz, "%s%s", model_collection->name, suffix_stddev);
    err |= vmaf_feature_collector_append(feature_collector, name, score->bootstrap.stddev, index);

    (void)snprintf(name, name_sz, "%s%s", model_collection->name, suffix_lo);
    err |=
        vmaf_feature_collector_append(feature_collector, name, score->bootstrap.ci.p95.lo, index);

    (void)snprintf(name, name_sz, "%s%s", model_collection->name, suffix_hi);
    err |=
        vmaf_feature_collector_append(feature_collector, name, score->bootstrap.ci.p95.hi, index);

    free(name);
    return err;
}

static int vmaf_bootstrap_predict_score_at_index(VmafModelCollection *model_collection,
                                                 VmafFeatureCollector *feature_collector,
                                                 unsigned index, VmafModelCollectionScore *score)
{
    int err = 0;
    /* MSVC rejects VLAs; heap-allocate the scratch array so both
     * compilers accept it. Size is typically the bootstrap count (~20),
     * so the allocation is trivially small. */
    double *scores = (double *)malloc(sizeof(double) * model_collection->cnt);
    if (!scores)
        return -ENOMEM;

    err = bootstrap_gather_scores(model_collection, feature_collector, index, scores);
    if (err)
        goto out;

    score->type = VMAF_MODEL_COLLECTION_SCORE_BOOTSTRAP;

    double score_plus_delta;
    double score_minus_delta;
    bootstrap_compute_statistics(model_collection, scores, score, &score_plus_delta,
                                 &score_minus_delta);

    const VmafModel *model = model_collection->model[0];
    err = bootstrap_transform_and_clip(model, score, &score_plus_delta, &score_minus_delta);
    if (err)
        goto out;

    const double delta = 0.01;
    const double slope = (score_plus_delta - score_minus_delta) / (2.0 * delta);
    score->bootstrap.stddev *= slope;

    err = bootstrap_append_named_scores(model_collection, feature_collector, index, score);

out:
    free(scores);
    return err;
}

int vmaf_predict_score_at_index_model_collection(VmafModelCollection *model_collection,
                                                 VmafFeatureCollector *feature_collector,
                                                 unsigned index, VmafModelCollectionScore *score)
{
    if (!model_collection)
        return -EINVAL;
    if (!feature_collector)
        return -EINVAL;
    if (!score)
        return -EINVAL;

    switch (model_collection->type) {
    case VMAF_MODEL_BOOTSTRAP_SVM_NUSVR:
    case VMAF_MODEL_RESIDUE_BOOTSTRAP_SVM_NUSVR:
        return vmaf_bootstrap_predict_score_at_index(model_collection, feature_collector, index,
                                                     score);
    default:
        return -EINVAL;
    }
}

/* NOLINTEND(modernize-use-nullptr) */
