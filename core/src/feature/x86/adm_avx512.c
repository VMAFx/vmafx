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
 * AVX-512 twins of the integer ADM stages.
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
#include "adm_avx512.h"

/* ------------------------------------------------------------------------- */
/* Lane helpers                                                              */
/* ------------------------------------------------------------------------- */

/* Sixteen int16 samples widened to int32 lanes. */
static FORCE_INLINE __m512i load_epi16x16(const int16_t *p)
{
    return _mm512_cvtepi16_epi32(_mm256_loadu_si256((const __m256i *)p));
}

/* Eight int32 samples widened to int64 lanes. */
static FORCE_INLINE __m512i load_epi32x8(const int32_t *p)
{
    return _mm512_cvtepi32_epi64(_mm256_loadu_si256((const __m256i *)p));
}

/* The lower / upper eight int32 lanes widened to int64 lanes. */
static FORCE_INLINE __m512i widen_lo(__m512i v)
{
    return _mm512_cvtepi32_epi64(_mm512_extracti32x8_epi32(v, 0));
}

static FORCE_INLINE __m512i widen_hi(__m512i v)
{
    return _mm512_cvtepi32_epi64(_mm512_extracti32x8_epi32(v, 1));
}

/* Two vectors of eight int64 truncated to one vector of sixteen int32. */
static FORCE_INLINE __m512i narrow_epi64(__m512i lo, __m512i hi)
{
    return _mm512_inserti32x8(_mm512_castsi256_si512(_mm512_cvtepi64_epi32(lo)),
                              _mm512_cvtepi64_epi32(hi), 1);
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

/* The eight 64-bit lanes folded to two. */
static FORCE_INLINE __m128i fold_epi64(__m512i v)
{
    const __m256i r4 = _mm256_add_epi64(_mm512_castsi512_si256(v), _mm512_extracti64x4_epi64(v, 1));
    return _mm_add_epi64(_mm256_castsi256_si128(r4), _mm256_extracti128_si256(r4, 1));
}

/* Sum of the eight int64 lanes. */
static FORCE_INLINE int64_t hsum_epi64(__m512i v)
{
    const __m128i r2 = fold_epi64(v);
    return (int64_t)extract_epi64_128(r2, 0) + (int64_t)extract_epi64_128(r2, 1);
}

/* Sum of the eight uint64 lanes. */
static FORCE_INLINE uint64_t hsum_epu64(__m512i v)
{
    const __m128i r2 = fold_epi64(v);
    return (uint64_t)extract_epi64_128(r2, 0) + (uint64_t)extract_epi64_128(r2, 1);
}

/* ------------------------------------------------------------------------- */
/* Decouple, scale 0                                                         */
/* ------------------------------------------------------------------------- */

/* The second half of the angle predicate for eight samples, in double:
 * (u.v)^2 >= cos(1deg)^2 * ||u||^2 * ||v||^2. */
static FORCE_INLINE __mmask8 decouple_angle_half_avx512(__m256 ot_dp, __m256 o_mag_sq,
                                                        __m256 t_mag_sq, __m512d cos_1deg_sq)
{
    const __m512d dp = _mm512_cvtps_pd(ot_dp);
    const __m512d dp_sq = _mm512_mul_pd(dp, dp);
    const __m512d mag = _mm512_mul_pd(_mm512_cvtps_pd(o_mag_sq), _mm512_cvtps_pd(t_mag_sq));
    return _mm512_cmp_pd_mask(dp_sq, _mm512_mul_pd(mag, cos_1deg_sq), _CMP_GE_OS);
}

/* angle_flag of sixteen samples. The dot product and the squared magnitudes
 * are formed in int32 by _mm512_madd_epi16, narrowed to float and compared in
 * double, as adm_angle_flag_fp64() does. */
static FORCE_INLINE __mmask16 decouple_angle_mask_avx512(__m512i oh, __m512i ov, __m512i th,
                                                         __m512i tv, float cos_1deg_sq)
{
    const __m512 inv_4096 = _mm512_set1_ps(1.0f / 4096.0f);
    const __m512d cos_pd = _mm512_set1_pd(cos_1deg_sq);
    const __m512i lo16 = _mm512_set1_epi32(0xFFFF);
    const __m512i oh_ov = _mm512_or_si512(_mm512_and_si512(oh, lo16), _mm512_slli_epi32(ov, 16));
    const __m512i th_tv = _mm512_or_si512(_mm512_and_si512(th, lo16), _mm512_slli_epi32(tv, 16));

    const __m512 o_mag_sq =
        _mm512_mul_ps(inv_4096, _mm512_cvtepi32_ps(_mm512_madd_epi16(oh_ov, oh_ov)));
    const __m512 ot_dp =
        _mm512_mul_ps(inv_4096, _mm512_cvtepi32_ps(_mm512_madd_epi16(oh_ov, th_tv)));
    const __m512 t_mag_sq =
        _mm512_mul_ps(inv_4096, _mm512_cvtepi32_ps(_mm512_madd_epi16(th_tv, th_tv)));

    const __mmask16 ge_0 = _mm512_cmp_ps_mask(ot_dp, _mm512_setzero_ps(), _CMP_GE_OS);
    const __mmask8 lo =
        decouple_angle_half_avx512(_mm512_castps512_ps256(ot_dp), _mm512_castps512_ps256(o_mag_sq),
                                   _mm512_castps512_ps256(t_mag_sq), cos_pd);
    const __mmask8 hi = decouple_angle_half_avx512(_mm512_extractf32x8_ps(ot_dp, 1),
                                                   _mm512_extractf32x8_ps(o_mag_sq, 1),
                                                   _mm512_extractf32x8_ps(t_mag_sq, 1), cos_pd);
    return _kand_mask16(ge_0, (__mmask16)(lo | ((__mmask16)hi << 8)));
}

/*
 * ADR-0502 — Approach B: software-prefetch the adm_div_lookup LUT entries of
 * the sixteen samples at `idx`, two blocks ahead of the one being decoupled,
 * into L2 (_MM_HINT_T1).
 *
 * Rationale: the LUT is 256 KB (65537 × int32) — too large for L1, fits in
 * L2 only partially. The 3 × vpgatherdd of a block issue 48 scatter reads
 * whose targets are scattered randomly across the 65 K-entry table, causing
 * frequent L2/L3 misses (66.5 % of function cycles per perf-profiler ADR-0502
 * finding). Prefetching 2 iterations ahead hides the miss latency behind the
 * ~300 cycles of arithmetic between the prefetch and the next gather, without
 * changing any computed value (pure access-strategy change; bit-exactness is
 * preserved).
 *
 * Approach A (vpermd + sequential loads) was ruled out because the DWT
 * coefficients oh/ov/od are arbitrary int16s in [-32768, 32767] with no
 * monotone ordering within a row — sequential-load substitution is not valid.
 */
static FORCE_INLINE void decouple_prefetch_avx512(const AdmBuffer *buf, ptrdiff_t idx,
                                                  const int32_t *lut)
{
    const int16_t *ph = buf->ref_dwt2.band_h + idx;
    const int16_t *pv = buf->ref_dwt2.band_v + idx;
    const int16_t *pd = buf->ref_dwt2.band_d + idx;
    for (int k = 0; k < 16; k++) {
        _mm_prefetch((const char *)&lut[(int32_t)ph[k] + 32768], _MM_HINT_T1);
        _mm_prefetch((const char *)&lut[(int32_t)pv[k] + 32768], _MM_HINT_T1);
        _mm_prefetch((const char *)&lut[(int32_t)pd[k] + 32768], _MM_HINT_T1);
    }
}

/* Reciprocal-table entries of sixteen denominators. */
static FORCE_INLINE __m512i decouple_div_avx512(__m512i o, const int32_t *lut)
{
    return _mm512_i32gather_epi32(_mm512_add_epi32(o, _mm512_set1_epi32(32768)), lut, 4);
}

/* Q15 ratio k = t / o of sixteen samples from their reciprocal-table entries
 * `div`, clamped to [0, 32768]. */
static FORCE_INLINE __m512i decouple_k_avx512(__m512i o, __m512i t, __m512i div)
{
    const __m512i const_32768 = _mm512_set1_epi32(32768);
    const __m512i const_16384 = _mm512_set1_epi64(16384);

    // Process Each element Pair for 64-bit multiplication
    __m512i lo = _mm512_mul_epi32(div, t);
    __m512i hi = _mm512_mul_epi32(_mm512_srli_epi64(div, 32), _mm512_srli_epi64(t, 32));
    lo = _mm512_srai_epi64(_mm512_add_epi64(lo, const_16384), 15);
    hi = _mm512_srai_epi64(_mm512_add_epi64(hi, const_16384), 15);

    __m512i k = _mm512_or_si512(_mm512_and_si512(lo, _mm512_set1_epi64(0xFFFFFFFF)),
                                _mm512_slli_epi64(hi, 32));
    // Handle division by zero
    const __mmask16 eqz = _mm512_cmp_epi32_mask(o, _mm512_setzero_si512(), _MM_CMPINT_EQ);
    k = _mm512_mask_blend_epi32(eqz, k, const_32768);
    // clamp to [0, 32768]
    k = _mm512_max_epi32(k, _mm512_setzero_si512());
    return _mm512_min_epi32(k, const_32768);
}

/* rst * gain of sixteen samples, in double precision, converted back as
 * _mm512_cvtpd_epi32 does. */
static FORCE_INLINE __m512i decouple_gain_avx512(__m512i rst, double gain)
{
    const __m512d g = _mm512_set1_pd(gain);
    const __m512d lo = _mm512_mul_pd(_mm512_cvtepi32_pd(_mm512_extracti32x8_epi32(rst, 0)), g);
    const __m512d hi = _mm512_mul_pd(_mm512_cvtepi32_pd(_mm512_extracti32x8_epi32(rst, 1)), g);
    return _mm512_inserti32x8(_mm512_castsi256_si512(_mm512_cvtpd_epi32(lo)),
                              _mm512_cvtpd_epi32(hi), 1);
}

/* Even int16 lanes gathered into the low half: sixteen int32 lanes narrowed
 * to sixteen int16. */
static const int16_t decouple_pack_idx[32] = {0,  2,  4,  6,  8,  10, 12, 14, 16, 18, 20,
                                              22, 24, 26, 28, 30, 0,  0,  0,  0,  0,  0,
                                              0,  0,  0,  0,  0,  0,  0,  0,  0,  0};

/* One band of sixteen samples: the restored signal and the additive
 * impairment, each packed to int16 in the low half of its vector
 * (adm_decouple_band()). */
static FORCE_INLINE void decouple_band_avx512(__m512i o, __m512i t, __m512i div,
                                              __mmask16 angle_mask, double gain, __m512i *r_out,
                                              __m512i *a_out)
{
    const __m512i k = decouple_k_avx512(o, t, div);
    // calculate rst values: ((kh * oh) + 16384) >> 15
    __m512i rst = _mm512_mullo_epi32(k, o);
    rst = _mm512_srai_epi32(_mm512_add_epi32(rst, _mm512_set1_epi32(16384)), 15);

    const __m512 k_f = _mm512_mul_ps(_mm512_set1_ps(1.0f / 32768.0f), _mm512_cvtepi32_ps(k));
    const __m512 o_f = _mm512_mul_ps(_mm512_set1_ps(1.0f / 64.0f), _mm512_cvtepi32_ps(o));
    const __m512 rst_f = _mm512_mul_ps(k_f, o_f);
    const __mmask16 gt0 = _mm512_cmp_ps_mask(rst_f, _mm512_setzero_ps(), _CMP_GT_OS);
    const __mmask16 lt0 = _mm512_cmp_ps_mask(rst_f, _mm512_setzero_ps(), _CMP_LT_OS);

    const __m512i rst_gain = decouple_gain_avx512(rst, gain);
    const __m512i v_min =
        _mm512_mask_blend_epi32(gt0, _mm512_setzero_epi32(), _mm512_min_epi32(rst_gain, t));
    const __m512i v_max =
        _mm512_mask_blend_epi32(lt0, _mm512_setzero_epi32(), _mm512_max_epi32(rst_gain, t));
    const __m512i min_max =
        _mm512_mask_blend_epi32(_kor_mask16(gt0, lt0), rst, _mm512_or_si512(v_min, v_max));
    rst = _mm512_mask_blend_epi32(angle_mask, rst, min_max);

    const __m512i perm = _mm512_loadu_si512((void const *)decouple_pack_idx);
    *r_out = _mm512_permutexvar_epi16(perm, rst);
    *a_out = _mm512_permutexvar_epi16(perm, _mm512_sub_epi32(t, rst));
}

/* Sixteen samples of all three bands, starting at `idx`. */
static FORCE_INLINE void decouple_block_avx512(const AdmBuffer *buf, ptrdiff_t idx,
                                               const int32_t *lut, double gain, float cos_1deg_sq)
{
    const adm_dwt_band_t *ref = &buf->ref_dwt2;
    const adm_dwt_band_t *dis = &buf->dis_dwt2;
    const adm_dwt_band_t *r = &buf->decouple_r;
    const adm_dwt_band_t *a = &buf->decouple_a;

    const __m512i oh = load_epi16x16(ref->band_h + idx);
    const __m512i ov = load_epi16x16(ref->band_v + idx);
    const __m512i od = load_epi16x16(ref->band_d + idx);
    const __m512i th = load_epi16x16(dis->band_h + idx);
    const __m512i tv = load_epi16x16(dis->band_v + idx);
    const __m512i td = load_epi16x16(dis->band_d + idx);

    const __mmask16 angle = decouple_angle_mask_avx512(oh, ov, th, tv, cos_1deg_sq);

    /* The three table lookups stay back to back, ahead of the per-band work. */
    const __m512i div_h = decouple_div_avx512(oh, lut);
    const __m512i div_v = decouple_div_avx512(ov, lut);
    const __m512i div_d = decouple_div_avx512(od, lut);

    __m512i rst[3];
    __m512i add[3];
    decouple_band_avx512(oh, th, div_h, angle, gain, &rst[0], &add[0]);
    decouple_band_avx512(ov, tv, div_v, angle, gain, &rst[1], &add[1]);
    decouple_band_avx512(od, td, div_d, angle, gain, &rst[2], &add[2]);

    _mm256_storeu_si256((__m256i *)(r->band_h + idx), _mm512_castsi512_si256(rst[0]));
    _mm256_storeu_si256((__m256i *)(r->band_v + idx), _mm512_castsi512_si256(rst[1]));
    _mm256_storeu_si256((__m256i *)(r->band_d + idx), _mm512_castsi512_si256(rst[2]));
    _mm256_storeu_si256((__m256i *)(a->band_h + idx), _mm512_castsi512_si256(add[0]));
    _mm256_storeu_si256((__m256i *)(a->band_v + idx), _mm512_castsi512_si256(add[1]));
    _mm256_storeu_si256((__m256i *)(a->band_d + idx), _mm512_castsi512_si256(add[2]));
}

/* `adm_div_lookup` keeps the mutable `int32_t *` of the dispatch signature it
 * shares with the scalar twin (integer_adm.c, ADR-1141). */
// NOLINTNEXTLINE(readability-non-const-parameter) — ADR-0141 / ADR-1141
void adm_decouple_avx512(AdmBuffer *buf, int w, int h, int stride, double adm_enhn_gain_limit,
                         int32_t *adm_div_lookup)
{
    const float cos_1deg_sq = adm_cos_1deg_sq();

    /* The computation of the score is not required for the regions
     * which lie outside the frame borders */
    const AdmBorder b = adm_border_filt(w, h);
    const int right_mod16 = b.right - ((b.right - b.left) % 16);

    for (int i = b.top; i < b.bottom; ++i) {
        for (int j = b.left; j < right_mod16; j += 16) {
            const ptrdiff_t idx = (ptrdiff_t)i * stride + j;
            if (j + 32 < right_mod16) {
                decouple_prefetch_avx512(buf, idx + 32, adm_div_lookup);
            }
            decouple_block_avx512(buf, idx, adm_div_lookup, adm_enhn_gain_limit, cos_1deg_sq);
        }
        adm_decouple_cols(buf, i, stride, right_mod16, b.right, adm_enhn_gain_limit, adm_div_lookup,
                          cos_1deg_sq);
    }
}

/* ------------------------------------------------------------------------- */
/* Decouple, scales 1..3                                                     */
/* ------------------------------------------------------------------------- */

/* angle_flag of eight samples held in int64 lanes. The predicate narrows its
 * int64 operands to float, which has no vector form here, so the lanes take a
 * scalar round trip. */
static FORCE_INLINE __mmask8 decouple_s123_angle_mask_avx512(__m512i oh, __m512i ov, __m512i th,
                                                             __m512i tv, float cos_1deg_sq)
{
    int64_t ot_dp[8];
    int64_t o_mag_sq[8];
    int64_t t_mag_sq[8];
    unsigned mask = 0;

    _mm512_storeu_si512((__m512i *)ot_dp,
                        _mm512_add_epi64(_mm512_mul_epi32(oh, th), _mm512_mul_epi32(ov, tv)));
    _mm512_storeu_si512((__m512i *)o_mag_sq,
                        _mm512_add_epi64(_mm512_mul_epi32(oh, oh), _mm512_mul_epi32(ov, ov)));
    _mm512_storeu_si512((__m512i *)t_mag_sq,
                        _mm512_add_epi64(_mm512_mul_epi32(th, th), _mm512_mul_epi32(tv, tv)));
    for (int k = 0; k < 8; ++k) {
        mask |= (unsigned)adm_angle_flag(ot_dp[k], o_mag_sq[k], t_mag_sq[k], cos_1deg_sq) << k;
    }
    return (__mmask8)mask;
}

/* What the table division of one band of sixteen samples needs: the table
 * entry, the sign of the denominator, the rounding term and the shift. */
typedef struct DecoupleS123Div {
    __m512i div;
    __m512i sign;
    __m512i round;
    __m512i count;
} DecoupleS123Div;

/* get_best15_from32() of sixteen magnitudes, with _mm512_lzcnt_epi32. A
 * magnitude below 32768 is its own 15-bit value, with a shift of 0. */
static FORCE_INLINE void decouple_s123_best15_avx512(__m512i abs_o, __m512i *msb, __m512i *shift)
{
    const __m512i const_1 = _mm512_set1_epi32(1);
    const __m512i k_shift = _mm512_sub_epi32(_mm512_set1_epi32(17), _mm512_lzcnt_epi32(abs_o));
    const __m512i round = _mm512_sllv_epi32(const_1, _mm512_sub_epi32(k_shift, const_1));
    const __m512i k_msb = _mm512_srlv_epi32(_mm512_add_epi32(abs_o, round), k_shift);
    const __mmask16 small = _mm512_cmp_epi32_mask(abs_o, _mm512_set1_epi32(32768), _MM_CMPINT_LT);

    *msb = _mm512_mask_blend_epi32(small, k_msb, abs_o);
    *shift = _mm512_mask_blend_epi32(small, k_shift, _mm512_setzero_si512());
}

/* The division terms of one band from its 15-bit denominators `msb` and
 * their shifts. */
static FORCE_INLINE DecoupleS123Div decouple_s123_div_avx512(__m512i o, __m512i msb, __m512i shift,
                                                             const int32_t *lut)
{
    const __m512i const_1 = _mm512_set1_epi32(1);
    DecoupleS123Div d;
    d.div = _mm512_i32gather_epi32(_mm512_add_epi32(msb, _mm512_set1_epi32(32768)), lut, 4);
    // -1 where the denominator is negative, 1 elsewhere, to match the C code.
    d.sign = _mm512_mask_blend_epi32(_mm512_cmplt_epi32_mask(o, _mm512_setzero_si512()), const_1,
                                     _mm512_set1_epi32(-1));
    d.round = _mm512_sllv_epi32(const_1, _mm512_add_epi32(_mm512_set1_epi32(14), shift));
    d.count = _mm512_add_epi32(_mm512_set1_epi32(15), shift);
    return d;
}

/* Table quotient k = t / o of eight samples in int64 lanes, clamped to
 * [0, 32768]. `div`, `sign`, `round` and `count` are the DecoupleS123Div
 * members of the same eight samples. */
static FORCE_INLINE __m512i decouple_s123_k_half_avx512(__m512i o, __m512i t, __m512i div,
                                                        __m512i sign, __m512i round, __m512i count)
{
    const __m512i zero = _mm512_setzero_si512();
    const __m512i const_32768 = _mm512_set1_epi64(32768);

    __m512i tmp_k = _mm512_mul_epi32(div, _mm512_mul_epi32(t, sign));
    tmp_k = _mm512_srav_epi64(_mm512_add_epi64(tmp_k, round), count);
    tmp_k = _mm512_mask_blend_epi64(_mm512_cmpeq_epi64_mask(o, zero), tmp_k, const_32768);

    const __m512i k =
        _mm512_mask_blend_epi64(_mm512_cmpgt_epi64_mask(tmp_k, const_32768), tmp_k, const_32768);
    return _mm512_mask_blend_epi64(_mm512_cmpgt_epi64_mask(zero, tmp_k), k, zero);
}

/* k of sixteen samples of one band, as two vectors of eight int64. */
static FORCE_INLINE void decouple_s123_k_avx512(__m512i o, __m512i t, const DecoupleS123Div *d,
                                                __m512i *k_lo, __m512i *k_hi)
{
    *k_lo = decouple_s123_k_half_avx512(widen_lo(o), widen_lo(t), widen_lo(d->div),
                                        widen_lo(d->sign), widen_lo(d->round), widen_lo(d->count));
    *k_hi = decouple_s123_k_half_avx512(widen_hi(o), widen_hi(t), widen_hi(d->div),
                                        widen_hi(d->sign), widen_hi(d->round), widen_hi(d->count));
}

/* Bound eight restored samples by the gain limit where the angle flag is set:
 * min(rst * gain, t) for a positive rst_f, max(rst * gain, t) for a negative
 * one. `rst_32` holds the same samples as int32. */
static FORCE_INLINE __m512i decouple_s123_limit_half_avx512(__m512i rst, __m256i rst_32, __m512i t,
                                                            __mmask8 angle_mask, __m256 rst_f,
                                                            double gain)
{
    const __m512d zero = _mm512_set1_pd(0.0);
    const __m512i rst_gain =
        _mm512_cvtpd_epi64(_mm512_mul_pd(_mm512_cvtepi32_pd(rst_32), _mm512_set1_pd(gain)));
    const __m512i v_min =
        _mm512_mask_blend_epi64(_mm512_cmpgt_epi64_mask(t, rst_gain), t, rst_gain);
    const __m512i v_max =
        _mm512_mask_blend_epi64(_mm512_cmpgt_epi64_mask(rst_gain, t), t, rst_gain);
    const __m512d rst_d = _mm512_cvtps_pd(rst_f);
    const __mmask8 gt = angle_mask & _mm512_cmp_pd_mask(rst_d, zero, _CMP_GT_OS);
    const __mmask8 lt = angle_mask & _mm512_cmp_pd_mask(rst_d, zero, _CMP_LT_OS);

    rst = _mm512_mask_blend_epi64(gt, rst, v_min);
    return _mm512_mask_blend_epi64(lt, rst, v_max);
}

/* The restored signal of sixteen samples of one band from their k
 * (adm_decouple_band_s123()). `angle_lo` and `angle_hi` are the angle masks of
 * the lower and upper eight samples. */
static FORCE_INLINE __m512i decouple_s123_rst_avx512(__m512i o, __m512i t, __m512i k_lo,
                                                     __m512i k_hi, __mmask8 angle_lo,
                                                     __mmask8 angle_hi, double gain)
{
    const __m512i const_16384 = _mm512_set1_epi64(16384);

    /* The shift is logical: only the low 32 bits of each lane are kept. */
    __m512i rst_lo =
        _mm512_srli_epi64(_mm512_add_epi64(_mm512_mul_epi32(k_lo, widen_lo(o)), const_16384), 15);
    __m512i rst_hi =
        _mm512_srli_epi64(_mm512_add_epi64(_mm512_mul_epi32(k_hi, widen_hi(o)), const_16384), 15);
    const __m512i rst_32 = narrow_epi64(rst_lo, rst_hi);

    const __m512 k_f = _mm512_insertf32x8(_mm512_castps256_ps512(_mm512_cvtepi64_ps(k_lo)),
                                          _mm512_cvtepi64_ps(k_hi), 1);
    const __m512 rst_f =
        _mm512_mul_ps(_mm512_mul_ps(k_f, _mm512_set1_ps((float)1 / 32768)),
                      _mm512_mul_ps(_mm512_cvtepi32_ps(o), _mm512_set1_ps((float)1 / 64)));

    rst_lo =
        decouple_s123_limit_half_avx512(rst_lo, _mm512_extracti32x8_epi32(rst_32, 0), widen_lo(t),
                                        angle_lo, _mm512_extractf32x8_ps(rst_f, 0), gain);
    rst_hi =
        decouple_s123_limit_half_avx512(rst_hi, _mm512_extracti32x8_epi32(rst_32, 1), widen_hi(t),
                                        angle_hi, _mm512_extractf32x8_ps(rst_f, 1), gain);
    return narrow_epi64(rst_lo, rst_hi);
}

/* Sixteen samples of all three bands, starting at `idx`. The bands advance
 * together, one step at a time, so that their dependency chains overlap. */
static FORCE_INLINE void decouple_s123_block_avx512(const AdmBuffer *buf, ptrdiff_t idx,
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
    __m512i o[3];
    __m512i t[3];
    __m512i msb[3];
    __m512i shift[3];
    DecoupleS123Div div[3];
    __m512i k_lo[3];
    __m512i k_hi[3];
    __m512i rst[3];

    for (int n = 0; n < 3; ++n) {
        o[n] = _mm512_loadu_si512((const __m512i *)(ref_bands[n] + idx));
        t[n] = _mm512_loadu_si512((const __m512i *)(dis_bands[n] + idx));
    }
    const __mmask8 angle_lo = decouple_s123_angle_mask_avx512(
        widen_lo(o[0]), widen_lo(o[1]), widen_lo(t[0]), widen_lo(t[1]), cos_1deg_sq);
    const __mmask8 angle_hi = decouple_s123_angle_mask_avx512(
        widen_hi(o[0]), widen_hi(o[1]), widen_hi(t[0]), widen_hi(t[1]), cos_1deg_sq);

    for (int n = 0; n < 3; ++n) {
        decouple_s123_best15_avx512(_mm512_abs_epi32(o[n]), &msb[n], &shift[n]);
    }
    for (int n = 0; n < 3; ++n) {
        div[n] = decouple_s123_div_avx512(o[n], msb[n], shift[n], lut);
    }
    for (int n = 0; n < 3; ++n) {
        decouple_s123_k_avx512(o[n], t[n], &div[n], &k_lo[n], &k_hi[n]);
    }
    for (int n = 0; n < 3; ++n) {
        rst[n] = decouple_s123_rst_avx512(o[n], t[n], k_lo[n], k_hi[n], angle_lo, angle_hi, gain);
    }
    for (int n = 0; n < 3; ++n) {
        _mm512_storeu_si512((__m512i *)(r_bands[n] + idx), rst[n]);
    }
    for (int n = 0; n < 3; ++n) {
        _mm512_storeu_si512((__m512i *)(a_bands[n] + idx), _mm512_sub_epi32(t[n], rst[n]));
    }
}

/* See adm_decouple_avx512() for the `adm_div_lookup` qualifier. */
// NOLINTNEXTLINE(readability-non-const-parameter) — ADR-0141 / ADR-1141
void adm_decouple_s123_avx512(AdmBuffer *buf, int w, int h, int stride, double adm_enhn_gain_limit,
                              int32_t *adm_div_lookup)
{
    const float cos_1deg_sq = adm_cos_1deg_sq();

    /* The computation of the score is not required for the regions
     * which lie outside the frame borders */
    const AdmBorder b = adm_border_filt(w, h);
    const int right_mod16 = b.right - ((b.right - b.left) % 16);

    for (int i = b.top; i < b.bottom; ++i) {
        for (int j = b.left; j < right_mod16; j += 16) {
            decouple_s123_block_avx512(buf, (ptrdiff_t)i * stride + j, adm_div_lookup,
                                       adm_enhn_gain_limit, cos_1deg_sq);
        }
        adm_decouple_s123_cols(buf, i, stride, right_mod16, b.right, adm_enhn_gain_limit,
                               adm_div_lookup, cos_1deg_sq);
    }
}

/* ------------------------------------------------------------------------- */
/* DWT, scale 0                                                              */
/* ------------------------------------------------------------------------- */

/* The low-pass and high-pass taps as int16 pairs for _mm512_madd_epi16. */
typedef struct Dwt2Filters {
    __m512i lo01;
    __m512i lo23;
    __m512i hi01;
    __m512i hi23;
} Dwt2Filters;

static FORCE_INLINE Dwt2Filters dwt2_filters_avx512(void)
{
    Dwt2Filters f;
    f.lo01 = _mm512_broadcastd_epi32(_mm_loadu_si128((const __m128i *)dwt2_db2_coeffs_lo));
    f.lo23 = _mm512_broadcastd_epi32(_mm_loadu_si128((const __m128i *)(dwt2_db2_coeffs_lo + 2)));
    f.hi01 = _mm512_broadcastd_epi32(_mm_loadu_si128((const __m128i *)dwt2_db2_coeffs_hi));
    f.hi23 = _mm512_broadcastd_epi32(_mm_loadu_si128((const __m128i *)(dwt2_db2_coeffs_hi + 2)));
    return f;
}

/* Thirty-two outputs of one filter of the 8-bit vertical pass. `s01` and
 * `s23` interleave the rows of taps 0, 1 and 2, 3; `sum_const` is the
 * coefficient sum that recentres the unsigned input. */
static FORCE_INLINE __m512i dwt2_8_vfilter_avx512(__m512i s01_lo, __m512i s01_hi, __m512i s23_lo,
                                                  __m512i s23_hi, __m512i f01, __m512i f23,
                                                  __m512i sum_const)
{
    const __m512i add_shift_VP = _mm512_set1_epi32(128);
    const __m512i pad = _mm512_setzero_si512();

    __m512i lo = _mm512_add_epi32(_mm512_madd_epi16(s01_lo, f01), _mm512_madd_epi16(s23_lo, f23));
    __m512i hi = _mm512_add_epi32(_mm512_madd_epi16(s01_hi, f01), _mm512_madd_epi16(s23_hi, f23));
    lo = _mm512_sub_epi32(lo, sum_const);
    hi = _mm512_sub_epi32(hi, sum_const);
    lo = _mm512_srli_epi32(_mm512_add_epi32(lo, add_shift_VP), 0x08);
    hi = _mm512_srli_epi32(_mm512_add_epi32(hi, add_shift_VP), 0x08);
    lo = _mm512_mask_blend_epi16(0xAAAAAAAA, lo, pad);
    hi = _mm512_mask_blend_epi16(0xAAAAAAAA, hi, pad);
    return _mm512_packus_epi32(lo, hi);
}

/* Vertical pass of thirty-two columns of output row `i`, starting at `j`. */
static FORCE_INLINE void dwt2_8_vpass_block_avx512(const uint8_t *src, int *const *ind_y, int i,
                                                   int src_stride, int j, const Dwt2Filters *f,
                                                   int16_t *tmplo, int16_t *tmphi)
{
    const __m512i lo_sum = _mm512_set1_epi32(dwt2_db2_coeffs_lo_sum * 128);
    const __m512i hi_sum = _mm512_set1_epi32(dwt2_db2_coeffs_hi_sum * 128);
    const __m512i s0 = _mm512_cvtepu8_epi16(
        _mm256_loadu_si256((const __m256i *)(src + ((ptrdiff_t)ind_y[0][i] * src_stride) + j)));
    const __m512i s1 = _mm512_cvtepu8_epi16(
        _mm256_loadu_si256((const __m256i *)(src + ((ptrdiff_t)ind_y[1][i] * src_stride) + j)));
    const __m512i s2 = _mm512_cvtepu8_epi16(
        _mm256_loadu_si256((const __m256i *)(src + ((ptrdiff_t)ind_y[2][i] * src_stride) + j)));
    const __m512i s3 = _mm512_cvtepu8_epi16(
        _mm256_loadu_si256((const __m256i *)(src + ((ptrdiff_t)ind_y[3][i] * src_stride) + j)));

    const __m512i s01_lo = _mm512_unpacklo_epi16(s0, s1);
    const __m512i s01_hi = _mm512_unpackhi_epi16(s0, s1);
    const __m512i s23_lo = _mm512_unpacklo_epi16(s2, s3);
    const __m512i s23_hi = _mm512_unpackhi_epi16(s2, s3);

    _mm512_storeu_si512(
        (__m512i *)(tmplo + j),
        dwt2_8_vfilter_avx512(s01_lo, s01_hi, s23_lo, s23_hi, f->lo01, f->lo23, lo_sum));
    _mm512_storeu_si512(
        (__m512i *)(tmphi + j),
        dwt2_8_vfilter_avx512(s01_lo, s01_hi, s23_lo, s23_hi, f->hi01, f->hi23, hi_sum));
}

/* Thirty-two outputs of one filter of the horizontal pass. `s0` / `s2` hold
 * the samples of taps 0, 1 and 2, 3 of the first sixteen outputs, `s0_next` /
 * `s2_next` those of the last sixteen. */
static FORCE_INLINE __m512i dwt2_hfilter_avx512(__m512i s0, __m512i s2, __m512i s0_next,
                                                __m512i s2_next, __m512i f01, __m512i f23)
{
    const __m512i add_shift_HP = _mm512_set1_epi32(32768);

    __m512i lo = _mm512_add_epi32(_mm512_madd_epi16(s0, f01), _mm512_madd_epi16(s2, f23));
    __m512i hi = _mm512_add_epi32(_mm512_madd_epi16(s0_next, f01), _mm512_madd_epi16(s2_next, f23));
    lo = _mm512_srai_epi32(_mm512_add_epi32(lo, add_shift_HP), 16);
    hi = _mm512_srai_epi32(_mm512_add_epi32(hi, add_shift_HP), 16);
    return _mm512_inserti64x4(_mm512_castsi256_si512(_mm512_cvtepi32_epi16(lo)),
                              _mm512_cvtepi32_epi16(hi), 1);
}

/* Horizontal pass of thirty-two outputs from one row buffer: its low-pass
 * into `dst_lo`, its high-pass into `dst_hi`. */
static FORCE_INLINE void dwt2_hpass_block_avx512(const int16_t *tmp, int *const *ind_x, int j,
                                                 const Dwt2Filters *f, int16_t *dst_lo,
                                                 int16_t *dst_hi)
{
    const __m512i s0 = _mm512_loadu_si512((const __m512i *)(tmp + ind_x[0][j]));
    const __m512i s2 = _mm512_loadu_si512((const __m512i *)(tmp + ind_x[2][j]));
    const __m512i s0_next = _mm512_loadu_si512((const __m512i *)(tmp + 32 + ind_x[0][j]));
    const __m512i s2_next = _mm512_loadu_si512((const __m512i *)(tmp + 32 + ind_x[2][j]));

    _mm512_storeu_si512((__m512i *)dst_lo,
                        dwt2_hfilter_avx512(s0, s2, s0_next, s2_next, f->lo01, f->lo23));
    _mm512_storeu_si512((__m512i *)dst_hi,
                        dwt2_hfilter_avx512(s0, s2, s0_next, s2_next, f->hi01, f->hi23));
}

/* Horizontal pass of output row `i`: the first column and the tail in scalar
 * code, thirty-two outputs at a time in between. The vector loop ends at the
 * last multiple of `tail_mod` outputs after the first column; the 8-bit DWT
 * passes 32 and the 16-bit DWT, as upstream, 64. */
static FORCE_INLINE void dwt2_hpass_row_avx512(const int16_t *tmplo, const int16_t *tmphi,
                                               const adm_dwt_band_t *dst, int *const *ind_x, int i,
                                               int w, int dst_stride, int tail_mod,
                                               const Dwt2Filters *f)
{
    const ptrdiff_t row = (ptrdiff_t)i * dst_stride;
    const int half_w = (w + 1) / 2;
    const int half_w_mod = half_w >= 2 ? half_w - 1 - ((half_w - 2) % tail_mod) : 1;

    adm_dwt2_hpass(tmplo, tmphi, dst, ind_x, i, 0, 1, dst_stride);
    for (int j = 1; j < half_w_mod; j += 32) {
        dwt2_hpass_block_avx512(tmplo, ind_x, j, f, dst->band_a + row + j, dst->band_v + row + j);
        dwt2_hpass_block_avx512(tmphi, ind_x, j, f, dst->band_h + row + j, dst->band_d + row + j);
    }
    adm_dwt2_hpass(tmplo, tmphi, dst, ind_x, i, half_w_mod, half_w, dst_stride);
}

void adm_dwt2_8_avx512(const uint8_t *src, const adm_dwt_band_t *dst, AdmBuffer *buf, int w, int h,
                       int src_stride, int dst_stride)
{
    int **ind_y = buf->ind_y;
    int **ind_x = buf->ind_x;
    int16_t *tmplo = (int16_t *)buf->tmp_ref;
    int16_t *tmphi = tmplo + w;
    const Dwt2Filters f = dwt2_filters_avx512();
    const int w_mod_32 = (w >> 5) << 5;

    for (int i = 0; i < (h + 1) / 2; ++i) {
        /* Vertical pass. */
        for (int j = 0; j < w_mod_32; j += 32) {
            dwt2_8_vpass_block_avx512(src, ind_y, i, src_stride, j, &f, tmplo, tmphi);
        }
        adm_dwt2_vpass_8(src, ind_y, i, src_stride, w_mod_32, w, tmplo, tmphi);
        dwt2_hpass_row_avx512(tmplo, tmphi, dst, ind_x, i, w, dst_stride, 32, &f);
    }
}

void adm_dwt2_16_avx512(const uint16_t *src, const adm_dwt_band_t *dst, AdmBuffer *buf, int w,
                        int h, int src_stride, int dst_stride, int inp_size_bits)
{
    int **ind_y = buf->ind_y;
    int **ind_x = buf->ind_x;
    int16_t *tmplo = (int16_t *)buf->tmp_ref;
    int16_t *tmphi = tmplo + w;
    const Dwt2Filters f = dwt2_filters_avx512();

    for (int i = 0; i < (h + 1) / 2; ++i) {
        /* The vertical pass forms its response in int64 (a bright 16-bit
         * column overflows int32) and stays scalar. */
        adm_dwt2_vpass_16(src, ind_y, i, src_stride, 0, w, inp_size_bits, tmplo, tmphi);
        dwt2_hpass_row_avx512(tmplo, tmphi, dst, ind_x, i, w, dst_stride, 64, &f);
    }
}

/* ------------------------------------------------------------------------- */
/* DWT, scales 1..3                                                          */
/* ------------------------------------------------------------------------- */

typedef struct I4Dwt2Consts {
    __m512i f_lo[4];
    __m512i f_hi[4];
    __m512i add_vp;
    __m512i add_hp;
    unsigned shift_vp;
    unsigned shift_hp;
} I4Dwt2Consts;

static FORCE_INLINE I4Dwt2Consts i4_dwt2_consts_avx512(const I4Dwt2Round *r)
{
    I4Dwt2Consts k;
    for (int t = 0; t < 4; ++t) {
        k.f_lo[t] = _mm512_set1_epi64(dwt2_db2_coeffs_lo[t]);
        k.f_hi[t] = _mm512_set1_epi64(dwt2_db2_coeffs_hi[t]);
    }
    k.add_vp = _mm512_set1_epi64(r->add_vp);
    k.add_hp = _mm512_set1_epi64(r->add_hp);
    k.shift_vp = (unsigned)r->shift_vp;
    k.shift_hp = (unsigned)r->shift_hp;
    return k;
}

/* Four-tap response of eight samples held in the even int32 lanes. */
static FORCE_INLINE __m512i i4_dwt2_taps_avx512(__m512i s0, __m512i s1, __m512i s2, __m512i s3,
                                                const __m512i f[4])
{
    __m512i accum = _mm512_add_epi64(_mm512_mul_epi32(s0, f[0]), _mm512_mul_epi32(s1, f[1]));
    accum = _mm512_add_epi64(accum, _mm512_mul_epi32(s2, f[2]));
    return _mm512_add_epi64(accum, _mm512_mul_epi32(s3, f[3]));
}

/* (accum + add) >> shift of eight int64 lanes, narrowed to eight int32. */
static FORCE_INLINE __m256i i4_dwt2_round_avx512(__m512i accum, __m512i add, unsigned shift)
{
    return _mm512_cvtepi64_epi32(_mm512_srai_epi64(_mm512_add_epi64(accum, add), shift));
}

/* Vertical pass of eight columns of one plane of output row `i`. */
static FORCE_INLINE void i4_dwt2_vpass_block_avx512(const int32_t *src, int *const *ind_y, int i,
                                                    int stride, int j, const I4Dwt2Consts *k,
                                                    int32_t *tmplo, int32_t *tmphi)
{
    const __m512i s0 = load_epi32x8(src + ((ptrdiff_t)ind_y[0][i] * stride) + j);
    const __m512i s1 = load_epi32x8(src + ((ptrdiff_t)ind_y[1][i] * stride) + j);
    const __m512i s2 = load_epi32x8(src + ((ptrdiff_t)ind_y[2][i] * stride) + j);
    const __m512i s3 = load_epi32x8(src + ((ptrdiff_t)ind_y[3][i] * stride) + j);

    _mm256_storeu_si256(
        (__m256i *)(tmplo + j),
        i4_dwt2_round_avx512(i4_dwt2_taps_avx512(s0, s1, s2, s3, k->f_lo), k->add_vp, k->shift_vp));
    _mm256_storeu_si256(
        (__m256i *)(tmphi + j),
        i4_dwt2_round_avx512(i4_dwt2_taps_avx512(s0, s1, s2, s3, k->f_hi), k->add_vp, k->shift_vp));
}

/* Horizontal pass of eight outputs from one row buffer: its low-pass into
 * `dst_lo`, its high-pass into `dst_hi`. */
static FORCE_INLINE void i4_dwt2_hpass_block_avx512(const int32_t *tmp, const int jx[4],
                                                    const I4Dwt2Consts *k, int32_t *dst_lo,
                                                    int32_t *dst_hi)
{
    const __m512i s0 = _mm512_loadu_si512((const __m512i *)(tmp + jx[0]));
    const __m512i s1 = _mm512_loadu_si512((const __m512i *)(tmp + jx[1]));
    const __m512i s2 = _mm512_loadu_si512((const __m512i *)(tmp + jx[2]));
    const __m512i s3 = _mm512_loadu_si512((const __m512i *)(tmp + jx[3]));

    _mm256_storeu_si256(
        (__m256i *)dst_lo,
        i4_dwt2_round_avx512(i4_dwt2_taps_avx512(s0, s1, s2, s3, k->f_lo), k->add_hp, k->shift_hp));
    _mm256_storeu_si256(
        (__m256i *)dst_hi,
        i4_dwt2_round_avx512(i4_dwt2_taps_avx512(s0, s1, s2, s3, k->f_hi), k->add_hp, k->shift_hp));
}

/* Horizontal pass of eight outputs of both planes of output row `i`. */
static FORCE_INLINE void i4_dwt2_hpass_planes_avx512(const int32_t *tmp, const AdmBuffer *buf,
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

    i4_dwt2_hpass_block_avx512(tmplo_ref, jx, k, ref->band_a + out, ref->band_v + out);
    i4_dwt2_hpass_block_avx512(tmphi_ref, jx, k, ref->band_h + out, ref->band_d + out);
    i4_dwt2_hpass_block_avx512(tmplo_dis, jx, k, dis->band_a + out, dis->band_v + out);
    i4_dwt2_hpass_block_avx512(tmphi_dis, jx, k, dis->band_h + out, dis->band_d + out);
}

/**
 * AVX-512 combined ref+distorted ADM DWT for scales 1..3 (32-bit pipe).
 *
 * Scale 0 does NOT route through this function — the caller
 * (`integer_compute_adm`) uses `s->dwt2_8` / `s->dwt2_16` for scale 0
 * because those read the source picture (8/16-bit), while this function
 * reads prior 32-bit DWT output via `i4_ref_scale` / `i4_curr_dis`.
 */
void adm_dwt2_s123_combined_avx512(const int32_t *i4_ref_scale, const int32_t *i4_curr_dis,
                                   AdmBuffer *buf, int w, int h, int ref_stride, int dis_stride,
                                   int dst_stride, int scale)
{
    const I4Dwt2Round r = i4_dwt2_round(scale);
    const I4Dwt2Consts k = i4_dwt2_consts_avx512(&r);
    int **ind_y = buf->ind_y;
    int **ind_x = buf->ind_x;
    int32_t *tmp = buf->tmp_ref;
    int32_t *tmplo_dis = tmp + ((ptrdiff_t)2 * w);

    const int w_mod8 = (w - (w % 8));
    const int half_w = (w + 1) / 2;
    const int half_w_mod8 = half_w >= 2 ? half_w - 1 - ((half_w - 2) % 8) : 1;

    for (int i = 0; i < (h + 1) / 2; ++i) {
        /* Vertical pass. */
        for (int j = 0; j < w_mod8; j += 8) {
            i4_dwt2_vpass_block_avx512(i4_ref_scale, ind_y, i, ref_stride, j, &k, tmp, tmp + w);
            i4_dwt2_vpass_block_avx512(i4_curr_dis, ind_y, i, dis_stride, j, &k, tmplo_dis,
                                       tmplo_dis + w);
        }
        i4_dwt2_vpass(i4_ref_scale, i4_curr_dis, ind_y, i, ref_stride, dis_stride, w, w_mod8, w,
                      tmp, r.add_vp, r.shift_vp);

        /* Horizontal pass (lo and hi). */
        i4_dwt2_hpass(tmp, &buf->i4_ref_dwt2, &buf->i4_dis_dwt2, ind_x, i, w, 0, 1, dst_stride,
                      r.add_hp, r.shift_hp);
        for (int j = 1; j < half_w_mod8; j += 8) {
            i4_dwt2_hpass_planes_avx512(tmp, buf, ind_x, w, (ptrdiff_t)i * dst_stride + j, j, &k);
        }
        i4_dwt2_hpass(tmp, &buf->i4_ref_dwt2, &buf->i4_dis_dwt2, ind_x, i, w, half_w_mod8, half_w,
                      dst_stride, r.add_hp, r.shift_hp);
    }
}

/* ------------------------------------------------------------------------- */
/* Contrast sensitivity filtering                                            */
/* ------------------------------------------------------------------------- */

/* Sixteen samples of one scale-0 band (adm_csf_cols()). */
static FORCE_INLINE void csf_block_avx512(const int16_t *src, int16_t *dst, int16_t *flt,
                                          __m512i i_rfactor, __m512i shiftadd, unsigned shift)
{
    const __m512i dst_val = _mm512_mullo_epi32(load_epi16x16(src), i_rfactor);
    const __m512i i16_dst_val = _mm512_srai_epi32(_mm512_add_epi32(dst_val, shiftadd), shift);
    _mm256_storeu_si256((__m256i *)dst, _mm512_cvtepi32_epi16(i16_dst_val));

    __m512i f =
        _mm512_mullo_epi32(_mm512_set1_epi32(ADM_FIX_ONE_BY_30), _mm512_abs_epi32(i16_dst_val));
    f = _mm512_srli_epi32(_mm512_add_epi32(f, _mm512_set1_epi32(2048)), 12);
    _mm256_storeu_si256((__m256i *)flt, _mm512_cvtepi32_epi16(f));
}

/* Columns [j0, j1) of the three bands of row `offset`, sixteen at a time. The
 * pointers and constants are read once here: the stores of the loop may alias
 * anything, so the compiler would otherwise reload them for every block. */
static FORCE_INLINE void csf_row_avx512(const AdmCsfBands *b, const uint16_t i_rfactor[3],
                                        ptrdiff_t offset, int j0, int j1)
{
    const int16_t *const src[3] = {b->src[0] + offset, b->src[1] + offset, b->src[2] + offset};
    int16_t *const dst[3] = {b->dst[0] + offset, b->dst[1] + offset, b->dst[2] + offset};
    int16_t *const flt[3] = {b->flt[0] + offset, b->flt[1] + offset, b->flt[2] + offset};
    const __m512i rfactor[3] = {_mm512_set1_epi32(i_rfactor[0]), _mm512_set1_epi32(i_rfactor[1]),
                                _mm512_set1_epi32(i_rfactor[2])};
    const __m512i shiftadd[3] = {_mm512_set1_epi32(adm_csf_shiftsadd[0]),
                                 _mm512_set1_epi32(adm_csf_shiftsadd[1]),
                                 _mm512_set1_epi32(adm_csf_shiftsadd[2])};

    for (int j = j0; j < j1; j += 16) {
        for (int theta = 0; theta < 3; ++theta) {
            csf_block_avx512(src[theta] + j, dst[theta] + j, flt[theta] + j, rfactor[theta],
                             shiftadd[theta], adm_csf_shifts[theta]);
        }
    }
}

void adm_csf_avx512(AdmBuffer *buf, int w, int h, int stride, double adm_norm_view_dist,
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
    const int right_mod_16 = b.right - ((b.right - b.left) % 16);

    for (int i = b.top; i < b.bottom; ++i) {
        const ptrdiff_t offset = (ptrdiff_t)i * stride;

        csf_row_avx512(&bands, i_rfactor, offset, b.left, right_mod_16);
        for (int theta = 0; theta < 3; ++theta) {
            adm_csf_cols(&bands, i_rfactor, theta, offset, right_mod_16, b.right);
        }
    }
}

/* Sixteen samples of one band of scales 1..3 (i4_adm_csf_cols()). */
static FORCE_INLINE void i4_csf_block_avx512(const int32_t *src, int32_t *dst, int32_t *flt,
                                             __m512i r_factor, __m512i add_dst, __m512i add_flt,
                                             unsigned shift_dst, unsigned shift_flt)
{
    const __m512i merge =
        _mm512_setr_epi32(0, 16, 2, 18, 4, 20, 6, 22, 8, 24, 10, 26, 12, 28, 14, 30);
    const __m512i one_by_30 = _mm512_set1_epi32((int)I4_ADM_FIX_ONE_BY_30);
    const __m512i s = _mm512_loadu_si512((const __m512i *)src);

    __m512i dst_lo = _mm512_mul_epi32(s, r_factor);
    __m512i dst_hi = _mm512_mul_epi32(_mm512_srli_epi64(s, 32), r_factor);
    dst_lo = _mm512_srai_epi64(_mm512_add_epi64(dst_lo, add_dst), shift_dst);
    dst_hi = _mm512_srai_epi64(_mm512_add_epi64(dst_hi, add_dst), shift_dst);
    dst_lo = _mm512_permutex2var_epi32(dst_lo, merge, dst_hi);
    _mm512_storeu_si512((__m512i *)dst, dst_lo);

    const __m512i abs_dst = _mm512_abs_epi32(dst_lo);
    __m512i flt_lo = _mm512_mul_epi32(one_by_30, abs_dst);
    __m512i flt_hi = _mm512_mul_epi32(one_by_30, _mm512_srli_epi64(abs_dst, 32));
    flt_lo = _mm512_srli_epi64(_mm512_add_epi64(flt_lo, add_flt), shift_flt);
    flt_hi = _mm512_srli_epi64(_mm512_add_epi64(flt_hi, add_flt), shift_flt);
    flt_lo = _mm512_permutex2var_epi32(flt_lo, merge, flt_hi);
    _mm512_storeu_si512((__m512i *)flt, flt_lo);
}

/* Columns [j0, j1) of the three bands of row `offset`, sixteen at a time; see
 * csf_row_avx512() for why the context is read once up front. */
static FORCE_INLINE void i4_csf_row_avx512(const I4AdmCsfCtx *c, ptrdiff_t offset, int j0, int j1)
{
    const int32_t *const src[3] = {c->src[0] + offset, c->src[1] + offset, c->src[2] + offset};
    int32_t *const dst[3] = {c->dst[0] + offset, c->dst[1] + offset, c->dst[2] + offset};
    int32_t *const flt[3] = {c->flt[0] + offset, c->flt[1] + offset, c->flt[2] + offset};
    const __m512i r_factor[3] = {_mm512_set1_epi32((int)c->i_rfactor[0]),
                                 _mm512_set1_epi32((int)c->i_rfactor[1]),
                                 _mm512_set1_epi32((int)c->i_rfactor[2])};
    const __m512i add_dst = _mm512_set1_epi64(c->add_bef_shift_dst);
    const __m512i add_flt = _mm512_set1_epi64(c->add_bef_shift_flt);
    const unsigned shift_dst = c->shift_dst;
    const unsigned shift_flt = c->shift_flt;

    for (int j = j0; j < j1; j += 16) {
        for (int theta = 0; theta < 3; ++theta) {
            i4_csf_block_avx512(src[theta] + j, dst[theta] + j, flt[theta] + j, r_factor[theta],
                                add_dst, add_flt, shift_dst, shift_flt);
        }
    }
}

void i4_adm_csf_avx512(AdmBuffer *buf, int scale, int w, int h, int stride,
                       double adm_norm_view_dist, int adm_ref_display_height, int adm_csf_mode,
                       double adm_csf_scale, double adm_csf_diag_scale, bool measure_aim)
{
    I4AdmCsfCtx c;
    i4_adm_csf_ctx_init(&c, buf, scale, adm_norm_view_dist, adm_ref_display_height, adm_csf_mode,
                        adm_csf_scale, adm_csf_diag_scale, measure_aim);

    /* The computation of the csf values is not required for the regions
     * which lie outside the frame borders */
    const AdmBorder b = adm_border_filt(w, h);
    const int right_mod_16 = b.right - ((b.right - b.left) % 16);

    for (int i = b.top; i < b.bottom; ++i) {
        const ptrdiff_t offset = (ptrdiff_t)i * stride;

        i4_csf_row_avx512(&c, offset, b.left, right_mod_16);
        for (int theta = 0; theta < 3; ++theta) {
            i4_adm_csf_cols(&c, theta, offset, right_mod_16, b.right);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Denominator (reference-energy) reductions                                 */
/* ------------------------------------------------------------------------- */

/* Cubed magnitudes of sixteen samples of one scale-0 band, added to two
 * accumulators of eight uint64 lanes. */
static FORCE_INLINE void csf_den_cube_avx512(const int16_t *src, __m512i *accum_lo,
                                             __m512i *accum_hi)
{
    const __m512i v =
        _mm512_cvtepu16_epi32(_mm256_abs_epi16(_mm256_loadu_si256((const __m256i *)src)));
    const __m512i sq = _mm512_mullo_epi32(v, v);
    *accum_lo = _mm512_add_epi64(*accum_lo, _mm512_mul_epu32(sq, v));
    *accum_hi = _mm512_add_epi64(
        *accum_hi, _mm512_mul_epu32(_mm512_srli_epi64(sq, 32), _mm512_srli_epi64(v, 32)));
}

/* Sums of the cubed magnitudes of columns [j0, j1) of the three bands of one
 * row, sixteen at a time. */
static FORCE_INLINE void csf_den_row_avx512(const int16_t *src_h, const int16_t *src_v,
                                            const int16_t *src_d, int j0, int j1, uint64_t inner[3])
{
    __m512i accum_lo[3] = {_mm512_setzero_si512(), _mm512_setzero_si512(), _mm512_setzero_si512()};
    __m512i accum_hi[3] = {_mm512_setzero_si512(), _mm512_setzero_si512(), _mm512_setzero_si512()};

    for (int j = j0; j < j1; j += 16) {
        csf_den_cube_avx512(src_h + j, &accum_lo[0], &accum_hi[0]);
        csf_den_cube_avx512(src_v + j, &accum_lo[1], &accum_hi[1]);
        csf_den_cube_avx512(src_d + j, &accum_lo[2], &accum_hi[2]);
    }
    for (int k = 0; k < 3; ++k) {
        inner[k] = hsum_epu64(_mm512_add_epi64(accum_lo[k], accum_hi[k]));
    }
}

float adm_csf_den_scale_avx512(const adm_dwt_band_t *src, int w, int h, int src_stride,
                               double adm_norm_view_dist, int adm_ref_display_height,
                               int adm_csf_mode, double adm_csf_scale, double adm_csf_diag_scale,
                               double adm_noise_weight)
{
    AdmDenCtx c;
    adm_csf_den_ctx_init(&c, w, h, adm_norm_view_dist, adm_ref_display_height, adm_csf_mode,
                         adm_csf_scale, adm_csf_diag_scale);
    const int right_mod_16 = c.b.right - ((c.b.right - c.b.left) % 16);

    uint64_t accum[3] = {0, 0, 0};
    uint64_t inner[3] = {0, 0, 0};

    const int16_t *src_h = src->band_h + (ptrdiff_t)c.b.top * src_stride;
    const int16_t *src_v = src->band_v + (ptrdiff_t)c.b.top * src_stride;
    const int16_t *src_d = src->band_d + (ptrdiff_t)c.b.top * src_stride;
    for (int i = c.b.top; i < c.b.bottom; ++i) {
        csf_den_row_avx512(src_h, src_v, src_d, c.b.left, right_mod_16, inner);
        adm_csf_den_cols(src_h, src_v, src_d, right_mod_16, c.b.right, inner);
        adm_csf_den_fold(inner, accum, (uint32_t)c.add_shift_accum, (uint32_t)c.shift_accum);
        src_h += src_stride;
        src_v += src_stride;
        src_d += src_stride;
    }
    return adm_csf_den_result(&c, accum, adm_noise_weight);
}

/* Cube terms of eight samples of one band of scales 1..3 (i4_cube_term()). */
static FORCE_INLINE __m512i i4_csf_den_cube_avx512(const int32_t *src, __m512i add_sq,
                                                   __m512i add_cub, unsigned shift_sq,
                                                   unsigned shift_cub)
{
    const __m512i v =
        _mm512_cvtepu32_epi64(_mm256_abs_epi32(_mm256_loadu_si256((const __m256i *)src)));
    __m512i sq = _mm512_add_epi64(_mm512_mul_epu32(v, v), add_sq);
    sq = _mm512_srli_epi64(sq, shift_sq);
    const __m512i cu = _mm512_add_epi64(_mm512_mullo_epi64(sq, v), add_cub);
    return _mm512_srli_epi64(cu, shift_cub);
}

/* Sums of the cube terms of columns [j0, j1) of the three bands of one row,
 * eight at a time. */
static FORCE_INLINE void i4_csf_den_row_avx512(const I4AdmDenCtx *c, const int32_t *src_h,
                                               const int32_t *src_v, const int32_t *src_d, int j0,
                                               int j1, uint64_t inner[3])
{
    const __m512i add_sq = _mm512_set1_epi64(c->add_shift_sq);
    const __m512i add_cub = _mm512_set1_epi64(c->add_shift_cub);
    const unsigned shift_sq = c->shift_sq;
    const unsigned shift_cub = c->shift_cub;
    __m512i accum[3] = {_mm512_setzero_si512(), _mm512_setzero_si512(), _mm512_setzero_si512()};

    for (int j = j0; j < j1; j += 8) {
        accum[0] = _mm512_add_epi64(
            accum[0], i4_csf_den_cube_avx512(src_h + j, add_sq, add_cub, shift_sq, shift_cub));
        accum[1] = _mm512_add_epi64(
            accum[1], i4_csf_den_cube_avx512(src_v + j, add_sq, add_cub, shift_sq, shift_cub));
        accum[2] = _mm512_add_epi64(
            accum[2], i4_csf_den_cube_avx512(src_d + j, add_sq, add_cub, shift_sq, shift_cub));
    }
    for (int k = 0; k < 3; ++k) {
        inner[k] = hsum_epu64(accum[k]);
    }
}

float adm_csf_den_s123_avx512(const i4_adm_dwt_band_t *src, int scale, int w, int h, int src_stride,
                              double adm_norm_view_dist, int adm_ref_display_height,
                              int adm_csf_mode, double adm_csf_scale, double adm_csf_diag_scale,
                              double adm_noise_weight)
{
    I4AdmDenCtx c;
    i4_adm_csf_den_ctx_init(&c, scale, w, h, adm_norm_view_dist, adm_ref_display_height,
                            adm_csf_mode, adm_csf_scale, adm_csf_diag_scale);
    const int right_mod_8 = c.b.right - ((c.b.right - c.b.left) % 8);

    uint64_t accum[3] = {0, 0, 0};
    uint64_t inner[3] = {0, 0, 0};

    const int32_t *src_h = src->band_h + (ptrdiff_t)c.b.top * src_stride;
    const int32_t *src_v = src->band_v + (ptrdiff_t)c.b.top * src_stride;
    const int32_t *src_d = src->band_d + (ptrdiff_t)c.b.top * src_stride;
    for (int i = c.b.top; i < c.b.bottom; ++i) {
        i4_csf_den_row_avx512(&c, src_h, src_v, src_d, c.b.left, right_mod_8, inner);
        i4_adm_csf_den_cols(&c, src_h, src_v, src_d, right_mod_8, c.b.right, inner);
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

/* One band's share of the masking threshold of the fourteen columns starting
 * at `j` of an interior row `i`: the 3x3 sum of the filtered band, its centre
 * replaced by the unfiltered sample scaled by 1/15 (adm_cm_thresh()). Lanes
 * 14 and 15 are not complete sums. */
static FORCE_INLINE __m512i cm_thresh_band_avx512(const int16_t *src, const int16_t *flt,
                                                  int stride, int i, int j)
{
    const __m512i perm1 = _mm512_set_epi32(0, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1);
    const __m512i perm2 = _mm512_set_epi32(1, 0, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2);
    const int16_t *flt_row = flt + ((ptrdiff_t)stride * (i - 1)) + j - 1;
    const __m512i flt0 = load_epi16x16(flt_row);
    const __m512i flt1 = load_epi16x16(flt_row + stride);
    const __m512i flt2 = load_epi16x16(flt_row + 2 * (ptrdiff_t)stride);

    __m512i centre = load_epi16x16(src + ((ptrdiff_t)stride * i) + j - 1);
    centre = _mm512_mullo_epi32(_mm512_abs_epi32(centre), _mm512_set1_epi32(ONE_BY_15));
    centre = _mm512_srai_epi32(_mm512_add_epi32(centre, _mm512_set1_epi32(2048)), 12);
    /* Wrap each tap to int16, as adm_cm_thresh()'s (int16_t) cast does: */
    centre = _mm512_srai_epi32(_mm512_slli_epi32(centre, 16), 16);
    centre = _mm512_sub_epi32(centre, flt1);

    const __m512i rows = _mm512_add_epi32(_mm512_add_epi32(flt0, flt1), flt2);
    __m512i sum = _mm512_add_epi32(_mm512_permutexvar_epi32(perm1, rows), rows);
    sum = _mm512_add_epi32(_mm512_permutexvar_epi32(perm2, rows), sum);
    return _mm512_add_epi32(sum, _mm512_permutexvar_epi32(perm1, centre));
}

/* Masking threshold of the fourteen columns starting at `j`; lanes 14 and 15
 * are 0. */
static FORCE_INLINE __m512i cm_thresh_avx512(const AdmCmCtx *c, int i, int j)
{
    const __m512i mask_end = _mm512_set_epi64(0x0LL, -1LL, -1LL, -1LL, -1LL, -1LL, -1LL, -1LL);
    __m512i sum = cm_thresh_band_avx512(c->angles[0], c->flt_angles[0], c->csf_a_stride, i, j);
    sum = _mm512_add_epi32(
        sum, cm_thresh_band_avx512(c->angles[1], c->flt_angles[1], c->csf_a_stride, i, j));
    sum = _mm512_add_epi32(
        sum, cm_thresh_band_avx512(c->angles[2], c->flt_angles[2], c->csf_a_stride, i, j));
    return _mm512_and_si512(mask_end, sum);
}

/* Rounded (|x| - thr)^3 of sixteen samples of one band, added to two
 * accumulators of eight int64 lanes (adm_cm_accum_round()). */
static FORCE_INLINE void cm_accum_avx512(__m512i x, __m512i thr, const AdmCmBand *p,
                                         __m512i *accum_lo, __m512i *accum_hi)
{
    const __m512i add_sq = _mm512_set1_epi64(p->add_shift_sq);
    const __m512i add_cub = _mm512_set1_epi64(p->add_shift_cub);
    const unsigned shift_sq = (unsigned)p->shift_sq;

    x = _mm512_sub_epi32(_mm512_abs_epi32(x), _mm512_slli_epi32(thr, (unsigned)p->shift_sub));
    x = _mm512_max_epi32(x, _mm512_setzero_si512());
    const __m512i x_hi = _mm512_srli_epi64(x, 32);

    __m512i lo = _mm512_srai_epi64(_mm512_add_epi64(_mm512_mul_epi32(x, x), add_sq), shift_sq);
    __m512i hi =
        _mm512_srai_epi64(_mm512_add_epi64(_mm512_mul_epi32(x_hi, x_hi), add_sq), shift_sq);
    lo = _mm512_srai_epi64(_mm512_add_epi64(_mm512_mul_epi32(lo, x), add_cub), p->shift_cub);
    hi = _mm512_srai_epi64(_mm512_add_epi64(_mm512_mul_epi32(hi, x_hi), add_cub), p->shift_cub);
    *accum_lo = _mm512_add_epi64(*accum_lo, lo);
    *accum_hi = _mm512_add_epi64(*accum_hi, hi);
}

/* An interior row, fourteen columns at a time. A row that reaches the first
 * or the last column needs the mirrored neighbourhood and stays scalar. */
static void cm_row_avx512(const AdmCmCtx *c, int i, const AdmCmBounds *bd, int64_t inner[3])
{
    if (bd->left_edge || bd->right_edge) {
        adm_cm_row(c, i, bd, inner);
        return;
    }

    const int end_col_mod14 = bd->end_col - ((bd->end_col - bd->start_col) % 14);
    const int16_t *const src[3] = {c->src->band_h, c->src->band_v, c->src->band_d};
    __m512i accum_lo[3] = {_mm512_setzero_si512(), _mm512_setzero_si512(), _mm512_setzero_si512()};
    __m512i accum_hi[3] = {_mm512_setzero_si512(), _mm512_setzero_si512(), _mm512_setzero_si512()};

    for (int j = bd->start_col; j < end_col_mod14; j += 14) {
        const ptrdiff_t idx = (ptrdiff_t)i * c->src_stride + j;
        const __m512i thr = cm_thresh_avx512(c, i, j);

        for (int k = 0; k < 3; ++k) {
            const __m512i rfactor = _mm512_maskz_set1_epi32(0x3FFF, c->i_rfactor[k]);
            const __m512i x = _mm512_mullo_epi32(load_epi16x16(src[k] + idx), rfactor);
            cm_accum_avx512(x, thr, &c->band[k], &accum_lo[k], &accum_hi[k]);
        }
    }
    for (int k = 0; k < 3; ++k) {
        inner[k] += hsum_epi64(_mm512_add_epi64(accum_lo[k], accum_hi[k]));
    }
    for (int j = end_col_mod14; j < bd->end_col; ++j) {
        adm_cm_accum_px(c, i, j, inner);
    }
}

float adm_cm_avx512(AdmBuffer *buf, int w, int h, int src_stride, int csf_a_stride,
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
    adm_cm_rows(&c, &bd, cm_row_avx512, accum);
    return adm_cm_result(&c, &bd, accum, adm_noise_weight, adm_p_norm);
}

/* ------------------------------------------------------------------------- */
/* Contrast masking (numerator), scales 1..3                                 */
/* ------------------------------------------------------------------------- */

/* One band's share of the masking threshold of the six columns starting at
 * `j` of an interior row `i` (i4_adm_cm_thresh()). Lanes 6 and 7 are not
 * complete sums. */
static FORCE_INLINE __m512i i4_cm_thresh_band_avx512(const I4AdmCmCtx *c, int theta, int i, int j)
{
    const __m512i perm1 = _mm512_set_epi64(0, 7, 6, 5, 4, 3, 2, 1);
    const __m512i perm2 = _mm512_set_epi64(1, 0, 7, 6, 5, 4, 3, 2);
    const ptrdiff_t stride = c->csf_a_stride;
    const int32_t *flt_row = c->flt_angles[theta] + (stride * (i - 1)) + j - 1;
    const __m512i flt0 = load_epi32x8(flt_row);
    const __m512i flt1 = load_epi32x8(flt_row + stride);
    const __m512i flt2 = load_epi32x8(flt_row + 2 * stride);

    __m512i centre = _mm512_abs_epi64(load_epi32x8(c->angles[theta] + (stride * i) + j - 1));
    centre = _mm512_mul_epi32(centre, _mm512_set1_epi64(I4_ONE_BY_15));
    centre = _mm512_srai_epi64(_mm512_add_epi64(centre, _mm512_set1_epi64(c->add_bef_shift_flt)),
                               c->shift_flt);
    centre = _mm512_sub_epi64(centre, flt1);

    const __m512i rows = _mm512_add_epi64(_mm512_add_epi64(flt0, flt1), flt2);
    __m512i sum = _mm512_add_epi64(rows, _mm512_permutexvar_epi64(perm1, rows));
    sum = _mm512_add_epi64(sum, _mm512_permutexvar_epi64(perm2, rows));
    return _mm512_add_epi64(sum, _mm512_permutexvar_epi64(perm1, centre));
}

/* Masking threshold of the six columns starting at `j`; lanes 6 and 7 are 0. */
static FORCE_INLINE __m512i i4_cm_thresh_avx512(const I4AdmCmCtx *c, int i, int j)
{
    const __m512i mask_end = _mm512_set_epi64(0x0LL, 0x0LL, -1LL, -1LL, -1LL, -1LL, -1LL, -1LL);
    __m512i sum = i4_cm_thresh_band_avx512(c, 0, i, j);
    sum = _mm512_add_epi64(sum, i4_cm_thresh_band_avx512(c, 1, i, j));
    sum = _mm512_add_epi64(sum, i4_cm_thresh_band_avx512(c, 2, i, j));
    return _mm512_and_si512(mask_end, sum);
}

/* Rounded (|x| - thr)^3 of the samples of one band held in int64 lanes
 * (i4_adm_cm_accum_round()). */
static FORCE_INLINE __m512i i4_cm_cube_avx512(__m512i x, __m512i thr, const AdmCmBand *p)
{
    const unsigned shift_sq = (unsigned)p->shift_sq;

    x = _mm512_sub_epi64(_mm512_abs_epi64(x), _mm512_srli_epi64(thr, (unsigned)p->shift_sub));
    x = _mm512_max_epi64(x, _mm512_setzero_si512());

    __m512i v = _mm512_add_epi64(_mm512_mul_epi32(x, x), _mm512_set1_epi64(p->add_shift_sq));
    v = _mm512_srai_epi64(v, shift_sq);
    v = _mm512_add_epi64(_mm512_mul_epi32(v, x), _mm512_set1_epi64(p->add_shift_cub));
    return _mm512_srai_epi64(v, p->shift_cub);
}

/* An interior row, six columns at a time; see cm_row_avx512(). */
static void i4_cm_row_avx512(const I4AdmCmCtx *c, int i, const AdmCmBounds *bd, int64_t inner[3])
{
    if (bd->left_edge || bd->right_edge) {
        i4_adm_cm_row(c, i, bd, inner);
        return;
    }

    const int end_col_mod6 = bd->end_col - ((bd->end_col - bd->start_col) % 6);
    const int32_t *const src[3] = {c->src->band_h, c->src->band_v, c->src->band_d};
    const __m512i add_dst = _mm512_set1_epi64(c->add_bef_shift_dst);
    __m512i accum[3] = {_mm512_setzero_si512(), _mm512_setzero_si512(), _mm512_setzero_si512()};

    for (int j = bd->start_col; j < end_col_mod6; j += 6) {
        const ptrdiff_t idx = (ptrdiff_t)i * c->src_stride + j;
        __m512i x[3];

        for (int t = 0; t < 3; ++t) {
            const __m512i rfactor = _mm512_maskz_set1_epi64(0x3F, c->rfactor[t]);
            x[t] = _mm512_mul_epi32(load_epi32x8(src[t] + idx), rfactor);
            x[t] = _mm512_srai_epi64(_mm512_add_epi64(x[t], add_dst), c->shift_dst);
        }
        const __m512i thr = i4_cm_thresh_avx512(c, i, j);
        for (int t = 0; t < 3; ++t) {
            accum[t] = _mm512_add_epi64(accum[t], i4_cm_cube_avx512(x[t], thr, &c->band));
        }
    }
    for (int t = 0; t < 3; ++t) {
        inner[t] += hsum_epi64(accum[t]);
    }
    for (int j = end_col_mod6; j < bd->end_col; ++j) {
        i4_adm_cm_accum_px(c, i, j, inner);
    }
}

float i4_adm_cm_avx512(AdmBuffer *buf, int w, int h, int src_stride, int csf_a_stride, int scale,
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
    i4_adm_cm_rows(&c, &bd, i4_cm_row_avx512, accum);
    return i4_adm_cm_result(&c, &bd, accum, adm_noise_weight, adm_p_norm);
}
