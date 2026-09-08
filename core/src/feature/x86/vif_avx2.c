/**
 *
 *  Copyright 2016-2026 Netflix, Inc.
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

#include <stdio.h>
#include <immintrin.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include "vif_avx2.h"
#include "feature/common/macros.h"

/* Preserve Netflix NULL spelling and avoid relying on undocumented MSVC C
 * nullptr support (ADR-1138); the C++ nullptr ratchet remains unchanged. */
// NOLINTBEGIN(modernize-use-nullptr)

#define MIN(x, y) (((x) < (y)) ? (x) : (y))
#define MAX(x, y) (((x) > (y)) ? (x) : (y))

#if defined(__GNUC__)
#define ALIGNED(x) __attribute__((aligned(x)))
#elif defined(_MSC_VER)
#define ALIGNED(x) __declspec(align(x))
#else
#define ALIGNED(x)
#endif

static FORCE_INLINE void pad_top_and_bottom(const VifBuffer *buf, unsigned h, int fwidth)
{
    const unsigned fwidth_half = fwidth / 2;
    unsigned char *ref = buf->ref;
    unsigned char *dis = buf->dis;
    for (unsigned i = 1; i <= fwidth_half; ++i) {
        size_t offset = (size_t)buf->stride * i;
        memcpy(ref - offset, ref + offset, (size_t)buf->stride);
        memcpy(dis - offset, dis + offset, (size_t)buf->stride);
        memcpy(ref + (size_t)buf->stride * (h - 1) + (size_t)buf->stride * i,
               ref + (size_t)buf->stride * (h - 1) - (size_t)buf->stride * i, (size_t)buf->stride);
        memcpy(dis + (size_t)buf->stride * (h - 1) + (size_t)buf->stride * i,
               dis + (size_t)buf->stride * (h - 1) - (size_t)buf->stride * i, (size_t)buf->stride);
    }
}

static FORCE_INLINE void copy_and_pad(const VifBuffer *buf, unsigned w, unsigned h, int scale)
{
    uint16_t *ref = buf->ref;
    uint16_t *dis = buf->dis;
    const ptrdiff_t stride = buf->stride / sizeof(uint16_t);
    const ptrdiff_t mu_stride = buf->stride_16 / sizeof(uint16_t);

    for (unsigned i = 0; i < h / 2; ++i) {
        for (unsigned j = 0; j < w / 2; ++j) {
            ref[i * stride + j] = buf->mu1[i * mu_stride + j];
            dis[i * stride + j] = buf->mu2[i * mu_stride + j];
        }
    }
    pad_top_and_bottom(buf, h / 2, vif_filter1d_width[scale]);
}

// multiply r0 * f and store in 32-bit accumulators (shuffled 0 1 2 3 8 9 10 11 / 4 5 6 7 12 13 14 15)
#define multiply2(acc_left, acc_right, r0, f)                                                      \
    {                                                                                              \
        __m256i zero = _mm256_setzero_si256();                                                     \
        (acc_left) = _mm256_madd_epi16(_mm256_unpacklo_epi16(r0, zero), f);                        \
        (acc_right) = _mm256_madd_epi16(_mm256_unpackhi_epi16(r0, zero), f);                       \
    }

// multiply r0 * f and r1 * f and store in 32-bit accumulators (shuffled 0 1 2 3 8 9 10 11 / 4 5 6 7 12 13 14 15)
#define multiply2_and_accumulate(acc_left, acc_right, r0, r1, f)                                   \
    (acc_left) =                                                                                   \
        _mm256_add_epi32((acc_left), _mm256_madd_epi16(_mm256_unpacklo_epi16(r0, r1), f));         \
    (acc_right) =                                                                                  \
        _mm256_add_epi32((acc_right), _mm256_madd_epi16(_mm256_unpackhi_epi16(r0, r1), f));

// compute r0 * r1 * f and set 32-bit accumulators (shuffled 0 1 2 3 8 9 10 11 / 4 5 6 7 12 13 14 15)
#define multiply3(accum_ref_left, accum_ref_right, r0, r1, f)                                      \
    {                                                                                              \
        __m256i mul = _mm256_mullo_epi16(r0, r1);                                                  \
        __m256i lo = _mm256_mullo_epi16(mul, f);                                                   \
        __m256i hi = _mm256_mulhi_epu16(mul, f);                                                   \
        (accum_ref_left) = _mm256_unpacklo_epi16(lo, hi);                                          \
        (accum_ref_right) = _mm256_unpackhi_epi16(lo, hi);                                         \
    }

// compute r0 * r1 * f and add to 32-bit accumulators (shuffled 0 1 2 3 8 9 10 11 / 4 5 6 7 12 13 14 15)
#define multiply3_and_accumulate(accum_ref_left, accum_ref_right, r0, r1, f)                       \
    {                                                                                              \
        __m256i mul = _mm256_mullo_epi16(r0, r1);                                                  \
        __m256i lo = _mm256_mullo_epi16(mul, f);                                                   \
        __m256i hi = _mm256_mulhi_epu16(mul, f);                                                   \
        __m256i left = _mm256_unpacklo_epi16(lo, hi);                                              \
        __m256i right = _mm256_unpackhi_epi16(lo, hi);                                             \
        (accum_ref_left) = _mm256_add_epi32((accum_ref_left), left);                               \
        (accum_ref_right) = _mm256_add_epi32((accum_ref_right), right);                            \
    }

#define shuffle_and_save(addr, x, y)                                                               \
    {                                                                                              \
        __m256i left = _mm256_permute2x128_si256(x, y, 0x20);                                      \
        __m256i right = _mm256_permute2x128_si256(x, y, 0x31);                                     \
        _mm256_storeu_si256((__m256i *)(addr), left);                                              \
        _mm256_storeu_si256(((__m256i *)(addr)) + 1, right);                                       \
    }

typedef struct VifVertical256 {
    __m256i accum_ref_left;
    __m256i accum_ref_right;
    __m256i accum_dis_left;
    __m256i accum_dis_right;
    __m256i accum_ref_dis_left;
    __m256i accum_ref_dis_right;
    __m256i accum_mu2_left;
    __m256i accum_mu2_right;
    __m256i accum_mu1_left;
    __m256i accum_mu1_right;
} VifVertical256;

/* The 17-tap center/pair/store order matches the original AVX2 kernel. */
static FORCE_INLINE void vif_vertical8_init(const VifBuffer *buf, unsigned i, unsigned jj,
                                            VifVertical256 *a)
{
    const unsigned fwidth = 17;
    const uint16_t *vif_filt_s0 = vif_filter1d_table[0];
    __m256i f0 = _mm256_set1_epi16(vif_filt_s0[fwidth / 2]);
    __m256i r0 = _mm256_cvtepu8_epi16(
        _mm_loadu_si128((__m128i *)(((uint8_t *)buf->ref) + (buf->stride * i) + jj)));
    __m256i d0 = _mm256_cvtepu8_epi16(
        _mm_loadu_si128((__m128i *)(((uint8_t *)buf->dis) + (buf->stride * i) + jj)));

    // filtered r,d
    multiply2(a->accum_mu1_left, a->accum_mu1_right, r0, f0);
    multiply2(a->accum_mu2_left, a->accum_mu2_right, d0, f0);

    // filtered(r * r, d * d, r * d)
    multiply3(a->accum_ref_left, a->accum_ref_right, r0, r0, f0);
    multiply3(a->accum_dis_left, a->accum_dis_right, d0, d0, f0);
    multiply3(a->accum_ref_dis_left, a->accum_ref_dis_right, d0, r0, f0);
}

