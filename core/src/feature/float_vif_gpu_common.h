/**
 *  Copyright 2016-2020 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  float_vif on a GPU: the kernel argument blocks and the per-pixel
 *  arithmetic of the twins that have an fp64 type on the device,
 *  float_vif_cuda (ADR-1412) and float_vif_hip (ADR-1444). The host side of a
 *  twin, its kernels and the device-free host test
 *  (core/test/test_float_vif_device_math.c) all compile this header.
 *
 *  Numerical contract: a twin built on this header returns the CPU
 *  extractor's values bit for bit. Everything below is vif_tools.c written
 *  out operation for operation, in the reference's types:
 *
 *   - fvif_tap() is one tap of vif_filter1d_s() / convolution_f32_avx_s():
 *     an fp32 product rounded once, an fp32 add rounded once, taps in order.
 *     The taps themselves come from the host, which calls vif_get_filter()
 *     as float_vif.c does. They are not constants of a kernel: the reference
 *     derives them at run time in fp32, and a table of the nearest decimal
 *     values differs from that in the last bit of 26 of the 34 taps.
 *   - fvif_log2() is log2f_approx(): vif_options.h defines VIF_OPT_FAST_LOG2,
 *     so the reference never calls libm's log2f. Exponent plus an fp32 Horner
 *     polynomial in the mantissa, no libm on either side.
 *   - fvif_pixel_statistic() is vif_pixel_statistic_s(): vif_sigma_nsq stays a
 *     double, so both log arguments are fp64 quotients and sums rounded to
 *     fp32 once, and the sigma1_sq < vif_sigma_nsq test is an fp64 compare.
 *   - fvif_row_sum() is the inner loop of vif_statistic_s(): one fp32
 *     accumulator per row, left to right. fvif_sum_rows() is its outer loop,
 *     a second fp32 accumulator over the rows, top to bottom; it runs on the
 *     host over the per-row sums the device returns.
 *
 *  Every operation that rounds goes through one of the FVIF_F* / FVIF_D*
 *  macros. The defaults are the plain C operators with the result converted
 *  to its type, which round once when the translation unit is built without
 *  contraction: a host TU that includes this header must be, and the HIP
 *  kernels are (ADR-1407; hipcc also divides correctly rounded there, in fp32
 *  by flag and in fp64 as measured for ADR-1444). A backend whose device
 *  compiler needs another spelling defines the macros before including the
 *  header: the CUDA kernels map them to the __fmul_rn() family, which neither
 *  nvcc nor clang's CUDA driver contracts (cuda/float_vif/float_vif_device.h).
 *  FVIF_HD is the linkage and execution space of the helpers, and
 *  FVIF_FLOAT_AS_UINT() / FVIF_UINT_AS_FLOAT() the device's bit casts.
 *
 *  The SYCL twin cannot use this header: its devices have no fp64 type, so it
 *  evaluates the two fp64 expressions otherwise (sycl/sycl_float_vif_math.h,
 *  ADR-1422).
 */

#ifndef VMAF_SRC_FEATURE_FLOAT_VIF_GPU_COMMON_H_
#define VMAF_SRC_FEATURE_FLOAT_VIF_GPU_COMMON_H_

#include <stddef.h>
#include <stdint.h>

#define FVIF_SCALES 4
#define FVIF_BX 16
#define FVIF_BY 16
#define FVIF_MAX_FW 17 /* widest filter of the scale ladder at vif_kernelscale = 1 */
#define FVIF_MAX_HFW (FVIF_MAX_FW / 2)
#define FVIF_ROW_THREADS 128 /* threads per block of float_vif_row_sums */
#define FVIF_TERM_FLOATS 2   /* num, den */

#ifndef FVIF_HD
#define FVIF_HD static inline
#endif

#ifndef FVIF_FMUL
#define FVIF_FMUL(a, b) ((float)((a) * (b)))
#define FVIF_FADD(a, b) ((float)((a) + (b)))
#define FVIF_FSUB(a, b) ((float)((a) - (b)))
#define FVIF_FDIV(a, b) ((float)((a) / (b)))
#define FVIF_DADD(a, b) ((double)((a) + (b)))
#define FVIF_DDIV(a, b) ((double)((a) / (b)))
#endif

/* ------------------------------------------------------------------ */
/* Kernel argument blocks. Each kernel takes exactly one, by value, and */
/* the host passes `void *params[] = {&args}`: one layout for both      */
/* sides, so an argument cannot be dropped or reordered between them    */
/* (the CUDA driver silently ignores a surplus one, ADR-1215). Device   */
/* pointers travel as uint64_t.                                         */
/* ------------------------------------------------------------------ */

/* One scale's Gaussian, as vif_get_filter() returns it. */
struct FloatVifGpuTaps {
    float coeff[FVIF_MAX_FW];
    int32_t width;
};
#ifndef __cplusplus
typedef struct FloatVifGpuTaps FloatVifGpuTaps;
#endif

