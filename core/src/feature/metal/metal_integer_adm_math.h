/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  The decouple of integer_adm_metal (the restored sample r of one band
 *  sample), valid as Metal Shading Language (integer_adm.metal) and as host
 *  C, where test_metal_integer_adm_math holds it against the CPU's
 *  adm_decouple_band() and adm_decouple_band_s123() (integer_adm_kernels.h)
 *  value by value (ADR-1498; the design of the SYCL twin, ADR-1413).
 *
 *  Two steps of the CPU's decouple are not fp32 arithmetic:
 *    - the Q15 ratio k = t / o takes the reciprocal 2^30 / o from div_lookup,
 *      an integer division (integer_adm.h, div_lookup_populate()). The kernel
 *      used to take it as the fp32 quotient 2^30 / (float)o truncated, which
 *      is another integer for 686 of the 65535 scale-0 operands (|o| = 3 is
 *      the first) and moves k for 741176 (o, t) pairs. Here it is the integer
 *      division, at every scale;
 *    - the enhancement gain limit stores MIN(rst * gain, t) or
 *      MAX(rst * gain, t), the double product of the restored sample and
 *      adm_enhn_gain_limit truncated toward zero. Metal has no double; the
 *      kernel used a binary32 product (another number for a non-integer limit,
 *      and above 2^24 at scales 1-3). Here the product is
 *      adm_gain_limit_product() of the shared core/src/feature/adm_gain_limit.h,
 *      the integer form the SYCL twin uses, on the limit the host splits with
 *      adm_gain_limit_split(). The header keeps its double-only parts out of
 *      MSL behind __METAL_VERSION__ and needs UINT64_C, defined below.
 *  The restored sample at scales 1-3 is the int64 expression narrowed to
 *  int32, as on the CPU; the kernel converted it to float.
 */

#ifndef VMAF_FEATURE_METAL_METAL_INTEGER_ADM_MATH_H_
#define VMAF_FEATURE_METAL_METAL_INTEGER_ADM_MATH_H_

#include "metal_portable.h"

#if defined(__METAL_VERSION__) && !defined(UINT64_C)
#define UINT64_C(c) ((vmaf_mtl_u64)(c))
#endif
#include "../adm_gain_limit.h"

/* integer_adm.h's div_lookup[o + 32768] for o != 0: 2^30 / o, truncated. */
VMAF_MTL_FUNC vmaf_mtl_i32 vmaf_mtl_iadm_recip(vmaf_mtl_i32 o)
{
    return 1073741824 / o;
}

/* The CPU's get_best15_from32(): the top 15 bits of `temp` (>= 2^15), rounded,
 * and how far they were shifted down. */
typedef struct VmafMtlAdmBest15 {
    vmaf_mtl_u32 value;
    vmaf_mtl_i32 shift;
} VmafMtlAdmBest15;

VMAF_MTL_FUNC VmafMtlAdmBest15 vmaf_mtl_iadm_best15(vmaf_mtl_u32 temp)
{
    const vmaf_mtl_i32 k = 17 - ((vmaf_mtl_i32)VMAF_MTL_CLZ64(temp) - 32);
    VmafMtlAdmBest15 b;
    b.value = (temp + (1u << (k - 1))) >> k;
    b.shift = k;
    return b;
}

/* The Q15 ratio clamp(t / o, 0, 1) of adm_decouple_band() (scale 0). */
VMAF_MTL_FUNC vmaf_mtl_i32 vmaf_mtl_iadm_k_s0(vmaf_mtl_i32 o, vmaf_mtl_i32 t)
{
    const vmaf_mtl_i32 tmp_k =
        (o == 0) ? 32768 :
                   (vmaf_mtl_i32)((((vmaf_mtl_i64)vmaf_mtl_iadm_recip(o) * t) + 16384) >> 15);
    return tmp_k < 0 ? 0 : (tmp_k > 32768 ? 32768 : tmp_k);
}

