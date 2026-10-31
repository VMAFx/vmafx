/**
 * Copyright 2026 Lusoris
 * SPDX-License-Identifier: EUPL-1.2
 *
 * Buffer layout and allocation verification for integer ADM (Netflix/vmaf 4611e47a7).
 * Verifies that the low-resolution detail bands (scale 1..3) are sized by
 * buf_sz_s1 (rows_s1 = ((h + 1) / 2 + 1) / 2 + 2) rather than scale-0 buf_sz_one,
 * reducing memory allocation while providing sufficient rows for multi-scale DWT.
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"
#include "feature/feature_extractor.h"
#include "feature/integer_adm.h"
#include "mem.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

/* Mirror of AdmState initial fields to inspect AdmBuffer layout in tests. */
typedef struct AdmTestState {
    size_t integer_stride;
    AdmBuffer buf;
} AdmTestState;

typedef struct AllocTestCase {
    unsigned w;
    unsigned h;
    const char *label;
} AllocTestCase;

/* Positive cases: standard production resolutions. */
static const AllocTestCase POSITIVE_CASES[] = {
    {576u, 324u, "576x324 golden pair size"},
    {640u, 480u, "640x480 VGA"},
    {1920u, 1080u, "1920x1080"},
};
#define NUM_POSITIVE_CASES (sizeof(POSITIVE_CASES) / sizeof(POSITIVE_CASES[0]))

/* Boundary cases: odd widths/heights and 1-scale minimum boundary (17x17).
 * Also exercises sizes from upstream check_adm.c post-dwt geometry suite. */
static const AllocTestCase BOUNDARY_CASES[] = {
    {17u, 17u, "17x17 1-scale floor boundary"},     {17u, 33u, "17x33 odd height at floor"},
    {33u, 17u, "33x17 odd width at floor"},         {33u, 21u, "33x21 upstream post-dwt size"},
    {65u, 49u, "65x49 upstream post-dwt size"},     {127u, 93u, "127x93 upstream post-dwt size"},
    {257u, 193u, "257x193 upstream post-dwt size"},
};
#define NUM_BOUNDARY_CASES (sizeof(BOUNDARY_CASES) / sizeof(BOUNDARY_CASES[0]))

/* Negative cases: geometries below the 17x17 extractor minimum. */
static const AllocTestCase NEGATIVE_CASES[] = {
    {16u, 16u, "16x16 below floor"},
    {16u, 17u, "16x17 width below floor"},
    {17u, 16u, "17x16 height below floor"},
    {8u, 8u, "8x8 far below floor"},
};
#define NUM_NEGATIVE_CASES (sizeof(NEGATIVE_CASES) / sizeof(NEGATIVE_CASES[0]))

static int check_band_offsets(const AdmBuffer *buf, size_t buf_sz_one, size_t buf_sz_s1,
                              size_t expected_data_sz)
{
    const char *base = (const char *)buf->data_buf;
    /* SAFETY: Pointer arithmetic verifies offsets against contiguous slab base. */
    const char *p_ref_a = (const char *)buf->i4_ref_dwt2.band_a;
    const char *p_ref_h = (const char *)buf->i4_ref_dwt2.band_h;
    const char *p_ref_v = (const char *)buf->i4_ref_dwt2.band_v;
    const char *p_ref_d = (const char *)buf->i4_ref_dwt2.band_d;
    const char *p_dis_a = (const char *)buf->i4_dis_dwt2.band_a;
    const char *p_dis_h = (const char *)buf->i4_dis_dwt2.band_h;
    const char *p_dis_v = (const char *)buf->i4_dis_dwt2.band_v;
    const char *p_dis_d = (const char *)buf->i4_dis_dwt2.band_d;
    const char *p_end = (const char *)buf->i4_csf_f.band_d + buf_sz_s1;

    if ((size_t)(p_ref_h - p_ref_a) != buf_sz_one)
        return -6;
    if ((size_t)(p_ref_v - p_ref_h) != buf_sz_s1)
        return -1;
    if ((size_t)(p_ref_d - p_ref_v) != buf_sz_s1)
        return -2;
    if ((size_t)(p_dis_h - p_dis_a) != buf_sz_one)
        return -7;
    if ((size_t)(p_dis_v - p_dis_h) != buf_sz_s1)
        return -3;
    if ((size_t)(p_dis_d - p_dis_v) != buf_sz_s1)
        return -4;
    if ((size_t)(p_end - base) != expected_data_sz)
        return -5;
    return 0;
}

static char *verify_valid_alloc(VmafFeatureExtractor *fex, unsigned w, unsigned h)
{
    void *priv = calloc(1, fex->priv_size);
    mu_assert("fex priv calloc failed", priv != NULL);
    fex->priv = priv;

    int rc = fex->init(fex, VMAF_PIX_FMT_YUV420P, 8u, w, h);
    mu_assert("fex init failed for valid dimension", rc == 0);

    const size_t ind_size_x = ALIGN_CEIL(((w + 1) / 2) * sizeof(int32_t));
    const size_t buf_sz_one = ind_size_x * ((h + 1) / 2);
    const size_t rows_s1 = (size_t)(((h + 1) / 2 + 1) / 2) + 2;
    const size_t buf_sz_s1 = ind_size_x * rows_s1;
    const size_t expected_data_sz = 10 * buf_sz_one + 2 * buf_sz_one + 18 * buf_sz_s1;

    const AdmTestState *s = (const AdmTestState *)fex->priv;
    int check_rc = check_band_offsets(&s->buf, buf_sz_one, buf_sz_s1, expected_data_sz);

    (void)fex->close(fex);
    free(priv);
    fex->priv = NULL;

    mu_assert("band_h - band_a must equal buf_sz_one", check_rc != -6 && check_rc != -7);
    mu_assert("band_v - band_h must equal buf_sz_s1", check_rc != -1 && check_rc != -3);
    mu_assert("band_d - band_v must equal buf_sz_s1", check_rc != -2 && check_rc != -4);
    mu_assert("total slab span must equal data_sz", check_rc != -5);
    return NULL;
}

static char *test_positive_cases(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("adm");
    mu_assert("adm extractor missing", fex != NULL);

    for (size_t i = 0; i < NUM_POSITIVE_CASES; ++i) {
        char *msg = verify_valid_alloc(fex, POSITIVE_CASES[i].w, POSITIVE_CASES[i].h);
        if (msg)
            return msg;
    }
    return NULL;
}

static char *test_boundary_cases(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("adm");
    mu_assert("adm extractor missing", fex != NULL);

    for (size_t i = 0; i < NUM_BOUNDARY_CASES; ++i) {
        char *msg = verify_valid_alloc(fex, BOUNDARY_CASES[i].w, BOUNDARY_CASES[i].h);
        if (msg)
            return msg;
    }
    return NULL;
}

static char *test_negative_cases(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("adm");
    mu_assert("adm extractor missing", fex != NULL);

    for (size_t i = 0; i < NUM_NEGATIVE_CASES; ++i) {
        void *priv = calloc(1, fex->priv_size);
        mu_assert("fex priv calloc failed", priv != NULL);
        fex->priv = priv;
        int rc = fex->init(fex, VMAF_PIX_FMT_YUV420P, 8u, NEGATIVE_CASES[i].w, NEGATIVE_CASES[i].h);
        (void)fex->close(fex);
        free(priv);
        fex->priv = NULL;
        mu_assert("fex init must reject below-minimum geometry", rc < 0);
    }
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_positive_cases);
    mu_run_test(test_boundary_cases);
    mu_run_test(test_negative_cases);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
