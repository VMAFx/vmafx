/**
 * Copyright 2026 Lusoris
 * SPDX-License-Identifier: EUPL-1.2
 */

#ifndef VMAF_FEATURE_NONFINITE_SCORE_H
#define VMAF_FEATURE_NONFINITE_SCORE_H

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>

#include "adm_score.h"
#include "dict.h"
#include "feature_collector.h"
#include "log.h"

#ifndef __cplusplus
typedef struct VmafNamedScore VmafNamedScore;
#endif
struct VmafNamedScore {
    const char *name;
    double value;
};

#ifdef __cplusplus
enum VmafVifNameSet : unsigned char {
#else
typedef enum VmafVifNameSet {
#endif
    VMAF_VIF_FLOAT_NAMES,
    VMAF_VIF_INTEGER_NAMES,
#ifdef __cplusplus
};
#else
} VmafVifNameSet;
#endif

#ifndef __cplusplus
typedef struct VmafVifScoreSet VmafVifScoreSet;
#endif
struct VmafVifScoreSet {
    double scale[8];
    double score;
    double score_num;
    double score_den;
    double minimum[3];
    bool use_minimums;
    bool single_precision_ratio;
    bool skip_scale0;
    bool debug;
};

static inline int vmaf_feature_emit_finite_scores(VmafFeatureCollector *feature_collector,
                                                  VmafDictionary *feature_name_dict,
                                                  const char *extractor,
                                                  const VmafNamedScore *scores, size_t score_count,
                                                  unsigned index);

static inline int vmaf_feature_validate_finite_scores(const VmafNamedScore *scores,
                                                      size_t score_count)
{
    if (!scores || score_count == 0u)
        return -EINVAL;

    for (size_t i = 0; i < score_count; ++i) {
        if (!scores[i].name || !isfinite(scores[i].value))
            return -EINVAL;
    }
    return 0;
}

static inline int vmaf_feature_validate_finite_scores_named(const char *extractor,
                                                            const VmafNamedScore *scores,
                                                            size_t score_count, unsigned index)
{
    const int err = vmaf_feature_validate_finite_scores(scores, score_count);
    if (!err)
        return 0;
    if (!extractor || !scores || score_count == 0u) {
        vmaf_log(VMAF_LOG_LEVEL_WARNING,
                 "metric: invalid score set at frame %u (extractor=%s count=%zu)\n", index,
                 extractor ? extractor : "<null>", score_count);
        return err;
    }
    for (size_t i = 0u; i < score_count; ++i) {
        if (!scores[i].name || !isfinite(scores[i].value)) {
            vmaf_log(VMAF_LOG_LEVEL_WARNING,
                     "%s: non-finite score at frame %u (feature=%s value=%g)\n", extractor, index,
                     scores[i].name ? scores[i].name : "<null>", scores[i].value);
            break;
        }
    }
    return err;
}

static inline int vmaf_adm_finalize_scores_named(const char *extractor, unsigned index, double num,
                                                 double den, double aim_num, double aim_den,
                                                 double *score, double *score_aim)
{
    const int err = vmaf_adm_finalize_scores(num, den, aim_num, aim_den, score, score_aim);
    if (err) {
        vmaf_log(VMAF_LOG_LEVEL_WARNING,
                 "%s: undefined or non-finite ADM score at frame %u "
                 "(num=%g den=%g aim_num=%g aim_den=%g)\n",
                 extractor ? extractor : "adm", index, num, den, aim_num, aim_den);
    }
    return err;
}

static inline int vmaf_adm_floor_pair_named(const char *extractor, unsigned index, double num,
                                            double den, double limit, double *floored_num,
                                            double *floored_den)
{
    const int err = vmaf_adm_floor_pair(num, den, limit, floored_num, floored_den);
    if (err) {
        vmaf_log(VMAF_LOG_LEVEL_WARNING,
                 "%s: invalid ADM reduction before floor at frame %u "
                 "(num=%g den=%g limit=%g)\n",
                 extractor ? extractor : "adm", index, num, den, limit);
    }
    return err;
}

