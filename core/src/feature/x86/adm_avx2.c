/**
 *
 *  Copyright 2016-2020 Netflix, Inc.
 *  Copyright 2026 Lusoris
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

/*
 * AVX2 twins of the integer ADM stages.
 *
 * Each stage vectorises the interior of a row and hands the columns a vector
 * does not cover, and the rows that need a mirrored neighbourhood, to the
 * scalar kernels of feature/integer_adm_kernels.h. Those are the kernels the
 * scalar reference in feature/integer_adm.c runs, so a tail column cannot
 * disagree with it. The vector paths must stay bit-identical to the same
 * kernels; core/test/test_integer_adm_simd*.c compare them.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <immintrin.h>

#include "feature/common/macros.h"
#include "feature/integer_adm_kernels.h"
#include "adm_avx2.h"

/* ------------------------------------------------------------------------- */
/* Lane helpers                                                              */
/* ------------------------------------------------------------------------- */

/* Eight int16 samples widened to int32 lanes. */
static FORCE_INLINE __m256i load_epi16x8(const int16_t *p)
{
    return _mm256_cvtepi16_epi32(_mm_loadu_si128((const __m128i *)p));
}

/* Four int32 samples widened to int64 lanes. */
static FORCE_INLINE __m256i load_epi32x4(const int32_t *p)
{
    return _mm256_cvtepi32_epi64(_mm_loadu_si128((const __m128i *)p));
}

/* The lower / upper four int32 lanes widened to int64 lanes. */
static FORCE_INLINE __m256i widen_lo(__m256i v)
{
    return _mm256_cvtepi32_epi64(_mm256_extracti128_si256(v, 0));
}

static FORCE_INLINE __m256i widen_hi(__m256i v)
{
    return _mm256_cvtepi32_epi64(_mm256_extracti128_si256(v, 1));
}

/* Two vectors of four int64 truncated to one vector of eight int32. */
static FORCE_INLINE __m256i narrow_epi64(__m256i lo, __m256i hi)
{
    int64_t t[8];
    _mm256_storeu_si256((__m256i *)(&t[0]), lo);
    _mm256_storeu_si256((__m256i *)(&t[4]), hi);
    return _mm256_setr_epi32((int)t[0], (int)t[1], (int)t[2], (int)t[3], (int)t[4], (int)t[5],
                             (int)t[6], (int)t[7]);
}

/* `mask ? a : b` per bit. */
static FORCE_INLINE __m256i blend(__m256i a, __m256i b, __m256i mask)
{
    return _mm256_or_si256(_mm256_and_si256(mask, a), _mm256_andnot_si256(mask, b));
}

/* Arithmetic right shift of int64 lanes by a per-lane count; AVX2 has none. */
static FORCE_INLINE __m256i sra_epi64(__m256i a, __m256i count)
{
    __m256i rl_shift = _mm256_srlv_epi64(a, count); // logical shift
    __m256i signmask = _mm256_cmpgt_epi64(_mm256_setzero_si256(), a);
    __m256i newmask = _mm256_sub_epi64(_mm256_set1_epi64x(64), count);
    signmask = _mm256_sllv_epi64(signmask, newmask);
    return _mm256_or_si256(rl_shift, signmask);
}

/* Arithmetic right shift by 15 of int64 lanes whose value fits in 49 bits:
 * the top 15 bits of such a value are copies of its sign. */
static FORCE_INLINE __m256i sra15_epi64(__m256i a)
{
    return _mm256_add_epi64(
        _mm256_srli_epi64(a, 15),
        _mm256_and_si256(a, _mm256_set1_epi64x((int64_t)0xFFFE000000000000ULL)));
}

/* Arithmetic right shift by `shift` of int64 lanes whose value fits in
 * 64 - shift bits; `msb_mask` has the top `shift` bits set. */
static FORCE_INLINE __m256i sra_fit_epi64(__m256i a, int shift, __m256i msb_mask)
{
    return _mm256_or_si256(_mm256_srli_epi64(a, shift), _mm256_and_si256(a, msb_mask));
}

/* The same shift, written as upstream does where the sign is tested first. */
static FORCE_INLINE __m256i sra_sign_epi64(__m256i a, int shift, __m256i msb_mask)
{
    const __m256i sign = _mm256_and_si256(msb_mask, _mm256_cmpgt_epi64(_mm256_setzero_si256(), a));
    return _mm256_or_si256(_mm256_srli_epi64(a, shift), sign);
}

/* |a| of int64 lanes holding sign-extended int32 values. */
static FORCE_INLINE __m256i abs_epi64_from32(__m256i a)
{
    const __m256i ltz = _mm256_cmpgt_epi64(_mm256_setzero_si256(), a);
    const __m256i neg = _mm256_and_si256(_mm256_mul_epi32(a, _mm256_set1_epi32(-1)), ltz);
    return _mm256_or_si256(_mm256_andnot_si256(ltz, a), neg);
}

#if defined(__x86_64__) || defined(_M_X64) || defined(_M_AMD64)
#define extract_epi64_128 _mm_extract_epi64
#else
/* _mm_extract_epi64 is x86-64 only. */
static FORCE_INLINE int64_t extract_epi64_128(__m128i a, const int index)
{
    if (index == 0) {
        return ((uint64_t)_mm_extract_epi32(a, 1) << 32) | (unsigned)_mm_extract_epi32(a, 0);
    }
    return ((uint64_t)_mm_extract_epi32(a, 3) << 32) | (unsigned)_mm_extract_epi32(a, 2);
}
#endif

/* Sum of the four int64 lanes. */
static FORCE_INLINE int64_t hsum_epi64(__m256i v)
{
    const __m128i r2 = _mm_add_epi64(_mm256_castsi256_si128(v), _mm256_extracti128_si256(v, 1));
    return (int64_t)extract_epi64_128(r2, 0) + (int64_t)extract_epi64_128(r2, 1);
}

/* Sum of the four uint64 lanes. */
static FORCE_INLINE uint64_t hsum_epu64(__m256i v)
{
    const __m128i r2 = _mm_add_epi64(_mm256_castsi256_si128(v), _mm256_extracti128_si256(v, 1));
    return (uint64_t)extract_epi64_128(r2, 0) + (uint64_t)extract_epi64_128(r2, 1);
}

/* ------------------------------------------------------------------------- */
/* Decouple, scale 0                                                         */
/* ------------------------------------------------------------------------- */

/* angle_flag of eight samples as 0 / -1 lanes. The dot product and the
 * squared magnitudes are formed in int32 by _mm256_madd_epi16. */
static FORCE_INLINE __m256i decouple_angle_mask_avx2(__m256i oh, __m256i ov, __m256i th, __m256i tv,
                                                     float cos_1deg_sq)
{
    const __m256i lo16 = _mm256_set1_epi32(0xFFFF);
    const __m256i oh_ov = _mm256_or_si256(_mm256_and_si256(oh, lo16), _mm256_slli_epi32(ov, 16));
    const __m256i th_tv = _mm256_or_si256(_mm256_and_si256(th, lo16), _mm256_slli_epi32(tv, 16));
    int32_t o_mag_sq[8];
    int32_t ot_dp[8];
    int32_t t_mag_sq[8];
    int32_t flag[8];

    _mm256_storeu_si256((__m256i *)o_mag_sq, _mm256_madd_epi16(oh_ov, oh_ov));
    _mm256_storeu_si256((__m256i *)ot_dp, _mm256_madd_epi16(oh_ov, th_tv));
    _mm256_storeu_si256((__m256i *)t_mag_sq, _mm256_madd_epi16(th_tv, th_tv));
    for (int k = 0; k < 8; ++k) {
        flag[k] = -adm_angle_flag(ot_dp[k], o_mag_sq[k], t_mag_sq[k], cos_1deg_sq);
    }
    return _mm256_setr_epi32(flag[0], flag[1], flag[2], flag[3], flag[4], flag[5], flag[6],
                             flag[7]);
}

/* Reciprocal-table entries of eight denominators. */
static FORCE_INLINE __m256i decouple_div_avx2(__m256i o, const int32_t *lut)
{
    return _mm256_i32gather_epi32(lut, _mm256_add_epi32(o, _mm256_set1_epi32(32768)), 4);
}

/* Q15 ratio k = t / o of eight samples from their reciprocal-table entries
 * `div`, clamped to [0, 32768]. */
static FORCE_INLINE __m256i decouple_k_avx2(__m256i o, __m256i t, __m256i div)
{
    const __m256i const_32768 = _mm256_set1_epi32(32768);
    const __m256i const_16384 = _mm256_set1_epi64x(16384);

    __m256i lo = _mm256_mul_epi32(div, t);
    __m256i hi = _mm256_mul_epi32(_mm256_srli_epi64(div, 32), _mm256_srli_epi64(t, 32));
    lo = sra15_epi64(_mm256_add_epi64(lo, const_16384));
    hi = sra15_epi64(_mm256_add_epi64(hi, const_16384));

    __m256i k = _mm256_or_si256(_mm256_and_si256(lo, _mm256_set1_epi64x(0xFFFFFFFF)),
                                _mm256_slli_epi64(hi, 32));
    const __m256i eqz = _mm256_cmpeq_epi32(o, _mm256_setzero_si256());
    k = _mm256_andnot_si256(eqz, k);
    k = _mm256_or_si256(k, _mm256_and_si256(const_32768, eqz));
    k = _mm256_max_epi32(k, _mm256_setzero_si256());
    return _mm256_min_epi32(k, const_32768);
}

/* rst * gain of eight samples, converted back as _mm256_cvtpd_epi32 does. */
static FORCE_INLINE __m256i decouple_gain_avx2(__m256i rst, double gain)
{
    const __m256d g = _mm256_set1_pd(gain);
    const __m256d lo = _mm256_mul_pd(_mm256_cvtepi32_pd(_mm256_extractf128_si256(rst, 0)), g);
    const __m256d hi = _mm256_mul_pd(_mm256_cvtepi32_pd(_mm256_extractf128_si256(rst, 1)), g);
    return _mm256_insertf128_si256(_mm256_castsi128_si256(_mm256_cvtpd_epi32(lo)),
                                   _mm256_cvtpd_epi32(hi), 1);
}

