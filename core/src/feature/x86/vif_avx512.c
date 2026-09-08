/**
     *
     *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
     *
     *     Licensed under the BSD+Patent License (the "License"); you may not
     *     use this file except in compliance with the License. You may obtain a
     *     copy of the License at
     *
     *         https://opensource.org/licenses/BSDplusPatent
     *
     *     Unless required by applicable law or agreed to in writing, software
     *     distributed under the License is distributed on an "AS IS" BASIS,
     *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
     *     implied. See the License for the specific language governing
     *     permissions and limitations under the License.
     *
     */

#include <immintrin.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include "feature/common/macros.h"
#include "vif_avx512.h"

/* ADR-1138: retain NULL for Windows C and upstream C compatibility. */
// NOLINTBEGIN(modernize-use-nullptr)

#define MIN(x, y) (((x) < (y)) ? (x) : (y))
#define MAX(x, y) (((x) > (y)) ? (x) : (y))

/*
 * Portable noinline attribute.
 *
 * GCC/Clang: __attribute__((noinline, noclone))
 *   - noinline: prevents the compiler from inlining the function.
 *   - noclone:  prevents GCC from synthesising specialised clones of the
 *               function (e.g. a constant-propagated clone).  Clones would
 *               undo the register-pressure isolation that is the whole point
 *               of factoring these helpers out (ADR-0503).
 *
 * MSVC: __declspec(noinline)
 *   - MSVC does not clone, so no equivalent of noclone is needed.
 *   - __attribute__ syntax is not supported by cl.exe and causes a hard
 *     syntax error (C2143 / C2059) — hence the guard (ADR-0519).
 */
#if defined(_MSC_VER)
#define VMAF_NOINLINE_NOCLONE __declspec(noinline)
#elif defined(__GNUC__) && !defined(__clang__)
#define VMAF_NOINLINE_NOCLONE __attribute__((noinline, noclone))
#elif defined(__clang__)
#define VMAF_NOINLINE_NOCLONE __attribute__((noinline))
#else
#define VMAF_NOINLINE_NOCLONE
#endif

static inline void pad_top_and_bottom(const VifBuffer *buf, unsigned h, int fwidth)
{
    const unsigned fwidth_half = fwidth / 2;
    unsigned char *ref = buf->ref;
    unsigned char *dis = buf->dis;
    for (unsigned i = 1; i <= fwidth_half; ++i) {
        size_t offset = buf->stride * i;
        memcpy(ref - offset, ref + offset, buf->stride);
        memcpy(dis - offset, dis + offset, buf->stride);
        memcpy(ref + buf->stride * (h - 1) + buf->stride * i,
               ref + buf->stride * (h - 1) - buf->stride * i, buf->stride);
        memcpy(dis + buf->stride * (h - 1) + buf->stride * i,
               dis + buf->stride * (h - 1) - buf->stride * i, buf->stride);
    }
}

static inline void decimate_and_pad(const VifBuffer *buf, unsigned w, unsigned h, int scale)
{
    uint16_t *ref = buf->ref;
    uint16_t *dis = buf->dis;
    const ptrdiff_t stride = buf->stride / sizeof(uint16_t);
    const ptrdiff_t mu_stride = buf->stride_16 / sizeof(uint16_t);

    for (unsigned i = 0; i < h / 2; ++i) {
        for (unsigned j = 0; j < w / 2; ++j) {
            ref[i * stride + j] = buf->mu1[((ptrdiff_t)i * 2) * mu_stride + ((ptrdiff_t)j * 2)];
            dis[i * stride + j] = buf->mu2[((ptrdiff_t)i * 2) * mu_stride + ((ptrdiff_t)j * 2)];
        }
    }
    pad_top_and_bottom(buf, h / 2, vif_filter1d_width[scale]);
}

typedef struct Residuals512 {
    __m512i maccum_num_log;
    __m512i maccum_den_log;
    __m512i maccum_num_non_log;
    __m512i maccum_den_non_log;
} Residuals512;

static FORCE_INLINE __m512i vif_log_denominator512(__m512i msigma1, const uint16_t *log2_table)
{
    static const int32_t sigma_nsq = 65536 << 1;
    // log stage
    __m512i mlog_den_stage1 = _mm512_add_epi64(msigma1, _mm512_set1_epi64(sigma_nsq));
    __m512i mnorm = _mm512_sub_epi64(_mm512_set1_epi64(48), _mm512_lzcnt_epi64(mlog_den_stage1));
    __m512i mlog_den1 = _mm512_srlv_epi64(mlog_den_stage1, mnorm);
    /* ADR-0500: VIF_LOG2_TABLE_SIZE is 32768 uint16_t entries.
     * The normalised mantissa is in [32768..65535]; mask off bit 15 to
     * obtain the 15-bit index into log2_table[0..32767].  All three gather
     * sites below apply the same mask.  Bit-exactness is preserved. */
    __m256i mlog_den1_idx = _mm256_and_si256(_mm512_cvtusepi64_epi32(mlog_den1),
                                             _mm256_set1_epi32((int)(VIF_LOG2_TABLE_SIZE - 1u)));
    __m512i mden_val = _mm512_i32gather_epi64(mlog_den1_idx, log2_table, sizeof(*log2_table));
    mden_val = _mm512_and_si512(mden_val, _mm512_set1_epi64(0xffff)); // we took 64 bits, we need 16
    mden_val = _mm512_add_epi64(mden_val, _mm512_slli_epi64(mnorm, 11));
    mden_val = _mm512_sub_epi64(mden_val, _mm512_set1_epi64(INT64_C(2048) * 17));
    return mden_val;
}

static FORCE_INLINE __m512i vif_log_numerator512(__m512i msigma1, __m512i msigma2, __m512i msigma12,
                                                 const uint16_t *log2_table,
                                                 double vif_enhn_gain_limit)
{
    static const int32_t sigma_nsq = 65536 << 1;
    const double eps = 65536 * 1.0e-10;
    __m512d msigma1_d = _mm512_cvtepu64_pd(msigma1);
    __m512d mg =
        _mm512_div_pd(_mm512_cvtepu64_pd(msigma12), _mm512_add_pd(msigma1_d, _mm512_set1_pd(eps)));
    __m512i msv_sq = _mm512_cvttpd_epi64(_mm512_sub_pd(
        _mm512_cvtepi64_pd(msigma2), _mm512_mul_pd(mg, _mm512_cvtepi64_pd(msigma12))));
    msv_sq = _mm512_max_epi64(msv_sq, _mm512_setzero_si512());
    mg = _mm512_min_pd(mg, _mm512_set1_pd(vif_enhn_gain_limit));

    __m512i mnumer1 = _mm512_add_epi64(msv_sq, _mm512_set1_epi64(sigma_nsq));
    __m512i mnumer1_lz = _mm512_sub_epi64(_mm512_set1_epi64(48), _mm512_lzcnt_epi64(mnumer1));
    __m512i mnumer1_mantissa = _mm512_srlv_epi64(mnumer1, mnumer1_lz);
    __m512i mnumer1_mantissa_log = _mm512_and_si512(
        _mm512_set1_epi64(0xffff),
        _mm512_i32gather_epi64(_mm256_and_si256(_mm512_cvtusepi64_epi32(mnumer1_mantissa),
                                                _mm256_set1_epi32((int)(VIF_LOG2_TABLE_SIZE - 1u))),
                               log2_table, sizeof(*log2_table))); // we took 64 bits, we need 16
    __m512i mnumer1_log = _mm512_add_epi64(mnumer1_mantissa_log, _mm512_slli_epi64(mnumer1_lz, 11));

    __m512i mnumer1_tmp = _mm512_add_epi64(
        mnumer1, _mm512_cvttpd_epi64(_mm512_mul_pd(_mm512_mul_pd(mg, mg), msigma1_d)));
    __m512i mnumer1_tmp_lz =
        _mm512_sub_epi64(_mm512_set1_epi64(48), _mm512_lzcnt_epi64(mnumer1_tmp));
    __m512i mnumer1_tmp_mantissa = _mm512_srlv_epi64(mnumer1_tmp, mnumer1_tmp_lz);
    __m512i mnumer1_tmp_mantissa_log = _mm512_and_si512(
        _mm512_set1_epi64(0xffff),
        _mm512_i32gather_epi64(_mm256_and_si256(_mm512_cvtusepi64_epi32(mnumer1_tmp_mantissa),
                                                _mm256_set1_epi32((int)(VIF_LOG2_TABLE_SIZE - 1u))),
                               log2_table, sizeof(*log2_table))); // we took 64 bits, we need 16
    __m512i mnumer1_tmp_log =
        _mm512_add_epi64(mnumer1_tmp_mantissa_log, _mm512_slli_epi64(mnumer1_tmp_lz, 11));

    __m512i mnum_val = _mm512_sub_epi64(mnumer1_tmp_log, mnumer1_log);

    return mnum_val;
}

// compute VIF on a 16 pixel block from xx (ref variance), yy (clamped dis variance), xy (ref dis covariance)
static inline void vif_statistic_avx512(Residuals512 *out, __m512i xx, __m512i xy, __m512i yy,
                                        const uint16_t *log2_table, double vif_enhn_gain_limit)
{
    //float equivalent of 2. (2 * 65536)
    static const int32_t sigma_nsq = 65536 << 1;

    __m512i maccum_num_log = out->maccum_num_log;
    __m512i maccum_den_log = out->maccum_den_log;
    __m512i maccum_num_non_log = out->maccum_num_non_log;
    __m512i maccum_den_non_log = out->maccum_den_non_log;

    for (int b = 0; b < 16; b += 8) {
        __m512i msigma1 = _mm512_cvtepi32_epi64(_mm512_castsi512_si256(xx));
        __m512i msigma2 = _mm512_cvtepi32_epi64(_mm512_castsi512_si256(yy));
        __m512i msigma12 = _mm512_cvtepi32_epi64(_mm512_castsi512_si256(xy));
        xx = _mm512_castsi256_si512(_mm512_extracti64x4_epi64(xx, 1));
        yy = _mm512_castsi256_si512(_mm512_extracti64x4_epi64(yy, 1));
        xy = _mm512_castsi256_si512(_mm512_extracti64x4_epi64(xy, 1));
        msigma2 = _mm512_max_epi64(msigma2, _mm512_setzero_si512());
        msigma12 = _mm512_max_epi64(msigma12, _mm512_setzero_si512());

        __m512i mden_val = vif_log_denominator512(msigma1, log2_table);
        __mmask8 msigma1_mask = _mm512_cmpgt_epi64_mask(_mm512_set1_epi64(sigma_nsq), msigma1);
        __mmask8 msigma2_mask = _mm512_cmpgt_epi64_mask(msigma2, _mm512_setzero_si512());
        __mmask8 msigma12_mask = _mm512_cmpgt_epi64_mask(msigma12, _mm512_setzero_si512());
        __m512i mnum_val =
            vif_log_numerator512(msigma1, msigma2, msigma12, log2_table, vif_enhn_gain_limit);

        maccum_num_log =
            _mm512_mask_add_epi64(maccum_num_log, (~msigma1_mask) & msigma12_mask & msigma2_mask,
                                  maccum_num_log, mnum_val);
        maccum_den_log =
            _mm512_mask_add_epi64(maccum_den_log, ~msigma1_mask, maccum_den_log, mden_val);

        // non log stage
        maccum_num_non_log =
            _mm512_mask_add_epi64(maccum_num_non_log, msigma1_mask, maccum_num_non_log, msigma2);
        maccum_den_non_log = _mm512_mask_add_epi64(maccum_den_non_log, msigma1_mask,
                                                   maccum_den_non_log, _mm512_set1_epi64(1));
    }

    out->maccum_num_log = maccum_num_log;
    out->maccum_den_log = maccum_den_log;
    out->maccum_num_non_log = maccum_num_non_log;
    out->maccum_den_non_log = maccum_den_non_log;
}

