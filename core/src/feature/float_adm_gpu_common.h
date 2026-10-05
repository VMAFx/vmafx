/**
 *  Copyright 2016-2020 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  float_adm on a GPU: the kernel argument blocks and the per-sample
 *  arithmetic of the twins that have an fp64 type on the device,
 *  float_adm_cuda (ADR-1420) and float_adm_hip (ADR-1458). The host side of a
 *  twin, its kernels and the device-free host test
 *  (core/test/test_float_adm_device_math.c) all compile this header.
 *
 *  Numerical contract: a twin built on this header returns the CPU
 *  extractor's values bit for bit. Everything below is adm_tools.c written
 *  out operation for operation, in the reference's types:
 *
 *   - fadm_divs() is DIVS(): the IEEE fp32 quotient on both sides (ADR-1442).
 *   - fadm_angle_flag() and fadm_decouple_band() are adm_angle_flag_s() and
 *     adm_decouple_band_s(): the threshold is (cos^2 * |o|^2) * |t|^2 in that
 *     association, the clamp is the reference's two ternaries, and the
 *     enhancement gain is applied in fp64 (adm_enhn_gain_limit is a double)
 *     and rounded to fp32 once.
 *   - fadm_csf_flt() and fadm_thresh_band() are adm_csf_s() and
 *     adm_cm_thresh3x3_s(): FLOAT_ONE_BY_30 and FLOAT_ONE_BY_15 are double
 *     literals, so the 1/30 product is fp64 rounded to fp32 once and the
 *     centre tap enters the nine-term sum as an fp64 addend. The other eight
 *     terms are fp32 adds in the reference's order, one sum per band.
 *   - fadm_den_term() and fadm_cm_term() are the terms adm_csf_den_scale_s()
 *     and adm_cm_s() accumulate. At adm_p_norm = 3 they are (x * x) * x in
 *     fp32. Any other adm_p_norm is powf() on both sides (FADM_POWF), and a
 *     device's powf is not glibc's: that option is close to the reference,
 *     not equal.
 *   - fadm_row_sum() is the reference's per-row accumulator, left to right in
 *     fp32. fadm_fold_rows() is its frame accumulator, top to bottom in fp32;
 *     it runs on the host over the per-row sums the device returns.
 *
 *  Every operation that rounds goes through one of the FADM_F* / FADM_D*
 *  macros. The defaults are the plain C operators with the result converted
 *  to its type, which round once when the translation unit is built without
 *  contraction: a host TU that includes this header must be, and the HIP
 *  kernels are (ADR-1407; hipcc also divides correctly rounded there, by flag
 *  in fp32, and its fp64 product and sum are the IEEE ones). A backend whose
 *  device compiler needs another spelling defines the macros before
 *  including the header: the CUDA kernels map them to the __fmul_rn() family,
 *  which neither nvcc nor clang's CUDA driver contracts
 *  (cuda/float_adm/float_adm_device.h). FADM_HD is the linkage and execution
 *  space of the helpers, FADM_BITS() / FADM_FROM_BITS() the bit casts, and
 *  FADM_DEVICE_ONLY leaves out what only a host runs.
 *
 *  The SYCL twin cannot use this header: its devices have no fp64 type, so it
 *  evaluates the three fp64 expressions otherwise
 *  (sycl/sycl_float_adm_math.h, ADR-1434).
 */

#ifndef VMAF_SRC_FEATURE_FLOAT_ADM_GPU_COMMON_H_
#define VMAF_SRC_FEATURE_FLOAT_ADM_GPU_COMMON_H_

#include <stddef.h>
#include <stdint.h>

#define FADM_SCALES 4
#define FADM_BANDS 3
#define FADM_BX 16
#define FADM_BY 16
#define FADM_ROW_THREADS 128 /* threads per block of float_adm_row_sums */

/* Per-sample terms, three bands (h, v, d) each. */
#define FADM_SLOT_DEN 0u /* |csf(ref)|^p, the adm2 denominator */
#define FADM_SLOT_CM 3u  /* masked restored signal, the adm2 numerator */
#define FADM_SLOT_AIM 6u /* masked additive signal, the AIM numerator */
#define FADM_TERM_SLOTS 9u

#ifndef FADM_HD
#define FADM_HD static inline
#endif

#ifndef FADM_FMUL
#define FADM_FMUL(a, b) ((float)((a) * (b)))
#define FADM_FADD(a, b) ((float)((a) + (b)))
#define FADM_FSUB(a, b) ((float)((a) - (b)))
#define FADM_FDIV(a, b) ((float)((a) / (b)))
#define FADM_DMUL(a, b) ((double)((a) * (b)))
#define FADM_DADD(a, b) ((double)((a) + (b)))
#endif

