/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Device-resident SpEED on HIP (ADR-1384): the parameter block shared by
 *  the host pipeline (speed_hip_pipeline.c) and the kernels
 *  (speed_pipeline.hip), and the per-work-item arithmetic both of them and
 *  the host unit test (core/test/test_hip_speed_device_math.c) run. The
 *  geometry, scoring, binding and result types are the backend-neutral ones
 *  of feature/speed_gpu_common.h, which the SYCL pipeline uses too.
 *
 *  Every routine is a line-for-line port of the SYCL pipeline of ADR-1358
 *  (core/src/feature/sycl/speed_sycl_pipeline.cpp), which reproduces the CPU
 *  reference (speed.c, vif_tools.c, convolution_internal.h) operation for
 *  operation in fp32. Products are held in named temporaries, and every TU
 *  that compiles this header for the device or for the unit test is built
 *  with -ffp-contract=off, so each multiply and each add rounds where the
 *  host rounds. Division and square root are the plain `/` and sqrtf():
 *  hipcc lowers both to the correctly rounded IEEE sequences under
 *  -fhip-fp32-correctly-rounded-divide-sqrt, which the speed kernel build
 *  passes explicitly (core/src/meson.build). HIP's __fsqrt_rn() is NOT a
 *  substitute: without OCML_BASIC_ROUNDED_OPERATIONS it is the native
 *  approximate square root, and __fmul_rn() / __fadd_rn() are the plain
 *  operators, which contract. The fp64 expressions of the reference are
 *  reproduced with exact fp32 pairs; nothing here uses fp64 (ADR-0220 spirit,
 *  core/test/test_hip_kernel_source_contract.py).
 *
 *  Group-cooperative routines take a SpeedHdLanes (this work-item's index and
 *  the group size) and synchronise with SPEED_HD_SYNC(); every element a
 *  phase writes is computed by exactly one lane from values the previous
 *  phase finished, so the unit test replays a group as one lane of count 1
 *  and gets the device's numbers.
 */

#ifndef FEATURE_HIP_SPEED_SPEED_HIP_DEVICE_H_
#define FEATURE_HIP_SPEED_SPEED_HIP_DEVICE_H_

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* The per-run contract every device-resident SpEED twin shares (geometry,
 * scoring constants, channel bindings, the per-frame result), filled at init
 * by speed_internal_gpu_configure(). */
#include "feature/speed_gpu_common.h"

#if defined(__HIPCC__)
#define SPEED_HD __host__ __device__
#else
#define SPEED_HD
#endif

#if defined(__HIP_DEVICE_COMPILE__)
#define SPEED_HD_SYNC() __syncthreads()
#else
#define SPEED_HD_SYNC() ((void)0)
#endif

#ifdef __cplusplus
#define SPEED_HIP_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#else
#define SPEED_HIP_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif

/* NOLINTBEGIN(modernize-use-nullptr): C header, also compiled as C. ADR-1138. */

#define SPEED_HIP_BLOCK SPEED_GPU_BLOCK
#define SPEED_HIP_N SPEED_GPU_ELEMENTS /* elements per block */
#define SPEED_HIP_MATRIX 625u          /* 25 x 25 */
#define SPEED_HIP_TRIANGLE 325u        /* 25 * 26 / 2 */
#define SPEED_HIP_MAX_CHANNELS SPEED_GPU_MAX_CHANNELS
#define SPEED_HIP_MAX_PAIRS SPEED_GPU_MAX_PAIRS
#define SPEED_HIP_MAX_RAW_PLANES SPEED_GPU_MAX_RAW_PLANES
#define SPEED_HIP_MAX_TAPS SPEED_GPU_MAX_TAPS
#define SPEED_HIP_BINDING_SETS 2u
#define SPEED_HIP_GROUP 256u       /* score work-group; covariance upper bound */
#define SPEED_HIP_LINALG_GROUP 64u /* 25x25 linear algebra */
#define SPEED_HIP_IMAGE_TILE 16u   /* 2-D pixel kernels */
#define SPEED_HIP_ITEMS_BLOCK 128u /* 1-D item kernels */
#define SPEED_HIP_DECIMATION 16u   /* 2^NUM_SCALES */
#define SPEED_HIP_QR_CAP 500u      /* EIGENVALUE_MAX_ITERS */
#define SPEED_HIP_PICTURE_OFFSET (-128.0f)
#define SPEED_HIP_ELEMENTS_F 25.0f
#define SPEED_HIP_EPS_HI 0x1.0c6f7ap-20f /* (float)EIGENVALUE_EPS */
#define SPEED_HIP_EPS_LO 0x1.6bdb1ap-49f /* EIGENVALUE_EPS - SPEED_HIP_EPS_HI, exact */
/* Linear-algebra shared memory: six 25x25 matrices and five vectors. */
#define SPEED_HIP_SLM_STRIDE 640u
#define SPEED_HIP_SLM_VECTOR 32u
#define SPEED_HIP_SLM_FLOATS (6u * SPEED_HIP_SLM_STRIDE + 5u * SPEED_HIP_SLM_VECTOR)

/* ------------------------------------------------------------------ */
/* Layout shared with the host (speed_hip_pipeline.c).                 */
/* ------------------------------------------------------------------ */

/* Everything the kernels read, in device memory, written once at init.
 * Every kernel takes (const SpeedHipParams *, uint32_t binding_set). */
typedef struct SpeedHipParams {
    const uint8_t *raw; /* raw planes, raw_planes x plane_bytes */
    float *taps;        /* antialias[128], then lowpass[128] */
    float *scaled;      /* channels x scaled_h x scaled_w, prescale only */
    float *down;        /* channels x down_h x down_w */
    float *centered;    /* channels x trunc_h x trunc_w */
    float *indterm;     /* channels x 25 x blocks */
    float *means;       /* channels x 25 */
    float *cov;         /* channels x 625 */
    float *eig;         /* channels x 25 */
    float *qmat;        /* channels x 625, accumulated reflector product */
    float *rmat;        /* channels x 625 */
    float *var;         /* channels x blocks */
    float *ent;         /* channels x blocks */
    float *contrib;     /* pairs x blocks */
    int32_t *status;    /* channels x 2: singular, iteration cap */
    SpeedGpuFrameResult *result;
    SpeedGpuGeometry geometry;
    SpeedGpuScoring scoring;
    SpeedGpuChannelBinding bindings[SPEED_HIP_BINDING_SETS][SPEED_HIP_MAX_CHANNELS];
    uint32_t plane_bytes;
    uint32_t channels; /* 2 (one score pair) or 4 (two pairs) */
    uint32_t antialias_width;
    uint32_t lowpass_width;
    uint32_t cov_group; /* covariance work-group size, a power of two */
    uint32_t reserved;
} SpeedHipParams;

/* The host C compiler and hipcc must agree on every offset. */
SPEED_HIP_STATIC_ASSERT(sizeof(SpeedGpuGeometry) == 64u, "SpeedGpuGeometry layout");
SPEED_HIP_STATIC_ASSERT(sizeof(SpeedGpuFrameResult) == 40u, "SpeedGpuFrameResult layout");
SPEED_HIP_STATIC_ASSERT(sizeof(SpeedHipParams) == 16u * 8u + 64u + 16u + 64u + 24u,
                        "SpeedHipParams layout");

/* ------------------------------------------------------------------ */
/* Bit casts, fused multiply-add and the exact fp32 pair arithmetic.   */
/* ------------------------------------------------------------------ */

static inline SPEED_HD uint32_t speed_hd_bits(float value)
{
#if defined(__HIP_DEVICE_COMPILE__)
    return __float_as_uint(value);
#else
    uint32_t bits = 0u;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
#endif
}

static inline SPEED_HD float speed_hd_float(uint32_t bits)
{
#if defined(__HIP_DEVICE_COMPILE__)
    return __uint_as_float(bits);
#else
    float value = 0.0f;
    memcpy(&value, &bits, sizeof(value));
    return value;
#endif
}

/* An explicit fused multiply-add: one rounding by definition, independent of
 * -ffp-contract. */
static inline SPEED_HD float speed_hd_fma(float a, float b, float c)
{
    return fmaf(a, b, c);
}