typedef struct VifMoments512 {
    __m512i mu1sq;
    __m512i mu2sq;
    __m512i mu1mu2;
} VifMoments512;

/* Preserve each accumulator's center, backward tap, forward tap sequence. */
typedef struct VifMeans512 {
    __m512i ref;
    __m512i dis;
} VifMeans512;

static FORCE_INLINE VifMeans512 vif_horizontal_means512(const VifBuffer *buf, unsigned j,
                                                        unsigned fwidth, const uint16_t *vif_filt)
{
    __m512i fq = _mm512_set1_epi32(vif_filt[fwidth / 2]);
    __m512i acc0 =
        _mm512_mullo_epi32(_mm512_loadu_si512((const __m512i *)(buf->tmp.mu1 + j + 0)), fq);
    __m512i acc1 =
        _mm512_mullo_epi32(_mm512_loadu_si512((const __m512i *)(buf->tmp.mu2 + j + 0)), fq);

    /* Research-2046: preserve the original fused channels and bounded GCC live set. */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC unroll 1
#endif
    for (unsigned fj = 0; fj < fwidth / 2; ++fj) {
        __m512i f_tap = _mm512_set1_epi32(vif_filt[fj]);
        acc0 = _mm512_add_epi64(
            acc0, _mm512_mullo_epi32(
                      _mm512_loadu_si512((const __m512i *)(buf->tmp.mu1 + j - fwidth / 2 + fj + 0)),
                      f_tap));
        acc0 = _mm512_add_epi64(
            acc0, _mm512_mullo_epi32(
                      _mm512_loadu_si512((const __m512i *)(buf->tmp.mu1 + j + fwidth / 2 - fj + 0)),
                      f_tap));
        acc1 = _mm512_add_epi64(
            acc1, _mm512_mullo_epi32(
                      _mm512_loadu_si512((const __m512i *)(buf->tmp.mu2 + j - fwidth / 2 + fj + 0)),
                      f_tap));
        acc1 = _mm512_add_epi64(
            acc1, _mm512_mullo_epi32(
                      _mm512_loadu_si512((const __m512i *)(buf->tmp.mu2 + j + fwidth / 2 - fj + 0)),
                      f_tap));
    }
    const VifMeans512 out = {acc0, acc1};
    return out;
}

static FORCE_INLINE VifMoments512 vif_horizontal_moments512(const VifBuffer *buf, unsigned j,
                                                            unsigned fwidth,
                                                            const uint16_t *vif_filt)
{
    VifMoments512 out;
    const __m512i mask5 =
        _mm512_set_epi32(30, 28, 14, 12, 26, 24, 10, 8, 22, 20, 6, 4, 18, 16, 2, 0);
    const VifMeans512 means = vif_horizontal_means512(buf, j, fwidth, vif_filt);
    const __m512i acc0 = means.ref;
    const __m512i acc1 = means.dis;
    __m512i mu1 = acc0;
    __m512i acc0_lo_512 = _mm512_unpacklo_epi32(acc0, _mm512_setzero_si512());
    __m512i acc0_hi_512 = _mm512_unpackhi_epi32(acc0, _mm512_setzero_si512());
    acc0_lo_512 = _mm512_mul_epu32(acc0_lo_512, acc0_lo_512);
    acc0_hi_512 = _mm512_mul_epu32(acc0_hi_512, acc0_hi_512);
    acc0_lo_512 =
        _mm512_srli_epi64(_mm512_add_epi64(acc0_lo_512, _mm512_set1_epi64(0x80000000)), 32);
    acc0_hi_512 =
        _mm512_srli_epi64(_mm512_add_epi64(acc0_hi_512, _mm512_set1_epi64(0x80000000)), 32);
    out.mu1sq = _mm512_permutex2var_epi32(acc0_lo_512, mask5, acc0_hi_512);

    __m512i acc0lo_512 = _mm512_unpacklo_epi32(acc1, _mm512_setzero_si512());
    __m512i acc0hi_512 = _mm512_unpackhi_epi32(acc1, _mm512_setzero_si512());
    __m512i mu1lo_512 = _mm512_unpacklo_epi32(mu1, _mm512_setzero_si512());
    __m512i mu1hi_512 = _mm512_unpackhi_epi32(mu1, _mm512_setzero_si512());

    mu1lo_512 = _mm512_mul_epu32(mu1lo_512, acc0lo_512);
    mu1hi_512 = _mm512_mul_epu32(mu1hi_512, acc0hi_512);
    mu1lo_512 = _mm512_srli_epi64(_mm512_add_epi64(mu1lo_512, _mm512_set1_epi64(0x80000000)), 32);
    mu1hi_512 = _mm512_srli_epi64(_mm512_add_epi64(mu1hi_512, _mm512_set1_epi64(0x80000000)), 32);

    out.mu1mu2 = _mm512_permutex2var_epi32(mu1lo_512, mask5, mu1hi_512);
    acc0lo_512 = _mm512_mul_epu32(acc0lo_512, acc0lo_512);
    acc0hi_512 = _mm512_mul_epu32(acc0hi_512, acc0hi_512);
    acc0lo_512 = _mm512_srli_epi64(_mm512_add_epi64(acc0lo_512, _mm512_set1_epi64(0x80000000)), 32);
    acc0hi_512 = _mm512_srli_epi64(_mm512_add_epi64(acc0hi_512, _mm512_set1_epi64(0x80000000)), 32);
    out.mu2sq = _mm512_permutex2var_epi32(acc0lo_512, mask5, acc0hi_512);
    return out;
}

typedef struct VifPair512 {
    __m512i lo;
    __m512i hi;
} VifPair512;

typedef struct VifEnergies512 {
    __m512i ref;
    __m512i dis;
    __m512i cross;
} VifEnergies512;

static FORCE_INLINE VifPair512 vif_horizontal_energy_init512(const uint32_t *plane, unsigned j,
                                                             __m512i fq)
{
    const __m512i rounder = _mm512_set1_epi64(0x8000);
    const __m512i s0 = _mm512_cvtepu32_epi64(_mm256_loadu_si256((const __m256i *)(plane + j)));
    const __m512i s2 = _mm512_cvtepu32_epi64(_mm256_loadu_si256((const __m256i *)(plane + j + 8)));
    const VifPair512 out = {
        _mm512_add_epi64(rounder, _mm512_mul_epu32(s0, fq)),
        _mm512_add_epi64(rounder, _mm512_mul_epu32(s2, fq)),
    };
    return out;
}

static FORCE_INLINE void vif_horizontal_energy_add512(VifPair512 *acc, const uint32_t *plane,
                                                      ptrdiff_t j, __m512i f_tap)
{
    const __m512i s0 = _mm512_cvtepu32_epi64(_mm256_loadu_si256((const __m256i *)(plane + j)));
    const __m512i s2 = _mm512_cvtepu32_epi64(_mm256_loadu_si256((const __m256i *)(plane + j + 8)));
    acc->lo = _mm512_add_epi64(acc->lo, _mm512_mul_epu32(s0, f_tap));
    acc->hi = _mm512_add_epi64(acc->hi, _mm512_mul_epu32(s2, f_tap));
}

static FORCE_INLINE __m512i vif_horizontal_energy_pack512(VifPair512 acc)
{
    const __m512i mask2 =
        _mm512_set_epi32(30, 28, 26, 24, 22, 20, 18, 16, 14, 12, 10, 8, 6, 4, 2, 0);
    acc.lo = _mm512_srli_epi64(acc.lo, 16);
    acc.hi = _mm512_srli_epi64(acc.hi, 16);
    return _mm512_permutex2var_epi32(acc.lo, mask2, acc.hi);
}

static FORCE_INLINE VifEnergies512 vif_horizontal_energies512(const VifBuffer *buf, unsigned j,
                                                              unsigned fwidth,
                                                              const uint16_t *vif_filt)
{
    const __m512i fq = _mm512_set1_epi64(vif_filt[fwidth / 2]);
    VifPair512 ref = vif_horizontal_energy_init512(buf->tmp.ref, j, fq);
    VifPair512 dis = vif_horizontal_energy_init512(buf->tmp.dis, j, fq);
    VifPair512 cross = vif_horizontal_energy_init512(buf->tmp.ref_dis, j, fq);
    /* Research-2046: retain one fused three-channel tap loop, as in the original kernel. */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC unroll 1
#endif
    for (unsigned fj = 0; fj < fwidth / 2; ++fj) {
        const __m512i f_tap = _mm512_set1_epi64(vif_filt[fj]);
        const ptrdiff_t back = (ptrdiff_t)j - (ptrdiff_t)(fwidth / 2) + (ptrdiff_t)fj;
        const ptrdiff_t forward = (ptrdiff_t)j + (ptrdiff_t)(fwidth / 2) - (ptrdiff_t)fj;
        vif_horizontal_energy_add512(&ref, buf->tmp.ref, back, f_tap);
        vif_horizontal_energy_add512(&ref, buf->tmp.ref, forward, f_tap);
        vif_horizontal_energy_add512(&dis, buf->tmp.dis, back, f_tap);
        vif_horizontal_energy_add512(&dis, buf->tmp.dis, forward, f_tap);
        vif_horizontal_energy_add512(&cross, buf->tmp.ref_dis, back, f_tap);
        vif_horizontal_energy_add512(&cross, buf->tmp.ref_dis, forward, f_tap);
    }
    const VifEnergies512 out = {
        vif_horizontal_energy_pack512(ref),
        vif_horizontal_energy_pack512(dis),
        vif_horizontal_energy_pack512(cross),
    };
    return out;
}

static FORCE_INLINE void vif_horizontal_statistics512(Residuals512 *out, const VifBuffer *buf,
                                                      unsigned j, unsigned fwidth,
                                                      const uint16_t *vif_filt,
                                                      const uint16_t *log2_table,
                                                      double vif_enhn_gain_limit)
{
    const VifMoments512 m = vif_horizontal_moments512(buf, j, fwidth, vif_filt);
    const VifEnergies512 energy = vif_horizontal_energies512(buf, j, fwidth, vif_filt);
    const __m512i xx = _mm512_sub_epi32(energy.ref, m.mu1sq);
    const __m512i yy =
        _mm512_max_epi32(_mm512_sub_epi32(energy.dis, m.mu2sq), _mm512_setzero_si512());
    const __m512i xy = _mm512_sub_epi32(energy.cross, m.mu1mu2);
    vif_statistic_avx512(out, xx, xy, yy, log2_table, vif_enhn_gain_limit);
}

