/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Single source of truth for the integer-ADM `angle_flag` predicate.
 *
 *  `decouple()` asks whether the angle between the reference (oh, ov) and
 *  distorted (th, tv) DWT H/V vectors is below 1 degree:
 *
 *      { u.v >= 0 } AND { (u.v)^2 >= cos(1deg)^2 * ||u||^2 * ||v||^2 }
 *
 *  Upstream evaluates that in floating point after narrowing the exact
 *  int64 operands to `float`.  The narrowing is lossy past the 24-bit
 *  significand, so the predicate is *not* the mathematically exact angle
 *  test — but it is the expression the Netflix golden-data gate freezes
 *  (CLAUDE.md rule 1), so every backend has to reproduce it, bug and all.
 *
 *  Two evaluations of the *same* predicate live here:
 *
 *    adm_angle_flag_fp64() — the frozen expression, verbatim.  Requires
 *        binary64.  Used by the scalar CPU path, CUDA and HIP.
 *
 *    adm_angle_flag_i64()  — an fp64-free reformulation in 64-bit integer
 *        arithmetic that returns the *bit-identical* result.  Used by the
 *        SYCL backend (Intel Arc A-series and most iGPUs expose no fp64,
 *        and a single fp64 instruction anywhere in a SYCL translation unit
 *        makes the runtime reject the whole SPIR-V module) and mirrored in
 *        MSL by core/src/feature/metal/integer_adm.metal (Metal Shading
 *        Language has no `double` type at all).
 *
 *  Derivation of adm_angle_flag_i64() — see
 *  docs/research/2030-adm-angle-flag-fp64-free.md for the full write-up:
 *
 *    Let of = (float)ot_dp = mp*2^ep, om = (float)o_mag_sq = mo*2^eo and
 *    tm = (float)t_mag_sq = mt*2^et with 24-bit significands mp, mo, mt in
 *    [2^23, 2^24), and c = (float)cos(1deg)^2 = MC * 2^-24 exactly.
 *
 *      LHS = (of/4096)^2                = mp^2 * 2^(2*ep-24)   [exact in f64]
 *      RHS = fl( (c*(om/4096)) * (tm/4096) )
 *          = round53(MC*mo*mt*2^-24) * 2^(eo+et-24)
 *
 *    so the predicate reduces to the integer comparison
 *
 *      mp^2 * 2^(2*ep-eo-et) >= round53(V),   V = MC*mo*mt*2^-24.
 *
 *    V is written as S - D*r*2^-24 with D = 2^24 - MC = 5110 (small), which
 *    keeps every intermediate inside 64 bits: no 128-bit product and no
 *    floating point of any width are needed.
 */

#ifndef LIBVMAF_FEATURE_ADM_ANGLE_FLAG_H_
#define LIBVMAF_FEATURE_ADM_ANGLE_FLAG_H_

#include <stdint.h>

/* Device backends need their own function decoration; plain C/C++ hosts and
 * SYCL (which compiles kernel lambdas from ordinary C++ functions) do not. */
#ifndef ADM_ANGLE_FLAG_FN
#if defined(__CUDACC__) || defined(__HIPCC__)
#define ADM_ANGLE_FLAG_FN static __device__ __forceinline__
#else
#define ADM_ANGLE_FLAG_FN static inline
#endif
#endif

/* cos(1 deg)^2 rounded to binary32. The scalar CPU path recomputes this at
 * run time as `cos(M_PI/180) * cos(M_PI/180)` narrowed to float; that value
 * and this literal are the same float (0x3F7FEC0A). */
#define ADM_ANGLE_FLAG_COS_1DEG_SQ 0.99969541789740297f

/* Significand of ADM_ANGLE_FLAG_COS_1DEG_SQ: MC = c * 2^24 exactly, and
 * D = 2^24 - MC. Pinned by test_adm_angle_flag.c against the float itself. */
#define ADM_ANGLE_FLAG_MC UINT64_C(16772106)
#define ADM_ANGLE_FLAG_D UINT64_C(5110)

/**
 * The frozen upstream expression, verbatim. `ot_dp`, `o_mag_sq` and
 * `t_mag_sq` are the exact int64 dot product and squared magnitudes.
 *
 * Every operand is narrowed to `float` first and the comparison is then
 * carried out in `double`; no sub-expression is a multiply-add, so FP
 * contraction cannot alter the result on any backend.
 */
ADM_ANGLE_FLAG_FN int adm_angle_flag_fp64(int64_t ot_dp, int64_t o_mag_sq, int64_t t_mag_sq,
                                          float cos_1deg_sq)
{
    return (((float)ot_dp / 4096.0) >= 0.0f) &&
           (((float)ot_dp / 4096.0) * ((float)ot_dp / 4096.0) >=
            cos_1deg_sq * ((float)o_mag_sq / 4096.0) * ((float)t_mag_sq / 4096.0));
}

/** Bit length of a non-zero 64-bit value, without compiler builtins
 * (`__builtin_clzll` is unavailable in MSL and not guaranteed in SYCL
 * device code). */
ADM_ANGLE_FLAG_FN int adm_angle_flag_bitlen(uint64_t v)
{
    int n = 0;
    if (v >> 32) {
        v >>= 32;
        n += 32;
    }
    if (v >> 16) {
        v >>= 16;
        n += 16;
    }
    if (v >> 8) {
        v >>= 8;
        n += 8;
    }
    if (v >> 4) {
        v >>= 4;
        n += 4;
    }
    if (v >> 2) {
        v >>= 2;
        n += 2;
    }
    if (v >> 1) {
        v >>= 1;
        n += 1;
    }
    return n + (int)v;
}

