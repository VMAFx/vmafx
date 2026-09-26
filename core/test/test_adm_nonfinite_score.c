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

    const int nan_hm_err = vmaf_adm3_score(NAN, 0.5, 1, 0.5, 0.0, &adm3);
    mu_assert("NaN ADM3 harmonic mean input must fail", nan_hm_err != 0);
    mu_assert("failed NaN ADM3 harmonic mean calculation must not modify output", adm3 == 44.0);

    const int inf_err = vmaf_adm3_score(INFINITY, 0.5, 1, 0.5, 0.0, &adm3);
    mu_assert("infinite ADM3 input must fail", inf_err != 0);
    mu_assert("failed infinite ADM3 calculation must not modify output", adm3 == 44.0);
    return NULL;
}

static char *test_integer_adm_production_emitter_rejects_atomically(void)
{
    double adm3 = 44.0;
    const int err_adm3 = vmaf_adm3_score_named("integer_adm", 10u, NAN, 0.5, 0, 0.5, 0.0, &adm3);
    mu_assert("integer_adm non-finite ADM3 input must fail with EINVAL", err_adm3 == -EINVAL);
    mu_assert("failed integer_adm ADM3 calculation must not modify output", adm3 == 44.0);

    VmafFeatureCollector *feature_collector = NULL;
    mu_assert("collector initialises", vmaf_feature_collector_init(&feature_collector) == 0);

    const VmafNamedScore integer_scores[] = {
        {"VMAF_integer_feature_adm2_score", 0.75},
        {"VMAF_integer_feature_aim_score", 0.8},
        {"VMAF_integer_feature_adm3_score", NAN},
    };
    const int err =
        vmaf_feature_emit_finite_scores(feature_collector, NULL, "integer_adm", integer_scores,
                                        sizeof(integer_scores) / sizeof(integer_scores[0]), 10u);
    mu_assert("non-finite integer_adm emit fails with EINVAL", err == -EINVAL);

    double published = 0.0;
    mu_assert("integer_adm score was not partially published",
              vmaf_feature_collector_get_score(feature_collector, "VMAF_integer_feature_adm2_score",
                                               &published, 10u) != 0);
    vmaf_feature_collector_destroy(feature_collector);
    return NULL;
}

static char *test_adm_nonfinite_reduction_is_rejected_before_floor(void)
{
    double num = 42.0;
    double den = 43.0;
    int err = vmaf_adm_floor_pair_named("adm_test", 6u, -INFINITY, 1.0, 1e-10, &num, &den);
    mu_assert("negative infinity must not be floored to zero", err == -EINVAL);
    mu_assert("failed floor leaves both outputs unchanged", num == 42.0 && den == 43.0);

    err = vmaf_adm_floor_pair_named("adm_test", 7u, 1.0, NAN, 1e-10, &num, &den);
    mu_assert("NaN denominator must fail before comparison", err == -EINVAL);
    mu_assert("failed denominator floor leaves both outputs unchanged", num == 42.0 && den == 43.0);
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
    return NULL;
}

static char *test_flat_adm_scores_preserve_defined_results(void)
{
    double score = 0.0;
    double score_aim = 0.0;
    int err = vmaf_adm_finalize_scores(0.0, 0.0, 0.0, 0.0, &score, &score_aim);
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

    const VmafVifScoreSet scores = {
        .scale = {1.0, 1.0, NAN, 1.0, 1.0, 1.0, 1.0, 1.0},
        .minimum = {0.25, 0.25, 0.25},
        .use_minimums = true,
    };
    const int err = vmaf_vif_emit_scores(feature_collector, NULL, "test_vif", &scores,
                                         VMAF_VIF_FLOAT_NAMES, 11u);
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

    const VmafVifScoreSet scores = {
        .scale = {NAN, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0},
    };
    const int err = vmaf_vif_emit_scores(feature_collector, NULL, "test_vif", &scores,
                                         VMAF_VIF_FLOAT_NAMES, 12u);
    mu_assert("non-finite VIF scale zero fails with EINVAL", err == -EINVAL);

    double published = 0.0;
    mu_assert("VIF scale zero was not published",
              vmaf_feature_collector_get_score(feature_collector, "VMAF_feature_vif_scale0_score",
                                               &published, 12u) != 0);
    vmaf_feature_collector_destroy(feature_collector);
    return NULL;
}

