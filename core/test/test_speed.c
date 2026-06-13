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
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("speed_chroma");
    mu_assert("speed_chroma extractor must be registered by name", fex != NULL);
    mu_assert("registered extractor has wrong name", !strcmp(fex->name, "speed_chroma"));
    mu_assert("speed_chroma.init must be set", fex->init != NULL);
    mu_assert("speed_chroma.extract must be set", fex->extract != NULL);
    mu_assert("speed_chroma.close must be set", fex->close != NULL);
    mu_assert("speed_chroma.priv_size must be non-zero", fex->priv_size > 0u);
    return NULL;
}

static char *test_speed_chroma_provided_features_well_formed(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("speed_chroma");
    mu_assert("speed_chroma must resolve before checking provided_features", fex != NULL);
    mu_assert("speed_chroma must publish a provided_features[] table",
              fex->provided_features != NULL);
    mu_assert("speed_chroma.provided_features[0] must be non-NULL",
              fex->provided_features[0] != NULL);

    VmafFeatureExtractor *via_feature =
        vmaf_get_feature_extractor_by_feature_name(fex->provided_features[0], 0);
    mu_assert("speed_chroma's first provided feature must round-trip to the extractor",
              via_feature != NULL);
    mu_assert("round-trip feature lookup must point at speed_chroma",
              !strcmp(via_feature->name, "speed_chroma"));
    return NULL;
}

static char *test_speed_temporal_is_registered(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("speed_temporal");
    mu_assert("speed_temporal extractor must be registered by name", fex != NULL);
    mu_assert("registered extractor has wrong name", !strcmp(fex->name, "speed_temporal"));
    mu_assert("speed_temporal.init must be set", fex->init != NULL);
    mu_assert("speed_temporal.extract must be set", fex->extract != NULL);
    mu_assert("speed_temporal.close must be set", fex->close != NULL);
    mu_assert("speed_temporal.priv_size must be non-zero", fex->priv_size > 0u);
    return NULL;
}

static char *test_speed_temporal_provided_features_well_formed(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("speed_temporal");
    mu_assert("speed_temporal must resolve before checking provided_features", fex != NULL);
    mu_assert("speed_temporal must publish a provided_features[] table",
              fex->provided_features != NULL);
    mu_assert("speed_temporal.provided_features[0] must be non-NULL",
              fex->provided_features[0] != NULL);

    VmafFeatureExtractor *via_feature =
        vmaf_get_feature_extractor_by_feature_name(fex->provided_features[0], 0);
    mu_assert("speed_temporal's first provided feature must round-trip to the extractor",
              via_feature != NULL);
    mu_assert("round-trip feature lookup must point at speed_temporal",
              !strcmp(via_feature->name, "speed_temporal"));
    return NULL;
}

static char *test_speed_options_tables_well_formed(void)
{
    VmafFeatureExtractor *fex_chroma = vmaf_get_feature_extractor_by_name("speed_chroma");
    mu_assert("speed_chroma must resolve before option-table check", fex_chroma != NULL);
    mu_assert("speed_chroma must publish an options table", fex_chroma->options != NULL);
    for (const VmafOption *opt = fex_chroma->options; opt->name != NULL; ++opt) {
        mu_assert("speed_chroma option must have a help string", opt->help != NULL);
    }

    VmafFeatureExtractor *fex_temporal = vmaf_get_feature_extractor_by_name("speed_temporal");
    mu_assert("speed_temporal must resolve before option-table check", fex_temporal != NULL);
    mu_assert("speed_temporal must publish an options table", fex_temporal->options != NULL);
    for (const VmafOption *opt = fex_temporal->options; opt->name != NULL; ++opt) {
        mu_assert("speed_temporal option must have a help string", opt->help != NULL);
    }
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_speed_chroma_is_registered);
    mu_run_test(test_speed_chroma_provided_features_well_formed);
    mu_run_test(test_speed_temporal_is_registered);
    mu_run_test(test_speed_temporal_provided_features_well_formed);
    mu_run_test(test_speed_options_tables_well_formed);
    return NULL;
}