static inline int vmaf_adm3_score_named(const char *extractor, unsigned index, double score,
                                        double score_aim, int apply_harmonic_mean,
                                        double dlm_weight, double min_value, double *adm3)
{
    const int err =
        vmaf_adm3_score(score, score_aim, apply_harmonic_mean, dlm_weight, min_value, adm3);
    if (err) {
        vmaf_log(VMAF_LOG_LEVEL_WARNING,
                 "%s: non-finite ADM3 score at frame %u "
                 "(adm=%g aim=%g weight=%g min=%g)\n",
                 extractor ? extractor : "adm", index, score, score_aim, dlm_weight, min_value);
    }
    return err;
}

static inline int vmaf_adm_scale_ratios_named(const char *extractor, unsigned index,
                                              const double *scores, size_t scale_count,
                                              double *ratios)
{
    const int err = vmaf_adm_scale_ratios(scores, scale_count, ratios);
    if (!err)
        return 0;
    if (!scores || scale_count == 0u) {
        vmaf_log(VMAF_LOG_LEVEL_WARNING, "%s: invalid ADM scale set at frame %u (count=%zu)\n",
                 extractor ? extractor : "adm", index, scale_count);
        return err;
    }
    for (size_t scale = 0u; scale < scale_count; ++scale) {
        const double num = scores[scale * 2u];
        const double den = scores[scale * 2u + 1u];
        const double ratio = den == 0.0 ? 0.0 : num / den;
        if (!isfinite(num) || !isfinite(den) || (den == 0.0 && num != 0.0) || !isfinite(ratio)) {
            vmaf_log(VMAF_LOG_LEVEL_WARNING,
                     "%s: undefined or non-finite ADM scale at frame %u "
                     "(scale=%zu num=%g den=%g)\n",
                     extractor ? extractor : "adm", index, scale, num, den);
            break;
        }
    }
    return err;
}

static inline int vmaf_feature_finite_ratio(double num, double den, double *ratio)
{
    if (!ratio || !isfinite(num) || !isfinite(den) || den <= 0.0)
        return -EINVAL;
    const double value = num / den;
    if (!isfinite(value))
        return -EINVAL;
    *ratio = value;
    return 0;
}

static inline int vmaf_feature_finite_ratio_named(const char *extractor, const char *feature,
                                                  double num, double den, unsigned index,
                                                  double *ratio)
{
    const int err = vmaf_feature_finite_ratio(num, den, ratio);
    if (err) {
        vmaf_log(VMAF_LOG_LEVEL_WARNING,
                 "%s: invalid ratio at frame %u "
                 "(feature=%s numerator=%g denominator=%g)\n",
                 extractor ? extractor : "metric", index, feature ? feature : "<null>", num, den);
    }
    return err;
}

static inline int vmaf_vif_prepare_scale_scores(const char *extractor,
                                                const VmafVifScoreSet *scores,
                                                const char *const scale_names[4],
                                                VmafNamedScore values[15], unsigned index)
{
    for (size_t scale = 0u; scale < 4u; ++scale) {
        if (scale == 0u && scores->skip_scale0) {
            values[scale].name = scale_names[scale];
            values[scale].value = 0.0;
            continue;
        }
        const double num = scores->scale[scale * 2u];
        const double den = scores->scale[scale * 2u + 1u];
        double ratio = 0.0;
        if (vmaf_feature_finite_ratio_named(extractor, scale_names[scale], num, den, index,
                                            &ratio)) {
            return -EINVAL;
        }
        if (scores->single_precision_ratio) {
            ratio = (double)((float)num / (float)den);
            if (!isfinite(ratio)) {
                vmaf_log(VMAF_LOG_LEVEL_WARNING,
                         "%s: non-finite single-precision ratio at frame %u "
                         "(feature=%s numerator=%g denominator=%g value=%g)\n",
                         extractor ? extractor : "vif", index, scale_names[scale], num, den, ratio);
                return -EINVAL;
            }
        }
        if (scale > 0u && scores->use_minimums) {
            const double minimum = scores->minimum[scale - 1u];
            if (!isfinite(minimum)) {
                vmaf_log(VMAF_LOG_LEVEL_WARNING,
                         "%s: non-finite VIF minimum at frame %u "
                         "(scale=%zu value=%g)\n",
                         extractor ? extractor : "vif", index, scale, minimum);
                return -EINVAL;
            }
            if (ratio < minimum)
                ratio = minimum;
        }
        values[scale].name = scale_names[scale];
        values[scale].value = ratio;
    }
    return 0;
}