/* One band of eight samples: the restored signal and the additive impairment,
 * each packed to int16 in the low half of its vector (adm_decouple_band()). */
static FORCE_INLINE void decouple_band_avx2(__m256i o, __m256i t, __m256i div, __m256i angle_mask,
                                            double gain, __m256i *r_out, __m256i *a_out)
{
    const __m256i k = decouple_k_avx2(o, t, div);
    __m256i rst = _mm256_mullo_epi32(k, o);
    rst = _mm256_srai_epi32(_mm256_add_epi32(rst, _mm256_set1_epi32(16384)), 15);

    const __m256 k_f = _mm256_mul_ps(_mm256_set1_ps((float)1 / 32768), _mm256_cvtepi32_ps(k));
    const __m256 o_f = _mm256_mul_ps(_mm256_set1_ps((float)1 / 64), _mm256_cvtepi32_ps(o));
    const __m256 rst_f = _mm256_mul_ps(k_f, o_f);
    /* GCC/clang allow C-style cast `(__m256i)x` between vector types
     * via the GNU vector extension; MSVC rejects it (C2440). The
     * portable form is the dedicated bit-cast intrinsic. */
    const __m256i gt0 = _mm256_castps_si256(_mm256_cmp_ps(rst_f, _mm256_setzero_ps(), _CMP_GT_OS));
    const __m256i lt0 = _mm256_castps_si256(_mm256_cmp_ps(rst_f, _mm256_setzero_ps(), _CMP_LT_OS));
    const __m256i mask = _mm256_and_si256(_mm256_or_si256(gt0, lt0), angle_mask);

    const __m256i rst_gain = decouple_gain_avx2(rst, gain);
    const __m256i v_min = _mm256_and_si256(_mm256_min_epi32(rst_gain, t), gt0);
    const __m256i v_max = _mm256_and_si256(_mm256_max_epi32(rst_gain, t), lt0);
    const __m256i min_max = _mm256_and_si256(_mm256_or_si256(v_min, v_max), mask);
    rst = _mm256_or_si256(min_max, _mm256_andnot_si256(mask, rst));

    const __m256i a = _mm256_sub_epi32(t, rst);
    *r_out = _mm256_packs_epi32(rst, _mm256_permute4x64_epi64(rst, 0x0E));
    *a_out = _mm256_packs_epi32(a, _mm256_permute4x64_epi64(a, 0x0E));
}

/* Eight samples of all three bands, starting at `idx`. */
static FORCE_INLINE void decouple_block_avx2(const AdmBuffer *buf, ptrdiff_t idx,
                                             const int32_t *lut, double gain, float cos_1deg_sq)
{
    const adm_dwt_band_t *ref = &buf->ref_dwt2;
    const adm_dwt_band_t *dis = &buf->dis_dwt2;
    const adm_dwt_band_t *r = &buf->decouple_r;
    const adm_dwt_band_t *a = &buf->decouple_a;

    const __m256i oh = load_epi16x8(ref->band_h + idx);
    const __m256i ov = load_epi16x8(ref->band_v + idx);
    const __m256i od = load_epi16x8(ref->band_d + idx);
    const __m256i th = load_epi16x8(dis->band_h + idx);
    const __m256i tv = load_epi16x8(dis->band_v + idx);
    const __m256i td = load_epi16x8(dis->band_d + idx);

    const __m256i angle = decouple_angle_mask_avx2(oh, ov, th, tv, cos_1deg_sq);

    /* The three table lookups stay back to back, ahead of the per-band work:
     * a gather issued between the floating-point steps of two bands costs
     * about a quarter of this stage's throughput. */
    const __m256i div_h = decouple_div_avx2(oh, lut);
    const __m256i div_v = decouple_div_avx2(ov, lut);
    const __m256i div_d = decouple_div_avx2(od, lut);

    __m256i rst[3];
    __m256i add[3];
    decouple_band_avx2(oh, th, div_h, angle, gain, &rst[0], &add[0]);
    decouple_band_avx2(ov, tv, div_v, angle, gain, &rst[1], &add[1]);
    decouple_band_avx2(od, td, div_d, angle, gain, &rst[2], &add[2]);

    _mm_storeu_si128((__m128i *)(r->band_h + idx), _mm256_castsi256_si128(rst[0]));
    _mm_storeu_si128((__m128i *)(r->band_v + idx), _mm256_castsi256_si128(rst[1]));
    _mm_storeu_si128((__m128i *)(r->band_d + idx), _mm256_castsi256_si128(rst[2]));
    _mm_storeu_si128((__m128i *)(a->band_h + idx), _mm256_castsi256_si128(add[0]));
    _mm_storeu_si128((__m128i *)(a->band_v + idx), _mm256_castsi256_si128(add[1]));
    _mm_storeu_si128((__m128i *)(a->band_d + idx), _mm256_castsi256_si128(add[2]));
}

/* `adm_div_lookup` keeps the mutable `int32_t *` of the dispatch signature it
 * shares with the scalar twin (integer_adm.c, ADR-1141). */
// NOLINTNEXTLINE(readability-non-const-parameter) — ADR-0141 / ADR-1141
void adm_decouple_avx2(AdmBuffer *buf, int w, int h, int stride, double adm_enhn_gain_limit,
                       int32_t *adm_div_lookup)
{
    const float cos_1deg_sq = adm_cos_1deg_sq();

    /* The computation of the score is not required for the regions
     * which lie outside the frame borders */
    const AdmBorder b = adm_border_filt(w, h);

    /* The vector loop starts at `left`, so the tail bound must be a multiple of
     * 8 columns away from `left`, not from zero; otherwise the last 8-wide
     * store runs past `right` (Netflix/vmaf 03b5562c5). */
    const int right_mod8 = b.right - ((b.right - b.left) % 8);

    for (int i = b.top; i < b.bottom; ++i) {
        for (int j = b.left; j < right_mod8; j += 8) {
            decouple_block_avx2(buf, (ptrdiff_t)i * stride + j, adm_div_lookup, adm_enhn_gain_limit,
                                cos_1deg_sq);
        }
        adm_decouple_cols(buf, i, stride, right_mod8, b.right, adm_enhn_gain_limit, adm_div_lookup,
                          cos_1deg_sq);
    }
}

/* ------------------------------------------------------------------------- */
/* Decouple, scales 1..3                                                     */
/* ------------------------------------------------------------------------- */

/* angle_flag of four samples held in int64 lanes, as 0 / -1 lanes. */
static FORCE_INLINE __m256i decouple_s123_angle_mask_avx2(__m256i oh, __m256i ov, __m256i th,
                                                          __m256i tv, float cos_1deg_sq)
{
    int64_t ot_dp[4];
    int64_t o_mag_sq[4];
    int64_t t_mag_sq[4];
    int64_t flag[4];

    _mm256_storeu_si256((__m256i *)ot_dp,
                        _mm256_add_epi64(_mm256_mul_epi32(oh, th), _mm256_mul_epi32(ov, tv)));
    _mm256_storeu_si256((__m256i *)o_mag_sq,
                        _mm256_add_epi64(_mm256_mul_epi32(oh, oh), _mm256_mul_epi32(ov, ov)));
    _mm256_storeu_si256((__m256i *)t_mag_sq,
                        _mm256_add_epi64(_mm256_mul_epi32(th, th), _mm256_mul_epi32(tv, tv)));
    for (int k = 0; k < 4; ++k) {
        flag[k] = -(int64_t)adm_angle_flag(ot_dp[k], o_mag_sq[k], t_mag_sq[k], cos_1deg_sq);
    }
    return _mm256_setr_epi64x(flag[0], flag[1], flag[2], flag[3]);
}

/* What the table division of one band of eight samples needs: the table
 * entry, the sign of the denominator, the rounding term and the shift. */
typedef struct DecoupleS123Div {
    __m256i div;
    __m256i sign;
    __m256i round;
    __m256i count;
} DecoupleS123Div;

/* get_best15_from32() of eight magnitudes. __builtin_clz has no AVX2
 * equivalent, so the lanes take a scalar round trip. A magnitude below 32768
 * is its own 15-bit value, with a shift of 0. */
static FORCE_INLINE void decouple_s123_best15_avx2(__m256i abs_o, __m256i *msb, __m256i *shift)
{
    uint32_t v[8];
    int32_t m[8];
    int32_t s[8];

    _mm256_storeu_si256((__m256i *)v, abs_o);
    for (int k = 0; k < 8; ++k) {
        s[k] = 0;
        m[k] = (v[k] < 32768u) ? (int32_t)v[k] : (int32_t)get_best15_from32(v[k], &s[k]);
    }
    *msb = _mm256_setr_epi32(m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7]);
    *shift = _mm256_setr_epi32(s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7]);
}

/* The division terms of one band from its 15-bit denominators `msb` and
 * their shifts. */
static FORCE_INLINE DecoupleS123Div decouple_s123_div_avx2(__m256i o, __m256i msb, __m256i shift,
                                                           const int32_t *lut)
{
    const __m256i const_1 = _mm256_set1_epi32(1);
    DecoupleS123Div d;
    d.div = _mm256_i32gather_epi32(lut, _mm256_add_epi32(msb, _mm256_set1_epi32(32768)), 4);
    // cmpgt_epi32 returns -1 : 0 if a > b. We really want -1 : 1 to match c code, so we include an or_si256.
    d.sign = _mm256_or_si256(_mm256_cmpgt_epi32(_mm256_setzero_si256(), o), const_1);
    d.round = _mm256_sllv_epi32(const_1, _mm256_add_epi32(_mm256_set1_epi32(14), shift));
    d.count = _mm256_add_epi32(_mm256_set1_epi32(15), shift);
    return d;
}

