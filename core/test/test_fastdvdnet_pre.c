/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  FastDVDnet temporal pre-filter (T6-7) — structural + stub-path tests.
 *  Body delegated to `tiny_ai_test_template.h`. Full end-to-end
 *  inference is exercised by the CLI smoke gate against
 *  model/tiny/fastdvdnet_pre.onnx.
 */

#include "tiny_ai_test_template.h"

VMAF_TINY_AI_DEFINE_REGISTRATION_TESTS("fastdvdnet_pre", "fastdvdnet_pre_l1_residual",
                                       "VMAF_FASTDVDNET_PRE_MODEL_PATH", fastdvdnet_pre)

char *run_tests(void)
{
    VMAF_TINY_AI_RUN_REGISTRATION_TESTS(fastdvdnet_pre);
    return NULL;
}
