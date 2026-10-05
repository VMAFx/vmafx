/**
 *  Copyright 2001-2012 Xiph.Org and contributors.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2 AND BSD-2-Clause
 *
 *  The per-block arithmetic of psnr_hvs_metal (ADR-1498), valid as Metal
 *  Shading Language (integer_psnr_hvs.metal) and as host C and C++, where
 *  test_metal_psnr_hvs_math composes it as the kernel does and holds the
 *  scores against the CPU extractor. It is calc_psnrhvs()
 *  (third_party/xiph/psnr_hvs.c) statement for statement, in its order and
 *  types, as the CUDA, HIP and SYCL twins compute it (ADR-1397, ADR-1401):
 *
 *    - the variance ratio: float means and variances over the 8x8 block and
 *      its four 4x4 quadrants, added in the CPU's i, j order;
 *    - od_bin_fdct8(), the integer DCT (one 8-point transform; the kernel
 *      runs it over the columns, then over the rows of the result);
 *    - the masking energy: `(float)(c * c) * mask`, added in i, j order,
 *      the DC coefficient skipped;
 *    - the masking threshold, `sqrt(energy * ratio) / 32.f` in the CPU's
 *      types (ADR-1488): a float product, its root, stored as float. The CPU
 *      takes the root in double; a correctly rounded fp32 root of the float
 *      product is the same value (rounding a square root to 53 bits and then
 *      to 24 equals rounding it to 24, as 53 >= 2 * 24 + 2), and Metal's
 *      `sqrt` is correctly rounded under the kernels' -fno-fast-math (Metal
 *      Shading Language Specification 4.1, Table 8.1), as SYCL's sqrt_rn()
 *      is. The division by 32 only moves the exponent;
 *    - the term the CPU adds to its running sum: the integer coefficient
 *      difference converted once, less the masking threshold over the masking
 *      table outside DC, squared after the CSF weight.
 *
 *  What stays outside: the masking table. The CPU forms it as a double
 *  product stored as float, and MSL has no double, so the host forms it with
 *  vmaf_psnr_hvs_mask_value() (feature/psnr_hvs_score.c) and hands it to the
 *  kernel; the fp32 product rounds 98 of the 192 entries differently. The
 *  kernel stores the 64 terms of every block and the host adds them with
 *  vmaf_psnr_hvs_plane_score(), into one float in the CPU's order.
 */

#ifndef VMAF_FEATURE_METAL_METAL_PSNR_HVS_MATH_H_
#define VMAF_FEATURE_METAL_METAL_PSNR_HVS_MATH_H_

#include "metal_portable.h"

/* Values calc_psnrhvs() adds to its running sum per 8x8 block: what the
 * kernel stores (VMAF_PSNR_HVS_TERMS_PER_BLOCK on the host). */
#define VMAF_MTL_HVS_TERMS 64u
#define VMAF_MTL_HVS_PLANES 3u

/* csf_y, csf_cb420 and csf_cr420 of third_party/xiph/psnr_hvs.c, row-major.
 * The CPU's tables are double literals stored as float; these float literals
 * are the same 192 values (test_metal_psnr_hvs_math compares the scores). */