static FORCE_INLINE void vif_vertical8_tap(const VifBuffer *buf, unsigned i, unsigned jj,
                                           unsigned tap, VifVertical256 *a)
{
    const unsigned fwidth = 17;
    const uint16_t *vif_filt_s0 = vif_filter1d_table[0];
    int ii_check = i - fwidth / 2 + tap;
    int ii_check_1 = i + fwidth / 2 - tap;

    __m256i f_tap = _mm256_set1_epi16(vif_filt_s0[tap]);
    __m256i r_top = _mm256_cvtepu8_epi16(
        _mm_loadu_si128((__m128i *)(((uint8_t *)buf->ref) + (buf->stride * ii_check) + jj)));
    __m256i r_bot = _mm256_cvtepu8_epi16(
        _mm_loadu_si128((__m128i *)(((uint8_t *)buf->ref) + (buf->stride * (ii_check_1)) + jj)));
    __m256i d_top = _mm256_cvtepu8_epi16(
        _mm_loadu_si128((__m128i *)(((uint8_t *)buf->dis) + (buf->stride * ii_check) + jj)));
    __m256i d_bot = _mm256_cvtepu8_epi16(
        _mm_loadu_si128((__m128i *)(((uint8_t *)buf->dis) + (buf->stride * (ii_check_1)) + jj)));

    // accumulate filtered r,d
    multiply2_and_accumulate(a->accum_mu1_left, a->accum_mu1_right, r_top, r_bot, f_tap);
    multiply2_and_accumulate(a->accum_mu2_left, a->accum_mu2_right, d_top, d_bot, f_tap);

    // accumulate filtered(r * r, d * d, r * d)
    multiply3_and_accumulate(a->accum_ref_left, a->accum_ref_right, r_top, r_top, f_tap);
    multiply3_and_accumulate(a->accum_ref_left, a->accum_ref_right, r_bot, r_bot, f_tap);
    multiply3_and_accumulate(a->accum_dis_left, a->accum_dis_right, d_top, d_top, f_tap);
    multiply3_and_accumulate(a->accum_dis_left, a->accum_dis_right, d_bot, d_bot, f_tap);
    multiply3_and_accumulate(a->accum_ref_dis_left, a->accum_ref_dis_right, d_top, r_top, f_tap);
    multiply3_and_accumulate(a->accum_ref_dis_left, a->accum_ref_dis_right, d_bot, r_bot, f_tap);
}

static FORCE_INLINE void vif_vertical8_store(const VifBuffer *buf, unsigned jj, VifVertical256 *a)
{
    __m256i x = _mm256_set1_epi32(128);

    a->accum_mu1_left = _mm256_add_epi32(a->accum_mu1_left, x);
    a->accum_mu1_right = _mm256_add_epi32(a->accum_mu1_right, x);
    a->accum_mu2_left = _mm256_add_epi32(a->accum_mu2_left, x);
    a->accum_mu2_right = _mm256_add_epi32(a->accum_mu2_right, x);

    a->accum_mu1_left = _mm256_srli_epi32(a->accum_mu1_left, 0x08);
    a->accum_mu1_right = _mm256_srli_epi32(a->accum_mu1_right, 0x08);
    a->accum_mu2_left = _mm256_srli_epi32(a->accum_mu2_left, 0x08);
    a->accum_mu2_right = _mm256_srli_epi32(a->accum_mu2_right, 0x08);

    shuffle_and_save(buf->tmp.mu1 + jj, a->accum_mu1_left, a->accum_mu1_right);
    shuffle_and_save(buf->tmp.mu2 + jj, a->accum_mu2_left, a->accum_mu2_right);
    shuffle_and_save(buf->tmp.ref + jj, a->accum_ref_left, a->accum_ref_right);
    shuffle_and_save(buf->tmp.dis + jj, a->accum_dis_left, a->accum_dis_right);
    shuffle_and_save(buf->tmp.ref_dis + jj, a->accum_ref_dis_left, a->accum_ref_dis_right);
}

static FORCE_INLINE void vif_vertical8_tail(const VifBuffer *buf, unsigned i, unsigned j)
{
    const unsigned fwidth = 17;
    const uint16_t *vif_filt_s0 = vif_filter1d_table[0];
    uint32_t accum_mu1 = 0;
    uint32_t accum_mu2 = 0;
    uint64_t accum_ref = 0;
    uint64_t accum_dis = 0;
    uint64_t accum_ref_dis = 0;

    for (unsigned fi = 0; fi < fwidth; ++fi) {
        int ii = i - fwidth / 2;
        int ii_check = ii + fi;
        const uint16_t fcoeff = vif_filt_s0[fi];
        const uint8_t *ref = (uint8_t *)buf->ref;
        const uint8_t *dis = (uint8_t *)buf->dis;
        uint16_t imgcoeff_ref = ref[ii_check * buf->stride + j];
        uint16_t imgcoeff_dis = dis[ii_check * buf->stride + j];
        uint32_t img_coeff_ref = fcoeff * (uint32_t)imgcoeff_ref;
        uint32_t img_coeff_dis = fcoeff * (uint32_t)imgcoeff_dis;
        accum_mu1 += img_coeff_ref;
        accum_mu2 += img_coeff_dis;
        accum_ref += img_coeff_ref * (uint64_t)imgcoeff_ref;
        accum_dis += img_coeff_dis * (uint64_t)imgcoeff_dis;
        accum_ref_dis += img_coeff_ref * (uint64_t)imgcoeff_dis;
    }

    buf->tmp.mu1[j] = (accum_mu1 + 128) >> 8;
    buf->tmp.mu2[j] = (accum_mu2 + 128) >> 8;
    buf->tmp.ref[j] = accum_ref;
    buf->tmp.dis[j] = accum_dis;
    buf->tmp.ref_dis[j] = accum_ref_dis;
}

static FORCE_INLINE void vif_vertical8_row(const VifBuffer *buf, unsigned w, unsigned i)
{
    const unsigned n = w >> 4;
    for (unsigned jj = 0; jj < n << 4; jj += 16) {
        VifVertical256 a;
        vif_vertical8_init(buf, i, jj, &a);
        for (unsigned tap = 0; tap < 17 / 2; ++tap)
            vif_vertical8_tap(buf, i, jj, tap, &a);
        vif_vertical8_store(buf, jj, &a);
    }
    for (unsigned j = n << 4; j < w; ++j)
        vif_vertical8_tail(buf, i, j);
}

static FORCE_INLINE int64_t vif_num_log256(const uint16_t *log2_table, double vif_enhn_gain_limit,
                                           int32_t sigma1_sq, int32_t sigma2_sq, int32_t sigma12)
{
    static const int32_t sigma_nsq = 65536 << 1;
    /**
    * In floating-point numerator = log2((1.0f + (g * g * sigma1_sq)/(sv_sq + sigma_nsq))
    *
    * In Fixed-point the above is converted to
    * numerator = log2((sv_sq + sigma_nsq)+(g * g * sigma1_sq))- log2(sv_sq + sigma_nsq)
    */

    const double eps = 65536 * 1.0e-10;
    double g = sigma12 / (sigma1_sq + eps); // this epsilon can go away
    int32_t sv_sq = sigma2_sq - g * sigma12;

    sv_sq = (uint32_t)(MAX(sv_sq, 0));

    g = MIN(g, vif_enhn_gain_limit);

    uint32_t numer1 = (sv_sq + sigma_nsq);
    int64_t numer1_tmp = (int64_t)((g * g * sigma1_sq)) + numer1; //numerator
    return log2_64(log2_table, numer1_tmp) - log2_64(log2_table, numer1);
}

