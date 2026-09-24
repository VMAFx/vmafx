/**
 * Copyright 2026 Lusoris
 * SPDX-License-Identifier: EUPL-1.2
 */

#include <math.h>

#include "feature/adm_score.h"
#include "feature/nonfinite_score.h"
#include "test.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit; MSVC's documented
 * /std:clatest C23 surface does not include nullptr (ADR-1138). */

static char *test_undefined_aim_ratio_is_rejected_atomically(void)
{
    double score = 42.0;
    double score_aim = 43.0;

    const int err = vmaf_adm_finalize_scores(1.0, 1.0, 1.0, 0.0, &score, &score_aim);

    mu_assert("undefined AIM ratio must fail", err != 0);
    mu_assert("failed finalization must not modify outputs", score == 42.0 && score_aim == 43.0);
    return NULL;
}

static char *test_nonfinite_adm3_is_rejected_atomically(void)
{
    double adm3 = 44.0;

    const int err = vmaf_adm3_score(NAN, 0.5, 0, 0.5, 0.0, &adm3);

    mu_assert("non-finite ADM3 input must fail", err != 0);
    mu_assert("failed ADM3 calculation must not modify output", adm3 == 44.0);

    const int inf_err = vmaf_adm3_score(INFINITY, 0.5, 1, 0.5, 0.0, &adm3);
    mu_assert("infinite ADM3 input must fail", inf_err != 0);
    mu_assert("failed infinite ADM3 calculation must not modify output", adm3 == 44.0);
    return NULL;
}

static char *test_finite_scores_preserve_existing_results(void)
{
    double score = 0.0;
    double score_aim = 0.0;
    double adm3 = -1.0;

    int err = vmaf_adm_finalize_scores(1.0, 2.0, 3.0, 4.0, &score, &score_aim);
    mu_assert("finite ADM finalization succeeds", err == 0);
    mu_assert("finite ADM ratios are unchanged", score == 0.5 && score_aim == 0.75);

    err = vmaf_adm3_score(0.0, 0.0, 1, 0.5, 0.0, &adm3);
    mu_assert("zero harmonic mean succeeds", err == 0);
    mu_assert("zero harmonic mean stays zero", adm3 == 0.0);

    score = 0.0;
    score_aim = 0.0;
    err = vmaf_adm_finalize_scores(0.0, 0.0, 0.0, 0.0, &score, &score_aim);
    mu_assert("finite flat-frame ADM finalization succeeds", err == 0);
    mu_assert("finite flat frame remains perfect", score == 1.0 && score_aim == 1.0);

    err = vmaf_adm_finalize_scores(0.0, 0.0, 1.0, 2.0, &score, &score_aim);
    mu_assert("ADM and AIM denominators are handled independently", err == 0);
    mu_assert("flat ADM does not override finite AIM", score == 1.0 && score_aim == 0.5);

    err = vmaf_adm_finalize_scores(1.0, 2.0, 0.0, 0.0, &score, &score_aim);
    mu_assert("flat AIM succeeds independently", err == 0);
    mu_assert("flat AIM does not override finite ADM", score == 0.5 && score_aim == 1.0);
    return NULL;
}

static char *test_adm_scale_ratios_define_flat_scale(void)
{
    const double scores[] = {0.0, 0.0, 1.0, 2.0};
    double ratios[] = {42.0, 43.0};

    int err = vmaf_adm_scale_ratios(scores, 2u, ratios);
    mu_assert("finite flat scale ratios succeed", err == 0);
    mu_assert("flat scale is perfect and finite scale is unchanged",
              ratios[0] == 1.0 && ratios[1] == 0.5);

    const double undefined_scores[] = {1.0, 2.0, 1.0, 0.0};
    ratios[0] = 44.0;
    ratios[1] = 45.0;
    err = vmaf_adm_scale_ratios(undefined_scores, 2u, ratios);
    mu_assert("nonzero-over-zero scale ratio fails", err == -EINVAL);
    mu_assert("failed scale ratio leaves every output unchanged",
              ratios[0] == 44.0 && ratios[1] == 45.0);
    return NULL;
}

