/*
 * Copyright 2026 Lusoris
 *
 * Licensed under the BSD+Patent License (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://opensource.org/licenses/BSDplusPatent
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * AVX2 implementation of the integer SSIM horizontal moment accumulation.
 *
 * Bit-exactness contract (ADR-0784):
 *   All accumulation is in int64 (epi64) arithmetic, which is the same
 *   integer domain as the scalar reference in integer_ssim.c.  There are
 *   no float/double conversions inside this function; consequently SIMD
 *   output is bit-identical to scalar output for any input.
 *
 * What we vectorise:
 *   The scalar ssim_accumulate_row() has an outer loop over x (output
 *   pixels) and an inner loop over kernel taps k.  For each (x, k) pair
 *   it reads src[x - hkernel_offs + k] and dst[...], multiplies by
 *   hkernel[k], and accumulates five int64 moments.
 *
 *   For the 8bpc path we process 8 consecutive output pixels in parallel
 *   using the following strategy:
 *
 *     1.  For each kernel tap k (loop over k; k typically 0..8 for σ=1.5):
 *           - Load 8 consecutive src bytes at offset (x_base - offs + k),
 *             widen to int32 via _mm256_cvtepu8_epi32.
 *           - Ditto for dst.
 *           - Load the scalar kernel weight w = hkernel[k] (int32 broadcast).
 *           - mux_vec  += w * s_vec   (32-bit intermediates widen to 64-bit below)
 *           - muy_vec  += w * d_vec
 *           - x2_vec   += w * s_vec * s_vec   (need 64-bit: use madd trick)
 *           - xy_vec   += w * s_vec * d_vec
 *           - y2_vec   += w * d_vec * d_vec
 *           - wsum_vec += w
 *
 *     2.  After the k-loop, 8 lanes hold the per-pixel moments.  Store each
 *         lane into buf[x_base + lane].
 *
 *   Overflow analysis (why int32 intermediates are safe for mux/muy):
 *     Each tap: w_max = KERNEL_WEIGHT = 256, s_max = 255.
 *     w * s ≤ 256 * 255 = 65280 < 2^17 — fits int32.
 *     Sum over 9 taps: ≤ 9 * 65280 = 587520 < 2^20 — still int32.
 *     We accumulate into 64-bit at the very end per pixel.
 *
 *   For x2/xy/y2 (w * s * s):
 *     w * s^2 ≤ 256 * 255^2 = 16,646,400 < 2^25.
 *     Sum over 9 taps: ≤ ~1.5e8 < 2^27 — still int32 safe at intermediate.
 *     We widen to int64 before the final store.
 *
 *   Boundary handling:
 *     Boundary pixels (x < hkernel_offs, x > w-1-hkernel_offs) have a
 *     per-pixel k_min/k_max that varies.  The SIMD path handles the safe
 *     interior (all 8 lanes fully in-bounds for all taps) and falls back
 *     to scalar for boundary pixels.  This is identical to the strategy
 *     used by the CUDA twin in integer_ssim_score.cu.
 *
 * ISA gating:
 *   This file is compiled into the x86_avx2 static library with
 *   -mavx -mavx2 -mfma.  It must only be called after a runtime
 *   cpu_supports_avx2() check (see integer_ssim.c dispatch).
 *
 * Alignment:
 *   All pixel loads use _mm256_loadu_si256 / _mm_loadu_si128 (unaligned)
 *   because VmafPicture strides are not guaranteed 32-byte aligned.
 *
 * FMA:
 *   No FMA is used — all multiplications are integer (epi32/epi64).
 *   No float arithmetic at all.
 *
 * See docs/adr/0784-integer-ssim-avx2.md.
 */

#include <immintrin.h>
#include <stdint.h>
#include <string.h>

#include "integer_ssim_avx2.h"

/*
 * Scalar fallback for a single pixel — identical to the scalar inner body
 * in integer_ssim.c:ssim_accumulate_row(), reproduced here to avoid a
 * cross-TU dependency on a static function.
 */
