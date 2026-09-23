/*
 * Copyright 2026 Lusoris
 * SPDX-License-Identifier: EUPL-1.2
 *
 * Research-2046: compare the configured integer AVX512 statistic with the
 * independent scalar implementation, including its final vertical planes.
 * Heights sweep from 7 up through the 17-row extractor minimum and a few just
 * above it. The 32K-entry logarithm table is generated once and each geometry
 * reuses its fixtures across the depth/scale/pattern sweep, so gcov
 * instrumentation does not turn setup into the dominant cost.
 */

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe. ADR-1138. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "feature/integer_vif.h"
#include "feature/x86/vif_avx512.h"
#include "feature/x86/vif_avx2.h"
#include "test.h"
#include "simd_bitexact_test.h"

typedef struct VifStageFixture {
    VifPublicState state;
    unsigned char *storage;
    size_t bytes;
    unsigned width;
    unsigned height;
} VifStageFixture;

static void free_fixture(VifStageFixture *fixture)
{
    if (fixture != NULL) {
        free(fixture->storage);
        free(fixture);
    }
}

static VifStageFixture *alloc_fixture(unsigned width, unsigned height, const uint16_t *log2_table)
{
    VifStageFixture *fixture = calloc(1, sizeof(*fixture));
    if (fixture == NULL)
        return NULL;
    const size_t stride = (width * sizeof(uint16_t) + 31u) & ~(size_t)31u;
    const size_t plane = stride * (height + 16u);
    const size_t temporary = (width + 128u) * sizeof(uint32_t);
    fixture->bytes = plane * 4u + temporary * 7u;
    fixture->storage = calloc(1, fixture->bytes);
    if (fixture->storage == NULL) {
        free_fixture(fixture);
        return NULL;
    }
    fixture->width = width;
    fixture->height = height;
    VifBuffer *buf = &fixture->state.buf;
    buf->stride = (ptrdiff_t)stride;
    buf->stride_16 = (ptrdiff_t)stride;
    buf->stride_tmp = (ptrdiff_t)temporary;
    buf->ref = fixture->storage + stride * 8u;
    buf->dis = fixture->storage + plane + stride * 8u;
    buf->mu1 = (uint16_t *)(fixture->storage + plane * 2u);
    buf->mu2 = (uint16_t *)(fixture->storage + plane * 3u);
    uint32_t *tmp = (uint32_t *)(fixture->storage + plane * 4u) + 64;
    const size_t span = width + 128u;
    buf->tmp.mu1 = tmp;
    buf->tmp.mu2 = tmp + span;
    buf->tmp.ref = tmp + span * 2u;
    buf->tmp.dis = tmp + span * 3u;
    buf->tmp.ref_dis = tmp + span * 4u;
    buf->tmp.ref_convol = tmp + span * 5u;
    buf->tmp.dis_convol = tmp + span * 6u;
    memcpy(fixture->state.log2_table, log2_table, sizeof(fixture->state.log2_table));
    fixture->state.vif_enhn_gain_limit = DEFAULT_VIF_ENHN_GAIN_LIMIT;
    return fixture;
}

static void fill_fixture(VifStageFixture *fixture, unsigned bpc, unsigned scale, unsigned pattern)
{
    VifBuffer *buf = &fixture->state.buf;
    const unsigned maximum = (1u << bpc) - 1u;
    for (int row = -8; row < (int)fixture->height + 8; ++row) {
        uint8_t *ref = (uint8_t *)buf->ref + row * buf->stride;
        uint8_t *dis = (uint8_t *)buf->dis + row * buf->stride;
        for (unsigned col = 0; col < fixture->width; ++col) {
            unsigned value =
                (((unsigned)(row + 8) * 53u + col * 37u + col * col * 7u) * 257u) & maximum;
            if (pattern == 1u)
                value = ((col + (unsigned)(row + 8)) & 1u) ? maximum : 0u;
            const unsigned altered = value ^ (maximum / 7u);
            if (bpc == 8u && scale == 0u) {
                ref[col] = (uint8_t)value;
                dis[col] = (uint8_t)altered;
            } else {
                ((uint16_t *)ref)[col] = (uint16_t)value;
                ((uint16_t *)dis)[col] = (uint16_t)altered;
            }
        }
    }
}

/* Only the valid row plus reflected filter padding is semantic output; AVX-512
 * computes additional scratch lanes within the production allocation (Research-2046). */