typedef struct SpeedHdFf {
    float hi;
    float lo;
} SpeedHdFf;

static inline SPEED_HD SpeedHdFf speed_hd_ff(float hi, float lo)
{
    SpeedHdFf r;
    r.hi = hi;
    r.lo = lo;
    return r;
}

static inline SPEED_HD SpeedHdFf speed_hd_two_sum(float a, float b)
{
    const float sum = a + b;
    const float b_virtual = sum - a;
    const float a_virtual = sum - b_virtual;
    const float b_error = b - b_virtual;
    const float a_error = a - a_virtual;
    return speed_hd_ff(sum, a_error + b_error);
}

static inline SPEED_HD SpeedHdFf speed_hd_quick_two_sum(float a, float b)
{
    const float sum = a + b;
    const float rebuilt = sum - a;
    return speed_hd_ff(sum, b - rebuilt);
}

static inline SPEED_HD SpeedHdFf speed_hd_two_prod(float a, float b)
{
    const float product = a * b;
    return speed_hd_ff(product, speed_hd_fma(a, b, -product));
}

static inline SPEED_HD SpeedHdFf speed_hd_ff_add(SpeedHdFf a, SpeedHdFf b)
{
    const SpeedHdFf high = speed_hd_two_sum(a.hi, b.hi);
    const SpeedHdFf low = speed_hd_two_sum(a.lo, b.lo);
    const SpeedHdFf first = speed_hd_quick_two_sum(high.hi, high.lo + low.hi);
    return speed_hd_quick_two_sum(first.hi, low.lo + first.lo);
}

static inline SPEED_HD SpeedHdFf speed_hd_ff_mul(SpeedHdFf a, SpeedHdFf b)
{
    const SpeedHdFf product = speed_hd_two_prod(a.hi, b.hi);
    const float cross1 = a.hi * b.lo;
    const float cross2 = a.lo * b.hi;
    const float cross = cross1 + cross2;
    return speed_hd_quick_two_sum(product.hi, product.lo + cross);
}

/* (hi + lo) / divisor, rounded once to fp32. */
static inline SPEED_HD float speed_hd_ff_div_to_float(SpeedHdFf value, float divisor)
{
    const float quotient = value.hi / divisor;
    const float remainder = speed_hd_fma(-quotient, divisor, value.hi);
    const float correction = (remainder + value.lo) / divisor;
    return quotient + correction;
}

/* speed.c compares against EIGENVALUE_EPS = 1e-6, an fp64 constant, after
 * promoting the fp32 operands. `a < 1e-6 * s` is decided exactly here from
 * EPS_HI + EPS_LO == 1e-6 and two exact products. */
static inline SPEED_HD int speed_hd_below_eps_scaled(float a, float s)
{
    const SpeedHdFf major = speed_hd_two_prod(SPEED_HIP_EPS_HI, s);
    const SpeedHdFf minor = speed_hd_two_prod(SPEED_HIP_EPS_LO, s);
    const float tail = (major.lo + minor.hi) + minor.lo;
    if (a < 0.5f * major.hi || a > 2.0f * major.hi)
        return a < major.hi;
    const float gap = a - major.hi; /* exact (Sterbenz) */
    return gap < tail;
}

/* `x < 1e-6` with fp64 promotion: no fp32 value lies strictly between
 * EPS_HI and 1e-6, so this is `x <= EPS_HI`. */
static inline SPEED_HD int speed_hd_below_eps(float x)
{
    return x <= SPEED_HIP_EPS_HI;
}

/* ------------------------------------------------------------------ */
/* log2f() of the reference, correctly rounded via fp32 pairs.         */
/* ------------------------------------------------------------------ */

/* 1/3, 1/5, ..., 1/21 as exact fp32 pairs. */
static inline SPEED_HD SpeedHdFf speed_hd_inverse_odd(uint32_t k)
{
    static const float hi[10] = {0x1.555556p-2f, 0x1.99999ap-3f, 0x1.24924ap-3f, 0x1.c71c72p-4f,
                                 0x1.745d18p-4f, 0x1.3b13b2p-4f, 0x1.111112p-4f, 0x1.e1e1e2p-5f,
                                 0x1.af286cp-5f, 0x1.861862p-5f};
    static const float lo[10] = {
        -0x1.555556p-27f, -0x1.99999ap-29f, -0x1.b6db6ep-28f, -0x1.c71c72p-31f, -0x1.745d18p-29f,
        -0x1.89d89ep-29f, -0x1.dddddep-29f, -0x1.e1e1e2p-33f, -0x1.af286cp-32f, -0x1.e79e7ap-31f};
    return speed_hd_ff(hi[k], lo[k]);
}

/* ln(m) = 2 atanh(s), s = (m - 1)/(m + 1), m in [sqrt(1/2), sqrt(2)],
 * |s| <= 0.1716; the series stops at s^21. */
static inline SPEED_HD SpeedHdFf speed_hd_ln_mantissa(float m)
{
    const float num = m - 1.0f;                      /* exact */
    const SpeedHdFf den = speed_hd_two_sum(m, 1.0f); /* exact */
    const float s_hi = num / den.hi;
    const float residual = speed_hd_fma(-s_hi, den.hi, num);
    const float tail = s_hi * den.lo;
    const float s_lo = (residual - tail) / den.hi;
    const SpeedHdFf s = speed_hd_quick_two_sum(s_hi, s_lo);
    const SpeedHdFf u = speed_hd_ff_mul(s, s);
    SpeedHdFf poly = speed_hd_inverse_odd(9u);
    for (uint32_t k = 9u; k > 0u; k--)
        poly = speed_hd_ff_add(speed_hd_ff_mul(poly, u), speed_hd_inverse_odd(k - 1u));
    poly = speed_hd_ff_add(speed_hd_ff_mul(poly, u), speed_hd_ff(1.0f, 0.0f));
    const SpeedHdFf half = speed_hd_ff_mul(s, poly);
    return speed_hd_ff(2.0f * half.hi, 2.0f * half.lo);
}

/* log2 correctly rounded to fp32: evaluated in fp32 pairs to about 2^-45 and
 * rounded once. The device builtin is not correctly rounded (ADR-1358). */
static inline SPEED_HD float speed_hd_log2_rn(float x)
{
    if (!(x > 0.0f))
        return x == 0.0f ? -HUGE_VALF : NAN;
    if (x == HUGE_VALF)
        return x;
    uint32_t bits = speed_hd_bits(x);
    int32_t exponent = 0;
    if (bits < 0x00800000u) { /* subnormal */
        bits = speed_hd_bits(x * 0x1p23f);
        exponent = -23;
    }
    exponent += (int32_t)(bits >> 23u) - 127;
    float m = speed_hd_float((bits & 0x007fffffu) | 0x3f800000u);
    if (m > 0x1.6a09e6p+0f) { /* sqrt(2) */
        m = m * 0.5f;
        exponent += 1;
    }
    const SpeedHdFf log2e = speed_hd_ff(0x1.715476p+0f, 0x1.4ae0c0p-26f);
    const SpeedHdFf log2m = speed_hd_ff_mul(speed_hd_ln_mantissa(m), log2e);
    return speed_hd_ff_add(speed_hd_ff((float)exponent, 0.0f), log2m).hi;
}

/* log2f() of the reference. The CPU extractor calls its libm's log2f, which
 * is correctly rounded (or nearly: libimf, the icx build's libm) or not
 * (glibc misrounds about 0.4% of arguments in [1, 8)); the device always
 * rounds correctly. SPEED_HD_HOST_LIBM_LOG2 is a host-only test seam: the
 * unit test defines it to replay the chain with the CPU's own log2f, which
 * isolates every other operation for a bit-exact comparison, and checks
 * speed_hd_log2_rn() separately. It cannot reach device code. */
static inline SPEED_HD float speed_hd_log2(float x)
{
#if defined(SPEED_HD_HOST_LIBM_LOG2) && !defined(__HIP_DEVICE_COMPILE__)
    return log2f(x);
#else
    return speed_hd_log2_rn(x);
#endif
}

/* ------------------------------------------------------------------ */
/* Picture sources.                                                    */
/* ------------------------------------------------------------------ */

