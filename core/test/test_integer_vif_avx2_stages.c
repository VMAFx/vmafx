/*
 * Copyright 2026 Lusoris
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 * Research-2045: compare the configured integer AVX2 statistic with the
 * independent scalar implementation, including its final vertical planes.
 */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "feature/integer_vif.h"
#include "feature/x86/vif_avx2.h"
#include "test.h"
#include "simd_bitexact_test.h"

/* Preserve Netflix-compatible NULL spelling without relying on undocumented
 * MSVC C nullptr support (ADR-1138); retain the C++ nullptr ratchet. */
// NOLINTBEGIN(modernize-use-nullptr)

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

static VifStageFixture *alloc_fixture(unsigned width, unsigned height)
{
    VifStageFixture *fixture = calloc(1, sizeof(*fixture));
    if (fixture == NULL)
        return NULL;
    const size_t stride = (width * sizeof(uint16_t) + 31u) & ~(size_t)31u;
    const size_t plane = stride * (height + 16u);
    const size_t temporary = (width + 16u) * sizeof(uint32_t);
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
    uint32_t *tmp = (uint32_t *)(fixture->storage + plane * 4u) + 8;
    const size_t span = width + 16u;
    buf->tmp.mu1 = tmp;
    buf->tmp.mu2 = tmp + span;
    buf->tmp.ref = tmp + span * 2u;
    buf->tmp.dis = tmp + span * 3u;
    buf->tmp.ref_dis = tmp + span * 4u;
    buf->tmp.ref_convol = tmp + span * 5u;
    buf->tmp.dis_convol = tmp + span * 6u;
    for (unsigned i = 0; i < VIF_LOG2_TABLE_SIZE; ++i) {
        fixture->state.log2_table[i] =
            (uint16_t)roundf(log2f((float)(VIF_LOG2_TABLE_OFFSET + i)) * 2048);
    }
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

static int compare_statistic(unsigned width, unsigned bpc, unsigned scale, unsigned pattern)
{
    VifStageFixture *scalar = alloc_fixture(width, 7);
    VifStageFixture *simd = alloc_fixture(width, 7);
    if (scalar == NULL || simd == NULL) {
        free_fixture(scalar);
        free_fixture(simd);
        return -1;
    }
    fill_fixture(scalar, bpc, scale, pattern);
    fill_fixture(simd, bpc, scale, pattern);
    float scalar_result[2] = {0};
    float simd_result[2] = {0};
    if (bpc == 8u && scale == 0u) {
        vif_statistic_8(&scalar->state, scalar_result, scalar_result + 1, width, 7);
        vif_statistic_8_avx2(&simd->state, simd_result, simd_result + 1, width, 7);
    } else {
        vif_statistic_16(&scalar->state, scalar_result, scalar_result + 1, width, 7, (int)bpc,
                         (int)scale);
        vif_statistic_16_avx2(&simd->state, simd_result, simd_result + 1, width, 7, (int)bpc,
                              (int)scale);
    }
    uint32_t scalar_bits[2];
    uint32_t simd_bits[2];
    static_assert(sizeof(scalar_bits) == sizeof(scalar_result));
    memcpy(scalar_bits, scalar_result, sizeof(scalar_bits));
    memcpy(simd_bits, simd_result, sizeof(simd_bits));
    const int different = !isfinite(simd_result[0]) || !isfinite(simd_result[1]) ||
                          scalar_bits[0] != simd_bits[0] || scalar_bits[1] != simd_bits[1] ||
                          memcmp(scalar->storage, simd->storage, scalar->bytes) != 0;
    free_fixture(scalar);
    free_fixture(simd);
    return different;
}

static int compare_depth(unsigned width, unsigned bpc)
{
    for (unsigned scale = 0; scale < 4; ++scale) {
        for (unsigned pattern = 0; pattern < 2; ++pattern) {
            const int result = compare_statistic(width, bpc, scale, pattern);
            if (result != 0)
                return result;
        }
    }
    return 0;
}

static char *test_integer_vif_avx2_stages(void)
{
    if (!simd_test_have_avx2())
        return NULL;
    const unsigned widths[] = {9, 15, 16, 17, 31, 32, 33, 63, 64, 65, 127, 257};
    const unsigned depths[] = {8, 9, 10, 11, 12, 13, 14, 15, 16};
    for (size_t w = 0; w < sizeof(widths) / sizeof(widths[0]); ++w) {
        for (size_t b = 0; b < sizeof(depths) / sizeof(depths[0]); ++b) {
            mu_assert("integer VIF AVX2 statistic/vertical buffers differ from scalar",
                      compare_depth(widths[w], depths[b]) == 0);
        }
    }
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_integer_vif_avx2_stages);
    return NULL;
}

// NOLINTEND(modernize-use-nullptr)