VMAF_MTL_CONSTANT float vmaf_mtl_hvs_csf[VMAF_MTL_HVS_PLANES][VMAF_MTL_HVS_TERMS] = {
    /* Y (csf_y) */
    {1.6193873005f,   2.2901594831f,   2.08509755623f,  1.48366094411f,  1.00227514334f,
     0.678296995242f, 0.466224900598f, 0.3265091542f,   2.2901594831f,   1.94321815382f,
     2.04793073064f,  1.68731108984f,  1.2305666963f,   0.868920337363f, 0.61280991668f,
     0.436405793551f, 2.08509755623f,  2.04793073064f,  1.34329019223f,  1.09205635862f,
     0.875748795257f, 0.670882927016f, 0.501731932449f, 0.372504254596f, 1.48366094411f,
     1.68731108984f,  1.09205635862f,  0.772819797575f, 0.605636379554f, 0.48309405692f,
     0.380429446972f, 0.295774038565f, 1.00227514334f,  1.2305666963f,   0.875748795257f,
     0.605636379554f, 0.448996256676f, 0.352889268808f, 0.283006984131f, 0.226951348204f,
     0.678296995242f, 0.868920337363f, 0.670882927016f, 0.48309405692f,  0.352889268808f,
     0.27032073436f,  0.215017739696f, 0.17408067321f,  0.466224900598f, 0.61280991668f,
     0.501731932449f, 0.380429446972f, 0.283006984131f, 0.215017739696f, 0.168869545842f,
     0.136153931001f, 0.3265091542f,   0.436405793551f, 0.372504254596f, 0.295774038565f,
     0.226951348204f, 0.17408067321f,  0.136153931001f, 0.109083846276f},
    /* Cb (csf_cb420) */
    {1.91113096927f,  2.46074210438f,  1.18284184739f,  1.14982565193f,  1.05017074788f,
     0.898018824055f, 0.74725392039f,  0.615105596242f, 2.46074210438f,  1.58529308355f,
     1.21363250036f,  1.38190029285f,  1.33100189972f,  1.17428548929f,  0.996404342439f,
     0.830890433625f, 1.18284184739f,  1.21363250036f,  0.978712413627f, 1.02624506078f,
     1.03145147362f,  0.960060382087f, 0.849823426169f, 0.731221236837f, 1.14982565193f,
     1.38190029285f,  1.02624506078f,  0.861317501629f, 0.801821139099f, 0.751437590932f,
     0.685398513368f, 0.608694761374f, 1.05017074788f,  1.33100189972f,  1.03145147362f,
     0.801821139099f, 0.676555426187f, 0.605503172737f, 0.55002013668f,  0.495804539034f,
     0.898018824055f, 1.17428548929f,  0.960060382087f, 0.751437590932f, 0.605503172737f,
     0.514674450957f, 0.454353482512f, 0.407050308965f, 0.74725392039f,  0.996404342439f,
     0.849823426169f, 0.685398513368f, 0.55002013668f,  0.454353482512f, 0.389234902883f,
     0.342353999733f, 0.615105596242f, 0.830890433625f, 0.731221236837f, 0.608694761374f,
     0.495804539034f, 0.407050308965f, 0.342353999733f, 0.295530605237f},
    /* Cr (csf_cr420) */
    {2.03871978502f,  2.62502345193f,  1.26180942886f,  1.11019789803f,  1.01397751469f,
     0.867069376285f, 0.721500455585f, 0.593906509971f, 2.62502345193f,  1.69112867013f,
     1.17180569821f,  1.3342742857f,   1.28513006198f,  1.13381474809f,  0.962064122248f,
     0.802254508198f, 1.26180942886f,  1.17180569821f,  0.944981930573f, 0.990876405848f,
     0.995903384143f, 0.926972725286f, 0.820534991409f, 0.706020324706f, 1.11019789803f,
     1.3342742857f,   0.990876405848f, 0.831632933426f, 0.77418706195f,  0.725539939514f,
     0.661776842059f, 0.587716619023f, 1.01397751469f,  1.28513006198f,  0.995903384143f,
     0.77418706195f,  0.653238524286f, 0.584635025748f, 0.531064164893f, 0.478717061273f,
     0.867069376285f, 1.13381474809f,  0.926972725286f, 0.725539939514f, 0.584635025748f,
     0.496936637883f, 0.438694579826f, 0.393021669543f, 0.721500455585f, 0.962064122248f,
     0.820534991409f, 0.661776842059f, 0.531064164893f, 0.438694579826f, 0.375820256136f,
     0.330555063063f, 0.593906509971f, 0.802254508198f, 0.706020324706f, 0.587716619023f,
     0.478717061273f, 0.393021669543f, 0.330555063063f, 0.285345396658f}};

/* ------------------------------------------------------------------ */
/* Variance ratio                                                       */
/* ------------------------------------------------------------------ */

/* calc_psnrhvs()'s accumulators for one image of a block: the whole block
 * and its four 4x4 quadrants. Means first, variances after. */
typedef struct VmafMtlHvsMoments {
    float whole;
    float quadrant[4];
} VmafMtlHvsMoments;

VMAF_MTL_FUNC VmafMtlHvsMoments vmaf_mtl_hvs_moments_zero(void)
{
    VmafMtlHvsMoments m;
    m.whole = 0.f;
    for (int k = 0; k < 4; k++)
        m.quadrant[k] = 0.f;
    return m;
}

/* `int sub = ((i & 12) >> 2) + ((j & 12) >> 1);` */
VMAF_MTL_FUNC vmaf_mtl_i32 vmaf_mtl_hvs_quadrant(vmaf_mtl_i32 i, vmaf_mtl_i32 j)
{
    return ((i & 12) >> 2) + ((j & 12) >> 1);
}

/* `s_gmean += dct_s[i * 8 + j]; s_means[sub] += dct_s[i * 8 + j];` */
VMAF_MTL_FUNC VmafMtlHvsMoments vmaf_mtl_hvs_mean_add(VmafMtlHvsMoments m, vmaf_mtl_i32 i,
                                                      vmaf_mtl_i32 j, vmaf_mtl_i32 sample)
{
    const vmaf_mtl_i32 sub = vmaf_mtl_hvs_quadrant(i, j);
    m.whole += (float)sample;
    m.quadrant[sub] += (float)sample;
    return m;
}