static FORCE_INLINE void vif_accumulate_pixel256(const VifPublicState *s, VifResiduals *totals,
                                                 int32_t sigma1_sq, int32_t sigma2_sq,
                                                 int32_t sigma12)
{
    static const int32_t sigma_nsq = 65536 << 1;
    if (sigma1_sq >= sigma_nsq) {
        totals->accum_den_log += log2_32(s->log2_table, sigma_nsq + sigma1_sq) - 2048 * 17;
        if (sigma12 > 0 && sigma2_sq > 0) {
            totals->accum_num_log += vif_num_log256(s->log2_table, s->vif_enhn_gain_limit,
                                                    sigma1_sq, sigma2_sq, sigma12);
        }
    } else {
        totals->accum_num_non_log += sigma2_sq;
        totals->accum_den_non_log++;
    }
}

typedef struct VifPair256 {
    __m256i lo;
    __m256i hi;
} VifPair256;

typedef struct VifQuad256 {
    __m256i a;
    __m256i b;
    __m256i c;
    __m256i d;
} VifQuad256;

/* Keep the original 64-bit adds of packed 32-bit mean products. */
static FORCE_INLINE VifPair256 vif_mean256(const uint32_t *src, unsigned scale)
{
    const uint16_t *filter = vif_filter1d_table[scale];
    const unsigned half = vif_filter1d_width[scale] / 2;
    __m256i fq = _mm256_set1_epi32(filter[half]);
    VifPair256 mean = {
        _mm256_mullo_epi32(_mm256_loadu_si256((const __m256i *)src), fq),
        _mm256_mullo_epi32(_mm256_loadu_si256((const __m256i *)(src + 8)), fq),
    };
    for (unsigned tap = 0; tap < half; ++tap) {
        __m256i f_tap = _mm256_set1_epi32(filter[tap]);
        mean.lo = _mm256_add_epi64(
            mean.lo,
            _mm256_mullo_epi32(_mm256_loadu_si256((const __m256i *)(src - half + tap)), f_tap));
        mean.hi = _mm256_add_epi64(
            mean.hi,
            _mm256_mullo_epi32(_mm256_loadu_si256((const __m256i *)(src - half + tap + 8)), f_tap));
        mean.lo = _mm256_add_epi64(
            mean.lo,
            _mm256_mullo_epi32(_mm256_loadu_si256((const __m256i *)(src + half - tap)), f_tap));
        mean.hi = _mm256_add_epi64(
            mean.hi,
            _mm256_mullo_epi32(_mm256_loadu_si256((const __m256i *)(src + half - tap + 8)), f_tap));
    }
    return mean;
}

/* The result retains the original shuffled 32-bit lane layout. */
static FORCE_INLINE __m256i vif_product8(__m256i a, __m256i b)
{
    __m256i a_lo = _mm256_unpacklo_epi32(a, _mm256_setzero_si256());
    __m256i a_hi = _mm256_unpackhi_epi32(a, _mm256_setzero_si256());
    __m256i b_lo = _mm256_unpacklo_epi32(b, _mm256_setzero_si256());
    __m256i b_hi = _mm256_unpackhi_epi32(b, _mm256_setzero_si256());
    a_lo = _mm256_mul_epu32(a_lo, b_lo);
    a_hi = _mm256_mul_epu32(a_hi, b_hi);
    a_lo = _mm256_srli_epi64(_mm256_add_epi64(a_lo, _mm256_set1_epi64x(0x80000000)), 32);
    a_hi = _mm256_srli_epi64(_mm256_add_epi64(a_hi, _mm256_set1_epi64x(0x80000000)), 32);
    return _mm256_blend_epi32(a_lo, _mm256_slli_si256(a_hi, 4), 0xAA);
}

static FORCE_INLINE VifQuad256 vif_moment8_init(const uint32_t *src)
{
    __m256i rounder = _mm256_set1_epi64x(0x8000);
    __m256i fq = _mm256_set1_epi64x(vif_filter1d_table[0][8]);
    __m256i m0 = _mm256_loadu_si256((const __m256i *)src);
    __m256i m1 = _mm256_loadu_si256((const __m256i *)(src + 8));
    VifQuad256 a = {
        _mm256_add_epi64(rounder,
                         _mm256_mul_epu32(_mm256_unpacklo_epi32(m0, _mm256_setzero_si256()), fq)),
        _mm256_add_epi64(rounder,
                         _mm256_mul_epu32(_mm256_unpackhi_epi32(m0, _mm256_setzero_si256()), fq)),
        _mm256_add_epi64(rounder,
                         _mm256_mul_epu32(_mm256_unpacklo_epi32(m1, _mm256_setzero_si256()), fq)),
        _mm256_add_epi64(rounder,
                         _mm256_mul_epu32(_mm256_unpackhi_epi32(m1, _mm256_setzero_si256()), fq)),
    };
    return a;
}

static FORCE_INLINE void vif_moment8_tap(VifQuad256 *a, const uint32_t *src, unsigned tap)
{
    __m256i f_tap = _mm256_set1_epi64x(vif_filter1d_table[0][tap]);
    __m256i m_top = _mm256_loadu_si256((const __m256i *)(src - 8 + tap));
    __m256i m_bot = _mm256_loadu_si256((const __m256i *)(src - 8 + tap + 8));
    __m256i m2 = _mm256_loadu_si256((const __m256i *)(src + 8 - tap));
    __m256i m3 = _mm256_loadu_si256((const __m256i *)(src + 8 - tap + 8));
    a->a = _mm256_add_epi64(
        a->a, _mm256_mul_epu32(_mm256_unpacklo_epi32(m_top, _mm256_setzero_si256()), f_tap));
    a->b = _mm256_add_epi64(
        a->b, _mm256_mul_epu32(_mm256_unpackhi_epi32(m_top, _mm256_setzero_si256()), f_tap));
    a->c = _mm256_add_epi64(
        a->c, _mm256_mul_epu32(_mm256_unpacklo_epi32(m_bot, _mm256_setzero_si256()), f_tap));
    a->d = _mm256_add_epi64(
        a->d, _mm256_mul_epu32(_mm256_unpackhi_epi32(m_bot, _mm256_setzero_si256()), f_tap));
    a->a = _mm256_add_epi64(
        a->a, _mm256_mul_epu32(_mm256_unpacklo_epi32(m2, _mm256_setzero_si256()), f_tap));
    a->b = _mm256_add_epi64(
        a->b, _mm256_mul_epu32(_mm256_unpackhi_epi32(m2, _mm256_setzero_si256()), f_tap));
    a->c = _mm256_add_epi64(
        a->c, _mm256_mul_epu32(_mm256_unpacklo_epi32(m3, _mm256_setzero_si256()), f_tap));
    a->d = _mm256_add_epi64(
        a->d, _mm256_mul_epu32(_mm256_unpackhi_epi32(m3, _mm256_setzero_si256()), f_tap));
}