typedef struct VifEnergy512 {
    __m512i lane0;
    __m512i lane2;
    __m512i lane1;
    __m512i lane3;
} VifEnergy512;

typedef struct VifStatConfig512 {
    VifBuffer buf;
    unsigned fwidth;
    const uint16_t *vif_filt;
    int fwidth_half;
    ptrdiff_t stride;
    int32_t add_shift_round_VP;
    int32_t shift_VP;
    int32_t add_shift_round_VP_sq;
    int32_t shift_VP_sq;
} VifStatConfig512;

static FORCE_INLINE VifStatConfig512 vif_stat_config512(const VifPublicState *s, int bpc, int scale)
{
    VifStatConfig512 c;
    c.buf = s->buf;
    c.fwidth = vif_filter1d_width[scale];
    c.vif_filt = vif_filter1d_table[scale];
    c.fwidth_half = c.fwidth >> 1;
    c.stride = c.buf.stride / sizeof(uint16_t);
    if (scale == 0) {
        c.shift_VP = bpc;
        c.add_shift_round_VP = 1 << (bpc - 1);
        c.shift_VP_sq = (bpc - 8) * 2;
        c.add_shift_round_VP_sq = (bpc == 8) ? 0 : 1 << (c.shift_VP_sq - 1);
    } else {
        c.shift_VP = 16;
        c.add_shift_round_VP = 32768;
        c.shift_VP_sq = 16;
        c.add_shift_round_VP_sq = 32768;
    }
    return c;
}

typedef struct VifTaps8 {
    __m512i back0;
    __m512i fwd0;
    __m512i back1;
    __m512i fwd1;
} VifTaps8;

typedef struct VifVertical8 {
    VifPair512 mu1;
    VifPair512 mu2;
    VifPair512 ref;
    VifPair512 dis;
    VifPair512 ref_dis;
} VifVertical8;

static FORCE_INLINE VifVertical8 vif_vertical_init8(const VifStatConfig512 *c, unsigned i,
                                                    unsigned j)
{
    const __m512i f0 = _mm512_set1_epi32(c->vif_filt[c->fwidth / 2]);
    const uint8_t *ref = c->buf.ref;
    const uint8_t *dis = c->buf.dis;
    const __m512i r0 =
        _mm512_cvtepu8_epi16(_mm256_loadu_si256((const __m256i *)(ref + c->buf.stride * i + j)));
    const __m512i d0 =
        _mm512_cvtepu8_epi16(_mm256_loadu_si256((const __m256i *)(dis + c->buf.stride * i + j)));
    const __m512i rlo = _mm512_unpacklo_epi16(r0, _mm512_setzero_si512());
    const __m512i rhi = _mm512_unpackhi_epi16(r0, _mm512_setzero_si512());
    const __m512i dlo = _mm512_unpacklo_epi16(d0, _mm512_setzero_si512());
    const __m512i dhi = _mm512_unpackhi_epi16(d0, _mm512_setzero_si512());
    const VifVertical8 out = {
        {_mm512_mullo_epi32(rlo, f0), _mm512_mullo_epi32(rhi, f0)},
        {_mm512_mullo_epi32(dlo, f0), _mm512_mullo_epi32(dhi, f0)},
        {_mm512_mullo_epi32(f0, _mm512_mullo_epi32(rlo, rlo)),
         _mm512_mullo_epi32(f0, _mm512_mullo_epi32(rhi, rhi))},
        {_mm512_mullo_epi32(f0, _mm512_mullo_epi32(dlo, dlo)),
         _mm512_mullo_epi32(f0, _mm512_mullo_epi32(dhi, dhi))},
        {_mm512_mullo_epi32(f0, _mm512_mullo_epi32(rlo, dlo)),
         _mm512_mullo_epi32(f0, _mm512_mullo_epi32(rhi, dhi))},
    };
    return out;
}

static FORCE_INLINE VifTaps8 vif_vertical_taps8(const uint8_t *plane, ptrdiff_t stride, int back,
                                                int forward, unsigned j)
{
    const VifTaps8 out = {
        _mm512_cvtepu8_epi16(_mm256_loadu_si256((const __m256i *)(plane + stride * back + j))),
        _mm512_cvtepu8_epi16(_mm256_loadu_si256((const __m256i *)(plane + stride * forward + j))),
        _mm512_cvtepu8_epi16(
            _mm256_loadu_si256((const __m256i *)(plane + stride * (back + 1) + j))),
        _mm512_cvtepu8_epi16(
            _mm256_loadu_si256((const __m256i *)(plane + stride * (forward - 1) + j))),
    };
    return out;
}

static FORCE_INLINE void vif_vertical_mean8(VifPair512 *acc, VifTaps8 t, __m512i coefficients)
{
    const __m512i paired0 = _mm512_add_epi16(t.back0, t.fwd0);
    const __m512i paired1 = _mm512_add_epi16(t.back1, t.fwd1);
    const __m512i lo = _mm512_unpacklo_epi16(paired0, paired1);
    const __m512i hi = _mm512_unpackhi_epi16(paired0, paired1);
    acc->lo = _mm512_add_epi32(acc->lo, _mm512_madd_epi16(lo, coefficients));
    acc->hi = _mm512_add_epi32(acc->hi, _mm512_madd_epi16(hi, coefficients));
}

static FORCE_INLINE void vif_vertical_energy8(VifPair512 *acc, VifTaps8 a, VifTaps8 b,
                                              __m512i f_tap0, __m512i f_tap1)
{
    const __m512i a0lo = _mm512_unpacklo_epi16(a.back0, a.fwd0);
    const __m512i a0hi = _mm512_unpackhi_epi16(a.back0, a.fwd0);
    const __m512i a1lo = _mm512_unpacklo_epi16(a.back1, a.fwd1);
    const __m512i a1hi = _mm512_unpackhi_epi16(a.back1, a.fwd1);
    const __m512i b0lo = _mm512_unpacklo_epi16(b.back0, b.fwd0);
    const __m512i b0hi = _mm512_unpackhi_epi16(b.back0, b.fwd0);
    const __m512i b1lo = _mm512_unpacklo_epi16(b.back1, b.fwd1);
    const __m512i b1hi = _mm512_unpackhi_epi16(b.back1, b.fwd1);
    const __m512i tap0lo = _mm512_madd_epi16(a0lo, b0lo);
    const __m512i tap0hi = _mm512_madd_epi16(a0hi, b0hi);
    const __m512i tap1lo = _mm512_madd_epi16(a1lo, b1lo);
    const __m512i tap1hi = _mm512_madd_epi16(a1hi, b1hi);
    acc->lo = _mm512_add_epi32(acc->lo, _mm512_mullo_epi32(tap0lo, f_tap0));
    acc->hi = _mm512_add_epi32(acc->hi, _mm512_mullo_epi32(tap0hi, f_tap0));
    acc->lo = _mm512_add_epi32(acc->lo, _mm512_mullo_epi32(tap1lo, f_tap1));
    acc->hi = _mm512_add_epi32(acc->hi, _mm512_mullo_epi32(tap1hi, f_tap1));
}

static FORCE_INLINE void vif_vertical_store8(uint32_t *dst, VifPair512 acc)
{
    const __m512i perm_lo = _mm512_set_epi64(11, 10, 3, 2, 9, 8, 1, 0);
    const __m512i perm_hi = _mm512_set_epi64(15, 14, 7, 6, 13, 12, 5, 4);
    const __m512i lo = _mm512_permutex2var_epi64(acc.lo, perm_lo, acc.hi);
    const __m512i hi = _mm512_permutex2var_epi64(acc.lo, perm_hi, acc.hi);
    _mm512_storeu_si512((__m512i *)dst, lo);
    _mm512_storeu_si512((__m512i *)(dst + 16), hi);
}

static FORCE_INLINE void vif_vertical_store_mean8(uint32_t *dst, VifPair512 acc)
{
    const __m512i round_128 = _mm512_set1_epi32(128);
    acc.lo = _mm512_add_epi32(acc.lo, round_128);
    acc.hi = _mm512_add_epi32(acc.hi, round_128);
    acc.lo = _mm512_srli_epi32(acc.lo, 8);
    acc.hi = _mm512_srli_epi32(acc.hi, 8);
    vif_vertical_store8(dst, acc);
}

static FORCE_INLINE void vif_vertical_statistics8_block(const VifStatConfig512 *c, unsigned i,
                                                        unsigned j)
{
    VifVertical8 acc = vif_vertical_init8(c, i, j);
    const int i_back = i - c->fwidth_half;
    const int i_forward = i + c->fwidth_half;
    for (unsigned tap = 0; tap < c->fwidth / 2; tap += 2) {
        const int back = i_back + tap;
        const int forward = i_forward - tap;
        const __m512i f_tap0 = _mm512_set1_epi32(c->vif_filt[tap]);
        const __m512i f_tap1 = _mm512_set1_epi32(c->vif_filt[tap + 1]);
        const __m512i coefficients =
            _mm512_set1_epi32(c->vif_filt[tap] + (c->vif_filt[tap + 1] << 16));
        const VifTaps8 r = vif_vertical_taps8(c->buf.ref, c->buf.stride, back, forward, j);
        const VifTaps8 d = vif_vertical_taps8(c->buf.dis, c->buf.stride, back, forward, j);
        vif_vertical_mean8(&acc.mu1, r, coefficients);
        vif_vertical_mean8(&acc.mu2, d, coefficients);
        vif_vertical_energy8(&acc.ref, r, r, f_tap0, f_tap1);
        vif_vertical_energy8(&acc.dis, d, d, f_tap0, f_tap1);
        vif_vertical_energy8(&acc.ref_dis, d, r, f_tap0, f_tap1);
    }
    vif_vertical_store_mean8(c->buf.tmp.mu1 + j, acc.mu1);
    vif_vertical_store_mean8(c->buf.tmp.mu2 + j, acc.mu2);
    vif_vertical_store8(c->buf.tmp.ref + j, acc.ref);
    vif_vertical_store8(c->buf.tmp.dis + j, acc.dis);
    vif_vertical_store8(c->buf.tmp.ref_dis + j, acc.ref_dis);
}

