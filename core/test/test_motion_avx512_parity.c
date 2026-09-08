/**
 *
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
 * Direct AVX-512 parity tests for the motion feature (ADR-0854).
 *
 * Coverage gap closed:
 *   The prior audit (`test_motion_v2_simd.c`) exercised only the
 *   `motion_score_pipeline_16_avx2` path via the AVX2 gate.  AVX-512
 *   paths were only reachable indirectly through the Netflix golden-data
 *   tests, which do not assert bit-exactness at the kernel level and run
 *   at full-frame granularity.  This file adds direct unit coverage for:
 *
 *     - motion_score_pipeline_8_avx512   (motion_v2_avx512.c)
 *     - motion_score_pipeline_16_avx512  (motion_v2_avx512.c)
 *     - sad_avx512                       (motion_avx512.c)
 *     - y_convolution_8_avx512           (motion_avx512.c)
 *     - y_convolution_16_avx512          (motion_avx512.c)
 *     - x_convolution_16_avx512          (motion_avx512.c)
 *
 * Scalar references used for comparison:
 *   - motion_score_pipeline_{8,16}: local duplicates of the integer_motion_v2
 *     scalar path (same `static` scope workaround as test_motion_v2_simd.c).
 *   - y_convolution_8, y_convolution_16, x_convolution_16, sad_c: the static
 *     scalar functions defined in integer_motion.c cannot be called from
 *     outside TU scope, so local scalar reimplementations are provided.
 *     They are line-for-line mirrors to ensure the comparison is fair.
 *
 * Bit-exactness contract (ADR-0138 / ADR-0139):
 *   All six kernels are expected to produce output that is bit-identical
 *   to their scalar references on the same input.  Tests use
 *   `SIMD_BITEXACT_ASSERT_MEMCMP` (byte-level comparison) for array
 *   outputs and exact `uint64_t` equality for scalar SAD outputs.
 *   Any divergence is a correctness regression, not a tolerance issue.
 *
 * ISA gate:
 *   All tests call `simd_test_have_avx512()` (added to
 *   simd_bitexact_test.h by this PR) and return NULL (SKIP) when the
 *   host CPU does not expose VMAF_X86_CPU_FLAG_AVX512.
 */

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "test.h"
/* clang-format off */
#include "simd_bitexact_test.h"
/* clang-format on */

#if ARCH_X86
#include "feature/integer_motion.h"
#include "picture.h"
#include "feature/x86/motion_avx512.h"
#include "libvmaf/picture.h"
#include "common/alignment.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe, and this file mirrors
 * the C spelling of the surface it exercises. ADR-1138. */
#endif

/* -----------------------------------------------------------------------
 * Fixture dimensions.  Chosen to:
 *   - exercise the SIMD body (w >= 64 for y_conv_8, w >= 32 for y_conv_16,
 *     w >= 32 for x_conv_16, w >= 32 for pipeline_8, w >= 16 for
 *     pipeline_16) AND
 *   - include a non-SIMD-multiple tail (w=80 has tail=16 for 64-wide y_conv_8
 *     and tail=16 for 32-wide y_conv_16/x_conv_16/pipeline_16).
 *   - sufficient height for the vertical-edge mirror regions (h >= 5).
 * ----------------------------------------------------------------------- */
#define TEST_W 80u
#define TEST_H 12u
#define ALIGN_BYTES 64u

/* -----------------------------------------------------------------------
 * Scalar reference: motion_score_pipeline_8.
 *
 * Mirrors the integer_motion_v2.c 8-bit pipeline.  The function is
 * duplicated here because the upstream symbol has `static` linkage.
 * ----------------------------------------------------------------------- */
#if ARCH_X86

static inline int mirror_idx_ref(int idx, int size)
{
    if (idx < 0)
        return -idx;
    if (idx >= size)
        return 2 * size - idx - 2;
    return idx;
}

