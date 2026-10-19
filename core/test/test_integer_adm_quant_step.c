/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 */

/*
 * The Watson quantisation step of integer ADM is upstream's arithmetic
 * (ADR-1475; T-ADM-CSF-EXPONENT-NOT-UPSTREAM-2026-10-01,
 * T-UPSTREAM-AB-SCORE-DELTA-2026-09-07).
 *
 * dwt_quant_step() raises 10 to k * temp * temp. Upstream (Netflix/vmaf
 * libvmaf/src/feature/integer_adm.c, lines 234 to 248 at cea2b4d8) multiplies
 * the three floats in float and promotes the product for pow(). The fork
 * carried `params->k * (double)temp * temp` from a static-analysis sweep
 * (PR #552) until ADR-1475: a double product, which moved the CSF weights of
 * every scale by one to three units in the last place and with them every
 * integer ADM score and every model score that reads one.
 *
 * Three checks:
 *  - the step equals the float-product form, bit for bit, on five viewing
 *    geometries, four scales and both band orientations;
 *  - the float-product form and the double-product form differ on the default
 *    geometry, so the first check fails when the cast comes back;
 *  - on glibc the default and four other geometries return the bits a build
 *    of Netflix/vmaf cea2b4d8 returns (GCC 16, glibc 2.44, x86-64). Another
 *    C library may round log10() or pow() differently in the last place of
 *    the double, so the constants are not asserted there; the first check is.
 */

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "test.h"

#include "feature/integer_adm.h"
#include "feature/integer_adm_kernels.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe, and this file mirrors
 * the C spelling of the surface it exercises. ADR-1138. */

enum { QS_SCALES = 4, QS_BANDS = 2, QS_GEOMETRIES = 5, QS_STEPS = 40 };

typedef struct QuantStepCase {
    double view_dist;
    int display_height;
    float step[QS_SCALES][QS_BANDS]; /* [lambda][theta - 1] */
} QuantStepCase;

/* Netflix/vmaf cea2b4d8, dwt_quant_step(&dwt_7_9_YCbCr_threshold[0], lambda,
 * theta, view_dist, display_height). The last row has the default row's
 * product view_dist * display_height and therefore its values. */
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

static uint32_t float_bits(float value)
{
    uint32_t bits = 0u;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

/* The step with the exponent formed as upstream forms it (wide == 0, a float
 * product) or as the fork formed it before ADR-1475 (wide == 1, a double
 * product). Everything else is the same in both. */
static float quant_step_form(int lambda, int theta, double view_dist, int display_height, int wide)
{
    const struct dwt_model_params *params = &dwt_7_9_YCbCr_threshold[0];
    const float r = (float)(view_dist * display_height * M_PI / 180.0);
    const float temp = (float)log10(pow(2.0, lambda + 1) * params->f0 * params->g[theta] / r);
    const float narrow_exponent = params->k * temp * temp;
    const double exponent = wide ? params->k * (double)temp * temp : narrow_exponent;
    const float step = (float)(2.0 * params->a * pow(10.0, exponent) /
                               dwt_7_9_basis_function_amplitudes[lambda][theta]);
    return step;
}

/* How many of the QS_STEPS steps (geometry x scale x band) differ from the
 * float-product form (against_table == 0) or from upstream's recorded bits
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

static char *test_step_is_the_float_product_form(void)
{
    mu_assert("dwt_quant_step() is not upstream's float-product form",
              count_step_mismatches(0) == 0u);
    return NULL;
}

static char *test_double_product_form_is_another_number(void)
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
    mu_assert("the double-product exponent no longer changes a default-geometry step, so the "
              "float-product check has lost its teeth",
              differing > 0u);
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

char *run_tests(void)
{
    mu_run_test(test_step_is_the_float_product_form);
    mu_run_test(test_double_product_form_is_another_number);
    mu_run_test(test_step_has_upstreams_bits_on_glibc);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