/* `s_gmean /= 64.f;` and `s_means[i] /= 16.f;` */
VMAF_MTL_FUNC VmafMtlHvsMoments vmaf_mtl_hvs_means(VmafMtlHvsMoments sums)
{
    sums.whole /= 64.f;
    for (int k = 0; k < 4; k++)
        sums.quadrant[k] /= 16.f;
    return sums;
}

/* `s_gvar += (dct_s[i * 8 + j] - s_gmean) * (dct_s[i * 8 + j] - s_gmean);`
 * and the same for the sample's quadrant. */
VMAF_MTL_FUNC VmafMtlHvsMoments vmaf_mtl_hvs_variance_add(VmafMtlHvsMoments v,
                                                          VmafMtlHvsMoments means, vmaf_mtl_i32 i,
                                                          vmaf_mtl_i32 j, vmaf_mtl_i32 sample)
{
    const vmaf_mtl_i32 sub = vmaf_mtl_hvs_quadrant(i, j);
    const float whole = (float)sample - means.whole;
    const float part = (float)sample - means.quadrant[sub];
    v.whole += whole * whole;
    v.quadrant[sub] += part * part;
    return v;
}

/* `s_gvar *= 1 / 63.f * 64; s_vars[i] *= 1 / 15.f * 16;` and, for a block
 * that is not flat, `s_gvar = (s_vars[0] + s_vars[1] + s_vars[2] +
 * s_vars[3]) / s_gvar;` -- the value calc_psnrhvs() multiplies the masking
 * energy by. */
VMAF_MTL_FUNC float vmaf_mtl_hvs_variance_ratio(VmafMtlHvsMoments v)
{
    float whole = v.whole;
    whole *= 1.f / 63.f * 64.f;
    for (int k = 0; k < 4; k++)
        v.quadrant[k] *= 1.f / 15.f * 16.f;
    if (whole > 0.f)
        whole = (v.quadrant[0] + v.quadrant[1] + v.quadrant[2] + v.quadrant[3]) / whole;
    return whole;
}

/* ------------------------------------------------------------------ */
/* od_bin_fdct8()                                                       */
/* ------------------------------------------------------------------ */

/* Eight coefficients: the input of one 8-point transform, or its output. */
typedef struct VmafMtlHvsLine {
    vmaf_mtl_i32 v[8];
} VmafMtlHvsLine;

/* OD_UNBIASED_RSHIFT32(a, b): a right shift that rounds toward zero. */
VMAF_MTL_FUNC vmaf_mtl_i32 vmaf_mtl_hvs_rshift(vmaf_mtl_i32 a, vmaf_mtl_i32 b)
{
    return (vmaf_mtl_i32)(((vmaf_mtl_u32)a >> (32 - b)) + (vmaf_mtl_u32)a) >> b;
}

/* od_bin_fdct8()'s registers between its two halves. */
typedef struct VmafMtlHvsDct {
    vmaf_mtl_i32 t0;
    vmaf_mtl_i32 t1;
    vmaf_mtl_i32 t2;
    vmaf_mtl_i32 t3;
    vmaf_mtl_i32 t4;
    vmaf_mtl_i32 t5;
    vmaf_mtl_i32 t6;
    vmaf_mtl_i32 t7;
    vmaf_mtl_i32 t1h;
} VmafMtlHvsDct;

/* The initial permutation, the +1/-1 butterflies, the embedded 4-point DCT
 * with its 2-point DCT and 2-point DST. */
VMAF_MTL_FUNC VmafMtlHvsDct vmaf_mtl_hvs_fdct8_even(VmafMtlHvsLine x)
{
    VmafMtlHvsDct r;
    r.t0 = x.v[0];
    r.t4 = x.v[1];
    r.t2 = x.v[2];
    r.t6 = x.v[3];
    r.t7 = x.v[4];
    r.t3 = x.v[5];
    r.t5 = x.v[6];
    r.t1 = x.v[7];
    r.t1 = r.t0 - r.t1;
    r.t1h = vmaf_mtl_hvs_rshift(r.t1, 1);
    r.t0 -= r.t1h;
    r.t4 += r.t5;
    const vmaf_mtl_i32 t4h = vmaf_mtl_hvs_rshift(r.t4, 1);
    r.t5 -= t4h;
    r.t3 = r.t2 - r.t3;
    r.t2 -= vmaf_mtl_hvs_rshift(r.t3, 1);
    r.t6 += r.t7;
    const vmaf_mtl_i32 t6h = vmaf_mtl_hvs_rshift(r.t6, 1);
    r.t7 = t6h - r.t7;
    r.t0 += t6h;
    r.t6 = r.t0 - r.t6;
    r.t2 = t4h - r.t2;
    r.t4 = r.t2 - r.t4;
    r.t0 -= (r.t4 * 13573 + 16384) >> 15;
    r.t4 += (r.t0 * 11585 + 8192) >> 14;
    r.t0 -= (r.t4 * 13573 + 16384) >> 15;
    r.t6 -= (r.t2 * 21895 + 16384) >> 15;
    r.t2 += (r.t6 * 15137 + 8192) >> 14;
    r.t6 -= (r.t2 * 21895 + 16384) >> 15;
    return r;
}