static int compare_planes(const VifStageFixture *scalar, const VifStageFixture *simd,
                          unsigned scale)
{
    const VifBuffer *a = &scalar->state.buf;
    const VifBuffer *b = &simd->state.buf;
    const unsigned half = (unsigned)vif_filter1d_width[scale] / 2u;
    const size_t bytes = (scalar->width + 2u * half) * sizeof(uint32_t);
    const uint32_t *const left[] = {a->tmp.mu1, a->tmp.mu2, a->tmp.ref, a->tmp.dis, a->tmp.ref_dis};
    const uint32_t *const right[] = {b->tmp.mu1, b->tmp.mu2, b->tmp.ref, b->tmp.dis,
                                     b->tmp.ref_dis};
    for (unsigned plane = 0; plane < 5; ++plane) {
        if (memcmp(left[plane] - half, right[plane] - half, bytes) != 0)
            return 1;
    }
    const size_t frames = (size_t)a->stride * (scalar->height + 16u) * 4u;
    return memcmp(scalar->storage, simd->storage, frames) != 0;
}

static int compare_statistic(VifStageFixture *scalar, VifStageFixture *simd, VifStageFixture *avx2,
                             unsigned bpc, unsigned scale, unsigned pattern)
{
    const unsigned width = scalar->width;
    const unsigned height = scalar->height;
    memset(scalar->storage, 0, scalar->bytes);
    memset(simd->storage, 0, simd->bytes);
    fill_fixture(scalar, bpc, scale, pattern);
    fill_fixture(simd, bpc, scale, pattern);
    float scalar_result[2] = {0};
    float simd_result[2] = {0};
    if (bpc == 8u && scale == 0u) {
        vif_statistic_8(&scalar->state, scalar_result, scalar_result + 1, width, height);
        vif_statistic_8_avx512(&simd->state, simd_result, simd_result + 1, width, height);
    } else {
        vif_statistic_16(&scalar->state, scalar_result, scalar_result + 1, width, height, (int)bpc,
                         (int)scale);
        vif_statistic_16_avx512(&simd->state, simd_result, simd_result + 1, width, height, (int)bpc,
                                (int)scale);
    }
    uint32_t scalar_bits[2];
    uint32_t simd_bits[2];
    static_assert(sizeof(scalar_bits) == sizeof(scalar_result));
    memcpy(scalar_bits, scalar_result, sizeof(scalar_bits));
    memcpy(simd_bits, simd_result, sizeof(simd_bits));
    int different = !isfinite(simd_result[0]) || !isfinite(simd_result[1]) ||
                    scalar_bits[0] != simd_bits[0] || scalar_bits[1] != simd_bits[1] ||
                    compare_planes(scalar, simd, scale) != 0;

    if (avx2 != NULL) {
        memset(avx2->storage, 0, avx2->bytes);
        fill_fixture(avx2, bpc, scale, pattern);
        float avx2_result[2] = {0};
        if (bpc == 8u && scale == 0u) {
            vif_statistic_8_avx2(&avx2->state, avx2_result, avx2_result + 1, width, height);
        } else {
            vif_statistic_16_avx2(&avx2->state, avx2_result, avx2_result + 1, width, height,
                                  (int)bpc, (int)scale);
        }
        uint32_t avx2_bits[2];
        memcpy(avx2_bits, avx2_result, sizeof(avx2_bits));
        if (!isfinite(avx2_result[0]) || !isfinite(avx2_result[1]) ||
            scalar_bits[0] != avx2_bits[0] || scalar_bits[1] != avx2_bits[1]) {
            different = 1;
        }
    }
    return different;
}

static int compare_depth(VifStageFixture *scalar, VifStageFixture *simd, VifStageFixture *avx2,
                         unsigned bpc)
{
    for (unsigned scale = 0; scale < 4; ++scale) {
        for (unsigned pattern = 0; pattern < 2; ++pattern) {
            const int result = compare_statistic(scalar, simd, avx2, bpc, scale, pattern);
            if (result != 0)
                return result;
        }
    }
    return 0;
}

static int compare_geometry(unsigned width, unsigned height, const uint16_t *log2_table)
{
    static const unsigned depths[] = {8, 9, 10, 11, 12, 13, 14, 15, 16};
    VifStageFixture *scalar = alloc_fixture(width, height, log2_table);
    VifStageFixture *simd = alloc_fixture(width, height, log2_table);
    VifStageFixture *avx2 = simd_test_have_avx2() ? alloc_fixture(width, height, log2_table) : NULL;
    if (scalar == NULL || simd == NULL) {
        free_fixture(scalar);
        free_fixture(simd);
        free_fixture(avx2);
        return -1;
    }
    int result = 0;
    for (size_t b = 0; b < sizeof(depths) / sizeof(depths[0]); ++b) {
        result = compare_depth(scalar, simd, avx2, depths[b]);
        if (result != 0) {
            (void)fprintf(stderr, "  %ux%u bpc %u differs from scalar/avx2\n", width, height,
                          depths[b]);
            break;
        }
    }
    free_fixture(scalar);
    free_fixture(simd);
    free_fixture(avx2);
    return result;
}

