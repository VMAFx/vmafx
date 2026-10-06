/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Internal entry points of the scoring engine (core/src/libvmaf.c) that the
 * VMAFx API (core/src/vmafx/) is built on (ADR-1852). The libvmaf functions
 * these replace (vmaf_init, vmaf_close, vmaf_feature_score_at_index,
 * vmaf_version) are generated compat shims on the VMAFx API now
 * (core/src/vmafx/compat_libvmaf_gen.c). Nothing here is exported.
 */

#ifndef VMAFX_ENGINE_H
#define VMAFX_ENGINE_H

#include <stdbool.h>

#include "libvmaf/feature.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/model.h"

/* Bound of the provided-feature walk in vmaf_engine_feature_producer()
 * (HISS-02); no extractor declares more than a few dozen. */
#define VMAF_ENGINE_MAX_PROVIDED_FEATURES 256u

struct VmafxContext;

/* The former bodies of vmaf_init(), vmaf_close(),
 * vmaf_feature_score_at_index() and vmaf_version(), unchanged. */
int vmaf_engine_init(VmafContext **vmaf, VmafConfiguration cfg);
int vmaf_engine_close(VmafContext *vmaf);
int vmaf_engine_feature_score_at_index(VmafContext *vmaf, const char *feature_name, double *score,
                                       unsigned index);
const char *vmaf_engine_version(void);

/* ADR-1852 RC4 WP2: the former bodies of these libvmaf functions, unchanged.
 * The libvmaf names forward to them (core/src/libvmaf.c) until WP6 generates
 * them as compat shims on the VMAFx API; engine code calls only these. */
int vmaf_engine_use_feature(VmafContext *vmaf, const char *feature_name,
                            VmafFeatureDictionary *opts_dict);
int vmaf_engine_use_features_from_model(VmafContext *vmaf, VmafModel *model);
int vmaf_engine_use_features_from_model_collection(VmafContext *vmaf,
                                                   VmafModelCollection *model_collection);
int vmaf_engine_import_feature_score(VmafContext *vmaf, const char *feature_name, double value,
                                     unsigned index);
int vmaf_engine_set_perceptual_weight_enabled(VmafContext *vmaf, int enabled);
int vmaf_engine_set_perceptual_weight_strength(VmafContext *vmaf, double strength);
int vmaf_engine_feature_backend_twin(VmafContext *vmaf, const char *feature_name,
                                     const VmafFeatureDictionary *opts_dict,
                                     const VmafPictureConfiguration *pic_cfg,
                                     const char **twin_name, const char **unsupported_option);
int vmaf_engine_registered_feature_extractor(VmafContext *vmaf, unsigned index, const char **name,
                                             enum VmafBackend *backend);
int vmaf_engine_read_pictures(VmafContext *vmaf, VmafPicture *ref, VmafPicture *dist,
                              unsigned index);
int vmaf_engine_score_at_index(VmafContext *vmaf, VmafModel *model, double *score, unsigned index);
int vmaf_engine_score_at_index_model_collection(VmafContext *vmaf,
                                                VmafModelCollection *model_collection,
                                                VmafModelCollectionScore *score, unsigned index);
int vmaf_engine_feature_score_pooled(VmafContext *vmaf, const char *feature_name,
                                     enum VmafPoolingMethod pool_method, double *score,
                                     unsigned index_low, unsigned index_high);
int vmaf_engine_score_pooled(VmafContext *vmaf, VmafModel *model,
                             enum VmafPoolingMethod pool_method, double *score, unsigned index_low,
                             unsigned index_high);
int vmaf_engine_score_pooled_model_collection(VmafContext *vmaf,
                                              VmafModelCollection *model_collection,
                                              enum VmafPoolingMethod pool_method,
                                              VmafModelCollectionScore *score, unsigned index_low,
                                              unsigned index_high);

/* RC4 WP4 (ADR-2074): whether a frame's score is final, without waiting for
 * work in flight (no fence) and without a log line. 0 when final; -EAGAIN when
 * an input is not written yet; -EINVAL when the feature or an input has no
 * score at `index` in the collector yet; another negative errno on failure.
 * The model and model-set forms predict the frame when its inputs are
 * written, as the first synchronous read of it would, so a later read
 * returns that stored score. */
int vmaf_engine_feature_written(VmafContext *vmaf, const char *feature_name, unsigned index);
int vmaf_engine_try_score_at_index(VmafContext *vmaf, VmafModel *model, unsigned index);
int vmaf_engine_try_score_at_index_model_collection(VmafContext *vmaf,
                                                    VmafModelCollection *model_collection,
                                                    unsigned index);

/* The context's worker threads (0: none) and its subsampling (1: every
 * frame is scored); 0 and 1 for NULL. */
unsigned vmaf_engine_thread_count(const VmafContext *vmaf);
/* The most pictures of each input the context holds when
 * vmaf_engine_read_pictures() returns (vmafx_context_max_in_flight()); 0 for
 * NULL. */
unsigned vmaf_engine_max_in_flight(const VmafContext *vmaf);
unsigned vmaf_engine_subsample(const VmafContext *vmaf);

/* Earlier reference frames the context keeps after a frame was read: 1, or 2
 * once an extractor that reads frame n-2 is registered (ADR-1478); 0 for
 * NULL. */
unsigned vmaf_engine_frame_retention(const VmafContext *vmaf);

/* Backend of the registered extractor named `extractor` (UNKNOWN, the CPU,
 * for a CPU extractor or an unknown name). */
enum VmafBackend vmaf_engine_extractor_backend(const char *extractor);

/* True once the context was flushed (vmaf_read_pictures(NULL, NULL)). */
bool vmaf_engine_is_flushed(const VmafContext *vmaf);

/* The VMAFx context an engine context belongs to (NULL for NULL). */
struct VmafxContext *vmaf_engine_api_owner(const VmafContext *vmaf);
void vmaf_engine_set_api_owner(VmafContext *vmaf, struct VmafxContext *owner);

/* ADR-2074: `listener(user)` runs on a worker thread after each frame job
 * (its features are in the collector). Set before the first frame is read;
 * NULL turns it off. */
void vmaf_engine_set_frame_listener(VmafContext *vmaf, void (*listener)(void *user), void *user);

/* ADR-2090: append the scores the collector's contents now make final
 * (motion2 / motion3 of every frame whose window the SAD scores complete),
 * as vmaf_engine_read_pictures() does after each frame. For the VMAFx
 * completion thread, under the context's engine lock, before it probes: a
 * SAD a worker appended after the feeder's last call completes its frames.
 * 0, or the first extractor's negative errno; nothing after the flush. */
int vmaf_engine_advance(VmafContext *vmaf);

/* Number of registered feature extractors. */
unsigned vmaf_engine_extractor_count(const VmafContext *vmaf);

/* The first registered extractor whose provided_features names `feature`:
 * 0 and its name and backend, or -ENOENT (name NULL, backend UNKNOWN). */
int vmaf_engine_feature_producer(const VmafContext *vmaf, const char *feature,
                                 const char **extractor, enum VmafBackend *backend);

#endif /* VMAFX_ENGINE_H */
