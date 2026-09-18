/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  MobileSal saliency feature extractor (T6-2a) — structural + stub-path
 *  tests. Body delegated to `tiny_ai_test_template.h`. End-to-end
 *  inference is covered by the CLI smoke gate against the placeholder
 *  ONNX shipped under model/tiny/mobilesal.onnx.
 */

#include "tiny_ai_test_template.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe, and this file mirrors
 * the C spelling of the surface it exercises. ADR-1138. */

VMAF_TINY_AI_DEFINE_REGISTRATION_TESTS("mobilesal", "saliency_mean", "VMAF_MOBILESAL_MODEL_PATH",
                                       mobilesal)

char *run_tests(void)
{
    VMAF_TINY_AI_RUN_REGISTRATION_TESTS(mobilesal);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
