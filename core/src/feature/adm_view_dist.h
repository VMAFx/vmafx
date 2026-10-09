/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 */

/*
 * The second viewing distance of the integer ADM extractors (Netflix/vmaf
 * cffd5b77d, ADR-2795), through the option table.
 *
 * `adm` and each of its twins (adm_rust, adm_cuda, adm_sycl, adm_hip,
 * integer_adm_metal) declare `adm_norm_view_dist`, `adm_norm_view_dist_extra`,
 * `adm_skip_aim` and `debug` with the same names and types, each at the offset
 * of its own state struct. These helpers find the fields by name, so one
 * implementation serves every descriptor: the merge callback, the
 * feature-name dictionary of the second distance, and the distances an
 * instance evaluates.
 */

#ifndef VMAF_FEATURE_ADM_VIEW_DIST_H_
#define VMAF_FEATURE_ADM_VIEW_DIST_H_

#include "dict.h"
#include "feature_extractor.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The seven scores an ADM extractor files per viewing distance. */
#define VMAF_ADM_VIEW_SCORE_COUNT 7u

/* Dictionary-key suffix of the second distance's scores. The Rust twin
 * (core/src/rust/feature/adm/src/score.rs) files under the same keys. */
#define VMAF_ADM_EXTRA_VIEW_KEY_SUFFIX ":nvde"

/* Provided-feature names of the seven scores, in filing order. */
extern const char *const vmaf_adm_view_names[VMAF_ADM_VIEW_SCORE_COUNT];

/* The same names with VMAF_ADM_EXTRA_VIEW_KEY_SUFFIX: the second distance's
 * dictionary keys. */
extern const char *const vmaf_adm_extra_view_keys[VMAF_ADM_VIEW_SCORE_COUNT];

/**
 * Viewing distances `fex`, with its options parsed into `priv`, evaluates: 2
 * with `adm_norm_view_dist_extra` > 0, else 1; 0 when `fex` has no such
 * options or no `priv`.
 */
unsigned vmaf_adm_view_count(const VmafFeatureExtractor *fex);

/**
 * The viewing distance of `view` (0: `adm_norm_view_dist`, 1:
 * `adm_norm_view_dist_extra`); 0.0 when `fex` has no such option.
 */
double vmaf_adm_view_dist(const VmafFeatureExtractor *fex, unsigned view);

/**
 * `VmafFeatureExtractor::merge` of every ADM extractor: fold `incoming`, an
 * instance whose options differ from `existing`'s only in
 * `adm_norm_view_dist`, into `existing` as its second distance. Returns 1
 * when absorbed, 0 when declined, or a negative errno. Declines an incoming
 * instance with `debug` (its unsuffixed debug scores would be lost) or with a
 * second distance of its own, a differing `adm_skip_aim` (not a feature
 * parameter) and an `existing` that already has another second distance. An
 * incoming instance at the distance `existing` evaluates second is absorbed
 * without a change.
 */
int vmaf_adm_merge_view_dist(VmafFeatureExtractorContext *existing,
                             VmafFeatureExtractorContext *incoming);

/**
 * `VmafFeatureExtractor::extend_name_dict` of every ADM extractor: with a
 * second distance, map each key of vmaf_adm_extra_view_keys to the name the
 * feature has at that distance. -EINVAL when those names are the first
 * distance's (an equal distance), -ENOMEM on allocation failure.
 */
int vmaf_adm_extend_name_dict(const VmafFeatureExtractor *fex, VmafDictionary **dict);

#ifdef __cplusplus
}
#endif

#endif /* VMAF_FEATURE_ADM_VIEW_DIST_H_ */