/* powf() as the reductions call it for adm_p_norm other than 3. A backend
 * whose device powf(x, 1) is not x replaces it (hip/float_adm/float_adm_score.hip). */
#ifndef FADM_POWF
#define FADM_POWF(x, p) powf((x), (p))
#endif

#ifndef FADM_BITS
#include <math.h>
#include <string.h>
#define FADM_BITS(x) fadm_host_bits(x)
#define FADM_FROM_BITS(u) fadm_host_from_bits(u)

static inline uint32_t fadm_host_bits(float x)
{
    uint32_t bits;
    memcpy(&bits, &x, sizeof(bits));
    return bits;
}

static inline float fadm_host_from_bits(uint32_t bits)
{
    float x;
    memcpy(&x, &bits, sizeof(x));
    return x;
}
#endif

/* adm_tools.c's constants, in its spelling: double literals. FLOAT_ONE_BY_30
 * and FLOAT_ONE_BY_15 stay fp64 where they are used; the decouple's epsilon is
 * `const float eps = 1e-30`. */
#define FADM_ONE_BY_30 (0.0333333351)
#define FADM_ONE_BY_15 (0.0666666701)
#define FADM_EPS ((float)1e-30)

/* ------------------------------------------------------------------ */
/* Kernel argument blocks. Each kernel takes exactly one, by value, and */
/* the host passes `void *params[] = {&args}`: one layout for both      */
/* sides, so an argument cannot be dropped or reordered between them    */
/* (the CUDA driver silently ignores a surplus one, ADR-1215). Device   */
/* pointers travel as uint64_t.                                         */
/*                                                                      */
/* Band buffers hold their sub-bands back to back, `buf_stride` floats  */
/* per row and `half_h` rows per sub-band: (a, h, v, d) for the DWT     */
/* bands, (h, v, d) for the CSF buffers.                                */
/* ------------------------------------------------------------------ */

/* The DWT bands of one scale and the four CSF buffers derived from them. */
struct FloatAdmGpuBands {
    uint64_t ref_band; /* float, 4 sub-bands */
    uint64_t dis_band; /* float, 4 sub-bands */
    uint64_t csf_a;    /* float, 3 sub-bands: rfactor * decouple_a */
    uint64_t csf_fa;   /* float, 3 sub-bands: |csf_a| / 30 */
    uint64_t csf_r;    /* float, 3 sub-bands: rfactor * decouple_r */
    uint64_t csf_fr;   /* float, 3 sub-bands: |csf_r| / 30 */
    int32_t half_w;
    int32_t half_h;
    int32_t buf_stride;
    float rfactor[FADM_BANDS];
};
#ifndef __cplusplus
typedef struct FloatAdmGpuBands FloatAdmGpuBands;
#endif

struct FloatAdmGpuDecoupleArgs {
    FloatAdmGpuBands bands;
    double adm_enhn_gain_limit; /* fp64, like the reference's argument */
    float cos_1deg_sq;
    uint32_t pad_;
};
#ifndef __cplusplus
typedef struct FloatAdmGpuDecoupleArgs FloatAdmGpuDecoupleArgs;
#endif

struct FloatAdmGpuTermArgs {
    FloatAdmGpuBands bands;
    uint64_t terms; /* float, see fadm_term_index() */
    int32_t left;   /* the reduced region, adm_border_s() */
    int32_t top;
    uint32_t region_w;
    uint32_t region_h;
    float p_norm;       /* (float)adm_p_norm, read when !is_cube */
    uint32_t is_cube;   /* adm_p_norm == 3.0 */
    uint32_t bypass_cm; /* adm_bypass_cm: no masking threshold */
    uint32_t reserved;
};
#ifndef __cplusplus
typedef struct FloatAdmGpuTermArgs FloatAdmGpuTermArgs;
#endif

struct FloatAdmGpuRowArgs {
    uint64_t terms; /* float, see fadm_term_index() */
    uint64_t rows;  /* float, FADM_TERM_SLOTS x region_h, see fadm_row_index() */
    uint32_t region_w;
    uint32_t region_h;
};
#ifndef __cplusplus
typedef struct FloatAdmGpuRowArgs FloatAdmGpuRowArgs;
#endif

/* ------------------------------------------------------------------ */
/* Layout.                                                              */
/* ------------------------------------------------------------------ */

/* Sample (x, y) of sub-band `band` of a band buffer. */
FADM_HD size_t fadm_band_index(int band, int y, int x, int buf_stride, int half_h)
{
    return ((size_t)band * (size_t)half_h + (size_t)y) * (size_t)buf_stride + (size_t)x;
}

