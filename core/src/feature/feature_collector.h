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

#ifndef VMAF_FEATURE_COLLECTOR_INCLUDED
#define VMAF_FEATURE_COLLECTOR_INCLUDED

#ifdef __cplusplus
extern "C" {
#endif

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <time.h>

#include "dict.h"
#include "model.h"
#include "metadata_handler.h"

/** Initial allocation size for FeatureVector::score[], AggregateVector::metric[],
 *  and VmafFeatureCollector::feature_vector[].  All three grow by doubling so
 *  this controls only the first malloc; 8 covers typical per-frame feature
 *  counts without over-allocating for single-feature runs. */
#define FEATURE_VECTOR_INITIAL_CAPACITY 8u

/** Upper bound on the per-feature frame index accepted by
 *  feature_vector_append().  FeatureVector::score[] is a sparse, frame-indexed
 *  array grown by capacity doubling; `index` reaches it unfiltered from the
 *  public vmaf_import_feature_score() entry point, so a caller-supplied value
 *  near UINT_MAX would (a) wrap `capacity` (unsigned) to 0 during doubling —
 *  with assert() compiled out under NDEBUG that yields realloc(p, 0) and an
 *  infinite loop — and (b) demand a multi-gigabyte allocation.  2^28 frames is
 *  far beyond any real video (~124 days at 25 fps) while keeping the doubling
 *  arithmetic and the resulting allocation bounded.  See finding R2-5. */
#define FEATURE_VECTOR_MAX_INDEX (1u << 28)

typedef struct {
    char *name;
    struct {
        bool written;
        double value;
    } *score;
    unsigned capacity;
} FeatureVector;

typedef struct {
    struct {
        char *name;
        double value;
    } *metric;
    unsigned cnt, capacity;
} AggregateVector;

typedef struct VmafPredictModel {
    VmafModel *model;
    struct VmafPredictModel *next;
} VmafPredictModel;

typedef struct VmafFeatureCollector {
    FeatureVector **feature_vector;
    AggregateVector aggregate_vector;
    VmafCallbackList *metadata;
    VmafPredictModel *models;
    unsigned cnt, capacity;
    struct {
        clock_t begin, end;
    } timer;
    pthread_mutex_t lock;
    /* Set to true under lock inside vmaf_feature_collector_destroy() before
     * the final unlock.  All public entry points that acquire lock must test
     * this flag immediately after locking; if true they must release the lock
     * and return -ENODEV.  This prevents the mutex-destroy-after-unlock race
     * where a thread blocked on pthread_mutex_lock would acquire a mutex that
     * has already been destroyed (UB). */
    bool destroyed;
} VmafFeatureCollector;

int vmaf_feature_collector_init(VmafFeatureCollector **const feature_collector);

int vmaf_feature_collector_mount_model(VmafFeatureCollector *feature_collector, VmafModel *model);

int vmaf_feature_collector_unmount_model(VmafFeatureCollector *feature_collector, VmafModel *model);

int vmaf_feature_collector_append(VmafFeatureCollector *feature_collector, const char *feature_name,
                                  double score, unsigned index);

int vmaf_feature_collector_register_metadata(VmafFeatureCollector *feature_collector,
                                             VmafMetadataConfiguration metadata_cfg);

int vmaf_feature_collector_append_with_dict(VmafFeatureCollector *fc, VmafDictionary *dict,
                                            const char *feature_name, double score, unsigned index);

int vmaf_feature_collector_get_score(VmafFeatureCollector *feature_collector,
                                     const char *feature_name, double *score, unsigned index);

FeatureVector *vmaf_feature_collector_find(VmafFeatureCollector *feature_collector,
                                           const char *feature_name);

/* Round-5 race fix (finding #4): this inline must only be called while the
 * caller holds the VmafFeatureCollector lock.  predict_load_feature_score
 * previously called it after dropping the lock; that call site was moved to
 * use vmaf_feature_collector_get_score (which acquires the lock internally)
 * so the read of .written / .value is always protected.  See predict.c. */
static inline int vmaf_feature_vector_get_score(FeatureVector *fv, double *score, unsigned index)
{
    if (!fv || index >= fv->capacity)
        return -EINVAL;
    if (!fv->score[index].written)
        return -EAGAIN; /* Netflix#755 / ADR-0154 — distinguish invalid vs
                         * not-yet-written (e.g. motion2 retroactive-write). */
    *score = fv->score[index].value;
    return 0;
}

int vmaf_feature_collector_set_aggregate(VmafFeatureCollector *feature_collector,
                                         const char *feature_name, double score);

int vmaf_feature_collector_get_aggregate(VmafFeatureCollector *feature_collector,
                                         const char *feature_name, double *score);

void vmaf_feature_collector_destroy(VmafFeatureCollector *feature_collector);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* VMAF_FEATURE_COLLECTOR_INCLUDED */