/**
 * Split a non-zero magnitude into the significand/exponent pair that
 * `(float)v` would produce: `*m` in [2^23, 2^24) and `*e` such that
 * (float)v == *m * 2^(*e). Rounding is round-to-nearest, ties-to-even —
 * the IEEE-754 default the int64 -> float conversion uses.
 */
ADM_ANGLE_FLAG_FN void adm_angle_flag_norm24(uint64_t v, uint64_t *m, int *e)
{
    const int n = adm_angle_flag_bitlen(v);

    if (n <= 24) {
        /* Exactly representable; left-normalise into [2^23, 2^24). */
        *m = v << (24 - n);
        *e = n - 24;
        return;
    }

    int s = n - 24;
    uint64_t q = v >> s;
    const uint64_t rem = v & ((UINT64_C(1) << s) - 1);
    const uint64_t half = UINT64_C(1) << (s - 1);

    if (rem > half || (rem == half && (q & 1u) != 0u)) {
        q++;
        if (q == (UINT64_C(1) << 24)) { /* carry out of the significand */
            q >>= 1;
            s++;
        }
    }

    *m = q;
    *e = s;
}

/**
 * fp64-free evaluation of adm_angle_flag_fp64() with
 * cos_1deg_sq == ADM_ANGLE_FLAG_COS_1DEG_SQ. Bit-identical for every
 * int64 input triple; uses no floating-point operation of any width.
 *
 * `o_mag_sq` and `t_mag_sq` are sums of squares, so they are non-negative
 * in practice; the sign handling below only exists so the helper stays
 * total (and matches the fp64 form) if a caller ever hands over a value
 * that overflowed int64.
 */
ADM_ANGLE_FLAG_FN int adm_angle_flag_i64(int64_t ot_dp, int64_t o_mag_sq, int64_t t_mag_sq)
{
    /* ((float)ot_dp / 4096.0) >= 0.0f  <=>  ot_dp >= 0: the conversion is
     * sign-preserving and |ot_dp| >= 1 can never round to zero. */
    if (ot_dp < 0) {
        return 0;
    }

    const int rhs_negative = (o_mag_sq < 0) != (t_mag_sq < 0);
    const uint64_t ao = (o_mag_sq < 0) ? (UINT64_C(0) - (uint64_t)o_mag_sq) : (uint64_t)o_mag_sq;
    const uint64_t at = (t_mag_sq < 0) ? (UINT64_C(0) - (uint64_t)t_mag_sq) : (uint64_t)t_mag_sq;

    if (ao == 0 || at == 0 || rhs_negative) {
        return 1; /* RHS <= 0 <= LHS */
    }
    if (ot_dp == 0) {
        return 0; /* LHS == 0 < RHS */
    }

    uint64_t mp = 0;
    uint64_t mo = 0;
    uint64_t mt = 0;
    int ep = 0;
    int eo = 0;
    int et = 0;
    adm_angle_flag_norm24((uint64_t)ot_dp, &mp, &ep);
    adm_angle_flag_norm24(ao, &mo, &eo);
    adm_angle_flag_norm24(at, &mt, &et);

    /* LHS = mp^2 * 2^sp, RHS = round53(V), with mp^2 and V both in
     * [2^45.99, 2^48]: a three-binade gap already decides the comparison. */
    const int sp = 2 * ep - eo - et;
    if (sp >= 3) {
        return 1;
    }
    if (sp <= -3) {
        return 0;
    }

    /* V = MC*mo*mt*2^-24 = S - D*r*2^-24, all terms inside 64 bits. */
    const uint64_t g = mo * mt; /* < 2^48 */
    const uint64_t r = g & UINT64_C(0xFFFFFF);
    const uint64_t s_val = g - ADM_ANGLE_FLAG_D * (g >> 24);
    const uint64_t dr = ADM_ANGLE_FLAG_D * r; /* < 2^37 */

    /* Binade of V. S - 2^(n-1) < D*r*2^-24 means V dropped a binade. */
    int n = adm_angle_flag_bitlen(s_val);
    const uint64_t below = s_val - (UINT64_C(1) << (n - 1));
    if (below < ADM_ANGLE_FLAG_D && (below << 24) < dr) {
        n--;
    }

    /* Round V to 53 significant bits: V * 2^p lands in [2^52, 2^53), so
     * round53(V) * 2^p is round-to-nearest-even of that to an integer. */
    const int p = 53 - n; /* 5 <= p <= 8 */
    const uint64_t u = dr << p;
    const uint64_t ui = u >> 24;
    const uint64_t uf = u & UINT64_C(0xFFFFFF);
    uint64_t rounded = (s_val << p) - ui;

    if (uf != 0) {
        /* Fraction of V*2^p is (2^24 - uf) / 2^24, integer part one lower. */
        const uint64_t frac = UINT64_C(0x1000000) - uf;
        rounded -= 1;
        if (frac > UINT64_C(0x800000)) {
            rounded += 1;
        } else if (frac == UINT64_C(0x800000)) {
            rounded += (rounded & 1u); /* ties to even */
        }
    }

    return ((mp * mp) << (sp + p)) >= rounded;
}

#endif /* LIBVMAF_FEATURE_ADM_ANGLE_FLAG_H_ */