static FORCE_INLINE void vif_vertical_statistics8(const VifStatConfig512 *c, unsigned w, unsigned i)
{
    /* Keep the original 16-sample extent with 32-sample loads; scalar tails overwrite it. */
    for (unsigned j = 0; j < (w >> 4) << 4; j += 32) {
        vif_vertical_statistics8_block(c, i, j);
    }
    for (unsigned j = (w >> 4) << 4; j < w; ++j) {
        uint32_t accum_mu1 = 0;
        uint32_t accum_mu2 = 0;
        uint64_t accum_ref = 0;
        uint64_t accum_dis = 0;
        uint64_t accum_ref_dis = 0;

        for (unsigned fi = 0; fi < c->fwidth; ++fi) {
            int ii = (int)i - c->fwidth_half;
            int ii_check = ii + fi;
            const uint16_t fcoeff = c->vif_filt[fi];
            const uint8_t *ref = (uint8_t *)c->buf.ref;
            const uint8_t *dis = (uint8_t *)c->buf.dis;
            uint16_t imgcoeff_ref = ref[ii_check * c->buf.stride + j];
            uint16_t imgcoeff_dis = dis[ii_check * c->buf.stride + j];
            uint32_t img_coeff_ref = fcoeff * (uint32_t)imgcoeff_ref;
            uint32_t img_coeff_dis = fcoeff * (uint32_t)imgcoeff_dis;
            accum_mu1 += img_coeff_ref;
            accum_mu2 += img_coeff_dis;
            accum_ref += img_coeff_ref * (uint64_t)imgcoeff_ref;
            accum_dis += img_coeff_dis * (uint64_t)imgcoeff_dis;
            accum_ref_dis += img_coeff_ref * (uint64_t)imgcoeff_dis;
        }

        c->buf.tmp.mu1[j] = (accum_mu1 + 128) >> 8;
        c->buf.tmp.mu2[j] = (accum_mu2 + 128) >> 8;
        c->buf.tmp.ref[j] = accum_ref;
        c->buf.tmp.dis[j] = accum_dis;
        c->buf.tmp.ref_dis[j] = accum_ref_dis;
    }
}

static FORCE_INLINE VifPair512 vif_vertical_weight16(__m512i pixels, __m512i f1)
{
    const __m512i mask3 = _mm512_set_epi64(11, 10, 3, 2, 9, 8, 1, 0);
    const __m512i mask4 = _mm512_set_epi64(15, 14, 7, 6, 13, 12, 5, 4);
    const __m512i hi = _mm512_mulhi_epu16(pixels, f1);
    const __m512i lo = _mm512_mullo_epi16(pixels, f1);
    const __m512i unpack_lo = _mm512_unpacklo_epi16(lo, hi);
    const __m512i unpack_hi = _mm512_unpackhi_epi16(lo, hi);
    const VifPair512 out = {
        _mm512_permutex2var_epi64(unpack_lo, mask3, unpack_hi),
        _mm512_permutex2var_epi64(unpack_lo, mask4, unpack_hi),
    };
    return out;
}

static FORCE_INLINE void vif_vertical_energy16(VifEnergy512 *acc, VifPair512 weighted,
                                               __m512i pixels)
{
    const __m512i sg0 = _mm512_cvtepu32_epi64(_mm512_castsi512_si256(weighted.lo));
    const __m512i sg1 = _mm512_cvtepu32_epi64(_mm512_extracti64x4_epi64(weighted.lo, 1));
    const __m512i sg2 = _mm512_cvtepu32_epi64(_mm512_castsi512_si256(weighted.hi));
    const __m512i sg3 = _mm512_cvtepu32_epi64(_mm512_extracti64x4_epi64(weighted.hi, 1));
    const __m128i l0 = _mm512_castsi512_si128(pixels);
    const __m128i l1 = _mm512_extracti32x4_epi32(pixels, 1);
    const __m128i l2 = _mm512_extracti32x4_epi32(pixels, 2);
    const __m128i l3 = _mm512_extracti32x4_epi32(pixels, 3);
    /* Preserve original accumulator order 0, 2, 1, 3; stores interleave 0/1 and 2/3. */
    acc->lane0 = _mm512_add_epi64(acc->lane0, _mm512_mul_epu32(sg0, _mm512_cvtepu16_epi64(l0)));
    acc->lane2 = _mm512_add_epi64(acc->lane2, _mm512_mul_epu32(sg2, _mm512_cvtepu16_epi64(l2)));
    acc->lane1 = _mm512_add_epi64(acc->lane1, _mm512_mul_epu32(sg1, _mm512_cvtepu16_epi64(l1)));
    acc->lane3 = _mm512_add_epi64(acc->lane3, _mm512_mul_epu32(sg3, _mm512_cvtepu16_epi64(l3)));
}

static FORCE_INLINE void vif_vertical_store_mean16(uint32_t *dst, VifPair512 acc, int round,
                                                   int shift)
{
    const __m512i addnum = _mm512_set1_epi32(round);
    acc.lo = _mm512_add_epi32(acc.lo, addnum);
    acc.hi = _mm512_add_epi32(acc.hi, addnum);
    acc.lo = _mm512_srli_epi32(acc.lo, shift);
    acc.hi = _mm512_srli_epi32(acc.hi, shift);
    _mm512_storeu_si512((__m512i *)dst, acc.lo);
    _mm512_storeu_si512((__m512i *)(dst + 16), acc.hi);
}

static FORCE_INLINE void vif_vertical_store_energy16(uint32_t *dst, VifEnergy512 acc, int round,
                                                     int shift)
{
    const __m512i addnum64 = _mm512_set1_epi64(round);
    const __m512i mask2 =
        _mm512_set_epi32(30, 28, 26, 24, 22, 20, 18, 16, 14, 12, 10, 8, 6, 4, 2, 0);
    acc.lane0 = _mm512_add_epi64(acc.lane0, addnum64);
    acc.lane2 = _mm512_add_epi64(acc.lane2, addnum64);
    acc.lane1 = _mm512_add_epi64(acc.lane1, addnum64);
    acc.lane3 = _mm512_add_epi64(acc.lane3, addnum64);
    acc.lane0 = _mm512_srli_epi64(acc.lane0, shift);
    acc.lane2 = _mm512_srli_epi64(acc.lane2, shift);
    acc.lane1 = _mm512_srli_epi64(acc.lane1, shift);
    acc.lane3 = _mm512_srli_epi64(acc.lane3, shift);
    _mm512_storeu_si512((__m512i *)dst, _mm512_permutex2var_epi32(acc.lane0, mask2, acc.lane1));
    _mm512_storeu_si512((__m512i *)(dst + 16),
                        _mm512_permutex2var_epi32(acc.lane2, mask2, acc.lane3));
}

static FORCE_INLINE void vif_vertical_statistics16_block(const VifStatConfig512 *c, int ii, int j)
{
    const uint16_t *ref = c->buf.ref;
    const uint16_t *dis = c->buf.dis;
    VifPair512 ref_mu = {0};
    VifPair512 dis_mu = {0};
    VifEnergy512 ref_sq = {0};
    VifEnergy512 ref_dis = {0};
    VifEnergy512 dis_sq = {0};
    int ii_check = ii;
    for (unsigned fi = 0; fi < c->fwidth; ++fi, ii_check = ii + fi) {
        const uint16_t fcoeff = c->vif_filt[fi];
        const __m512i f1 = _mm512_set1_epi16(fcoeff);
        const __m512i ref1 = _mm512_loadu_si512((const __m512i *)(ref + ii_check * c->stride + j));
        const __m512i dis1 = _mm512_loadu_si512((const __m512i *)(dis + ii_check * c->stride + j));
        const VifPair512 rmul = vif_vertical_weight16(ref1, f1);
        ref_mu.lo = _mm512_add_epi32(ref_mu.lo, rmul.lo);
        ref_mu.hi = _mm512_add_epi32(ref_mu.hi, rmul.hi);
        const VifPair512 dmul = vif_vertical_weight16(dis1, f1);
        dis_mu.lo = _mm512_add_epi32(dis_mu.lo, dmul.lo);
        dis_mu.hi = _mm512_add_epi32(dis_mu.hi, dmul.hi);
        vif_vertical_energy16(&ref_sq, rmul, ref1);
        vif_vertical_energy16(&ref_dis, rmul, dis1);
        vif_vertical_energy16(&dis_sq, dmul, dis1);
    }
    vif_vertical_store_mean16(c->buf.tmp.mu1 + j, ref_mu, c->add_shift_round_VP, c->shift_VP);
    vif_vertical_store_mean16(c->buf.tmp.mu2 + j, dis_mu, c->add_shift_round_VP, c->shift_VP);
    vif_vertical_store_energy16(c->buf.tmp.ref + j, ref_sq, c->add_shift_round_VP_sq,
                                c->shift_VP_sq);
    vif_vertical_store_energy16(c->buf.tmp.ref_dis + j, ref_dis, c->add_shift_round_VP_sq,
                                c->shift_VP_sq);
    vif_vertical_store_energy16(c->buf.tmp.dis + j, dis_sq, c->add_shift_round_VP_sq,
                                c->shift_VP_sq);
}

static FORCE_INLINE void vif_vertical_statistics16(const VifStatConfig512 *c, unsigned w,
                                                   unsigned i)
{
    const uint16_t *ref = c->buf.ref;
    const uint16_t *dis = c->buf.dis;
    const int ii = (int)i - c->fwidth_half;
    for (int j = 0; j < (int)(w >> 5) << 5; j += 32) {
        vif_vertical_statistics16_block(c, ii, j);
    }
    for (unsigned j = (w >> 5) << 5; j < w; ++j) {
        uint32_t accum_mu1 = 0;
        uint32_t accum_mu2 = 0;
        uint64_t accum_ref = 0;
        uint64_t accum_dis = 0;
        uint64_t accum_ref_dis = 0;
        for (unsigned fi = 0; fi < c->fwidth; ++fi) {
            int ii_check = ii + fi;
            const uint16_t fcoeff = c->vif_filt[fi];
            uint16_t imgcoeff_ref = ref[ii_check * c->stride + j];
            uint16_t imgcoeff_dis = dis[ii_check * c->stride + j];
            uint32_t img_coeff_ref = fcoeff * (uint32_t)imgcoeff_ref;
            uint32_t img_coeff_dis = fcoeff * (uint32_t)imgcoeff_dis;
            accum_mu1 += img_coeff_ref;
            accum_mu2 += img_coeff_dis;
            accum_ref += img_coeff_ref * (uint64_t)imgcoeff_ref;
            accum_dis += img_coeff_dis * (uint64_t)imgcoeff_dis;
            accum_ref_dis += img_coeff_ref * (uint64_t)imgcoeff_dis;
        }
        c->buf.tmp.mu1[j] = (uint16_t)((accum_mu1 + c->add_shift_round_VP) >> c->shift_VP);
        c->buf.tmp.mu2[j] = (uint16_t)((accum_mu2 + c->add_shift_round_VP) >> c->shift_VP);
        c->buf.tmp.ref[j] = (uint32_t)((accum_ref + c->add_shift_round_VP_sq) >> c->shift_VP_sq);
        c->buf.tmp.dis[j] = (uint32_t)((accum_dis + c->add_shift_round_VP_sq) >> c->shift_VP_sq);
        c->buf.tmp.ref_dis[j] =
            (uint32_t)((accum_ref_dis + c->add_shift_round_VP_sq) >> c->shift_VP_sq);
    }
}

static FORCE_INLINE void vif_finish_statistics512(const Residuals512 *r, VifResiduals accum,
                                                  float *num, float *den)
{
    accum.accum_num_log += _mm512_reduce_add_epi64(r->maccum_num_log);
    accum.accum_den_log += _mm512_reduce_add_epi64(r->maccum_den_log);
    accum.accum_num_non_log += _mm512_reduce_add_epi64(r->maccum_num_non_log);
    accum.accum_den_non_log += _mm512_reduce_add_epi64(r->maccum_den_non_log);
    num[0] = accum.accum_num_log / 2048.0 +
             (accum.accum_den_non_log - ((accum.accum_num_non_log) / 16384.0) / (65025.0));
    den[0] = accum.accum_den_log / 2048.0 + accum.accum_den_non_log;
}

