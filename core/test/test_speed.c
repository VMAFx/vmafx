/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  speed_chroma + speed_temporal feature extractors —
 *  registration / wiring smoke tests.
 *
 *  The metric internals (Gaussian kernels, prescale resampling, NN
 *  weighting, temporal accumulation) are exercised end-to-end by the
 *  upstream Netflix test corpus that this commit ports.  These unit
 *  tests catch the regressions that are easy to introduce when porting
 *  a new feature extractor onto a fork that already carries SIMD / GPU
 *  variants of neighbouring metrics:
 *
 *    - the "speed_chroma" and "speed_temporal" extractors are
 *      discoverable by name (registration wired into
 *      feature_extractor_list[]);
 *    - their VTable entries (init / extract / close) are non-NULL;
 *    - priv_size is non-zero so the framework allocates state;
 *    - provided_features[] is well-formed (non-NULL strings) and the
 *      first entry resolves back to the same extractor via
 *      vmaf_get_feature_extractor_by_feature_name.
 */



#include <string.h>

#include "test.h"

#include "feature/feature_extractor.h"

static char *test_speed_chroma_is_registered(void)
{
    const VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("speed_chroma");
    mu_assert("speed_chroma extractor must be registered by name", fex != VMAF_NULLPTR);
    mu_assert("registered extractor has wrong name", !strcmp(fex->name, "speed_chroma"));
    mu_assert("speed_chroma.init must be set", fex->init != VMAF_NULLPTR);
    mu_assert("speed_chroma.extract must be set", fex->extract != VMAF_NULLPTR);
    mu_assert("speed_chroma.close must be set", fex->close != VMAF_NULLPTR);
    mu_assert("speed_chroma.priv_size must be non-zero", fex->priv_size > 0u);
    return VMAF_NULLPTR;
}

static char *test_speed_chroma_provided_features_well_formed(void)
{
    const VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("speed_chroma");
    mu_assert("speed_chroma must resolve before checking provided_features", fex != VMAF_NULLPTR);
    mu_assert("speed_chroma must publish a provided_features[] table",
              fex->provided_features != VMAF_NULLPTR);
    mu_assert("speed_chroma.provided_features[0] must be non-NULL",
              fex->provided_features[0] != VMAF_NULLPTR);

    const VmafFeatureExtractor *via_feature =
        vmaf_get_feature_extractor_by_feature_name(fex->provided_features[0], 0);
    mu_assert("speed_chroma's first provided feature must round-trip to the extractor",
              via_feature != VMAF_NULLPTR);
    mu_assert("round-trip feature lookup must point at speed_chroma",
              !strcmp(via_feature->name, "speed_chroma"));
    return VMAF_NULLPTR;
}

static char *test_speed_temporal_is_registered(void)
{
    const VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("speed_temporal");
    mu_assert("speed_temporal extractor must be registered by name", fex != VMAF_NULLPTR);
    mu_assert("registered extractor has wrong name", !strcmp(fex->name, "speed_temporal"));
    mu_assert("speed_temporal.init must be set", fex->init != VMAF_NULLPTR);
    mu_assert("speed_temporal.extract must be set", fex->extract != VMAF_NULLPTR);
    mu_assert("speed_temporal.close must be set", fex->close != VMAF_NULLPTR);
    mu_assert("speed_temporal.priv_size must be non-zero", fex->priv_size > 0u);
    return VMAF_NULLPTR;
}

static char *test_speed_temporal_provided_features_well_formed(void)
{
    const VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("speed_temporal");
    mu_assert("speed_temporal must resolve before checking provided_features", fex != VMAF_NULLPTR);
    mu_assert("speed_temporal must publish a provided_features[] table",
              fex->provided_features != VMAF_NULLPTR);
    mu_assert("speed_temporal.provided_features[0] must be non-NULL",
              fex->provided_features[0] != VMAF_NULLPTR);

    const VmafFeatureExtractor *via_feature =
        vmaf_get_feature_extractor_by_feature_name(fex->provided_features[0], 0);
    mu_assert("speed_temporal's first provided feature must round-trip to the extractor",
              via_feature != VMAF_NULLPTR);
    mu_assert("round-trip feature lookup must point at speed_temporal",
              !strcmp(via_feature->name, "speed_temporal"));
    return VMAF_NULLPTR;
}

static char *test_speed_options_tables_well_formed(void)
{
    const VmafFeatureExtractor *fex_chroma = vmaf_get_feature_extractor_by_name("speed_chroma");
    mu_assert("speed_chroma must resolve before option-table check", fex_chroma != VMAF_NULLPTR);
    mu_assert("speed_chroma must publish an options table", fex_chroma->options != VMAF_NULLPTR);
    for (const VmafOption *opt = fex_chroma->options; opt->name != VMAF_NULLPTR; ++opt) {
        mu_assert("speed_chroma option must have a help string", opt->help != VMAF_NULLPTR);
    }

    const VmafFeatureExtractor *fex_temporal = vmaf_get_feature_extractor_by_name("speed_temporal");
    mu_assert("speed_temporal must resolve before option-table check", fex_temporal != VMAF_NULLPTR);
    mu_assert("speed_temporal must publish an options table", fex_temporal->options != VMAF_NULLPTR);
    for (const VmafOption *opt = fex_temporal->options; opt->name != VMAF_NULLPTR; ++opt) {
        mu_assert("speed_temporal option must have a help string", opt->help != VMAF_NULLPTR);
    }
    return VMAF_NULLPTR;
}

char *run_tests(void)
{
    mu_run_test(test_speed_chroma_is_registered);
    mu_run_test(test_speed_chroma_provided_features_well_formed);
    mu_run_test(test_speed_temporal_is_registered);
    mu_run_test(test_speed_temporal_provided_features_well_formed);
    mu_run_test(test_speed_options_tables_well_formed);
    return VMAF_NULLPTR;
}
