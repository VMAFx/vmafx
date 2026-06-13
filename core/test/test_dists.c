/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  DISTS-Sq feature extractor — structural + missing-model tests.
 *
 *  Full inference is exercised by the DNN smoke gate when
 *  model/tiny/dists_sq.onnx is available; this file verifies the
 *  registration / option-table / init-rejection contract.
 */

#include "tiny_ai_test_template.h"

#include "dnn/tiny_extractor_template.h"

VMAF_TINY_AI_DEFINE_REGISTRATION_TESTS("dists_sq", "dists_sq", "VMAF_DISTS_SQ_MODEL_PATH", dists_sq)

static void put_le16(uint8_t *dst, uint16_t v)
{
    dst[0] = (uint8_t)(v & 0xffu);
    dst[1] = (uint8_t)(v >> 8u);
}

static char *test_dists_high_bitdepth_rgb_normalisation(void)
{
    uint8_t y8[4] = {64u, 96u, 128u, 160u};
    uint8_t u8[1] = {128u};
    uint8_t v8[1] = {128u};
    uint8_t y10[8];
    uint8_t u10[2];
    uint8_t v10[2];
    for (unsigned i = 0u; i < 4u; ++i) {
        put_le16(y10 + (size_t)i * 2u, (uint16_t)y8[i] << 2u);
    }
    put_le16(u10, (uint16_t)u8[0] << 2u);
    put_le16(v10, (uint16_t)v8[0] << 2u);

    VmafPicture pic8 = {
        .pix_fmt = VMAF_PIX_FMT_YUV420P,
        .bpc = 8u,
        .w = {2u, 1u, 1u},
        .h = {2u, 1u, 1u},
        .stride = {2, 1, 1},
        .data = {y8, u8, v8},
    };
    VmafPicture pic10 = {
        .pix_fmt = VMAF_PIX_FMT_YUV420P,
        .bpc = 10u,
        .w = {2u, 1u, 1u},
        .h = {2u, 1u, 1u},
        .stride = {4, 2, 2},
        .data = {y10, u10, v10},
    };
    uint8_t r8[4] = {0};
    uint8_t g8[4] = {0};
    uint8_t b8[4] = {0};
    uint8_t r10[4] = {0};
    uint8_t g10[4] = {0};
    uint8_t b10[4] = {0};

    int rc = vmaf_tiny_ai_yuv_to_rgb8_planes(&pic8, r8, g8, b8);
    mu_assert("8-bit conversion must pass", rc == 0);
    rc = vmaf_tiny_ai_yuv_to_rgb8_planes(&pic10, r10, g10, b10);
    mu_assert("10-bit conversion must pass", rc == 0);
    mu_assert("10-bit R plane must normalise to matching 8-bit RGB",
              memcmp(r8, r10, sizeof(r8)) == 0);
    mu_assert("10-bit G plane must normalise to matching 8-bit RGB",
              memcmp(g8, g10, sizeof(g8)) == 0);
    mu_assert("10-bit B plane must normalise to matching 8-bit RGB",
              memcmp(b8, b10, sizeof(b8)) == 0);
    return NULL;
}

char *run_tests(void)
{
    VMAF_TINY_AI_RUN_REGISTRATION_TESTS(dists_sq);
    mu_run_test(test_dists_high_bitdepth_rgb_normalisation);
    return NULL;
}