/* Research-2046: VifState dispatch requires the mutable VifPublicState callback ABI. */
// cppcheck-suppress constParameterPointer
void vif_statistic_8_avx512(struct VifPublicState *s, float *num, float *den, unsigned w,
                            unsigned h)
{
    const VifStatConfig512 c = vif_stat_config512(s, 8, 0);
    Residuals512 residuals = {0};
    VifResiduals accum = {0};
    for (unsigned i = 0; i < h; ++i) {
        vif_vertical_statistics8(&c, w, i);
        PADDING_SQ_DATA(&c.buf, w, c.fwidth_half);
        const unsigned n = w >> 4;
        for (unsigned j = 0; j < n << 4; j += 16) {
            vif_horizontal_statistics512(&residuals, &c.buf, j, c.fwidth, c.vif_filt, s->log2_table,
                                         s->vif_enhn_gain_limit);
        }
        if ((n << 4) != w) {
            const VifResiduals tail = vif_compute_line_residuals(s, n << 4, w, 0);
            accum.accum_num_log += tail.accum_num_log;
            accum.accum_den_log += tail.accum_den_log;
            accum.accum_num_non_log += tail.accum_num_non_log;
            accum.accum_den_non_log += tail.accum_den_non_log;
        }
    }
    vif_finish_statistics512(&residuals, accum, num, den);
}

/* Research-2046: VifState dispatch requires the mutable VifPublicState callback ABI. */
// cppcheck-suppress constParameterPointer
void vif_statistic_16_avx512(struct VifPublicState *s, float *num, float *den, unsigned w,
                             unsigned h, int bpc, int scale)
{
    const VifStatConfig512 c = vif_stat_config512(s, bpc, scale);
    Residuals512 residuals = {0};
    VifResiduals accum = {0};
    for (unsigned i = 0; i < h; ++i) {
        vif_vertical_statistics16(&c, w, i);
        PADDING_SQ_DATA(&c.buf, w, c.fwidth_half);
        const int n = w >> 4;
        for (int j = 0; j < n << 4; j += 16) {
            vif_horizontal_statistics512(&residuals, &c.buf, j, c.fwidth, c.vif_filt, s->log2_table,
                                         s->vif_enhn_gain_limit);
        }
        if ((n << 4) != (int)w) {
            const VifResiduals tail = vif_compute_line_residuals(s, n << 4, w, scale);
            accum.accum_num_log += tail.accum_num_log;
            accum.accum_den_log += tail.accum_den_log;
            accum.accum_num_non_log += tail.accum_num_non_log;
            accum.accum_den_non_log += tail.accum_den_non_log;
        }
    }
    vif_finish_statistics512(&residuals, accum, num, den);
}

/* ADR-0503: loop-fission helpers for vif_subsample_rd_8_avx512.
 *
 * Moving the vertical and horizontal inner-loop bodies into separate
 * __attribute__((noinline)) functions reduces the simultaneous ZMM live-set
 * inside each function from ~30 to ~20, eliminating the vmovdqa64-to-stack
 * spill cluster (zmm13/zmm7/zmm15, 4.47%+4.29%+1.10% of profiled cycles).
 *
 * Bit-exactness proof: the accumulation order inside each helper is
 * identical to the original monolithic loop body — no reordering of
 * _mm512_add_epi32 operands, no change to shift constants. The only
 * structural difference is ABI call/return overhead, which is pure
 * integer traffic and has no effect on the integer SIMD results.
 * Verified by meson test -C build --suite=fast + Netflix golden gate
 * (python/test/quality_runner_test.py).
 */

/* Filter-coefficient constants shared across per-row vertical calls. */
typedef struct VifVertCoeffs8 {
    __m512i f0;
    __m512i f1;
    __m512i f2;
    __m512i f3;
    __m512i f4;
    __m512i mask2;
    __m512i mask3;
    __m512i x;
} VifVertCoeffs8;

/* Filter-coefficient constants shared across per-row horizontal calls. */
typedef struct VifHorizCoeffs8 {
    __m512i fcoeff;
    __m512i fcoeff1;
    __m512i fcoeff2;
    __m512i fcoeff3;
    __m512i fcoeff4;
    __m512i addnum;
    __m512i mask1;
} VifHorizCoeffs8;

/*
 * Vertical-pass inner j-iteration: load 10 rows of ref/dis pixels starting
 * at row `ii` and column `j`, apply the 9-tap separable filter in the
 * vertical direction, and store 32 filtered ref and 32 filtered dis results
 * into ref_convol[j..j+31] and dis_convol[j..j+31].
 *
 * The accumulation order (s0/s1 via f0, s2/s3 via f1, …, g0/g1 via f0, …)
 * is identical to the original monolithic loop (ADR-0138 / ADR-0139).
 */
/* NOLINTNEXTLINE(readability-function-size): ADR-0503 noinline helper; size is load-bearing for register-pressure isolation */
static VMAF_NOINLINE_NOCLONE void vif_subsample_rd_8_vert_j(const uint8_t *ref, const uint8_t *dis,
                                                            ptrdiff_t stride_bytes, int ii, int j,
                                                            const VifVertCoeffs8 *c,
                                                            uint32_t *ref_convol,
                                                            uint32_t *dis_convol)
{
    int ii_check = ii;
    __m512i accum_mu2_lo;
    __m512i accum_mu1_lo;
    __m512i accum_mu2_hi;
    __m512i accum_mu1_hi;
    accum_mu2_lo = accum_mu2_hi = accum_mu1_lo = accum_mu1_hi = _mm512_setzero_si512();

    {
        __m512i g0 = _mm512_cvtepu8_epi16(
            _mm256_loadu_si256((__m256i *)(ref + (stride_bytes * ii_check) + j)));
        __m512i g1 = _mm512_cvtepu8_epi16(
            _mm256_loadu_si256((__m256i *)(ref + stride_bytes * (ii_check) + stride_bytes + j)));
        __m512i g2 = _mm512_cvtepu8_epi16(
            _mm256_loadu_si256((__m256i *)(ref + stride_bytes * (ii_check + 2) + j)));
        __m512i g3 = _mm512_cvtepu8_epi16(
            _mm256_loadu_si256((__m256i *)(ref + stride_bytes * (ii_check + 3) + j)));
        __m512i g4 = _mm512_cvtepu8_epi16(
            _mm256_loadu_si256((__m256i *)(ref + stride_bytes * (ii_check + 4) + j)));
        __m512i g5 = _mm512_cvtepu8_epi16(
            _mm256_loadu_si256((__m256i *)(ref + stride_bytes * (ii_check + 5) + j)));
        __m512i g6 = _mm512_cvtepu8_epi16(
            _mm256_loadu_si256((__m256i *)(ref + stride_bytes * (ii_check + 6) + j)));
        __m512i g7 = _mm512_cvtepu8_epi16(
            _mm256_loadu_si256((__m256i *)(ref + stride_bytes * (ii_check + 7) + j)));
        __m512i g8 = _mm512_cvtepu8_epi16(
            _mm256_loadu_si256((__m256i *)(ref + stride_bytes * (ii_check + 8) + j)));
        __m512i g9 = _mm512_cvtepu8_epi16(
            _mm256_loadu_si256((__m256i *)(ref + stride_bytes * (ii_check + 9) + j)));

        __m512i s0 = _mm512_cvtepu8_epi16(
            _mm256_loadu_si256((__m256i *)(dis + (stride_bytes * ii_check) + j)));
        __m512i s1 = _mm512_cvtepu8_epi16(
            _mm256_loadu_si256((__m256i *)(dis + stride_bytes * (ii_check + 1) + j)));
        __m512i s2 = _mm512_cvtepu8_epi16(
            _mm256_loadu_si256((__m256i *)(dis + stride_bytes * (ii_check + 2) + j)));
        __m512i s3 = _mm512_cvtepu8_epi16(
            _mm256_loadu_si256((__m256i *)(dis + stride_bytes * (ii_check + 3) + j)));
        __m512i s4 = _mm512_cvtepu8_epi16(
            _mm256_loadu_si256((__m256i *)(dis + stride_bytes * (ii_check + 4) + j)));
        __m512i s5 = _mm512_cvtepu8_epi16(
            _mm256_loadu_si256((__m256i *)(dis + stride_bytes * (ii_check + 5) + j)));
        __m512i s6 = _mm512_cvtepu8_epi16(
            _mm256_loadu_si256((__m256i *)(dis + stride_bytes * (ii_check + 6) + j)));
        __m512i s7 = _mm512_cvtepu8_epi16(
            _mm256_loadu_si256((__m256i *)(dis + stride_bytes * (ii_check + 7) + j)));
        __m512i s8 = _mm512_cvtepu8_epi16(
            _mm256_loadu_si256((__m256i *)(dis + stride_bytes * (ii_check + 8) + j)));
        __m512i s9 = _mm512_cvtepu8_epi16(
            _mm256_loadu_si256((__m256i *)(dis + stride_bytes * (ii_check + 9) + j)));

        __m512i s0lo = _mm512_unpacklo_epi16(s0, s1);
        __m512i s0hi = _mm512_unpackhi_epi16(s0, s1);
        accum_mu2_lo = _mm512_add_epi32(accum_mu2_lo, _mm512_madd_epi16(s0lo, c->f0));
        accum_mu2_hi = _mm512_add_epi32(accum_mu2_hi, _mm512_madd_epi16(s0hi, c->f0));
        __m512i s1lo = _mm512_unpacklo_epi16(s2, s3);
        __m512i s1hi = _mm512_unpackhi_epi16(s2, s3);
        accum_mu2_lo = _mm512_add_epi32(accum_mu2_lo, _mm512_madd_epi16(s1lo, c->f1));
        accum_mu2_hi = _mm512_add_epi32(accum_mu2_hi, _mm512_madd_epi16(s1hi, c->f1));
        __m512i s2lo = _mm512_unpacklo_epi16(s4, s5);
        __m512i s2hi = _mm512_unpackhi_epi16(s4, s5);
        accum_mu2_lo = _mm512_add_epi32(accum_mu2_lo, _mm512_madd_epi16(s2lo, c->f2));
        accum_mu2_hi = _mm512_add_epi32(accum_mu2_hi, _mm512_madd_epi16(s2hi, c->f2));
        __m512i s3lo = _mm512_unpacklo_epi16(s6, s7);
        __m512i s3hi = _mm512_unpackhi_epi16(s6, s7);
        accum_mu2_lo = _mm512_add_epi32(accum_mu2_lo, _mm512_madd_epi16(s3lo, c->f3));
        accum_mu2_hi = _mm512_add_epi32(accum_mu2_hi, _mm512_madd_epi16(s3hi, c->f3));
        __m512i s4lo = _mm512_unpacklo_epi16(s8, s9);
        __m512i s4hi = _mm512_unpackhi_epi16(s8, s9);
        accum_mu2_lo = _mm512_add_epi32(accum_mu2_lo, _mm512_madd_epi16(s4lo, c->f4));
        accum_mu2_hi = _mm512_add_epi32(accum_mu2_hi, _mm512_madd_epi16(s4hi, c->f4));

        __m512i g0lo = _mm512_unpacklo_epi16(g0, g1);
        __m512i g0hi = _mm512_unpackhi_epi16(g0, g1);
        accum_mu1_lo = _mm512_add_epi32(accum_mu1_lo, _mm512_madd_epi16(g0lo, c->f0));
        accum_mu1_hi = _mm512_add_epi32(accum_mu1_hi, _mm512_madd_epi16(g0hi, c->f0));
        __m512i g1lo = _mm512_unpacklo_epi16(g2, g3);
        __m512i g1hi = _mm512_unpackhi_epi16(g2, g3);
        accum_mu1_lo = _mm512_add_epi32(accum_mu1_lo, _mm512_madd_epi16(g1lo, c->f1));
        accum_mu1_hi = _mm512_add_epi32(accum_mu1_hi, _mm512_madd_epi16(g1hi, c->f1));
        __m512i g2lo = _mm512_unpacklo_epi16(g4, g5);
        __m512i g2hi = _mm512_unpackhi_epi16(g4, g5);
        accum_mu1_lo = _mm512_add_epi32(accum_mu1_lo, _mm512_madd_epi16(g2lo, c->f2));
        accum_mu1_hi = _mm512_add_epi32(accum_mu1_hi, _mm512_madd_epi16(g2hi, c->f2));
        __m512i g3lo = _mm512_unpacklo_epi16(g6, g7);
        __m512i g3hi = _mm512_unpackhi_epi16(g6, g7);
        accum_mu1_lo = _mm512_add_epi32(accum_mu1_lo, _mm512_madd_epi16(g3lo, c->f3));
        accum_mu1_hi = _mm512_add_epi32(accum_mu1_hi, _mm512_madd_epi16(g3hi, c->f3));
        __m512i g4lo = _mm512_unpacklo_epi16(g8, g9);
        __m512i g4hi = _mm512_unpackhi_epi16(g8, g9);
        accum_mu1_lo = _mm512_add_epi32(accum_mu1_lo, _mm512_madd_epi16(g4lo, c->f4));
        accum_mu1_hi = _mm512_add_epi32(accum_mu1_hi, _mm512_madd_epi16(g4hi, c->f4));
    }

    __m512i accumu1_lo =
        _mm512_add_epi32(c->x, _mm512_permutex2var_epi64(accum_mu1_lo, c->mask2, accum_mu1_hi));
    __m512i accumu1_hi =
        _mm512_add_epi32(c->x, _mm512_permutex2var_epi64(accum_mu1_lo, c->mask3, accum_mu1_hi));
    __m512i accumu2_lo =
        _mm512_add_epi32(c->x, _mm512_permutex2var_epi64(accum_mu2_lo, c->mask2, accum_mu2_hi));
    __m512i accumu2_hi =
        _mm512_add_epi32(c->x, _mm512_permutex2var_epi64(accum_mu2_lo, c->mask3, accum_mu2_hi));
    accumu1_lo = _mm512_srli_epi32(accumu1_lo, 0x08);
    accumu1_hi = _mm512_srli_epi32(accumu1_hi, 0x08);
    accumu2_lo = _mm512_srli_epi32(accumu2_lo, 0x08);
    accumu2_hi = _mm512_srli_epi32(accumu2_hi, 0x08);
    _mm512_storeu_si512((__m512i *)(ref_convol + j), accumu1_lo);
    _mm512_storeu_si512((__m512i *)(ref_convol + j + 16), accumu1_hi);
    _mm512_storeu_si512((__m512i *)(dis_convol + j), accumu2_lo);
    _mm512_storeu_si512((__m512i *)(dis_convol + j + 16), accumu2_hi);
}