/* The per-sample terms are stored slot by slot and, inside a slot, column by
 * column: the row-sum kernel runs one thread per (slot, row), so at every step
 * neighbouring threads read neighbouring addresses. */
FADM_HD size_t fadm_term_index(uint32_t slot, uint32_t x, uint32_t y, uint32_t region_w,
                               uint32_t region_h)
{
    return ((size_t)slot * (size_t)region_w + (size_t)x) * (size_t)region_h + (size_t)y;
}

FADM_HD size_t fadm_row_index(uint32_t slot, uint32_t y, uint32_t region_h)
{
    return (size_t)slot * (size_t)region_h + (size_t)y;
}

/* adm_cm_thresh3x3_s()'s neighbour indices: the sample before the first one
 * mirrors to index 1, the sample past the last one clamps to the last index.
 * A one-sample band has no index 1; the reference reads past the band there
 * and the device reads the only sample it has. */
FADM_HD int fadm_before(int i, int n)
{
    if (i != 0)
        return i - 1;
    return (n > 1) ? 1 : 0;
}

FADM_HD int fadm_after(int i, int n)
{
    return (i == n - 1) ? n - 1 : i + 1;
}

/* ------------------------------------------------------------------ */
/* The reference's arithmetic.                                          */
/* ------------------------------------------------------------------ */

FADM_HD float fadm_abs(float x)
{
    return FADM_FROM_BITS(FADM_BITS(x) & 0x7fffffffu);
}

/* DIVS(n, d): the IEEE fp32 quotient (ADR-1442). */
FADM_HD float fadm_divs(float n, float d)
{
    return FADM_FDIV(n, d);
}

/* adm_angle_flag_s() with ADM_OPT_AVOID_ATAN: the angle between (oh, ov) and
 * (th, tv) is below one degree. */
FADM_HD int fadm_angle_flag(float oh, float ov, float th, float tv, float cos_1deg_sq)
{
    const float ot_dp = FADM_FADD(FADM_FMUL(oh, th), FADM_FMUL(ov, tv));
    const float o_mag_sq = FADM_FADD(FADM_FMUL(oh, oh), FADM_FMUL(ov, ov));
    const float t_mag_sq = FADM_FADD(FADM_FMUL(th, th), FADM_FMUL(tv, tv));
    const float lhs = FADM_FMUL(ot_dp, ot_dp);
    const float rhs = FADM_FMUL(FADM_FMUL(cos_1deg_sq, o_mag_sq), t_mag_sq);
    return (ot_dp >= 0.0f) && (lhs >= rhs);
}

/* adm_decouple_band_s(): the restored signal of one band. */
FADM_HD float fadm_decouple_band(const FloatAdmGpuDecoupleArgs *a, float o, float t, int angle_flag)
{
    float k = fadm_divs(t, FADM_FADD(o, FADM_EPS));
    k = k < 0.0f ? 0.0f : (k > 1.0f ? 1.0f : k);
    float rst = FADM_FMUL(k, o);

    if (angle_flag && (rst > 0.0f)) {
        const double gained = FADM_DMUL((double)rst, a->adm_enhn_gain_limit);
        rst = (float)((gained < (double)t) ? gained : (double)t);
    }
    if (angle_flag && (rst < 0.0f)) {
        const double gained = FADM_DMUL((double)rst, a->adm_enhn_gain_limit);
        rst = (float)((gained > (double)t) ? gained : (double)t);
    }
    return rst;
}

/* adm_csf_s()'s filtered value: FLOAT_ONE_BY_30 * fabsf(csf). */
FADM_HD float fadm_csf_flt(float csf)
{
    return (float)FADM_DMUL(FADM_ONE_BY_30, (double)fadm_abs(csf));
}

/* What the decouple kernel stores for one band sample: the CSF-weighted
 * additive (a = t - rst) and restored (r = rst) signals, each with its
 * filtered magnitude. */
struct FloatAdmCsfSample {
    float csf_a;
    float csf_fa;
    float csf_r;
    float csf_fr;
};
#ifndef __cplusplus
typedef struct FloatAdmCsfSample FloatAdmCsfSample;
#endif

/* adm_decouple_s() and both adm_csf_s() calls of compute_adm() for band
 * `band` of one sample. */
FADM_HD FloatAdmCsfSample fadm_decouple_csf(const FloatAdmGpuDecoupleArgs *a, int band, float o,
                                            float t, int angle_flag)
{
    const float rst = fadm_decouple_band(a, o, t, angle_flag);
    const float add = FADM_FSUB(t, rst);
    FloatAdmCsfSample c;
    c.csf_a = FADM_FMUL(a->bands.rfactor[band], add);
    c.csf_fa = fadm_csf_flt(c.csf_a);
    c.csf_r = FADM_FMUL(a->bands.rfactor[band], rst);
    c.csf_fr = fadm_csf_flt(c.csf_r);
    return c;
}