/* convolution_reflect101(), convolution_internal.h. */
static inline SPEED_HD uint32_t speed_hd_reflect101(int32_t index, uint32_t size)
{
    const int32_t n = (int32_t)size;
    if (n <= 1)
        return 0u;
    int32_t folded = index;
    for (uint32_t bounce = 0u; bounce < 8u && (folded < 0 || folded >= n); bounce++)
        folded = folded < 0 ? -folded : 2 * n - folded - 2;
    return (uint32_t)folded;
}

/* Whether every tap of a `2 * radius + 1` window around `centre` is inside
 * [0, size), so the reflect-101 fold can be skipped. */
static inline SPEED_HD int speed_hd_window_inside(int32_t centre, int32_t radius, uint32_t size)
{
    return centre >= radius && centre + radius < (int32_t)size;
}

/* Plane index of tap `k` around `centre`. */
static inline SPEED_HD uint32_t speed_hd_tap(int32_t centre, int32_t radius, uint32_t k,
                                             uint32_t size, int inside)
{
    const int32_t index = centre - radius + (int32_t)k;
    return inside ? (uint32_t)index : speed_hd_reflect101(index, size);
}

/* picture_copy(): 8-bit `(float)v + offset`, else `(float)v / scaler + offset`. */
static inline SPEED_HD float speed_hd_sample(const SpeedGpuGeometry *g, const uint8_t *plane,
                                             size_t offset)
{
    if (g->bytes_per_sample == 1u)
        return (float)plane[offset] + SPEED_HIP_PICTURE_OFFSET;
    const float sample = (float)((const uint16_t *)(const void *)plane)[offset];
    const float scaled = sample / g->sample_scale;
    return scaled + SPEED_HIP_PICTURE_OFFSET;
}

/* Converted picture, or the temporal difference `minuend - subtrahend` of two
 * converted pictures (speed.c extract(): subtract_image()). */
static inline SPEED_HD float speed_hd_raw_at(const SpeedHipParams *p, uint32_t set, uint32_t ch,
                                             uint32_t row, uint32_t col)
{
    const SpeedGpuChannelBinding b = p->bindings[set][ch];
    const size_t offset = (size_t)row * p->geometry.src_w + col;
    const float value =
        speed_hd_sample(&p->geometry, p->raw + (size_t)b.minuend * p->plane_bytes, offset);
    if (b.subtrahend < 0)
        return value;
    const float other =
        speed_hd_sample(&p->geometry, p->raw + (size_t)b.subtrahend * p->plane_bytes, offset);
    return value - other;
}

/* The plane the anti-alias filter reads: the prescaled float plane, or the
 * raw planes when no resample is needed. */
static inline SPEED_HD float speed_hd_source_at(const SpeedHipParams *p, uint32_t set, uint32_t ch,
                                                uint32_t row, uint32_t col)
{
    const SpeedGpuGeometry *g = &p->geometry;
    if (g->prescale == 0)
        return speed_hd_raw_at(p, set, ch, row, col);
    return p->scaled[((size_t)ch * g->scaled_h + row) * g->scaled_w + col];
}

/* ------------------------------------------------------------------ */
/* Prescale: vif_scale_frame_s(), vif_tools.c.                         */
/* ------------------------------------------------------------------ */

/* `(y + 0.5) * ratio - 0.5`, evaluated by the reference in fp64 and rounded
 * once to fp32. Every intermediate below is exact. */
static inline SPEED_HD float speed_hd_centre_coordinate(uint32_t index, float ratio)
{
    const float centre = (float)index + 0.5f;
    const SpeedHdFf scaled = speed_hd_two_prod(centre, ratio);
    const SpeedHdFf shifted = speed_hd_two_sum(scaled.hi, -0.5f);
    const float tail = shifted.lo + scaled.lo;
    return shifted.hi + tail;
}

/* mirror(), vif_tools.c (float arguments). */
static inline SPEED_HD float speed_hd_mirror(float i, float right)
{
    if (i < 0.0f)
        return -i;
    if (i > right) {
        const float twice = 2.0f * right;
        return twice - i;
    }
    return i;
}

static inline SPEED_HD float speed_hd_scale_bilinear(const SpeedHipParams *p, uint32_t set,
                                                     uint32_t ch, float x, float y)
{
    const float right = (float)(p->geometry.src_w - 1u);
    const float bottom = (float)(p->geometry.src_h - 1u);
    const int32_t x1 = (int32_t)speed_hd_mirror(floorf(x), right);
    const int32_t x2 = (int32_t)speed_hd_mirror(ceilf(x), right);
    const int32_t y1 = (int32_t)speed_hd_mirror(floorf(y), bottom);
    const int32_t y2 = (int32_t)speed_hd_mirror(ceilf(y), bottom);
    const float dx = x - (float)x1;
    const float dy = y - (float)y1;
    const float ix = 1.0f - dx;
    const float iy = 1.0f - dy;
    const float w11 = iy * ix;
    const float w12 = iy * dx;
    const float w21 = dy * ix;
    const float w22 = dy * dx;
    const float t11 = w11 * speed_hd_raw_at(p, set, ch, (uint32_t)y1, (uint32_t)x1);
    const float t12 = w12 * speed_hd_raw_at(p, set, ch, (uint32_t)y1, (uint32_t)x2);
    const float t21 = w21 * speed_hd_raw_at(p, set, ch, (uint32_t)y2, (uint32_t)x1);
    const float t22 = w22 * speed_hd_raw_at(p, set, ch, (uint32_t)y2, (uint32_t)x2);
    const float s1 = t11 + t12;
    const float s2 = s1 + t21;
    return s2 + t22;
}

/* bicubic_kernel(), vif_tools.c. */
static inline SPEED_HD float speed_hd_bicubic_weight(float t)
{
    const float a = -0.75f;
    const float u = t < 0.0f ? -t : t;
    if (u < 1.0f) {
        const float c1 = (a + 2.0f) * u;
        const float c2 = c1 - (a + 3.0f);
        const float c3 = c2 * u;
        const float c4 = c3 * u;
        return c4 + 1.0f;
    }
    if (u < 2.0f) {
        const float c1 = (u - 5.0f) * u;
        const float c2 = (c1 + 8.0f) * u;
        return (c2 - 4.0f) * a;
    }
    return 0.0f;
}

static inline SPEED_HD float speed_hd_scale_bicubic(const SpeedHipParams *p, uint32_t set,
                                                    uint32_t ch, float x, float y)
{
    const int32_t x0 = (int32_t)floorf(x);
    const int32_t y0 = (int32_t)floorf(y);
    const float dx = x - (float)x0;
    const float dy = y - (float)y0;
    float wx[4];
    float wy[4];
    for (int32_t i = -1; i <= 2; i++) {
        wx[i + 1] = speed_hd_bicubic_weight((float)i - dx);
        wy[i + 1] = speed_hd_bicubic_weight((float)i - dy);
    }
    const float right = (float)(p->geometry.src_w - 1u);
    const float bottom = (float)(p->geometry.src_h - 1u);
    float value = 0.0f;
    for (int32_t j = -1; j <= 2; j++) {
        for (int32_t i = -1; i <= 2; i++) {
            const int32_t xi = (int32_t)speed_hd_mirror((float)(x0 + i), right);
            const int32_t yj = (int32_t)speed_hd_mirror((float)(y0 + j), bottom);
            const float weight = wx[i + 1] * wy[j + 1];
            const float term = speed_hd_raw_at(p, set, ch, (uint32_t)yj, (uint32_t)xi) * weight;
            value = value + term;
        }
    }
    return value;
}

/* sin(pi x): the device's fp32 sinpi. The reference evaluates the lanczos
 * weight in fp64 with sin(), so that option is within the ADR-0214 tolerance
 * but not bit for bit (ADR-1358); the host fallback only lets the unit test
 * compile the path, which it does not score. */
static inline SPEED_HD float speed_hd_sinpi(float x)
{
#if defined(__HIP_DEVICE_COMPILE__)
    return sinpif(x);
#else
    return sinf(x * 0x1.921fb6p+1f);
#endif
}