/*
 * Horizontal-pass inner j-iteration: read 9 overlapping 512-bit windows of
 * ref_convol and dis_convol starting at jj_check, apply the 9-tap horizontal
 * filter, and store 16 output pixels each into mu1[out_j] and mu2[out_j].
 *
 * The accumulation order (refconvol via fcoeff, refconvol1 via fcoeff1, …)
 * is identical to the original monolithic loop (ADR-0138 / ADR-0139).
 */
/* NOLINTNEXTLINE(readability-function-size): ADR-0503 noinline helper; size is load-bearing for register-pressure isolation */
static VMAF_NOINLINE_NOCLONE void vif_subsample_rd_8_horiz_j(const uint32_t *ref_convol,
                                                             const uint32_t *dis_convol,
                                                             int jj_check, const VifHorizCoeffs8 *c,
                                                             uint16_t *mu1_out, uint16_t *mu2_out)
{
    __m512i accumrlo = _mm512_setzero_si512();
    __m512i accumdlo = _mm512_setzero_si512();
    __m512i accumrhi = _mm512_setzero_si512();
    __m512i accumdhi = _mm512_setzero_si512();

    /* ADR-0503: process ref and dis interleaved per tap — keeps at most 2 data
     * ZMMs live at a time (the current ref/dis pair) instead of 9+9, reducing
     * peak live-set from ~30 to ~13 ZMMs (4 accum + 7 const + 2 data). The
     * accumulation order for each accumulator is identical to the original: tap
     * 0 (fcoeff), tap 1 (fcoeff1), …, tap 8 (fcoeff). ADR-0138 / ADR-0139. */
    {
        __m512i rv;
        __m512i rlo;
        __m512i dv;
        __m512i dlo;

        rv = _mm512_loadu_si512((__m512i *)(ref_convol + jj_check));
        dv = _mm512_loadu_si512((__m512i *)(dis_convol + jj_check));
        rlo = _mm512_mullo_epi16(rv, c->fcoeff);
        rv = _mm512_mulhi_epu16(rv, c->fcoeff);
        accumrlo = _mm512_add_epi32(accumrlo, _mm512_unpacklo_epi16(rlo, rv));
        accumrhi = _mm512_add_epi32(accumrhi, _mm512_unpackhi_epi16(rlo, rv));
        dlo = _mm512_mullo_epi16(dv, c->fcoeff);
        dv = _mm512_mulhi_epu16(dv, c->fcoeff);
        accumdlo = _mm512_add_epi32(accumdlo, _mm512_unpacklo_epi16(dlo, dv));
        accumdhi = _mm512_add_epi32(accumdhi, _mm512_unpackhi_epi16(dlo, dv));

        rv = _mm512_loadu_si512((__m512i *)(ref_convol + jj_check + 1));
        dv = _mm512_loadu_si512((__m512i *)(dis_convol + jj_check + 1));
        rlo = _mm512_mullo_epi16(rv, c->fcoeff1);
        rv = _mm512_mulhi_epu16(rv, c->fcoeff1);
        accumrlo = _mm512_add_epi32(accumrlo, _mm512_unpacklo_epi16(rlo, rv));
        accumrhi = _mm512_add_epi32(accumrhi, _mm512_unpackhi_epi16(rlo, rv));
        dlo = _mm512_mullo_epi16(dv, c->fcoeff1);
        dv = _mm512_mulhi_epu16(dv, c->fcoeff1);
        accumdlo = _mm512_add_epi32(accumdlo, _mm512_unpacklo_epi16(dlo, dv));
        accumdhi = _mm512_add_epi32(accumdhi, _mm512_unpackhi_epi16(dlo, dv));

        rv = _mm512_loadu_si512((__m512i *)(ref_convol + jj_check + 2));
        dv = _mm512_loadu_si512((__m512i *)(dis_convol + jj_check + 2));
        rlo = _mm512_mullo_epi16(rv, c->fcoeff2);
        rv = _mm512_mulhi_epu16(rv, c->fcoeff2);
        accumrlo = _mm512_add_epi32(accumrlo, _mm512_unpacklo_epi16(rlo, rv));
        accumrhi = _mm512_add_epi32(accumrhi, _mm512_unpackhi_epi16(rlo, rv));
        dlo = _mm512_mullo_epi16(dv, c->fcoeff2);
        dv = _mm512_mulhi_epu16(dv, c->fcoeff2);
        accumdlo = _mm512_add_epi32(accumdlo, _mm512_unpacklo_epi16(dlo, dv));
        accumdhi = _mm512_add_epi32(accumdhi, _mm512_unpackhi_epi16(dlo, dv));

        rv = _mm512_loadu_si512((__m512i *)(ref_convol + jj_check + 3));
        dv = _mm512_loadu_si512((__m512i *)(dis_convol + jj_check + 3));
        rlo = _mm512_mullo_epi16(rv, c->fcoeff3);
        rv = _mm512_mulhi_epu16(rv, c->fcoeff3);
        accumrlo = _mm512_add_epi32(accumrlo, _mm512_unpacklo_epi16(rlo, rv));
        accumrhi = _mm512_add_epi32(accumrhi, _mm512_unpackhi_epi16(rlo, rv));
        dlo = _mm512_mullo_epi16(dv, c->fcoeff3);
        dv = _mm512_mulhi_epu16(dv, c->fcoeff3);
        accumdlo = _mm512_add_epi32(accumdlo, _mm512_unpacklo_epi16(dlo, dv));
        accumdhi = _mm512_add_epi32(accumdhi, _mm512_unpackhi_epi16(dlo, dv));

        rv = _mm512_loadu_si512((__m512i *)(ref_convol + jj_check + 4));
        dv = _mm512_loadu_si512((__m512i *)(dis_convol + jj_check + 4));
        rlo = _mm512_mullo_epi16(rv, c->fcoeff4);
        rv = _mm512_mulhi_epu16(rv, c->fcoeff4);
        accumrlo = _mm512_add_epi32(accumrlo, _mm512_unpacklo_epi16(rlo, rv));
        accumrhi = _mm512_add_epi32(accumrhi, _mm512_unpackhi_epi16(rlo, rv));
        dlo = _mm512_mullo_epi16(dv, c->fcoeff4);
        dv = _mm512_mulhi_epu16(dv, c->fcoeff4);
        accumdlo = _mm512_add_epi32(accumdlo, _mm512_unpacklo_epi16(dlo, dv));
        accumdhi = _mm512_add_epi32(accumdhi, _mm512_unpackhi_epi16(dlo, dv));

        rv = _mm512_loadu_si512((__m512i *)(ref_convol + jj_check + 5));
        dv = _mm512_loadu_si512((__m512i *)(dis_convol + jj_check + 5));
        rlo = _mm512_mullo_epi16(rv, c->fcoeff3);
        rv = _mm512_mulhi_epu16(rv, c->fcoeff3);
        accumrlo = _mm512_add_epi32(accumrlo, _mm512_unpacklo_epi16(rlo, rv));
        accumrhi = _mm512_add_epi32(accumrhi, _mm512_unpackhi_epi16(rlo, rv));
        dlo = _mm512_mullo_epi16(dv, c->fcoeff3);
        dv = _mm512_mulhi_epu16(dv, c->fcoeff3);
        accumdlo = _mm512_add_epi32(accumdlo, _mm512_unpacklo_epi16(dlo, dv));
        accumdhi = _mm512_add_epi32(accumdhi, _mm512_unpackhi_epi16(dlo, dv));

        rv = _mm512_loadu_si512((__m512i *)(ref_convol + jj_check + 6));
        dv = _mm512_loadu_si512((__m512i *)(dis_convol + jj_check + 6));
        rlo = _mm512_mullo_epi16(rv, c->fcoeff2);
        rv = _mm512_mulhi_epu16(rv, c->fcoeff2);
        accumrlo = _mm512_add_epi32(accumrlo, _mm512_unpacklo_epi16(rlo, rv));
        accumrhi = _mm512_add_epi32(accumrhi, _mm512_unpackhi_epi16(rlo, rv));
        dlo = _mm512_mullo_epi16(dv, c->fcoeff2);
        dv = _mm512_mulhi_epu16(dv, c->fcoeff2);
        accumdlo = _mm512_add_epi32(accumdlo, _mm512_unpacklo_epi16(dlo, dv));
        accumdhi = _mm512_add_epi32(accumdhi, _mm512_unpackhi_epi16(dlo, dv));

        rv = _mm512_loadu_si512((__m512i *)(ref_convol + jj_check + 7));
        dv = _mm512_loadu_si512((__m512i *)(dis_convol + jj_check + 7));
        rlo = _mm512_mullo_epi16(rv, c->fcoeff1);
        rv = _mm512_mulhi_epu16(rv, c->fcoeff1);
        accumrlo = _mm512_add_epi32(accumrlo, _mm512_unpacklo_epi16(rlo, rv));
        accumrhi = _mm512_add_epi32(accumrhi, _mm512_unpackhi_epi16(rlo, rv));
        dlo = _mm512_mullo_epi16(dv, c->fcoeff1);
        dv = _mm512_mulhi_epu16(dv, c->fcoeff1);
        accumdlo = _mm512_add_epi32(accumdlo, _mm512_unpacklo_epi16(dlo, dv));
        accumdhi = _mm512_add_epi32(accumdhi, _mm512_unpackhi_epi16(dlo, dv));

        rv = _mm512_loadu_si512((__m512i *)(ref_convol + jj_check + 8));
        dv = _mm512_loadu_si512((__m512i *)(dis_convol + jj_check + 8));
        rlo = _mm512_mullo_epi16(rv, c->fcoeff);
        rv = _mm512_mulhi_epu16(rv, c->fcoeff);
        accumrlo = _mm512_add_epi32(accumrlo, _mm512_unpacklo_epi16(rlo, rv));
        accumrhi = _mm512_add_epi32(accumrhi, _mm512_unpackhi_epi16(rlo, rv));
        dlo = _mm512_mullo_epi16(dv, c->fcoeff);
        dv = _mm512_mulhi_epu16(dv, c->fcoeff);
        accumdlo = _mm512_add_epi32(accumdlo, _mm512_unpacklo_epi16(dlo, dv));
        accumdhi = _mm512_add_epi32(accumdhi, _mm512_unpackhi_epi16(dlo, dv));
    }

    accumdlo = _mm512_add_epi32(accumdlo, c->addnum);
    accumdhi = _mm512_add_epi32(accumdhi, c->addnum);
    accumrlo = _mm512_add_epi32(accumrlo, c->addnum);
    accumrhi = _mm512_add_epi32(accumrhi, c->addnum);
    accumdlo = _mm512_srli_epi32(accumdlo, 0x10);
    accumdhi = _mm512_srli_epi32(accumdhi, 0x10);
    accumrlo = _mm512_srli_epi32(accumrlo, 0x10);
    accumrhi = _mm512_srli_epi32(accumrhi, 0x10);

    __m512i result = _mm512_permutex2var_epi16(accumdlo, c->mask1, accumdhi);
    __m512i resultd = _mm512_permutex2var_epi16(accumrlo, c->mask1, accumrhi);

    _mm256_storeu_si256((__m256i *)mu1_out, _mm512_castsi512_si256(resultd));
    _mm256_storeu_si256((__m256i *)mu2_out, _mm512_castsi512_si256(result));
}