static char *test_integer_vif_avx512_stages_red_check(void)
{
    if (!simd_test_have_avx512())
        return NULL;
    static uint16_t log2_table[VIF_LOG2_TABLE_SIZE];
    for (unsigned i = 0; i < VIF_LOG2_TABLE_SIZE; ++i) {
        log2_table[i] = (uint16_t)roundf(log2f((float)(VIF_LOG2_TABLE_OFFSET + i)) * 2048);
    }
    VifStageFixture *scalar = alloc_fixture(32, 17, log2_table);
    VifStageFixture *simd = alloc_fixture(32, 17, log2_table);
    VifStageFixture *avx2 = simd_test_have_avx2() ? alloc_fixture(32, 17, log2_table) : NULL;
    if (scalar == NULL || simd == NULL) {
        free_fixture(scalar);
        free_fixture(simd);
        free_fixture(avx2);
        return "allocation failure in red check";
    }

    /* 1. Baseline must pass (bit-exact scalar == AVX512 [== AVX2]). */
    int baseline = compare_statistic(scalar, simd, avx2, 8, 0, 0);
    mu_assert("baseline must be bit-exact across scalar, AVX2, and AVX-512", baseline == 0);

    /* 2. Red test: 1-bit perturbation in reference pixel input must turn RED (detected). */
    ((uint8_t *)scalar->state.buf.ref)[0] ^= 1;
    float res_scalar[2] = {0};
    float res_simd[2] = {0};
    vif_statistic_8(&scalar->state, res_scalar, res_scalar + 1, 32, 17);
    vif_statistic_8_avx512(&simd->state, res_simd, res_simd + 1, 32, 17);
    uint32_t bits_sc[2];
    uint32_t bits_simd[2];
    memcpy(bits_sc, res_scalar, sizeof(bits_sc));
    memcpy(bits_simd, res_simd, sizeof(bits_simd));
    const int input_perturbed_diff = (bits_sc[0] != bits_simd[0] || bits_sc[1] != bits_simd[1]);
    mu_assert("red check: harness must detect 1-bit input perturbation", input_perturbed_diff != 0);

    /* 3. Red test: 1-bit perturbation in intermediate plane must turn RED. */
    fill_fixture(scalar, 8, 0, 0);
    fill_fixture(simd, 8, 0, 0);
    vif_statistic_8(&scalar->state, res_scalar, res_scalar + 1, 32, 17);
    vif_statistic_8_avx512(&simd->state, res_simd, res_simd + 1, 32, 17);
    simd->state.buf.tmp.ref[0] ^= 1;
    const int plane_perturbed_diff = compare_planes(scalar, simd, 0);
    mu_assert("red check: harness must detect 1-bit intermediate plane difference",
              plane_perturbed_diff != 0);

    free_fixture(scalar);
    free_fixture(simd);
    free_fixture(avx2);
    return NULL;
}

static char *test_integer_vif_avx512_stages(void)
{
    if (!simd_test_have_avx512())
        return NULL;
    static uint16_t log2_table[VIF_LOG2_TABLE_SIZE];
    for (unsigned i = 0; i < VIF_LOG2_TABLE_SIZE; ++i) {
        log2_table[i] = (uint16_t)roundf(log2f((float)(VIF_LOG2_TABLE_OFFSET + i)) * 2048);
    }
    const unsigned widths[] = {9, 15, 16, 17, 31, 32, 33, 63, 64, 65, 127, 257};
    const unsigned heights[] = {7, 17, 18, 24};
    for (size_t w = 0; w < sizeof(widths) / sizeof(widths[0]); ++w) {
        for (size_t h = 0; h < sizeof(heights) / sizeof(heights[0]); ++h) {
            mu_assert("integer VIF AVX512 statistic/vertical buffers differ from scalar",
                      compare_geometry(widths[w], heights[h], log2_table) == 0);
        }
    }
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_integer_vif_avx512_stages_red_check);
    mu_run_test(test_integer_vif_avx512_stages);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