static FORCE_INLINE VifPair256 vif_moment8(const uint32_t *src, VifPair256 mean_product)
{
    VifQuad256 a = vif_moment8_init(src);
    /* Research-2045: GCC's full unroll of these inlined 64-bit moments
     * creates spills; retain the original compact tap loop. */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC unroll 1
#endif
    for (unsigned tap = 0; tap < 8; ++tap)
        vif_moment8_tap(&a, src, tap);
    a.a = _mm256_srli_epi64(a.a, 16);
    a.b = _mm256_srli_epi64(a.b, 16);
    a.c = _mm256_srli_epi64(a.c, 16);
    a.d = _mm256_srli_epi64(a.d, 16);
    VifPair256 moment = {
        _mm256_blend_epi32(a.a, _mm256_slli_si256(a.b, 4), 0xAA),
        _mm256_blend_epi32(a.c, _mm256_slli_si256(a.d, 4), 0xAA),
    };
    moment.lo = _mm256_sub_epi32(moment.lo, mean_product.lo);
    moment.hi = _mm256_sub_epi32(moment.hi, mean_product.hi);
    moment.lo = _mm256_shuffle_epi32(moment.lo, 0xD8);
    moment.hi = _mm256_shuffle_epi32(moment.hi, 0xD8);
    return moment;
}

static FORCE_INLINE void vif_horizontal8(const VifPublicState *s, const VifBuffer *buf, unsigned j,
                                         VifResiduals *totals)
{
    ALIGNED(32) uint32_t xx[16];
    ALIGNED(32) uint32_t yy[16];
    ALIGNED(32) uint32_t xy[16];
    VifPair256 mu1 = vif_mean256(buf->tmp.mu1 + j, 0);
    VifPair256 mu1sq = {vif_product8(mu1.lo, mu1.lo), vif_product8(mu1.hi, mu1.hi)};
    VifPair256 mu2 = vif_mean256(buf->tmp.mu2 + j, 0);
    VifPair256 mu2sq = {vif_product8(mu2.lo, mu2.lo), vif_product8(mu2.hi, mu2.hi)};
    VifPair256 mu1mu2 = {vif_product8(mu1.lo, mu2.lo), vif_product8(mu1.hi, mu2.hi)};
    {
        VifPair256 ref = vif_moment8(buf->tmp.ref + j, mu1sq);
        _mm256_storeu_si256((__m256i *)&xx[0], ref.lo);
        _mm256_storeu_si256((__m256i *)&xx[8], ref.hi);
    }
    {
        VifPair256 dis = vif_moment8(buf->tmp.dis + j, mu2sq);
        _mm256_storeu_si256((__m256i *)&yy[0], _mm256_max_epi32(dis.lo, _mm256_setzero_si256()));
        _mm256_storeu_si256((__m256i *)&yy[8], _mm256_max_epi32(dis.hi, _mm256_setzero_si256()));
    }
    {
        VifPair256 ref_dis = vif_moment8(buf->tmp.ref_dis + j, mu1mu2);
        _mm256_storeu_si256((__m256i *)&xy[0], ref_dis.lo);
        _mm256_storeu_si256((__m256i *)&xy[8], ref_dis.hi);
    }
    for (unsigned b = 0; b < 16; ++b)
        vif_accumulate_pixel256(s, totals, xx[b], yy[b], xy[b]);
}

/* Research-2045: integer_vif.c assigns this function to VifState callbacks
 * taking VifPublicState *, shared with the scalar and other ISA implementations. */
// cppcheck-suppress constParameterPointer
void vif_statistic_8_avx2(struct VifPublicState *s, float *num, float *den, unsigned w, unsigned h)
{
    VifResiduals totals = {0};
    assert(vif_filter1d_width[0] == 17);
    static const unsigned fwidth = 17;
    VifBuffer buf = s->buf;

    // loop on row, each iteration produces one line of output
    for (unsigned i = 0; i < h; ++i) {
        const unsigned n = w >> 4;
        vif_vertical8_row(&buf, w, i);

        PADDING_SQ_DATA(&buf, w, fwidth / 2);

        for (unsigned j = 0; j < n << 4; j += 16)
            vif_horizontal8(s, &buf, j, &totals);
        if ((n << 4) != w) {
            VifResiduals residuals = vif_compute_line_residuals(s, n << 4, w, 0);
            totals.accum_num_log += residuals.accum_num_log;
            totals.accum_den_log += residuals.accum_den_log;
            totals.accum_num_non_log += residuals.accum_num_non_log;
            totals.accum_den_non_log += residuals.accum_den_non_log;
        }
    }

    /* log has to be divided by 2048 as log_value = log2(i*2048)  i=16384 to 65535 */
    num[0] = totals.accum_num_log / 2048.0 +
             (totals.accum_den_non_log - ((totals.accum_num_non_log) / 16384.0) / (65025.0));
    den[0] = totals.accum_den_log / 2048.0 + totals.accum_den_non_log;
}

static FORCE_INLINE VifPair256 vif_product16(VifPair256 a, VifPair256 b)
{
    VifPair256 product = {vif_product8(a.lo, b.lo), vif_product8(a.hi, b.hi)};
    product.lo = _mm256_shuffle_epi32(product.lo, 0xD8);
    product.hi = _mm256_shuffle_epi32(product.hi, 0xD8);
    return product;
}

static FORCE_INLINE __m256i vif_moment16_product(const uint32_t *src, __m256i coefficient)
{
    return _mm256_mul_epu32(_mm256_cvtepu32_epi64(_mm_loadu_si128((const __m128i *)src)),
                            coefficient);
}

static FORCE_INLINE VifQuad256 vif_moment16_init(const uint32_t *src, unsigned scale)
{
    __m256i rounder = _mm256_set1_epi64x(0x8000);
    __m256i fq = _mm256_set1_epi64x(vif_filter1d_table[scale][vif_filter1d_width[scale] / 2]);
    VifQuad256 a = {
        _mm256_add_epi64(rounder, vif_moment16_product(src, fq)),
        _mm256_add_epi64(rounder, vif_moment16_product(src + 4, fq)),
        _mm256_add_epi64(rounder, vif_moment16_product(src + 8, fq)),
        _mm256_add_epi64(rounder, vif_moment16_product(src + 12, fq)),
    };
    return a;
}

static FORCE_INLINE void vif_moment16_tap(VifQuad256 *a, const uint32_t *src, unsigned scale,
                                          unsigned tap)
{
    const unsigned half = vif_filter1d_width[scale] / 2;
    __m256i f_tap = _mm256_set1_epi64x(vif_filter1d_table[scale][tap]);
    a->a = _mm256_add_epi64(a->a, vif_moment16_product(src - half + tap, f_tap));
    a->b = _mm256_add_epi64(a->b, vif_moment16_product(src - half + tap + 4, f_tap));
    a->c = _mm256_add_epi64(a->c, vif_moment16_product(src - half + tap + 8, f_tap));
    a->d = _mm256_add_epi64(a->d, vif_moment16_product(src - half + tap + 12, f_tap));
    a->a = _mm256_add_epi64(a->a, vif_moment16_product(src + half - tap, f_tap));
    a->b = _mm256_add_epi64(a->b, vif_moment16_product(src + half - tap + 4, f_tap));
    a->c = _mm256_add_epi64(a->c, vif_moment16_product(src + half - tap + 8, f_tap));
    a->d = _mm256_add_epi64(a->d, vif_moment16_product(src + half - tap + 12, f_tap));
}

static FORCE_INLINE VifPair256 vif_pack16(VifQuad256 a)
{
    __m256i mask = _mm256_set_epi32(7, 5, 3, 1, 6, 4, 2, 0);
    a.b = _mm256_slli_si256(a.b, 4);
    a.b = _mm256_blend_epi32(a.a, a.b, 0xAA);
    a.a = _mm256_permutevar8x32_epi32(a.b, mask);
    a.d = _mm256_slli_si256(a.d, 4);
    a.d = _mm256_blend_epi32(a.c, a.d, 0xAA);
    a.b = _mm256_permutevar8x32_epi32(a.d, mask);
    VifPair256 packed = {a.a, a.b};
    return packed;
}

