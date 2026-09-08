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

/* C-compatible context ownership and pointer-array growth (ADR-0723).
 * Feature identity includes parsed options, including for backend twins
 * (ADR-0385; Research-2047). No C++ container owns the published array. */

// feature_extractor.h uses <atomic>; include it before the C-linkage header.
#include <atomic>
#include "fex_ctx_vector.h"
#include "fex_ctx_vector_internal.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "feature/feature_name.h"
#include "log.h"

namespace
{

int feature_names_match(const VmafFeatureExtractor *a, const char *base_a,
                        const VmafFeatureExtractor *b, const char *base_b)
{
    using FeatureName = std::unique_ptr<char, decltype(&std::free)>;
    const FeatureName name_a(vmaf_feature_name_from_options(base_a, a->options, a->priv),
                             &std::free);
    if (!name_a)
        return -ENOMEM;
    const FeatureName name_b(vmaf_feature_name_from_options(base_b, b->options, b->priv),
                             &std::free);
    if (!name_b)
        return -ENOMEM;
    return strcmp(name_a.get(), name_b.get()) == 0;
}

int provided_features_overlap(const VmafFeatureExtractor *a, const VmafFeatureExtractor *b)
{
    if (!a->provided_features || !b->provided_features)
        return feature_names_match(a, a->name, b, b->name);

    // ADR-0385: preserve first shared feature wins, but compare emitted keys.
    // Different feature parameters have different collector slots (Research-2047).
    for (unsigned i = 0; a->provided_features[i]; i++) {
        for (unsigned j = 0; b->provided_features[j]; j++) {
            if (strcmp(a->provided_features[i], b->provided_features[j]) != 0)
                continue;
            const int result =
                feature_names_match(a, a->provided_features[i], b, b->provided_features[j]);
            if (result != 0)
                return result;
        }
    }
    return 0;
}

int grow_context_vector(RegisteredFeatureExtractors *rfe)
{
    const unsigned capacity = vmaf_next_fex_capacity(rfe->capacity);
    if (capacity == 0)
        return -ENOMEM;
    auto *contexts = static_cast<VmafFeatureExtractorContext **>(
        realloc(static_cast<void *>(rfe->fex_ctx), sizeof(*rfe->fex_ctx) * capacity));
    if (!contexts)
        return -ENOMEM;
    rfe->fex_ctx = contexts;
    rfe->capacity = capacity;
    for (unsigned i = rfe->cnt; i < rfe->capacity; i++)
        rfe->fex_ctx[i] = nullptr;
    return 0;
}

void log_registered_context(const VmafFeatureExtractorContext *fex_ctx)
{
    const unsigned cnt = fex_ctx->opts_dict ? fex_ctx->opts_dict->cnt : 0u;
    vmaf_log(VMAF_LOG_LEVEL_DEBUG, "feature extractor \"%s\" registered with %u opts\n",
             fex_ctx->fex->name, cnt);
    for (unsigned i = 0; i < cnt; i++) {
        vmaf_log(VMAF_LOG_LEVEL_DEBUG, "%s: %s\n", fex_ctx->opts_dict->entry[i].key,
                 fex_ctx->opts_dict->entry[i].val);
    }
}

} // namespace

int feature_extractor_vector_init(RegisteredFeatureExtractors *rfe)
{
    if (!rfe)
        return -EINVAL;

    static constexpr unsigned kInitialCapacity = 8u;
    rfe->cnt = 0;
    rfe->capacity = kInitialCapacity;
    const size_t bytes = sizeof(*rfe->fex_ctx) * rfe->capacity;
    rfe->fex_ctx = static_cast<VmafFeatureExtractorContext **>(malloc(bytes));
    if (!rfe->fex_ctx)
        return -ENOMEM;
    memset(static_cast<void *>(rfe->fex_ctx), 0, bytes);
    return 0;
}

int feature_extractor_vector_append(RegisteredFeatureExtractors *rfe,
                                    VmafFeatureExtractorContext *fex_ctx, uint64_t flags)
{
    if (!rfe || !fex_ctx)
        return -EINVAL;

    (void)flags;
    for (unsigned i = 0; i < rfe->cnt; i++) {
        const int overlap = provided_features_overlap(rfe->fex_ctx[i]->fex, fex_ctx->fex);
        if (overlap < 0)
            return overlap;
        if (overlap != 0) {
            vmaf_log(VMAF_LOG_LEVEL_DEBUG,
                     "feature extractor \"%s\" skipped: provided features already covered "
                     "by registered extractor \"%s\"\n",
                     fex_ctx->fex->name, rfe->fex_ctx[i]->fex->name);
            return vmaf_feature_extractor_context_destroy(fex_ctx);
        }
    }

    if (rfe->cnt >= rfe->capacity) {
        const int err = grow_context_vector(rfe);
        if (err)
            return err;
    }
    log_registered_context(fex_ctx);
    rfe->fex_ctx[rfe->cnt++] = fex_ctx;
    return 0;
}

void feature_extractor_vector_destroy(RegisteredFeatureExtractors *rfe)
{
    if (!rfe)
        return;
    for (unsigned i = 0; i < rfe->cnt; i++) {
        (void)vmaf_feature_extractor_context_close(rfe->fex_ctx[i]);
        (void)vmaf_feature_extractor_context_destroy(rfe->fex_ctx[i]);
    }
    free(static_cast<void *>(rfe->fex_ctx));
}
