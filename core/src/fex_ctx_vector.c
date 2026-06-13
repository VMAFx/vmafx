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
#include <string.h>

#include "feature/feature_extractor.h"
#include "feature/feature_name.h"
#include "fex_ctx_vector.h"
#include "log.h"

int feature_extractor_vector_init(RegisteredFeatureExtractors *rfe)
{
    rfe->cnt = 0;
    rfe->capacity = 8;
    const size_t sz = sizeof(*(rfe->fex_ctx)) * rfe->capacity;
    rfe->fex_ctx = (VmafFeatureExtractorContext **)malloc(sz);
    if (!rfe->fex_ctx)
        return -ENOMEM;
    memset((void *)rfe->fex_ctx, 0, sz);
    return 0;
}

/**
 * Check whether two feature extractors advertise any common provided-feature
 * name.  CPU and GPU twins (e.g. "adm" / "adm_cuda" / "adm_sycl") have
 * different extractor names but share every entry in provided_features[]; a
 * single common name is sufficient to identify them as twins that would write
 * the same feature-collector slot.
 *
 * Returns true when at least one provided-feature name appears in both lists.
 * Returns false when either extractor has a NULL provided_features pointer
 * (extractors that do not declare what they emit are not considered twins).
 */
static bool provided_features_overlap(const VmafFeatureExtractor *a, const VmafFeatureExtractor *b)
{
    if (!a->provided_features || !b->provided_features)
        return false;

    for (unsigned i = 0; a->provided_features[i]; i++) {
        for (unsigned j = 0; b->provided_features[j]; j++) {
            if (!strcmp(a->provided_features[i], b->provided_features[j]))
                return true;
        }
    }
    return false;
}

int feature_extractor_vector_append(RegisteredFeatureExtractors *rfe,
                                    VmafFeatureExtractorContext *fex_ctx, uint64_t flags)
{
    if (!rfe)
        return -EINVAL;
    if (!fex_ctx)
        return -EINVAL;

    (void)flags;

    for (unsigned i = 0; i < rfe->cnt; i++) {
        /* Deduplicate by provided-feature names rather than extractor name.
         * CPU/GPU twins (e.g. "adm" vs "adm_cuda") have different extractor
         * names but advertise the same provided_features[] entries — matching
         * on extractor name alone misses these pairs and lets both register,
         * causing "cannot be overwritten" warnings on every scored frame.
         * Matching on any shared provided-feature name catches all backend
         * twins because the cross-backend parity contract (ADR-0214) requires
         * every twin to emit the same feature set.
         * Fall back to the original extractor-name comparison when either side
         * has no provided_features list (NULL), so extractors that skip the
         * declaration field are still deduplicated by the old path. */
        if (provided_features_overlap(rfe->fex_ctx[i]->fex, fex_ctx->fex)) {
            vmaf_log(VMAF_LOG_LEVEL_DEBUG,
                     "feature extractor \"%s\" skipped: provided features already covered "
                     "by registered extractor \"%s\"\n",
                     fex_ctx->fex->name, rfe->fex_ctx[i]->fex->name);
            return vmaf_feature_extractor_context_destroy(fex_ctx);
        }

        /* Legacy path: both extractors omit provided_features — fall back to
         * comparing the vmaf_feature_name_from_options()-derived key so that
         * same-named extractors with identical option sets are still deduped. */
        if (!fex_ctx->fex->provided_features || !rfe->fex_ctx[i]->fex->provided_features) {
            char *feature_a = vmaf_feature_name_from_options(rfe->fex_ctx[i]->fex->name,
                                                             rfe->fex_ctx[i]->fex->options,
                                                             rfe->fex_ctx[i]->fex->priv);
            char *feature_b = vmaf_feature_name_from_options(
                fex_ctx->fex->name, fex_ctx->fex->options, fex_ctx->fex->priv);
            int ret = 1;
            if (feature_a && feature_b)
                ret = strcmp(feature_a, feature_b);
            free(feature_a);
            free(feature_b);
            if (ret == 0)
                return vmaf_feature_extractor_context_destroy(fex_ctx);
        }
    }

    if (rfe->cnt >= rfe->capacity) {
        assert(rfe->capacity > 0);
        const size_t capacity = (size_t)rfe->capacity * 2;
        VmafFeatureExtractorContext **fex_ctx_new = (VmafFeatureExtractorContext **)realloc(
            (void *)rfe->fex_ctx, sizeof(*(rfe->fex_ctx)) * capacity);
        if (!fex_ctx_new)
            return -ENOMEM;
        rfe->fex_ctx = fex_ctx_new;
        rfe->capacity = capacity;
        for (unsigned i = rfe->cnt; i < rfe->capacity; i++)
            rfe->fex_ctx[i] = NULL;
    }

    const unsigned cnt = fex_ctx->opts_dict ? fex_ctx->opts_dict->cnt : 0;
    vmaf_log(VMAF_LOG_LEVEL_DEBUG,
             "feature extractor \"%s\" registered "
             "with %d opts\n",
             fex_ctx->fex->name, cnt);

    for (unsigned i = 0; i < cnt; i++) {
        vmaf_log(VMAF_LOG_LEVEL_DEBUG, "%s: %s\n", fex_ctx->opts_dict->entry[i].key,
                 fex_ctx->opts_dict->entry[i].val);
    }

    rfe->fex_ctx[rfe->cnt++] = fex_ctx;
    return 0;
}

void feature_extractor_vector_destroy(RegisteredFeatureExtractors *rfe)
{
    if (!rfe)
        return;
    for (unsigned i = 0; i < rfe->cnt; i++) {
        vmaf_feature_extractor_context_close(rfe->fex_ctx[i]);
        vmaf_feature_extractor_context_destroy(rfe->fex_ctx[i]);
    }
    free((void *)rfe->fex_ctx);
}
