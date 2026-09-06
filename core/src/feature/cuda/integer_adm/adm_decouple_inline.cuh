/**
 *
 *  Copyright 2016-2023 Netflix, Inc.
 *  Copyright 2021 NVIDIA Corporation.
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
 */

#ifndef ADM_DECOUPLE_INLINE_CUH_
#define ADM_DECOUPLE_INLINE_CUH_

/* Inline decouple helpers — shared between adm_csf.cu and adm_cm.cu to
 * eliminate d_decouple_r, d_decouple_a and d_csf_a intermediate buffers. */

#include "adm_angle_flag.h"

static constexpr float COS_1DEG_SQ = ADM_ANGLE_FLAG_COS_1DEG_SQ; // cos(1 deg)^2

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
    const float div_Q_factor = 1073741824; // 2^30
    int16_t o_val, t_val;
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

    int32_t tmp_k = (o_val == 0) ?
                        32768 :
                        (((int64_t)int32_t(div_Q_factor / float(o_val)) * t_val) + 16384) >> 15;

    int32_t k = max(0, min(32768, tmp_k));
    if (!angle_flag)
        adm_enhn_gain_limit = 1;

    int32_t rst = (int32_t)(((k * (int32_t)o_val) + 16384) >> 15) * adm_enhn_gain_limit;
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
    int32_t ot_dp = (int32_t)oh * th + (int32_t)ov * tv;
    int32_t o_mag_sq = (int32_t)oh * oh + (int32_t)ov * ov;
    int32_t t_mag_sq = (int32_t)th * th + (int32_t)tv * tv;
    return adm_angle_flag_fp64(ot_dp, o_mag_sq, t_mag_sq, COS_1DEG_SQ);
}

/* Scales 1-3 decouple: int32_t bands, returns r_val for a single band component. */
__device__ __forceinline__ int32_t decouple_r_s123(int32_t oh, int32_t ov, int32_t od, int32_t th,
                                                   int32_t tv, int32_t td, int band, int angle_flag,
                                                   double adm_enhn_gain_limit)
{
    const int32_t div_Q_factor = 1073741824; // 2^30
    int32_t o_val, t_val;
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
    uint32_t abs_o = abs(o_val);
    int8_t sign_o = (o_val < 0 ? -1 : 1);
    int32_t o_msb = (abs_o < 32768 ? abs_o : get_best15_from32(abs_o, &kh_shift));

    int64_t tmp_k =
        (o_val == 0) ?
            32768 :
            (((int64_t)(div_Q_factor / o_msb) * t_val) * sign_o + (1 << (14 + kh_shift))) >>
                (15 + kh_shift);

    int64_t k = tmp_k < 0 ? 0 : (tmp_k > 32768 ? 32768 : tmp_k);

    if (!angle_flag)
        adm_enhn_gain_limit = 1;

    int32_t rst = (int32_t)(((k * o_val) + 16384) >> 15) * adm_enhn_gain_limit;

    const float rst_f = ((float)k / 32768) * ((float)o_val / 64);

    if (angle_flag && (rst_f > 0.f))
        rst = min(rst, t_val);
    if (angle_flag && (rst_f < 0.f))
        rst = max(rst, t_val);

    return rst;
}

/* Scales 1-3 angle flag test from H,V ref/dis bands. */
__device__ __forceinline__ int decouple_angle_flag_s123(int32_t oh, int32_t ov, int32_t th,
                                                        int32_t tv)
{
    int64_t ot_dp = (int64_t)oh * th + (int64_t)ov * tv;
    int64_t o_mag_sq = (int64_t)oh * oh + (int64_t)ov * ov;
    int64_t t_mag_sq = (int64_t)th * th + (int64_t)tv * tv;
    return adm_angle_flag_fp64(ot_dp, o_mag_sq, t_mag_sq, COS_1DEG_SQ);
}

#endif /* ADM_DECOUPLE_INLINE_CUH_ */