/* The Q15 ratio of adm_decouple_band_s123() (scales 1-3). */
VMAF_MTL_FUNC vmaf_mtl_i64 vmaf_mtl_iadm_k_s123(vmaf_mtl_i32 o, vmaf_mtl_i32 t)
{
    if (o == 0) {
        return 32768;
    }
    const vmaf_mtl_u32 abs_o = (o < 0) ? 0u - (vmaf_mtl_u32)o : (vmaf_mtl_u32)o;
    const vmaf_mtl_i64 k_sign = (o < 0) ? -1 : 1;
    VmafMtlAdmBest15 b;
    b.value = abs_o;
    b.shift = 0;
    if (abs_o >= 32768u) {
        b = vmaf_mtl_iadm_best15(abs_o);
    }
    const vmaf_mtl_i64 rnd = (vmaf_mtl_i64)(1u << (14 + b.shift));
    const vmaf_mtl_i64 tmp_k =
        ((((vmaf_mtl_i64)vmaf_mtl_iadm_recip((vmaf_mtl_i32)b.value) * t) * k_sign) + rnd) >>
        (15 + b.shift);
    return tmp_k < 0 ? 0 : (tmp_k > 32768 ? 32768 : tmp_k);
}

/* The enhancement gain limit of a sample whose angle flag is set: rst_f is
 * the CPU's (k / 32768) * (o / 64), of which only the sign is used; the
 * truncated double product is bounded by the distorted sample t. */
VMAF_MTL_FUNC vmaf_mtl_i32 vmaf_mtl_iadm_gain_limit(vmaf_mtl_i32 rst, vmaf_mtl_i64 k,
                                                    vmaf_mtl_i32 o, vmaf_mtl_i32 t,
                                                    struct AdmGainLimit g)
{
    const float rst_f = ((float)k / 32768.0f) * ((float)o / 64.0f);
    const vmaf_mtl_i64 gained = adm_gain_limit_product(rst, g);
    if (rst_f > 0.0f) {
        return (vmaf_mtl_i32)((gained < (vmaf_mtl_i64)t) ? gained : (vmaf_mtl_i64)t);
    }
    if (rst_f < 0.0f) {
        return (vmaf_mtl_i32)((gained > (vmaf_mtl_i64)t) ? gained : (vmaf_mtl_i64)t);
    }
    return rst;
}

/* adm_decouple_band(): the restored part of one scale-0 band sample. */
VMAF_MTL_FUNC vmaf_mtl_i32 vmaf_mtl_iadm_decouple_s0(vmaf_mtl_i32 o, vmaf_mtl_i32 t,
                                                     vmaf_mtl_i32 angle_flag, struct AdmGainLimit g)
{
    const vmaf_mtl_i32 k = vmaf_mtl_iadm_k_s0(o, t);
    const vmaf_mtl_i32 rst = ((k * o) + 16384) >> 15;
    return angle_flag ? vmaf_mtl_iadm_gain_limit(rst, (vmaf_mtl_i64)k, o, t, g) : rst;
}

/* adm_decouple_band_s123(): the restored part of one scales-1-3 band sample. */
VMAF_MTL_FUNC vmaf_mtl_i32 vmaf_mtl_iadm_decouple_s123(vmaf_mtl_i32 o, vmaf_mtl_i32 t,
                                                       vmaf_mtl_i32 angle_flag,
                                                       struct AdmGainLimit g)
{
    const vmaf_mtl_i64 k = vmaf_mtl_iadm_k_s123(o, t);
    const vmaf_mtl_i32 rst = (vmaf_mtl_i32)(((k * o) + 16384) >> 15);
    return angle_flag ? vmaf_mtl_iadm_gain_limit(rst, k, o, t, g) : rst;
}

#endif /* VMAF_FEATURE_METAL_METAL_INTEGER_ADM_MATH_H_ */