/* lanczos4_kernel(), vif_tools.c. */
static inline SPEED_HD float speed_hd_lanczos_weight(float x)
{
    const float a = 4.0f;
    if (x == 0.0f)
        return 1.0f;
    if (x > -a && x < a) {
        const float pi = 0x1.921fb6p+1f;
        const float s1 = speed_hd_sinpi(x);
        const float s2 = speed_hd_sinpi(x / a);
        const float num = (a * s1) * s2;
        const float den = ((pi * pi) * x) * x;
        return num / den;
    }
    return 0.0f;
}

static inline SPEED_HD float speed_hd_scale_lanczos(const SpeedHipParams *p, uint32_t set,
                                                    uint32_t ch, float x, float y)
{
    const int32_t x0 = (int32_t)floorf(x);
    const int32_t y0 = (int32_t)floorf(y);
    const float dx = x - (float)x0;
    const float dy = y - (float)y0;
    float wx[9];
    float wy[9];
    for (int32_t i = -4; i <= 4; i++) {
        wx[i + 4] = speed_hd_lanczos_weight((float)i - dx);
        wy[i + 4] = speed_hd_lanczos_weight((float)i - dy);
    }
    const float right = (float)(p->geometry.src_w - 1u);
    const float bottom = (float)(p->geometry.src_h - 1u);
    float value = 0.0f;
    float weight_sum = 0.0f;
    for (int32_t iy = -4; iy <= 4; iy++) {
        for (int32_t ix = -4; ix <= 4; ix++) {
            const float weight = wx[ix + 4] * wy[iy + 4];
            weight_sum = weight_sum + weight;
            const int32_t xi = (int32_t)speed_hd_mirror((float)(x0 + ix), right);
            const int32_t yi = (int32_t)speed_hd_mirror((float)(y0 + iy), bottom);
            const float term = speed_hd_raw_at(p, set, ch, (uint32_t)yi, (uint32_t)xi) * weight;
            value = value + term;
        }
    }
    return value / weight_sum;
}

/* One output sample of vif_scale_frame_s(). */
static inline SPEED_HD float speed_hd_scale_sample(const SpeedHipParams *p, uint32_t set,
                                                   uint32_t ch, uint32_t y, uint32_t x)
{
    const SpeedGpuGeometry *g = &p->geometry;
    if (g->src_w == g->scaled_w && g->src_h == g->scaled_h)
        return speed_hd_raw_at(p, set, ch, y, x);
    const float ratio_x = (float)g->src_w / (float)g->scaled_w;
    const float ratio_y = (float)g->src_h / (float)g->scaled_h;
    if (g->scale_method == 0) { /* vif_scale_nearest */
        const uint32_t sy = (uint32_t)((float)y * ratio_y);
        const uint32_t sx = (uint32_t)((float)x * ratio_x);
        return speed_hd_raw_at(p, set, ch, sy, sx);
    }
    const float xx = speed_hd_centre_coordinate(x, ratio_x);
    const float yy = speed_hd_centre_coordinate(y, ratio_y);
    if (g->scale_method == 1) /* vif_scale_bicubic */
        return speed_hd_scale_bicubic(p, set, ch, xx, yy);
    if (g->scale_method == 2) /* vif_scale_lanczos4 */
        return speed_hd_scale_lanczos(p, set, ch, xx, yy);
    return speed_hd_scale_bilinear(p, set, ch, xx, yy); /* vif_scale_bilinear */
}

/* ------------------------------------------------------------------ */
/* Anti-alias filter at the decimated sample points: vif_filter1d_s()  */
/* (vertical then horizontal) + vif_dec16_s(); then the local mean     */
/* subtraction and independent term of filter_and_downscale() /        */
/* compute_independent_term().                                          */
/* ------------------------------------------------------------------ */

static inline SPEED_HD float speed_hd_antialias_at(const SpeedHipParams *p, uint32_t set,
                                                   uint32_t ch, uint32_t i, uint32_t j)
{
    const SpeedGpuGeometry *g = &p->geometry;
    const uint32_t width = p->antialias_width;
    const int32_t radius = (int32_t)(width / 2u);
    const int32_t row = (int32_t)(i * SPEED_HIP_DECIMATION);
    const int32_t col = (int32_t)(j * SPEED_HIP_DECIMATION);
    const int rows_inside = speed_hd_window_inside(row, radius, g->scaled_h);
    const int cols_inside = speed_hd_window_inside(col, radius, g->scaled_w);
    float horizontal = 0.0f;
    for (uint32_t kx = 0u; kx < width; kx++) {
        const uint32_t c = speed_hd_tap(col, radius, kx, g->scaled_w, cols_inside);
        float vertical = 0.0f;
        for (uint32_t ky = 0u; ky < width; ky++) {
            const uint32_t r = speed_hd_tap(row, radius, ky, g->scaled_h, rows_inside);
            const float product = p->taps[ky] * speed_hd_source_at(p, set, ch, r, c);
            vertical = vertical + product;
        }
        const float product = p->taps[kx] * vertical;
        horizontal = horizontal + product;
    }
    return horizontal;
}

static inline SPEED_HD float speed_hd_lowpass_at(const SpeedHipParams *p, const float *plane,
                                                 uint32_t i, uint32_t j)
{
    const SpeedGpuGeometry *g = &p->geometry;
    const float *taps = p->taps + SPEED_HIP_MAX_TAPS;
    const uint32_t width = p->lowpass_width;
    const int32_t radius = (int32_t)(width / 2u);
    const int rows_inside = speed_hd_window_inside((int32_t)i, radius, g->down_h);
    const int cols_inside = speed_hd_window_inside((int32_t)j, radius, g->down_w);
    float horizontal = 0.0f;
    for (uint32_t kx = 0u; kx < width; kx++) {
        const uint32_t c = speed_hd_tap((int32_t)j, radius, kx, g->down_w, cols_inside);
        float vertical = 0.0f;
        for (uint32_t ky = 0u; ky < width; ky++) {
            const uint32_t r = speed_hd_tap((int32_t)i, radius, ky, g->down_h, rows_inside);
            const float product = taps[ky] * plane[(size_t)r * g->down_w + c];
            vertical = vertical + product;
        }
        const float product = taps[kx] * vertical;
        horizontal = horizontal + product;
    }
    return horizontal;
}

/* One truncated-plane pixel: the centred value and its independent term. */
static inline SPEED_HD void speed_hd_centre_item(const SpeedHipParams *p, uint32_t ch, uint32_t i,
                                                 uint32_t j)
{
    const SpeedGpuGeometry *g = &p->geometry;
    const float *plane = p->down + (size_t)ch * g->down_h * g->down_w;
    const float smooth = speed_hd_lowpass_at(p, plane, i, j);
    const float value = plane[(size_t)i * g->down_w + j] - smooth;
    const size_t plane_size = (size_t)g->trunc_h * g->trunc_w;
    p->centered[ch * plane_size + (size_t)i * g->trunc_w + j] = value;
    const uint32_t element = (i % SPEED_HIP_BLOCK) * SPEED_HIP_BLOCK + (j % SPEED_HIP_BLOCK);
    const uint32_t tile = (i / SPEED_HIP_BLOCK) * g->blocks_h + (j / SPEED_HIP_BLOCK);
    const size_t term_size = (size_t)SPEED_HIP_N * g->blocks;
    p->indterm[ch * term_size + (size_t)element * g->blocks + tile] = value;
}

/* ------------------------------------------------------------------ */
/* Means and covariance: compute_mean() and compute_covariance_matrix() */
/* ------------------------------------------------------------------ */

/* compute_mean(): one sequential fp32 sum per element, then the division. */
static inline SPEED_HD float speed_hd_submatrix_mean(const SpeedHipParams *p, uint32_t ch,
                                                     uint32_t element)
{
    const SpeedGpuGeometry *g = &p->geometry;
    const float *plane = p->centered + (size_t)ch * g->trunc_h * g->trunc_w;
    const uint32_t row0 = element / SPEED_HIP_BLOCK;
    const uint32_t col0 = element % SPEED_HIP_BLOCK;
    float sum = 0.0f;
    for (uint32_t i = 0u; i < g->sub_h; i++) {
        const float *row = plane + (size_t)(row0 + i) * g->trunc_w + col0;
        for (uint32_t j = 0u; j < g->sub_w; j++)
            sum = sum + row[j];
    }
    const float count = (float)(g->sub_w * g->sub_h);
    return sum / count;
}

