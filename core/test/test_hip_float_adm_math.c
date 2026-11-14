/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * ADR-1458 — the arithmetic float_adm_hip's kernels run, value by value on a
 * HIP device against the same statements on the host.
 *
 * feature/float_adm_gpu_common.h is adm_tools.c written out operation for
 * operation; test_float_adm_device_math holds it to the CPU extractor on the
 * host. The HIP kernels compile it with plain C operators, so that it is the
 * reference's arithmetic on the device rests on what hipcc makes of them: an
 * fp32 `/` that could become a reciprocal multiply, a product and a sum that
 * could be contracted, an fp64 product and sum for the gain limit and the
 * two CSF constants. This test runs fadm_probe_sample() (hip_float_adm_math_
 * sample.h) for a million samples in a kernel built like the extractor's and
 * on the host, and demands the same bits: every division of the decouple,
 * including quotients of operands a few units apart and the reference's
 * `o + 1e-30`, the angle test at vectors about one degree apart, the fp64
 * gain at a limit that is not an fp32 value, and the terms at adm_p_norm 3
 * and 1.
 *
 * Skips (77) without a HIP device.
 */

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <hip/hip_runtime_api.h>

#include "test.h"

#include "feature/adm_float_reference.h"
#include "feature/float_adm_gpu_common.h"

#include "hip_float_adm_math_sample.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe. ADR-1138. */

/* HSACO embedded by xxd from test_hip_float_adm_math_probe.hip. */
extern unsigned char test_hip_float_adm_math_probe_hsaco[];

enum {
    SAMPLES = 1 << 20,
    PROBE_BLOCK = 256,
};

/* Deterministic generator: the same samples on every host. */
static uint64_t rng_state = 0x9E3779B97F4A7C15ull;