static FORCE_INLINE VifVertCoeffs8 vif_subsample_vert_coeffs8(const uint16_t *vif_filt_s1)
{
    VifVertCoeffs8 vc;
    vc.f0 = _mm512_broadcastd_epi32(_mm_loadu_si128((__m128i *)vif_filt_s1));
    vc.f1 = _mm512_broadcastd_epi32(_mm_loadu_si128((__m128i *)(vif_filt_s1 + 2)));
    vc.f2 = _mm512_broadcastd_epi32(_mm_loadu_si128((__m128i *)(vif_filt_s1 + 4)));
    vc.f3 = _mm512_broadcastd_epi32(_mm_loadu_si128((__m128i *)(vif_filt_s1 + 6)));
    vc.f4 = _mm512_broadcastd_epi32(_mm_loadu_si128((__m128i *)(vif_filt_s1 + 8)));
    vc.mask2 = _mm512_set_epi64(11, 10, 3, 2, 9, 8, 1, 0);
    vc.mask3 = _mm512_set_epi64(15, 14, 7, 6, 13, 12, 5, 4);
    vc.x = _mm512_set1_epi32(128);
    return vc;
}

static FORCE_INLINE VifHorizCoeffs8 vif_subsample_horiz_coeffs8(const uint16_t *vif_filt_s1)
{
    const int M = 1 << 16;
    VifHorizCoeffs8 hc;
    hc.fcoeff = _mm512_broadcastw_epi16(_mm_loadu_si128((__m128i *)vif_filt_s1));
    hc.fcoeff1 = _mm512_broadcastw_epi16(_mm_loadu_si128((__m128i *)(vif_filt_s1 + 1)));
    hc.fcoeff2 = _mm512_broadcastw_epi16(_mm_loadu_si128((__m128i *)(vif_filt_s1 + 2)));
    hc.fcoeff3 = _mm512_broadcastw_epi16(_mm_loadu_si128((__m128i *)(vif_filt_s1 + 3)));
    hc.fcoeff4 = _mm512_broadcastw_epi16(_mm_loadu_si128((__m128i *)(vif_filt_s1 + 4)));
    hc.addnum = _mm512_set1_epi32(32768);
    hc.mask1 =
        _mm512_set_epi32(60 * M + 56, 28 * M + 24, 52 * M + 48, 20 * M + 16, 44 * M + 40,
                         12 * M + 8, 36 * M + 32, 4 * M + 0, 60 * M + 56, 28 * M + 24, 52 * M + 48,
                         20 * M + 16, 44 * M + 40, 12 * M + 8, 36 * M + 32, 4 * M + 0);
    return hc;
}

static FORCE_INLINE void vif_subsample_vertical8(const VifBuffer *buf, unsigned w, unsigned i,
                                                 const VifVertCoeffs8 *vc)
{
    const unsigned fwidth = vif_filter1d_width[1];
    const uint16_t *vif_filt_s1 = vif_filter1d_table[1];
    const uint8_t *ref = buf->ref;
    const uint8_t *dis = buf->dis;
    const int fwidth_half = fwidth >> 1;
    //VERTICAL
    int n = w >> 5;
    int ii = (int)i - fwidth_half;
    for (int j = 0; j < n << 5; j = j + 32) {
        vif_subsample_rd_8_vert_j(ref, dis, buf->stride, ii, j, vc, buf->tmp.ref_convol,
                                  buf->tmp.dis_convol);
    }
    for (unsigned j = n << 5; j < w; ++j) {
        uint32_t accum_ref = 0;
        uint32_t accum_dis = 0;
        for (unsigned fi = 0; fi < fwidth; ++fi) {
            int ii_check = ii + fi;
            const uint16_t fcoeff_scalar = vif_filt_s1[fi];
            accum_ref += fcoeff_scalar * (uint32_t)ref[ii_check * buf->stride + j];
            accum_dis += fcoeff_scalar * (uint32_t)dis[ii_check * buf->stride + j];
        }
        buf->tmp.ref_convol[j] = (accum_ref + 128) >> 8;
        buf->tmp.dis_convol[j] = (accum_dis + 128) >> 8;
    }
}

static FORCE_INLINE void vif_subsample_horizontal8(const VifBuffer *buf, unsigned w, unsigned i,
                                                   const VifHorizCoeffs8 *hc)
{
    const unsigned fwidth = vif_filter1d_width[1];
    const uint16_t *vif_filt_s1 = vif_filter1d_table[1];
    const ptrdiff_t stride = buf->stride_16 / sizeof(uint16_t);
    const int fwidth_half = fwidth >> 1;
    //HORIZONTAL
    int n = w >> 4;
    for (int j = 0; j < n << 4; j = j + 16) {
        int jj = j - fwidth_half;
        int jj_check = jj;
        vif_subsample_rd_8_horiz_j(buf->tmp.ref_convol, buf->tmp.dis_convol, jj_check, hc,
                                   buf->mu1 + i * stride + j, buf->mu2 + i * stride + j);
    }

    for (unsigned j = n << 4; j < w; ++j) {
        uint32_t accum_ref = 0;
        uint32_t accum_dis = 0;
        int jj = (int)j - fwidth_half;
        int jj_check = jj;
        for (unsigned fj = 0; fj < fwidth; ++fj, jj_check = jj + fj) {
            const uint16_t fcoeff_scalar = vif_filt_s1[fj];
            accum_ref += fcoeff_scalar * buf->tmp.ref_convol[jj_check];
            accum_dis += fcoeff_scalar * buf->tmp.dis_convol[jj_check];
        }
        buf->mu1[i * stride + j] = (uint16_t)((accum_ref + 32768) >> 16);
        buf->mu2[i * stride + j] = (uint16_t)((accum_dis + 32768) >> 16);
    }
}

/* Read padded 8-bit ref/dis planes, filter into full-width mu1/mu2 rows,
 * then decimate to half-resolution uint16_t ref/dis planes and reflect padding.
 * Research-2046 preserves the per-accumulator order; ADR-0503 keeps the two
 * noinline block boundaries that isolate register pressure. */
void vif_subsample_rd_8_avx512(const VifBuffer *buf, unsigned w, unsigned h)
{
    assert(buf != NULL);
    assert(w > 0u);
    assert(h > 0u);
    const unsigned fwidth = vif_filter1d_width[1];
    const uint16_t *vif_filt_s1 = vif_filter1d_table[1];
    /* ADR-0503: retain separate coefficient sets and the noinline vertical /
     * horizontal block calls. Private stages inline into this original caller. */
    const VifVertCoeffs8 vc = vif_subsample_vert_coeffs8(vif_filt_s1);
    const VifHorizCoeffs8 hc = vif_subsample_horiz_coeffs8(vif_filt_s1);
    const int fwidth_half = fwidth >> 1;
    for (unsigned i = 0; i < h; ++i) {
        vif_subsample_vertical8(buf, w, i, &vc);
        PADDING_SQ_DATA_2(buf, w, fwidth_half);
        vif_subsample_horizontal8(buf, w, i, &hc);
    }
    decimate_and_pad(buf, w, h, 0);
}