static FORCE_INLINE VifPair256 vif_moment16(const uint32_t *src, VifPair256 mean_product,
                                            unsigned scale)
{
    VifQuad256 a = vif_moment16_init(src, scale);
    const unsigned half = vif_filter1d_width[scale] / 2;
    for (unsigned tap = 0; tap < half; ++tap)
        vif_moment16_tap(&a, src, scale, tap);
    a.a = _mm256_srli_epi64(a.a, 16);
    a.b = _mm256_srli_epi64(a.b, 16);
    a.c = _mm256_srli_epi64(a.c, 16);
    a.d = _mm256_srli_epi64(a.d, 16);
    VifPair256 moment = vif_pack16(a);
    moment.lo = _mm256_sub_epi32(moment.lo, mean_product.lo);
    moment.hi = _mm256_sub_epi32(moment.hi, mean_product.hi);
    return moment;
}

static FORCE_INLINE void vif_horizontal16(const VifPublicState *s, unsigned j, unsigned scale,
                                          VifResiduals *totals)
{
    const VifBuffer *buf = &s->buf;
    VifPair256 mu1 = vif_mean256(buf->tmp.mu1 + j, scale);
    VifPair256 mu2 = vif_mean256(buf->tmp.mu2 + j, scale);
    VifPair256 ref = vif_moment16(buf->tmp.ref + j, vif_product16(mu1, mu1), scale);
    VifPair256 dis = vif_moment16(buf->tmp.dis + j, vif_product16(mu2, mu2), scale);
    VifPair256 ref_dis = vif_moment16(buf->tmp.ref_dis + j, vif_product16(mu1, mu2), scale);
    ALIGNED(32) uint32_t xx[16];
    ALIGNED(32) uint32_t yy[16];
    ALIGNED(32) uint32_t xy[16];
    _mm256_storeu_si256((__m256i *)&xx[0], ref.lo);
    _mm256_storeu_si256((__m256i *)&xx[8], ref.hi);
    _mm256_storeu_si256((__m256i *)&yy[0], _mm256_max_epi32(dis.lo, _mm256_setzero_si256()));
    _mm256_storeu_si256((__m256i *)&yy[8], _mm256_max_epi32(dis.hi, _mm256_setzero_si256()));
    _mm256_storeu_si256((__m256i *)&xy[0], ref_dis.lo);
    _mm256_storeu_si256((__m256i *)&xy[8], ref_dis.hi);
    for (unsigned b = 0; b < 16; ++b)
        vif_accumulate_pixel256(s, totals, xx[b], yy[b], xy[b]);
}

typedef struct VifVertical16Plan256 {
    unsigned fwidth;
    const uint16_t *vif_filt;
    int32_t add_shift_round_VP;
    int32_t shift_VP;
    int32_t add_shift_round_VP_sq;
    int32_t shift_VP_sq;
} VifVertical16Plan256;

typedef struct VifVertical16Accum256 {
    VifPair256 mu1;
    VifPair256 mu2;
    VifQuad256 ref;
    VifQuad256 ref_dis;
    VifQuad256 dis;
} VifVertical16Accum256;

static FORCE_INLINE VifVertical16Plan256 vif_vertical16_plan(int bpc, int scale)
{
    VifVertical16Plan256 p;
    p.fwidth = vif_filter1d_width[scale];
    p.vif_filt = vif_filter1d_table[scale];
    if (scale == 0) {
        p.shift_VP = bpc;
        p.add_shift_round_VP = 1 << (bpc - 1);
        p.shift_VP_sq = (bpc - 8) * 2;
        p.add_shift_round_VP_sq = (bpc == 8) ? 0 : 1 << (p.shift_VP_sq - 1);
    } else {
        p.shift_VP = 16;
        p.add_shift_round_VP = 32768;
        p.shift_VP_sq = 16;
        p.add_shift_round_VP_sq = 32768;
    }
    return p;
}

static FORCE_INLINE VifPair256 vif_multiply16(__m256i pixels, __m256i coefficient)
{
    __m256i hi = _mm256_mulhi_epu16(pixels, coefficient);
    __m256i lo = _mm256_mullo_epi16(pixels, coefficient);
    VifPair256 product = {_mm256_unpacklo_epi16(lo, hi), _mm256_unpackhi_epi16(lo, hi)};
    return product;
}

static FORCE_INLINE void vif_accumulate16_moment(VifQuad256 *a, VifPair256 product, __m256i pixels)
{
    __m256i sg0 = _mm256_cvtepu32_epi64(_mm256_castsi256_si128(product.lo));
    __m256i sg1 = _mm256_cvtepu32_epi64(_mm256_extracti128_si256(product.lo, 1));
    __m256i sg2 = _mm256_cvtepu32_epi64(_mm256_castsi256_si128(product.hi));
    __m256i sg3 = _mm256_cvtepu32_epi64(_mm256_extracti128_si256(product.hi, 1));
    __m128i l0 = _mm256_castsi256_si128(pixels);
    __m128i l1 = _mm256_extracti128_si256(pixels, 1);
    a->a = _mm256_add_epi64(a->a, _mm256_mul_epu32(sg0, _mm256_cvtepu16_epi64(l0)));
    a->b = _mm256_add_epi64(a->b,
                            _mm256_mul_epu32(sg2, _mm256_cvtepu16_epi64(_mm_bsrli_si128(l0, 8))));
    a->c = _mm256_add_epi64(a->c, _mm256_mul_epu32(sg1, _mm256_cvtepu16_epi64(l1)));
    a->d = _mm256_add_epi64(a->d,
                            _mm256_mul_epu32(sg3, _mm256_cvtepu16_epi64(_mm_bsrli_si128(l1, 8))));
}

static FORCE_INLINE void vif_vertical16_tap(const uint16_t *ref, const uint16_t *dis,
                                            uint16_t coefficient, VifVertical16Accum256 *a)
{
    __m256i f1 = _mm256_set1_epi16(coefficient);
    __m256i ref1 = _mm256_loadu_si256((const __m256i *)ref);
    __m256i dis1 = _mm256_loadu_si256((const __m256i *)dis);
    VifPair256 rmul = vif_multiply16(ref1, f1);
    a->mu1.lo = _mm256_add_epi32(a->mu1.lo, rmul.lo);
    a->mu1.hi = _mm256_add_epi32(a->mu1.hi, rmul.hi);
    VifPair256 dmul = vif_multiply16(dis1, f1);
    a->mu2.lo = _mm256_add_epi32(a->mu2.lo, dmul.lo);
    a->mu2.hi = _mm256_add_epi32(a->mu2.hi, dmul.hi);
    vif_accumulate16_moment(&a->ref, rmul, ref1);
    vif_accumulate16_moment(&a->ref_dis, rmul, dis1);
    vif_accumulate16_moment(&a->dis, dmul, dis1);
}

static FORCE_INLINE void vif_vertical16_store_mean(uint32_t *dst, VifPair256 a,
                                                   const VifVertical16Plan256 *p)
{
    __m256i bias = _mm256_set1_epi32(p->add_shift_round_VP);
    a.lo = _mm256_add_epi32(a.lo, bias);
    a.hi = _mm256_add_epi32(a.hi, bias);
    a.lo = _mm256_srli_epi32(a.lo, p->shift_VP);
    a.hi = _mm256_srli_epi32(a.hi, p->shift_VP);
    __m256i lo = _mm256_permute2x128_si256(a.lo, a.hi, 0x20);
    __m256i hi = _mm256_permute2x128_si256(a.lo, a.hi, 0x31);
    _mm256_storeu_si256((__m256i *)dst, lo);
    _mm256_storeu_si256((__m256i *)(dst + 8), hi);
}