static inline int vmaf_vif_prepare_debug_scores(const char *extractor,
                                                const VmafVifScoreSet *scores,
                                                const char *const debug_names[11],
                                                VmafNamedScore values[15], unsigned index)
{
    const VmafNamedScore aggregate[] = {
        {.name = debug_names[0], .value = scores->score},
        {.name = debug_names[1], .value = scores->score_num},
        {.name = debug_names[2], .value = scores->score_den},
    };
    if (scores->score_den <= 0.0 ||
        vmaf_feature_validate_finite_scores_named(extractor, aggregate, 3u, index)) {
        vmaf_log(VMAF_LOG_LEVEL_WARNING,
                 "%s: invalid VIF aggregate at frame %u (score=%g num=%g den=%g)\n",
                 extractor ? extractor : "vif", index, scores->score, scores->score_num,
                 scores->score_den);
        return -EINVAL;
    }
    values[4] = aggregate[0];
    values[5] = aggregate[1];
    values[6] = aggregate[2];
    for (size_t scale = 0u; scale < 4u; ++scale) {
        const bool skipped = scale == 0u && scores->skip_scale0;
        values[7u + scale * 2u].name = debug_names[3u + scale * 2u];
        values[7u + scale * 2u].value = skipped ? 0.0 : scores->scale[scale * 2u];
        values[8u + scale * 2u].name = debug_names[4u + scale * 2u];
        values[8u + scale * 2u].value = skipped ? -1.0 : scores->scale[scale * 2u + 1u];
    }
    return 0;
}

/**
 * Validate and publish the complete VIF result set through the same production
 * seam used by every CPU and GPU collector.  Scale ratios, aggregate debug
 * values, and raw debug atoms are all validated before the first append.
 */
static inline int vmaf_vif_emit_scores(VmafFeatureCollector *feature_collector,
                                       VmafDictionary *feature_name_dict, const char *extractor,
                                       const VmafVifScoreSet *scores, VmafVifNameSet name_set,
                                       unsigned index)
{
    static const char *const float_names[15] = {
        "VMAF_feature_vif_scale0_score",
        "VMAF_feature_vif_scale1_score",
        "VMAF_feature_vif_scale2_score",
        "VMAF_feature_vif_scale3_score",
        "vif",
        "vif_num",
        "vif_den",
        "vif_num_scale0",
        "vif_den_scale0",
        "vif_num_scale1",
        "vif_den_scale1",
        "vif_num_scale2",
        "vif_den_scale2",
        "vif_num_scale3",
        "vif_den_scale3",
    };
    static const char *const integer_names[15] = {
        "VMAF_integer_feature_vif_scale0_score",
        "VMAF_integer_feature_vif_scale1_score",
        "VMAF_integer_feature_vif_scale2_score",
        "VMAF_integer_feature_vif_scale3_score",
        "integer_vif",
        "integer_vif_num",
        "integer_vif_den",
        "integer_vif_num_scale0",
        "integer_vif_den_scale0",
        "integer_vif_num_scale1",
        "integer_vif_den_scale1",
        "integer_vif_num_scale2",
        "integer_vif_den_scale2",
        "integer_vif_num_scale3",
        "integer_vif_den_scale3",
    };
    if (!feature_collector || !scores)
        return -EINVAL;
    const char *const *names = name_set == VMAF_VIF_INTEGER_NAMES ? integer_names : float_names;
    VmafNamedScore values[15];
    int err = vmaf_vif_prepare_scale_scores(extractor, scores, names, values, index);
    if (err)
        return err;
    if (scores->debug) {
        err = vmaf_vif_prepare_debug_scores(extractor, scores, names + 4u, values, index);
        if (err)
            return err;
    }
    return vmaf_feature_emit_finite_scores(feature_collector, feature_name_dict, extractor, values,
                                           scores->debug ? 15u : 4u, index);
}