static uint64_t pipeline_8_scalar(const uint8_t *prev, ptrdiff_t prev_stride, const uint8_t *cur,
                                  ptrdiff_t cur_stride, int32_t *y_row, unsigned w, unsigned h,
                                  unsigned bpc)
{
    (void)bpc;
    const int radius = filter_width / 2;
    const int32_t x_round = 1 << 15;
    uint64_t sad = 0;

    for (unsigned i = 0; i < h; i++) {
        int32_t any_nonzero = 0;
        for (unsigned j = 0; j < w; j++) {
            int32_t accum = 0;
            for (int k = 0; k < filter_width; k++) {
                const int row = mirror_idx_ref((int)i - radius + k, (int)h);
                const int32_t diff =
                    (int32_t)prev[row * prev_stride + j] - (int32_t)cur[row * cur_stride + j];
                accum += (int32_t)filter[k] * diff;
            }
            y_row[j] = (accum + (1 << 7)) >> 8;
            any_nonzero |= y_row[j];
        }

        if (!any_nonzero)
            continue;

        uint32_t row_sad = 0;
        for (unsigned j = 0; j < w; j++) {
            int64_t accum = 0;
            for (int k = 0; k < filter_width; k++) {
                const int col = mirror_idx_ref((int)j - radius + k, (int)w);
                accum += (int64_t)filter[k] * y_row[col];
            }
            int32_t val = (int32_t)((accum + x_round) >> 16);
            row_sad += (uint32_t)abs(val);
        }
        sad += row_sad;
    }
    return sad;
}

/* -----------------------------------------------------------------------
 * Scalar reference: motion_score_pipeline_16.
 * (Also duplicated in test_motion_v2_simd.c; reproduced here for
 * locality so this TU compiles standalone.)
 * ----------------------------------------------------------------------- */
static uint64_t pipeline_16_scalar(const uint8_t *prev_u8, ptrdiff_t prev_stride,
                                   const uint8_t *cur_u8, ptrdiff_t cur_stride, int32_t *y_row,
                                   unsigned w, unsigned h, unsigned bpc)
{
    const uint16_t *prev = (const uint16_t *)prev_u8;
    const uint16_t *cur = (const uint16_t *)cur_u8;
    const ptrdiff_t p_stride = prev_stride / 2;
    const ptrdiff_t c_stride = cur_stride / 2;

    const int radius = filter_width / 2;
    const int64_t y_round = (int64_t)1 << (bpc - 1);
    const int32_t x_round = 1 << 15;
    uint64_t sad = 0;

    for (unsigned i = 0; i < h; i++) {
        int32_t any_nonzero = 0;
        for (unsigned j = 0; j < w; j++) {
            int64_t accum = 0;
            for (int k = 0; k < filter_width; k++) {
                const int row = mirror_idx_ref((int)i - radius + k, (int)h);
                const int32_t diff =
                    (int32_t)prev[row * p_stride + j] - (int32_t)cur[row * c_stride + j];
                accum += (int64_t)filter[k] * diff;
            }
            y_row[j] = (int32_t)((accum + y_round) >> bpc);
            any_nonzero |= y_row[j];
        }

        if (!any_nonzero)
            continue;

        uint32_t row_sad = 0;
        for (unsigned j = 0; j < w; j++) {
            int64_t accum = 0;
            for (int k = 0; k < filter_width; k++) {
                const int col = mirror_idx_ref((int)j - radius + k, (int)w);
                accum += (int64_t)filter[k] * y_row[col];
            }
            int32_t val = (int32_t)((accum + x_round) >> 16);
            row_sad += (uint32_t)abs(val);
        }
        sad += row_sad;
    }
    return sad;
}

/* -----------------------------------------------------------------------
 * Scalar reference: y_convolution_8.
 * Mirrors integer_motion.c::y_convolution_8 (static linkage; copy here).
 * ----------------------------------------------------------------------- */
static inline uint32_t scalar_edge_8(const uint8_t *src, int height, int stride, int i, int j)
{
    const int radius = filter_width / 2;
    uint32_t accum = 0;
    for (int k = 0; k < filter_width; ++k) {
        int i_tap = i - radius + k;
        if (i_tap < 0)
            i_tap = -i_tap;
        else if (i_tap >= height)
            i_tap = height - (i_tap - height + 2);
        accum += filter[k] * src[i_tap * stride + j];
    }
    return accum;
}

