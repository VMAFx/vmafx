/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  Model ownership: vmaf_model_ref() and vmaf_model_destroy() (ADR-1755).
 *
 *  They live apart from model.c, in the predict_c archive, because the feature
 *  collector mounts models and so takes and drops owners; every binary that
 *  links the collector (libvmaf and the private-source tests) already links
 *  that archive, whereas model.c is compiled by only some of them.
 */

#include <errno.h>
#include <stdlib.h>

#include "libvmaf/model.h"

#include "dict.h"
#include "model.h"
#include "ref.h"
#include "svm.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr`, and this
 * file mirrors the C spelling of the code it was split from. ADR-1138. */

int vmaf_model_ref(VmafModel *model)
{
    if (!model || !model->owners)
        return -EINVAL;
    vmaf_ref_fetch_increment(model->owners);
    return 0;
}

void vmaf_model_destroy(VmafModel *model)
{
    if (!model)
        return;
    /* ADR-1755: the caller and every feature collector the model is mounted on
     * each hold one owner; the last one frees it. */
    if (model->owners && vmaf_ref_fetch_decrement(model->owners) != 1)
        return;
    if (model->owners)
        (void)vmaf_ref_close(model->owners);
    free(model->path);
    free(model->name);
    free(model->source);
    free(model->overrides);
    svm_free_and_destroy_model(&(model->svm));
    /* Walk the full feature_cap, not min(feature_cap, n_features).
     *
     * feature_cap IS the allocated element count, so this cannot read past the
     * buffer — it preserves the overflow safety the previous min() was written
     * for, while also freeing slots n_features does not cover.
     *
     * That gap was a real leak. n_features is only incremented by
     * parse_feature_names, but ensure_feature_capacity() is also called by
     * parse_feature_opts_dicts / parse_slopes / parse_intercepts, and
     * parse_feature_opts_dicts stores an owned VmafDictionary in the slot. A
     * model carrying `feature_opts_dicts` with no (or fewer) `feature_names`
     * therefore left dictionaries above n_features that nothing could free —
     * a 16-byte-per-entry leak found by the fuzz_json_model LeakSanitizer lane.
     * Inflating n_features instead would be wrong: it is the semantic count of
     * model features and feeds prediction, not a memory-management counter.
     *
     * Walking the tail is safe because ensure_feature_capacity() memsets every
     * newly grown slot to zero, so an untouched slot holds NULL and both
     * free(NULL) and vmaf_dictionary_free(&NULL) are no-ops. */
    for (unsigned i = 0; i < model->feature_cap; i++) {
        free(model->feature[i].name);
        vmaf_dictionary_free(&model->feature[i].opts_dict);
    }
    free(model->feature);
    free(model->score_transform.knots.list);
    free(model->predict_nodes);
    if (model->predict_feature_names) {
        for (unsigned i = 0; i < model->n_features; i++) {
            free(model->predict_feature_names[i]);
        }
        free((void *)model->predict_feature_names);
    }
    free((void *)model->predict_feature_vectors);
    /* Round-5 race fix (finding #3): destroy the predict-cache mutex that was
     * initialized in vmaf_read_json_model(). */
    pthread_mutex_destroy(&model->predict_cache_lock);
    free(model);
}

/* NOLINTEND(modernize-use-nullptr) */