static inline int vmaf_ssim_prepare_score(double raw_score, int enable_db, double max_db,
                                          double *score)
{
    if (!score || !isfinite(raw_score))
        return -EINVAL;
    if (!enable_db) {
        *score = raw_score;
        return 0;
    }
    if (isnan(max_db) || max_db == -INFINITY)
        return -EINVAL;
    if (raw_score >= 1.0) {
        *score = max_db;
        return 0;
    }

    const double db_score = -10.0 * log10(1.0 - raw_score);
    const double value = db_score < max_db ? db_score : max_db;
    if (!isfinite(value))
        return -EINVAL;
    *score = value;
    return 0;
}

static inline int vmaf_ssim_prepare_score_named(const char *name, double raw_score, int enable_db,
                                                double max_db, unsigned index, double *score)
{
    const int err = vmaf_ssim_prepare_score(raw_score, enable_db, max_db, score);
    if (err) {
        vmaf_log(VMAF_LOG_LEVEL_WARNING,
                 "%s: undefined or non-finite SSIM score at frame %u "
                 "(raw=%g enable_db=%d max_db=%g)\n",
                 name ? name : "ssim", index, raw_score, enable_db, max_db);
    }
    return err;
}

static inline int vmaf_ssim_emit_score_named(VmafFeatureCollector *feature_collector,
                                             VmafDictionary *feature_name_dict,
                                             const char *extractor, const char *name,
                                             double raw_score, int enable_db, double max_db,
                                             unsigned index)
{
    if (!feature_collector || !name)
        return -EINVAL;
    double score = 0.0;
    const int err =
        vmaf_ssim_prepare_score_named(extractor, raw_score, enable_db, max_db, index, &score);
    if (err)
        return err;
    if (feature_name_dict) {
        return vmaf_feature_collector_append_with_dict(feature_collector, feature_name_dict, name,
                                                       score, index);
    }
    return vmaf_feature_collector_append(feature_collector, name, score, index);
}

static inline int vmaf_ssim_emit_score(VmafFeatureCollector *feature_collector,
                                       VmafDictionary *feature_name_dict, const char *name,
                                       double raw_score, int enable_db, double max_db,
                                       unsigned index)
{
    return vmaf_ssim_emit_score_named(feature_collector, feature_name_dict, name, name, raw_score,
                                      enable_db, max_db, index);
}

static inline int vmaf_ssim_emit_ratio_score_named(VmafFeatureCollector *feature_collector,
                                                   VmafDictionary *feature_name_dict,
                                                   const char *extractor, const char *name,
                                                   double numerator, double denominator,
                                                   int enable_db, double max_db, unsigned index)
{
    double raw_score = 0.0;
    if (vmaf_feature_finite_ratio_named(extractor, name, numerator, denominator, index,
                                        &raw_score)) {
        return -EINVAL;
    }
    return vmaf_ssim_emit_score_named(feature_collector, feature_name_dict, extractor, name,
                                      raw_score, enable_db, max_db, index);
}

static inline int vmaf_ssim_emit_ratio_score(VmafFeatureCollector *feature_collector,
                                             VmafDictionary *feature_name_dict, const char *name,
                                             double numerator, double denominator, int enable_db,
                                             double max_db, unsigned index)
{
    return vmaf_ssim_emit_ratio_score_named(feature_collector, feature_name_dict, name, name,
                                            numerator, denominator, enable_db, max_db, index);
}