/* Entry `index` of the lower triangle, row-major: (x, y) with y <= x. */
static inline SPEED_HD void speed_hd_triangle_entry(uint32_t index, uint32_t *x, uint32_t *y)
{
    uint32_t rest = index;
    *x = 0u;
    for (uint32_t row = 0u; row < SPEED_HIP_N; row++) {
        if (rest <= row) {
            *x = row;
            break;
        }
        rest -= row + 1u;
    }
    *y = rest;
}

/* (vx - mx) * (vy - my) as an exact fp32 pair (the reference sums it in fp64). */
static inline SPEED_HD SpeedHdFf speed_hd_centred_product(float vx, float mx, float vy, float my)
{
    const SpeedHdFf dx = speed_hd_two_sum(vx, -mx);
    const SpeedHdFf dy = speed_hd_two_sum(vy, -my);
    const SpeedHdFf main = speed_hd_two_prod(dx.hi, dy.hi);
    const float cross1 = dx.hi * dy.lo;
    const float cross2 = dx.lo * dy.hi;
    const float cross = cross1 + cross2;
    return speed_hd_ff(main.hi, main.lo + cross);
}

/* Compensated accumulation of one term: the high parts are summed exactly,
 * every low-order part lands in `lo`. */
static inline SPEED_HD SpeedHdFf speed_hd_accumulate(SpeedHdFf acc, SpeedHdFf term)
{
    const SpeedHdFf high = speed_hd_two_sum(acc.hi, term.hi);
    const float low = term.lo + high.lo;
    return speed_hd_ff(high.hi, acc.lo + low);
}

/* Lane `lid` of a `group`-wide covariance work-group: its strided share of
 * the submatrix sum for entry (x, y) of channel ch, normalised. */
static inline SPEED_HD SpeedHdFf speed_hd_covariance_partial(const SpeedHipParams *p, uint32_t ch,
                                                             uint32_t x, uint32_t y, uint32_t lid,
                                                             uint32_t group)
{
    const SpeedGpuGeometry *g = &p->geometry;
    const float *plane = p->centered + (size_t)ch * g->trunc_h * g->trunc_w;
    const float mx = p->means[ch * SPEED_HIP_N + x];
    const float my = p->means[ch * SPEED_HIP_N + y];
    const uint32_t xr = x / SPEED_HIP_BLOCK;
    const uint32_t xc = x % SPEED_HIP_BLOCK;
    const uint32_t yr = y / SPEED_HIP_BLOCK;
    const uint32_t yc = y % SPEED_HIP_BLOCK;
    const uint32_t total = g->sub_w * g->sub_h;
    SpeedHdFf acc = speed_hd_ff(0.0f, 0.0f);
    for (uint32_t pos = lid; pos < total; pos += group) {
        const uint32_t row = pos / g->sub_w;
        const uint32_t col = pos % g->sub_w;
        const float vx = plane[(size_t)(xr + row) * g->trunc_w + xc + col];
        const float vy = plane[(size_t)(yr + row) * g->trunc_w + yc + col];
        acc = speed_hd_accumulate(acc, speed_hd_centred_product(vx, mx, vy, my));
    }
    return speed_hd_quick_two_sum(acc.hi, acc.lo);
}

/* The reduced sum divided by the element count, rounded once, into both
 * symmetric entries of the channel's matrix. */
static inline SPEED_HD void speed_hd_covariance_store(const SpeedHipParams *p, uint32_t ch,
                                                      uint32_t x, uint32_t y, SpeedHdFf sum)
{
    const float count = (float)(p->geometry.sub_w * p->geometry.sub_h);
    const float value = speed_hd_ff_div_to_float(sum, count);
    float *matrix = p->cov + (size_t)ch * SPEED_HIP_MATRIX;
    matrix[x * SPEED_HIP_N + y] = value;
    matrix[y * SPEED_HIP_N + x] = value;
}

/* Enough work-items per covariance entry to cover the submatrix, no more. */
static inline SPEED_HD uint32_t speed_hd_covariance_group_size(uint32_t terms)
{
    uint32_t group = 32u;
    while (group < SPEED_HIP_GROUP && group < terms / 8u)
        group *= 2u;
    return group;
}

/* ------------------------------------------------------------------ */
/* Eigenvalues: compute_eigenvalues() (Householder tridiagonalisation  */
/* + implicit-shift QR), regularity, matrix_qr_decomposition().        */
/* ------------------------------------------------------------------ */

typedef struct SpeedHdLanes {
    uint32_t lid;
    uint32_t count;
} SpeedHdLanes;

typedef struct SpeedHdSlm {
    float *a;   /* tridiagonalisation work */
    float *cov; /* covariance, untouched */
    float *z;   /* QR working matrix */
    float *q;   /* accumulated reflectors */
    float *h;   /* current reflector */
    float *t;   /* product scratch */
    float *vec;
    float *x;
    float *d;
    float *sd;
    float *scalar; /* [0] tau, [2] norm, [3] regular, [4] capped, [5] s, [6] beta */
} SpeedHdSlm;

static inline SPEED_HD SpeedHdSlm speed_hd_slm_layout(float *base)
{
    float *vectors = base + 6u * SPEED_HIP_SLM_STRIDE;
    SpeedHdSlm m;
    m.a = base;
    m.cov = base + SPEED_HIP_SLM_STRIDE;
    m.z = base + 2u * SPEED_HIP_SLM_STRIDE;
    m.q = base + 3u * SPEED_HIP_SLM_STRIDE;
    m.h = base + 4u * SPEED_HIP_SLM_STRIDE;
    m.t = base + 5u * SPEED_HIP_SLM_STRIDE;
    m.vec = vectors;
    m.x = vectors + SPEED_HIP_SLM_VECTOR;
    m.d = vectors + 2u * SPEED_HIP_SLM_VECTOR;
    m.sd = vectors + 3u * SPEED_HIP_SLM_VECTOR;
    m.scalar = vectors + 4u * SPEED_HIP_SLM_VECTOR;
    return m;
}

static inline SPEED_HD float speed_hd_sign_of(float x)
{
    return x >= 0.0f ? 1.0f : -1.0f;
}

/* pythagoras(), speed.c. */
static inline SPEED_HD float speed_hd_pythagoras(float x, float y)
{
    const float xx = x * x;
    const float yy = y * y;
    return sqrtf(xx + yy);
}

/* compute_column_norm(), speed.c. */
static inline SPEED_HD float speed_hd_column_norm(const float *a, uint32_t col, uint32_t start)
{
    float norm = 0.0f;
    for (uint32_t r = start; r < SPEED_HIP_N; r++) {
        const float value = a[r * SPEED_HIP_N + col];
        const float square = value * value;
        norm = norm + square;
    }
    return sqrtf(norm);
}

/* The scalar half of compute_householder_transform(), speed.c, on one lane:
 * tau, the column divisor s (0 when the column is left alone) and beta. */
static inline SPEED_HD void speed_hd_householder_scalars(const SpeedHdSlm *m, uint32_t col,
                                                         uint32_t start)
{
    m->scalar[0] = 0.0f;
    m->scalar[5] = 0.0f;
    if (SPEED_HIP_N - start == 1u)
        return;
    const float xnorm = speed_hd_column_norm(m->a, col, start + 1u);
    if (xnorm == 0.0f)
        return;
    const float alpha = m->a[start * SPEED_HIP_N + col];
    const float beta = -speed_hd_sign_of(alpha) * speed_hd_pythagoras(alpha, xnorm);
    m->scalar[0] = (beta - alpha) / beta;
    m->scalar[5] = alpha - beta;
    m->scalar[6] = beta;
}

/* `A[i][col] /= s` for the column, then `A[start][col] = beta`. */
static inline SPEED_HD void speed_hd_householder_scale(const SpeedHdLanes *g, const SpeedHdSlm *m,
                                                       uint32_t col, uint32_t start)
{
    const float s = m->scalar[5];
    if (s == 0.0f)
        return;
    for (uint32_t r = start + g->lid; r < SPEED_HIP_N; r += g->count)
        m->a[r * SPEED_HIP_N + col] = m->a[r * SPEED_HIP_N + col] / s;
    SPEED_HD_SYNC();
    if (g->lid == 0u)
        m->a[start * SPEED_HIP_N + col] = m->scalar[6];
    SPEED_HD_SYNC();
}