static char *test_vif_scale_two_rejects_without_publication(void)
{
    VmafFeatureCollector *feature_collector = NULL;
    mu_assert("collector initialises", vmaf_feature_collector_init(&feature_collector) == 0);

    const VmafVifScoreSet scores = {
        .scale = {1.0, 1.0, 1.0, 1.0, NAN, 1.0, 1.0, 1.0},
        .minimum = {0.25, 0.25, 0.25},
        .use_minimums = true,
    };
    const int err = vmaf_vif_emit_scores(feature_collector, NULL, "test_vif", &scores,
                                         VMAF_VIF_FLOAT_NAMES, 27u);
    mu_assert("non-finite VIF scale two fails with EINVAL", err == -EINVAL);

    double published = 0.0;
    mu_assert("VIF scale zero was not published on scale 2 failure",
              vmaf_feature_collector_get_score(feature_collector, "VMAF_feature_vif_scale0_score",
                                               &published, 27u) != 0);
    vmaf_feature_collector_destroy(feature_collector);
    return NULL;
}

static char *test_vif_scale_three_rejects_without_publication(void)
{
    VmafFeatureCollector *feature_collector = NULL;
    mu_assert("collector initialises", vmaf_feature_collector_init(&feature_collector) == 0);

    const VmafVifScoreSet scores = {
        .scale = {1.0, 1.0, 1.0, 1.0, 1.0, 1.0, NAN, 1.0},
        .minimum = {0.25, 0.25, 0.25},
        .use_minimums = true,
    };
    const int err = vmaf_vif_emit_scores(feature_collector, NULL, "test_vif", &scores,
                                         VMAF_VIF_FLOAT_NAMES, 28u);
    mu_assert("non-finite VIF scale three fails with EINVAL", err == -EINVAL);

    double published = 0.0;
    mu_assert("VIF scale zero was not published on scale 3 failure",
              vmaf_feature_collector_get_score(feature_collector, "VMAF_feature_vif_scale0_score",
                                               &published, 28u) != 0);
    vmaf_feature_collector_destroy(feature_collector);
    return NULL;
}

static char *test_vif_complete_production_emit_rejects_debug_atom_atomically(void)
{
    VmafFeatureCollector *feature_collector = NULL;
    mu_assert("collector initialises", vmaf_feature_collector_init(&feature_collector) == 0);

    const VmafVifScoreSet scores = {
        .scale = {1.0, 1.0, 2.0, 2.0, 3.0, 3.0, 4.0, 4.0},
        .score = NAN,
        .score_num = 10.0,
        .score_den = 10.0,
        .debug = true,
    };
    const int err = vmaf_vif_emit_scores(feature_collector, NULL, "float_vif_test", &scores,
                                         VMAF_VIF_FLOAT_NAMES, 19u);
    mu_assert("non-finite VIF debug score fails with EINVAL", err == -EINVAL);

    double published = 0.0;
    mu_assert("VIF scale zero was not partially published",
              vmaf_feature_collector_get_score(feature_collector, "VMAF_feature_vif_scale0_score",
                                               &published, 19u) != 0);
    mu_assert("VIF debug score was not published",
              vmaf_feature_collector_get_score(feature_collector, "vif", &published, 19u) != 0);
    vmaf_feature_collector_destroy(feature_collector);
    return NULL;
}

