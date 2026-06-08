/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
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

VMAF_TINY_AI_DEFINE_REGISTRATION_TESTS("lpips", "lpips", "VMAF_LPIPS_MODEL_PATH", lpips)

char *run_tests(void)
{
    VMAF_TINY_AI_RUN_REGISTRATION_TESTS(lpips);
    return NULL;
}
