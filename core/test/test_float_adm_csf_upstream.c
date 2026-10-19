/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 */

/*
 * The CSF weights of float ADM are upstream's arithmetic (ADR-1489;
 * T-ADM-CSF-EXPONENT-NOT-UPSTREAM-2026-10-01).
 *
 * Two routines, both inherited from Netflix/vmaf and both changed by fork
 * ports that widened float intermediates to double:
 *
 *  - dwt_quant_step() (libvmaf/src/feature/adm_tools.h, lines 336 to 347 at
 *    cea2b4d8) stores r and temp in float and raises 10 to the float
 *    product k * temp * temp. The fork kept all three in double (PR #760).
 *  - barten_csf() and its helpers (libvmaf/src/feature/barten_csf_tools.h,
 *    lines 28 to 128) form p_0 * f, f / 7, a * b, -b[i] * f, the interpolation
 *    slope and the product of the three CSF factors in float. The fork
 *    promoted one operand of each to double (PR #44).
 *
 * Either moved every weight by a few units in the last place, and with the
 * weights every float_adm score; the Barten weights also feed the integer
 * extractor's adm_csf_mode=1.
 *
 * Checks:
 *  - the quantisation step equals the float form, bit for bit, on five
 *    viewing geometries, four scales and both band orientations, and the
 *    double form is another number;
 *  - linear_interpolate() equals the float form, and the double form is
 *    another number;
 *  - barten_csf() compiled as C++ (the SYCL and Metal twins of integer ADM
 *    include the header) returns the bits of the C one. Upstream's text
 *    leaves the promotions to the language, and C++ then calls the float
 *    math functions: the header writes the promotions out;
 *  - on glibc both routines return the bits a build of Netflix/vmaf cea2b4d8
 *    returns (GCC 16, glibc 2.44, x86-64). Another C library may round
 *    log10(), pow() or exp() differently in the last place of the double, so
 *    the constants are not asserted there; the other checks are.
 */

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "test.h"

#include "barten_csf_cxx.h"
#include "feature/adm_tools.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe, and this file mirrors
 * the C spelling of the surface it exercises. ADR-1138. */

enum {
    QS_SCALES = 4,
    QS_BANDS = 2,
    QS_GEOMETRIES = 5,
    QS_STEPS = 40,
    BARTEN_CASES = 42,
    BARTEN_VALUES = 168,
    INTERP_COLUMNS = 2,
    INTERP_SAMPLES = 64,
};

typedef struct QuantStepCase {
    double view_dist;
    int display_height;
    float step[QS_SCALES][QS_BANDS]; /* [lambda][theta - 1] */
} QuantStepCase;

/* Netflix/vmaf cea2b4d8, adm_tools.h::dwt_quant_step(&dwt_7_9_YCbCr_threshold[0],
 * lambda, theta, view_dist, display_height). The last row has the default
 * row's product view_dist * display_height and therefore its values. */
static const QuantStepCase upstream_steps[QS_GEOMETRIES] = {
    {3.00,
     1080,
     {{0x1.cc422cp+5f, 0x1.5384dcp+7f},
      {0x1.f43cb6p+4f, 0x1.17bd1p+6f},
      {0x1.70e55ep+4f, 0x1.47e91p+5f},
      {0x1.5e503p+4f, 0x1.fef964p+4f}}},
    {5.00,
     1080,
     {{0x1.246ed6p+7f, 0x1.eb31a2p+8f},
      {0x1.13659p+6f, 0x1.5eabacp+7f},
      {0x1.5ff1dap+5f, 0x1.642c22p+6f},
      {0x1.219758p+5f, 0x1.e0e87ep+5f}}},
    {1.50,
     1080,
     {{0x1.33689p+4f, 0x1.7c55f4p+5f},
      {0x1.95d53ap+3f, 0x1.7ca364p+4f},
      {0x1.6b85e2p+3f, 0x1.0efbaap+4f},
      {0x1.a350ecp+3f, 0x1.0074cp+4f}}},
    {3.00,
     2160,
     {{0x1.a284bep+7f, 0x1.7024ep+9f},
      {0x1.767b9cp+6f, 0x1.f36f6cp+7f},
      {0x1.c6b542p+5f, 0x1.e1f9c2p+6f},
      {0x1.637db2p+5f, 0x1.3528bap+6f}}},
    {0.75,
     4320,
     {{0x1.cc422cp+5f, 0x1.5384dcp+7f},
      {0x1.f43cb6p+4f, 0x1.17bd1p+6f},
      {0x1.70e55ep+4f, 0x1.47e91p+5f},
      {0x1.5e503p+4f, 0x1.fef964p+4f}}},
};

