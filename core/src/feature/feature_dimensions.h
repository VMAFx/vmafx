/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Centralised feature and model dimension validation helpers.
 *  Sourced directly from feature-internal headers (cambi_internal.h,
 *  speed_internal.h) to eliminate duplicate threshold definitions.
 */

#ifndef LIBVMAF_FEATURE_DIMENSIONS_H_
#define LIBVMAF_FEATURE_DIMENSIONS_H_

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "libvmaf/model.h"
#include "libvmaf/picture.h"
#include "cambi_internal.h"
#include "speed_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Validate that input dimensions satisfy a feature's minimum resolution constraints.
 * Sourced directly from cambi_internal.h and speed_internal.h.
 * Returns 0 if valid or unconstrained, -EINVAL if constraints violated.
 * On -EINVAL, writes a reason phrase into err_msg (e.g. "needs width or height >= 216; got 160x90").
 */
static inline int vmaf_validate_feature_dimensions(const char *feature_name, unsigned w, unsigned h,
                                                   enum VmafPixelFormat pix_fmt, char *err_msg,
                                                   size_t sz)
{
    if (!feature_name)
        return 0;

    /* CAMBI resolution constraint: at least one dimension >= CAMBI_MIN_WIDTH_HEIGHT (216). */
    if (strstr(feature_name, "cambi") || strstr(feature_name, "Cambi")) {
        if (!cambi_validate_dimensions(w, h)) {
            if (err_msg && sz > 0) {
                (void)snprintf(err_msg, sz, "needs width or height >= %u; got %ux%u",
                               (unsigned)CAMBI_MIN_WIDTH_HEIGHT, w, h);
            }
            return -EINVAL;
        }
    }

    /* SpEED chroma resolution constraint: chroma width and height >= SPEED_INTERNAL_MIN_DIMENSION (80). */
    if (strstr(feature_name, "speed_chroma") || strstr(feature_name, "Speed_chroma")) {
        unsigned cw = 0, ch = 0;
        const int rc = speed_chroma_dimensions(w, h, pix_fmt, &cw, &ch);
        if (rc) {
            if (err_msg && sz > 0) {
                (void)snprintf(err_msg, sz, "requires chroma planes (unsupported pixel format %d)",
                               (int)pix_fmt);
            }
            return -EINVAL;
        }
        if (!speed_validate_dimensions(cw, ch, 1.0)) {
            if (err_msg && sz > 0) {
                (void)snprintf(err_msg, sz, "needs chroma width and height >= %u; got %ux%u",
                               (unsigned)SPEED_INTERNAL_MIN_DIMENSION, cw, ch);
            }
            return -EINVAL;
        }
    }

    /* SpEED temporal / QA resolution constraint: width and height >= SPEED_INTERNAL_MIN_DIMENSION (80). */
    if (strstr(feature_name, "speed_temporal") || strstr(feature_name, "Speed_temporal") ||
        strstr(feature_name, "speed_qa") || strstr(feature_name, "Speed_qa")) {
        if (!speed_validate_dimensions(w, h, 1.0)) {
            if (err_msg && sz > 0) {
                (void)snprintf(err_msg, sz, "needs width and height >= %u; got %ux%u",
                               (unsigned)SPEED_INTERNAL_MIN_DIMENSION, w, h);
            }
            return -EINVAL;
        }
    }

    return 0;
}

/**
 * Validate that input dimensions satisfy all features required by a model.
 * Returns 0 if all features satisfy constraints, -EINVAL if any feature fails.
 * On -EINVAL, writes "model '<name>' requires feature '<feature>', which <reason>" into err_msg.
 */
static inline int vmaf_validate_model_dimensions(const VmafModel *model, const char *model_name,
                                                 unsigned w, unsigned h,
                                                 enum VmafPixelFormat pix_fmt, char *err_msg,
                                                 size_t sz)
{
    if (!model)
        return 0;

    const char *const mname = model_name ? model_name : "vmaf";

    const unsigned n_features = vmaf_model_feature_count(model);
    for (unsigned i = 0; i < n_features; i++) {
        const char *const feat_name = vmaf_model_feature_name(model, i);
        if (!feat_name)
            continue;

        const char *canonical_name = feat_name;
        if (strstr(feat_name, "cambi") || strstr(feat_name, "Cambi"))
            canonical_name = "cambi";
        else if (strstr(feat_name, "speed_chroma") || strstr(feat_name, "Speed_chroma"))
            canonical_name = "speed_chroma";
        else if (strstr(feat_name, "speed_temporal") || strstr(feat_name, "Speed_temporal"))
            canonical_name = "speed_temporal";
        else if (strstr(feat_name, "speed_qa") || strstr(feat_name, "Speed_qa"))
            canonical_name = "speed_qa";

        char reason[256] = {0};
        const int rc =
            vmaf_validate_feature_dimensions(canonical_name, w, h, pix_fmt, reason, sizeof(reason));
        if (rc) {
            if (err_msg && sz > 0) {
                (void)snprintf(err_msg, sz, "model '%s' requires feature '%s', which %s", mname,
                               canonical_name, reason);
            }
            return rc;
        }
    }

    return 0;
}

#ifdef __cplusplus
}

static inline int vmaf_validate_model_dimensions(const VmafModel *model, unsigned w, unsigned h,
                                                 enum VmafPixelFormat pix_fmt, char *err_msg,
                                                 size_t sz)
{
    return vmaf_validate_model_dimensions(model, nullptr, w, h, pix_fmt, err_msg, sz);
}
#endif

#endif /* LIBVMAF_FEATURE_DIMENSIONS_H_ */