static void y_conv_8_scalar(void *src, uint16_t *dst, unsigned width, unsigned height,
                            ptrdiff_t src_stride, ptrdiff_t dst_stride, unsigned inp_size_bits)
{
    (void)inp_size_bits;
    const unsigned radius = filter_width / 2;
    const unsigned top_edge = vmaf_ceiln(radius, 1);
    const unsigned bottom_edge = vmaf_floorn(height - (filter_width - radius), 1);
    const unsigned shift_var = 8;
    const unsigned add_before_shift = (unsigned)(1 << (shift_var - 1));

    for (unsigned i = 0; i < top_edge; i++) {
        for (unsigned j = 0; j < width; ++j) {
            dst[i * dst_stride + j] =
                (uint16_t)((scalar_edge_8(src, (int)height, (int)src_stride, (int)i, (int)j) +
                            add_before_shift) >>
                           shift_var);
        }
    }

    uint8_t *src_p = (uint8_t *)src + (top_edge - radius) * src_stride;
    for (unsigned i = top_edge; i < bottom_edge; i++) {
        uint8_t *src_p1 = src_p;
        for (unsigned j = 0; j < width; ++j) {
            uint8_t *src_p2 = src_p1;
            uint32_t accum = 0;
            for (int k = 0; k < filter_width; ++k) {
                accum += filter[k] * (*src_p2);
                src_p2 += src_stride;
            }
            dst[i * dst_stride + j] = (uint16_t)((accum + add_before_shift) >> shift_var);
            src_p1++;
        }
        src_p += src_stride;
    }

    for (unsigned i = bottom_edge; i < height; i++) {
        for (unsigned j = 0; j < width; ++j) {
            dst[i * dst_stride + j] =
                (uint16_t)((scalar_edge_8(src, (int)height, (int)src_stride, (int)i, (int)j) +
                            add_before_shift) >>
                           shift_var);
        }
    }
}

/* -----------------------------------------------------------------------
 * Scalar reference: y_convolution_16.
 * Mirrors integer_motion.c::y_convolution_16.
 * ----------------------------------------------------------------------- */
static void y_conv_16_scalar(void *src, uint16_t *dst, unsigned width, unsigned height,
                             ptrdiff_t src_stride, ptrdiff_t dst_stride, unsigned inp_size_bits)
{
    const unsigned radius = filter_width / 2;
    const unsigned top_edge = vmaf_ceiln(radius, 1);
    const unsigned bottom_edge = vmaf_floorn(height - (filter_width - radius), 1);
    const unsigned add_before_shift = (unsigned)(1 << (inp_size_bits - 1));
    const unsigned shift_var = inp_size_bits;

    uint16_t *src_u16 = (uint16_t *)src;
    for (unsigned i = 0; i < top_edge; i++) {
        for (unsigned j = 0; j < width; ++j) {
            dst[i * dst_stride + j] = (uint16_t)((edge_16(false, src_u16, (int)width, (int)height,
                                                          (int)src_stride, (int)i, (int)j) +
                                                  add_before_shift) >>
                                                 shift_var);
        }
    }

    uint16_t *src_p = src_u16 + (top_edge - radius) * src_stride;
    for (unsigned i = top_edge; i < bottom_edge; i++) {
        uint16_t *src_p1 = src_p;
        for (unsigned j = 0; j < width; ++j) {
            uint16_t *src_p2 = src_p1;
            uint32_t accum = 0;
            for (int k = 0; k < filter_width; ++k) {
                accum += filter[k] * (*src_p2);
                src_p2 += src_stride;
            }
            dst[i * dst_stride + j] = (uint16_t)((accum + add_before_shift) >> shift_var);
            src_p1++;
        }
        src_p += src_stride;
    }

    for (unsigned i = bottom_edge; i < height; i++) {
        for (unsigned j = 0; j < width; ++j) {
            dst[i * dst_stride + j] = (uint16_t)((edge_16(false, src_u16, (int)width, (int)height,
                                                          (int)src_stride, (int)i, (int)j) +
                                                  add_before_shift) >>
                                                 shift_var);
        }
    }
}

/* -----------------------------------------------------------------------
 * Scalar reference: x_convolution_16.
 * Mirrors integer_motion.c::x_convolution_16.
 * ----------------------------------------------------------------------- */