typedef struct BartenCase {
    double view_dist;
    int display_height;
    double lum_level;
    double csf_scale;
    float csf[QS_SCALES]; /* [lambda] */
} BartenCase;

/* Netflix/vmaf cea2b4d8, barten_csf_tools.h::barten_csf(lambda, view_dist,
 * display_height, lum_level, csf_scale). The luminances cover both clamps
 * (0.001 below the first anchor, 155 above the last) and every segment of
 * the interpolation. */
static const BartenCase upstream_barten[BARTEN_CASES] = {
    {3.0, 1080, 0.001, 1.0, {0x1.3c70fap-10f, 0x1.b05736p-8f, 0x1.de6bap-6f, 0x1.cdcaf2p-4f}},
    {3.0, 1080, 0.001, 1.2, {0x1.7bbac6p-10f, 0x1.036786p-7f, 0x1.1f0d6p-5f, 0x1.15135ep-3f}},
    {3.0, 1080, 0.01, 1.0, {0x1.889962p-8f, 0x1.fe8dd8p-6f, 0x1.06110ap-3f, 0x1.c255e8p-2f}},
    {3.0, 1080, 0.01, 1.2, {0x1.d71e76p-8f, 0x1.32551cp-5f, 0x1.3a7ad8p-3f, 0x1.0e338cp-1f}},
    {3.0, 1080, 0.2, 1.0, {0x1.b63388p-6f, 0x1.741f48p-3f, 0x1.e5e006p-1f, 0x1.ed768cp+1f}},
    {3.0, 1080, 0.2, 1.2, {0x1.06ebb8p-5f, 0x1.be8bfp-3f, 0x1.23866ap+0f, 0x1.2813eep+2f}},
    {3.0, 1080, 1, 1.0, {0x1.1cd1b2p-4f, 0x1.e6ep-2f, 0x1.37148ap+1f, 0x1.2207dp+3f}},
    {3.0, 1080, 1, 1.2, {0x1.55c87p-4f, 0x1.242p-1f, 0x1.754bd8p+1f, 0x1.5c096p+3f}},
    {3.0, 1080, 10, 1.0, {0x1.619592p-2f, 0x1.0506a2p+1f, 0x1.1518a8p+3f, 0x1.7b8e8ap+4f}},
    {3.0, 1080, 10, 1.2, {0x1.a84d16p-2f, 0x1.393b28p+1f, 0x1.4c83fcp+3f, 0x1.c777d8p+4f}},
    {3.0, 1080, 100, 1.0, {0x1.35e31ap+0f, 0x1.42b3c6p+2f, 0x1.d410eap+3f, 0x1.af9d36p+4f}},
    {3.0, 1080, 100, 1.2, {0x1.73dd52p+0f, 0x1.833e2p+2f, 0x1.18d6f2p+4f, 0x1.02f7eep+5f}},
    {3.0, 1080, 155, 1.0, {0x1.857e32p+0f, 0x1.7967c8p+2f, 0x1.fabaap+3f, 0x1.b1d50ep+4f}},
    {3.0, 1080, 155, 1.2, {0x1.d3643cp+0f, 0x1.c4e2fp+2f, 0x1.300994p+4f, 0x1.044ca2p+5f}},
    {5.0, 1080, 0.001, 1.0, {0x1.1f364p-12f, 0x1.fbf508p-10f, 0x1.455f6p-7f, 0x1.593c2ep-5f}},
    {5.0, 1080, 0.001, 1.2, {0x1.58a78p-12f, 0x1.30c638p-9f, 0x1.867274p-7f, 0x1.9e4838p-5f}},
    {5.0, 1080, 0.01, 1.0, {0x1.713ed6p-10f, 0x1.372014p-7f, 0x1.7a5c62p-5f, 0x1.7010e8p-3f}},
    {5.0, 1080, 0.01, 1.2, {0x1.bb1834p-10f, 0x1.7559b2p-7f, 0x1.c60876p-5f, 0x1.b9ade4p-3f}},
    {5.0, 1080, 0.2, 1.0, {0x1.5240cep-8f, 0x1.74a202p-5f, 0x1.271dd2p-2f, 0x1.683d74p+0f}},
    {5.0, 1080, 0.2, 1.2, {0x1.95e75ep-8f, 0x1.bf28dp-5f, 0x1.6223c8p-2f, 0x1.b049bep+0f}},
    {5.0, 1080, 1, 1.0, {0x1.b53f42p-7f, 0x1.e55f9cp-4f, 0x1.81c718p-1f, 0x1.c6927cp+1f}},
    {5.0, 1080, 1, 1.2, {0x1.065928p-6f, 0x1.23395ep-3f, 0x1.ceeeeap-1f, 0x1.10be4ap+2f}},
    {5.0, 1080, 10, 1.0, {0x1.2df1b6p-4f, 0x1.220542p-1f, 0x1.8ca3f8p+1f, 0x1.7bd2ep+3f}},
    {5.0, 1080, 10, 1.2, {0x1.6a554p-4f, 0x1.5c065p-1f, 0x1.dbf7f6p+1f, 0x1.c7c9dap+3f}},
    {5.0, 1080, 100, 1.0, {0x1.559908p-2f, 0x1.d003ecp+0f, 0x1.bde96ap+2f, 0x1.1fc134p+4f}},
    {5.0, 1080, 100, 1.2, {0x1.99ead6p-2f, 0x1.1668cp+1f, 0x1.0b8c0cp+3f, 0x1.594e3ep+4f}},
    {5.0, 1080, 155, 1.0, {0x1.c485d2p-2f, 0x1.1e2bc6p+1f, 0x1.ff693ep+2f, 0x1.30fd2ep+4f}},
    {5.0, 1080, 155, 1.2, {0x1.0f837ep-1f, 0x1.5767bap+1f, 0x1.32d8bep+3f, 0x1.6dfc9ep+4f}},
    {3.0, 2160, 0.001, 1.0, {0x1.39b812p-13f, 0x1.3c70fap-10f, 0x1.b05736p-8f, 0x1.de6bap-6f}},
    {3.0, 2160, 0.001, 1.2, {0x1.78767cp-13f, 0x1.7bbac6p-10f, 0x1.036786p-7f, 0x1.1f0d6p-5f}},
    {3.0, 2160, 0.01, 1.0, {0x1.987aaep-11f, 0x1.889962p-8f, 0x1.fe8dd8p-6f, 0x1.06110ap-3f}},
    {3.0, 2160, 0.01, 1.2, {0x1.ea2cdp-11f, 0x1.d71e76p-8f, 0x1.32551cp-5f, 0x1.3a7ad8p-3f}},
    {3.0, 2160, 0.2, 1.0, {0x1.5cb7b8p-9f, 0x1.b63388p-6f, 0x1.741f48p-3f, 0x1.e5e006p-1f}},
    {3.0, 2160, 0.2, 1.2, {0x1.a2761p-9f, 0x1.06ebb8p-5f, 0x1.be8bfp-3f, 0x1.23866ap+0f}},
    {3.0, 2160, 1, 1.0, {0x1.c1dee4p-8f, 0x1.1cd1b2p-4f, 0x1.e6ep-2f, 0x1.37148ap+1f}},
    {3.0, 2160, 1, 1.2, {0x1.0dec22p-7f, 0x1.55c87p-4f, 0x1.242p-1f, 0x1.754bd8p+1f}},
    {3.0, 2160, 10, 1.0, {0x1.42b55cp-5f, 0x1.619592p-2f, 0x1.0506a2p+1f, 0x1.1518a8p+3f}},
    {3.0, 2160, 10, 1.2, {0x1.834008p-5f, 0x1.a84d16p-2f, 0x1.393b28p+1f, 0x1.4c83fcp+3f}},
    {3.0, 2160, 100, 1.0, {0x1.8fe4c4p-3f, 0x1.35e31ap+0f, 0x1.42b3c6p+2f, 0x1.d410eap+3f}},
    {3.0, 2160, 100, 1.2, {0x1.dfdf52p-3f, 0x1.73dd52p+0f, 0x1.833e2p+2f, 0x1.18d6f2p+4f}},
    {3.0, 2160, 155, 1.0, {0x1.0de1f2p-2f, 0x1.857e32p+0f, 0x1.7967c8p+2f, 0x1.fabaap+3f}},
    {3.0, 2160, 155, 1.2, {0x1.43dbfp-2f, 0x1.d3643cp+0f, 0x1.c4e2fp+2f, 0x1.300994p+4f}},
};