static uint64_t rng_next(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

/* Uniform in [-1, 1). */
static float rng_signed(void)
{
    return (float)((double)(rng_next() >> 11) * 0x1p-52 - 1.0);
}

/* A DWT coefficient: a magnitude between 2^-12 and 2^10. */
static float rng_coefficient(void)
{
    return ldexpf(rng_signed(), (int)(rng_next() % 23u) - 12);
}

/* One sample: the reference (h, v, d) and the distorted (h, v, d). Most
 * samples are a reference coefficient and a distorted one near it, as in a
 * picture; the kinds below reach the branches a random pair seldom does. */
static void fill_sample(float *s, uint64_t kind)
{
    for (unsigned b = 0u; b < FADM_BANDS; b++) {
        s[b] = rng_coefficient();
        s[FADM_BANDS + b] = s[b] * (1.0f + 0.25f * rng_signed());
    }
    switch (kind % 16u) {
    case 0u: /* unrelated signals: the angle test fails, the gain is not applied */
        for (unsigned b = 0u; b < FADM_BANDS; b++)
            s[FADM_BANDS + b] = rng_coefficient();
        break;
    case 1u: /* a zero reference: the quotient is t / 1e-30 */
        s[rng_next() % FADM_BANDS] = 0.0f;
        break;
    case 2u: /* a zero distorted signal */
        s[FADM_BANDS + rng_next() % FADM_BANDS] = 0.0f;
        break;
    case 3u: { /* (h, v) about one degree apart: the angle test at its threshold */
        const float angle = 0.0174532925f * (1.0f + 0.01f * rng_signed());
        s[FADM_BANDS + 0u] = s[0] * cosf(angle) - s[1] * sinf(angle);
        s[FADM_BANDS + 1u] = s[0] * sinf(angle) + s[1] * cosf(angle);
        break;
    }
    case 4u: /* operands a few units in the last place apart */
        for (unsigned b = 0u; b < FADM_BANDS; b++)
            s[FADM_BANDS + b] = nextafterf(s[b], (rng_next() & 1u) ? INFINITY : -INFINITY);
        break;
    default:
        break;
    }
}

static hipError_t probe_launch(hipFunction_t kernel, const FadmProbeArgs *args, const float *in,
                               float *out, float *dev_in, float *dev_out)
{
    hipError_t rc = hipMemcpy(dev_in, in, (size_t)SAMPLES * FADM_PROBE_IN * sizeof(float),
                              hipMemcpyHostToDevice);
    if (rc != hipSuccess)
        return rc;
    FadmProbeArgs a = *args;
    unsigned count = (unsigned)SAMPLES;
    void *params[] = {(void *)&a, (void *)&dev_in, (void *)&dev_out, (void *)&count};
    const unsigned grid = (count + PROBE_BLOCK - 1u) / PROBE_BLOCK;
    rc = hipModuleLaunchKernel(kernel, grid, 1u, 1u, PROBE_BLOCK, 1u, 1u, 0u, NULL, params, NULL);
    if (rc == hipSuccess)
        rc = hipDeviceSynchronize();
    if (rc == hipSuccess) {
        rc = hipMemcpy(out, dev_out, (size_t)SAMPLES * FADM_PROBE_OUT * sizeof(float),
                       hipMemcpyDeviceToHost);
    }
    return rc;
}

/* Runs the probe kernel over `in`. 0, -ENODEV without a usable device, -EIO
 * when a device call fails. */
static int probe_device(const FadmProbeArgs *args, const float *in, float *out)
{
    int devices = 0;
    if (hipInit(0) != hipSuccess || hipGetDeviceCount(&devices) != hipSuccess || devices < 1 ||
        hipSetDevice(0) != hipSuccess)
        return -ENODEV;

    hipModule_t module = NULL;
    hipFunction_t kernel = NULL;
    float *dev_in = NULL;
    float *dev_out = NULL;
    hipError_t rc = hipModuleLoadData(&module, test_hip_float_adm_math_probe_hsaco);
    if (rc != hipSuccess)
        return -EIO;
    rc = hipModuleGetFunction(&kernel, module, "vmaf_test_hip_float_adm_kernel");
    if (rc == hipSuccess)
        rc = hipMalloc((void **)&dev_in, (size_t)SAMPLES * FADM_PROBE_IN * sizeof(float));
    if (rc == hipSuccess)
        rc = hipMalloc((void **)&dev_out, (size_t)SAMPLES * FADM_PROBE_OUT * sizeof(float));
    if (rc == hipSuccess)
        rc = probe_launch(kernel, args, in, out, dev_in, dev_out);
    if (dev_out != NULL)
        (void)hipFree(dev_out);
    if (dev_in != NULL)
        (void)hipFree(dev_in);
    (void)hipModuleUnload(module);
    return (rc == hipSuccess) ? 0 : -EIO;
}

/* The decouple arguments of scale `scale` at the default options, with the
 * reference's own CSF weights and angle threshold. */
static FadmProbeArgs probe_args(int scale, double gain_limit, double p_norm)
{
    FadmProbeArgs a;
    memset(&a, 0, sizeof(a));
    adm_csf_rfactor_s(scale, 3.0, 1080, 0, 100.0, 1.0, 1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0,
                      -1.0, -1.0, a.decouple.bands.rfactor);
    a.decouple.adm_enhn_gain_limit = gain_limit;
    a.decouple.cos_1deg_sq = adm_decouple_cos_1deg_sq_s();
    a.is_cube = (p_norm == 3.0) ? 1u : 0u;
    a.p_norm = (float)p_norm;
    return a;
}

/* Outputs whose bits differ between the device and the host, with the first
 * one reported. */
static size_t count_mismatches(const FadmProbeArgs *a, const float *in, const float *device)
{
    size_t mismatches = 0u;
    for (size_t i = 0u; i < (size_t)SAMPLES; i++) {
        float host[FADM_PROBE_OUT];
        fadm_probe_sample(a, in + i * FADM_PROBE_IN, host);
        const float *dev = device + i * FADM_PROBE_OUT;
        for (unsigned k = 0u; k < FADM_PROBE_OUT; k++) {
            if (fadm_host_bits(host[k]) == fadm_host_bits(dev[k]))
                continue;
            if (mismatches == 0u) {
                (void)fprintf(stderr, "\nsample %zu output %u: host %a device %a (o %a t %a)\n", i,
                              k, (double)host[k], (double)dev[k], (double)in[i * FADM_PROBE_IN],
                              (double)in[i * FADM_PROBE_IN + FADM_BANDS]);
            }
            mismatches++;
        }
    }
    return mismatches;
}

static int no_device;

/* One run of the probe at a gain limit and an exponent. */
static char *check_arithmetic(int scale, double gain_limit, double p_norm)
{
    float *in = calloc((size_t)SAMPLES * FADM_PROBE_IN, sizeof(float));
    float *out = calloc((size_t)SAMPLES * FADM_PROBE_OUT, sizeof(float));
    char *msg = NULL;
    if (!in || !out) {
        msg = "allocation failed";
    } else {
        for (size_t i = 0u; i < (size_t)SAMPLES; i++)
            fill_sample(in + i * FADM_PROBE_IN, rng_next());
        const FadmProbeArgs a = probe_args(scale, gain_limit, p_norm);
        const int err = probe_device(&a, in, out);
        if (err == -ENODEV) {
            no_device = 1;
        } else if (err != 0) {
            msg = "the HIP probe failed";
        } else if (count_mismatches(&a, in, out) != 0u) {
            msg = "the device's float ADM arithmetic is not the host's";
        }
    }
    free(out);
    free(in);
    return msg;
}

static char *test_default_options(void)
{
    return check_arithmetic(0, 100.0, 3.0);
}

/* 1.2 is not an fp32 value: the gain is an fp64 product. */
static char *test_gain_limit(void)
{
    return check_arithmetic(2, 1.2, 3.0);
}

/* adm_p_norm = 1: the terms are the samples themselves. */
static char *test_p_norm_one(void)
{
    return check_arithmetic(3, 100.0, 1.0);
}

char *run_tests(void)
{
    mu_run_test(test_default_options);
    mu_run_test(test_gain_limit);
    mu_run_test(test_p_norm_one);
    if (no_device) {
        (void)fprintf(stderr, "[skip: no HIP device] ");
        mu_skipped = 1;
    }
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