/* The planes a compute or decimate launch reads: the raw luma planes of the
 * frame (uint8 or uint16 samples, tightly packed) when `is_raw`, otherwise the
 * fp32 planes the previous decimate wrote. */
struct FloatVifGpuInput {
    uint64_t ref;
    uint64_t dis;
    int64_t stride; /* bytes per row when is_raw, floats per row otherwise */
    uint32_t width;
    uint32_t height;
    uint32_t bpc;
    uint32_t is_raw;
};
#ifndef __cplusplus
typedef struct FloatVifGpuInput FloatVifGpuInput;
#endif

struct FloatVifGpuComputeArgs {
    FloatVifGpuInput in;
    FloatVifGpuTaps taps;
    uint64_t terms;       /* float, see fvif_term_index() */
    double vif_sigma_nsq; /* fp64, like the reference's argument */
    float vif_enhn_gain_limit;
    float sigma_max_inv;
};
#ifndef __cplusplus
typedef struct FloatVifGpuComputeArgs FloatVifGpuComputeArgs;
#endif

struct FloatVifGpuDecimateArgs {
    FloatVifGpuInput in;
    FloatVifGpuTaps taps;
    uint64_t ref_out; /* float, out_width x out_height, tightly packed */
    uint64_t dis_out;
    uint32_t out_width;
    uint32_t out_height;
};
#ifndef __cplusplus
typedef struct FloatVifGpuDecimateArgs FloatVifGpuDecimateArgs;
#endif

struct FloatVifGpuRowArgs {
    uint64_t terms; /* float, see fvif_term_index() */
    uint64_t rows;  /* float, FVIF_TERM_FLOATS per row: num, den */
    uint32_t width;
    uint32_t height;
};
#ifndef __cplusplus
typedef struct FloatVifGpuRowArgs FloatVifGpuRowArgs;
#endif

/* ------------------------------------------------------------------ */
/* The reference's arithmetic.                                          */
/* ------------------------------------------------------------------ */

/* The per-pixel terms are stored column by column: the row-sum kernel runs
 * one thread per row, so at every step neighbouring threads read neighbouring
 * addresses. */
FVIF_HD size_t fvif_term_index(uint32_t x, uint32_t y, uint32_t height)
{
    return ((size_t)x * (size_t)height + (size_t)y) * (size_t)FVIF_TERM_FLOATS;
}

/* One filter tap: acc + coeff * sample, the product and the sum each rounded
 * to fp32. */
FVIF_HD float fvif_tap(float acc, float coeff, float sample)
{
    const float product = FVIF_FMUL(coeff, sample);
    return FVIF_FADD(acc, product);
}

#if defined(FVIF_FLOAT_AS_UINT)
FVIF_HD uint32_t fvif_float_bits(float value)
{
    return (uint32_t)FVIF_FLOAT_AS_UINT(value);
}