static uint32_t float_bits(float value)
{
    uint32_t bits = 0u;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

/* The step as upstream forms it (wide == 0: r and temp stored in float, the
 * exponent a float product) or as the fork formed it before ADR-1489
 * (wide == 1: all three in double). */
static float quant_step_form(int lambda, int theta, double view_dist, int display_height, int wide)
{
    const struct dwt_model_params *params = &dwt_7_9_YCbCr_threshold[0];
    const double amplitude = dwt_7_9_basis_function_amplitudes[lambda][theta];
    if (wide) {
        const double r = view_dist * display_height * M_PI / 180.0;
        const double temp = log10(pow(2.0, lambda + 1) * params->f0 * params->g[theta] / r);
        return (float)(2.0 * params->a * pow(10.0, params->k * temp * temp) / amplitude);
    }
    const float r = (float)(view_dist * display_height * M_PI / 180.0);
    const float temp = (float)log10(pow(2.0, lambda + 1) * params->f0 * params->g[theta] / r);
    const float exponent = params->k * temp * temp;
    return (float)(2.0 * params->a * pow(10.0, (double)exponent) / amplitude);
}

/* How many of the QS_STEPS steps (geometry x scale x band) differ from the
 * float form (against_table == 0) or from upstream's recorded bits
 * (against_table == 1). */
static unsigned count_step_mismatches(int against_table)
{
    unsigned mismatches = 0u;
    for (int i = 0; i < QS_STEPS; i++) {
        const QuantStepCase *c = &upstream_steps[i / (QS_SCALES * QS_BANDS)];
        const int lambda = (i / QS_BANDS) % QS_SCALES;
        const int theta = 1 + i % QS_BANDS;
        const float got = dwt_quant_step(&dwt_7_9_YCbCr_threshold[0], lambda, theta, c->view_dist,
                                         c->display_height);
        const float want = against_table ?
                               c->step[lambda][theta - 1] :
                               quant_step_form(lambda, theta, c->view_dist, c->display_height, 0);
        mismatches += float_bits(got) != float_bits(want) ? 1u : 0u;
    }
    return mismatches;
}

static char *test_step_is_the_float_form(void)
{
    mu_assert("dwt_quant_step() is not upstream's float form", count_step_mismatches(0) == 0u);
    return NULL;
}

static char *test_double_form_of_the_step_is_another_number(void)
{
    const QuantStepCase *c = &upstream_steps[0];
    unsigned differing = 0u;
    for (int i = 0; i < QS_SCALES * QS_BANDS; i++) {
        const int lambda = i / QS_BANDS;
        const int theta = 1 + i % QS_BANDS;
        const float narrow = quant_step_form(lambda, theta, c->view_dist, c->display_height, 0);
        const float wide = quant_step_form(lambda, theta, c->view_dist, c->display_height, 1);
        differing += float_bits(narrow) != float_bits(wide) ? 1u : 0u;
    }
    mu_assert("the double intermediates no longer change a default-geometry step, so the "
              "float-form check has lost its teeth",
              differing > 0u);
    return NULL;
}

/* linear_interpolate() as upstream writes it (wide == 0, barten_csf_tools.h
 * line 31: every operation in float) or with the slope's numerator promoted
 * (wide == 1, the fork before ADR-1489). */
static float interpolate_form(float left_position, float left_value, float right_position,
                              float right_value, float sample_position, int wide)
{
    if (wide) {
        return (float)(left_value +
                       ((double)(right_value - left_value) / (right_position - left_position)) *
                           (sample_position - left_position));
    }
    const float slope = (right_value - left_value) / (right_position - left_position);
    const float offset = slope * (sample_position - left_position);
    return left_value + offset;
}

static char *test_linear_interpolate_is_float_arithmetic(void)
{
    /* The second and third columns of barten_csf_params between the 0.2 and
     * the 2 cd/m2 anchors, sampled across the segment. No math library is
     * involved, so the comparison holds on every platform. */
    const float left_position = -0.69897f;
    const float right_position = 0.30103f;
    const float left_value[INTERP_COLUMNS] = {0.476505f, 4.37453f};
    const float right_value[INTERP_COLUMNS] = {0.405782f, 4.40602f};
    unsigned mismatches = 0u;
    unsigned differing = 0u;
    for (int i = 0; i < INTERP_COLUMNS * INTERP_SAMPLES; i++) {
        const int column = i / INTERP_SAMPLES;
        const float sample = left_position + (float)(i % INTERP_SAMPLES) / (float)INTERP_SAMPLES;
        const float got = linear_interpolate(left_position, left_value[column], right_position,
                                             right_value[column], sample);
        const float narrow = interpolate_form(left_position, left_value[column], right_position,
                                              right_value[column], sample, 0);
        const float wide = interpolate_form(left_position, left_value[column], right_position,
                                            right_value[column], sample, 1);
        mismatches += float_bits(got) != float_bits(narrow) ? 1u : 0u;
        differing += float_bits(narrow) != float_bits(wide) ? 1u : 0u;
    }
    mu_assert("linear_interpolate() is not upstream's float expression", mismatches == 0u);
    mu_assert("the double slope no longer changes an interpolated parameter, so the float "
              "check has lost its teeth",
              differing > 0u);
    return NULL;
}

/* How many of the BARTEN_VALUES weights (case x scale) differ from the C++
 * translation unit's (against_table == 0) or from upstream's recorded bits
 * (against_table == 1). */
static unsigned count_barten_mismatches(int against_table)
{
    unsigned mismatches = 0u;
    for (int i = 0; i < BARTEN_VALUES; i++) {
        const BartenCase *c = &upstream_barten[i / QS_SCALES];
        const int lambda = i % QS_SCALES;
        const float got =
            barten_csf(lambda, c->view_dist, c->display_height, c->lum_level, c->csf_scale);
        const float want = against_table ?
                               c->csf[lambda] :
                               vmaf_test_barten_csf_cxx(lambda, c->view_dist, c->display_height,
                                                        c->lum_level, c->csf_scale);
        mismatches += float_bits(got) != float_bits(want) ? 1u : 0u;
    }
    return mismatches;
}

static char *test_barten_csf_is_the_same_in_c_and_cxx(void)
{
    mu_assert("barten_csf() returns another value in a C++ translation unit",
              count_barten_mismatches(0) == 0u);
    return NULL;
}

static char *test_step_has_upstreams_bits_on_glibc(void)
{
#if defined(__GLIBC__)
    mu_assert("dwt_quant_step() does not return Netflix/vmaf cea2b4d8's bits",
              count_step_mismatches(1) == 0u);
#endif
    return NULL;
}

static char *test_barten_csf_has_upstreams_bits_on_glibc(void)
{
#if defined(__GLIBC__)
    mu_assert("barten_csf() does not return Netflix/vmaf cea2b4d8's bits",
              count_barten_mismatches(1) == 0u);
#endif
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_step_is_the_float_form);
    mu_run_test(test_double_form_of_the_step_is_another_number);
    mu_run_test(test_linear_interpolate_is_float_arithmetic);
    mu_run_test(test_barten_csf_is_the_same_in_c_and_cxx);
    mu_run_test(test_step_has_upstreams_bits_on_glibc);
    mu_run_test(test_barten_csf_has_upstreams_bits_on_glibc);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