/* One band of adm_cm_thresh3x3_s(): the eight filtered neighbours `n` in
 * row order (above-left, above, above-right, left, right, below-left, below,
 * below-right) and the unfiltered centre, which is added fifth. */
FADM_HD float fadm_thresh_band(const float n[8], float centre)
{
    float sum = 0.0f;
    sum = FADM_FADD(sum, n[0]);
    sum = FADM_FADD(sum, n[1]);
    sum = FADM_FADD(sum, n[2]);
    sum = FADM_FADD(sum, n[3]);
    sum = (float)FADM_DADD((double)sum, FADM_DMUL(FADM_ONE_BY_15, (double)fadm_abs(centre)));
    sum = FADM_FADD(sum, n[4]);
    sum = FADM_FADD(sum, n[5]);
    sum = FADM_FADD(sum, n[6]);
    sum = FADM_FADD(sum, n[7]);
    return sum;
}

/* adm_cm_thresh3x3_s() at (x, y): `csf` is the unfiltered CSF buffer whose
 * sample supplies the centre tap, `csf_f` its filtered companion. One sum per
 * band, the three added in band order. */
FADM_HD float fadm_threshold(const float *csf, const float *csf_f, const FloatAdmGpuBands *bd,
                             int y, int x)
{
    const int ym = fadm_before(y, bd->half_h);
    const int yp = fadm_after(y, bd->half_h);
    const int xm = fadm_before(x, bd->half_w);
    const int xp = fadm_after(x, bd->half_w);
    float accum = 0.0f;
    for (int band = 0; band < FADM_BANDS; band++) {
        float n[8];
        n[0] = csf_f[fadm_band_index(band, ym, xm, bd->buf_stride, bd->half_h)];
        n[1] = csf_f[fadm_band_index(band, ym, x, bd->buf_stride, bd->half_h)];
        n[2] = csf_f[fadm_band_index(band, ym, xp, bd->buf_stride, bd->half_h)];
        n[3] = csf_f[fadm_band_index(band, y, xm, bd->buf_stride, bd->half_h)];
        n[4] = csf_f[fadm_band_index(band, y, xp, bd->buf_stride, bd->half_h)];
        n[5] = csf_f[fadm_band_index(band, yp, xm, bd->buf_stride, bd->half_h)];
        n[6] = csf_f[fadm_band_index(band, yp, x, bd->buf_stride, bd->half_h)];
        n[7] = csf_f[fadm_band_index(band, yp, xp, bd->buf_stride, bd->half_h)];
        const float centre = csf[fadm_band_index(band, y, x, bd->buf_stride, bd->half_h)];
        accum = FADM_FADD(accum, fadm_thresh_band(n, centre));
    }
    return accum;
}

/* x^adm_p_norm as the reductions take it. */
FADM_HD float fadm_pnorm(float x, uint32_t is_cube, float p_norm)
{
    if (is_cube)
        return FADM_FMUL(FADM_FMUL(x, x), x);
    return FADM_POWF(x, p_norm);
}

/* The term adm_csf_den_scale_s() accumulates for one band sample. */
FADM_HD float fadm_den_term(float rfactor, float src, uint32_t is_cube, float p_norm)
{
    return fadm_pnorm(fadm_abs(FADM_FMUL(rfactor, src)), is_cube, p_norm);
}

/* The term adm_cm_s() accumulates for one band sample: `csf` is the sample
 * times its CSF weight, `thr` the masking threshold. */
FADM_HD float fadm_cm_term(float csf, float thr, uint32_t is_cube, float p_norm)
{
    float x = FADM_FSUB(fadm_abs(csf), thr);
    x = x < 0.0f ? 0.0f : x;
    return fadm_pnorm(x, is_cube, p_norm);
}

/* One row of one slot: `count` terms `stride` floats apart, left to right. */
FADM_HD float fadm_row_sum(const float *terms, size_t stride, uint32_t count)
{
    float inner = 0.0f;
    for (uint32_t x = 0u; x < count; x++)
        inner = FADM_FADD(inner, terms[(size_t)x * stride]);
    return inner;
}

#if !defined(FADM_DEVICE_ONLY)
/* adm_fold3_s() over the rows of one slot, top to bottom. */
static inline float fadm_fold_rows(const float *rows, uint32_t count)
{
    float accum = 0.0f;
    for (uint32_t y = 0u; y < count; y++)
        accum = FADM_FADD(accum, rows[y]);
    return accum;
}
#endif

#endif /* VMAF_SRC_FEATURE_FLOAT_ADM_GPU_COMMON_H_ */