static void x_conv_16_scalar(const uint16_t *src, uint16_t *dst, unsigned width, unsigned height,
                             ptrdiff_t src_stride, ptrdiff_t dst_stride)
{
    const unsigned radius = filter_width / 2;
    const unsigned left_edge = vmaf_ceiln(radius, 1);
    const unsigned right_edge = vmaf_floorn(width - (filter_width - radius), 1);
    const unsigned shift_add_round = 32768;

    uint16_t *src_p = (uint16_t *)src + (left_edge - radius);
    for (unsigned i = 0; i < height; ++i) {
        for (unsigned j = 0; j < left_edge; j++) {
            dst[i * dst_stride + j] = (uint16_t)((edge_16(true, src, (int)width, (int)height,
                                                          (int)src_stride, (int)i, (int)j) +
                                                  shift_add_round) >>
                                                 16);
        }

        uint16_t *src_p1 = src_p;
        for (unsigned j = left_edge; j < right_edge; j++) {
            uint32_t accum = 0;
            uint16_t *src_p2 = src_p1;
            for (int k = 0; k < filter_width; ++k) {
                accum += filter[k] * (*src_p2);
                src_p2++;
            }
            src_p1++;
            dst[i * dst_stride + j] = (uint16_t)((accum + shift_add_round) >> 16);
        }

        for (unsigned j = right_edge; j < width; j++) {
            dst[i * dst_stride + j] = (uint16_t)((edge_16(true, src, (int)width, (int)height,
                                                          (int)src_stride, (int)i, (int)j) +
                                                  shift_add_round) >>
                                                 16);
        }

        src_p += src_stride;
    }
}

/* -----------------------------------------------------------------------
 * Scalar reference: sad_c.
 * Mirrors integer_motion.c::sad_c (static linkage; copy here).
 * ----------------------------------------------------------------------- */
static void sad_scalar(VmafPicture *pic_a, VmafPicture *pic_b, uint64_t *sad)
{
    *sad = 0;
    uint16_t *a = (uint16_t *)pic_a->data[0];
    uint16_t *b = (uint16_t *)pic_b->data[0];
    for (unsigned i = 0; i < pic_a->h[0]; i++) {
        uint32_t inner_sad = 0;
        for (unsigned j = 0; j < pic_a->w[0]; j++) {
            inner_sad += (uint32_t)abs((int)a[j] - (int)b[j]);
        }
        *sad += inner_sad;
        a += (pic_a->stride[0] / 2);
        b += (pic_b->stride[0] / 2);
    }
}

/* -----------------------------------------------------------------------
 * pipeline_8 tests: random, adversarial-positive, adversarial-negative
 * ----------------------------------------------------------------------- */
static char *check_pipeline_8(unsigned seed_val, const char *label)
{
    const ptrdiff_t stride8 = (ptrdiff_t)TEST_W;
    uint8_t *prev = (uint8_t *)simd_test_aligned_malloc(TEST_W * TEST_H, ALIGN_BYTES);
    uint8_t *cur = (uint8_t *)simd_test_aligned_malloc(TEST_W * TEST_H, ALIGN_BYTES);
    int32_t *y_scalar = (int32_t *)simd_test_aligned_malloc(sizeof(int32_t) * TEST_W, ALIGN_BYTES);
    int32_t *y_avx512 = (int32_t *)simd_test_aligned_malloc(sizeof(int32_t) * TEST_W, ALIGN_BYTES);

    if (!prev || !cur || !y_scalar || !y_avx512) {
        simd_test_aligned_free(prev);
        simd_test_aligned_free(cur);
        simd_test_aligned_free(y_scalar);
        simd_test_aligned_free(y_avx512);
        return "allocation failure (pipeline_8)";
    }

    uint32_t state = seed_val;
    for (unsigned i = 0; i < TEST_W * TEST_H; i++) {
        prev[i] = (uint8_t)(simd_test_xorshift32(&state) & 0xFF);
        cur[i] = (uint8_t)(simd_test_xorshift32(&state) & 0xFF);
    }

    const uint64_t sad_s = pipeline_8_scalar(prev, stride8, cur, stride8, y_scalar, TEST_W, TEST_H,
                                             8 /* bpc unused */);
    const uint64_t sad_v = motion_score_pipeline_8_avx512(prev, stride8, cur, stride8, y_avx512,
                                                          TEST_W, TEST_H, 8 /* bpc unused */);

    simd_test_aligned_free(prev);
    simd_test_aligned_free(cur);
    simd_test_aligned_free(y_scalar);
    simd_test_aligned_free(y_avx512);

    if (sad_s != sad_v) {
        (void)fprintf(stderr,
                      "pipeline_8 AVX-512 parity FAIL (%s, seed=0x%08x):"
                      " scalar=%llu avx512=%llu\n",
                      label, seed_val, (unsigned long long)sad_s, (unsigned long long)sad_v);
        return "motion_score_pipeline_8_avx512 diverges from scalar";
    }
    return NULL;
}