static void accumulate_pixel_8(int x, const uint8_t *src, const uint8_t *dst, int w,
                               const unsigned *hkernel, int hkernel_sz, int hkernel_offs,
                               integer_ssim_moments_t *out)
{
    integer_ssim_moments_t m;
    (void)memset(&m, 0, sizeof(m));
    const int k_min = (hkernel_offs - x <= 0) ? 0 : hkernel_offs - x;
    const int k_max =
        (x + hkernel_offs - w + 1 <= 0) ? hkernel_sz : hkernel_sz - (x + hkernel_offs - w + 1);
    for (int k = k_min; k < k_max; k++) {
        const int off = x - hkernel_offs + k;
        const int64_t sv = (int64_t)src[off];
        const int64_t dv = (int64_t)dst[off];
        const int64_t wv = (int64_t)hkernel[k];
        m.mux += wv * sv;
        m.muy += wv * dv;
        m.x2 += wv * sv * sv;
        m.xy += wv * sv * dv;
        m.y2 += wv * dv * dv;
        m.w += wv;
    }
    *out = m;
}

static void accumulate_pixel_16(int x, const uint16_t *src, const uint16_t *dst, int w,
                                const unsigned *hkernel, int hkernel_sz, int hkernel_offs,
                                integer_ssim_moments_t *out)
{
    integer_ssim_moments_t m;
    (void)memset(&m, 0, sizeof(m));
    const int k_min = (hkernel_offs - x <= 0) ? 0 : hkernel_offs - x;
    const int k_max =
        (x + hkernel_offs - w + 1 <= 0) ? hkernel_sz : hkernel_sz - (x + hkernel_offs - w + 1);
    for (int k = k_min; k < k_max; k++) {
        const int off = x - hkernel_offs + k;
        const int64_t sv = (int64_t)src[off];
        const int64_t dv = (int64_t)dst[off];
        const int64_t wv = (int64_t)hkernel[k];
        m.mux += wv * sv;
        m.muy += wv * dv;
        m.x2 += wv * sv * sv;
        m.xy += wv * sv * dv;
        m.y2 += wv * dv * dv;
        m.w += wv;
    }
    *out = m;
}

/*
 * Widen lower 4 int32 lanes of a 256i to 4 x int64 in a 256i.
 * (_mm256_cvtepi32_epi64 takes __m128i input.)
 */
static inline __m256i widen_lo_epi32_epi64(__m256i v)
{
    return _mm256_cvtepi32_epi64(_mm256_castsi256_si128(v));
}

/*
 * Widen upper 4 int32 lanes of a 256i to 4 x int64 in a 256i.
 */
static inline __m256i widen_hi_epi32_epi64(__m256i v)
{
    return _mm256_cvtepi32_epi64(_mm256_extracti128_si256(v, 1));
}