static char *test_vif_complete_production_emit_rejects_invalid_denominator(void)
{
    VmafFeatureCollector *feature_collector = NULL;
    mu_assert("collector initialises", vmaf_feature_collector_init(&feature_collector) == 0);

    const VmafVifScoreSet scores = {
        .scale = {1.0, 1.0, 2.0, -1.0, 3.0, 3.0, 4.0, 4.0},
        .score = 1.0,
        .score_num = 10.0,
        .score_den = 7.0,
        .debug = true,
    };
    const int err = vmaf_vif_emit_scores(feature_collector, NULL, "integer_vif_test", &scores,
                                         VMAF_VIF_INTEGER_NAMES, 20u);
    mu_assert("negative VIF denominator fails with EINVAL", err == -EINVAL);

    double published = 0.0;
    mu_assert("integer VIF scale zero was not partially published",
              vmaf_feature_collector_get_score(feature_collector,
                                               "VMAF_integer_feature_vif_scale0_score", &published,
                                               20u) != 0);
    vmaf_feature_collector_destroy(feature_collector);
    return NULL;
}

static char *test_vif_complete_emitter_preserves_collector_ratio_precision(void)
{
    VmafFeatureCollector *feature_collector = NULL;
    mu_assert("collector initialises", vmaf_feature_collector_init(&feature_collector) == 0);

    VmafVifScoreSet scores = {
        .scale = {1.0, 3.0, 1.0, 3.0, 1.0, 3.0, 1.0, 3.0},
        .single_precision_ratio = true,
    };
    int err = vmaf_vif_emit_scores(feature_collector, NULL, "integer_vif_test", &scores,
                                   VMAF_VIF_INTEGER_NAMES, 24u);
    mu_assert("float-ratio integer VIF publish succeeds", err == 0);
    double published = 0.0;
    err = vmaf_feature_collector_get_score(
        feature_collector, "VMAF_integer_feature_vif_scale0_score", &published, 24u);
    mu_assert("integer VIF score is available", err == 0);
    mu_assert("CPU/CUDA/HIP integer VIF keeps float division",
              published == (double)((float)1.0 / (float)3.0));

    scores.single_precision_ratio = false;
    err = vmaf_vif_emit_scores(feature_collector, NULL, "integer_vif_sycl_test", &scores,
                               VMAF_VIF_INTEGER_NAMES, 25u);
    mu_assert("double-ratio integer VIF publish succeeds", err == 0);
    err = vmaf_feature_collector_get_score(
        feature_collector, "VMAF_integer_feature_vif_scale0_score", &published, 25u);
    mu_assert("SYCL/Metal integer VIF score is available", err == 0);
    mu_assert("SYCL/Metal integer VIF keeps double division", published == 1.0 / 3.0);

    vmaf_feature_collector_destroy(feature_collector);
    return NULL;
}