/* Table quotient k = t / o of four samples in int64 lanes, clamped to
 * [0, 32768]. `div`, `sign`, `round` and `count` are the DecoupleS123Div
 * members of the same four samples. */
static FORCE_INLINE __m256i decouple_s123_k_half_avx2(__m256i o, __m256i t, __m256i div,
                                                      __m256i sign, __m256i round, __m256i count)
{
    const __m256i zero = _mm256_setzero_si256();
    const __m256i const_32768 = _mm256_set1_epi64x(32768);

    __m256i tmp_k = _mm256_mul_epi32(div, _mm256_mul_epi32(t, sign));
    tmp_k = sra_epi64(_mm256_add_epi64(tmp_k, round), count);
    tmp_k = blend(const_32768, tmp_k, _mm256_cmpeq_epi64(o, zero));

    const __m256i k = blend(const_32768, tmp_k, _mm256_cmpgt_epi64(tmp_k, const_32768));
    return blend(zero, k, _mm256_cmpgt_epi64(zero, tmp_k));
}

/* (int64_t)(rst * gain) of four samples; the conversion truncates, as the C
 * cast of the scalar kernel does. */
static FORCE_INLINE __m256i decouple_s123_gain_half_avx2(__m128i rst, double gain)
{
    double g[4];
    _mm256_storeu_pd(g, _mm256_mul_pd(_mm256_cvtepi32_pd(rst), _mm256_set1_pd(gain)));
    return _mm256_setr_epi64x((int64_t)g[0], (int64_t)g[1], (int64_t)g[2], (int64_t)g[3]);
}

/* Bound four restored samples by the gain limit where the angle flag is set:
 * min(rst * gain, t) for a positive rst_f, max(rst * gain, t) for a negative
 * one. */
static FORCE_INLINE __m256i decouple_s123_limit_half_avx2(__m256i rst, __m256i t, __m256i rst_gain,
                                                          __m256i angle_mask, __m256d rst_f)
{
    const __m256d zero = _mm256_set1_pd(0.0);
    const __m256i v_min = blend(rst_gain, t, _mm256_cmpgt_epi64(t, rst_gain));
    const __m256i v_max = blend(rst_gain, t, _mm256_cmpgt_epi64(rst_gain, t));
    const __m256i gt =
        _mm256_and_si256(angle_mask, _mm256_castpd_si256(_mm256_cmp_pd(rst_f, zero, _CMP_GT_OS)));
    const __m256i lt =
        _mm256_and_si256(angle_mask, _mm256_castpd_si256(_mm256_cmp_pd(rst_f, zero, _CMP_LT_OS)));

    rst = blend(v_min, rst, gt);
    return blend(v_max, rst, lt);
}

/* k of eight samples of one band, as two vectors of four int64. */
static FORCE_INLINE void decouple_s123_k_avx2(__m256i o, __m256i t, const DecoupleS123Div *d,
                                              __m256i *k_lo, __m256i *k_hi)
{
    *k_lo = decouple_s123_k_half_avx2(widen_lo(o), widen_lo(t), widen_lo(d->div), widen_lo(d->sign),
                                      widen_lo(d->round), widen_lo(d->count));
    *k_hi = decouple_s123_k_half_avx2(widen_hi(o), widen_hi(t), widen_hi(d->div), widen_hi(d->sign),
                                      widen_hi(d->round), widen_hi(d->count));
}

/* The restored signal of eight samples of one band from their k
 * (adm_decouple_band_s123()). `angle_lo` and `angle_hi` are the angle masks of
 * the lower and upper four samples. */
static FORCE_INLINE __m256i decouple_s123_rst_avx2(__m256i o, __m256i t, __m256i k_lo, __m256i k_hi,
                                                   __m256i angle_lo, __m256i angle_hi, double gain)
{
    const __m256i const_16384 = _mm256_set1_epi64x(16384);

    /* The shift is logical: only the low 32 bits of each lane are kept. */
    __m256i rst_lo =
        _mm256_srli_epi64(_mm256_add_epi64(_mm256_mul_epi32(k_lo, widen_lo(o)), const_16384), 15);
    __m256i rst_hi =
        _mm256_srli_epi64(_mm256_add_epi64(_mm256_mul_epi32(k_hi, widen_hi(o)), const_16384), 15);
    const __m256i rst_32 = narrow_epi64(rst_lo, rst_hi);

    const __m256 k_f = _mm256_cvtepi32_ps(narrow_epi64(k_lo, k_hi));
    const __m256 rst_f =
        _mm256_mul_ps(_mm256_mul_ps(k_f, _mm256_set1_ps((double)1 / 32768)),
                      _mm256_mul_ps(_mm256_cvtepi32_ps(o), _mm256_set1_ps((double)1 / 64)));

    const __m256i gain_lo = decouple_s123_gain_half_avx2(_mm256_extracti128_si256(rst_32, 0), gain);
    const __m256i gain_hi = decouple_s123_gain_half_avx2(_mm256_extracti128_si256(rst_32, 1), gain);
    rst_lo = decouple_s123_limit_half_avx2(rst_lo, widen_lo(t), gain_lo, angle_lo,
                                           _mm256_cvtps_pd(_mm256_extractf128_ps(rst_f, 0)));
    rst_hi = decouple_s123_limit_half_avx2(rst_hi, widen_hi(t), gain_hi, angle_hi,
                                           _mm256_cvtps_pd(_mm256_extractf128_ps(rst_f, 1)));
    return narrow_epi64(rst_lo, rst_hi);
}

/* Eight samples of all three bands, starting at `idx`. The bands advance
 * together, one step at a time: each step ends in a scalar round trip, and
 * three of those chains only overlap when they are issued side by side. */
static FORCE_INLINE void decouple_s123_block_avx2(const AdmBuffer *buf, ptrdiff_t idx,
                                                  const int32_t *lut, double gain,
                                                  float cos_1deg_sq)
{
    const i4_adm_dwt_band_t *ref = &buf->i4_ref_dwt2;
    const i4_adm_dwt_band_t *dis = &buf->i4_dis_dwt2;
    const i4_adm_dwt_band_t *r = &buf->i4_decouple_r;
    const i4_adm_dwt_band_t *a = &buf->i4_decouple_a;
    const int32_t *const ref_bands[3] = {ref->band_h, ref->band_v, ref->band_d};
    const int32_t *const dis_bands[3] = {dis->band_h, dis->band_v, dis->band_d};
    int32_t *const r_bands[3] = {r->band_h, r->band_v, r->band_d};
    int32_t *const a_bands[3] = {a->band_h, a->band_v, a->band_d};
    __m256i o[3];
    __m256i t[3];
    __m256i msb[3];
    __m256i shift[3];
    DecoupleS123Div div[3];
    __m256i k_lo[3];
    __m256i k_hi[3];
    __m256i rst[3];

    for (int n = 0; n < 3; ++n) {
        o[n] = _mm256_loadu_si256((const __m256i *)(ref_bands[n] + idx));
        t[n] = _mm256_loadu_si256((const __m256i *)(dis_bands[n] + idx));
    }
    const __m256i angle_lo = decouple_s123_angle_mask_avx2(
        widen_lo(o[0]), widen_lo(o[1]), widen_lo(t[0]), widen_lo(t[1]), cos_1deg_sq);
    const __m256i angle_hi = decouple_s123_angle_mask_avx2(
        widen_hi(o[0]), widen_hi(o[1]), widen_hi(t[0]), widen_hi(t[1]), cos_1deg_sq);

    for (int n = 0; n < 3; ++n) {
        decouple_s123_best15_avx2(_mm256_abs_epi32(o[n]), &msb[n], &shift[n]);
    }
    for (int n = 0; n < 3; ++n) {
        div[n] = decouple_s123_div_avx2(o[n], msb[n], shift[n], lut);
    }
    for (int n = 0; n < 3; ++n) {
        decouple_s123_k_avx2(o[n], t[n], &div[n], &k_lo[n], &k_hi[n]);
    }
    for (int n = 0; n < 3; ++n) {
        rst[n] = decouple_s123_rst_avx2(o[n], t[n], k_lo[n], k_hi[n], angle_lo, angle_hi, gain);
    }
    for (int n = 0; n < 3; ++n) {
        _mm256_storeu_si256((__m256i *)(r_bands[n] + idx), rst[n]);
    }
    for (int n = 0; n < 3; ++n) {
        _mm256_storeu_si256((__m256i *)(a_bands[n] + idx), _mm256_sub_epi32(t[n], rst[n]));
    }
}

/* See adm_decouple_avx2() for the `adm_div_lookup` qualifier. */
// NOLINTNEXTLINE(readability-non-const-parameter) — ADR-0141 / ADR-1141
void adm_decouple_s123_avx2(AdmBuffer *buf, int w, int h, int stride, double adm_enhn_gain_limit,
                            int32_t *adm_div_lookup)
{
    const float cos_1deg_sq = adm_cos_1deg_sq();

    /* The computation of the score is not required for the regions
     * which lie outside the frame borders */
    const AdmBorder b = adm_border_filt(w, h);
    const int right_mod8 = b.right - ((b.right - b.left) % 8);

    for (int i = b.top; i < b.bottom; ++i) {
        for (int j = b.left; j < right_mod8; j += 8) {
            decouple_s123_block_avx2(buf, (ptrdiff_t)i * stride + j, adm_div_lookup,
                                     adm_enhn_gain_limit, cos_1deg_sq);
        }
        adm_decouple_s123_cols(buf, i, stride, right_mod8, b.right, adm_enhn_gain_limit,
                               adm_div_lookup, cos_1deg_sq);
    }
}

/* ------------------------------------------------------------------------- */
/* DWT, scale 0                                                              */
/* ------------------------------------------------------------------------- */

/* The low-pass and high-pass taps as int16 pairs for _mm256_madd_epi16. */
typedef struct Dwt2Filters {
    __m256i lo01;
    __m256i lo23;
    __m256i hi01;
    __m256i hi23;
} Dwt2Filters;

