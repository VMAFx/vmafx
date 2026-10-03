/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * The option tables, flags and provided features of every Metal twin equal
 * their CPU extractor's (T-BUG048-GPU-OPTION-PARITY-REMAINDER-2026-09-26: the
 * row closes when the Metal option tables equal the CPU's; the provided-feature
 * halves of T-GPU-FLOAT-MOTION3-MISSING-2026-09-30 and
 * T-GPU-MOTION-SAD-SCORE-NOT-EMITTED-2026-10-02). The device-free checks of
 * test_cuda_twin_option_parity.c, test_hip_twin_option_parity.c and
 * test_sycl_twin_option_parity.c, for the seventeen Metal twins. There is
 * test_twin_<x> case per twin for options, flags and the unknown option, so
 * the report names the twin whose table differs, and a test_twin_<x>_provides
 * case for the provided features, which does not depend on the tables.
 *
 * Each test_twin_<x> case compares, for `cpu` and its Metal twin:
 *   - option tables, both directions: every CPU option is declared by the twin
 *     (test_twin_option_tables_match_cpu) and every twin option is a CPU
 *     option (test_twin_options_are_cpu_options), with the same name, alias,
 *     type, default, range and flags. An option the twin declares
 *     VMAF_OPT_FLAG_DEFAULT_ONLY because it implements the default only is a
 *     difference of flags and is reported: the row closes when the twin
 *     executes the option. The one accepted exception is float_ssim's
 *     `scale`: the Metal twin implements scale 1 only (ADR-1324, a separate
 *     row), so a DEFAULT_ONLY flag on that one option is ignored. The CPU
 *     float_ssim has no `enable_chroma`, so the CUDA twin's accepted no-op
 *     for it does not apply to Metal and is not allowed;
 *   - the ADR-1183 gate: a valid non-default value of every CPU option, by
 *     name and by alias, is honoured by the twin;
 *   - a CPU TEMPORAL extractor has a TEMPORAL twin (psnr's apsnr totals), and
 *     --subsample may skip a frame on both or neither (TEMPORAL or PREV_REF).
 *     The CPU motion extractors carry PREV_REF, not TEMPORAL, and every
 *     motion twin carries TEMPORAL, so strict TEMPORAL equality is not asked;
 *   - an unknown option is refused and named by the gate
 *     (test_twin_rejects_unknown_option).
 *
 * Each test_twin_<x>_provides case checks that the twin's provided_features
 * include every CPU provided feature (the motion twins:
 * VMAF_integer_feature_motion_sad_score and VMAF_feature_motion3_score).
 *
 * The cases need no device state, but they begin with metal_twin_have_device()
 * so the hosted macOS CI, which has none, skips them: today's twins fail them.
 * Under VMAF_METAL_TWIN_SELFTEST the CPU extractor stands in for the twin and
 * every case compares it with itself.
 */

#include "metal_twin.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dict.h"
#include "feature/feature_extractor.h"
#include "libvmaf/feature.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy proposes `nullptr`, but the required MSVC C build does
 * not provide that keyword. Preserve the portable C spelling. ADR-1138. */

#define NAME_LEN 64u
#define VALUE_LEN 48u

static const VmafOption *lookup_option(const VmafFeatureExtractor *fex, const char *key)
{
    for (unsigned i = 0; fex && fex->options && fex->options[i].name; i++) {
        const VmafOption *opt = &fex->options[i];
        if (!strcmp(opt->name, key) || (opt->alias && !strcmp(opt->alias, key))) {
            return opt;
        }
    }
    return NULL;
}

/* The flag bits that count as a difference for option `key` of `cpu`: all of
 * them, except float_ssim's `scale`, which the Metal twin implements for 1
 * only (ADR-1324). */
static uint64_t flags_compared(const char *cpu, const char *key)
{
    if (!strcmp(cpu, "float_ssim") && !strcmp(key, "scale")) {
        return ~(uint64_t)VMAF_OPT_FLAG_DEFAULT_ONLY;
    }
    return ~(uint64_t)0;
}

static bool same_default(const VmafOption *a, const VmafOption *b)
{
    switch (a->type) {
    case VMAF_OPT_TYPE_BOOL:
        return a->default_val.b == b->default_val.b;
    case VMAF_OPT_TYPE_INT:
        return a->default_val.i == b->default_val.i && a->min == b->min && a->max == b->max;
    case VMAF_OPT_TYPE_DOUBLE:
        return a->default_val.d == b->default_val.d && a->min == b->min && a->max == b->max;
    default:
        return (a->default_val.s == NULL) == (b->default_val.s == NULL) &&
               (!a->default_val.s || !strcmp(a->default_val.s, b->default_val.s));
    }
}

static bool same_option(const VmafOption *a, const VmafOption *b, uint64_t flag_mask)
{
    if (strcmp(a->name, b->name) != 0 || a->type != b->type) {
        return false;
    }
    if ((a->flags & flag_mask) != (b->flags & flag_mask)) {
        return false;
    }
    if ((a->alias == NULL) != (b->alias == NULL) || (a->alias && strcmp(a->alias, b->alias) != 0)) {
        return false;
    }
    return same_default(a, b);
}

/* ADR-1183 gate with a one-entry dictionary. The gate points the rejected
 * key into the dictionary, so it is copied to `unsupported` (empty when the
 * gate passes) before the dictionary is freed. */
static bool honours(const VmafFeatureExtractor *fex, const char *key, const char *value,
                    char unsupported[NAME_LEN])
{
    VmafDictionary *dict = NULL;
    unsupported[0] = '\0';
    if (vmaf_dictionary_set(&dict, key, value, 0)) {
        return false;
    }
    const char *rejected = NULL;
    const bool ok = vmaf_feature_extractor_honours_options(fex, dict, &rejected);
    if (rejected) {
        (void)snprintf(unsupported, NAME_LEN, "%s", rejected);
    }
    (void)vmaf_dictionary_free(&dict);
    return ok;
}

/* A valid value of `opt` other than its default, or false for a string. */
static bool non_default_value(const VmafOption *opt, char value[VALUE_LEN])
{
    switch (opt->type) {
    case VMAF_OPT_TYPE_BOOL:
        (void)snprintf(value, VALUE_LEN, "%s", opt->default_val.b ? "false" : "true");
        return true;
    case VMAF_OPT_TYPE_INT: {
        const int up = opt->default_val.i + 1;
        (void)snprintf(value, VALUE_LEN, "%d",
                       (double)up <= opt->max ? up : opt->default_val.i - 1);
        return true;
    }
    case VMAF_OPT_TYPE_DOUBLE: {
        const double up = opt->default_val.d + 0.5;
        (void)snprintf(value, VALUE_LEN, "%.17g", up <= opt->max ? up : opt->default_val.d - 0.5);
        return true;
    }
    default:
        return false;
    }
}

static unsigned count_options(const VmafFeatureExtractor *fex)
{
    unsigned n = 0u;
    while (fex->options && fex->options[n].name) {
        n++;
    }
    return n;
}

/* Options of `from` that `to` lacks or declares differently; `to_is_cpu`
 * says which side is the CPU extractor, for the report. */
static unsigned table_differences(const char *cpu_name, const VmafFeatureExtractor *from,
                                  const VmafFeatureExtractor *to, bool to_is_cpu)
{
    unsigned bad = 0u;
    for (unsigned i = 0; i < count_options(from); i++) {
        const VmafOption *a = &from->options[i];
        const VmafOption *b = lookup_option(to, a->name);
        if (b && same_option(a, b, flags_compared(cpu_name, a->name))) {
            continue;
        }
        bad++;
        (void)fprintf(stderr, "\n  option %s: %s declares it %s\n", a->name,
                      to_is_cpu ? "the twin" : "the CPU extractor",
                      b ? "differently from its counterpart" : "and its counterpart does not");
    }
    return bad;
}

/* The gate keeps no valid CPU option off the twin, by name and by alias. */
static unsigned gate_differences(const char *cpu_name, const VmafFeatureExtractor *cpu,
                                 const VmafFeatureExtractor *twin)
{
    unsigned bad = 0u;
    for (unsigned i = 0; i < count_options(cpu); i++) {
        const VmafOption *opt = &cpu->options[i];
        char value[VALUE_LEN];
        char unsupported[NAME_LEN];
        /* float_ssim's `scale`: the Metal twin implements scale 1 only
         * (ADR-1324, a separate row), as in flags_compared(). */
        if (flags_compared(cpu_name, opt->name) != ~(uint64_t)0 || !non_default_value(opt, value)) {
            continue;
        }
        const char *keys[2] = {opt->name, opt->alias};
        for (unsigned k = 0; k < 2u; k++) {
            if (keys[k] && !(honours(twin, keys[k], value, unsupported) && !unsupported[0])) {
                bad++;
                (void)fprintf(stderr, "\n  the gate keeps %s=%s off the twin\n", keys[k], value);
            }
        }
    }
    return bad;
}

/* Whether --subsample may skip a frame (fex_subsample_skip(): neither
 * TEMPORAL nor PREV_REF set). */
static bool sees_every_frame(const VmafFeatureExtractor *fex)
{
    return (fex->flags & (VMAF_FEATURE_EXTRACTOR_TEMPORAL | VMAF_FEATURE_EXTRACTOR_PREV_REF)) != 0;
}

static bool provides(const VmafFeatureExtractor *fex, const char *feature)
{
    for (size_t j = 0; fex->provided_features && fex->provided_features[j]; j++) {
        if (!strcmp(fex->provided_features[j], feature)) {
            return true;
        }
    }
    return false;
}

/* The TEMPORAL flag: a CPU extractor that has it implies the twin has it (the
 * apsnr totals of psnr need every frame), and --subsample treats both alike.
 * The CPU motion extractors carry PREV_REF, not TEMPORAL, while every motion
 * twin carries TEMPORAL, so strict TEMPORAL equality is not the contract. */
static unsigned flag_differences(const VmafFeatureExtractor *cpu, const VmafFeatureExtractor *twin)
{
    unsigned bad = 0u;
    if ((cpu->flags & VMAF_FEATURE_EXTRACTOR_TEMPORAL) &&
        !(twin->flags & VMAF_FEATURE_EXTRACTOR_TEMPORAL)) {
        bad++;
        (void)fprintf(stderr, "\n  the CPU is TEMPORAL and the twin is not\n");
    }
    if (sees_every_frame(cpu) != sees_every_frame(twin)) {
        bad++;
        (void)fprintf(stderr, "\n  --subsample skips frames on one side only\n");
    }
    return bad;
}

static unsigned feature_differences(const VmafFeatureExtractor *cpu,
                                    const VmafFeatureExtractor *twin)
{
    unsigned bad = 0u;
    for (size_t i = 0; cpu->provided_features && cpu->provided_features[i]; i++) {
        if (!provides(twin, cpu->provided_features[i])) {
            bad++;
            (void)fprintf(stderr, "\n  the twin does not provide %s\n", cpu->provided_features[i]);
        }
    }
    return bad;
}

static unsigned unknown_option_differences(const VmafFeatureExtractor *twin)
{
    char unsupported[NAME_LEN];
    if (honours(twin, "enable_frobnication", "true", unsupported)) {
        (void)fprintf(stderr, "\n  an unknown key does not keep the feature off the twin\n");
        return 1u;
    }
    if (strcmp(unsupported, "enable_frobnication") != 0) {
        (void)fprintf(stderr, "\n  the gate does not name the unknown key\n");
        return 1u;
    }
    return 0u;
}

/* All checks of one (cpu, twin) pair. */
static char *check_twin(const char *cpu_name, const char *twin_name)
{
    if (!metal_twin_have_device()) {
        return NULL;
    }
    const VmafFeatureExtractor *cpu = vmaf_get_feature_extractor_by_name(cpu_name);
    const VmafFeatureExtractor *twin = vmaf_get_feature_extractor_by_name(twin_name);
    mu_assert("the CPU extractor is not registered", cpu != NULL);
    mu_assert("the Metal twin is not registered", twin != NULL);
    unsigned bad = table_differences(cpu_name, cpu, twin, true);
    bad += table_differences(cpu_name, twin, cpu, false);
    bad += gate_differences(cpu_name, cpu, twin);
    bad += flag_differences(cpu, twin);
    bad += unknown_option_differences(twin);
    if (bad) {
        (void)fprintf(stderr, "\n%s: %u difference(s) from %s\n", twin_name, bad, cpu_name);
    }
    mu_assert("the Metal twin's options or flags differ from the CPU's", bad == 0u);
    return NULL;
}

/* The twin provides every feature its CPU extractor provides. */
static char *check_provides(const char *cpu_name, const char *twin_name)
{
    if (!metal_twin_have_device()) {
        return NULL;
    }
    const VmafFeatureExtractor *cpu = vmaf_get_feature_extractor_by_name(cpu_name);
    const VmafFeatureExtractor *twin = vmaf_get_feature_extractor_by_name(twin_name);
    mu_assert("the CPU extractor is not registered", cpu != NULL);
    mu_assert("the Metal twin is not registered", twin != NULL);
    const unsigned bad = feature_differences(cpu, twin);
    if (bad) {
        (void)fprintf(stderr, "\n%s: %u feature(s) of %s missing\n", twin_name, bad, cpu_name);
    }
    mu_assert("the Metal twin lacks a feature its CPU extractor provides", bad == 0u);
    return NULL;
}

#define TWIN_CASE(x, cpu_name, metal_name)                                                         \
    static char *test_twin_##x(void)                                                               \
    {                                                                                              \
        return check_twin(cpu_name, METAL_TWIN(metal_name, cpu_name));                             \
    }                                                                                              \
    static char *test_twin_##x##_provides(void)                                                    \
    {                                                                                              \
        return check_provides(cpu_name, METAL_TWIN(metal_name, cpu_name));                         \
    }

TWIN_CASE(psnr, "psnr", "integer_psnr_metal")
TWIN_CASE(ssim, "ssim", "integer_ssim_metal")
TWIN_CASE(float_ssim, "float_ssim", "float_ssim_metal")
TWIN_CASE(float_ms_ssim, "float_ms_ssim", "float_ms_ssim_metal")
TWIN_CASE(float_motion, "float_motion", "float_motion_metal")
TWIN_CASE(motion, "motion", "integer_motion_metal")
TWIN_CASE(motion_v2, "motion_v2", "motion_v2_metal")
TWIN_CASE(float_psnr, "float_psnr", "float_psnr_metal")
TWIN_CASE(float_moment, "float_moment", "float_moment_metal")
TWIN_CASE(float_vif, "float_vif", "float_vif_metal")
TWIN_CASE(float_adm, "float_adm", "float_adm_metal")
TWIN_CASE(vif, "vif", "integer_vif_metal")
TWIN_CASE(adm, "adm", "integer_adm_metal")
TWIN_CASE(ciede, "ciede", "integer_ciede_metal")
TWIN_CASE(psnr_hvs, "psnr_hvs", "integer_psnr_hvs_metal")
TWIN_CASE(cambi, "cambi", "integer_cambi_metal")
TWIN_CASE(ssimulacra2, "ssimulacra2", "ssimulacra2_metal")

static void run_group_0(void)
{
    metal_run_case(test_twin_psnr);
    metal_run_case(test_twin_psnr_provides);
    metal_run_case(test_twin_ssim);
    metal_run_case(test_twin_ssim_provides);
    metal_run_case(test_twin_float_ssim);
    metal_run_case(test_twin_float_ssim_provides);
    metal_run_case(test_twin_float_ms_ssim);
    metal_run_case(test_twin_float_ms_ssim_provides);
}

static void run_group_1(void)
{
    metal_run_case(test_twin_float_motion);
    metal_run_case(test_twin_float_motion_provides);
    metal_run_case(test_twin_motion);
    metal_run_case(test_twin_motion_provides);
    metal_run_case(test_twin_motion_v2);
    metal_run_case(test_twin_motion_v2_provides);
    metal_run_case(test_twin_float_psnr);
    metal_run_case(test_twin_float_psnr_provides);
}

static void run_group_2(void)
{
    metal_run_case(test_twin_float_moment);
    metal_run_case(test_twin_float_moment_provides);
    metal_run_case(test_twin_float_vif);
    metal_run_case(test_twin_float_vif_provides);
    metal_run_case(test_twin_float_adm);
    metal_run_case(test_twin_float_adm_provides);
    metal_run_case(test_twin_vif);
    metal_run_case(test_twin_vif_provides);
}

static void run_group_3(void)
{
    metal_run_case(test_twin_adm);
    metal_run_case(test_twin_adm_provides);
    metal_run_case(test_twin_ciede);
    metal_run_case(test_twin_ciede_provides);
    metal_run_case(test_twin_psnr_hvs);
    metal_run_case(test_twin_psnr_hvs_provides);
    metal_run_case(test_twin_cambi);
    metal_run_case(test_twin_cambi_provides);
}

static void run_group_4(void)
{
    metal_run_case(test_twin_ssimulacra2);
    metal_run_case(test_twin_ssimulacra2_provides);
}

char *run_tests(void)
{
    run_group_0();
    run_group_1();
    run_group_2();
    run_group_3();
    run_group_4();
    return metal_first_failure;
}

/* NOLINTEND(modernize-use-nullptr) */