static char *test_pipeline_8_random(void)
{
    return check_pipeline_8(0xdeadbeef, "random");
}
static char *test_pipeline_8_bright_dis(void)
{
    return check_pipeline_8(0x12345678, "bright-dis");
}

/* -----------------------------------------------------------------------
 * pipeline_16 tests: bpc=10, bpc=12
 * ----------------------------------------------------------------------- */
static char *check_pipeline_16(unsigned bpc, unsigned seed_val, const char *label)
{
    const ptrdiff_t stride16 = (ptrdiff_t)TEST_W;
    uint16_t *prev =
        (uint16_t *)simd_test_aligned_malloc(sizeof(uint16_t) * TEST_W * TEST_H, ALIGN_BYTES);
    uint16_t *cur =
        (uint16_t *)simd_test_aligned_malloc(sizeof(uint16_t) * TEST_W * TEST_H, ALIGN_BYTES);
    int32_t *y_scalar = (int32_t *)simd_test_aligned_malloc(sizeof(int32_t) * TEST_W, ALIGN_BYTES);
    int32_t *y_avx512 = (int32_t *)simd_test_aligned_malloc(sizeof(int32_t) * TEST_W, ALIGN_BYTES);

    if (!prev || !cur || !y_scalar || !y_avx512) {
        simd_test_aligned_free(prev);
        simd_test_aligned_free(cur);
        simd_test_aligned_free(y_scalar);
        simd_test_aligned_free(y_avx512);
        return "allocation failure (pipeline_16)";
    }

    simd_test_fill_random_u16(prev, TEST_W * TEST_H, (uint16_t)((1u << bpc) - 1u), seed_val);
    simd_test_fill_random_u16(cur, TEST_W * TEST_H, (uint16_t)((1u << bpc) - 1u),
                              seed_val ^ 0xFFFFFFFFu);

    const ptrdiff_t byte_stride = stride16 * (ptrdiff_t)sizeof(uint16_t);

    const uint64_t sad_s =
        pipeline_16_scalar((const uint8_t *)prev, byte_stride, (const uint8_t *)cur, byte_stride,
                           y_scalar, TEST_W, TEST_H, bpc);
    const uint64_t sad_v =
        motion_score_pipeline_16_avx512((const uint8_t *)prev, byte_stride, (const uint8_t *)cur,
                                        byte_stride, y_avx512, TEST_W, TEST_H, bpc);

    simd_test_aligned_free(prev);
    simd_test_aligned_free(cur);
    simd_test_aligned_free(y_scalar);
    simd_test_aligned_free(y_avx512);

    if (sad_s != sad_v) {
        (void)fprintf(stderr,
                      "pipeline_16 AVX-512 parity FAIL (%s, bpc=%u, seed=0x%08x):"
                      " scalar=%llu avx512=%llu\n",
                      label, bpc, seed_val, (unsigned long long)sad_s, (unsigned long long)sad_v);
        return "motion_score_pipeline_16_avx512 diverges from scalar";
    }
    return NULL;
}