FVIF_HD float fvif_bits_float(uint32_t bits)
{
    return FVIF_UINT_AS_FLOAT(bits);
}
#else
#include <string.h>
FVIF_HD uint32_t fvif_float_bits(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

FVIF_HD float fvif_bits_float(uint32_t bits)
{
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}
#endif

/* vif_tools.c::log2f_approx() with horner_s() and log2_poly_s[] inlined. The
 * coefficients are the reference's decimal literals, rounded to fp32 as its
 * `static const float` table rounds them. */
FVIF_HD float fvif_log2(float x)
{
    if (x == 0.0f)
        return fvif_bits_float(0xff800000u); /* -inf */
    if (x < 0.0f)
        return fvif_bits_float(0x7fc00000u); /* NaN */

    const uint32_t bits = fvif_float_bits(x);
    const uint32_t exponent = (bits & 0x7F800000u) >> 23u;
    const uint32_t mantissa = bits & 0x007FFFFFu;
    const float remain = fvif_bits_float(mantissa | 0x3F800000u);
    const float log_base = (float)((int32_t)exponent - 127);
    const float t = FVIF_FSUB(remain, 1.0f);

    float var = 0.0f;
    var = FVIF_FADD(FVIF_FMUL(var, t), (float)-0.012671635276421);
    var = FVIF_FADD(FVIF_FMUL(var, t), (float)0.064841182402670);
    var = FVIF_FADD(FVIF_FMUL(var, t), (float)-0.157048836463065);
    var = FVIF_FADD(FVIF_FMUL(var, t), (float)0.257167726303123);
    var = FVIF_FADD(FVIF_FMUL(var, t), (float)-0.353800560300520);
    var = FVIF_FADD(FVIF_FMUL(var, t), (float)0.480131410397451);
    var = FVIF_FADD(FVIF_FMUL(var, t), (float)-0.721314327952201);
    var = FVIF_FADD(FVIF_FMUL(var, t), (float)1.442694803896991);
    var = FVIF_FADD(FVIF_FMUL(var, t), 0.0f);
    return FVIF_FADD(log_base, var);
}

/* vif_tools.c::vif_pixel_statistic_s(): the numerator and denominator terms of
 * one pixel from its five filtered moments. */
FVIF_HD void fvif_pixel_statistic(float mu1, float mu2, float xx_filt, float yy_filt, float xy_filt,
                                  float sigma_max_inv, float vif_enhn_gain_limit,
                                  double vif_sigma_nsq, float *num_val, float *den_val)
{
    const float eps = 1.0e-10f;
    const float mu1_sq = FVIF_FMUL(mu1, mu1);
    const float mu2_sq = FVIF_FMUL(mu2, mu2);
    const float mu1_mu2 = FVIF_FMUL(mu1, mu2);

    float sigma1_sq = FVIF_FSUB(xx_filt, mu1_sq);
    float sigma2_sq = FVIF_FSUB(yy_filt, mu2_sq);
    const float sigma12 = FVIF_FSUB(xy_filt, mu1_mu2);

    /* The reference's MAX() / MIN() macros, not fmaxf() / fminf(): they pick
     * the second operand for a NaN and for -0 against +0. */
    sigma1_sq = (sigma1_sq > 0.0f) ? sigma1_sq : 0.0f;
    sigma2_sq = (sigma2_sq > 0.0f) ? sigma2_sq : 0.0f;

    float g = FVIF_FDIV(sigma12, FVIF_FADD(sigma1_sq, eps));
    float sv_sq = FVIF_FSUB(sigma2_sq, FVIF_FMUL(g, sigma12));

    if (sigma1_sq < eps) {
        g = 0.0f;
        sv_sq = sigma2_sq;
        sigma1_sq = 0.0f;
    }
    if (sigma2_sq < eps) {
        g = 0.0f;
        sv_sq = 0.0f;
    }
    if (g < 0.0f) {
        sv_sq = sigma2_sq;
        g = 0.0f;
    }
    sv_sq = (sv_sq > eps) ? sv_sq : eps;
    g = (g < vif_enhn_gain_limit) ? g : vif_enhn_gain_limit;

    /* log2f(1.0f + (g * g * sigma1_sq) / (sv_sq + vif_sigma_nsq)): the product
     * is fp32, the denominator, the quotient and the sum are fp64 because
     * vif_sigma_nsq is, and the argument is rounded to fp32 at the call. */
    const float gain_sq_sigma1 = FVIF_FMUL(FVIF_FMUL(g, g), sigma1_sq);
    const double num_den = FVIF_DADD((double)sv_sq, vif_sigma_nsq);
    const double num_arg = FVIF_DADD(1.0, FVIF_DDIV((double)gain_sq_sigma1, num_den));
    const double den_arg = FVIF_DADD(1.0, FVIF_DDIV((double)sigma1_sq, vif_sigma_nsq));
    float num = fvif_log2((float)num_arg);
    float den = fvif_log2((float)den_arg);

    if (sigma12 < 0.0f)
        num = 0.0f;
    if ((double)sigma1_sq < vif_sigma_nsq) {
        num = FVIF_FSUB(1.0f, FVIF_FMUL(sigma2_sq, sigma_max_inv));
        den = 1.0f;
    }
    *num_val = num;
    *den_val = den;
}

/* The inner loop of vif_statistic_s() for row `y`: every term of the row, left
 * to right, into one fp32 accumulator per output. */
FVIF_HD void fvif_row_sum(const float *terms, uint32_t width, uint32_t height, uint32_t y,
                          float *num_sum, float *den_sum)
{
    float accum_num = 0.0f;
    float accum_den = 0.0f;
    for (uint32_t x = 0u; x < width; x++) {
        const size_t at = fvif_term_index(x, y, height);
        accum_num = FVIF_FADD(accum_num, terms[at]);
        accum_den = FVIF_FADD(accum_den, terms[at + 1u]);
    }
    *num_sum = accum_num;
    *den_sum = accum_den;
}

#if !defined(FVIF_DEVICE_ONLY)
/* The outer loop of vif_statistic_s(): the per-row sums, top to bottom, into
 * one fp32 accumulator per output. compute_vif() widens the two floats to
 * double afterwards, so the results are returned that way. Host code. */
static inline void fvif_sum_rows(const float *rows, uint32_t height, double *num, double *den)
{
    float accum_num = 0.0f;
    float accum_den = 0.0f;
    for (uint32_t y = 0u; y < height; y++) {
        accum_num = FVIF_FADD(accum_num, rows[(size_t)y * FVIF_TERM_FLOATS]);
        accum_den = FVIF_FADD(accum_den, rows[(size_t)y * FVIF_TERM_FLOATS + 1u]);
    }
    *num = (double)accum_num;
    *den = (double)accum_den;
}
#endif

#endif /* VMAF_SRC_FEATURE_FLOAT_VIF_GPU_COMMON_H_ */
