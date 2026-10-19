/**
 *
 *  Copyright 2016-2023 Netflix, Inc.
 *  Copyright 2021 NVIDIA Corporation.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *     Licensed under the BSD+Patent License (the "License");
 *     you may not use this file except in compliance with the License.
 *     You may obtain a copy of the License at
 *
 *         https://opensource.org/licenses/BSDplusPatent
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 *
 *  HIP port of cuda/integer_adm/adm_decouple_inline.cuh — identical
 *  arithmetic, CUDA-specific headers replaced with HIP equivalents.
 */

#ifndef ADM_DECOUPLE_INLINE_HIP_
#define ADM_DECOUPLE_INLINE_HIP_

/* Inline decouple helpers — shared between adm_csf.hip and adm_cm.hip to
 * eliminate d_decouple_r, d_decouple_a and d_csf_a intermediate buffers. */

#include "adm_angle_flag.h"

namespace
{

/* The CPU's reciprocal of the scale-0 decouple: `div_lookup[o + 32768]`, the integer
 * quotient 2^30 / o truncated toward zero (integer_adm.h, div_lookup_populate()),
 * for o != 0. `int32_t(2^30f / float(o))` is another integer for 343 positive
 * operands (the first is o = 3), which changes the restored sample for a reference
 * coefficient above 16566 (T-GPU-ADM-DECOUPLE-FP32-RECIPROCAL-2026-10-03).
 *
 * The fp32 quotient q is a first estimate: it is off by at most 11 (at |o| = 3
 * and 5, measured over all 32768 magnitudes) and its remainder r = 2^30 - q * o
 * is exact in int32 (|r| <= 32400, |q * o| <= 2^30 + 63). 2^30 / o = q + r / o, so
 * the quotient is q plus the quotient of r by o truncated toward zero, which is
 * floor(r / |o|) with the sign of o. |r / o| <= 10.7, where fp32 has a spacing of
 * 2^-20, and a non-integer r / |o| is at least 1 / 32768 from an integer, so the
 * fp32 quotient floors to the exact one. `test_adm_decouple_recip_*` holds the
 * decouple to div_lookup for every int16 operand. A 32-bit integer division in
 * place of these two fp32 ones takes `adm_cm_line_kernel_8` from 148 to 228
 * registers and `adm_cm_aim_line_kernel_4` from 208 to 211, past the budget of
 * `test_cuda_adm_cm_register_pressure` (ADR-1226). */
__device__ __forceinline__ int32_t adm_recip_q30(int16_t o)
{
    const float fo = (float)o;
    const int32_t q = (int32_t)(1073741824.0f / fo);
    const int32_t dq = (int32_t)floorf((float)(1073741824 - (q * (int32_t)o)) / fabsf(fo));
    return o < 0 ? q - dq : q + dq;
}

constexpr float COS_1DEG_SQ = ADM_ANGLE_FLAG_COS_1DEG_SQ; // cos(1 deg)^2

__device__ __forceinline__ uint16_t get_best15_from32(uint32_t temp, int *x)
{
    int k = __clz(temp);
    k = 17 - k;
    temp = (temp + (1 << (k - 1))) >> k;
    *x = k;
    return temp;
}

/* Scale-0 decouple: int16_t bands, returns r_val for a single band component.
 * oh/ov/od = ref H/V/D, th/tv/td = dis H/V/D.
 * band selects which component (0=H, 1=V, 2=D) to produce r for.
 * angle_flag is precomputed from H,V bands. */
__device__ __forceinline__ int16_t decouple_r_s0(int16_t oh, int16_t ov, int16_t od, int16_t th,
                                                 int16_t tv, int16_t td, int band, int angle_flag,
                                                 double adm_enhn_gain_limit)
{
    int16_t o_val;
    int16_t t_val;
    if (band == 0) {
        o_val = oh;
        t_val = th;
    } else if (band == 1) {
        o_val = ov;
        t_val = tv;
    } else {
        o_val = od;
        t_val = td;
    }

    const int32_t tmp_k =
        (o_val == 0) ? 32768 : (((int64_t)adm_recip_q30(o_val) * t_val) + 16384) >> 15;

    const int32_t k = max(0, min(32768, tmp_k));
    if (!angle_flag)
        adm_enhn_gain_limit = 1;

    int32_t rst = (int32_t)((int32_t)(((k * (int32_t)o_val) + 16384) >> 15) * adm_enhn_gain_limit);
    const int32_t rst_s = k * (int32_t)o_val;

    if (angle_flag) {
        if (rst_s > 0)
            rst = min(rst, (int32_t)t_val);
        if (rst_s < 0)
            rst = max(rst, (int32_t)t_val);
    }
    return (int16_t)rst;
}

/* Scale-0 angle flag test from H,V ref/dis bands.
 *
 * ADR-1194: this used to compare the *exact* int64 products in double, which
 * is a strictly more accurate angle test than the CPU's — and therefore a
 * different one. The golden-frozen CPU expression narrows each operand to
 * float first, so scale-0 bands past the 24-bit significand flipped the flag
 * (and with it the enhancement-gain-limited branch of decouple) relative to
 * the CPU. Route through the shared predicate instead. */
__device__ __forceinline__ int decouple_angle_flag_s0(int16_t oh, int16_t ov, int16_t th,
                                                      int16_t tv)
{
    /* Each product of two int16 values fits int32 (at most 2^30). The dot product reaches 2^31 only
     * with every band at -32768: it is formed as an unsigned sum, and `- 1` then `+ 1` in 64 bits
     * restores that one value an int32 cannot hold without a select (the CPU's sum is int64:
     * adm_angle_flag() in integer_adm_kernels.h). The squared magnitudes are never negative and
     * fit uint32. T-GPU-ADM-ANGLE-FLAG-S0-INT32-CORNER-2026-10-06; register budget: ADR-1226 and
     * its amendment. test_adm_decouple_recip_* holds the flag to the CPU's at every int16 corner. */
    const uint32_t ot_sum = (uint32_t)((int32_t)oh * th) + (uint32_t)((int32_t)ov * tv);
    const int64_t ot_dp = (int64_t)(int32_t)(ot_sum - 1u) + 1;
    const uint32_t o_sq32 = (uint32_t)((int32_t)oh * oh) + (uint32_t)((int32_t)ov * ov);
    const uint32_t t_sq32 = (uint32_t)((int32_t)th * th) + (uint32_t)((int32_t)tv * tv);
    const int64_t o_mag_sq = o_sq32;
    const int64_t t_mag_sq = t_sq32;
    return adm_angle_flag_fp64(ot_dp, o_mag_sq, t_mag_sq, COS_1DEG_SQ);
}

/* Scales 1-3 decouple: int32_t bands, returns r_val for a single band component. */
__device__ __forceinline__ int32_t decouple_r_s123(int32_t oh, int32_t ov, int32_t od, int32_t th,
                                                   int32_t tv, int32_t td, int band, int angle_flag,
                                                   double adm_enhn_gain_limit)
{
    const int32_t div_Q_factor = 1073741824; // 2^30
    int32_t o_val;
    int32_t t_val;
    if (band == 0) {
        o_val = oh;
        t_val = th;
    } else if (band == 1) {
        o_val = ov;
        t_val = tv;
    } else {
        o_val = od;
        t_val = td;
    }

    int32_t kh_shift = 0;
    const uint32_t abs_o = abs(o_val);
    const int8_t sign_o = (o_val < 0 ? -1 : 1);
    const int32_t o_msb = (abs_o < 32768 ? abs_o : get_best15_from32(abs_o, &kh_shift));

    const int64_t tmp_k =
        (o_val == 0) ?
            32768 :
            (((int64_t)(div_Q_factor / o_msb) * t_val) * sign_o + ((int64_t)1 << (14 + kh_shift))) >>
                (15 + kh_shift);

    const int64_t k = tmp_k < 0 ? 0 : (tmp_k > 32768 ? 32768 : tmp_k);

    const int32_t rst_q = (int32_t)(((k * o_val) + 16384) >> 15);
    const float rst_f = ((float)k / 32768) * ((float)o_val / 64);
    if (!angle_flag || rst_f == 0.f)
        return rst_q;

    /* adm_decouple_band_s123(): the gain-limited product is formed and bounded by
     * t in double and narrowed once. |o| reaches 1.45e9 at scale 1, so the
     * product with the default limit of 100 leaves int32; narrowing it before
     * the bound was an undefined conversion that the device's saturating
     * cvt happened to bound to t. */
    const double gained = (double)rst_q * adm_enhn_gain_limit;
    return (int32_t)((rst_f > 0.f) ? fmin(gained, (double)t_val) : fmax(gained, (double)t_val));
}

/* Scales 1-3 angle flag test from H,V ref/dis bands. */
__device__ __forceinline__ int decouple_angle_flag_s123(int32_t oh, int32_t ov, int32_t th,
                                                        int32_t tv)
{
    const int64_t ot_dp = (int64_t)oh * th + (int64_t)ov * tv;
    const int64_t o_mag_sq = (int64_t)oh * oh + (int64_t)ov * ov;
    const int64_t t_mag_sq = (int64_t)th * th + (int64_t)tv * tv;
    return adm_angle_flag_fp64(ot_dp, o_mag_sq, t_mag_sq, COS_1DEG_SQ);
}

} // namespace

#endif /* ADM_DECOUPLE_INLINE_HIP_ */