static FORCE_INLINE void vif_vertical16_store_moment(uint32_t *dst, VifQuad256 a,
                                                     const VifVertical16Plan256 *p)
{
    __m256i bias = _mm256_set1_epi64x(p->add_shift_round_VP_sq);
    a.a = _mm256_add_epi64(a.a, bias);
    a.b = _mm256_add_epi64(a.b, bias);
    a.c = _mm256_add_epi64(a.c, bias);
    a.d = _mm256_add_epi64(a.d, bias);
    a.a = _mm256_srli_epi64(a.a, p->shift_VP_sq);
    a.b = _mm256_srli_epi64(a.b, p->shift_VP_sq);
    a.c = _mm256_srli_epi64(a.c, p->shift_VP_sq);
    a.d = _mm256_srli_epi64(a.d, p->shift_VP_sq);
    VifPair256 packed = vif_pack16(a);
    _mm256_storeu_si256((__m256i *)dst, packed.lo);
    _mm256_storeu_si256((__m256i *)(dst + 8), packed.hi);
}

static FORCE_INLINE void vif_vertical16_block(const VifBuffer *buf, int ii, unsigned j,
                                              const VifVertical16Plan256 *p)
{
    const uint16_t *ref = buf->ref;
    const uint16_t *dis = buf->dis;
    const ptrdiff_t stride = buf->stride / sizeof(uint16_t);
    VifVertical16Accum256 a = {0};
    int ii_check = ii;
    for (unsigned fi = 0; fi < p->fwidth; ++fi, ii_check = ii + fi) {
        vif_vertical16_tap(ref + ii_check * stride + j, dis + ii_check * stride + j,
                           p->vif_filt[fi], &a);
    }
    vif_vertical16_store_mean(buf->tmp.mu1 + j, a.mu1, p);
    vif_vertical16_store_mean(buf->tmp.mu2 + j, a.mu2, p);
    vif_vertical16_store_moment(buf->tmp.ref + j, a.ref, p);
    vif_vertical16_store_moment(buf->tmp.ref_dis + j, a.ref_dis, p);
    vif_vertical16_store_moment(buf->tmp.dis + j, a.dis, p);
}