static FORCE_INLINE Dwt2Filters dwt2_filters_avx2(void)
{
    Dwt2Filters f;
    f.lo01 = _mm256_broadcastd_epi32(_mm_loadu_si128((const __m128i *)dwt2_db2_coeffs_lo));
    f.lo23 = _mm256_broadcastd_epi32(_mm_loadu_si128((const __m128i *)(dwt2_db2_coeffs_lo + 2)));
    f.hi01 = _mm256_broadcastd_epi32(_mm_loadu_si128((const __m128i *)dwt2_db2_coeffs_hi));
    f.hi23 = _mm256_broadcastd_epi32(_mm_loadu_si128((const __m128i *)(dwt2_db2_coeffs_hi + 2)));
    return f;
}

/* Sixteen outputs of one filter of the 8-bit vertical pass. `s01` and `s23`
 * interleave the rows of taps 0, 1 and 2, 3; `sum_const` is the coefficient
 * sum that recentres the unsigned input. */
static FORCE_INLINE __m256i dwt2_8_vfilter_avx2(__m256i s01_lo, __m256i s01_hi, __m256i s23_lo,
                                                __m256i s23_hi, __m256i f01, __m256i f23,
                                                __m256i sum_const)
{
    const __m256i add_shift_VP = _mm256_set1_epi32(128);
    const __m256i pad = _mm256_setzero_si256();

    __m256i lo = _mm256_add_epi32(_mm256_madd_epi16(s01_lo, f01), _mm256_madd_epi16(s23_lo, f23));
    __m256i hi = _mm256_add_epi32(_mm256_madd_epi16(s01_hi, f01), _mm256_madd_epi16(s23_hi, f23));
    lo = _mm256_sub_epi32(lo, sum_const);
    hi = _mm256_sub_epi32(hi, sum_const);
    lo = _mm256_srli_epi32(_mm256_add_epi32(lo, add_shift_VP), 0x08);
    hi = _mm256_srli_epi32(_mm256_add_epi32(hi, add_shift_VP), 0x08);
    lo = _mm256_blend_epi16(lo, pad, 0xAA);
    hi = _mm256_blend_epi16(hi, pad, 0xAA);
    return _mm256_packus_epi32(lo, hi);
}

/* Vertical pass of sixteen columns of output row `i`, starting at `j`. */
static FORCE_INLINE void dwt2_8_vpass_block_avx2(const uint8_t *src, int *const *ind_y, int i,
                                                 int src_stride, int j, const Dwt2Filters *f,
                                                 int16_t *tmplo, int16_t *tmphi)
{
    const __m256i lo_sum = _mm256_set1_epi32(dwt2_db2_coeffs_lo_sum * 128);
    const __m256i hi_sum = _mm256_set1_epi32(dwt2_db2_coeffs_hi_sum * 128);
    const __m256i s0 = _mm256_cvtepu8_epi16(
        _mm_loadu_si128((const __m128i *)(src + ((ptrdiff_t)ind_y[0][i] * src_stride) + j)));
    const __m256i s1 = _mm256_cvtepu8_epi16(
        _mm_loadu_si128((const __m128i *)(src + ((ptrdiff_t)ind_y[1][i] * src_stride) + j)));
    const __m256i s2 = _mm256_cvtepu8_epi16(
        _mm_loadu_si128((const __m128i *)(src + ((ptrdiff_t)ind_y[2][i] * src_stride) + j)));
    const __m256i s3 = _mm256_cvtepu8_epi16(
        _mm_loadu_si128((const __m128i *)(src + ((ptrdiff_t)ind_y[3][i] * src_stride) + j)));

    const __m256i s01_lo = _mm256_unpacklo_epi16(s0, s1);
    const __m256i s01_hi = _mm256_unpackhi_epi16(s0, s1);
    const __m256i s23_lo = _mm256_unpacklo_epi16(s2, s3);
    const __m256i s23_hi = _mm256_unpackhi_epi16(s2, s3);

    _mm256_storeu_si256((__m256i *)(tmplo + j), dwt2_8_vfilter_avx2(s01_lo, s01_hi, s23_lo, s23_hi,
                                                                    f->lo01, f->lo23, lo_sum));
    _mm256_storeu_si256((__m256i *)(tmphi + j), dwt2_8_vfilter_avx2(s01_lo, s01_hi, s23_lo, s23_hi,
                                                                    f->hi01, f->hi23, hi_sum));
}

/* Sixteen outputs of one filter of the horizontal pass. `s0` / `s2` hold the
 * samples of taps 0, 1 and 2, 3 of the first eight outputs, `s0_next` /
 * `s2_next` those of the last eight. */
static FORCE_INLINE __m256i dwt2_hfilter_avx2(__m256i s0, __m256i s2, __m256i s0_next,
                                              __m256i s2_next, __m256i f01, __m256i f23)
{
    const __m256i add_shift_HP = _mm256_set1_epi32(32768);

    __m256i lo = _mm256_add_epi32(_mm256_madd_epi16(s0, f01), _mm256_madd_epi16(s2, f23));
    __m256i hi = _mm256_add_epi32(_mm256_madd_epi16(s0_next, f01), _mm256_madd_epi16(s2_next, f23));
    lo = _mm256_srai_epi32(_mm256_add_epi32(lo, add_shift_HP), 16);
    hi = _mm256_srai_epi32(_mm256_add_epi32(hi, add_shift_HP), 16);
    return _mm256_permute4x64_epi64(_mm256_packs_epi32(lo, hi), 0xD8);
}

/* Horizontal pass of sixteen outputs from one row buffer: its low-pass into
 * `dst_lo`, its high-pass into `dst_hi`. */
static FORCE_INLINE void dwt2_hpass_block_avx2(const int16_t *tmp, int *const *ind_x, int j,
                                               const Dwt2Filters *f, int16_t *dst_lo,
                                               int16_t *dst_hi)
{
    const __m256i s0 = _mm256_loadu_si256((const __m256i *)(tmp + ind_x[0][j]));
    const __m256i s2 = _mm256_loadu_si256((const __m256i *)(tmp + ind_x[2][j]));
    const __m256i s0_next = _mm256_loadu_si256((const __m256i *)(tmp + 16 + ind_x[0][j]));
    const __m256i s2_next = _mm256_loadu_si256((const __m256i *)(tmp + 16 + ind_x[2][j]));

    _mm256_storeu_si256((__m256i *)dst_lo,
                        dwt2_hfilter_avx2(s0, s2, s0_next, s2_next, f->lo01, f->lo23));
    _mm256_storeu_si256((__m256i *)dst_hi,
                        dwt2_hfilter_avx2(s0, s2, s0_next, s2_next, f->hi01, f->hi23));
}

/* Horizontal pass of output row `i`: the first column and the tail in scalar
 * code, sixteen outputs at a time in between. */
static FORCE_INLINE void dwt2_hpass_row_avx2(const int16_t *tmplo, const int16_t *tmphi,
                                             const adm_dwt_band_t *dst, int *const *ind_x, int i,
                                             int w, int dst_stride, const Dwt2Filters *f)
{
    const ptrdiff_t row = (ptrdiff_t)i * dst_stride;
    const int half_w = (w + 1) / 2;
    const int half_w_mod16 = half_w >= 2 ? half_w - 1 - ((half_w - 2) % 16) : 1;

    adm_dwt2_hpass(tmplo, tmphi, dst, ind_x, i, 0, 1, dst_stride);
    for (int j = 1; j < half_w_mod16; j += 16) {
        dwt2_hpass_block_avx2(tmplo, ind_x, j, f, dst->band_a + row + j, dst->band_v + row + j);
        dwt2_hpass_block_avx2(tmphi, ind_x, j, f, dst->band_h + row + j, dst->band_d + row + j);
    }
    adm_dwt2_hpass(tmplo, tmphi, dst, ind_x, i, half_w_mod16, half_w, dst_stride);
}

void adm_dwt2_8_avx2(const uint8_t *src, const adm_dwt_band_t *dst, AdmBuffer *buf, int w, int h,
                     int src_stride, int dst_stride)
{
    int **ind_y = buf->ind_y;
    int **ind_x = buf->ind_x;
    int16_t *tmplo = (int16_t *)buf->tmp_ref;
    int16_t *tmphi = tmplo + w;
    const Dwt2Filters f = dwt2_filters_avx2();

    // Vertical pass 16 pixels at a time
    // Ensure we only process complete 16 element chunks that fit entirely
    // within bounds
    const int j_vp_end = (w / 16) * 16;

    for (int i = 0; i < (h + 1) / 2; ++i) {
        for (int j = 0; j < j_vp_end; j += 16) {
            dwt2_8_vpass_block_avx2(src, ind_y, i, src_stride, j, &f, tmplo, tmphi);
        }
        adm_dwt2_vpass_8(src, ind_y, i, src_stride, j_vp_end, w, tmplo, tmphi);
        dwt2_hpass_row_avx2(tmplo, tmphi, dst, ind_x, i, w, dst_stride, &f);
    }
}

void adm_dwt2_16_avx2(const uint16_t *src, const adm_dwt_band_t *dst, AdmBuffer *buf, int w, int h,
                      int src_stride, int dst_stride, int inp_size_bits)
{
    int **ind_y = buf->ind_y;
    int **ind_x = buf->ind_x;
    int16_t *tmplo = (int16_t *)buf->tmp_ref;
    int16_t *tmphi = tmplo + w;
    const Dwt2Filters f = dwt2_filters_avx2();

    for (int i = 0; i < (h + 1) / 2; ++i) {
        /* The vertical pass forms its response in int64 (a bright 16-bit
         * column overflows int32) and stays scalar. */
        adm_dwt2_vpass_16(src, ind_y, i, src_stride, 0, w, inp_size_bits, tmplo, tmphi);
        dwt2_hpass_row_avx2(tmplo, tmphi, dst, ind_x, i, w, dst_stride, &f);
    }
}