static inline int vmaf_ssim_emit_scores_named(VmafFeatureCollector *feature_collector,
                                              VmafDictionary *feature_name_dict,
                                              const char *extractor, const char *name,
                                              double raw_score, int enable_db, double max_db,
                                              const VmafNamedScore *atoms, size_t atom_count,
                                              unsigned index)
{
    if (!feature_collector || !name || (atom_count != 0u && !atoms))
        return -EINVAL;

    double score = 0.0;
    int err = vmaf_ssim_prepare_score_named(extractor, raw_score, enable_db, max_db, index, &score);
    if (err)
        return err;
    for (size_t i = 0; i < atom_count; ++i) {
        if (!atoms[i].name || !isfinite(atoms[i].value)) {
            vmaf_log(VMAF_LOG_LEVEL_WARNING,
                     "%s: non-finite SSIM atom at frame %u (feature=%s value=%g)\n",
                     extractor ? extractor : name, index, atoms[i].name ? atoms[i].name : "<null>",
                     atoms[i].value);
            return -EINVAL;
        }
    }

    if (feature_name_dict) {
        err = vmaf_feature_collector_append_with_dict(feature_collector, feature_name_dict, name,
                                                      score, index);
    } else {
        err = vmaf_feature_collector_append(feature_collector, name, score, index);
    }
    for (size_t i = 0; i < atom_count && !err; ++i) {
        if (feature_name_dict) {
            err = vmaf_feature_collector_append_with_dict(feature_collector, feature_name_dict,
                                                          atoms[i].name, atoms[i].value, index);
        } else {
            err = vmaf_feature_collector_append(feature_collector, atoms[i].name, atoms[i].value,
                                                index);
        }
    }
    return err;
}

static inline int vmaf_ssim_emit_scores(VmafFeatureCollector *feature_collector,
                                        VmafDictionary *feature_name_dict, const char *name,
                                        double raw_score, int enable_db, double max_db,
                                        const VmafNamedScore *atoms, size_t atom_count,
                                        unsigned index)
{
    return vmaf_ssim_emit_scores_named(feature_collector, feature_name_dict, name, name, raw_score,
                                       enable_db, max_db, atoms, atom_count, index);
}

static inline int vmaf_ms_ssim_emit_scores(VmafFeatureCollector *feature_collector,
                                           VmafDictionary *feature_name_dict, const char *extractor,
                                           const char *name, double raw_score, int enable_db,
                                           double max_db, const double *luminance,
                                           const double *contrast, const double *structure,
                                           size_t scale_count, bool emit_atoms, unsigned index)
{
    static const char *const atom_names[15] = {
        "float_ms_ssim_l_scale0", "float_ms_ssim_l_scale1", "float_ms_ssim_l_scale2",
        "float_ms_ssim_l_scale3", "float_ms_ssim_l_scale4", "float_ms_ssim_c_scale0",
        "float_ms_ssim_c_scale1", "float_ms_ssim_c_scale2", "float_ms_ssim_c_scale3",
        "float_ms_ssim_c_scale4", "float_ms_ssim_s_scale0", "float_ms_ssim_s_scale1",
        "float_ms_ssim_s_scale2", "float_ms_ssim_s_scale3", "float_ms_ssim_s_scale4",
    };
    if (!luminance || !contrast || !structure || scale_count == 0u || scale_count > 5u)
        return -EINVAL;
    VmafNamedScore atoms[15];
    for (size_t scale = 0u; scale < scale_count; ++scale) {
        atoms[scale].name = atom_names[scale];
        atoms[scale].value = luminance[scale];
        atoms[scale_count + scale].name = atom_names[5u + scale];
        atoms[scale_count + scale].value = contrast[scale];
        atoms[2u * scale_count + scale].name = atom_names[10u + scale];
        atoms[2u * scale_count + scale].value = structure[scale];
    }
    const size_t atom_count = 3u * scale_count;
    double prepared_score = 0.0;
    int err = vmaf_ssim_prepare_score_named(extractor, raw_score, enable_db, max_db, index,
                                            &prepared_score);
    if (err)
        return err;
    err = vmaf_feature_validate_finite_scores_named(extractor, atoms, atom_count, index);
    if (err)
        return err;
    if (!emit_atoms) {
        return vmaf_ssim_emit_score_named(feature_collector, feature_name_dict, extractor, name,
                                          raw_score, enable_db, max_db, index);
    }
    return vmaf_ssim_emit_scores_named(feature_collector, feature_name_dict, extractor, name,
                                       raw_score, enable_db, max_db, atoms, atom_count, index);
}

