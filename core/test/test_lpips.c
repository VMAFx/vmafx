/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  LPIPS feature extractor — structural + stub-path tests.
 *
 *  Body delegated to `tiny_ai_test_template.h` (the four standard
 *  tiny-AI registration tests live in one place — emitted via the
 *  macro). Full end-to-end inference (YUV → ORT run → score append)
 *  is exercised by the CLI smoke gate against
 *  model/tiny/lpips_sq.onnx; this file only verifies the
 *  registration / option-table / init-rejection contract.
 */

#include "tiny_ai_test_template.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe, and this file mirrors
 * the C spelling of the surface it exercises. ADR-1138. */

/* The registration tests set the model-path environment variable they
 * exercise; the binary is single-threaded (ADR-0141). */
/* NOLINTNEXTLINE(concurrency-mt-unsafe) */
VMAF_TINY_AI_DEFINE_REGISTRATION_TESTS("lpips", "lpips", "VMAF_LPIPS_MODEL_PATH", lpips)

char *run_tests(void)
{
    VMAF_TINY_AI_RUN_REGISTRATION_TESTS(lpips);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