/* ------------------------------------------------------------------------- */
/* DWT, scales 1..3                                                          */
/* ------------------------------------------------------------------------- */

typedef struct I4Dwt2Consts {
    __m256i f_lo[4];
    __m256i f_hi[4];
    __m256i add_vp;
    __m256i add_hp;
    __m256i mask_vp;
    __m256i mask_hp;
    int shift_vp;
    int shift_hp;
} I4Dwt2Consts;

static FORCE_INLINE I4Dwt2Consts i4_dwt2_consts_avx2(const I4Dwt2Round *r)
{
    I4Dwt2Consts k;
    for (int t = 0; t < 4; ++t) {
        k.f_lo[t] = _mm256_set1_epi64x(dwt2_db2_coeffs_lo[t]);
        k.f_hi[t] = _mm256_set1_epi64x(dwt2_db2_coeffs_hi[t]);
    }
    k.add_vp = _mm256_set1_epi64x(r->add_vp);
    k.add_hp = _mm256_set1_epi64x(r->add_hp);
    k.shift_vp = r->shift_vp;
    k.shift_hp = r->shift_hp;
    k.mask_vp = _mm256_andnot_si256(_mm256_srli_epi64(_mm256_set1_epi64x(-1LL), k.shift_vp),
                                    _mm256_set1_epi8((char)0xFF));
    k.mask_hp = _mm256_andnot_si256(_mm256_srli_epi64(_mm256_set1_epi64x(-1LL), k.shift_hp),
                                    _mm256_set1_epi8((char)0xFF));
    return k;
}

/* Four-tap response of four samples held in the even int32 lanes. */
static FORCE_INLINE __m256i i4_dwt2_taps_avx2(__m256i s0, __m256i s1, __m256i s2, __m256i s3,
                                              const __m256i f[4])
{
    __m256i accum = _mm256_add_epi64(_mm256_mul_epi32(s0, f[0]), _mm256_mul_epi32(s1, f[1]));
    accum = _mm256_add_epi64(accum, _mm256_mul_epi32(s2, f[2]));
    return _mm256_add_epi64(accum, _mm256_mul_epi32(s3, f[3]));
}

/* (accum + add) >> shift of four int64 lanes, narrowed to four int32. */
static FORCE_INLINE __m128i i4_dwt2_round_avx2(__m256i accum, __m256i add, int shift,
                                               __m256i msb_mask)
{
    accum = sra_fit_epi64(_mm256_add_epi64(accum, add), shift, msb_mask);
    return _mm256_castsi256_si128(
        _mm256_permutevar8x32_epi32(accum, _mm256_setr_epi32(0, 2, 4, 6, 0, 0, 0, 0)));
}

/* Vertical pass of four columns of one plane of output row `i`. */
static FORCE_INLINE void i4_dwt2_vpass_block_avx2(const int32_t *src, int *const *ind_y, int i,
                                                  int stride, int j, const I4Dwt2Consts *k,
                                                  int32_t *tmplo, int32_t *tmphi)
{
    const __m256i s0 = load_epi32x4(src + ((ptrdiff_t)ind_y[0][i] * stride) + j);
    const __m256i s1 = load_epi32x4(src + ((ptrdiff_t)ind_y[1][i] * stride) + j);
    const __m256i s2 = load_epi32x4(src + ((ptrdiff_t)ind_y[2][i] * stride) + j);
    const __m256i s3 = load_epi32x4(src + ((ptrdiff_t)ind_y[3][i] * stride) + j);

    _mm_storeu_si128((__m128i *)(tmplo + j),
                     i4_dwt2_round_avx2(i4_dwt2_taps_avx2(s0, s1, s2, s3, k->f_lo), k->add_vp,
                                        k->shift_vp, k->mask_vp));
    _mm_storeu_si128((__m128i *)(tmphi + j),
                     i4_dwt2_round_avx2(i4_dwt2_taps_avx2(s0, s1, s2, s3, k->f_hi), k->add_vp,
                                        k->shift_vp, k->mask_vp));
}

/* Horizontal pass of four outputs from one row buffer: its low-pass into
 * `dst_lo`, its high-pass into `dst_hi`. */
static FORCE_INLINE void i4_dwt2_hpass_block_avx2(const int32_t *tmp, const int jx[4],
                                                  const I4Dwt2Consts *k, int32_t *dst_lo,
                                                  int32_t *dst_hi)
{
    const __m256i s0 = _mm256_loadu_si256((const __m256i *)(tmp + jx[0]));
    const __m256i s1 = _mm256_loadu_si256((const __m256i *)(tmp + jx[1]));
    const __m256i s2 = _mm256_loadu_si256((const __m256i *)(tmp + jx[2]));
    const __m256i s3 = _mm256_loadu_si256((const __m256i *)(tmp + jx[3]));

    _mm_storeu_si128((__m128i *)dst_lo,
                     i4_dwt2_round_avx2(i4_dwt2_taps_avx2(s0, s1, s2, s3, k->f_lo), k->add_hp,
                                        k->shift_hp, k->mask_hp));
    _mm_storeu_si128((__m128i *)dst_hi,
                     i4_dwt2_round_avx2(i4_dwt2_taps_avx2(s0, s1, s2, s3, k->f_hi), k->add_hp,
                                        k->shift_hp, k->mask_hp));
}

/* Horizontal pass of four outputs of both planes of output row `i`. */
static FORCE_INLINE void i4_dwt2_hpass_planes_avx2(const int32_t *tmp, const AdmBuffer *buf,
                                                   int *const *ind_x, int w, ptrdiff_t out, int j,
                                                   const I4Dwt2Consts *k)
{
    const i4_adm_dwt_band_t *ref = &buf->i4_ref_dwt2;
    const i4_adm_dwt_band_t *dis = &buf->i4_dis_dwt2;
    const int32_t *tmplo_ref = tmp;
    const int32_t *tmphi_ref = tmplo_ref + w;
    const int32_t *tmplo_dis = tmphi_ref + w;
    const int32_t *tmphi_dis = tmplo_dis + w;
    const int jx[4] = {ind_x[0][j], ind_x[1][j], ind_x[2][j], ind_x[3][j]};

    i4_dwt2_hpass_block_avx2(tmplo_ref, jx, k, ref->band_a + out, ref->band_v + out);
    i4_dwt2_hpass_block_avx2(tmphi_ref, jx, k, ref->band_h + out, ref->band_d + out);
    i4_dwt2_hpass_block_avx2(tmplo_dis, jx, k, dis->band_a + out, dis->band_v + out);
    i4_dwt2_hpass_block_avx2(tmphi_dis, jx, k, dis->band_h + out, dis->band_d + out);
}

void adm_dwt2_s123_combined_avx2(const int32_t *i4_ref_scale, const int32_t *i4_curr_dis,
                                 AdmBuffer *buf, int w, int h, int ref_stride, int dis_stride,
                                 int dst_stride, int scale)
{
    const I4Dwt2Round r = i4_dwt2_round(scale);
    const I4Dwt2Consts k = i4_dwt2_consts_avx2(&r);
    int **ind_y = buf->ind_y;
    int **ind_x = buf->ind_x;
    int32_t *tmp = buf->tmp_ref;
    int32_t *tmplo_dis = tmp + ((ptrdiff_t)2 * w);

    const int w_mod4 = (w - (w % 4));
    const int half_w = (w + 1) / 2;
    const int half_w_mod4 = half_w >= 2 ? half_w - 1 - ((half_w - 2) % 4) : 1;

    for (int i = 0; i < (h + 1) / 2; ++i) {
        /* Vertical pass. */
        for (int j = 0; j < w_mod4; j += 4) {
            i4_dwt2_vpass_block_avx2(i4_ref_scale, ind_y, i, ref_stride, j, &k, tmp, tmp + w);
            i4_dwt2_vpass_block_avx2(i4_curr_dis, ind_y, i, dis_stride, j, &k, tmplo_dis,
                                     tmplo_dis + w);
        }
        i4_dwt2_vpass(i4_ref_scale, i4_curr_dis, ind_y, i, ref_stride, dis_stride, w, w_mod4, w,
                      tmp, r.add_vp, r.shift_vp);

        /* Horizontal pass (lo and hi). */
        i4_dwt2_hpass(tmp, &buf->i4_ref_dwt2, &buf->i4_dis_dwt2, ind_x, i, w, 0, 1, dst_stride,
                      r.add_hp, r.shift_hp);
        for (int j = 1; j < half_w_mod4; j += 4) {
            i4_dwt2_hpass_planes_avx2(tmp, buf, ind_x, w, (ptrdiff_t)i * dst_stride + j, j, &k);
        }
        i4_dwt2_hpass(tmp, &buf->i4_ref_dwt2, &buf->i4_dis_dwt2, ind_x, i, w, half_w_mod4, half_w,
                      dst_stride, r.add_hp, r.shift_hp);
    }
}

/* ------------------------------------------------------------------------- */
/* Contrast sensitivity filtering                                            */
/* ------------------------------------------------------------------------- */

/* Eight samples of one scale-0 band (adm_csf_cols()). */
static FORCE_INLINE void csf_block_avx2(const int16_t *src, int16_t *dst, int16_t *flt,
                                        __m256i i_rfactor, __m256i shiftadd, int shift)
{
    const __m256i zero = _mm256_setzero_si256();
    const __m256i dst_val = _mm256_mullo_epi32(load_epi16x8(src), i_rfactor);
    const __m256i i16_dst_val = _mm256_srai_epi32(_mm256_add_epi32(dst_val, shiftadd), shift);
    _mm_storeu_si128((__m128i *)dst, _mm256_castsi256_si128(_mm256_permute4x64_epi64(
                                         _mm256_packs_epi32(i16_dst_val, zero), 0x8)));

    __m256i f =
        _mm256_mullo_epi32(_mm256_set1_epi32(ADM_FIX_ONE_BY_30), _mm256_abs_epi32(i16_dst_val));
    f = _mm256_srai_epi32(_mm256_add_epi32(f, _mm256_set1_epi32(2048)), 12);
    f = _mm256_packs_epi32(f, zero);
    _mm_storeu_si128((__m128i *)flt, _mm256_castsi256_si128(_mm256_permute4x64_epi64(f, 0x8)));
}