/**
 * Atomically publish a set of ordinary metric scores.
 *
 * Validation deliberately precedes the first collector write: a failed
 * multi-value computation must not leave a partially published frame.  This
 * helper is for score families whose public values are required to be finite;
 * callers with an explicitly documented infinite sentinel must use their
 * family-specific emitter instead.
 */
static inline int vmaf_feature_emit_finite_scores(VmafFeatureCollector *feature_collector,
                                                  VmafDictionary *feature_name_dict,
                                                  const char *extractor,
                                                  const VmafNamedScore *scores, size_t score_count,
                                                  unsigned index)
{
    if (!feature_collector)
        return -EINVAL;
    int err = vmaf_feature_validate_finite_scores_named(extractor, scores, score_count, index);
    if (err)
        return err;

    err = 0;
    for (size_t i = 0; i < score_count && !err; ++i) {
        if (feature_name_dict) {
            err = vmaf_feature_collector_append_with_dict(feature_collector, feature_name_dict,
                                                          scores[i].name, scores[i].value, index);
        } else {
            err = vmaf_feature_collector_append(feature_collector, scores[i].name, scores[i].value,
                                                index);
        }
    }
    return err;
}

/**
 * Publish the four VIF scale scores after validating every computed ratio.
 * Validation finishes before scale 0 is written, preventing any bad scale
 * from leaving a partial frame in the collector.
 */
static inline int vmaf_vif_emit_scale_scores(VmafFeatureCollector *feature_collector,
                                             VmafDictionary *feature_name_dict,
                                             const char *extractor, const double scores[8],
                                             int skip_scale0, const double minimums[3],
                                             unsigned index)
{
    if (!feature_collector || !scores)
        return -EINVAL;

    double values[4] = {0.0, 0.0, 0.0, 0.0};
    if (!skip_scale0) {
        const int err = vmaf_feature_finite_ratio(scores[0], scores[1], &values[0]);
        if (err) {
            vmaf_log(VMAF_LOG_LEVEL_WARNING,
                     "%s: undefined or non-finite VIF scale at frame %u "
                     "(scale=0 num=%g den=%g)\n",
                     extractor ? extractor : "vif", index, scores[0], scores[1]);
            return err;
        }
    }
    for (size_t scale = 1u; scale < 4u; ++scale) {
        const int err =
            vmaf_feature_finite_ratio(scores[scale * 2u], scores[scale * 2u + 1u], &values[scale]);
        if (err) {
            vmaf_log(VMAF_LOG_LEVEL_WARNING,
                     "%s: undefined or non-finite VIF scale at frame %u "
                     "(scale=%zu num=%g den=%g)\n",
                     extractor ? extractor : "vif", index, scale, scores[scale * 2u],
                     scores[scale * 2u + 1u]);
            return err;
        }
        if (minimums) {
            const double minimum = minimums[scale - 1u];
            if (!isfinite(minimum)) {
                vmaf_log(VMAF_LOG_LEVEL_WARNING,
                         "%s: non-finite VIF minimum at frame %u "
                         "(scale=%zu minimum=%g)\n",
                         extractor ? extractor : "vif", index, scale, minimum);
                return -EINVAL;
            }
            if (values[scale] < minimum)
                values[scale] = minimum;
        }
    }

    static const char *const names[4] = {
        "VMAF_feature_vif_scale0_score",
        "VMAF_feature_vif_scale1_score",
        "VMAF_feature_vif_scale2_score",
        "VMAF_feature_vif_scale3_score",
    };
    int err = 0;
    for (size_t scale = 0u; scale < 4u && !err; ++scale) {
        if (feature_name_dict) {
            err = vmaf_feature_collector_append_with_dict(feature_collector, feature_name_dict,
                                                          names[scale], values[scale], index);
        } else {
            err = vmaf_feature_collector_append(feature_collector, names[scale], values[scale],
                                                index);
        }
    }
    return err;
}

#endif /* VMAF_FEATURE_NONFINITE_SCORE_H */