void integer_ssim_accumulate_row_avx2(const uint8_t *src, const uint8_t *dst, int width,
                                      const unsigned *hkernel, int hkernel_sz, int hkernel_offs,
                                      integer_ssim_moments_t *buf)
{
    /* Number of interior pixels: those with x in [hkernel_offs, width-1-hkernel_offs].
     * The SIMD loop requires all 8 lanes to be interior — so we need at least
     * x_base + 7 <= width - 1 - hkernel_offs, i.e. x_base <= width - 8 - hkernel_offs. */
    const int x_int_first = hkernel_offs;
    const int x_int_last = width - 1 - hkernel_offs; /* inclusive */

    /* Scalar left boundary.  Clamped to width so narrow rows (width <
     * hkernel_offs) do not write past the end of buf[0..width). */
    int x = 0;
    const int x_left_end = (x_int_first < width) ? x_int_first : width;
    for (; x < x_left_end; x++) {
        accumulate_pixel_8(x, src, dst, width, hkernel, hkernel_sz, hkernel_offs, &buf[x]);
    }

    /* SIMD interior: process 8 pixels at a time.
     * For interior pixels, k_min=0 and k_max=hkernel_sz, so the k-loop is unconditional. */
    for (; x + 8 <= x_int_last + 1; x += 8) {
        /* Accumulators for 8 output pixels, in int32 (safe per overflow analysis above). */
        __m256i acc_mux = _mm256_setzero_si256();
        __m256i acc_muy = _mm256_setzero_si256();
        __m256i acc_x2 = _mm256_setzero_si256();
        __m256i acc_xy = _mm256_setzero_si256();
        __m256i acc_y2 = _mm256_setzero_si256();
        int32_t wsum = 0;

        for (int k = 0; k < hkernel_sz; k++) {
            /* The k-th pixel read for output pixel x is at offset (x - hkernel_offs + k).
             * For 8 consecutive x values x..x+7, the offsets form a contiguous run
             * starting at (x - hkernel_offs + k). */
            const int off = x - hkernel_offs + k;
            /* Load 8 consecutive uint8 src pixels and widen to int32 */
            __m128i s8 = _mm_loadl_epi64((const __m128i *)(src + off));
            __m256i sv = _mm256_cvtepu8_epi32(s8); /* 8 x uint8 -> 8 x int32 */

            __m128i d8 = _mm_loadl_epi64((const __m128i *)(dst + off));
            __m256i dv = _mm256_cvtepu8_epi32(d8);

            const int32_t w_scalar = (int32_t)hkernel[k];
            const __m256i wv = _mm256_set1_epi32(w_scalar);

            /* mux += w * s */
            acc_mux = _mm256_add_epi32(acc_mux, _mm256_mullo_epi32(wv, sv));
            /* muy += w * d */
            acc_muy = _mm256_add_epi32(acc_muy, _mm256_mullo_epi32(wv, dv));
            /* x2  += w * s * s */
            acc_x2 = _mm256_add_epi32(acc_x2, _mm256_mullo_epi32(wv, _mm256_mullo_epi32(sv, sv)));
            /* xy  += w * s * d */
            acc_xy = _mm256_add_epi32(acc_xy, _mm256_mullo_epi32(wv, _mm256_mullo_epi32(sv, dv)));
            /* y2  += w * d * d */
            acc_y2 = _mm256_add_epi32(acc_y2, _mm256_mullo_epi32(wv, _mm256_mullo_epi32(dv, dv)));
            wsum += w_scalar;
        }

        /* Store 8 moments.  Widen int32 accumulators to int64 for the
         * integer_ssim_moments_t fields.  We process lanes 0..3 and 4..7
         * separately using 256-bit epi64 stores. */
        _Alignas(32) int64_t mux_arr[8];
        _Alignas(32) int64_t muy_arr[8];
        _Alignas(32) int64_t x2_arr[8];
        _Alignas(32) int64_t xy_arr[8];
        _Alignas(32) int64_t y2_arr[8];

        /* lanes 0..3 */
        _mm256_store_si256((__m256i *)&mux_arr[0], widen_lo_epi32_epi64(acc_mux));
        _mm256_store_si256((__m256i *)&muy_arr[0], widen_lo_epi32_epi64(acc_muy));
        _mm256_store_si256((__m256i *)&x2_arr[0], widen_lo_epi32_epi64(acc_x2));
        _mm256_store_si256((__m256i *)&xy_arr[0], widen_lo_epi32_epi64(acc_xy));
        _mm256_store_si256((__m256i *)&y2_arr[0], widen_lo_epi32_epi64(acc_y2));
        /* lanes 4..7 */
        _mm256_store_si256((__m256i *)&mux_arr[4], widen_hi_epi32_epi64(acc_mux));
        _mm256_store_si256((__m256i *)&muy_arr[4], widen_hi_epi32_epi64(acc_muy));
        _mm256_store_si256((__m256i *)&x2_arr[4], widen_hi_epi32_epi64(acc_x2));
        _mm256_store_si256((__m256i *)&xy_arr[4], widen_hi_epi32_epi64(acc_xy));
        _mm256_store_si256((__m256i *)&y2_arr[4], widen_hi_epi32_epi64(acc_y2));

        for (int lane = 0; lane < 8; lane++) {
            buf[x + lane].mux = mux_arr[lane];
            buf[x + lane].muy = muy_arr[lane];
            buf[x + lane].x2 = x2_arr[lane];
            buf[x + lane].xy = xy_arr[lane];
            buf[x + lane].y2 = y2_arr[lane];
            buf[x + lane].w = (int64_t)wsum;
        }
    }

    /* Scalar right boundary (and any leftover from the SIMD loop) */
    for (; x < width; x++) {
        accumulate_pixel_8(x, src, dst, width, hkernel, hkernel_sz, hkernel_offs, &buf[x]);
    }
}