/* Columns [j0, j1) of the three bands of row `offset`, eight at a time. The
 * pointers and constants are read once here: the stores of the loop may alias
 * anything, so the compiler would otherwise reload them for every block. */
static FORCE_INLINE void csf_row_avx2(const AdmCsfBands *b, const uint16_t i_rfactor[3],
                                      ptrdiff_t offset, int j0, int j1)
{
    const int16_t *const src[3] = {b->src[0] + offset, b->src[1] + offset, b->src[2] + offset};
    int16_t *const dst[3] = {b->dst[0] + offset, b->dst[1] + offset, b->dst[2] + offset};
    int16_t *const flt[3] = {b->flt[0] + offset, b->flt[1] + offset, b->flt[2] + offset};
    const __m256i rfactor[3] = {_mm256_set1_epi32(i_rfactor[0]), _mm256_set1_epi32(i_rfactor[1]),
                                _mm256_set1_epi32(i_rfactor[2])};
    const __m256i shiftadd[3] = {_mm256_set1_epi32(adm_csf_shiftsadd[0]),
                                 _mm256_set1_epi32(adm_csf_shiftsadd[1]),
                                 _mm256_set1_epi32(adm_csf_shiftsadd[2])};

    for (int j = j0; j < j1; j += 8) {
        for (int theta = 0; theta < 3; ++theta) {
            csf_block_avx2(src[theta] + j, dst[theta] + j, flt[theta] + j, rfactor[theta],
                           shiftadd[theta], adm_csf_shifts[theta]);
        }
    }
}

void adm_csf_avx2(AdmBuffer *buf, int w, int h, int stride, double adm_norm_view_dist,
                  int adm_ref_display_height, int adm_csf_mode, double adm_csf_scale,
                  double adm_csf_diag_scale, bool measure_aim)
{
    const AdmCsfBands bands = adm_csf_bands(buf, measure_aim);
    uint16_t i_rfactor[3];
    (void)adm_csf_i_rfactor(adm_norm_view_dist, adm_ref_display_height, adm_csf_mode, adm_csf_scale,
                            adm_csf_diag_scale, i_rfactor);

    /* The computation of the csf values is not required for the regions which
     * lie outside the frame borders */
    const AdmBorder b = adm_border_filt(w, h);
    const int right_mod_8 = b.right - ((b.right - b.left) % 8);

    for (int i = b.top; i < b.bottom; ++i) {
        const ptrdiff_t offset = (ptrdiff_t)i * stride;

        csf_row_avx2(&bands, i_rfactor, offset, b.left, right_mod_8);
        for (int theta = 0; theta < 3; ++theta) {
            adm_csf_cols(&bands, i_rfactor, theta, offset, right_mod_8, b.right);
        }
    }
}

/* Eight samples of one band of scales 1..3 (i4_adm_csf_cols()). */
static FORCE_INLINE void i4_csf_block_avx2(const int32_t *src, int32_t *dst, int32_t *flt,
                                           __m256i r_factor, __m256i add_dst, __m256i add_flt,
                                           int shift_dst, int shift_flt)
{
    const __m256i msb_mask = _mm256_set1_epi64x((int64_t)0xFFFFFFF000000000ULL);
    const __m256i lo32 = _mm256_set1_epi64x(0x00000000FFFFFFFF);
    const __m256i one_by_30 = _mm256_set1_epi32((int)I4_ADM_FIX_ONE_BY_30);
    const __m256i s = _mm256_loadu_si256((const __m256i *)src);

    __m256i dst_lo = _mm256_mul_epi32(s, r_factor);
    __m256i dst_hi = _mm256_mul_epi32(_mm256_srli_epi64(s, 32), r_factor);
    dst_lo = sra_fit_epi64(_mm256_add_epi64(dst_lo, add_dst), shift_dst, msb_mask);
    dst_hi = sra_fit_epi64(_mm256_add_epi64(dst_hi, add_dst), shift_dst, msb_mask);
    dst_lo = _mm256_or_si256(_mm256_and_si256(dst_lo, lo32), _mm256_slli_epi64(dst_hi, 32));
    _mm256_storeu_si256((__m256i *)dst, dst_lo);

    const __m256i abs_dst = _mm256_abs_epi32(dst_lo);
    __m256i flt_lo = _mm256_mul_epi32(one_by_30, abs_dst);
    __m256i flt_hi = _mm256_mul_epi32(one_by_30, _mm256_srli_epi64(abs_dst, 32));
    flt_lo = _mm256_srli_epi64(_mm256_add_epi64(flt_lo, add_flt), shift_flt);
    flt_hi = _mm256_srli_epi64(_mm256_add_epi64(flt_hi, add_flt), shift_flt);
    flt_lo = _mm256_or_si256(_mm256_and_si256(flt_lo, lo32), _mm256_slli_epi64(flt_hi, 32));
    _mm256_storeu_si256((__m256i *)flt, flt_lo);
}

/* Columns [j0, j1) of the three bands of row `offset`, eight at a time; see
 * csf_row_avx2() for why the context is read once up front. */
static FORCE_INLINE void i4_csf_row_avx2(const I4AdmCsfCtx *c, ptrdiff_t offset, int j0, int j1)
{
    const int32_t *const src[3] = {c->src[0] + offset, c->src[1] + offset, c->src[2] + offset};
    int32_t *const dst[3] = {c->dst[0] + offset, c->dst[1] + offset, c->dst[2] + offset};
    int32_t *const flt[3] = {c->flt[0] + offset, c->flt[1] + offset, c->flt[2] + offset};
    const __m256i r_factor[3] = {_mm256_set1_epi32((int)c->i_rfactor[0]),
                                 _mm256_set1_epi32((int)c->i_rfactor[1]),
                                 _mm256_set1_epi32((int)c->i_rfactor[2])};
    const __m256i add_dst = _mm256_set1_epi64x(c->add_bef_shift_dst);
    const __m256i add_flt = _mm256_set1_epi64x(c->add_bef_shift_flt);
    const int shift_dst = (int)c->shift_dst;
    const int shift_flt = (int)c->shift_flt;

    for (int j = j0; j < j1; j += 8) {
        for (int theta = 0; theta < 3; ++theta) {
            i4_csf_block_avx2(src[theta] + j, dst[theta] + j, flt[theta] + j, r_factor[theta],
                              add_dst, add_flt, shift_dst, shift_flt);
        }
    }
}

