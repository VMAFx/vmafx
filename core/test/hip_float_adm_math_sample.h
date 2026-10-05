/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * One sample of float ADM through the arithmetic of
 * feature/float_adm_gpu_common.h, for test_hip_float_adm_math: the device
 * probe (test_hip_float_adm_math_probe.hip) and the host test compile this
 * same function, the probe in the spelling of a HIP device and the test in
 * plain C, and the test demands the same bits from both.
 *
 * It runs every operation of the header that rounds: the angle test, the
 * decouple with its division and its fp64 gain, the two CSF products and
 * their fp64 filter, one nine-term masking sum with its fp64 centre tap, the
 * denominator terms and one masked term. The including file has included the
 * shared header (through float_adm_hip_math.h on the device).
 */

#ifndef LIBVMAF_TEST_HIP_FLOAT_ADM_MATH_SAMPLE_H_
#define LIBVMAF_TEST_HIP_FLOAT_ADM_MATH_SAMPLE_H_

#ifdef __cplusplus
#include <cstdint>
#else
#include <stdint.h>
#endif

/* Floats per sample: the reference (h, v, d), then the distorted (h, v, d). */
#define FADM_PROBE_IN 6u
/* Floats per sample out: 3 x (csf_a, csf_fa, csf_r, csf_fr), the masking sum,
 * 3 denominator terms, one masked term, the angle flag. */
#define FADM_PROBE_OUT 18u

/* The exponent of the terms: adm_p_norm = 3 (`is_cube`) or 1. */
struct FadmProbeArgs {
    FloatAdmGpuDecoupleArgs decouple;
    uint32_t is_cube;
    float p_norm;
};
#ifndef __cplusplus
typedef struct FadmProbeArgs FadmProbeArgs;
#endif

FADM_HD void fadm_probe_sample(const FadmProbeArgs *a, const float *in, float *out)
{
    const float *o = in;
    const float *t = in + FADM_BANDS;
    const int angle_flag = fadm_angle_flag(o[0], o[1], t[0], t[1], a->decouple.cos_1deg_sq);
    for (int b = 0; b < FADM_BANDS; b++) {
        const FloatAdmCsfSample c = fadm_decouple_csf(&a->decouple, b, o[b], t[b], angle_flag);
        out[4 * b + 0] = c.csf_a;
        out[4 * b + 1] = c.csf_fa;
        out[4 * b + 2] = c.csf_r;
        out[4 * b + 3] = c.csf_fr;
    }
    /* A masking sum over eight of the values above as the neighbours and a
     * ninth as the centre: any floats exercise its fp32 adds and its fp64
     * centre tap. */
    const float threshold = fadm_thresh_band(out, out[8]);
    out[12] = threshold;
    for (int b = 0; b < FADM_BANDS; b++) {
        out[13 + b] = fadm_den_term(a->decouple.bands.rfactor[b], o[b], a->is_cube, a->p_norm);
    }
    /* A sixteenth of the sum, so that the term is not zero on most samples;
     * the product is exact. */
    out[16] = fadm_cm_term(out[2], FADM_FMUL(threshold, 0.0625f), a->is_cube, a->p_norm);
    out[17] = (float)angle_flag;
}

#endif /* LIBVMAF_TEST_HIP_FLOAT_ADM_MATH_SAMPLE_H_ */