static char *test_pipeline_16_bpc10(void)
{
    return check_pipeline_16(10, 0xa5a5a5a5u, "bpc10");
}
static char *test_pipeline_16_bpc12(void)
{
    return check_pipeline_16(12, 0x0f0f0f0fu, "bpc12");
}
/* Adversarial: dis brighter than ref -> negative diffs -> tests srav_epi64 path */
static char *test_pipeline_16_neg_diff_bpc10(void)
{
    const unsigned bpc = 10;
    const ptrdiff_t stride16 = (ptrdiff_t)TEST_W;
    uint16_t *prev =
        (uint16_t *)simd_test_aligned_malloc(sizeof(uint16_t) * TEST_W * TEST_H, ALIGN_BYTES);
    uint16_t *cur =
        (uint16_t *)simd_test_aligned_malloc(sizeof(uint16_t) * TEST_W * TEST_H, ALIGN_BYTES);
    int32_t *y_scalar = (int32_t *)simd_test_aligned_malloc(sizeof(int32_t) * TEST_W, ALIGN_BYTES);
    int32_t *y_avx512 = (int32_t *)simd_test_aligned_malloc(sizeof(int32_t) * TEST_W, ALIGN_BYTES);

    if (!prev || !cur || !y_scalar || !y_avx512) {
        simd_test_aligned_free(prev);
        simd_test_aligned_free(cur);
        simd_test_aligned_free(y_scalar);
        simd_test_aligned_free(y_avx512);
        return "allocation failure (pipeline_16_neg)";
    }

    /* prev low, cur high — forces negative accum (same fixture as AVX2 test) */
    uint32_t rng = 0xa5a5a5a5u;
    for (unsigned i = 0; i < TEST_W * TEST_H; i++) {
        prev[i] = (uint16_t)(simd_test_xorshift32(&rng) & 0x3Fu);
        cur[i] = (uint16_t)(960u + (simd_test_xorshift32(&rng) & 0x3Fu));
    }

    const ptrdiff_t byte_stride = stride16 * (ptrdiff_t)sizeof(uint16_t);
    const uint64_t sad_s =
        pipeline_16_scalar((const uint8_t *)prev, byte_stride, (const uint8_t *)cur, byte_stride,
                           y_scalar, TEST_W, TEST_H, bpc);
    const uint64_t sad_v =
        motion_score_pipeline_16_avx512((const uint8_t *)prev, byte_stride, (const uint8_t *)cur,
                                        byte_stride, y_avx512, TEST_W, TEST_H, bpc);

    simd_test_aligned_free(prev);
    simd_test_aligned_free(cur);
    simd_test_aligned_free(y_scalar);
    simd_test_aligned_free(y_avx512);

    if (sad_s != sad_v) {
        (void)fprintf(stderr,
                      "pipeline_16 AVX-512 neg-diff FAIL (bpc=10): scalar=%llu avx512=%llu\n",
                      (unsigned long long)sad_s, (unsigned long long)sad_v);
        return "motion_score_pipeline_16_avx512 diverges from scalar on negative-diff fixture";
    }
    return NULL;
}

/* -----------------------------------------------------------------------
 * sad_avx512 test.
 *
 * Allocates two VmafPictures via vmaf_picture_alloc (10-bit, 420) and
 * fills with random data.  Compares sad_avx512 vs. sad_scalar.
 * ----------------------------------------------------------------------- */
static char *test_sad_avx512(void)
{
    VmafPicture pic_a, pic_b;
    memset(&pic_a, 0, sizeof(pic_a));
    memset(&pic_b, 0, sizeof(pic_b));

    /* vmaf_picture_alloc requires bpc in {8,10,12,16} */
    if (vmaf_picture_alloc(&pic_a, VMAF_PIX_FMT_YUV400P, 10, TEST_W, TEST_H) != 0)
        return "vmaf_picture_alloc failed for pic_a";
    if (vmaf_picture_alloc(&pic_b, VMAF_PIX_FMT_YUV400P, 10, TEST_W, TEST_H) != 0) {
        (void)vmaf_picture_unref(&pic_a);
        return "vmaf_picture_alloc failed for pic_b";
    }

    uint32_t state_a = 0xCAFEBABEu;
    uint32_t state_b = 0xDEADC0DEu;
    uint16_t *a = (uint16_t *)pic_a.data[0];
    uint16_t *b = (uint16_t *)pic_b.data[0];
    const ptrdiff_t a_stride_u16 = pic_a.stride[0] / 2;
    const ptrdiff_t b_stride_u16 = pic_b.stride[0] / 2;

    for (unsigned i = 0; i < TEST_H; i++) {
        for (unsigned j = 0; j < TEST_W; j++) {
            a[i * a_stride_u16 + j] = (uint16_t)(simd_test_xorshift32(&state_a) & 0x3FFu);
            b[i * b_stride_u16 + j] = (uint16_t)(simd_test_xorshift32(&state_b) & 0x3FFu);
        }
    }

    uint64_t sad_s = 0, sad_v = 0;
    sad_scalar(&pic_a, &pic_b, &sad_s);
    sad_avx512(&pic_a, &pic_b, &sad_v);

    (void)vmaf_picture_unref(&pic_a);
    (void)vmaf_picture_unref(&pic_b);

    if (sad_s != sad_v) {
        (void)fprintf(stderr, "sad_avx512 FAIL: scalar=%llu avx512=%llu\n",
                      (unsigned long long)sad_s, (unsigned long long)sad_v);
        return "sad_avx512 diverges from scalar";
    }
    return NULL;
}