void i4_adm_csf_avx2(AdmBuffer *buf, int scale, int w, int h, int stride, double adm_norm_view_dist,
                     int adm_ref_display_height, int adm_csf_mode, double adm_csf_scale,
                     double adm_csf_diag_scale, bool measure_aim)
{
    I4AdmCsfCtx c;
    i4_adm_csf_ctx_init(&c, buf, scale, adm_norm_view_dist, adm_ref_display_height, adm_csf_mode,
                        adm_csf_scale, adm_csf_diag_scale, measure_aim);

    /* The computation of the csf values is not required for the regions
     * which lie outside the frame borders */
    const AdmBorder b = adm_border_filt(w, h);
    const int right_mod_8 = b.right - ((b.right - b.left) % 8);

    for (int i = b.top; i < b.bottom; ++i) {
        const ptrdiff_t offset = (ptrdiff_t)i * stride;

        i4_csf_row_avx2(&c, offset, b.left, right_mod_8);
        for (int theta = 0; theta < 3; ++theta) {
            i4_adm_csf_cols(&c, theta, offset, right_mod_8, b.right);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Denominator (reference-energy) reductions                                 */
/* ------------------------------------------------------------------------- */

/* Cubed magnitudes of eight samples of one scale-0 band, added to two
 * accumulators of four uint64 lanes. */
static FORCE_INLINE void csf_den_cube_avx2(const int16_t *src, __m256i *accum_lo, __m256i *accum_hi)
{
    const __m256i v = _mm256_cvtepu16_epi32(_mm_abs_epi16(_mm_loadu_si128((const __m128i *)src)));
    const __m256i sq = _mm256_mullo_epi32(v, v);
    *accum_lo = _mm256_add_epi64(*accum_lo, _mm256_mul_epu32(sq, v));
    *accum_hi = _mm256_add_epi64(
        *accum_hi, _mm256_mul_epu32(_mm256_srli_epi64(sq, 32), _mm256_srli_epi64(v, 32)));
}

/* Sums of the cubed magnitudes of columns [j0, j1) of the three bands of one
 * row, eight at a time. */
static FORCE_INLINE void csf_den_row_avx2(const int16_t *src_h, const int16_t *src_v,
                                          const int16_t *src_d, int j0, int j1, uint64_t inner[3])
{
    __m256i accum_lo[3] = {_mm256_setzero_si256(), _mm256_setzero_si256(), _mm256_setzero_si256()};
    __m256i accum_hi[3] = {_mm256_setzero_si256(), _mm256_setzero_si256(), _mm256_setzero_si256()};

    for (int j = j0; j < j1; j += 8) {
        csf_den_cube_avx2(src_h + j, &accum_lo[0], &accum_hi[0]);
        csf_den_cube_avx2(src_v + j, &accum_lo[1], &accum_hi[1]);
        csf_den_cube_avx2(src_d + j, &accum_lo[2], &accum_hi[2]);
    }
    for (int k = 0; k < 3; ++k) {
        inner[k] = hsum_epu64(_mm256_add_epi64(accum_lo[k], accum_hi[k]));
    }
}

float adm_csf_den_scale_avx2(const adm_dwt_band_t *src, int w, int h, int src_stride,
                             double adm_norm_view_dist, int adm_ref_display_height,
                             int adm_csf_mode, double adm_csf_scale, double adm_csf_diag_scale,
                             double adm_noise_weight)
{
    AdmDenCtx c;
    adm_csf_den_ctx_init(&c, w, h, adm_norm_view_dist, adm_ref_display_height, adm_csf_mode,
                         adm_csf_scale, adm_csf_diag_scale);
    const int right_mod_8 = c.b.right - ((c.b.right - c.b.left) % 8);

    uint64_t accum[3] = {0, 0, 0};
    uint64_t inner[3] = {0, 0, 0};

    const int16_t *src_h = src->band_h + (ptrdiff_t)c.b.top * src_stride;
    const int16_t *src_v = src->band_v + (ptrdiff_t)c.b.top * src_stride;
    const int16_t *src_d = src->band_d + (ptrdiff_t)c.b.top * src_stride;
    for (int i = c.b.top; i < c.b.bottom; ++i) {
        csf_den_row_avx2(src_h, src_v, src_d, c.b.left, right_mod_8, inner);
        adm_csf_den_cols(src_h, src_v, src_d, right_mod_8, c.b.right, inner);
        adm_csf_den_fold(inner, accum, (uint32_t)c.add_shift_accum, (uint32_t)c.shift_accum);
        src_h += src_stride;
        src_v += src_stride;
        src_d += src_stride;
    }
    return adm_csf_den_result(&c, accum, adm_noise_weight);
}

/* Cube terms of four samples of one band of scales 1..3 (i4_cube_term()). */
static FORCE_INLINE __m256i i4_csf_den_cube_avx2(const int32_t *src, __m256i add_sq,
                                                 __m256i add_cub, int shift_sq, int shift_cub)
{
    const __m256i v = _mm256_cvtepu32_epi64(_mm_abs_epi32(_mm_loadu_si128((const __m128i *)src)));
    __m256i sq = _mm256_add_epi64(_mm256_mul_epu32(v, v), add_sq);
    sq = _mm256_srli_epi64(sq, shift_sq);
    const __m256i cu = _mm256_add_epi64(_mm256_mul_epu32(sq, v), add_cub);
    return _mm256_srli_epi64(cu, shift_cub);
}

/* Sums of the cube terms of columns [j0, j1) of the three bands of one row,
 * four at a time. */
static FORCE_INLINE void i4_csf_den_row_avx2(const I4AdmDenCtx *c, const int32_t *src_h,
                                             const int32_t *src_v, const int32_t *src_d, int j0,
                                             int j1, uint64_t inner[3])
{
    const __m256i add_sq = _mm256_set1_epi64x(c->add_shift_sq);
    const __m256i add_cub = _mm256_set1_epi64x(c->add_shift_cub);
    const int shift_sq = (int)c->shift_sq;
    const int shift_cub = (int)c->shift_cub;
    __m256i accum[3] = {_mm256_setzero_si256(), _mm256_setzero_si256(), _mm256_setzero_si256()};

    for (int j = j0; j < j1; j += 4) {
        accum[0] = _mm256_add_epi64(
            accum[0], i4_csf_den_cube_avx2(src_h + j, add_sq, add_cub, shift_sq, shift_cub));
        accum[1] = _mm256_add_epi64(
            accum[1], i4_csf_den_cube_avx2(src_v + j, add_sq, add_cub, shift_sq, shift_cub));
        accum[2] = _mm256_add_epi64(
            accum[2], i4_csf_den_cube_avx2(src_d + j, add_sq, add_cub, shift_sq, shift_cub));
    }
    for (int k = 0; k < 3; ++k) {
        inner[k] = hsum_epu64(accum[k]);
    }
}

float adm_csf_den_s123_avx2(const i4_adm_dwt_band_t *src, int scale, int w, int h, int src_stride,
                            double adm_norm_view_dist, int adm_ref_display_height, int adm_csf_mode,
                            double adm_csf_scale, double adm_csf_diag_scale,
                            double adm_noise_weight)
{
    I4AdmDenCtx c;
    i4_adm_csf_den_ctx_init(&c, scale, w, h, adm_norm_view_dist, adm_ref_display_height,
                            adm_csf_mode, adm_csf_scale, adm_csf_diag_scale);
    const int right_mod_4 = c.b.right - ((c.b.right - c.b.left) % 4);

    uint64_t accum[3] = {0, 0, 0};
    uint64_t inner[3] = {0, 0, 0};

    const int32_t *src_h = src->band_h + (ptrdiff_t)c.b.top * src_stride;
    const int32_t *src_v = src->band_v + (ptrdiff_t)c.b.top * src_stride;
    const int32_t *src_d = src->band_d + (ptrdiff_t)c.b.top * src_stride;
    for (int i = c.b.top; i < c.b.bottom; ++i) {
        i4_csf_den_row_avx2(&c, src_h, src_v, src_d, c.b.left, right_mod_4, inner);
        i4_adm_csf_den_cols(&c, src_h, src_v, src_d, right_mod_4, c.b.right, inner);
        adm_csf_den_fold(inner, accum, c.add_shift_accum, c.shift_accum);
        src_h += src_stride;
        src_v += src_stride;
        src_d += src_stride;
    }
    return i4_adm_csf_den_result(&c, accum, adm_noise_weight);
}

/* ------------------------------------------------------------------------- */
/* Contrast masking (numerator), scale 0                                     */
/* ------------------------------------------------------------------------- */

/* One band's share of the masking threshold of the six columns starting at
 * `j` of an interior row `i`: the 3x3 sum of the filtered band, its centre
 * replaced by the unfiltered sample scaled by 1/15 (adm_cm_thresh()). Lanes 6
 * and 7 are not complete sums. */
static FORCE_INLINE __m256i cm_thresh_band_avx2(const int16_t *src, const int16_t *flt, int stride,
                                                int i, int j)
{
    const __m256i perm1 = _mm256_set_epi32(0, 7, 6, 5, 4, 3, 2, 1);
    const __m256i perm2 = _mm256_set_epi32(1, 0, 7, 6, 5, 4, 3, 2);
    const int16_t *flt_row = flt + ((ptrdiff_t)stride * (i - 1)) + j - 1;
    const __m256i flt0 = load_epi16x8(flt_row);
    const __m256i flt1 = load_epi16x8(flt_row + stride);
    const __m256i flt2 = load_epi16x8(flt_row + 2 * (ptrdiff_t)stride);

    __m256i centre = load_epi16x8(src + ((ptrdiff_t)stride * i) + j - 1);
    centre = _mm256_mullo_epi32(_mm256_abs_epi32(centre), _mm256_set1_epi32(ONE_BY_15));
    centre = _mm256_srai_epi32(_mm256_add_epi32(centre, _mm256_set1_epi32(2048)), 12);
    /* Wrap each tap to int16, as adm_cm_thresh()'s (int16_t) cast does: */
    centre = _mm256_srai_epi32(_mm256_slli_epi32(centre, 16), 16);
    centre = _mm256_sub_epi32(centre, flt1);

    const __m256i rows = _mm256_add_epi32(_mm256_add_epi32(flt0, flt1), flt2);
    __m256i sum = _mm256_add_epi32(_mm256_permutevar8x32_epi32(rows, perm1), rows);
    sum = _mm256_add_epi32(_mm256_permutevar8x32_epi32(rows, perm2), sum);
    return _mm256_add_epi32(sum, _mm256_permutevar8x32_epi32(centre, perm1));
}

/* Masking threshold of the six columns starting at `j`; lanes 6 and 7 are 0. */
static FORCE_INLINE __m256i cm_thresh_avx2(const AdmCmCtx *c, int i, int j)
{
    const __m256i mask_end = _mm256_set_epi64x(0x0, -1LL, -1LL, -1LL);
    __m256i sum = cm_thresh_band_avx2(c->angles[0], c->flt_angles[0], c->csf_a_stride, i, j);
    sum = _mm256_add_epi32(
        sum, cm_thresh_band_avx2(c->angles[1], c->flt_angles[1], c->csf_a_stride, i, j));
    sum = _mm256_add_epi32(
        sum, cm_thresh_band_avx2(c->angles[2], c->flt_angles[2], c->csf_a_stride, i, j));
    return _mm256_and_si256(mask_end, sum);
}

/* Rounded (|x| - thr)^3 of eight samples of one band, added to two
 * accumulators of four int64 lanes (adm_cm_accum_round()). */
static FORCE_INLINE void cm_accum_avx2(__m256i x, __m256i thr, const AdmCmBand *p,
                                       __m256i *accum_lo, __m256i *accum_hi)
{
    const __m256i add_sq = _mm256_set1_epi64x(p->add_shift_sq);
    const __m256i add_cub = _mm256_set1_epi64x(p->add_shift_cub);

    x = _mm256_sub_epi32(_mm256_abs_epi32(x), _mm256_slli_epi32(thr, p->shift_sub));
    x = _mm256_max_epi32(x, _mm256_setzero_si256());
    const __m256i x_hi = _mm256_srli_epi64(x, 32);

    __m256i lo = _mm256_srli_epi64(_mm256_add_epi64(_mm256_mul_epi32(x, x), add_sq), p->shift_sq);
    __m256i hi =
        _mm256_srli_epi64(_mm256_add_epi64(_mm256_mul_epi32(x_hi, x_hi), add_sq), p->shift_sq);
    lo = _mm256_srli_epi64(_mm256_add_epi64(_mm256_mul_epi32(lo, x), add_cub), (int)p->shift_cub);
    hi =
        _mm256_srli_epi64(_mm256_add_epi64(_mm256_mul_epi32(hi, x_hi), add_cub), (int)p->shift_cub);
    *accum_lo = _mm256_add_epi64(*accum_lo, lo);
    *accum_hi = _mm256_add_epi64(*accum_hi, hi);
}

/* An interior row, six columns at a time. A row that reaches the first or
 * the last column needs the mirrored neighbourhood and stays scalar. */
static void cm_row_avx2(const AdmCmCtx *c, int i, const AdmCmBounds *bd, int64_t inner[3])
{
    if (bd->left_edge || bd->right_edge) {
        adm_cm_row(c, i, bd, inner);
        return;
    }

    const __m256i lanes = _mm256_set_epi64x(0x0, -1LL, -1LL, -1LL);
    const int end_col_mod6 = bd->end_col - ((bd->end_col - bd->start_col) % 6);
    __m256i accum_lo[3] = {_mm256_setzero_si256(), _mm256_setzero_si256(), _mm256_setzero_si256()};
    __m256i accum_hi[3] = {_mm256_setzero_si256(), _mm256_setzero_si256(), _mm256_setzero_si256()};

    for (int j = bd->start_col; j < end_col_mod6; j += 6) {
        const ptrdiff_t idx = (ptrdiff_t)i * c->src_stride + j;
        const __m256i thr = cm_thresh_avx2(c, i, j);
        const int16_t *const src[3] = {c->src->band_h, c->src->band_v, c->src->band_d};

        for (int k = 0; k < 3; ++k) {
            const __m256i rfactor = _mm256_and_si256(_mm256_set1_epi32(c->i_rfactor[k]), lanes);
            const __m256i x = _mm256_mullo_epi32(load_epi16x8(src[k] + idx), rfactor);
            cm_accum_avx2(x, thr, &c->band[k], &accum_lo[k], &accum_hi[k]);
        }
    }
    for (int k = 0; k < 3; ++k) {
        inner[k] += hsum_epi64(_mm256_add_epi64(accum_lo[k], accum_hi[k]));
    }
    for (int j = end_col_mod6; j < bd->end_col; ++j) {
        adm_cm_accum_px(c, i, j, inner);
    }
}

float adm_cm_avx2(AdmBuffer *buf, int w, int h, int src_stride, int csf_a_stride,
                  double adm_norm_view_dist, int adm_ref_display_height, int adm_csf_mode,
                  double adm_csf_scale, double adm_csf_diag_scale, double adm_noise_weight,
                  double adm_p_norm, bool measure_aim)
{
    AdmCmCtx c;
    adm_cm_ctx_init(&c, buf, w, h, src_stride, csf_a_stride, adm_norm_view_dist,
                    adm_ref_display_height, adm_csf_mode, adm_csf_scale, adm_csf_diag_scale,
                    measure_aim);
    const AdmCmBounds bd = adm_cm_bounds(w, h);

    int64_t accum[3] = {0, 0, 0};
    adm_cm_rows(&c, &bd, cm_row_avx2, accum);
    return adm_cm_result(&c, &bd, accum, adm_noise_weight, adm_p_norm);
}

/* ------------------------------------------------------------------------- */
/* Contrast masking (numerator), scales 1..3                                 */
/* ------------------------------------------------------------------------- */

/* Constants of one i4 contrast-masking row. */
typedef struct I4CmConsts {
    __m256i rfactor[3];
    __m256i add_dst;
    __m256i add_flt;
    __m256i msb_dst;
    __m256i msb_flt;
} I4CmConsts;

static FORCE_INLINE I4CmConsts i4_cm_consts_avx2(const I4AdmCmCtx *c)
{
    const __m256i lanes = _mm256_set_epi64x(0x0, 0x0, -1LL, -1LL);
    I4CmConsts k;
    for (int t = 0; t < 3; ++t) {
        k.rfactor[t] = _mm256_and_si256(_mm256_set1_epi32((int)c->rfactor[t]), lanes);
    }
    k.add_dst = _mm256_set1_epi64x(c->add_bef_shift_dst);
    k.add_flt = _mm256_set1_epi64x(c->add_bef_shift_flt);
    k.msb_dst = _mm256_set1_epi64x((int64_t)0xFFFFFFF000000000ULL);
    k.msb_flt = _mm256_set1_epi64x((int64_t)0xFFFFFFFF00000000ULL);
    return k;
}

/* One band's share of the masking threshold of the two columns starting at
 * `j` of an interior row `i` (i4_adm_cm_thresh()). Lanes 2 and 3 are not
 * complete sums. */
static FORCE_INLINE __m256i i4_cm_thresh_band_avx2(const I4AdmCmCtx *c, const I4CmConsts *k,
                                                   int theta, int i, int j)
{
    const ptrdiff_t stride = c->csf_a_stride;
    const int32_t *flt_row = c->flt_angles[theta] + (stride * (i - 1)) + j - 1;
    const __m256i flt0 = load_epi32x4(flt_row);
    const __m256i flt1 = load_epi32x4(flt_row + stride);
    const __m256i flt2 = load_epi32x4(flt_row + 2 * stride);

    __m256i centre = abs_epi64_from32(load_epi32x4(c->angles[theta] + (stride * i) + j - 1));
    centre = _mm256_mul_epi32(centre, _mm256_set1_epi64x(I4_ONE_BY_15));
    centre = sra_sign_epi64(_mm256_add_epi64(centre, k->add_flt), (int)c->shift_flt, k->msb_flt);
    centre = _mm256_sub_epi64(centre, flt1);

    const __m256i rows = _mm256_add_epi64(_mm256_add_epi64(flt0, flt1), flt2);
    __m256i sum = _mm256_add_epi64(rows, _mm256_permute4x64_epi64(rows, 0x39));
    sum = _mm256_add_epi64(sum, _mm256_permute4x64_epi64(rows, 0x0E));
    return _mm256_add_epi64(sum, _mm256_permute4x64_epi64(centre, 0x39));
}

/* Masking threshold of the two columns starting at `j`; lanes 2 and 3 are 0. */
static FORCE_INLINE __m256i i4_cm_thresh_avx2(const I4AdmCmCtx *c, const I4CmConsts *k, int i,
                                              int j)
{
    const __m256i mask_end = _mm256_set_epi64x(0x0, 0x0, -1LL, -1LL);
    __m256i sum = i4_cm_thresh_band_avx2(c, k, 0, i, j);
    sum = _mm256_add_epi64(sum, i4_cm_thresh_band_avx2(c, k, 1, i, j));
    sum = _mm256_add_epi64(sum, i4_cm_thresh_band_avx2(c, k, 2, i, j));
    return _mm256_and_si256(mask_end, sum);
}

/* Rounded (|x| - thr)^3 of the samples of one band held in int64 lanes
 * (i4_adm_cm_accum_round()). */
static FORCE_INLINE __m256i i4_cm_cube_avx2(__m256i x, __m256i thr, const AdmCmBand *p)
{
    x = abs_epi64_from32(x);
    x = _mm256_sub_epi64(x, _mm256_srli_epi64(thr, p->shift_sub));
    x = _mm256_and_si256(x, _mm256_cmpgt_epi64(x, _mm256_setzero_si256()));

    __m256i v = _mm256_add_epi64(_mm256_mul_epi32(x, x), _mm256_set1_epi64x(p->add_shift_sq));
    v = _mm256_srli_epi64(v, p->shift_sq);
    v = _mm256_add_epi64(_mm256_mul_epi32(v, x), _mm256_set1_epi64x(p->add_shift_cub));
    return _mm256_srli_epi64(v, (int)p->shift_cub);
}

/* An interior row, two columns at a time; see cm_row_avx2(). */
static void i4_cm_row_avx2(const I4AdmCmCtx *c, int i, const AdmCmBounds *bd, int64_t inner[3])
{
    if (bd->left_edge || bd->right_edge) {
        i4_adm_cm_row(c, i, bd, inner);
        return;
    }

    const I4CmConsts k = i4_cm_consts_avx2(c);
    const int end_col_mod2 = bd->end_col - ((bd->end_col - bd->start_col) % 2);
    const int32_t *const src[3] = {c->src->band_h, c->src->band_v, c->src->band_d};
    __m256i accum[3] = {_mm256_setzero_si256(), _mm256_setzero_si256(), _mm256_setzero_si256()};

    for (int j = bd->start_col; j < end_col_mod2; j += 2) {
        const ptrdiff_t idx = (ptrdiff_t)i * c->src_stride + j;
        __m256i x[3];

        for (int t = 0; t < 3; ++t) {
            x[t] = _mm256_mul_epi32(load_epi32x4(src[t] + idx), k.rfactor[t]);
            x[t] = sra_sign_epi64(_mm256_add_epi64(x[t], k.add_dst), (int)c->shift_dst, k.msb_dst);
        }
        const __m256i thr = i4_cm_thresh_avx2(c, &k, i, j);
        for (int t = 0; t < 3; ++t) {
            accum[t] = _mm256_add_epi64(accum[t], i4_cm_cube_avx2(x[t], thr, &c->band));
        }
    }
    for (int t = 0; t < 3; ++t) {
        inner[t] += hsum_epi64(accum[t]);
    }
    for (int j = end_col_mod2; j < bd->end_col; ++j) {
        i4_adm_cm_accum_px(c, i, j, inner);
    }
}

float i4_adm_cm_avx2(AdmBuffer *buf, int w, int h, int src_stride, int csf_a_stride, int scale,
                     double adm_norm_view_dist, int adm_ref_display_height, int adm_csf_mode,
                     double adm_csf_scale, double adm_csf_diag_scale, double adm_noise_weight,
                     double adm_p_norm, bool measure_aim)
{
    I4AdmCmCtx c;
    i4_adm_cm_ctx_init(&c, buf, w, h, src_stride, csf_a_stride, scale, adm_norm_view_dist,
                       adm_ref_display_height, adm_csf_mode, adm_csf_scale, adm_csf_diag_scale,
                       measure_aim);
    const AdmCmBounds bd = adm_cm_bounds(w, h);

    int64_t accum[3] = {0, 0, 0};
    i4_adm_cm_rows(&c, &bd, i4_cm_row_avx2, accum);
    return i4_adm_cm_result(&c, &bd, accum, adm_noise_weight, adm_p_norm);
}