/* The embedded 4-point DST and the output order. */
VMAF_MTL_FUNC VmafMtlHvsLine vmaf_mtl_hvs_fdct8_odd(VmafMtlHvsDct r)
{
    r.t3 += (r.t5 * 19195 + 16384) >> 15;
    r.t5 += (r.t3 * 11585 + 8192) >> 14;
    r.t3 -= (r.t5 * 7489 + 4096) >> 13;
    r.t7 = vmaf_mtl_hvs_rshift(r.t5, 1) - r.t7;
    r.t5 -= r.t7;
    r.t3 = r.t1h - r.t3;
    r.t1 -= r.t3;
    r.t7 += (r.t1 * 3227 + 16384) >> 15;
    r.t1 -= (r.t7 * 6393 + 16384) >> 15;
    r.t7 += (r.t1 * 3227 + 16384) >> 15;
    r.t5 += (r.t3 * 2485 + 4096) >> 13;
    r.t3 -= (r.t5 * 18205 + 16384) >> 15;
    r.t5 += (r.t3 * 2485 + 4096) >> 13;
    VmafMtlHvsLine y;
    y.v[0] = r.t0;
    y.v[1] = r.t1;
    y.v[2] = r.t2;
    y.v[3] = r.t3;
    y.v[4] = r.t4;
    y.v[5] = r.t5;
    y.v[6] = r.t6;
    y.v[7] = r.t7;
    return y;
}

/* od_bin_fdct8(): one 8-point forward DCT. od_bin_fdct8x8() runs it over the
 * eight columns of the block into a scratch block, then over the eight
 * columns of the scratch block into the rows of the result. */
VMAF_MTL_FUNC VmafMtlHvsLine vmaf_mtl_hvs_fdct8(VmafMtlHvsLine x)
{
    return vmaf_mtl_hvs_fdct8_odd(vmaf_mtl_hvs_fdct8_even(x));
}

/* ------------------------------------------------------------------ */
/* Masking and the stored term                                          */
/* ------------------------------------------------------------------ */

/* `s_mask += dct_s[i * 8 + j] * dct_s[i * 8 + j] * mask[i][j];` for one
 * coefficient other than DC: an integer square, converted, times the
 * masking-table entry. */
VMAF_MTL_FUNC float vmaf_mtl_hvs_energy_add(float energy, vmaf_mtl_i32 coefficient, float mask)
{
    return energy + (float)(coefficient * coefficient) * mask;
}

/* `s_mask = sqrt((double)(s_mask * s_gvar)) / 32.f;` (ADR-1488): the float
 * product, its root, stored as float. See the top of the file for why the
 * fp32 root is the CPU's value. */
VMAF_MTL_FUNC float vmaf_mtl_hvs_threshold(float energy, float ratio)
{
    const float product = energy * ratio;
    return VMAF_MTL_SQRT(product) / 32.f;
}

/* `if (d_mask > s_mask) s_mask = d_mask;` */
VMAF_MTL_FUNC float vmaf_mtl_hvs_block_threshold(float ref_threshold, float dist_threshold)
{
    return dist_threshold > ref_threshold ? dist_threshold : ref_threshold;
}

/* The value calc_psnrhvs() adds to its running sum for the coefficient at
 * `index` (i * 8 + j):
 *
 *     err = abs(dct_s[i * 8 + j] - dct_d[i * 8 + j]);
 *     if (i != 0 || j != 0)
 *         err = err < s_mask / mask[i][j] ? 0 : err - s_mask / mask[i][j];
 *     ret += (err * _csf[i][j]) * (err * _csf[i][j]);
 */
VMAF_MTL_FUNC float vmaf_mtl_hvs_term(vmaf_mtl_i32 ref, vmaf_mtl_i32 dist, float csf, float mask,
                                      float threshold, vmaf_mtl_u32 index)
{
    const vmaf_mtl_i32 diff = ref - dist;
    float error = (float)(diff < 0 ? -diff : diff);
    if (index != 0u) {
        const float masking = threshold / mask;
        error = error < masking ? 0.f : error - masking;
    }
    return (error * csf) * (error * csf);
}

#endif /* VMAF_FEATURE_METAL_METAL_PSNR_HVS_MATH_H_ */