/* x = tau * A * v, then x += alpha * v (tridiagonal_multiply/_axpy). */
static inline SPEED_HD void speed_hd_tridiagonal_vectors(const SpeedHdLanes *g, const SpeedHdSlm *m,
                                                         uint32_t start, float tau)
{
    for (uint32_t r = start + g->lid; r < SPEED_HIP_N; r += g->count) {
        float acc = 0.0f;
        for (uint32_t c = start; c < SPEED_HIP_N; c++) {
            const float scaled = tau * m->a[r * SPEED_HIP_N + c];
            const float product = scaled * m->vec[c];
            acc = acc + product;
        }
        m->x[r] = acc;
    }
    SPEED_HD_SYNC();
    if (g->lid == 0u) {
        float xv = 0.0f;
        for (uint32_t r = start; r < SPEED_HIP_N; r++) {
            const float product = m->x[r] * m->vec[r];
            xv = xv + product;
        }
        const float half_tau = -0.5f * tau;
        m->scalar[1] = half_tau * xv;
    }
    SPEED_HD_SYNC();
    const float alpha = m->scalar[1];
    for (uint32_t r = start + g->lid; r < SPEED_HIP_N; r += g->count) {
        const float product = alpha * m->vec[r];
        m->x[r] = m->x[r] + product;
    }
    SPEED_HD_SYNC();
}

/* Load the Householder vector of column i into vec (lane 0). */
static inline SPEED_HD void speed_hd_householder_vector(const SpeedHdSlm *m, uint32_t i,
                                                        uint32_t start, float tau)
{
    for (uint32_t j = start; j < SPEED_HIP_N; j++)
        m->vec[j] = m->a[j * SPEED_HIP_N + i];
    if (tau != 0.0f) {
        m->a[start * SPEED_HIP_N + i] = m->vec[start];
        m->vec[start] = 1.0f;
    }
}

/* One step of convert_to_tridiagonal(), speed.c. */
static inline SPEED_HD void speed_hd_tridiagonal_step(const SpeedHdLanes *g, const SpeedHdSlm *m,
                                                      uint32_t i)
{
    const uint32_t start = i + 1u;
    if (g->lid == 0u)
        speed_hd_householder_scalars(m, i, start);
    SPEED_HD_SYNC();
    speed_hd_householder_scale(g, m, i, start);
    const float tau = m->scalar[0];
    if (g->lid == 0u)
        speed_hd_householder_vector(m, i, start, tau);
    SPEED_HD_SYNC();
    if (tau == 0.0f)
        return;
    speed_hd_tridiagonal_vectors(g, m, start, tau);
    const uint32_t span = SPEED_HIP_N - start;
    for (uint32_t idx = g->lid; idx < span * span; idx += g->count) {
        const uint32_t r = start + idx / span;
        const uint32_t c = start + idx % span;
        const float p1 = m->x[r] * m->vec[c];
        const float p2 = m->vec[r] * m->x[c];
        const float both = p1 + p2;
        m->a[r * SPEED_HIP_N + c] = m->a[r * SPEED_HIP_N + c] - both;
    }
    SPEED_HD_SYNC();
}

/* chop_small_elements(), speed.c. */
static inline SPEED_HD void speed_hd_chop_small(const float *d, float *sd, uint32_t n)
{
    for (uint32_t i = 0u; i + 1u < n; i++) {
        const float bound = fabsf(d[i]) + fabsf(d[i + 1u]);
        if (speed_hd_below_eps_scaled(fabsf(sd[i]), bound))
            sd[i] = 0.0f;
    }
}

/* trailing_eigenvalue(), speed.c. */
static inline SPEED_HD float speed_hd_trailing_eigenvalue(const float *d, const float *sd,
                                                          uint32_t n)
{
    const float ta = d[n - 2u];
    const float tb = d[n - 1u];
    const float tab = sd[n - 2u];
    const float dt = (ta - tb) / 2.0f;
    if (dt > 0.0f) {
        const float den = dt + speed_hd_pythagoras(dt, tab);
        const float ratio = tab / den;
        const float step = tab * ratio;
        return tb - step;
    }
    if (dt == 0.0f)
        return tb - fabsf(tab);
    const float den = -dt + speed_hd_pythagoras(dt, tab);
    const float ratio = tab / den;
    const float step = tab * ratio;
    return tb + step;
}

/* create_givens(), speed.c. */
static inline SPEED_HD void speed_hd_create_givens(float a, float b, float *c, float *s)
{
    if (b == 0.0f) {
        *c = 1.0f;
        *s = 0.0f;
        return;
    }
    const int b_larger = fabsf(b) > fabsf(a);
    const float t = b_larger ? -a / b : -b / a;
    const float tt = t * t;
    const float root = sqrtf(1.0f + tt);
    const float unit = 1.0f / root;
    const float other = unit * t;
    *s = b_larger ? unit : other;
    *c = b_larger ? other : unit;
}

typedef struct SpeedHdRotated {
    float ak;
    float bk;
    float ap;
} SpeedHdRotated;

/* G' T G of qr_step_size2() / qr_step_general(), speed.c. */
static inline SPEED_HD SpeedHdRotated speed_hd_rotate(float c, float s, float ap, float bp,
                                                      float aq)
{
    const float cap = c * ap;
    const float sbp = s * bp;
    const float saq = s * aq;
    const float cbp = c * bp;
    const float sap = s * ap;
    const float caq = c * aq;
    const float l1 = c * (cap - sbp);
    const float l2 = s * (saq - cbp);
    const float m1 = c * (sap + cbp);
    const float m2 = s * (sbp + caq);
    const float n1 = s * (sap + cbp);
    const float n2 = c * (sbp + caq);
    SpeedHdRotated r;
    r.ak = l1 + l2;
    r.bk = m1 - m2;
    r.ap = n1 + n2;
    return r;
}

static inline SPEED_HD void speed_hd_qr_step_size2(float *d, float *sd, float x, float z)
{
    float c = 0.0f;
    float s = 0.0f;
    speed_hd_create_givens(x, z, &c, &s);
    const SpeedHdRotated r = speed_hd_rotate(c, s, d[0], sd[0], d[1]);
    d[0] = r.ak;
    sd[0] = r.bk;
    d[1] = r.ap;
}

/* The chase state of qr_step_general(). */
typedef struct SpeedHdChase {
    float bk;
    float zk;
    float ap;
    float bp;
    float aq;
    float bq;
} SpeedHdChase;

/* Step k of qr_step_general()'s bulge chase; returns the new (x, z) in the
 * chase's bk / zk. */
static inline SPEED_HD void speed_hd_qr_chase_step(float *d, float *sd, uint32_t n, uint32_t k,
                                                   SpeedHdChase *st, float x, float z)
{
    float c = 0.0f;
    float s = 0.0f;
    speed_hd_create_givens(x, z, &c, &s);
    const float cbk = c * st->bk;
    const float szk = s * st->zk;
    const float bk1 = cbk - szk;
    const SpeedHdRotated r = speed_hd_rotate(c, s, st->ap, st->bp, st->aq);
    const float zp1 = -s * st->bq;
    const float bq1 = c * st->bq;
    st->bk = r.bk;
    st->zk = zp1;
    st->ap = r.ap;
    st->bp = bq1;
    if (k + 2u < n)
        st->aq = d[k + 2u];
    if (k + 3u < n)
        st->bq = sd[k + 2u];
    d[k] = r.ak;
    if (k > 0u)
        sd[k - 1u] = bk1;
    if (k + 2u < n)
        sd[k + 1u] = st->bp;
}

static inline SPEED_HD void speed_hd_qr_step_general(float *d, float *sd, uint32_t n, float x,
                                                     float z)
{
    SpeedHdChase st;
    st.bk = 0.0f;
    st.zk = 0.0f;
    st.ap = d[0];
    st.bp = sd[0];
    st.aq = d[1];
    st.bq = sd[1];
    for (uint32_t k = 0u; k + 1u < n; k++) {
        speed_hd_qr_chase_step(d, sd, n, k, &st, x, z);
        x = st.bk;
        z = st.zk;
    }
    d[n - 1u] = st.ap;
    sd[n - 2u] = st.bk;
}

