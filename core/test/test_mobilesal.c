/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  MobileSal saliency feature extractor (T6-2a) — structural + stub-path
 *  tests. Body delegated to `tiny_ai_test_template.h`. End-to-end
 *  inference is covered by the CLI smoke gate against the placeholder
 *  ONNX shipped under model/tiny/mobilesal.onnx.
 */

#include "tiny_ai_test_template.h"

VMAF_TINY_AI_DEFINE_REGISTRATION_TESTS("mobilesal", "saliency_mean", "VMAF_MOBILESAL_MODEL_PATH",
                                       mobilesal)

char *run_tests(void)
{
    VMAF_TINY_AI_RUN_REGISTRATION_TESTS(mobilesal);
    return NULL;
}