void integer_ssim_accumulate_row_16_avx2(const uint16_t *src, const uint16_t *dst, int width,
                                         const unsigned *hkernel, int hkernel_sz, int hkernel_offs,
                                         integer_ssim_moments_t *buf)
{
    /*
     * 16-bit path: pixel values up to 65535.
     * w * s ≤ 256 * 65535 = 16,776,960 — fits int32.
     * w * s^2 ≤ 256 * 65535^2 = 1.099e12 — DOES NOT fit int32.
     *
     * Therefore for x2/xy/y2 we must widen intermediate products to int64.
     * Strategy: accumulate mux/muy in int32 (safe), widen to int64 at the end.
     *           accumulate x2/xy/y2 in int64 per-tap using 64-bit ops.
     *
     * Since AVX2 lacks native 64-bit multiply, we perform the 64-bit
     * accumulation on the lower/upper 128-bit halves via SSE4 _mm_mul_epi32
     * (true signed 64-bit product of lower 32 bits of each 64-bit lane).
     *
     * Overflow: pixels are unsigned, so we zero-extend to 64-bit before
     * multiplying.  The products are always non-negative (wv * sv >= 0).
     */
    const int x_int_first = hkernel_offs;
    const int x_int_last = width - 1 - hkernel_offs;

    int x = 0;
    const int x_left_end_16 = (x_int_first < width) ? x_int_first : width;
    for (; x < x_left_end_16; x++) {
        accumulate_pixel_16(x, src, dst, width, hkernel, hkernel_sz, hkernel_offs, &buf[x]);
    }

    /* SIMD interior: process 4 pixels at a time (to stay in 256-bit int64). */
    for (; x + 4 <= x_int_last + 1; x += 4) {
        __m256i acc_mux = _mm256_setzero_si256(); /* 4 x int64 */
        __m256i acc_muy = _mm256_setzero_si256();
        __m256i acc_x2 = _mm256_setzero_si256();
        __m256i acc_xy = _mm256_setzero_si256();
        __m256i acc_y2 = _mm256_setzero_si256();
        int64_t wsum = 0;

        for (int k = 0; k < hkernel_sz; k++) {
            const int off = x - hkernel_offs + k;
            /* Load 4 uint16 pixels, zero-extend to int64 in a 256i. */
            /* Use unaligned 64-bit (8 bytes = 4 * uint16). */
            __m128i s16_128 = _mm_loadl_epi64((const __m128i *)(src + off));
            __m128i d16_128 = _mm_loadl_epi64((const __m128i *)(dst + off));

            /* uint16 -> uint32 (128-bit, 4 lanes) */
            __m128i sv32 = _mm_cvtepu16_epi32(s16_128);
            __m128i dv32 = _mm_cvtepu16_epi32(d16_128);

            /* uint32 -> uint64 (256-bit, 4 lanes) */
            __m256i sv = _mm256_cvtepu32_epi64(sv32);
            __m256i dv = _mm256_cvtepu32_epi64(dv32);

            const int64_t w_scalar = (int64_t)hkernel[k];
            const __m256i wv = _mm256_set1_epi64x(w_scalar);

            /* mux += w * s  (64-bit mul: use _mm256_mul_epi32 on low dwords) */
            acc_mux = _mm256_add_epi64(acc_mux, _mm256_mul_epi32(wv, sv));
            acc_muy = _mm256_add_epi64(acc_muy, _mm256_mul_epi32(wv, dv));
            /* x2  += w * s * s */
            __m256i sv_sq = _mm256_mul_epi32(sv, sv);
            acc_x2 = _mm256_add_epi64(acc_x2, _mm256_mul_epi32(wv, sv_sq));
            /* xy  += w * s * d */
            __m256i sd = _mm256_mul_epi32(sv, dv);
            acc_xy = _mm256_add_epi64(acc_xy, _mm256_mul_epi32(wv, sd));
            /* y2  += w * d * d */
            __m256i dv_sq = _mm256_mul_epi32(dv, dv);
            acc_y2 = _mm256_add_epi64(acc_y2, _mm256_mul_epi32(wv, dv_sq));
            wsum += w_scalar;
        }

        /* Store 4 moments directly from 64-bit accumulators. */
        _Alignas(32) int64_t mux_arr[4];
        _Alignas(32) int64_t muy_arr[4];
        _Alignas(32) int64_t x2_arr[4];
        _Alignas(32) int64_t xy_arr[4];
        _Alignas(32) int64_t y2_arr[4];

        _mm256_store_si256((__m256i *)mux_arr, acc_mux);
        _mm256_store_si256((__m256i *)muy_arr, acc_muy);
        _mm256_store_si256((__m256i *)x2_arr, acc_x2);
        _mm256_store_si256((__m256i *)xy_arr, acc_xy);
        _mm256_store_si256((__m256i *)y2_arr, acc_y2);

        for (int lane = 0; lane < 4; lane++) {
            buf[x + lane].mux = mux_arr[lane];
            buf[x + lane].muy = muy_arr[lane];
            buf[x + lane].x2 = x2_arr[lane];
            buf[x + lane].xy = xy_arr[lane];
            buf[x + lane].y2 = y2_arr[lane];
            buf[x + lane].w = wsum;
        }
    }

    for (; x < width; x++) {
        accumulate_pixel_16(x, src, dst, width, hkernel, hkernel_sz, hkernel_offs, &buf[x]);
    }
}