/* qr_step(), speed.c. */
static inline SPEED_HD void speed_hd_qr_step(float *d, float *sd, uint32_t n)
{
    float mu = speed_hd_trailing_eigenvalue(d, sd, n);
    const float scale = fabsf(d[0]) + fabsf(sd[0]);
    if (speed_hd_below_eps_scaled(scale, fabsf(mu)))
        mu = 0.0f;
    const float x = d[0] - mu;
    const float z = sd[0];
    if (n == 2u) {
        speed_hd_qr_step_size2(d, sd, x, z);
        return;
    }
    speed_hd_qr_step_general(d, sd, n, x, z);
}

/* compute_eigenvalues_tridiagonal(), speed.c. Returns whether the iteration
 * cap was reached. */
static inline SPEED_HD int speed_hd_eigenvalues_tridiagonal(float *d, float *sd)
{
    speed_hd_chop_small(d, sd, SPEED_HIP_N);
    uint32_t b = SPEED_HIP_N - 1u;
    uint32_t iter = 0u;
    for (uint32_t guard = 0u;
         guard < SPEED_HIP_QR_CAP + SPEED_HIP_N && b > 0u && iter < SPEED_HIP_QR_CAP; guard++) {
        if (sd[b - 1u] == 0.0f) {
            b--;
            continue;
        }
        uint32_t a = b - 1u;
        for (uint32_t probe = 0u; probe < SPEED_HIP_N && a > 0u && sd[a - 1u] != 0.0f; probe++)
            a--;
        const uint32_t n_block = b - a + 1u;
        speed_hd_qr_step(d + a, sd + a, n_block);
        speed_hd_chop_small(d + a, sd + a, n_block);
        iter++;
    }
    return iter == SPEED_HIP_QR_CAP;
}

/* Tridiagonal QR on lane 0: eigenvalues, regularity and the cap flag. */
static inline SPEED_HD void speed_hd_eigen_serial(const SpeedHdSlm *m, float *eig)
{
    for (uint32_t r = 0u; r < SPEED_HIP_N; r++)
        m->d[r] = m->a[r * SPEED_HIP_N + r];
    for (uint32_t r = 0u; r + 1u < SPEED_HIP_N; r++)
        m->sd[r] = m->a[(r + 1u) * SPEED_HIP_N + r];
    const int capped = speed_hd_eigenvalues_tridiagonal(m->d, m->sd);
    int regular = 1;
    for (uint32_t r = 0u; r < SPEED_HIP_N; r++) {
        eig[r] = m->d[r];
        if (speed_hd_below_eps(m->d[r])) /* is_matrix_regular() */
            regular = 0;
    }
    m->scalar[3] = regular ? 1.0f : 0.0f;
    m->scalar[4] = capped ? 1.0f : 0.0f;
}

/* matrix_mul() with speed_matmul_scalar() accumulation order. */
static inline SPEED_HD void speed_hd_group_matmul(const SpeedHdLanes *g, float *dst, const float *x,
                                                  const float *y)
{
    for (uint32_t idx = g->lid; idx < SPEED_HIP_MATRIX; idx += g->count) {
        const uint32_t i = idx / SPEED_HIP_N;
        const uint32_t j = idx % SPEED_HIP_N;
        float acc = 0.0f;
        for (uint32_t k = 0u; k < SPEED_HIP_N; k++) {
            const float product = x[i * SPEED_HIP_N + k] * y[k * SPEED_HIP_N + j];
            acc = acc + product;
        }
        dst[idx] = acc;
    }
    SPEED_HD_SYNC();
}

/* vector_norm(), speed.c. */
static inline SPEED_HD float speed_hd_vector_norm(const float *v)
{
    float sum = 0.0f;
    for (uint32_t i = 0u; i < SPEED_HIP_N; i++) {
        const float square = v[i] * v[i];
        sum = sum + square;
    }
    return sqrtf(sum);
}

/* matrix_minor() of the working matrix, then its k-th column into vec. */
static inline SPEED_HD void speed_hd_qr_minor_column(const SpeedHdLanes *g, const SpeedHdSlm *m,
                                                     uint32_t k)
{
    for (uint32_t idx = g->lid; idx < SPEED_HIP_MATRIX; idx += g->count) {
        const uint32_t i = idx / SPEED_HIP_N;
        const uint32_t j = idx % SPEED_HIP_N;
        if (i < k || j < k)
            m->z[idx] = i == j ? 1.0f : 0.0f;
    }
    SPEED_HD_SYNC();
    for (uint32_t r = g->lid; r < SPEED_HIP_N; r += g->count)
        m->vec[r] = m->z[r * SPEED_HIP_N + k];
    SPEED_HD_SYNC();
}

/* One column of matrix_qr_decomposition(): minor, reflector vector and
 * I - 2 v v^T into h. Returns 0 when the reflector vanishes. */
static inline SPEED_HD int speed_hd_qr_reflector(const SpeedHdLanes *g, const SpeedHdSlm *m,
                                                 uint32_t k)
{
    speed_hd_qr_minor_column(g, m, k);
    if (g->lid == 0u) {
        const float norm = speed_hd_vector_norm(m->vec);
        const float shift = speed_hd_sign_of(m->cov[k * SPEED_HIP_N + k]) * norm;
        m->vec[k] = m->vec[k] + shift;
        m->scalar[2] = speed_hd_vector_norm(m->vec);
    }
    SPEED_HD_SYNC();
    const float vn = m->scalar[2];
    if (vn == 0.0f)
        return 0;
    for (uint32_t r = g->lid; r < SPEED_HIP_N; r += g->count)
        m->vec[r] = m->vec[r] / vn;
    SPEED_HD_SYNC();
    for (uint32_t idx = g->lid; idx < SPEED_HIP_MATRIX; idx += g->count) {
        const uint32_t i = idx / SPEED_HIP_N;
        const uint32_t j = idx % SPEED_HIP_N;
        const float scaled = -2.0f * m->vec[i];
        const float value = scaled * m->vec[j];
        m->h[idx] = i == j ? value + 1.0f : value;
    }
    SPEED_HD_SYNC();
    return 1;
}

static inline SPEED_HD void speed_hd_swap(float **a, float **b)
{
    float *t = *a;
    *a = *b;
    *b = t;
}

/* matrix_qr_decomposition(), speed.c: leaves the accumulated reflector
 * product (the matrix solve_linear_system() multiplies B by) in m->q and R
 * in m->t. */
static inline SPEED_HD void speed_hd_qr_decompose(const SpeedHdLanes *g, SpeedHdSlm *m)
{
    for (uint32_t idx = g->lid; idx < SPEED_HIP_MATRIX; idx += g->count) {
        m->z[idx] = m->cov[idx];
        m->q[idx] = (idx / SPEED_HIP_N == idx % SPEED_HIP_N) ? 1.0f : 0.0f;
    }
    SPEED_HD_SYNC();
    for (uint32_t k = 0u; k + 1u < SPEED_HIP_N; k++) {
        if (!speed_hd_qr_reflector(g, m, k))
            continue;
        speed_hd_group_matmul(g, m->t, m->h, m->z);
        speed_hd_swap(&m->z, &m->t);
        speed_hd_group_matmul(g, m->t, m->h, m->q);
        speed_hd_swap(&m->q, &m->t);
    }
    speed_hd_group_matmul(g, m->t, m->q, m->cov);
}

/* solve_triangular_system()'s pivot rejection, speed.c. */
static inline SPEED_HD int speed_hd_pivot_singular(const float *r)
{
    int singular = 0;
    for (uint32_t i = 0u; i < SPEED_HIP_N; i++) {
        if (speed_hd_below_eps(fabsf(r[i * SPEED_HIP_N + i])))
            singular = 1;
    }
    return singular;
}

static inline SPEED_HD void speed_hd_linalg_store(const SpeedHdLanes *g, const SpeedHipParams *p,
                                                  const SpeedHdSlm *m, uint32_t ch, int regular)
{
    if (regular) {
        for (uint32_t idx = g->lid; idx < SPEED_HIP_MATRIX; idx += g->count) {
            p->qmat[ch * SPEED_HIP_MATRIX + idx] = m->q[idx];
            p->rmat[ch * SPEED_HIP_MATRIX + idx] = m->t[idx];
        }
    }
    if (g->lid == 0u) {
        const int singular = !regular || speed_hd_pivot_singular(m->t);
        const size_t slot = (size_t)ch * 2u;
        p->status[slot] = singular ? 1 : 0;
        p->status[slot + 1u] = m->scalar[4] != 0.0f ? 1 : 0;
    }
}

