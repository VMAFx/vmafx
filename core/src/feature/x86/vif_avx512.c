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
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
#include "stdio.h"
#include "feature/common/macros.h"
#include "feature/integer_vif.h"

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
            ref[i * stride + j] = buf->mu1[(i * 2) * mu_stride + (j * 2)];
            dis[i * stride + j] = buf->mu2[(i * 2) * mu_stride + (j * 2)];
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

    const double eps = 65536 * 1.0e-10;

    for (int b = 0; b < 16; b += 8) {
        __m512i msigma1 = _mm512_cvtepi32_epi64(_mm512_castsi512_si256(xx));
        __m512i msigma2 = _mm512_cvtepi32_epi64(_mm512_castsi512_si256(yy));
        __m512i msigma12 = _mm512_cvtepi32_epi64(_mm512_castsi512_si256(xy));
        xx = _mm512_castsi256_si512(_mm512_extracti64x4_epi64(xx, 1));
        yy = _mm512_castsi256_si512(_mm512_extracti64x4_epi64(yy, 1));
        xy = _mm512_castsi256_si512(_mm512_extracti64x4_epi64(xy, 1));
        msigma2 = _mm512_max_epi64(msigma2, _mm512_setzero_si512());
        msigma12 = _mm512_max_epi64(msigma12, _mm512_setzero_si512());

        // log stage
        __m512i mlog_den_stage1 = _mm512_add_epi64(msigma1, _mm512_set1_epi64(sigma_nsq));
        __m512i mnorm =
            _mm512_sub_epi64(_mm512_set1_epi64(48), _mm512_lzcnt_epi64(mlog_den_stage1));
        __m512i mlog_den1 = _mm512_srlv_epi64(mlog_den_stage1, mnorm);
        /* ADR-0500: LUT shrunk to 16384 entries (32 KB, fits L1D on Zen).
         * The normalised mantissa is in [32768..65535]; mask off bit 15 to
         * obtain the 15-bit index into log2_table[0..32767].  All three gather
         * sites below apply the same mask.  Bit-exactness is preserved. */
        __m256i mlog_den1_idx = _mm256_and_si256(
            _mm512_cvtusepi64_epi32(mlog_den1), _mm256_set1_epi32((int)(VIF_LOG2_TABLE_SIZE - 1u)));
        __m512i mden_val = _mm512_i32gather_epi64(mlog_den1_idx, log2_table, sizeof(*log2_table));
        mden_val =
            _mm512_and_si512(mden_val, _mm512_set1_epi64(0xffff)); // we took 64 bits, we need 16
        mden_val = _mm512_add_epi64(mden_val, _mm512_slli_epi64(mnorm, 11));
        mden_val = _mm512_sub_epi64(mden_val, _mm512_set1_epi64(2048 * 17));
        __mmask8 msigma1_mask = _mm512_cmpgt_epi64_mask(_mm512_set1_epi64(sigma_nsq), msigma1);
        __mmask8 msigma2_mask = _mm512_cmpgt_epi64_mask(msigma2, _mm512_setzero_si512());
        __mmask8 msigma12_mask = _mm512_cmpgt_epi64_mask(msigma12, _mm512_setzero_si512());
        __m512d msigma1_d = _mm512_cvtepu64_pd(msigma1);
        __m512d mg = _mm512_div_pd(_mm512_cvtepu64_pd(msigma12),
                                   _mm512_add_pd(msigma1_d, _mm512_set1_pd(eps)));
        __m512i msv_sq = _mm512_cvttpd_epi64(_mm512_sub_pd(
            _mm512_cvtepi64_pd(msigma2), _mm512_mul_pd(mg, _mm512_cvtepi64_pd(msigma12))));
        msv_sq = _mm512_max_epi64(msv_sq, _mm512_setzero_si512());
        mg = _mm512_min_pd(mg, _mm512_set1_pd(vif_enhn_gain_limit));

        __m512i mnumer1 = _mm512_add_epi64(msv_sq, _mm512_set1_epi64(sigma_nsq));
        __m512i mnumer1_lz = _mm512_sub_epi64(_mm512_set1_epi64(48), _mm512_lzcnt_epi64(mnumer1));
        __m512i mnumer1_mantissa = _mm512_srlv_epi64(mnumer1, mnumer1_lz);
        __m512i mnumer1_mantissa_log = _mm512_and_si512(
            _mm512_set1_epi64(0xffff),
            _mm512_i32gather_epi64(
                _mm256_and_si256(_mm512_cvtusepi64_epi32(mnumer1_mantissa),
                                 _mm256_set1_epi32((int)(VIF_LOG2_TABLE_SIZE - 1u))),
                log2_table, sizeof(*log2_table))); // we took 64 bits, we need 16
        __m512i mnumer1_log =
            _mm512_add_epi64(mnumer1_mantissa_log, _mm512_slli_epi64(mnumer1_lz, 11));

        __m512i mnumer1_tmp = _mm512_add_epi64(
            mnumer1, _mm512_cvttpd_epi64(_mm512_mul_pd(_mm512_mul_pd(mg, mg), msigma1_d)));
        __m512i mnumer1_tmp_lz =
            _mm512_sub_epi64(_mm512_set1_epi64(48), _mm512_lzcnt_epi64(mnumer1_tmp));
        __m512i mnumer1_tmp_mantissa = _mm512_srlv_epi64(mnumer1_tmp, mnumer1_tmp_lz);
        __m512i mnumer1_tmp_mantissa_log = _mm512_and_si512(
            _mm512_set1_epi64(0xffff),
            _mm512_i32gather_epi64(
                _mm256_and_si256(_mm512_cvtusepi64_epi32(mnumer1_tmp_mantissa),
                                 _mm256_set1_epi32((int)(VIF_LOG2_TABLE_SIZE - 1u))),
                log2_table, sizeof(*log2_table))); // we took 64 bits, we need 16
        __m512i mnumer1_tmp_log =
            _mm512_add_epi64(mnumer1_tmp_mantissa_log, _mm512_slli_epi64(mnumer1_tmp_lz, 11));

        __m512i mnum_val = _mm512_sub_epi64(mnumer1_tmp_log, mnumer1_log);

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

void vif_statistic_8_avx512(struct VifPublicState *s, float *num, float *den, unsigned w,
                            unsigned h)
{
    const unsigned fwidth = vif_filter1d_width[0];
    const uint16_t *vif_filt = vif_filter1d_table[0];
    VifBuffer buf = s->buf;
    const unsigned fwidth_half = fwidth >> 1;
    const uint16_t *log2_table = s->log2_table;
    double vif_enhn_gain_limit = s->vif_enhn_gain_limit;

    int64_t accum_num_log = 0;
    int64_t accum_den_log = 0;
    int64_t accum_num_non_log = 0;
    int64_t accum_den_non_log = 0;

    __m512i round_128 = _mm512_set1_epi32(128);
    __m512i mask2 = _mm512_set_epi32(30, 28, 26, 24, 22, 20, 18, 16, 14, 12, 10, 8, 6, 4, 2, 0);

    Residuals512 residuals;
    residuals.maccum_den_log = _mm512_setzero_si512();
    residuals.maccum_num_log = _mm512_setzero_si512();
    residuals.maccum_den_non_log = _mm512_setzero_si512();
    residuals.maccum_num_non_log = _mm512_setzero_si512();
    for (unsigned i = 0; i < h; ++i) {
        // VERTICAL
        int i_back = i - fwidth_half;
        int i_forward = i + fwidth_half;

        // First consider all blocks of 16 elements until it's not possible anymore
        unsigned n = w >> 4;
        for (unsigned jj = 0; jj < n << 4; jj += 32) {
            __m512i f0 = _mm512_set1_epi32(vif_filt[fwidth / 2]);
            __m512i r0 = _mm512_cvtepu8_epi16(
                _mm256_loadu_si256((__m256i *)(((uint8_t *)buf.ref) + (buf.stride * i) + jj)));
            __m512i d0 = _mm512_cvtepu8_epi16(
                _mm256_loadu_si256((__m256i *)(((uint8_t *)buf.dis) + (buf.stride * i) + jj)));

            __m512i r0_lo = _mm512_unpacklo_epi16(r0, _mm512_setzero_si512());
            __m512i r0_hi = _mm512_unpackhi_epi16(r0, _mm512_setzero_si512());
            __m512i d0_lo = _mm512_unpacklo_epi16(d0, _mm512_setzero_si512());
            __m512i d0_hi = _mm512_unpackhi_epi16(d0, _mm512_setzero_si512());

            // filtered r,d
            __m512i accum_mu1_lo = _mm512_mullo_epi32(r0_lo, f0);
            __m512i accum_mu1_hi = _mm512_mullo_epi32(r0_hi, f0);
            __m512i accum_mu2_lo = _mm512_mullo_epi32(d0_lo, f0);
            __m512i accum_mu2_hi = _mm512_mullo_epi32(d0_hi, f0);
            __m512i accum_ref_lo = _mm512_mullo_epi32(f0, _mm512_mullo_epi32(r0_lo, r0_lo));
            __m512i accum_ref_hi = _mm512_mullo_epi32(f0, _mm512_mullo_epi32(r0_hi, r0_hi));
            __m512i accum_dis_lo = _mm512_mullo_epi32(f0, _mm512_mullo_epi32(d0_lo, d0_lo));
            __m512i accum_dis_hi = _mm512_mullo_epi32(f0, _mm512_mullo_epi32(d0_hi, d0_hi));
            __m512i accum_ref_dis_lo = _mm512_mullo_epi32(f0, _mm512_mullo_epi32(r0_lo, d0_lo));
            __m512i accum_ref_dis_hi = _mm512_mullo_epi32(f0, _mm512_mullo_epi32(r0_hi, d0_hi));

            for (unsigned int tap = 0; tap < fwidth / 2; tap += 2) {
                int ii_back = i_back + tap;
                int ii_forward = i_forward - tap;

                __m512i f_tap0 = _mm512_set1_epi32(vif_filt[tap]);
                __m512i f_tap1 = _mm512_set1_epi32(vif_filt[tap + 1]);
                __m512i f_tap0_1 = _mm512_set1_epi32(vif_filt[tap] + (vif_filt[tap + 1] << 16));

                __m512i r_back0 = _mm512_cvtepu8_epi16(_mm256_loadu_si256(
                    (__m256i *)(((uint8_t *)buf.ref) + (buf.stride * ii_back) + jj)));
                __m512i d_back0 = _mm512_cvtepu8_epi16(_mm256_loadu_si256(
                    (__m256i *)(((uint8_t *)buf.dis) + (buf.stride * ii_back) + jj)));
                __m512i r_fwd0 = _mm512_cvtepu8_epi16(_mm256_loadu_si256(
                    (__m256i *)(((uint8_t *)buf.ref) + (buf.stride * ii_forward) + jj)));
                __m512i d_fwd0 = _mm512_cvtepu8_epi16(_mm256_loadu_si256(
                    (__m256i *)(((uint8_t *)buf.dis) + (buf.stride * ii_forward) + jj)));

                __m512i r_back1 = _mm512_cvtepu8_epi16(_mm256_loadu_si256(
                    (__m256i *)(((uint8_t *)buf.ref) + (buf.stride * (ii_back + 1)) + jj)));
                __m512i d_back1 = _mm512_cvtepu8_epi16(_mm256_loadu_si256(
                    (__m256i *)(((uint8_t *)buf.dis) + (buf.stride * (ii_back + 1)) + jj)));
                __m512i r_fwd1 = _mm512_cvtepu8_epi16(_mm256_loadu_si256(
                    (__m256i *)(((uint8_t *)buf.ref) + (buf.stride * (ii_forward - 1)) + jj)));
                __m512i d_fwd1 = _mm512_cvtepu8_epi16(_mm256_loadu_si256(
                    (__m256i *)(((uint8_t *)buf.dis) + (buf.stride * (ii_forward - 1)) + jj)));

                __m512i r0p16 = _mm512_add_epi16(r_back0, r_fwd0);
                __m512i r1p15 = _mm512_add_epi16(r_back1, r_fwd1);

                __m512i d0p16 = _mm512_add_epi16(d_back0, d_fwd0);
                __m512i d1p15 = _mm512_add_epi16(d_back1, d_fwd1);

                __m512i r_0p16_1p15_lo = _mm512_unpacklo_epi16(r0p16, r1p15);
                __m512i r_0p16_1p15_hi = _mm512_unpackhi_epi16(r0p16, r1p15);
                __m512i d_0p16_1p15_lo = _mm512_unpacklo_epi16(d0p16, d1p15);
                __m512i d_0p16_1p15_hi = _mm512_unpackhi_epi16(d0p16, d1p15);

                accum_mu1_lo =
                    _mm512_add_epi32(accum_mu1_lo, _mm512_madd_epi16(r_0p16_1p15_lo, f_tap0_1));
                accum_mu1_hi =
                    _mm512_add_epi32(accum_mu1_hi, _mm512_madd_epi16(r_0p16_1p15_hi, f_tap0_1));
                accum_mu2_lo =
                    _mm512_add_epi32(accum_mu2_lo, _mm512_madd_epi16(d_0p16_1p15_lo, f_tap0_1));
                accum_mu2_hi =
                    _mm512_add_epi32(accum_mu2_hi, _mm512_madd_epi16(d_0p16_1p15_hi, f_tap0_1));

                __m512i r0_r16_lo = _mm512_unpacklo_epi16(r_back0, r_fwd0);
                __m512i r0_r16_hi = _mm512_unpackhi_epi16(r_back0, r_fwd0);
                __m512i r1_r15_lo = _mm512_unpacklo_epi16(r_back1, r_fwd1);
                __m512i r1_r15_hi = _mm512_unpackhi_epi16(r_back1, r_fwd1);

                __m512i d0_r16_lo = _mm512_unpacklo_epi16(d_back0, d_fwd0);
                __m512i d0_r16_hi = _mm512_unpackhi_epi16(d_back0, d_fwd0);
                __m512i d1_r15_lo = _mm512_unpacklo_epi16(d_back1, d_fwd1);
                __m512i d1_r15_hi = _mm512_unpackhi_epi16(d_back1, d_fwd1);

                __m512i r0_16_lo_sq = _mm512_madd_epi16(r0_r16_lo, r0_r16_lo);
                __m512i r0_16_hi_sq = _mm512_madd_epi16(r0_r16_hi, r0_r16_hi);
                __m512i r1_15_lo_sq = _mm512_madd_epi16(r1_r15_lo, r1_r15_lo);
                __m512i r1_15_hi_sq = _mm512_madd_epi16(r1_r15_hi, r1_r15_hi);

                __m512i d0_16_lo_sq = _mm512_madd_epi16(d0_r16_lo, d0_r16_lo);
                __m512i d0_16_hi_sq = _mm512_madd_epi16(d0_r16_hi, d0_r16_hi);
                __m512i d1_15_lo_sq = _mm512_madd_epi16(d1_r15_lo, d1_r15_lo);
                __m512i d1_15_hi_sq = _mm512_madd_epi16(d1_r15_hi, d1_r15_hi);

                __m512i r016_d016_lo_sq = _mm512_madd_epi16(d0_r16_lo, r0_r16_lo);
                __m512i r016_d016_hi_sq = _mm512_madd_epi16(d0_r16_hi, r0_r16_hi);
                __m512i r115_d115_lo_sq = _mm512_madd_epi16(d1_r15_lo, r1_r15_lo);
                __m512i r115_d115_hi_sq = _mm512_madd_epi16(d1_r15_hi, r1_r15_hi);

                accum_ref_lo =
                    _mm512_add_epi32(accum_ref_lo, _mm512_mullo_epi32(r0_16_lo_sq, f_tap0));
                accum_ref_hi =
                    _mm512_add_epi32(accum_ref_hi, _mm512_mullo_epi32(r0_16_hi_sq, f_tap0));
                accum_dis_lo =
                    _mm512_add_epi32(accum_dis_lo, _mm512_mullo_epi32(d0_16_lo_sq, f_tap0));
                accum_dis_hi =
                    _mm512_add_epi32(accum_dis_hi, _mm512_mullo_epi32(d0_16_hi_sq, f_tap0));
                accum_ref_dis_lo =
                    _mm512_add_epi32(accum_ref_dis_lo, _mm512_mullo_epi32(r016_d016_lo_sq, f_tap0));
                accum_ref_dis_hi =
                    _mm512_add_epi32(accum_ref_dis_hi, _mm512_mullo_epi32(r016_d016_hi_sq, f_tap0));
                accum_ref_lo =
                    _mm512_add_epi32(accum_ref_lo, _mm512_mullo_epi32(r1_15_lo_sq, f_tap1));
                accum_ref_hi =
                    _mm512_add_epi32(accum_ref_hi, _mm512_mullo_epi32(r1_15_hi_sq, f_tap1));
                accum_dis_lo =
                    _mm512_add_epi32(accum_dis_lo, _mm512_mullo_epi32(d1_15_lo_sq, f_tap1));
                accum_dis_hi =
                    _mm512_add_epi32(accum_dis_hi, _mm512_mullo_epi32(d1_15_hi_sq, f_tap1));
                accum_ref_dis_lo =
                    _mm512_add_epi32(accum_ref_dis_lo, _mm512_mullo_epi32(r115_d115_lo_sq, f_tap1));
                accum_ref_dis_hi =
                    _mm512_add_epi32(accum_ref_dis_hi, _mm512_mullo_epi32(r115_d115_hi_sq, f_tap1));
            }

            accum_mu1_lo = _mm512_add_epi32(accum_mu1_lo, round_128);
            accum_mu1_hi = _mm512_add_epi32(accum_mu1_hi, round_128);
            accum_mu2_lo = _mm512_add_epi32(accum_mu2_lo, round_128);
            accum_mu2_hi = _mm512_add_epi32(accum_mu2_hi, round_128);
            accum_mu1_lo = _mm512_srli_epi32(accum_mu1_lo, 0x08);
            accum_mu1_hi = _mm512_srli_epi32(accum_mu1_hi, 0x08);
            accum_mu2_lo = _mm512_srli_epi32(accum_mu2_lo, 0x08);
            accum_mu2_hi = _mm512_srli_epi32(accum_mu2_hi, 0x08);

            __m512i perm_lo = _mm512_set_epi64(11, 10, 3, 2, 9, 8, 1, 0);
            __m512i perm_hi = _mm512_set_epi64(15, 14, 7, 6, 13, 12, 5, 4);

            __m512i tmp_mu1 = accum_mu1_lo;
            accum_mu1_lo = _mm512_permutex2var_epi64(tmp_mu1, perm_lo, accum_mu1_hi);
            accum_mu1_hi = _mm512_permutex2var_epi64(tmp_mu1, perm_hi, accum_mu1_hi);
            __m512i tmp_mu2 = accum_mu2_lo;
            accum_mu2_lo = _mm512_permutex2var_epi64(tmp_mu2, perm_lo, accum_mu2_hi);
            accum_mu2_hi = _mm512_permutex2var_epi64(tmp_mu2, perm_hi, accum_mu2_hi);
            __m512i tmp_mu_ref = accum_ref_lo;
            accum_ref_lo = _mm512_permutex2var_epi64(tmp_mu_ref, perm_lo, accum_ref_hi);
            accum_ref_hi = _mm512_permutex2var_epi64(tmp_mu_ref, perm_hi, accum_ref_hi);
            __m512i tmp_mu_dis = accum_dis_lo;
            accum_dis_lo = _mm512_permutex2var_epi64(tmp_mu_dis, perm_lo, accum_dis_hi);
            accum_dis_hi = _mm512_permutex2var_epi64(tmp_mu_dis, perm_hi, accum_dis_hi);
            __m512i tmp_ref_dis = accum_ref_dis_lo;
            accum_ref_dis_lo = _mm512_permutex2var_epi64(tmp_ref_dis, perm_lo, accum_ref_dis_hi);
            accum_ref_dis_hi = _mm512_permutex2var_epi64(tmp_ref_dis, perm_hi, accum_ref_dis_hi);

            _mm512_storeu_si512((__m512i *)(buf.tmp.mu1 + jj), accum_mu1_lo);
            _mm512_storeu_si512((__m512i *)(buf.tmp.mu1 + jj + 16), accum_mu1_hi);
            _mm512_storeu_si512((__m512i *)(buf.tmp.mu2 + jj), accum_mu2_lo);
            _mm512_storeu_si512((__m512i *)(buf.tmp.mu2 + jj + 16), accum_mu2_hi);
            _mm512_storeu_si512((__m512i *)(buf.tmp.ref + jj), accum_ref_lo);
            _mm512_storeu_si512((__m512i *)(buf.tmp.ref + jj + 16), accum_ref_hi);
            _mm512_storeu_si512((__m512i *)(buf.tmp.dis + jj), accum_dis_lo);
            _mm512_storeu_si512((__m512i *)(buf.tmp.dis + jj + 16), accum_dis_hi);
            _mm512_storeu_si512((__m512i *)(buf.tmp.ref_dis + jj), accum_ref_dis_lo);
            _mm512_storeu_si512((__m512i *)(buf.tmp.ref_dis + jj + 16), accum_ref_dis_hi);
        }

        // Then consider the remaining elements individually
        for (unsigned j = n << 4; j < w; ++j) {
            uint32_t accum_mu1 = 0;
            uint32_t accum_mu2 = 0;
            uint64_t accum_ref = 0;
            uint64_t accum_dis = 0;
            uint64_t accum_ref_dis = 0;

            for (unsigned fi = 0; fi < fwidth; ++fi) {
                int ii = (int)i - fwidth_half;
                int ii_check = ii + fi;
                const uint16_t fcoeff = vif_filt[fi];
                const uint8_t *ref = (uint8_t *)buf.ref;
                const uint8_t *dis = (uint8_t *)buf.dis;
                uint16_t imgcoeff_ref = ref[ii_check * buf.stride + j];
                uint16_t imgcoeff_dis = dis[ii_check * buf.stride + j];
                uint32_t img_coeff_ref = fcoeff * (uint32_t)imgcoeff_ref;
                uint32_t img_coeff_dis = fcoeff * (uint32_t)imgcoeff_dis;
                accum_mu1 += img_coeff_ref;
                accum_mu2 += img_coeff_dis;
                accum_ref += img_coeff_ref * (uint64_t)imgcoeff_ref;
                accum_dis += img_coeff_dis * (uint64_t)imgcoeff_dis;
                accum_ref_dis += img_coeff_ref * (uint64_t)imgcoeff_dis;
            }

            buf.tmp.mu1[j] = (accum_mu1 + 128) >> 8;
            buf.tmp.mu2[j] = (accum_mu2 + 128) >> 8;
            buf.tmp.ref[j] = accum_ref;
            buf.tmp.dis[j] = accum_dis;
            buf.tmp.ref_dis[j] = accum_ref_dis;
        }

        PADDING_SQ_DATA(&buf, w, fwidth_half);

        //HORIZONTAL
        for (unsigned j = 0; j < n << 4; j += 16) {
            __m512i mu1sq;
            __m512i mu2sq;
            __m512i mu1mu2;
            __m512i xx;
            __m512i yy;
            __m512i xy;
            __m512i mask5 =
                _mm512_set_epi32(30, 28, 14, 12, 26, 24, 10, 8, 22, 20, 6, 4, 18, 16, 2, 0);
            // compute mu1sq, mu2sq, mu1mu2
            {
                __m512i fq = _mm512_set1_epi32(vif_filt[fwidth / 2]);
                __m512i acc0 =
                    _mm512_mullo_epi32(_mm512_loadu_si512((__m512i *)(buf.tmp.mu1 + j + 0)), fq);
                __m512i acc1 =
                    _mm512_mullo_epi32(_mm512_loadu_si512((__m512i *)(buf.tmp.mu2 + j + 0)), fq);

                for (unsigned fj = 0; fj < fwidth / 2; ++fj) {
                    __m512i f_tap = _mm512_set1_epi32(vif_filt[fj]);
                    acc0 = _mm512_add_epi64(
                        acc0,
                        _mm512_mullo_epi32(
                            _mm512_loadu_si512((__m512i *)(buf.tmp.mu1 + j - fwidth / 2 + fj + 0)),
                            f_tap));
                    acc0 = _mm512_add_epi64(
                        acc0,
                        _mm512_mullo_epi32(
                            _mm512_loadu_si512((__m512i *)(buf.tmp.mu1 + j + fwidth / 2 - fj + 0)),
                            f_tap));
                    acc1 = _mm512_add_epi64(
                        acc1,
                        _mm512_mullo_epi32(
                            _mm512_loadu_si512((__m512i *)(buf.tmp.mu2 + j - fwidth / 2 + fj + 0)),
                            f_tap));
                    acc1 = _mm512_add_epi64(
                        acc1,
                        _mm512_mullo_epi32(
                            _mm512_loadu_si512((__m512i *)(buf.tmp.mu2 + j + fwidth / 2 - fj + 0)),
                            f_tap));
                }
                __m512i mu1 = acc0;
                __m512i acc0_lo_512 = _mm512_unpacklo_epi32(acc0, _mm512_setzero_si512());
                __m512i acc0_hi_512 = _mm512_unpackhi_epi32(acc0, _mm512_setzero_si512());
                acc0_lo_512 = _mm512_mul_epu32(acc0_lo_512, acc0_lo_512);
                acc0_hi_512 = _mm512_mul_epu32(acc0_hi_512, acc0_hi_512);
                acc0_lo_512 = _mm512_srli_epi64(
                    _mm512_add_epi64(acc0_lo_512, _mm512_set1_epi64(0x80000000)), 32);
                acc0_hi_512 = _mm512_srli_epi64(
                    _mm512_add_epi64(acc0_hi_512, _mm512_set1_epi64(0x80000000)), 32);
                mu1sq = _mm512_permutex2var_epi32(acc0_lo_512, mask5, acc0_hi_512);

                __m512i acc0lo_512 = _mm512_unpacklo_epi32(acc1, _mm512_setzero_si512());
                __m512i acc0hi_512 = _mm512_unpackhi_epi32(acc1, _mm512_setzero_si512());
                __m512i mu1lo_512 = _mm512_unpacklo_epi32(mu1, _mm512_setzero_si512());
                __m512i mu1hi_512 = _mm512_unpackhi_epi32(mu1, _mm512_setzero_si512());

                mu1lo_512 = _mm512_mul_epu32(mu1lo_512, acc0lo_512);
                mu1hi_512 = _mm512_mul_epu32(mu1hi_512, acc0hi_512);
                mu1lo_512 = _mm512_srli_epi64(
                    _mm512_add_epi64(mu1lo_512, _mm512_set1_epi64(0x80000000)), 32);
                mu1hi_512 = _mm512_srli_epi64(
                    _mm512_add_epi64(mu1hi_512, _mm512_set1_epi64(0x80000000)), 32);

                mu1mu2 = _mm512_permutex2var_epi32(mu1lo_512, mask5, mu1hi_512);
                acc0lo_512 = _mm512_mul_epu32(acc0lo_512, acc0lo_512);
                acc0hi_512 = _mm512_mul_epu32(acc0hi_512, acc0hi_512);
                acc0lo_512 = _mm512_srli_epi64(
                    _mm512_add_epi64(acc0lo_512, _mm512_set1_epi64(0x80000000)), 32);
                acc0hi_512 = _mm512_srli_epi64(
                    _mm512_add_epi64(acc0hi_512, _mm512_set1_epi64(0x80000000)), 32);
                mu2sq = _mm512_permutex2var_epi32(acc0lo_512, mask5, acc0hi_512);
            }

            // compute xx, yy, xy
            {
                __m512i rounder = _mm512_set1_epi64(0x8000);
                __m512i fq = _mm512_set1_epi64(vif_filt[fwidth / 2]);
                __m512i s0 = _mm512_cvtepu32_epi64(
                    _mm256_loadu_si256((__m256i *)(buf.tmp.ref + j + 0))); // 4
                __m512i s2 = _mm512_cvtepu32_epi64(
                    _mm256_loadu_si256((__m256i *)(buf.tmp.ref + j + 8))); // 4
                __m512i refsq_lo = _mm512_add_epi64(rounder, _mm512_mul_epu32(s0, fq));
                __m512i refsq_hi = _mm512_add_epi64(rounder, _mm512_mul_epu32(s2, fq));

                s0 = _mm512_cvtepu32_epi64(
                    _mm256_loadu_si256((__m256i *)(buf.tmp.dis + j + 0))); // 4
                s2 = _mm512_cvtepu32_epi64(
                    _mm256_loadu_si256((__m256i *)(buf.tmp.dis + j + 8))); // 4
                __m512i dissq_lo = _mm512_add_epi64(rounder, _mm512_mul_epu32(s0, fq));
                __m512i dissq_hi = _mm512_add_epi64(rounder, _mm512_mul_epu32(s2, fq));

                s0 = _mm512_cvtepu32_epi64(
                    _mm256_loadu_si256((__m256i *)(buf.tmp.ref_dis + j + 0))); // 4
                s2 = _mm512_cvtepu32_epi64(
                    _mm256_loadu_si256((__m256i *)(buf.tmp.ref_dis + j + 8))); // 4
                __m512i refdis_lo = _mm512_add_epi64(rounder, _mm512_mul_epu32(s0, fq));
                __m512i refdis_hi = _mm512_add_epi64(rounder, _mm512_mul_epu32(s2, fq));

                for (unsigned fj = 0; fj < fwidth / 2; ++fj) {
                    __m512i f_tap = _mm512_set1_epi64(vif_filt[fj]);
                    s0 = _mm512_cvtepu32_epi64(_mm256_loadu_si256(
                        (__m256i *)(buf.tmp.ref + j - fwidth / 2 + fj + 0))); // 4
                    s2 = _mm512_cvtepu32_epi64(_mm256_loadu_si256(
                        (__m256i *)(buf.tmp.ref + j - fwidth / 2 + fj + 8))); // 4
                    refsq_lo = _mm512_add_epi64(refsq_lo, _mm512_mul_epu32(s0, f_tap));
                    refsq_hi = _mm512_add_epi64(refsq_hi, _mm512_mul_epu32(s2, f_tap));
                    s0 = _mm512_cvtepu32_epi64(_mm256_loadu_si256(
                        (__m256i *)(buf.tmp.ref + j + fwidth / 2 - fj + 0))); // 4
                    s2 = _mm512_cvtepu32_epi64(_mm256_loadu_si256(
                        (__m256i *)(buf.tmp.ref + j + fwidth / 2 - fj + 8))); // 4
                    refsq_lo = _mm512_add_epi64(refsq_lo, _mm512_mul_epu32(s0, f_tap));
                    refsq_hi = _mm512_add_epi64(refsq_hi, _mm512_mul_epu32(s2, f_tap));

                    s0 = _mm512_cvtepu32_epi64(_mm256_loadu_si256(
                        (__m256i *)(buf.tmp.dis + j - fwidth / 2 + fj + 0))); // 4
                    s2 = _mm512_cvtepu32_epi64(_mm256_loadu_si256(
                        (__m256i *)(buf.tmp.dis + j - fwidth / 2 + fj + 8))); // 4
                    dissq_lo = _mm512_add_epi64(dissq_lo, _mm512_mul_epu32(s0, f_tap));
                    dissq_hi = _mm512_add_epi64(dissq_hi, _mm512_mul_epu32(s2, f_tap));
                    s0 = _mm512_cvtepu32_epi64(_mm256_loadu_si256(
                        (__m256i *)(buf.tmp.dis + j + fwidth / 2 - fj + 0))); // 4
                    s2 = _mm512_cvtepu32_epi64(_mm256_loadu_si256(
                        (__m256i *)(buf.tmp.dis + j + fwidth / 2 - fj + 8))); // 4
                    dissq_lo = _mm512_add_epi64(dissq_lo, _mm512_mul_epu32(s0, f_tap));
                    dissq_hi = _mm512_add_epi64(dissq_hi, _mm512_mul_epu32(s2, f_tap));

                    s0 = _mm512_cvtepu32_epi64(_mm256_loadu_si256(
                        (__m256i *)(buf.tmp.ref_dis + j - fwidth / 2 + fj + 0))); // 4
                    s2 = _mm512_cvtepu32_epi64(_mm256_loadu_si256(
                        (__m256i *)(buf.tmp.ref_dis + j - fwidth / 2 + fj + 8))); // 4
                    refdis_lo = _mm512_add_epi64(refdis_lo, _mm512_mul_epu32(s0, f_tap));
                    refdis_hi = _mm512_add_epi64(refdis_hi, _mm512_mul_epu32(s2, f_tap));
                    s0 = _mm512_cvtepu32_epi64(_mm256_loadu_si256(
                        (__m256i *)(buf.tmp.ref_dis + j + fwidth / 2 - fj + 0))); // 4
                    s2 = _mm512_cvtepu32_epi64(_mm256_loadu_si256(
                        (__m256i *)(buf.tmp.ref_dis + j + fwidth / 2 - fj + 8))); // 4
                    refdis_lo = _mm512_add_epi64(refdis_lo, _mm512_mul_epu32(s0, f_tap));
                    refdis_hi = _mm512_add_epi64(refdis_hi, _mm512_mul_epu32(s2, f_tap));
                }
                refsq_lo = _mm512_srli_epi64(refsq_lo, 16);
                refsq_hi = _mm512_srli_epi64(refsq_hi, 16);
                __m512i refsq = _mm512_permutex2var_epi32(refsq_lo, mask2, refsq_hi);
                xx = _mm512_sub_epi32(refsq, mu1sq);

                dissq_lo = _mm512_srli_epi64(dissq_lo, 16);
                dissq_hi = _mm512_srli_epi64(dissq_hi, 16);
                __m512i dissq = _mm512_permutex2var_epi32(dissq_lo, mask2, dissq_hi);
                yy = _mm512_max_epi32(_mm512_sub_epi32(dissq, mu2sq), _mm512_setzero_si512());

                refdis_lo = _mm512_srli_epi64(refdis_lo, 16);
                refdis_hi = _mm512_srli_epi64(refdis_hi, 16);
                __m512i refdis = _mm512_permutex2var_epi32(refdis_lo, mask2, refdis_hi);
                xy = _mm512_sub_epi32(refdis, mu1mu2);
            }
            vif_statistic_avx512(&residuals, xx, xy, yy, log2_table, vif_enhn_gain_limit);
        }

        if ((n << 4) != w) {
            VifResiduals tail_residuals = vif_compute_line_residuals(s, n << 4, w, 0);
            accum_num_log += tail_residuals.accum_num_log;
            accum_den_log += tail_residuals.accum_den_log;
            accum_num_non_log += tail_residuals.accum_num_non_log;
            accum_den_non_log += tail_residuals.accum_den_non_log;
        }
    }

    accum_num_log += _mm512_reduce_add_epi64(residuals.maccum_num_log);
    accum_den_log += _mm512_reduce_add_epi64(residuals.maccum_den_log);
    accum_num_non_log += _mm512_reduce_add_epi64(residuals.maccum_num_non_log);
    accum_den_non_log += _mm512_reduce_add_epi64(residuals.maccum_den_non_log);
    num[0] =
        accum_num_log / 2048.0 + (accum_den_non_log - ((accum_num_non_log) / 16384.0) / (65025.0));
    den[0] = accum_den_log / 2048.0 + accum_den_non_log;
}

void vif_statistic_16_avx512(struct VifPublicState *s, float *num, float *den, unsigned w,
                             unsigned h, int bpc, int scale)
{
    const unsigned fwidth = vif_filter1d_width[scale];
    const uint16_t *vif_filt = vif_filter1d_table[scale];
    VifBuffer buf = s->buf;
    const ptrdiff_t stride = buf.stride / sizeof(uint16_t);
    int fwidth_half = fwidth >> 1;

    int32_t add_shift_round_VP;
    int32_t shift_VP;
    int32_t add_shift_round_VP_sq;
    int32_t shift_VP_sq;
    const uint16_t *log2_table = s->log2_table;
    double vif_enhn_gain_limit = s->vif_enhn_gain_limit;
    __m512i mask2 = _mm512_set_epi32(30, 28, 26, 24, 22, 20, 18, 16, 14, 12, 10, 8, 6, 4, 2, 0);

    Residuals512 residuals;
    residuals.maccum_den_log = _mm512_setzero_si512();
    residuals.maccum_num_log = _mm512_setzero_si512();
    residuals.maccum_den_non_log = _mm512_setzero_si512();
    residuals.maccum_num_non_log = _mm512_setzero_si512();

    int64_t accum_num_log = 0;
    int64_t accum_den_log = 0;
    int64_t accum_num_non_log = 0;
    int64_t accum_den_non_log = 0;

    if (scale == 0) {
        shift_VP = bpc;
        add_shift_round_VP = 1 << (bpc - 1);
        shift_VP_sq = (bpc - 8) * 2;
        add_shift_round_VP_sq = (bpc == 8) ? 0 : 1 << (shift_VP_sq - 1);
    } else {
        shift_VP = 16;
        add_shift_round_VP = 32768;
        shift_VP_sq = 16;
        add_shift_round_VP_sq = 32768;
    }
    __m512i addnum64 = _mm512_set1_epi64(add_shift_round_VP_sq);
    __m512i addnum = _mm512_set1_epi32(add_shift_round_VP);
    uint16_t *ref = buf.ref;
    uint16_t *dis = buf.dis;

    for (unsigned i = 0; i < h; ++i) {
        //VERTICAL
        int ii = (int)i - fwidth_half;
        int n = w >> 5;
        for (int j = 0; j < n << 5; j = j + 32) {

            __m512i mask3 = _mm512_set_epi64(11, 10, 3, 2, 9, 8, 1, 0);   //first half of 512
            __m512i mask4 = _mm512_set_epi64(15, 14, 7, 6, 13, 12, 5, 4); //second half of 512
            int ii_check = ii;
            __m512i accumr_lo;
            __m512i accumr_hi;
            __m512i accumd_lo;
            __m512i accumd_hi;
            __m512i rmul1;
            __m512i rmul2;
            __m512i dmul1;
            __m512i dmul2;
            __m512i accumref1;
            __m512i accumref2;
            __m512i accumref3;
            __m512i accumref4;
            __m512i accumrefdis1;
            __m512i accumrefdis2;
            __m512i accumrefdis3;
            __m512i accumrefdis4;
            __m512i accumdis1;
            __m512i accumdis2;
            __m512i accumdis3;
            __m512i accumdis4;
            // NOLINTNEXTLINE(clang-analyzer-deadcode.DeadStores): chained zero-init of every SIMD accumulator (rmul1/rmul2/dmul1/dmul2 included) before the inner fi loop overwrites them. Verbatim from upstream Netflix; preserved to keep the AVX-512 VIF kernel byte-identical to its AVX2 / scalar twins.
            accumr_lo = accumr_hi = accumd_lo = accumd_hi = rmul1 = rmul2 = dmul1 = dmul2 =
                accumref1 = accumref2 = accumref3 = accumref4 = accumrefdis1 = accumrefdis2 =
                    accumrefdis3 = accumrefdis4 = accumdis1 = accumdis2 = accumdis3 = accumdis4 =
                        _mm512_setzero_si512();

            for (unsigned fi = 0; fi < fwidth; ++fi, ii_check = ii + fi) {

                const uint16_t fcoeff = vif_filt[fi];
                __m512i f1 = _mm512_set1_epi16(fcoeff);
                __m512i ref1 = _mm512_loadu_si512((__m512i *)(ref + (ii_check * stride) + j));
                __m512i dis1 = _mm512_loadu_si512((__m512i *)(dis + (ii_check * stride) + j));
                __m512i result2 = _mm512_mulhi_epu16(ref1, f1);
                __m512i result2lo = _mm512_mullo_epi16(ref1, f1);
                __m512i rmult1 = _mm512_unpacklo_epi16(result2lo, result2);
                __m512i rmult2 = _mm512_unpackhi_epi16(result2lo, result2);
                rmul1 = _mm512_permutex2var_epi64(rmult1, mask3, rmult2);
                rmul2 = _mm512_permutex2var_epi64(rmult1, mask4, rmult2);
                accumr_lo = _mm512_add_epi32(accumr_lo, rmul1);
                accumr_hi = _mm512_add_epi32(accumr_hi, rmul2);
                __m512i d0 = _mm512_mulhi_epu16(dis1, f1);
                __m512i d0lo = _mm512_mullo_epi16(dis1, f1);
                __m512i dmult1 = _mm512_unpacklo_epi16(d0lo, d0);
                __m512i dmult2 = _mm512_unpackhi_epi16(d0lo, d0);
                dmul1 = _mm512_permutex2var_epi64(dmult1, mask3, dmult2);
                dmul2 = _mm512_permutex2var_epi64(dmult1, mask4, dmult2);
                accumd_lo = _mm512_add_epi32(accumd_lo, dmul1);
                accumd_hi = _mm512_add_epi32(accumd_hi, dmul2);

                __m512i sg0 = _mm512_cvtepu32_epi64(_mm512_castsi512_si256(rmul1));
                __m512i sg1 = _mm512_cvtepu32_epi64(_mm512_extracti64x4_epi64(rmul1, 1));
                __m512i sg2 = _mm512_cvtepu32_epi64(_mm512_castsi512_si256(rmul2));
                __m512i sg3 = _mm512_cvtepu32_epi64(_mm512_extracti64x4_epi64(rmul2, 1));
                __m128i l0 = _mm512_castsi512_si128(ref1);
                __m128i l1 = _mm512_extracti32x4_epi32(ref1, 1);
                __m128i l2 = _mm512_extracti32x4_epi32(ref1, 2);
                __m128i l3 = _mm512_extracti32x4_epi32(ref1, 3);
                accumref1 =
                    _mm512_add_epi64(accumref1, _mm512_mul_epu32(sg0, _mm512_cvtepu16_epi64(l0)));
                accumref2 =
                    _mm512_add_epi64(accumref2, _mm512_mul_epu32(sg2, _mm512_cvtepu16_epi64(l2)));
                accumref3 =
                    _mm512_add_epi64(accumref3, _mm512_mul_epu32(sg1, _mm512_cvtepu16_epi64(l1)));
                accumref4 =
                    _mm512_add_epi64(accumref4, _mm512_mul_epu32(sg3, _mm512_cvtepu16_epi64(l3)));
                l0 = _mm512_castsi512_si128(dis1);
                l1 = _mm512_extracti32x4_epi32(dis1, 1);
                l2 = _mm512_extracti32x4_epi32(dis1, 2);
                l3 = _mm512_extracti32x4_epi32(dis1, 3);

                accumrefdis1 = _mm512_add_epi64(accumrefdis1,
                                                _mm512_mul_epu32(sg0, _mm512_cvtepu16_epi64(l0)));
                accumrefdis2 = _mm512_add_epi64(accumrefdis2,
                                                _mm512_mul_epu32(sg2, _mm512_cvtepu16_epi64(l2)));
                accumrefdis3 = _mm512_add_epi64(accumrefdis3,
                                                _mm512_mul_epu32(sg1, _mm512_cvtepu16_epi64(l1)));
                accumrefdis4 = _mm512_add_epi64(accumrefdis4,
                                                _mm512_mul_epu32(sg3, _mm512_cvtepu16_epi64(l3)));
                __m512i sd0 = _mm512_cvtepu32_epi64(_mm512_castsi512_si256(dmul1));
                __m512i sd1 = _mm512_cvtepu32_epi64(_mm512_extracti64x4_epi64(dmul1, 1));
                __m512i sd2 = _mm512_cvtepu32_epi64(_mm512_castsi512_si256(dmul2));
                __m512i sd3 = _mm512_cvtepu32_epi64(_mm512_extracti64x4_epi64(dmul2, 1));
                accumdis1 =
                    _mm512_add_epi64(accumdis1, _mm512_mul_epu32(sd0, _mm512_cvtepu16_epi64(l0)));
                accumdis2 =
                    _mm512_add_epi64(accumdis2, _mm512_mul_epu32(sd2, _mm512_cvtepu16_epi64(l2)));
                accumdis3 =
                    _mm512_add_epi64(accumdis3, _mm512_mul_epu32(sd1, _mm512_cvtepu16_epi64(l1)));
                accumdis4 =
                    _mm512_add_epi64(accumdis4, _mm512_mul_epu32(sd3, _mm512_cvtepu16_epi64(l3)));
            }
            accumr_lo = _mm512_add_epi32(accumr_lo, addnum);
            accumr_hi = _mm512_add_epi32(accumr_hi, addnum);
            accumr_lo = _mm512_srli_epi32(accumr_lo, shift_VP);
            accumr_hi = _mm512_srli_epi32(accumr_hi, shift_VP);
            _mm512_storeu_si512((__m512i *)(buf.tmp.mu1 + j), accumr_lo);
            _mm512_storeu_si512((__m512i *)(buf.tmp.mu1 + j + 16), accumr_hi);

            accumd_lo = _mm512_add_epi32(accumd_lo, addnum);
            accumd_hi = _mm512_add_epi32(accumd_hi, addnum);
            accumd_lo = _mm512_srli_epi32(accumd_lo, shift_VP);
            accumd_hi = _mm512_srli_epi32(accumd_hi, shift_VP);
            _mm512_storeu_si512((__m512i *)(buf.tmp.mu2 + j), accumd_lo);
            _mm512_storeu_si512((__m512i *)(buf.tmp.mu2 + j + 16), accumd_hi);

            accumref1 = _mm512_add_epi64(accumref1, addnum64);
            accumref2 = _mm512_add_epi64(accumref2, addnum64);
            accumref3 = _mm512_add_epi64(accumref3, addnum64);
            accumref4 = _mm512_add_epi64(accumref4, addnum64);
            accumref1 = _mm512_srli_epi64(accumref1, shift_VP_sq);
            accumref2 = _mm512_srli_epi64(accumref2, shift_VP_sq);
            accumref3 = _mm512_srli_epi64(accumref3, shift_VP_sq);
            accumref4 = _mm512_srli_epi64(accumref4, shift_VP_sq);

            _mm512_storeu_si512((__m512i *)(buf.tmp.ref + j),
                                _mm512_permutex2var_epi32(accumref1, mask2, accumref3));
            _mm512_storeu_si512((__m512i *)(buf.tmp.ref + 16 + j),
                                _mm512_permutex2var_epi32(accumref2, mask2, accumref4));

            accumrefdis1 = _mm512_add_epi64(accumrefdis1, addnum64);
            accumrefdis2 = _mm512_add_epi64(accumrefdis2, addnum64);
            accumrefdis3 = _mm512_add_epi64(accumrefdis3, addnum64);
            accumrefdis4 = _mm512_add_epi64(accumrefdis4, addnum64);
            accumrefdis1 = _mm512_srli_epi64(accumrefdis1, shift_VP_sq);
            accumrefdis2 = _mm512_srli_epi64(accumrefdis2, shift_VP_sq);
            accumrefdis3 = _mm512_srli_epi64(accumrefdis3, shift_VP_sq);
            accumrefdis4 = _mm512_srli_epi64(accumrefdis4, shift_VP_sq);

            _mm512_storeu_si512((__m512i *)(buf.tmp.ref_dis + j),
                                _mm512_permutex2var_epi32(accumrefdis1, mask2, accumrefdis3));
            _mm512_storeu_si512((__m512i *)(buf.tmp.ref_dis + 16 + j),
                                _mm512_permutex2var_epi32(accumrefdis2, mask2, accumrefdis4));

            accumdis1 = _mm512_add_epi64(accumdis1, addnum64);
            accumdis2 = _mm512_add_epi64(accumdis2, addnum64);
            accumdis3 = _mm512_add_epi64(accumdis3, addnum64);
            accumdis4 = _mm512_add_epi64(accumdis4, addnum64);
            accumdis1 = _mm512_srli_epi64(accumdis1, shift_VP_sq);
            accumdis2 = _mm512_srli_epi64(accumdis2, shift_VP_sq);
            accumdis3 = _mm512_srli_epi64(accumdis3, shift_VP_sq);
            accumdis4 = _mm512_srli_epi64(accumdis4, shift_VP_sq);

            _mm512_storeu_si512((__m512i *)(buf.tmp.dis + j),
                                _mm512_permutex2var_epi32(accumdis1, mask2, accumdis3));
            _mm512_storeu_si512((__m512i *)(buf.tmp.dis + 16 + j),
                                _mm512_permutex2var_epi32(accumdis2, mask2, accumdis4));
        }

        for (unsigned j = n << 5; j < w; ++j) {
            uint32_t accum_mu1 = 0;
            uint32_t accum_mu2 = 0;
            uint64_t accum_ref = 0;
            uint64_t accum_dis = 0;
            uint64_t accum_ref_dis = 0;
            for (unsigned fi = 0; fi < fwidth; ++fi) {
                int ii_check = ii + fi;
                const uint16_t fcoeff = vif_filt[fi];
                uint16_t imgcoeff_ref = ref[ii_check * stride + j];
                uint16_t imgcoeff_dis = dis[ii_check * stride + j];
                uint32_t img_coeff_ref = fcoeff * (uint32_t)imgcoeff_ref;
                uint32_t img_coeff_dis = fcoeff * (uint32_t)imgcoeff_dis;
                accum_mu1 += img_coeff_ref;
                accum_mu2 += img_coeff_dis;
                accum_ref += img_coeff_ref * (uint64_t)imgcoeff_ref;
                accum_dis += img_coeff_dis * (uint64_t)imgcoeff_dis;
                accum_ref_dis += img_coeff_ref * (uint64_t)imgcoeff_dis;
            }
            buf.tmp.mu1[j] = (uint16_t)((accum_mu1 + add_shift_round_VP) >> shift_VP);
            buf.tmp.mu2[j] = (uint16_t)((accum_mu2 + add_shift_round_VP) >> shift_VP);
            buf.tmp.ref[j] = (uint32_t)((accum_ref + add_shift_round_VP_sq) >> shift_VP_sq);
            buf.tmp.dis[j] = (uint32_t)((accum_dis + add_shift_round_VP_sq) >> shift_VP_sq);
            buf.tmp.ref_dis[j] = (uint32_t)((accum_ref_dis + add_shift_round_VP_sq) >> shift_VP_sq);
        }

        PADDING_SQ_DATA(&buf, w, fwidth_half);

        //HORIZONTAL
        n = w >> 4;
        for (int j = 0; j < n << 4; j = j + 16) {
            __m512i mu1sq;
            __m512i mu2sq;
            __m512i mu1mu2;
            __m512i xx;
            __m512i yy;
            __m512i xy;
            __m512i mask5 =
                _mm512_set_epi32(30, 28, 14, 12, 26, 24, 10, 8, 22, 20, 6, 4, 18, 16, 2, 0);
            // compute mu1sq, mu2sq, mu1mu2
            {
                __m512i fq = _mm512_set1_epi32(vif_filt[fwidth / 2]);
                __m512i acc0 =
                    _mm512_mullo_epi32(_mm512_loadu_si512((__m512i *)(buf.tmp.mu1 + j + 0)), fq);
                __m512i acc1 =
                    _mm512_mullo_epi32(_mm512_loadu_si512((__m512i *)(buf.tmp.mu2 + j + 0)), fq);

                for (unsigned fj = 0; fj < fwidth / 2; ++fj) {
                    __m512i f_tap = _mm512_set1_epi32(vif_filt[fj]);
                    acc0 = _mm512_add_epi64(
                        acc0,
                        _mm512_mullo_epi32(
                            _mm512_loadu_si512((__m512i *)(buf.tmp.mu1 + j - fwidth / 2 + fj + 0)),
                            f_tap));
                    acc0 = _mm512_add_epi64(
                        acc0,
                        _mm512_mullo_epi32(
                            _mm512_loadu_si512((__m512i *)(buf.tmp.mu1 + j + fwidth / 2 - fj + 0)),
                            f_tap));
                    acc1 = _mm512_add_epi64(
                        acc1,
                        _mm512_mullo_epi32(
                            _mm512_loadu_si512((__m512i *)(buf.tmp.mu2 + j - fwidth / 2 + fj + 0)),
                            f_tap));
                    acc1 = _mm512_add_epi64(
                        acc1,
                        _mm512_mullo_epi32(
                            _mm512_loadu_si512((__m512i *)(buf.tmp.mu2 + j + fwidth / 2 - fj + 0)),
                            f_tap));
                }
                __m512i mu1 = acc0;
                __m512i acc0_lo_512 = _mm512_unpacklo_epi32(acc0, _mm512_setzero_si512());
                __m512i acc0_hi_512 = _mm512_unpackhi_epi32(acc0, _mm512_setzero_si512());
                acc0_lo_512 = _mm512_mul_epu32(acc0_lo_512, acc0_lo_512);
                acc0_hi_512 = _mm512_mul_epu32(acc0_hi_512, acc0_hi_512);
                acc0_lo_512 = _mm512_srli_epi64(
                    _mm512_add_epi64(acc0_lo_512, _mm512_set1_epi64(0x80000000)), 32);
                acc0_hi_512 = _mm512_srli_epi64(
                    _mm512_add_epi64(acc0_hi_512, _mm512_set1_epi64(0x80000000)), 32);
                mu1sq = _mm512_permutex2var_epi32(acc0_lo_512, mask5, acc0_hi_512);

                __m512i acc0lo_512 = _mm512_unpacklo_epi32(acc1, _mm512_setzero_si512());
                __m512i acc0hi_512 = _mm512_unpackhi_epi32(acc1, _mm512_setzero_si512());
                __m512i mu1lo_512 = _mm512_unpacklo_epi32(mu1, _mm512_setzero_si512());
                __m512i mu1hi_512 = _mm512_unpackhi_epi32(mu1, _mm512_setzero_si512());

                mu1lo_512 = _mm512_mul_epu32(mu1lo_512, acc0lo_512);
                mu1hi_512 = _mm512_mul_epu32(mu1hi_512, acc0hi_512);
                mu1lo_512 = _mm512_srli_epi64(
                    _mm512_add_epi64(mu1lo_512, _mm512_set1_epi64(0x80000000)), 32);
                mu1hi_512 = _mm512_srli_epi64(
                    _mm512_add_epi64(mu1hi_512, _mm512_set1_epi64(0x80000000)), 32);

                mu1mu2 = _mm512_permutex2var_epi32(mu1lo_512, mask5, mu1hi_512);
                acc0lo_512 = _mm512_mul_epu32(acc0lo_512, acc0lo_512);
                acc0hi_512 = _mm512_mul_epu32(acc0hi_512, acc0hi_512);
                acc0lo_512 = _mm512_srli_epi64(
                    _mm512_add_epi64(acc0lo_512, _mm512_set1_epi64(0x80000000)), 32);
                acc0hi_512 = _mm512_srli_epi64(
                    _mm512_add_epi64(acc0hi_512, _mm512_set1_epi64(0x80000000)), 32);
                mu2sq = _mm512_permutex2var_epi32(acc0lo_512, mask5, acc0hi_512);
            }

            // compute xx, yy, xy
            {
                __m512i rounder = _mm512_set1_epi64(0x8000);
                __m512i fq = _mm512_set1_epi64(vif_filt[fwidth / 2]);
                __m512i s0 = _mm512_cvtepu32_epi64(
                    _mm256_loadu_si256((__m256i *)(buf.tmp.ref + j + 0))); // 4
                __m512i s2 = _mm512_cvtepu32_epi64(
                    _mm256_loadu_si256((__m256i *)(buf.tmp.ref + j + 8))); // 4
                __m512i refsq_lo = _mm512_add_epi64(rounder, _mm512_mul_epu32(s0, fq));
                __m512i refsq_hi = _mm512_add_epi64(rounder, _mm512_mul_epu32(s2, fq));

                s0 = _mm512_cvtepu32_epi64(
                    _mm256_loadu_si256((__m256i *)(buf.tmp.dis + j + 0))); // 4
                s2 = _mm512_cvtepu32_epi64(
                    _mm256_loadu_si256((__m256i *)(buf.tmp.dis + j + 8))); // 4
                __m512i dissq_lo = _mm512_add_epi64(rounder, _mm512_mul_epu32(s0, fq));
                __m512i dissq_hi = _mm512_add_epi64(rounder, _mm512_mul_epu32(s2, fq));

                s0 = _mm512_cvtepu32_epi64(
                    _mm256_loadu_si256((__m256i *)(buf.tmp.ref_dis + j + 0))); // 4
                s2 = _mm512_cvtepu32_epi64(
                    _mm256_loadu_si256((__m256i *)(buf.tmp.ref_dis + j + 8))); // 4
                __m512i refdis_lo = _mm512_add_epi64(rounder, _mm512_mul_epu32(s0, fq));
                __m512i refdis_hi = _mm512_add_epi64(rounder, _mm512_mul_epu32(s2, fq));

                for (unsigned fj = 0; fj < fwidth / 2; ++fj) {
                    __m512i f_tap = _mm512_set1_epi64(vif_filt[fj]);
                    s0 = _mm512_cvtepu32_epi64(_mm256_loadu_si256(
                        (__m256i *)(buf.tmp.ref + j - fwidth / 2 + fj + 0))); // 4
                    s2 = _mm512_cvtepu32_epi64(_mm256_loadu_si256(
                        (__m256i *)(buf.tmp.ref + j - fwidth / 2 + fj + 8))); // 4
                    refsq_lo = _mm512_add_epi64(refsq_lo, _mm512_mul_epu32(s0, f_tap));
                    refsq_hi = _mm512_add_epi64(refsq_hi, _mm512_mul_epu32(s2, f_tap));
                    s0 = _mm512_cvtepu32_epi64(_mm256_loadu_si256(
                        (__m256i *)(buf.tmp.ref + j + fwidth / 2 - fj + 0))); // 4
                    s2 = _mm512_cvtepu32_epi64(_mm256_loadu_si256(
                        (__m256i *)(buf.tmp.ref + j + fwidth / 2 - fj + 8))); // 4
                    refsq_lo = _mm512_add_epi64(refsq_lo, _mm512_mul_epu32(s0, f_tap));
                    refsq_hi = _mm512_add_epi64(refsq_hi, _mm512_mul_epu32(s2, f_tap));

                    s0 = _mm512_cvtepu32_epi64(_mm256_loadu_si256(
                        (__m256i *)(buf.tmp.dis + j - fwidth / 2 + fj + 0))); // 4
                    s2 = _mm512_cvtepu32_epi64(_mm256_loadu_si256(
                        (__m256i *)(buf.tmp.dis + j - fwidth / 2 + fj + 8))); // 4
                    dissq_lo = _mm512_add_epi64(dissq_lo, _mm512_mul_epu32(s0, f_tap));
                    dissq_hi = _mm512_add_epi64(dissq_hi, _mm512_mul_epu32(s2, f_tap));
                    s0 = _mm512_cvtepu32_epi64(_mm256_loadu_si256(
                        (__m256i *)(buf.tmp.dis + j + fwidth / 2 - fj + 0))); // 4
                    s2 = _mm512_cvtepu32_epi64(_mm256_loadu_si256(
                        (__m256i *)(buf.tmp.dis + j + fwidth / 2 - fj + 8))); // 4
                    dissq_lo = _mm512_add_epi64(dissq_lo, _mm512_mul_epu32(s0, f_tap));
                    dissq_hi = _mm512_add_epi64(dissq_hi, _mm512_mul_epu32(s2, f_tap));

                    s0 = _mm512_cvtepu32_epi64(_mm256_loadu_si256(
                        (__m256i *)(buf.tmp.ref_dis + j - fwidth / 2 + fj + 0))); // 4
                    s2 = _mm512_cvtepu32_epi64(_mm256_loadu_si256(
                        (__m256i *)(buf.tmp.ref_dis + j - fwidth / 2 + fj + 8))); // 4
                    refdis_lo = _mm512_add_epi64(refdis_lo, _mm512_mul_epu32(s0, f_tap));
                    refdis_hi = _mm512_add_epi64(refdis_hi, _mm512_mul_epu32(s2, f_tap));
                    s0 = _mm512_cvtepu32_epi64(_mm256_loadu_si256(
                        (__m256i *)(buf.tmp.ref_dis + j + fwidth / 2 - fj + 0))); // 4
                    s2 = _mm512_cvtepu32_epi64(_mm256_loadu_si256(
                        (__m256i *)(buf.tmp.ref_dis + j + fwidth / 2 - fj + 8))); // 4
                    refdis_lo = _mm512_add_epi64(refdis_lo, _mm512_mul_epu32(s0, f_tap));
                    refdis_hi = _mm512_add_epi64(refdis_hi, _mm512_mul_epu32(s2, f_tap));
                }
                refsq_lo = _mm512_srli_epi64(refsq_lo, 16);
                refsq_hi = _mm512_srli_epi64(refsq_hi, 16);
                __m512i refsq = _mm512_permutex2var_epi32(refsq_lo, mask2, refsq_hi);
                xx = _mm512_sub_epi32(refsq, mu1sq);

                dissq_lo = _mm512_srli_epi64(dissq_lo, 16);
                dissq_hi = _mm512_srli_epi64(dissq_hi, 16);
                __m512i dissq = _mm512_permutex2var_epi32(dissq_lo, mask2, dissq_hi);
                yy = _mm512_max_epi32(_mm512_sub_epi32(dissq, mu2sq), _mm512_setzero_si512());

                refdis_lo = _mm512_srli_epi64(refdis_lo, 16);
                refdis_hi = _mm512_srli_epi64(refdis_hi, 16);
                __m512i refdis = _mm512_permutex2var_epi32(refdis_lo, mask2, refdis_hi);
                xy = _mm512_sub_epi32(refdis, mu1mu2);
            }
            vif_statistic_avx512(&residuals, xx, xy, yy, log2_table, vif_enhn_gain_limit);
        }

        if ((n << 4) != (int)w) {
            VifResiduals tail_residuals = vif_compute_line_residuals(s, n << 4, w, scale);
            accum_num_log += tail_residuals.accum_num_log;
            accum_den_log += tail_residuals.accum_den_log;
            accum_num_non_log += tail_residuals.accum_num_non_log;
            accum_den_non_log += tail_residuals.accum_den_non_log;
        }
    }

    accum_num_log += _mm512_reduce_add_epi64(residuals.maccum_num_log);
    accum_den_log += _mm512_reduce_add_epi64(residuals.maccum_den_log);
    accum_num_non_log += _mm512_reduce_add_epi64(residuals.maccum_num_non_log);
    accum_den_non_log += _mm512_reduce_add_epi64(residuals.maccum_den_non_log);

    /**
        * In floating-point there are two types of numerator scores and denominator scores
        * 1. num = 1 - sigma1_sq * constant den =1  when sigma1_sq<2  here constant=4/(255*255)
        * 2. num = log2(((sigma2_sq+2)*sigma1_sq)/((sigma2_sq+2)*sigma1_sq-sigma12*sigma12) den=log2(1+(sigma1_sq/2)) else
        *
        * In fixed-point separate accumulator is used for non-log score accumulations and log-based score accumulation
        * For non-log accumulator of numerator, only sigma1_sq * constant in fixed-point is accumulated
        * log based values are separately accumulated.
        * While adding both accumulator values the non-log accumulator is converted such that it is equivalent to 1 - sigma1_sq * constant(1's are accumulated with non-log denominator accumulator)
    */
    /* log has to be divided by 2048 as log_value = log2(i*2048)  i=16384 to 65535 */
    num[0] =
        accum_num_log / 2048.0 + (accum_den_non_log - ((accum_num_non_log) / 16384.0) / (65025.0));
    den[0] = accum_den_log / 2048.0 + accum_den_non_log;
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
        __m512i rv, rlo, dv, dlo;

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

/**
 * AVX-512 8-bit VIF subsampled-readout (32-wide port of `vif_subsample_rd_8_avx2`).
 *
 * Upstream-mirror kernel; bit-exact with the scalar reference in
 * `libvmaf/src/feature/integer_vif.c`. The body is one large function on
 * purpose — the per-row reduction order across `accum_mu1_*`,
 * `accum_mu2_*`, `accum_ref_sq_*`, `accum_dis_sq_*`, and `accum_ref_dis_*`
 * is the bit-exactness invariant (ADR-0138 / ADR-0139). Splitting the
 * function would re-order partial sums and produce ULP drift visible in
 * `/cross-backend-diff` against the scalar path.
 *
 * `mask1` / `mask2` / `mask3` are pre-computed lane permutations that
 * fold the 32-wide vertical-pass results back into 16 horizontally-
 * adjacent pixel pairs for the sub-2x downsample. The `M = 1<<16`
 * trick packs two 16-bit lane indices into each `_mm512_set_epi32`
 * lane because `_mm512_set_epi16` is not constexpr-friendly under
 * older MSVC — see commented reference form just above.
 *
 * Caller contract is identical to `vif_subsample_rd_8_avx2`: `buf.ref` /
 * `buf.dis` are 8-bit Y planes, output written half-resolution into
 * `buf.mu*` / `buf.*_sq` / `buf.ref_dis`.
 */
void vif_subsample_rd_8_avx512(const VifBuffer *buf, unsigned w, unsigned h)
{
    const unsigned fwidth = vif_filter1d_width[1];
    const uint16_t *vif_filt_s1 = vif_filter1d_table[1];
    const uint8_t *ref = (uint8_t *)buf->ref;
    const uint8_t *dis = (uint8_t *)buf->dis;
    const ptrdiff_t stride = buf->stride_16 / sizeof(uint16_t);

    /* ADR-0503: filter constants are collected into two structs so that the
     * noinline per-row helpers (vif_subsample_rd_8_vert_j and
     * vif_subsample_rd_8_horiz_j) each receive only the constants relevant
     * to their pass — avoiding the ~30-ZMM live-set that caused the spill
     * cluster in the original monolithic body. */

    /* Vertical-pass coefficient struct (populated once, pointer passed per j). */
    VifVertCoeffs8 vc;
    vc.f0 = _mm512_broadcastd_epi32(_mm_loadu_si128((__m128i *)vif_filt_s1));
    vc.f1 = _mm512_broadcastd_epi32(_mm_loadu_si128((__m128i *)(vif_filt_s1 + 2)));
    vc.f2 = _mm512_broadcastd_epi32(_mm_loadu_si128((__m128i *)(vif_filt_s1 + 4)));
    vc.f3 = _mm512_broadcastd_epi32(_mm_loadu_si128((__m128i *)(vif_filt_s1 + 6)));
    vc.f4 = _mm512_broadcastd_epi32(_mm_loadu_si128((__m128i *)(vif_filt_s1 + 8)));
    vc.mask2 = _mm512_set_epi64(11, 10, 3, 2, 9, 8, 1, 0);
    vc.mask3 = _mm512_set_epi64(15, 14, 7, 6, 13, 12, 5, 4);
    vc.x = _mm512_set1_epi32(128);

    /* Horizontal-pass coefficient struct. */
    /* mask1 packs two 16-bit lane indices per epi32 element because
     * _mm512_set_epi16 is not constexpr-friendly under older MSVC. */
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

    int fwidth_half = fwidth >> 1;

    for (unsigned i = 0; i < h; ++i) {
        //VERTICAL
        int n = w >> 5;
        int ii = (int)i - fwidth_half;
        for (int j = 0; j < n << 5; j = j + 32) {
            vif_subsample_rd_8_vert_j(ref, dis, buf->stride, ii, j, &vc, buf->tmp.ref_convol,
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

        PADDING_SQ_DATA_2(buf, w, fwidth_half);

        //HORIZONTAL
        n = w >> 4;
        for (int j = 0; j < n << 4; j = j + 16) {
            int jj = j - fwidth_half;
            int jj_check = jj;
            vif_subsample_rd_8_horiz_j(buf->tmp.ref_convol, buf->tmp.dis_convol, jj_check, &hc,
                                       buf->mu1 + i * stride + j, buf->mu2 + i * stride + j);
        }

        for (unsigned j = n << 4; j < w; ++j) {
            uint32_t accum_ref = 0;
            uint32_t accum_dis = 0;
            int jj = j - fwidth_half;
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
    decimate_and_pad(buf, w, h, 0);
}

void vif_subsample_rd_16_avx512(const VifBuffer *buf, unsigned w, unsigned h, int scale, int bpc)
{
    const unsigned fwidth = vif_filter1d_width[scale + 1];
    const uint16_t *vif_filt = vif_filter1d_table[scale + 1];
    int32_t add_shift_round_VP;
    int32_t shift_VP;
    int fwidth_half = fwidth >> 1;
    const ptrdiff_t stride = buf->stride / sizeof(uint16_t);
    const ptrdiff_t stride16 = buf->stride_16 / sizeof(uint16_t);
    uint16_t *ref = buf->ref;
    uint16_t *dis = buf->dis;

    if (scale == 0) {
        add_shift_round_VP = 1 << (bpc - 1);
        shift_VP = bpc;
    } else {
        add_shift_round_VP = 32768;
        shift_VP = 16;
    }

    for (unsigned i = 0; i < h; ++i) {
        //VERTICAL

        int n = w >> 4;
        int ii = (int)i - fwidth_half;
        for (int j = 0; j < n << 4; j = j + 32) {
            int ii_check = ii;
            __m512i accumr_lo;
            __m512i accumr_hi;
            __m512i accumd_lo;
            __m512i accumd_hi;
            __m512i rmul1;
            __m512i rmul2;
            __m512i dmul1;
            __m512i dmul2;
            // NOLINTNEXTLINE(clang-analyzer-deadcode.DeadStores): chained zero-init reset every fi iteration; upstream-verbatim AVX-512 VIF chunk.
            accumr_lo = accumr_hi = accumd_lo = accumd_hi = rmul1 = rmul2 = dmul1 = dmul2 =
                _mm512_setzero_si512();
            __m512i mask3 = _mm512_set_epi64(11, 10, 3, 2, 9, 8, 1, 0);   //first half of 512
            __m512i mask4 = _mm512_set_epi64(15, 14, 7, 6, 13, 12, 5, 4); //second half of 512
            for (unsigned fi = 0; fi < fwidth; ++fi, ii_check = ii + fi) {

                const uint16_t fcoeff = vif_filt[fi];
                __m512i f1 = _mm512_set1_epi16(fcoeff);
                __m512i ref1 = _mm512_loadu_si512((__m512i *)(ref + (ii_check * stride) + j));
                __m512i dis1 = _mm512_loadu_si512((__m512i *)(dis + (ii_check * stride) + j));
                __m512i result2 = _mm512_mulhi_epu16(ref1, f1);
                __m512i result2lo = _mm512_mullo_epi16(ref1, f1);
                rmul1 = _mm512_unpacklo_epi16(result2lo, result2);
                rmul2 = _mm512_unpackhi_epi16(result2lo, result2);
                accumr_lo = _mm512_add_epi32(accumr_lo, rmul1);
                accumr_hi = _mm512_add_epi32(accumr_hi, rmul2);

                __m512i d0 = _mm512_mulhi_epu16(dis1, f1);
                __m512i d0lo = _mm512_mullo_epi16(dis1, f1);
                dmul1 = _mm512_unpacklo_epi16(d0lo, d0);
                dmul2 = _mm512_unpackhi_epi16(d0lo, d0);
                accumd_lo = _mm512_add_epi32(accumd_lo, dmul1);
                accumd_hi = _mm512_add_epi32(accumd_hi, dmul2);
            }
            __m512i addnum = _mm512_set1_epi32(add_shift_round_VP);
            accumr_lo = _mm512_add_epi32(accumr_lo, addnum);
            accumr_hi = _mm512_add_epi32(accumr_hi, addnum);
            accumr_lo = _mm512_srli_epi32(accumr_lo, shift_VP);
            accumr_hi = _mm512_srli_epi32(accumr_hi, shift_VP);

            _mm512_storeu_si512((__m512i *)(buf->tmp.ref_convol + j),
                                _mm512_permutex2var_epi64(accumr_lo, mask3, accumr_hi));
            _mm512_storeu_si512((__m512i *)(buf->tmp.ref_convol + j + 16),
                                _mm512_permutex2var_epi64(accumr_lo, mask4, accumr_hi));

            accumd_lo = _mm512_add_epi32(accumd_lo, addnum);
            accumd_hi = _mm512_add_epi32(accumd_hi, addnum);
            accumd_lo = _mm512_srli_epi32(accumd_lo, shift_VP);
            accumd_hi = _mm512_srli_epi32(accumd_hi, shift_VP);
            _mm512_storeu_si512((__m512i *)(buf->tmp.dis_convol + j),
                                _mm512_permutex2var_epi64(accumd_lo, mask3, accumd_hi));
            _mm512_storeu_si512((__m512i *)(buf->tmp.dis_convol + j + 16),
                                _mm512_permutex2var_epi64(accumd_lo, mask4, accumd_hi));
        }

        // //VERTICAL
        for (unsigned j = n << 4; j < w; ++j) {
            uint32_t accum_ref = 0;
            uint32_t accum_dis = 0;
            int ii_check = ii;
            for (unsigned fi = 0; fi < fwidth; ++fi, ii_check = ii + fi) {
                const uint16_t fcoeff = vif_filt[fi];
                accum_ref += fcoeff * ((uint32_t)ref[ii_check * stride + j]);
                accum_dis += fcoeff * ((uint32_t)dis[ii_check * stride + j]);
            }
            buf->tmp.ref_convol[j] = (uint16_t)((accum_ref + add_shift_round_VP) >> shift_VP);
            buf->tmp.dis_convol[j] = (uint16_t)((accum_dis + add_shift_round_VP) >> shift_VP);
        }

        PADDING_SQ_DATA_2(buf, w, fwidth_half);

        //HORIZONTAL
        n = w >> 4;
        for (int j = 0; j < n << 4; j = j + 16) {
            int jj = j - fwidth_half;
            int jj_check = jj;
            __m512i accumrlo;
            __m512i accumdlo;
            __m512i accumrhi;
            __m512i accumdhi;
            accumrlo = accumdlo = accumrhi = accumdhi = _mm512_setzero_si512();
            for (unsigned fj = 0; fj < fwidth; ++fj, jj_check = jj + fj) {

                __m512i refconvol = _mm512_loadu_si512((__m512i *)(buf->tmp.ref_convol + jj_check));
                __m512i fcoeff = _mm512_set1_epi16(vif_filt[fj]);
                __m512i result2 = _mm512_mulhi_epu16(refconvol, fcoeff);
                __m512i result2lo = _mm512_mullo_epi16(refconvol, fcoeff);
                accumrlo = _mm512_add_epi32(accumrlo, _mm512_unpacklo_epi16(result2lo, result2));
                accumrhi = _mm512_add_epi32(accumrhi, _mm512_unpackhi_epi16(result2lo, result2));
                __m512i disconvol = _mm512_loadu_si512((__m512i *)(buf->tmp.dis_convol + jj_check));
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

            // __m512i mask2 = _mm512_set_epi16(60, 56, 28, 24, 52, 48, 20, 16, 44,
            //                                  40, 12, 8, 36, 32, 4, 0, 60, 56, 28, 24,
            //                                  52, 48, 20, 16, 44, 40, 12, 8, 36, 32, 4, 0);
            const int M = 1 << 16;
            __m512i mask2 = _mm512_set_epi32(60 * M + 56, 28 * M + 24, 52 * M + 48, 20 * M + 16,
                                             44 * M + 40, 12 * M + 8, 36 * M + 32, 4 * M + 0,
                                             60 * M + 56, 28 * M + 24, 52 * M + 48, 20 * M + 16,
                                             44 * M + 40, 12 * M + 8, 36 * M + 32, 4 * M + 0);

            _mm256_storeu_si256(
                (__m256i *)(buf->mu1 + (stride16 * i) + j),
                _mm512_castsi512_si256(_mm512_permutex2var_epi16(accumrlo, mask2, accumrhi)));
            _mm256_storeu_si256(
                (__m256i *)(buf->mu2 + (stride16 * i) + j),
                _mm512_castsi512_si256(_mm512_permutex2var_epi16(accumdlo, mask2, accumdhi)));
        }

        for (unsigned j = n << 4; j < w; ++j) {
            uint32_t accum_ref = 0;
            uint32_t accum_dis = 0;
            int jj = j - fwidth_half;
            int jj_check = jj;
            for (unsigned fj = 0; fj < fwidth; ++fj, jj_check = jj + fj) {
                const uint16_t fcoeff = vif_filt[fj];
                accum_ref += fcoeff * ((uint32_t)buf->tmp.ref_convol[jj_check]);
                accum_dis += fcoeff * ((uint32_t)buf->tmp.dis_convol[jj_check]);
            }
            buf->mu1[i * stride16 + j] = (uint16_t)((accum_ref + 32768) >> 16);
            buf->mu2[i * stride16 + j] = (uint16_t)((accum_dis + 32768) >> 16);
        }
    }
    decimate_and_pad(buf, w, h, scale);
}