static char *test_production_emitter_rejects_without_publication(void)
{
    VmafFeatureCollector *feature_collector = NULL;
    mu_assert("collector initialises", vmaf_feature_collector_init(&feature_collector) == 0);

    const VmafNamedScore scores[] = {
        {"VMAF_feature_adm2_score", 0.75},
        {"VMAF_feature_adm3_score", NAN},
    };
    const int err = vmaf_feature_emit_finite_scores(feature_collector, NULL, "test_adm", scores,
                                                    sizeof(scores) / sizeof(scores[0]), 7u);
    mu_assert("non-finite production emit fails with EINVAL", err == -EINVAL);

    double published = 0.0;
    mu_assert("first score was not partially published",
              vmaf_feature_collector_get_score(feature_collector, "VMAF_feature_adm2_score",
                                               &published, 7u) != 0);
    mu_assert("non-finite score was not published",
              vmaf_feature_collector_get_score(feature_collector, "VMAF_feature_adm3_score",
                                               &published, 7u) != 0);
    vmaf_feature_collector_destroy(feature_collector);
    return NULL;
}

static char *test_vif_production_emitter_rejects_atomically(void)
{
    VmafFeatureCollector *feature_collector = NULL;
    mu_assert("collector initialises", vmaf_feature_collector_init(&feature_collector) == 0);

    const double scores[8] = {1.0, 1.0, NAN, 1.0, 1.0, 1.0, 1.0, 1.0};
    const int err =
        vmaf_vif_emit_scale_scores(feature_collector, NULL, "test_vif", scores, 0, NULL, 11u);
    mu_assert("non-finite VIF scale fails with EINVAL", err == -EINVAL);

    double published = 0.0;
    mu_assert("VIF scale zero was not partially published",
              vmaf_feature_collector_get_score(feature_collector, "VMAF_feature_vif_scale0_score",
                                               &published, 11u) != 0);
    vmaf_feature_collector_destroy(feature_collector);
    return NULL;
}

static char *test_vif_scale_zero_rejects_without_publication(void)
{
    VmafFeatureCollector *feature_collector = NULL;
    mu_assert("collector initialises", vmaf_feature_collector_init(&feature_collector) == 0);

    const double scores[8] = {NAN, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0};
    const int err =
        vmaf_vif_emit_scale_scores(feature_collector, NULL, "test_vif", scores, 0, NULL, 12u);
    mu_assert("non-finite VIF scale zero fails with EINVAL", err == -EINVAL);

    double published = 0.0;
    mu_assert("VIF scale zero was not published",
              vmaf_feature_collector_get_score(feature_collector, "VMAF_feature_vif_scale0_score",
                                               &published, 12u) != 0);
    vmaf_feature_collector_destroy(feature_collector);
    return NULL;
}

static char *test_ssim_production_emitter_rejects_without_publication(void)
{
    VmafFeatureCollector *feature_collector = NULL;
    mu_assert("collector initialises", vmaf_feature_collector_init(&feature_collector) == 0);

    const int err = vmaf_ssim_emit_score(feature_collector, NULL, "ssim", NAN, 1, 60.0, 13u);
    mu_assert("non-finite SSIM fails with EINVAL", err == -EINVAL);

    double published = 0.0;
    mu_assert("SSIM was not published",
              vmaf_feature_collector_get_score(feature_collector, "ssim", &published, 13u) != 0);
    vmaf_feature_collector_destroy(feature_collector);
    return NULL;
}