/* -----------------------------------------------------------------------
 * y_convolution_8_avx512 test.
 * ----------------------------------------------------------------------- */
static char *test_y_conv_8_avx512(void)
{
    const ptrdiff_t src_stride = (ptrdiff_t)TEST_W;
    const ptrdiff_t dst_stride = (ptrdiff_t)TEST_W;

    uint8_t *src = (uint8_t *)simd_test_aligned_malloc(TEST_W * TEST_H, ALIGN_BYTES);
    uint16_t *dst_s =
        (uint16_t *)simd_test_aligned_malloc(sizeof(uint16_t) * TEST_W * TEST_H, ALIGN_BYTES);
    uint16_t *dst_v =
        (uint16_t *)simd_test_aligned_malloc(sizeof(uint16_t) * TEST_W * TEST_H, ALIGN_BYTES);

    if (!src || !dst_s || !dst_v) {
        simd_test_aligned_free(src);
        simd_test_aligned_free(dst_s);
        simd_test_aligned_free(dst_v);
        return "allocation failure (y_conv_8)";
    }

    uint32_t state = 0x11223344u;
    for (unsigned i = 0; i < TEST_W * TEST_H; i++) {
        src[i] = (uint8_t)(simd_test_xorshift32(&state) & 0xFFu);
    }

    y_conv_8_scalar(src, dst_s, TEST_W, TEST_H, src_stride, dst_stride, 8);
    y_convolution_8_avx512(src, dst_v, TEST_W, TEST_H, src_stride, dst_stride, 8);

    const size_t n_bytes = sizeof(uint16_t) * TEST_W * TEST_H;
    SIMD_BITEXACT_ASSERT_MEMCMP(dst_s, dst_v, n_bytes, "y_convolution_8_avx512");

    simd_test_aligned_free(src);
    simd_test_aligned_free(dst_s);
    simd_test_aligned_free(dst_v);
    return NULL;
}

/* -----------------------------------------------------------------------
 * y_convolution_16_avx512 test: bpc=10 and bpc=12.
 * ----------------------------------------------------------------------- */
static char *check_y_conv_16(unsigned bpc, uint32_t seed_val, const char *label)
{
    const ptrdiff_t src_stride = (ptrdiff_t)TEST_W;
    const ptrdiff_t dst_stride = (ptrdiff_t)TEST_W;

    uint16_t *src =
        (uint16_t *)simd_test_aligned_malloc(sizeof(uint16_t) * TEST_W * TEST_H, ALIGN_BYTES);
    uint16_t *dst_s =
        (uint16_t *)simd_test_aligned_malloc(sizeof(uint16_t) * TEST_W * TEST_H, ALIGN_BYTES);
    uint16_t *dst_v =
        (uint16_t *)simd_test_aligned_malloc(sizeof(uint16_t) * TEST_W * TEST_H, ALIGN_BYTES);

    if (!src || !dst_s || !dst_v) {
        simd_test_aligned_free(src);
        simd_test_aligned_free(dst_s);
        simd_test_aligned_free(dst_v);
        return "allocation failure (y_conv_16)";
    }

    simd_test_fill_random_u16(src, TEST_W * TEST_H, (uint16_t)((1u << bpc) - 1u), seed_val);
    memset(dst_s, 0, sizeof(uint16_t) * TEST_W * TEST_H);
    memset(dst_v, 0, sizeof(uint16_t) * TEST_W * TEST_H);

    y_conv_16_scalar(src, dst_s, TEST_W, TEST_H, src_stride, dst_stride, bpc);
    y_convolution_16_avx512(src, dst_v, TEST_W, TEST_H, src_stride, dst_stride, bpc);

    const size_t n_bytes = sizeof(uint16_t) * TEST_W * TEST_H;
    if (memcmp(dst_s, dst_v, n_bytes) != 0) {
        const uint16_t *ps = dst_s;
        const uint16_t *pv = dst_v;
        size_t idx = 0;
        while (idx < TEST_W * TEST_H && ps[idx] == pv[idx]) {
            idx++;
        }
        (void)fprintf(stderr,
                      "y_convolution_16_avx512 FAIL (%s, bpc=%u): "
                      "first diverging element at index %zu "
                      "(scalar=%u avx512=%u)\n",
                      label, bpc, idx, (unsigned)ps[idx], (unsigned)pv[idx]);
        simd_test_aligned_free(src);
        simd_test_aligned_free(dst_s);
        simd_test_aligned_free(dst_v);
        return "y_convolution_16_avx512 diverges from scalar";
    }

    simd_test_aligned_free(src);
    simd_test_aligned_free(dst_s);
    simd_test_aligned_free(dst_v);
    return NULL;
}