typedef struct VifSubsample16 {
    const VifBuffer *buf;
    unsigned fwidth;
    const uint16_t *vif_filt;
    int32_t add_shift_round_VP;
    int32_t shift_VP;
    int fwidth_half;
    ptrdiff_t stride;
    ptrdiff_t stride16;
    const uint16_t *ref;
    const uint16_t *dis;
} VifSubsample16;

static FORCE_INLINE void vif_subsample_vertical16_store(const VifSubsample16 *c, int j,
                                                        __m512i accumr_lo, __m512i accumr_hi,
                                                        __m512i accumd_lo, __m512i accumd_hi)
{
    const __m512i mask3 = _mm512_set_epi64(11, 10, 3, 2, 9, 8, 1, 0);
    const __m512i mask4 = _mm512_set_epi64(15, 14, 7, 6, 13, 12, 5, 4);
    __m512i addnum = _mm512_set1_epi32(c->add_shift_round_VP);
    accumr_lo = _mm512_add_epi32(accumr_lo, addnum);
    accumr_hi = _mm512_add_epi32(accumr_hi, addnum);
    accumr_lo = _mm512_srli_epi32(accumr_lo, c->shift_VP);
    accumr_hi = _mm512_srli_epi32(accumr_hi, c->shift_VP);

    _mm512_storeu_si512((__m512i *)(c->buf->tmp.ref_convol + j),
                        _mm512_permutex2var_epi64(accumr_lo, mask3, accumr_hi));
    _mm512_storeu_si512((__m512i *)(c->buf->tmp.ref_convol + j + 16),
                        _mm512_permutex2var_epi64(accumr_lo, mask4, accumr_hi));

    accumd_lo = _mm512_add_epi32(accumd_lo, addnum);
    accumd_hi = _mm512_add_epi32(accumd_hi, addnum);
    accumd_lo = _mm512_srli_epi32(accumd_lo, c->shift_VP);
    accumd_hi = _mm512_srli_epi32(accumd_hi, c->shift_VP);
    _mm512_storeu_si512((__m512i *)(c->buf->tmp.dis_convol + j),
                        _mm512_permutex2var_epi64(accumd_lo, mask3, accumd_hi));
    _mm512_storeu_si512((__m512i *)(c->buf->tmp.dis_convol + j + 16),
                        _mm512_permutex2var_epi64(accumd_lo, mask4, accumd_hi));
}

static FORCE_INLINE void vif_subsample_vertical16_block(const VifSubsample16 *c, int ii, int j)
{
    int ii_check = ii;
    __m512i accumr_lo;
    __m512i accumr_hi;
    __m512i accumd_lo;
    __m512i accumd_hi;
    accumr_lo = accumr_hi = accumd_lo = accumd_hi = _mm512_setzero_si512();
    for (unsigned fi = 0; fi < c->fwidth; ++fi, ii_check = ii + fi) {

        const uint16_t fcoeff = c->vif_filt[fi];
        __m512i f1 = _mm512_set1_epi16(fcoeff);
        __m512i ref1 = _mm512_loadu_si512((__m512i *)(c->ref + (ii_check * c->stride) + j));
        __m512i dis1 = _mm512_loadu_si512((__m512i *)(c->dis + (ii_check * c->stride) + j));
        __m512i result2 = _mm512_mulhi_epu16(ref1, f1);
        __m512i result2lo = _mm512_mullo_epi16(ref1, f1);
        const __m512i rmul1 = _mm512_unpacklo_epi16(result2lo, result2);
        const __m512i rmul2 = _mm512_unpackhi_epi16(result2lo, result2);
        accumr_lo = _mm512_add_epi32(accumr_lo, rmul1);
        accumr_hi = _mm512_add_epi32(accumr_hi, rmul2);

        __m512i d0 = _mm512_mulhi_epu16(dis1, f1);
        __m512i d0lo = _mm512_mullo_epi16(dis1, f1);
        const __m512i dmul1 = _mm512_unpacklo_epi16(d0lo, d0);
        const __m512i dmul2 = _mm512_unpackhi_epi16(d0lo, d0);
        accumd_lo = _mm512_add_epi32(accumd_lo, dmul1);
        accumd_hi = _mm512_add_epi32(accumd_hi, dmul2);
    }
    vif_subsample_vertical16_store(c, j, accumr_lo, accumr_hi, accumd_lo, accumd_hi);
}

static FORCE_INLINE void vif_subsample_vertical16(const VifSubsample16 *c, unsigned w, unsigned i)
{
    const int n = w >> 4;
    const int ii = (int)i - c->fwidth_half;
    for (int j = 0; j < n << 4; j += 32) {
        vif_subsample_vertical16_block(c, ii, j);
    }
    for (unsigned j = n << 4; j < w; ++j) {
        uint32_t accum_ref = 0;
        uint32_t accum_dis = 0;
        int ii_check = ii;
        for (unsigned fi = 0; fi < c->fwidth; ++fi, ii_check = ii + fi) {
            const uint16_t fcoeff = c->vif_filt[fi];
            accum_ref += fcoeff * ((uint32_t)c->ref[ii_check * c->stride + j]);
            accum_dis += fcoeff * ((uint32_t)c->dis[ii_check * c->stride + j]);
        }
        c->buf->tmp.ref_convol[j] = (uint16_t)((accum_ref + c->add_shift_round_VP) >> c->shift_VP);
        c->buf->tmp.dis_convol[j] = (uint16_t)((accum_dis + c->add_shift_round_VP) >> c->shift_VP);
    }
}

static FORCE_INLINE void vif_subsample_horizontal16_block(const VifSubsample16 *c, unsigned i,
                                                          int j)
{
    int jj = j - c->fwidth_half;
    int jj_check = jj;
    __m512i accumrlo;
    __m512i accumdlo;
    __m512i accumrhi;
    __m512i accumdhi;
    accumrlo = accumdlo = accumrhi = accumdhi = _mm512_setzero_si512();
    for (unsigned fj = 0; fj < c->fwidth; ++fj, jj_check = jj + fj) {

        __m512i refconvol = _mm512_loadu_si512((__m512i *)(c->buf->tmp.ref_convol + jj_check));
        __m512i fcoeff = _mm512_set1_epi16(c->vif_filt[fj]);
        __m512i result2 = _mm512_mulhi_epu16(refconvol, fcoeff);
        __m512i result2lo = _mm512_mullo_epi16(refconvol, fcoeff);
        accumrlo = _mm512_add_epi32(accumrlo, _mm512_unpacklo_epi16(result2lo, result2));
        accumrhi = _mm512_add_epi32(accumrhi, _mm512_unpackhi_epi16(result2lo, result2));
        __m512i disconvol = _mm512_loadu_si512((__m512i *)(c->buf->tmp.dis_convol + jj_check));
        result2 = _mm512_mulhi_epu16(disconvol, fcoeff);
        result2lo = _mm512_mullo_epi16(disconvol, fcoeff);
        accumdlo = _mm512_add_epi32(accumdlo, _mm512_unpacklo_epi16(result2lo, result2));
        accumdhi = _mm512_add_epi32(accumdhi, _mm512_unpackhi_epi16(result2lo, result2));
    }

    __m512i addnum = _mm512_set1_epi32(32768);
    accumdlo = _mm512_add_epi32(accumdlo, addnum);
    accumdhi = _mm512_add_epi32(accumdhi, addnum);
    accumrlo = _mm512_add_epi32(accumrlo, addnum);
    accumrhi = _mm512_add_epi32(accumrhi, addnum);
    accumdlo = _mm512_srli_epi32(accumdlo, 0x10);
    accumdhi = _mm512_srli_epi32(accumdhi, 0x10);
    accumrlo = _mm512_srli_epi32(accumrlo, 0x10);
    accumrhi = _mm512_srli_epi32(accumrhi, 0x10);

    const int M = 1 << 16;
    __m512i mask2 =
        _mm512_set_epi32(60 * M + 56, 28 * M + 24, 52 * M + 48, 20 * M + 16, 44 * M + 40,
                         12 * M + 8, 36 * M + 32, 4 * M + 0, 60 * M + 56, 28 * M + 24, 52 * M + 48,
                         20 * M + 16, 44 * M + 40, 12 * M + 8, 36 * M + 32, 4 * M + 0);

    _mm256_storeu_si256(
        (__m256i *)(c->buf->mu1 + (c->stride16 * i) + j),
        _mm512_castsi512_si256(_mm512_permutex2var_epi16(accumrlo, mask2, accumrhi)));
    _mm256_storeu_si256(
        (__m256i *)(c->buf->mu2 + (c->stride16 * i) + j),
        _mm512_castsi512_si256(_mm512_permutex2var_epi16(accumdlo, mask2, accumdhi)));
}

static FORCE_INLINE void vif_subsample_horizontal16(const VifSubsample16 *c, unsigned w, unsigned i)
{
    const int n = w >> 4;
    for (int j = 0; j < n << 4; j += 16) {
        vif_subsample_horizontal16_block(c, i, j);
    }
    for (unsigned j = n << 4; j < w; ++j) {
        uint32_t accum_ref = 0;
        uint32_t accum_dis = 0;
        int jj = (int)j - c->fwidth_half;
        int jj_check = jj;
        for (unsigned fj = 0; fj < c->fwidth; ++fj, jj_check = jj + fj) {
            const uint16_t fcoeff = c->vif_filt[fj];
            accum_ref += fcoeff * ((uint32_t)c->buf->tmp.ref_convol[jj_check]);
            accum_dis += fcoeff * ((uint32_t)c->buf->tmp.dis_convol[jj_check]);
        }
        c->buf->mu1[i * c->stride16 + j] = (uint16_t)((accum_ref + 32768) >> 16);
        c->buf->mu2[i * c->stride16 + j] = (uint16_t)((accum_dis + 32768) >> 16);
    }
}

void vif_subsample_rd_16_avx512(const VifBuffer *buf, unsigned w, unsigned h, int scale, int bpc)
{
    assert(buf != NULL);
    assert(w > 0u);
    assert(h > 0u);
    VifSubsample16 c;
    c.buf = buf;
    c.fwidth = vif_filter1d_width[scale + 1];
    c.vif_filt = vif_filter1d_table[scale + 1];
    c.fwidth_half = c.fwidth >> 1;
    c.stride = buf->stride / sizeof(uint16_t);
    c.stride16 = buf->stride_16 / sizeof(uint16_t);
    c.ref = buf->ref;
    c.dis = buf->dis;
    if (scale == 0) {
        c.add_shift_round_VP = 1 << (bpc - 1);
        c.shift_VP = bpc;
    } else {
        c.add_shift_round_VP = 32768;
        c.shift_VP = 16;
    }
    for (unsigned i = 0; i < h; ++i) {
        vif_subsample_vertical16(&c, w, i);
        PADDING_SQ_DATA_2(buf, w, c.fwidth_half);
        vif_subsample_horizontal16(&c, w, i);
    }
    decimate_and_pad(buf, w, h, scale);
}

// NOLINTEND(modernize-use-nullptr)