static char *test_ssim_invalid_db_ceiling_rejects_without_publication(void)
{
    VmafFeatureCollector *feature_collector = NULL;
    mu_assert("collector initialises", vmaf_feature_collector_init(&feature_collector) == 0);

    int err = vmaf_ssim_emit_score(feature_collector, NULL, "ssim", 1.0, 1, NAN, 14u);
    mu_assert("NaN SSIM dB ceiling fails with EINVAL", err == -EINVAL);
    err = vmaf_ssim_emit_score(feature_collector, NULL, "ssim", 1.0, 1, -INFINITY, 15u);
    mu_assert("negative-infinite SSIM dB ceiling fails with EINVAL", err == -EINVAL);

    double published = 0.0;
    mu_assert("NaN-ceiling SSIM was not published",
              vmaf_feature_collector_get_score(feature_collector, "ssim", &published, 14u) != 0);
    mu_assert("negative-infinite-ceiling SSIM was not published",
              vmaf_feature_collector_get_score(feature_collector, "ssim", &published, 15u) != 0);
    vmaf_feature_collector_destroy(feature_collector);
    return NULL;
}

static char *test_ssim_unclipped_perfect_score_preserves_positive_infinity(void)
{
    VmafFeatureCollector *feature_collector = NULL;
    mu_assert("collector initialises", vmaf_feature_collector_init(&feature_collector) == 0);

    const int err = vmaf_ssim_emit_score(feature_collector, NULL, "ssim", 1.0, 1, INFINITY, 16u);
    mu_assert("documented unclipped perfect SSIM succeeds", err == 0);

    double published = 0.0;
    mu_assert("unclipped perfect SSIM was published",
              vmaf_feature_collector_get_score(feature_collector, "ssim", &published, 16u) == 0);
    mu_assert("unclipped perfect SSIM retains its positive-infinity sentinel",
              isinf(published) && published > 0.0);
    vmaf_feature_collector_destroy(feature_collector);
    return NULL;
}

static char *test_ssim_infinity_exception_does_not_admit_failed_inputs(void)
{
    VmafFeatureCollector *feature_collector = NULL;
    mu_assert("collector initialises", vmaf_feature_collector_init(&feature_collector) == 0);

    int err = vmaf_ssim_emit_score(feature_collector, NULL, "ssim", INFINITY, 1, INFINITY, 17u);
    mu_assert("infinite raw SSIM is not an unclipped-perfect sentinel", err == -EINVAL);

    const VmafNamedScore atoms[] = {{"float_ssim_l", NAN}};
    err = vmaf_ssim_emit_scores(feature_collector, NULL, "float_ssim", 1.0, 1, INFINITY, atoms,
                                sizeof(atoms) / sizeof(atoms[0]), 18u);
    mu_assert("unclipped-perfect SSIM does not bypass non-finite atom validation", err == -EINVAL);

    double published = 0.0;
    mu_assert("infinite raw SSIM was not published",
              vmaf_feature_collector_get_score(feature_collector, "ssim", &published, 17u) != 0);
    mu_assert("SSIM family was not partially published",
              vmaf_feature_collector_get_score(feature_collector, "float_ssim", &published, 18u) !=
                  0);
    vmaf_feature_collector_destroy(feature_collector);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_undefined_aim_ratio_is_rejected_atomically);
    mu_run_test(test_nonfinite_adm3_is_rejected_atomically);
    mu_run_test(test_finite_scores_preserve_existing_results);
    mu_run_test(test_adm_scale_ratios_define_flat_scale);
    mu_run_test(test_production_emitter_rejects_without_publication);
    mu_run_test(test_vif_production_emitter_rejects_atomically);
    mu_run_test(test_vif_scale_zero_rejects_without_publication);
    mu_run_test(test_ssim_production_emitter_rejects_without_publication);
    mu_run_test(test_ssim_invalid_db_ceiling_rejects_without_publication);
    mu_run_test(test_ssim_unclipped_perfect_score_preserves_positive_infinity);
    mu_run_test(test_ssim_infinity_exception_does_not_admit_failed_inputs);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
