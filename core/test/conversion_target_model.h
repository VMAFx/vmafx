/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * A model with a `conversion_target` (ADR-2093) for the tests of the frame
 * colour of the VMAFx API and of its libvmaf compat function (ADR-2094):
 * one integer VIF feature on a one-vector SVM, defined in ICtCp (BT.2020
 * primaries, PQ, limited range). A test writes it to a file of its own
 * with plain stdio (no mkstemp, as upstream's Windows fix 130569c45 does)
 * and loads it from there.
 */

#ifndef VMAF_TEST_CONVERSION_TARGET_MODEL_H
#define VMAF_TEST_CONVERSION_TARGET_MODEL_H

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>

#include "test_fopen.h"

#define CONVERSION_TARGET_MODEL_JSON                                                               \
    "{\"model_dict\": {"                                                                           \
    "\"conversion_target\": {\"colorspace\": {\"range\": \"limited\", \"primaries\": "             \
    "\"bt2020\", \"trc\": \"smpte2084\", \"matrix\": \"ictcp\"}}, "                                \
    "\"model_type\": \"LIBSVMNUSVR\", \"norm_type\": \"linear_rescale\", "                         \
    "\"score_clip\": [0.0, 100.0], "                                                               \
    "\"feature_names\": [\"VMAF_integer_feature_vif_scale0_score\"], "                             \
    "\"slopes\": [1.0, 1.0], \"intercepts\": [0.0, 0.0], "                                         \
    "\"model\": \"svm_type nu_svr\\nkernel_type rbf\\ngamma 0.5\\nnr_class 2\\ntotal_sv 1\\n"      \
    "rho -1\\nSV\\n1 1:0.5 \\n\"}}"

/* Write the model to `path`: 0, or -EIO. */
static inline int conversion_target_model_write(const char *path)
{
    FILE *const out = vmaf_test_fopen(path, "wb");
    if (!out) {
        return -EIO;
    }
    const bool written = fputs(CONVERSION_TARGET_MODEL_JSON, out) >= 0;
    const bool closed = fclose(out) == 0;
    return written && closed ? 0 : -EIO;
}

#endif /* VMAF_TEST_CONVERSION_TARGET_MODEL_H */