static char *test_y_conv_16_bpc10(void)
{
    return check_y_conv_16(10, 0x55aa55aau, "bpc10");
}
static char *test_y_conv_16_bpc12(void)
{
    return check_y_conv_16(12, 0xbeefcafeu, "bpc12");
}

/* -----------------------------------------------------------------------
 * x_convolution_16_avx512 test.
 * ----------------------------------------------------------------------- */
static char *test_x_conv_16_avx512(void)
{
    const ptrdiff_t src_stride = (ptrdiff_t)TEST_W;
    const ptrdiff_t dst_stride = (ptrdiff_t)TEST_W;

    uint16_t *src =
        (uint16_t *)simd_test_aligned_malloc(sizeof(uint16_t) * TEST_W * TEST_H, ALIGN_BYTES);
    uint16_t *dst_s =
        (uint16_t *)simd_test_aligned_malloc(sizeof(uint16_t) * TEST_W * TEST_H, ALIGN_BYTES);
    uint16_t *dst_v =
        (uint16_t *)simd_test_aligned_malloc(sizeof(uint16_t) * TEST_W * TEST_H, ALIGN_BYTES);

    if (!src || !dst_s || !dst_v) {
        simd_test_aligned_free(src);
        simd_test_aligned_free(dst_s);
        simd_test_aligned_free(dst_v);
        return "allocation failure (x_conv_16)";
    }

    /* Use 16-bit full range (uint16_max) to stress the mulhi/mullo path */
    simd_test_fill_random_u16(src, TEST_W * TEST_H, 0xFFFFu, 0xFACEFACEu);
    memset(dst_s, 0, sizeof(uint16_t) * TEST_W * TEST_H);
    memset(dst_v, 0, sizeof(uint16_t) * TEST_W * TEST_H);

    x_conv_16_scalar(src, dst_s, TEST_W, TEST_H, src_stride, dst_stride);
    x_convolution_16_avx512(src, dst_v, TEST_W, TEST_H, src_stride, dst_stride);

    const size_t n_bytes = sizeof(uint16_t) * TEST_W * TEST_H;
    if (memcmp(dst_s, dst_v, n_bytes) != 0) {
        const uint16_t *ps = dst_s;
        const uint16_t *pv = dst_v;
        size_t idx = 0;
        while (idx < TEST_W * TEST_H && ps[idx] == pv[idx]) {
            idx++;
        }
        (void)fprintf(stderr,
                      "x_convolution_16_avx512 FAIL: "
                      "first diverging element at index %zu "
                      "(scalar=%u avx512=%u)\n",
                      idx, (unsigned)ps[idx], (unsigned)pv[idx]);
        simd_test_aligned_free(src);
        simd_test_aligned_free(dst_s);
        simd_test_aligned_free(dst_v);
        return "x_convolution_16_avx512 diverges from scalar";
    }

    simd_test_aligned_free(src);
    simd_test_aligned_free(dst_s);
    simd_test_aligned_free(dst_v);
    return NULL;
}

#endif /* ARCH_X86 */

/* -----------------------------------------------------------------------
 * Test runner.
 * ----------------------------------------------------------------------- */
char *run_tests(void)
{
#if ARCH_X86
    if (!simd_test_have_avx512()) {
        return NULL; /* SKIP on hosts without AVX-512 */
    }
    mu_run_test(test_pipeline_8_random);
    mu_run_test(test_pipeline_8_bright_dis);
    mu_run_test(test_pipeline_16_bpc10);
    mu_run_test(test_pipeline_16_bpc12);
    mu_run_test(test_pipeline_16_neg_diff_bpc10);
    mu_run_test(test_sad_avx512);
    mu_run_test(test_y_conv_8_avx512);
    mu_run_test(test_y_conv_16_bpc10);
    mu_run_test(test_y_conv_16_bpc12);
    mu_run_test(test_x_conv_16_avx512);
#else
    (void)fprintf(stderr, "skipping: non-x86 arch\n");
#endif
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
