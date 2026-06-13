/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  TransNet V2 shot-boundary detector (T6-3a) — structural + stub-path
 *  tests. The four standard tiny-AI registration tests come from the
 *  shared `tiny_ai_test_template.h` macro; this file adds two
 *  extractor-specific tests:
 *
 *    - the second provided-feature name (`shot_boundary` binary flag)
 *      round-trips to the extractor, mirroring the primary
 *      `shot_boundary_probability` round-trip the macro emits;
 *    - the `provided_features` array is NULL-terminated and contains
 *      both surface names so the per-shot CRF predictor (T6-3b) can
 *      discover the surface by name.
 *
 *  Full end-to-end inference (100-frame window → ORT run → two scores
 *  per frame) requires a real ORT build linked against the placeholder
 *  ONNX shipped under `model/tiny/transnet_v2.onnx`; that path is
 *  exercised by the CLI smoke gate, not by this unit test.
 */

#include <string.h>

#include "tiny_ai_test_template.h"

#include "feature/feature_extractor.h"

VMAF_TINY_AI_DEFINE_REGISTRATION_TESTS("transnet_v2", "shot_boundary_probability",
                                       "VMAF_TRANSNET_V2_MODEL_PATH", transnet_v2)

static char *test_transnet_v2_provides_binary_flag_feature(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_feature_name("shot_boundary", 0);
    mu_assert("'shot_boundary' feature name must resolve to an extractor", fex != NULL);
    mu_assert("'shot_boundary' must map to the transnet_v2 extractor",
              !strcmp(fex->name, "transnet_v2"));
    return NULL;
}

static char *test_transnet_v2_provided_features_list_terminated(void)
{
    /* The provided_features array must be NULL-terminated and contain
     * both feature names so the per-shot CRF predictor (T6-3b) can
     * discover the surface by name. */
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("transnet_v2");
    mu_assert("transnet_v2 extractor missing", fex != NULL);
    mu_assert("provided_features must be set", fex->provided_features != NULL);

    int saw_prob = 0;
    int saw_flag = 0;
    unsigned count = 0u;
    for (const char *const *p = fex->provided_features; *p; ++p) {
        if (!strcmp(*p, "shot_boundary_probability"))
            saw_prob = 1;
        if (!strcmp(*p, "shot_boundary"))
            saw_flag = 1;
        count += 1u;
        mu_assert("runaway provided_features (missing NULL terminator?)", count < 16u);
    }
    mu_assert("provided_features must contain shot_boundary_probability", saw_prob);
    mu_assert("provided_features must contain shot_boundary", saw_flag);
    return NULL;
}

char *run_tests(void)
{
    VMAF_TINY_AI_RUN_REGISTRATION_TESTS(transnet_v2);
    mu_run_test(test_transnet_v2_provides_binary_flag_feature);
    mu_run_test(test_transnet_v2_provided_features_list_terminated);
    return NULL;
}