static char *test_ssim_production_emitter_rejects_without_publication(void)
{
    double prepared_score = 42.0;
    const int prep_err = vmaf_ssim_prepare_score(NAN, 1, 60.0, &prepared_score);
    mu_assert("vmaf_ssim_prepare_score rejects NaN dB raw score", prep_err == -EINVAL);
    mu_assert("failed SSIM prepare score leaves output unchanged", prepared_score == 42.0);

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

static char *test_ssim_ratio_production_emit_rejects_invalid_weight(void)
{
    VmafFeatureCollector *feature_collector = NULL;
    mu_assert("collector initialises", vmaf_feature_collector_init(&feature_collector) == 0);

    int err = vmaf_ssim_emit_ratio_score(feature_collector, NULL, "ssim_ratio_test", 1.0, NAN, 0,
                                         0.0, 21u);
    mu_assert("non-finite SSIM weight fails", err == -EINVAL);
    err = vmaf_ssim_emit_ratio_score(feature_collector, NULL, "ssim_ratio_test", 0.0, -1.0, 0, 0.0,
                                     22u);
    mu_assert("negative SSIM weight fails", err == -EINVAL);

    double published = 0.0;
    mu_assert("invalid ratio was not published",
              vmaf_feature_collector_get_score(feature_collector, "ssim_ratio_test", &published,
                                               21u) != 0);
    vmaf_feature_collector_destroy(feature_collector);
    return NULL;
}

static char *test_ms_ssim_hidden_atom_is_rejected_before_headline(void)
{
    VmafFeatureCollector *feature_collector = NULL;
    mu_assert("collector initialises", vmaf_feature_collector_init(&feature_collector) == 0);
    const double luminance[2] = {NAN, 1.0};
    const double contrast[2] = {1.0, 1.0};
    const double structure[2] = {1.0, 1.0};

    const int err =
        vmaf_ms_ssim_emit_scores(feature_collector, NULL, "ms_ssim_test", "float_ms_ssim", 1.0, 0,
                                 0.0, luminance, contrast, structure, 2u, false, 23u);
    mu_assert("hidden non-finite MS-SSIM atom fails", err == -EINVAL);
    double published = 0.0;
    mu_assert(
        "MS-SSIM headline was not partially published",
        vmaf_feature_collector_get_score(feature_collector, "float_ms_ssim", &published, 23u) != 0);
    vmaf_feature_collector_destroy(feature_collector);
    return NULL;
}

static char *test_ms_ssim_db_conversion_rejects_nonfinite_raw_score(void)
{
    VmafFeatureCollector *feature_collector = NULL;
    mu_assert("collector initialises", vmaf_feature_collector_init(&feature_collector) == 0);
    const double luminance[2] = {1.0, 1.0};
    const double contrast[2] = {1.0, 1.0};
    const double structure[2] = {1.0, 1.0};

    const int err =
        vmaf_ms_ssim_emit_scores(feature_collector, NULL, "float_ms_ssim", "float_ms_ssim", NAN, 1,
                                 60.0, luminance, contrast, structure, 2u, false, 29u);
    mu_assert("non-finite MS-SSIM dB raw score fails with EINVAL", err == -EINVAL);

    double published = 0.0;
    mu_assert(
        "MS-SSIM headline was not published on non-finite dB conversion",
        vmaf_feature_collector_get_score(feature_collector, "float_ms_ssim", &published, 29u) != 0);
    vmaf_feature_collector_destroy(feature_collector);
    return NULL;
}

static char *run_adm_tests(void)
{
    mu_run_test(test_undefined_aim_ratio_is_rejected_atomically);
    mu_run_test(test_nonfinite_adm3_is_rejected_atomically);
    mu_run_test(test_integer_adm_production_emitter_rejects_atomically);
    mu_run_test(test_adm_nonfinite_reduction_is_rejected_before_floor);
    mu_run_test(test_finite_scores_preserve_existing_results);
    mu_run_test(test_flat_adm_scores_preserve_defined_results);
    mu_run_test(test_adm_scale_ratios_define_flat_scale);
    mu_run_test(test_production_emitter_rejects_without_publication);
    return NULL;
}

static char *run_vif_tests(void)
{
    mu_run_test(test_vif_production_emitter_rejects_atomically);
    mu_run_test(test_vif_scale_zero_rejects_without_publication);
    mu_run_test(test_vif_scale_two_rejects_without_publication);
    mu_run_test(test_vif_scale_three_rejects_without_publication);
    mu_run_test(test_vif_complete_production_emit_rejects_debug_atom_atomically);
    mu_run_test(test_vif_complete_production_emit_rejects_invalid_denominator);
    mu_run_test(test_vif_complete_emitter_preserves_collector_ratio_precision);
    return NULL;
}

static char *run_ssim_tests(void)
{
    mu_run_test(test_ssim_production_emitter_rejects_without_publication);
    mu_run_test(test_ssim_invalid_db_ceiling_rejects_without_publication);
    mu_run_test(test_ssim_unclipped_perfect_score_preserves_positive_infinity);
    mu_run_test(test_ssim_infinity_exception_does_not_admit_failed_inputs);
    mu_run_test(test_ssim_ratio_production_emit_rejects_invalid_weight);
    mu_run_test(test_ms_ssim_hidden_atom_is_rejected_before_headline);
    mu_run_test(test_ms_ssim_db_conversion_rejects_nonfinite_raw_score);
    return NULL;
}

char *run_tests(void)
{
    mu_assert_msg(run_adm_tests());
    mu_assert_msg(run_vif_tests());
    mu_assert_msg(run_ssim_tests());
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