/* One work-group per channel: tridiagonalisation, the tridiagonal QR,
 * regularity and, when regular, the QR factorisation. `base` is the
 * group's SPEED_HIP_SLM_FLOATS of shared memory. */
static inline SPEED_HD void speed_hd_linalg_group(const SpeedHdLanes *g, const SpeedHipParams *p,
                                                  uint32_t ch, float *base)
{
    SpeedHdSlm m = speed_hd_slm_layout(base);
    for (uint32_t idx = g->lid; idx < SPEED_HIP_MATRIX; idx += g->count) {
        const float value = p->cov[ch * SPEED_HIP_MATRIX + idx];
        m.a[idx] = value;
        m.cov[idx] = value;
    }
    SPEED_HD_SYNC();
    for (uint32_t i = 0u; i + 2u < SPEED_HIP_N; i++)
        speed_hd_tridiagonal_step(g, &m, i);
    if (g->lid == 0u)
        speed_hd_eigen_serial(&m, p->eig + (size_t)ch * SPEED_HIP_N);
    SPEED_HD_SYNC();
    const int regular = m.scalar[3] != 0.0f;
    if (regular)
        speed_hd_qr_decompose(g, &m);
    speed_hd_linalg_store(g, p, &m, ch, regular);
}

/* ------------------------------------------------------------------ */
/* Solve, variance and entropy per block: solve_linear_system() tail,  */
/* compute_pointwise_product_and_division(), sum_columns(),            */
/* update_entropy().                                                   */
/* ------------------------------------------------------------------ */

static inline SPEED_HD void speed_hd_solve_block(const float *b, uint32_t stride, const float *q,
                                                 const float *r, float *solution)
{
    float y[SPEED_HIP_N];
    for (uint32_t i = 0u; i < SPEED_HIP_N; i++) {
        float acc = 0.0f;
        for (uint32_t k = 0u; k < SPEED_HIP_N; k++) {
            const float product = q[i * SPEED_HIP_N + k] * b[(size_t)k * stride];
            acc = acc + product;
        }
        y[i] = acc;
    }
    for (uint32_t step = 0u; step < SPEED_HIP_N; step++) {
        const uint32_t i = SPEED_HIP_N - 1u - step;
        float term = y[i];
        for (uint32_t k = i + 1u; k < SPEED_HIP_N; k++) {
            const float product = solution[k] * r[i * SPEED_HIP_N + k];
            term = term - product;
        }
        solution[i] = term / r[i * SPEED_HIP_N + i];
    }
}

static inline SPEED_HD float speed_hd_block_entropy(const SpeedHipParams *p, uint32_t ch,
                                                    float variance)
{
    float entropy = 0.0f;
    for (uint32_t k = 0u; k < SPEED_HIP_N; k++) {
        const float eigenvalue = p->eig[ch * SPEED_HIP_N + k];
        const float l = eigenvalue < 0.0f ? 0.0f : eigenvalue;
        const float scaled = l * variance;
        const float shifted = scaled + p->scoring.sigma_nn;
        const float term = speed_hd_log2(shifted) + p->scoring.entropy_constant;
        entropy = entropy + term;
    }
    return entropy;
}

static inline SPEED_HD void speed_hd_block_statistics(const SpeedHipParams *p, uint32_t ch,
                                                      uint32_t block)
{
    const uint32_t blocks = p->geometry.blocks;
    const float *b = p->indterm + (size_t)ch * SPEED_HIP_N * blocks + block;
    float solution[SPEED_HIP_N];
    for (uint32_t i = 0u; i < SPEED_HIP_N; i++)
        solution[i] = 0.0f;
    if (p->status[(size_t)ch * 2u] == 0) {
        const size_t matrix = (size_t)ch * SPEED_HIP_MATRIX;
        speed_hd_solve_block(b, blocks, p->qmat + matrix, p->rmat + matrix, solution);
    }
    const float first = solution[0] * b[0];
    float variance = first / SPEED_HIP_ELEMENTS_F;
    for (uint32_t e = 1u; e < SPEED_HIP_N; e++) {
        const float product = solution[e] * b[(size_t)e * blocks];
        const float term = product / SPEED_HIP_ELEMENTS_F;
        variance = variance + term;
    }
    p->var[(size_t)ch * blocks + block] = variance;
    p->ent[(size_t)ch * blocks + block] = speed_hd_block_entropy(p, ch, variance);
}

/* ------------------------------------------------------------------ */
/* Frame score: get_speed_score() + speed_extract_score().             */
/* ------------------------------------------------------------------ */

static inline SPEED_HD float speed_hd_weighted_log(float variance)
{
    return speed_hd_log2(1.0f + variance);
}

static inline SPEED_HD float speed_hd_spatial_dis_weight(float rv, float dv, int32_t mode)
{
    if (mode == 0 || mode == 2)
        return speed_hd_weighted_log(dv);
    if (mode == 1)
        return speed_hd_weighted_log(rv);
    if (mode == 5 || mode == 6) {
        const float ref_share = mode == 5 ? 0.75f : 0.25f;
        const float dis_share = mode == 5 ? 0.25f : 0.75f;
        const float wr = ref_share * rv;
        const float wd = dis_share * dv;
        return speed_hd_weighted_log(wr + wd);
    }
    const float mean = (rv + dv) / 2.0f; /* modes 3 and 4 */
    return speed_hd_weighted_log(mean);
}

static inline SPEED_HD float speed_hd_spatial_ref_weight(float rv, float dv, int32_t mode)
{
    if (mode == 2)
        return speed_hd_weighted_log(dv);
    if (mode == 3) {
        const float mean = (rv + dv) / 2.0f;
        return speed_hd_weighted_log(mean);
    }
    return speed_hd_weighted_log(rv);
}

static inline SPEED_HD float speed_hd_block_score(const SpeedHipParams *p, uint32_t pair,
                                                  uint32_t block)
{
    const uint32_t blocks = p->geometry.blocks;
    const size_t ref = (size_t)(2u * pair) * blocks + block;
    const size_t dis = ref + blocks;
    const float re = p->ent[ref];
    const float de = p->ent[dis];
    const float rv = p->var[ref];
    const float dv = p->var[dis];
    if (re < p->scoring.base_entropy && de < p->scoring.base_entropy)
        return 0.0f;
    const int32_t mode = p->scoring.weight_mode;
    const float spatial_ref = re * speed_hd_spatial_ref_weight(rv, dv, mode);
    const float spatial_dis = de * speed_hd_spatial_dis_weight(rv, dv, mode);
    return fabsf(spatial_ref - spatial_dis);
}

/* Lane 0 of the score work-group: the sequential sum of the per-block
 * contributions, the mean, the singular rule of speed_extract_score() and
 * the flags. */
static inline SPEED_HD void speed_hd_score_finish(const SpeedHipParams *p, uint32_t pair)
{
    const uint32_t blocks = p->geometry.blocks;
    const float *contrib = p->contrib + (size_t)pair * blocks;
    float score = 0.0f;
    for (uint32_t b = 0u; b < blocks; b++)
        score = score + contrib[b];
    score = score / (float)blocks;
    const size_t slots = (size_t)pair * 4u;
    const int32_t sing_ref = p->status[slots];
    const int32_t sing_dis = p->status[slots + 2u];
    if ((sing_ref != 0) != (sing_dis != 0))
        score = 0.0f;
    p->result->score[pair] = score;
    for (uint32_t side = 0u; side < 2u; side++) {
        const uint32_t ch = 2u * pair + side;
        const size_t slot = (size_t)ch * 2u;
        p->result->singular[ch] = p->status[slot];
        p->result->iteration_cap[ch] = p->status[slot + 1u];
    }
}

/* NOLINTEND(modernize-use-nullptr) */

#endif /* FEATURE_HIP_SPEED_SPEED_HIP_DEVICE_H_ */
