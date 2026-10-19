/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * libvmaf model-collection scores on the VMAFx API (ADR-1852 design section
 * 2.11): VmafxModelSetScore read back into libvmaf's
 * VmafModelCollectionScore. VMAFX_PENDING is libvmaf's -EAGAIN.
 */

#include <errno.h>
#include <stdint.h>

#include "compat_errno.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/model.h"
#include "vmafx/libvmaf_bridge.h"
#include "vmafx/vmafx.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

static void collection_score(const VmafxModelSetScore *in, VmafModelCollectionScore *out)
{
    out->type = VMAF_MODEL_COLLECTION_SCORE_BOOTSTRAP;
    out->bootstrap.bagging_score = in->bagging;
    out->bootstrap.stddev = in->stddev;
    out->bootstrap.ci.p95.lo = in->ci95_lo;
    out->bootstrap.ci.p95.hi = in->ci95_hi;
}

int vmaf_score_at_index_model_collection(VmafContext *vmaf, VmafModelCollection *model_collection,
                                         VmafModelCollectionScore *score, unsigned index)
{
    if (!vmaf || !model_collection || !score) {
        return -EINVAL;
    }
    VmafxModelSetScore record = VMAFX_MODEL_SET_SCORE_INIT;
    VmafxError *error = NULL;
    const VmafxStatus status = vmafx_score_frame_model_set(
        vmafx_context_from_libvmaf(vmaf), vmafx_model_set_from_libvmaf(model_collection),
        (uint64_t)index, &record, &error);
    if (status != VMAFX_OK) {
        return compat_errno(status, error);
    }
    collection_score(&record, score);
    return 0;
}

int vmaf_score_pooled_model_collection(VmafContext *vmaf, VmafModelCollection *model_collection,
                                       enum VmafPoolingMethod pool_method,
                                       VmafModelCollectionScore *score, unsigned index_low,
                                       unsigned index_high)
{
    if (!vmaf || !model_collection || !score) {
        return -EINVAL;
    }
    VmafxModelSetScore record = VMAFX_MODEL_SET_SCORE_INIT;
    VmafxError *error = NULL;
    const VmafxStatus status = vmafx_score_pooled_model_set(
        vmafx_context_from_libvmaf(vmaf), vmafx_model_set_from_libvmaf(model_collection),
        (uint32_t)pool_method, (uint64_t)index_low, (uint64_t)index_high, &record, &error);
    if (status != VMAFX_OK) {
        return compat_errno(status, error);
    }
    collection_score(&record, score);
    return 0;
}

/* NOLINTEND(modernize-use-nullptr) */