static FORCE_INLINE void vif_vertical16_tail(const VifBuffer *buf, int ii, unsigned j,
                                             const VifVertical16Plan256 *p)
{
    const ptrdiff_t stride = buf->stride / sizeof(uint16_t);
    uint32_t accum_mu1 = 0;
    uint32_t accum_mu2 = 0;
    uint64_t accum_ref = 0;
    uint64_t accum_dis = 0;
    uint64_t accum_ref_dis = 0;

    int ii_check = ii;
    for (unsigned fi = 0; fi < p->fwidth; ++fi, ii_check = ii + fi) {
        const uint16_t fcoeff = p->vif_filt[fi];
        const uint16_t *ref = buf->ref;
        const uint16_t *dis = buf->dis;
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
    buf->tmp.mu1[j] = (uint16_t)((accum_mu1 + p->add_shift_round_VP) >> p->shift_VP);
    buf->tmp.mu2[j] = (uint16_t)((accum_mu2 + p->add_shift_round_VP) >> p->shift_VP);
    buf->tmp.ref[j] = (uint32_t)((accum_ref + p->add_shift_round_VP_sq) >> p->shift_VP_sq);
    buf->tmp.ref_dis[j] = (uint32_t)((accum_ref_dis + p->add_shift_round_VP_sq) >> p->shift_VP_sq);
    buf->tmp.dis[j] = (uint32_t)((accum_dis + p->add_shift_round_VP_sq) >> p->shift_VP_sq);
}

static FORCE_INLINE void vif_vertical16_row(const VifBuffer *buf, unsigned w, unsigned i,
                                            const VifVertical16Plan256 *p)
{
    const int fwidth_half = p->fwidth >> 1;
    const int ii = i - fwidth_half;
    const unsigned n = w >> 4;
    for (unsigned j = 0; j < n << 4; j += 16)
        vif_vertical16_block(buf, ii, j, p);
    for (unsigned j = n << 4; j < w; ++j)
        vif_vertical16_tail(buf, ii, j, p);
}

/* Research-2045: integer_vif.c assigns this function to VifState callbacks
 * taking VifPublicState *, shared with the scalar and other ISA implementations. */
// cppcheck-suppress constParameterPointer
void vif_statistic_16_avx2(struct VifPublicState *s, float *num, float *den, unsigned w, unsigned h,
                           int bpc, int scale)
{
    VifResiduals totals = {0};
    const unsigned fwidth = vif_filter1d_width[scale];
    VifBuffer buf = s->buf;
    const VifVertical16Plan256 plan = vif_vertical16_plan(bpc, scale);
    const int fwidth_half = fwidth >> 1;

    for (unsigned i = 0; i < h; ++i) {
        const unsigned n = w >> 4;
        vif_vertical16_row(&buf, w, i, &plan);

        PADDING_SQ_DATA(&buf, w, fwidth_half);

        for (unsigned j = 0; j < n << 4; j += 16)
            vif_horizontal16(s, j, scale, &totals);
        if ((n << 4) != w) {
            VifResiduals residuals = vif_compute_line_residuals(s, n << 4, w, scale);
            totals.accum_num_log += residuals.accum_num_log;
            totals.accum_den_log += residuals.accum_den_log;
            totals.accum_num_non_log += residuals.accum_num_non_log;
            totals.accum_den_non_log += residuals.accum_den_non_log;
        }
    }

    num[0] = totals.accum_num_log / 2048.0 +
             (totals.accum_den_non_log - ((totals.accum_num_non_log) / 16384.0) / (65025.0));
    den[0] = totals.accum_den_log / 2048.0 + totals.accum_den_non_log;
}

typedef struct VifSubsample8Taps256 {
    __m256i pixels[9];
} VifSubsample8Taps256;

static FORCE_INLINE VifSubsample8Taps256 vif_subsample8_load(const uint8_t *src, ptrdiff_t stride)
{
    VifSubsample8Taps256 taps;
    for (unsigned i = 0; i < 9; ++i)
        taps.pixels[i] = _mm256_cvtepu8_epi16(_mm_loadu_si128((const __m128i *)(src + stride * i)));
    return taps;
}

static FORCE_INLINE VifPair256 vif_subsample8_filter(const VifSubsample8Taps256 *taps)
{
    const uint16_t *filter = vif_filter1d_table[1];
    VifPair256 a;
    __m256i center = _mm256_set1_epi16(filter[4]);
    multiply2(a.lo, a.hi, taps->pixels[4], center);
    for (unsigned tap = 0; tap < 4; ++tap) {
        __m256i coefficient = _mm256_set1_epi16(filter[tap]);
        multiply2_and_accumulate(a.lo, a.hi, taps->pixels[tap], taps->pixels[8 - tap], coefficient);
    }
    return a;
}

static FORCE_INLINE void vif_subsample8_store(uint32_t *dst, VifPair256 a)
{
    __m256i x = _mm256_set1_epi32(128);
    __m256i lo = _mm256_add_epi32(x, _mm256_permute2x128_si256(a.lo, a.hi, 0x20));
    __m256i hi = _mm256_add_epi32(x, _mm256_permute2x128_si256(a.lo, a.hi, 0x31));
    lo = _mm256_srli_epi32(lo, 0x08);
    hi = _mm256_srli_epi32(hi, 0x08);
    _mm256_storeu_si256((__m256i *)dst, lo);
    _mm256_storeu_si256((__m256i *)(dst + 8), hi);
}

static FORCE_INLINE void vif_subsample8_vertical(const VifBuffer *buf, unsigned i, unsigned j)
{
    const int half = vif_filter1d_width[1] >> 1;
    const int ii = i * 2 - half;
    const uint8_t *ref = buf->ref;
    const uint8_t *dis = buf->dis;
    VifSubsample8Taps256 r = vif_subsample8_load(ref + buf->stride * ii + j, buf->stride);
    VifSubsample8Taps256 d = vif_subsample8_load(dis + buf->stride * ii + j, buf->stride);
    VifPair256 mu2 = vif_subsample8_filter(&d);
    VifPair256 mu1 = vif_subsample8_filter(&r);
    vif_subsample8_store(buf->tmp.ref_convol + j, mu1);
    vif_subsample8_store(buf->tmp.dis_convol + j, mu2);
}

static FORCE_INLINE void vif_subsample8_vertical_tail(const VifBuffer *buf, unsigned i, unsigned j)
{
    const unsigned fwidth = vif_filter1d_width[1];
    const int fwidth_half = fwidth >> 1;
    const uint16_t *vif_filt_s1 = vif_filter1d_table[1];
    const uint8_t *ref = buf->ref;
    const uint8_t *dis = buf->dis;
    uint32_t accum_ref = 0;
    uint32_t accum_dis = 0;
    for (unsigned fi = 0; fi < fwidth; ++fi) {
        int ii = i * 2 - fwidth_half;
        int ii_check = ii + fi;
        const uint16_t fcoeff = vif_filt_s1[fi];
        accum_ref += fcoeff * (uint32_t)ref[ii_check * buf->stride + j];
        accum_dis += fcoeff * (uint32_t)dis[ii_check * buf->stride + j];
    }
    buf->tmp.ref_convol[j] = (accum_ref + 128) >> 8;
    buf->tmp.dis_convol[j] = (accum_dis + 128) >> 8;
}

static FORCE_INLINE void vif_subsample_accumulate(VifPair256 *a, __m256i pixels,
                                                  uint16_t coefficient)
{
    VifPair256 product = vif_multiply16(pixels, _mm256_set1_epi16(coefficient));
    a->lo = _mm256_add_epi32(a->lo, product.lo);
    a->hi = _mm256_add_epi32(a->hi, product.hi);
}

static FORCE_INLINE VifPair256 vif_subsample8_horizontal(const uint32_t *src)
{
    const uint16_t *filter = vif_filter1d_table[1];
    VifPair256 a = {0};
    __m256i p0 = _mm256_loadu_si256((const __m256i *)src);
    __m256i p4 = _mm256_loadu_si256((const __m256i *)(src + 4));
    __m256i p8 = _mm256_loadu_si256((const __m256i *)(src + 8));
    __m256i p1 = _mm256_alignr_epi8(p4, p0, 4);
    __m256i p2 = _mm256_alignr_epi8(p4, p0, 8);
    __m256i p3 = _mm256_alignr_epi8(p4, p0, 12);
    __m256i p5 = _mm256_alignr_epi8(p8, p4, 4);
    __m256i p6 = _mm256_alignr_epi8(p8, p4, 8);
    __m256i p7 = _mm256_alignr_epi8(p8, p4, 12);
    vif_subsample_accumulate(&a, p0, filter[0]);
    vif_subsample_accumulate(&a, p1, filter[1]);
    vif_subsample_accumulate(&a, p2, filter[2]);
    vif_subsample_accumulate(&a, p3, filter[3]);
    vif_subsample_accumulate(&a, p4, filter[4]);
    vif_subsample_accumulate(&a, p5, filter[3]);
    vif_subsample_accumulate(&a, p6, filter[2]);
    vif_subsample_accumulate(&a, p7, filter[1]);
    vif_subsample_accumulate(&a, p8, filter[0]);
    return a;
}

static FORCE_INLINE __m256i vif_subsample_round(VifPair256 a)
{
    __m256i bias = _mm256_set1_epi32(32768);
    a.lo = _mm256_add_epi32(a.lo, bias);
    a.hi = _mm256_add_epi32(a.hi, bias);
    a.lo = _mm256_srli_epi32(a.lo, 0x10);
    a.hi = _mm256_srli_epi32(a.hi, 0x10);
    return _mm256_packus_epi32(a.lo, a.hi);
}

static FORCE_INLINE void vif_subsample8_horizontal_block(const VifBuffer *buf, unsigned i,
                                                         unsigned j)
{
    const int half = vif_filter1d_width[1] >> 1;
    const int jj = j - half;
    const ptrdiff_t stride = buf->stride_16 / sizeof(uint16_t);
    __m256i mask = _mm256_set_epi32(6, 4, 2, 0, 6, 4, 2, 0);
    VifPair256 r = vif_subsample8_horizontal(buf->tmp.ref_convol + jj);
    VifPair256 d = vif_subsample8_horizontal(buf->tmp.dis_convol + jj);
    __m256i result = vif_subsample_round(d);
    __m256i resultd = vif_subsample_round(r);
    resultd = _mm256_permutevar8x32_epi32(resultd, mask);
    result = _mm256_permutevar8x32_epi32(result, mask);
    resultd = _mm256_packus_epi32(resultd, resultd);
    result = _mm256_packus_epi32(result, result);
    _mm_storel_epi64((__m128i *)(buf->mu1 + i * stride + (j >> 1)),
                     _mm256_castsi256_si128(resultd));
    _mm_storel_epi64((__m128i *)(buf->mu2 + i * stride + (j >> 1)), _mm256_castsi256_si128(result));
}

static FORCE_INLINE void vif_subsample8_horizontal_tail(const VifBuffer *buf, unsigned i,
                                                        unsigned j)
{
    const unsigned fwidth = vif_filter1d_width[1];
    const int fwidth_half = fwidth >> 1;
    const uint16_t *vif_filt_s1 = vif_filter1d_table[1];
    const ptrdiff_t stride = buf->stride_16 / sizeof(uint16_t);
    uint32_t accum_ref = 0;
    uint32_t accum_dis = 0;
    int jj = j - fwidth_half;
    int jj_check = jj;
    for (unsigned fj = 0; fj < fwidth; ++fj, jj_check = jj + fj) {
        const uint16_t fcoeff = vif_filt_s1[fj];
        accum_ref += fcoeff * buf->tmp.ref_convol[jj_check];
        accum_dis += fcoeff * buf->tmp.dis_convol[jj_check];
    }
    buf->mu1[i * stride + (j >> 1)] = (uint16_t)((accum_ref + 32768) >> 16);
    buf->mu2[i * stride + (j >> 1)] = (uint16_t)((accum_dis + 32768) >> 16);
}

/* Preserve the 9-tap center/pair vertical order and left-to-right horizontal order. */
void vif_subsample_rd_8_avx2(const VifBuffer *buf, unsigned w, unsigned h)
{
    assert(buf != NULL);
    assert(w > 0u);
    assert(h > 0u);
    for (unsigned i = 0; i < h / 2; ++i) {
        unsigned n = w >> 4;
        for (unsigned j = 0; j < n << 4; j += 16)
            vif_subsample8_vertical(buf, i, j);
        for (unsigned j = n << 4; j < w; ++j)
            vif_subsample8_vertical_tail(buf, i, j);
        PADDING_SQ_DATA_2(buf, w, vif_filter1d_width[1] >> 1);
        n = w >> 3;
        for (unsigned j = 0; j < n << 3; j += 8)
            vif_subsample8_horizontal_block(buf, i, j);
        for (unsigned j = n << 3; j < w; j += 2)
            vif_subsample8_horizontal_tail(buf, i, j);
    }
    copy_and_pad(buf, w, h, 0);
}

static FORCE_INLINE void vif_subsample16_vertical(const VifBuffer *buf, int ii, unsigned j,
                                                  const VifVertical16Plan256 *p)
{
    const uint16_t *ref = buf->ref;
    const uint16_t *dis = buf->dis;
    const ptrdiff_t stride = buf->stride / sizeof(uint16_t);
    VifPair256 r = {0};
    VifPair256 d = {0};
    int ii_check = ii;
    for (unsigned fi = 0; fi < p->fwidth; ++fi, ii_check = ii + fi) {
        __m256i ref1 = _mm256_loadu_si256((const __m256i *)(ref + ii_check * stride + j));
        __m256i dis1 = _mm256_loadu_si256((const __m256i *)(dis + ii_check * stride + j));
        vif_subsample_accumulate(&r, ref1, p->vif_filt[fi]);
        vif_subsample_accumulate(&d, dis1, p->vif_filt[fi]);
    }
    vif_vertical16_store_mean(buf->tmp.ref_convol + j, r, p);
    vif_vertical16_store_mean(buf->tmp.dis_convol + j, d, p);
}

static FORCE_INLINE void vif_subsample16_vertical_tail(const VifBuffer *buf, int ii, unsigned j,
                                                       const VifVertical16Plan256 *p)
{
    const uint16_t *ref = buf->ref;
    const uint16_t *dis = buf->dis;
    const ptrdiff_t stride = buf->stride / sizeof(uint16_t);
    uint32_t accum_ref = 0;
    uint32_t accum_dis = 0;
    int ii_check = ii;
    for (unsigned fi = 0; fi < p->fwidth; ++fi, ii_check = ii + fi) {
        const uint16_t fcoeff = p->vif_filt[fi];
        accum_ref += fcoeff * ((uint32_t)ref[ii_check * stride + j]);
        accum_dis += fcoeff * ((uint32_t)dis[ii_check * stride + j]);
    }
    buf->tmp.ref_convol[j] = (uint16_t)((accum_ref + p->add_shift_round_VP) >> p->shift_VP);
    buf->tmp.dis_convol[j] = (uint16_t)((accum_dis + p->add_shift_round_VP) >> p->shift_VP);
}

static FORCE_INLINE VifPair256 vif_subsample16_horizontal(const uint32_t *src,
                                                          const VifVertical16Plan256 *p)
{
    VifPair256 a = {0};
    for (unsigned tap = 0; tap < p->fwidth; ++tap) {
        __m256i pixels = _mm256_loadu_si256((const __m256i *)(src + tap));
        vif_subsample_accumulate(&a, pixels, p->vif_filt[tap]);
    }
    return a;
}

static FORCE_INLINE void vif_subsample16_horizontal_block(const VifBuffer *buf, unsigned i,
                                                          unsigned j, const VifVertical16Plan256 *p)
{
    const int half = p->fwidth >> 1;
    const int jj = j - half;
    const ptrdiff_t stride16 = buf->stride_16 / sizeof(uint16_t);
    __m256i mask = _mm256_set_epi32(6, 4, 2, 0, 6, 4, 2, 0);
    VifPair256 r = vif_subsample16_horizontal(buf->tmp.ref_convol + jj, p);
    VifPair256 d = vif_subsample16_horizontal(buf->tmp.dis_convol + jj, p);
    __m256i result = vif_subsample_round(d);
    __m256i resultd = vif_subsample_round(r);
    __m256i resulttmp = _mm256_srli_si256(resultd, 2);
    resultd = _mm256_blend_epi16(resultd, resulttmp, 0xAA);
    resultd = _mm256_permutevar8x32_epi32(resultd, mask);
    _mm_storeu_si128((__m128i *)(buf->mu1 + i * stride16 + j), _mm256_castsi256_si128(resultd));
    resulttmp = _mm256_srli_si256(result, 2);
    result = _mm256_blend_epi16(result, resulttmp, 0xAA);
    result = _mm256_permutevar8x32_epi32(result, mask);
    _mm_storeu_si128((__m128i *)(buf->mu2 + i * stride16 + j), _mm256_castsi256_si128(result));
}

static FORCE_INLINE void vif_subsample16_horizontal_tail(const VifBuffer *buf, unsigned i,
                                                         unsigned j, const VifVertical16Plan256 *p)
{
    const int fwidth_half = p->fwidth >> 1;
    const ptrdiff_t stride16 = buf->stride_16 / sizeof(uint16_t);
    uint32_t accum_ref = 0;
    uint32_t accum_dis = 0;
    int jj = j - fwidth_half;
    int jj_check = jj;
    for (unsigned fj = 0; fj < p->fwidth; ++fj, jj_check = jj + fj) {
        const uint16_t fcoeff = p->vif_filt[fj];
        accum_ref += fcoeff * ((uint32_t)buf->tmp.ref_convol[jj_check]);
        accum_dis += fcoeff * ((uint32_t)buf->tmp.dis_convol[jj_check]);
    }
    buf->mu1[i * stride16 + j] = (uint16_t)((accum_ref + 32768) >> 16);
    buf->mu2[i * stride16 + j] = (uint16_t)((accum_dis + 32768) >> 16);
}

static FORCE_INLINE void vif_subsample16_copy(const VifBuffer *buf, unsigned w, unsigned h)
{
    uint16_t *ref = buf->ref;
    uint16_t *dis = buf->dis;
    const ptrdiff_t stride = buf->stride / sizeof(uint16_t);
    const ptrdiff_t stride16 = buf->stride_16 / sizeof(uint16_t);
    for (unsigned i = 0; i < h / 2; ++i) {
        for (unsigned j = 0; j < w / 2; ++j) {
            ref[i * stride + j] = buf->mu1[i * stride16 + ((ptrdiff_t)j * 2)];
            dis[i * stride + j] = buf->mu2[i * stride16 + ((ptrdiff_t)j * 2)];
        }
    }
}

void vif_subsample_rd_16_avx2(const VifBuffer *buf, unsigned w, unsigned h, int scale, int bpc)
{
    assert(buf != NULL);
    assert(w > 0u);
    assert(h > 0u);
    VifVertical16Plan256 plan = vif_vertical16_plan(bpc, scale);
    plan.fwidth = vif_filter1d_width[scale + 1];
    plan.vif_filt = vif_filter1d_table[scale + 1];
    const int half = plan.fwidth >> 1;
    for (unsigned i = 0; i < h / 2; ++i) {
        unsigned n = w >> 4;
        const int ii = i * 2 - half;
        for (unsigned j = 0; j < n << 4; j += 16)
            vif_subsample16_vertical(buf, ii, j, &plan);
        for (unsigned j = n << 4; j < w; ++j)
            vif_subsample16_vertical_tail(buf, ii, j, &plan);
        PADDING_SQ_DATA_2(buf, w, half);
        n = w >> 3;
        for (unsigned j = 0; j < n << 3; j += 8)
            vif_subsample16_horizontal_block(buf, i, j, &plan);
        for (unsigned j = n << 3; j < w; ++j)
            vif_subsample16_horizontal_tail(buf, i, j, &plan);
    }
    vif_subsample16_copy(buf, w, h);
    pad_top_and_bottom(buf, h / 2, vif_filter1d_width[scale]);
}

// NOLINTEND(modernize-use-nullptr)
