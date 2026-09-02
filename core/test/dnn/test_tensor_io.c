/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 */

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"

#include "dnn/tensor_io.h"

static char *test_luma_to_f32_unnormalized(void)
{
    uint8_t src[16] = {0, 64, 128, 192, 255, 0, 128, 255, 10, 20, 30, 40, 50, 60, 70, 80};
    float dst[16];
    int err = vmaf_tensor_from_luma(src, 16, 16, 1, VMAF_TENSOR_LAYOUT_NCHW, VMAF_TENSOR_DTYPE_F32,
                                    NULL, NULL, dst);
    mu_assert("vmaf_tensor_from_luma failed", err == 0);
    mu_assert("0 did not map to 0", dst[0] == 0.0f);
    mu_assert("255 did not map to 1", fabsf(dst[4] - 1.0f) < 1e-6f);
    mu_assert("128 did not round correctly", fabsf(dst[2] - (128.0f / 255.0f)) < 1e-6f);
    return NULL;
}

static char *test_f16_roundtrip(void)
{
    const float inputs[] = {0.0f, 1.0f, -1.0f, 0.5f, 2.5f, 0.123456f, -7.25f};
    const size_t n = sizeof(inputs) / sizeof(inputs[0]);
    uint16_t h[16];
    float back[16];
    vmaf_f32_to_f16(inputs, h, n);
    vmaf_f16_to_f32(h, back, n);
    for (size_t i = 0; i < n; ++i) {
        float tol = fabsf(inputs[i]) * 1e-3f + 1e-3f;
        mu_assert("f16 roundtrip exceeded tolerance", fabsf(inputs[i] - back[i]) <= tol);
    }
    return NULL;
}

static char *test_luma_roundtrip(void)
{
    uint8_t src[64];
    uint8_t dst[64];
    float tensor[64];
    for (int i = 0; i < 64; ++i)
        src[i] = (uint8_t)(i * 4 % 256);
    int err = vmaf_tensor_from_luma(src, 8, 8, 8, VMAF_TENSOR_LAYOUT_NCHW, VMAF_TENSOR_DTYPE_F32,
                                    NULL, NULL, tensor);
    mu_assert("luma→tensor failed", err == 0);
    err = vmaf_tensor_to_luma(tensor, VMAF_TENSOR_LAYOUT_NCHW, VMAF_TENSOR_DTYPE_F32, 8, 8, NULL,
                              NULL, dst, 8);
    mu_assert("tensor→luma failed", err == 0);
    for (int i = 0; i < 64; ++i) {
        mu_assert("luma roundtrip differed", src[i] == dst[i]);
    }
    return NULL;
}

static char *test_rejects_bad_args(void)
{
    uint8_t src[4] = {0};
    float dst[4];
    int err = vmaf_tensor_from_luma(NULL, 2, 2, 2, VMAF_TENSOR_LAYOUT_NCHW, VMAF_TENSOR_DTYPE_F32,
                                    NULL, NULL, dst);
    mu_assert("expected -EINVAL on NULL src", err < 0);
    err = vmaf_tensor_from_luma(src, 1, 2, 2, VMAF_TENSOR_LAYOUT_NCHW, VMAF_TENSOR_DTYPE_F32, NULL,
                                NULL, dst);
    mu_assert("expected -EINVAL on stride < width", err < 0);
    return NULL;
}

static char *test_rgb_imagenet_known_values(void)
{
    /* 2x2 frame, each channel a constant. ImageNet mean/std:
     *   R: (v/255 - 0.485) / 0.229
     *   G: (v/255 - 0.456) / 0.224
     *   B: (v/255 - 0.406) / 0.225
     * For v=0 this is the classic "most-negative ImageNet value" per channel. */
    uint8_t r[4] = {0, 0, 0, 0};
    uint8_t g[4] = {0, 0, 0, 0};
    uint8_t b[4] = {0, 0, 0, 0};
    float dst[12];
    int err = vmaf_tensor_from_rgb_imagenet(r, 2, g, 2, b, 2, 2, 2, dst);
    mu_assert("rgb imagenet failed for zero input", err == 0);
    const float expected_r = (0.0f - 0.485f) / 0.229f;
    const float expected_g = (0.0f - 0.456f) / 0.224f;
    const float expected_b = (0.0f - 0.406f) / 0.225f;
    for (int i = 0; i < 4; ++i) {
        mu_assert("R plane mismatch at zero", fabsf(dst[i] - expected_r) < 1e-5f);
        mu_assert("G plane mismatch at zero", fabsf(dst[4 + i] - expected_g) < 1e-5f);
        mu_assert("B plane mismatch at zero", fabsf(dst[8 + i] - expected_b) < 1e-5f);
    }

    /* v=255 → (1 - mean) / std */
    uint8_t r2[4] = {255, 255, 255, 255};
    uint8_t g2[4] = {255, 255, 255, 255};
    uint8_t b2[4] = {255, 255, 255, 255};
    err = vmaf_tensor_from_rgb_imagenet(r2, 2, g2, 2, b2, 2, 2, 2, dst);
    mu_assert("rgb imagenet failed for 255 input", err == 0);
    const float r255 = (1.0f - 0.485f) / 0.229f;
    const float g255 = (1.0f - 0.456f) / 0.224f;
    const float b255 = (1.0f - 0.406f) / 0.225f;
    for (int i = 0; i < 4; ++i) {
        mu_assert("R plane mismatch at 255", fabsf(dst[i] - r255) < 1e-5f);
        mu_assert("G plane mismatch at 255", fabsf(dst[4 + i] - g255) < 1e-5f);
        mu_assert("B plane mismatch at 255", fabsf(dst[8 + i] - b255) < 1e-5f);
    }
    return NULL;
}

static char *test_rgb_imagenet_nchw_layout(void)
{
    /* Verify planes are written contiguously in NCHW order (R first,
     * then G, then B). Distinct per-channel values should land in
     * distinct 1/3 segments of the destination buffer. */
    uint8_t r[6] = {10, 20, 30, 40, 50, 60};
    uint8_t g[6] = {70, 80, 90, 100, 110, 120};
    uint8_t b[6] = {130, 140, 150, 160, 170, 180};
    float dst[3 * 6];
    int err = vmaf_tensor_from_rgb_imagenet(r, 3, g, 3, b, 3, 3, 2, dst);
    mu_assert("rgb imagenet 3x2 failed", err == 0);
    /* The R plane's first element should reflect r[0]=10: (10/255 - 0.485) / 0.229. */
    const float first_r = ((10.0f / 255.0f) - 0.485f) / 0.229f;
    const float first_g = ((70.0f / 255.0f) - 0.456f) / 0.224f;
    const float first_b = ((130.0f / 255.0f) - 0.406f) / 0.225f;
    mu_assert("R plane not first in NCHW order", fabsf(dst[0] - first_r) < 1e-5f);
    mu_assert("G plane not second in NCHW order", fabsf(dst[6] - first_g) < 1e-5f);
    mu_assert("B plane not third in NCHW order", fabsf(dst[12] - first_b) < 1e-5f);
    return NULL;
}

static char *test_rgb_imagenet_rejects_bad_args(void)
{
    uint8_t c[4] = {0};
    float dst[12];
    int err = vmaf_tensor_from_rgb_imagenet(NULL, 2, c, 2, c, 2, 2, 2, dst);
    mu_assert("expected -EINVAL on NULL R", err < 0);
    err = vmaf_tensor_from_rgb_imagenet(c, 2, NULL, 2, c, 2, 2, 2, dst);
    mu_assert("expected -EINVAL on NULL G", err < 0);
    err = vmaf_tensor_from_rgb_imagenet(c, 2, c, 2, NULL, 2, 2, 2, dst);
    mu_assert("expected -EINVAL on NULL B", err < 0);
    err = vmaf_tensor_from_rgb_imagenet(c, 2, c, 2, c, 2, 2, 2, NULL);
    mu_assert("expected -EINVAL on NULL dst", err < 0);
    err = vmaf_tensor_from_rgb_imagenet(c, 1, c, 2, c, 2, 2, 2, dst);
    mu_assert("expected -EINVAL on stride_r < width", err < 0);
    err = vmaf_tensor_from_rgb_imagenet(c, 2, c, 1, c, 2, 2, 2, dst);
    mu_assert("expected -EINVAL on stride_g < width", err < 0);
    err = vmaf_tensor_from_rgb_imagenet(c, 2, c, 2, c, 1, 2, 2, dst);
    mu_assert("expected -EINVAL on stride_b < width", err < 0);
    err = vmaf_tensor_from_rgb_imagenet(c, 2, c, 2, c, 2, 0, 2, dst);
    mu_assert("expected -EINVAL on zero width", err < 0);
    err = vmaf_tensor_from_rgb_imagenet(c, 2, c, 2, c, 2, 2, 0, dst);
    mu_assert("expected -EINVAL on zero height", err < 0);
    return NULL;
}

static char *test_f16_special_values(void)
{
    /* Cover the NaN/inf and flush-to-zero branches in the soft-float
     * f32->f16 converter. The subnormal-rounding branch of
     * tensor_io.c f32_to_f16_one is exercised separately in
     * test_f16_subnormal_range — splitting the two keeps each function
     * under clang-tidy's branch-count threshold. Large-finite overflow
     * (which must map to inf, not NaN) is in test_f16_finite_overflow_to_inf. */
    float specials[6] = {
        INFINITY,          /* exp >= 31, input exp == 0xff   -> +inf */
        -INFINITY,         /* exp >= 31, input exp == 0xff   -> -inf */
        NAN,               /* exp >= 31, input NaN           -> NaN propagation */
        1e-8f,             /* exp < -10                      -> flush-to-zero */
        -1e-8f,    1e-30f, /* exp < -10                      -> flush-to-zero */
    };
    uint16_t h[6];
    float back[6];
    vmaf_f32_to_f16(specials, h, 6);
    vmaf_f16_to_f32(h, back, 6);

    mu_assert("+inf survives roundtrip", isinf(back[0]) && back[0] > 0.0f);
    mu_assert("-inf survives roundtrip", isinf(back[1]) && back[1] < 0.0f);
    mu_assert("NaN survives roundtrip", isnan(back[2]));
    mu_assert("tiny positive becomes subnormal or zero", back[3] >= 0.0f && back[3] < 1e-3f);
    mu_assert("tiny negative becomes subnormal or zero", back[4] <= 0.0f && back[4] > -1e-3f);
    mu_assert("underflow flushes to zero", back[5] == 0.0f);
    return NULL;
}

/* Regression: a large *finite* f32 that overflows the f16 range must
 * convert to a clean ±inf, NOT a NaN. The earlier exp>=31 branch
 * propagated a non-zero mantissa for any overflowing finite value, so
 * 70000.0f / 1e30f silently became NaN — poisoning every downstream
 * score. NaN must only survive for a genuine f32 NaN input. Mirrors the
 * exp_f==128 (inf/nan) vs exp_f>15 (overflow) split in
 * ort_backend.c:fp32_to_fp16. */
static char *test_f16_finite_overflow_to_inf(void)
{
    /* 65504 is the largest finite f16; everything above overflows. */
    float overflow[4] = {70000.0f, 1.0e30f, -1.0e30f, -70000.0f};
    uint16_t h[4];
    float back[4];
    vmaf_f32_to_f16(overflow, h, 4);
    vmaf_f16_to_f32(h, back, 4);

    mu_assert("large +finite overflows to +inf, not NaN", isinf(back[0]) && back[0] > 0.0f);
    mu_assert("1e30 overflows to +inf, not NaN", isinf(back[1]) && back[1] > 0.0f);
    mu_assert("-1e30 overflows to -inf, not NaN", isinf(back[2]) && back[2] < 0.0f);
    mu_assert("large -finite overflows to -inf, not NaN", isinf(back[3]) && back[3] < 0.0f);
    return NULL;
}

/* Drives the subnormal-rounding branch (lines 33-35) of f32_to_f16_one.
 *
 * The branch fires when the fp16 exponent lands in [-10, 0], which
 * corresponds to fp32 magnitudes in roughly [3.0e-8, 3.0e-5]. The
 * original test_f16_special_values comment claimed 1e-8f triggered this
 * path, but 1e-8f has fp32 exp ~-27 which maps to fp16 exp ~-12 — that
 * trips the flush-to-zero branch (line 31), not the subnormal-rounding
 * branch. 1.0e-6f and 2.0e-5f land squarely in the subnormal-rounding
 * window. */
static char *test_f16_subnormal_range(void)
{
    float subn[2] = {
        1.0e-6f, /* exp in [-10, 0] -> subnormal path */
        2.0e-5f, /* exp in [-10, 0] -> subnormal path */
    };
    uint16_t h[2];
    float back[2];
    vmaf_f32_to_f16(subn, h, 2);
    vmaf_f16_to_f32(h, back, 2);

    /* fp16 subnormal granularity is 2^-24 ≈ 6e-8; round-trip is lossy
     * but the converted value stays bounded and same-sign. */
    mu_assert("subnormal-range 1e-6 stays non-negative and bounded",
              back[0] >= 0.0f && back[0] < 1e-3f);
    mu_assert("subnormal-range 2e-5 round-trips near input",
              back[1] >= 0.0f && fabsf(back[1] - 2.0e-5f) < 5.0e-5f);
    return NULL;
}

static char *test_f16_to_f32_subnormal(void)
{
    /* Hand-construct a fp16 subnormal (exp=0, mant!=0) to drive the
     * normalize-loop branch (line 52-58). 0x0001 is the smallest positive
     * fp16 subnormal: 2^-24. */
    uint16_t subnormal[2] = {0x0001u, 0x8001u};
    float out[2];
    vmaf_f16_to_f32(subnormal, out, 2);
    const float expected = 1.0f / (float)(1u << 24);
    mu_assert("smallest positive subnormal", fabsf(out[0] - expected) < 1e-9f);
    mu_assert("smallest negative subnormal", fabsf(out[1] + expected) < 1e-9f);
    return NULL;
}

static char *test_from_luma_zero_std_rejected(void)
{
    /* std == 0 must be rejected to avoid divide-by-zero (line 97). */
    uint8_t src[4] = {0, 64, 128, 255};
    float dst[4];
    float zero_std = 0.0f;
    float zero_mean = 0.0f;
    int err = vmaf_tensor_from_luma(src, 2, 2, 2, VMAF_TENSOR_LAYOUT_NCHW, VMAF_TENSOR_DTYPE_F32,
                                    &zero_mean, &zero_std, dst);
    mu_assert("zero std must be rejected", err < 0);
    return NULL;
}

static char *test_from_luma_f16_path(void)
{
    /* Drive the F16 destination branch (lines 111-117). */
    uint8_t src[4] = {0, 64, 128, 255};
    uint16_t dst[4] = {0xffffu, 0xffffu, 0xffffu, 0xffffu};
    int err = vmaf_tensor_from_luma(src, 2, 2, 2, VMAF_TENSOR_LAYOUT_NCHW, VMAF_TENSOR_DTYPE_F16,
                                    NULL, NULL, dst);
    mu_assert("F16 luma conversion ok", err == 0);
    /* 0 must round to fp16 +0.0 (0x0000). */
    mu_assert("0 maps to fp16 zero", dst[0] == 0x0000u);
    /* 255 maps to ~1.0; fp16 1.0 is 0x3c00. */
    mu_assert("255 maps near fp16 1.0", dst[3] == 0x3c00u);
    return NULL;
}

static char *test_from_luma_invalid_dtype(void)
{
    /* Drive the dtype-default reject (line 121). Passing an out-of-enum
     * value as dtype is a coding error we still want to fail closed on. */
    uint8_t src[4] = {0, 0, 0, 0};
    float dst[4];
    int err = vmaf_tensor_from_luma(src, 2, 2, 2, VMAF_TENSOR_LAYOUT_NCHW, (VmafTensorDType)99,
                                    NULL, NULL, dst);
    mu_assert("unknown dtype rejected", err < 0);
    return NULL;
}

static char *test_to_luma_rejects_bad_args(void)
{
    /* Drive the input-validation branch in vmaf_tensor_to_luma (line 166). */
    float src[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    uint8_t dst[4];
    int err = vmaf_tensor_to_luma(NULL, VMAF_TENSOR_LAYOUT_NCHW, VMAF_TENSOR_DTYPE_F32, 2, 2, NULL,
                                  NULL, dst, 2);
    mu_assert("NULL src rejected", err < 0);
    err = vmaf_tensor_to_luma(src, VMAF_TENSOR_LAYOUT_NCHW, VMAF_TENSOR_DTYPE_F32, 2, 2, NULL, NULL,
                              NULL, 2);
    mu_assert("NULL dst rejected", err < 0);
    err = vmaf_tensor_to_luma(src, VMAF_TENSOR_LAYOUT_NCHW, VMAF_TENSOR_DTYPE_F32, 2, 2, NULL, NULL,
                              dst, 1);
    mu_assert("stride < width rejected", err < 0);
    err = vmaf_tensor_to_luma(src, VMAF_TENSOR_LAYOUT_NCHW, (VmafTensorDType)99, 2, 2, NULL, NULL,
                              dst, 2);
    mu_assert("unknown dtype rejected", err < 0);
    return NULL;
}

static char *test_to_luma_clamps_out_of_range(void)
{
    /* Drive the < 0 and > 255 clamps (lines 188, 190). With mean=0 std=1,
     * a tensor value of 2.0 maps to 510 (clamped to 255), -1.0 maps to
     * -255 (clamped to 0). */
    float src[4] = {-1.0f, 2.0f, 0.5f, 0.0f};
    uint8_t dst[4] = {99, 99, 99, 99};
    int err = vmaf_tensor_to_luma(src, VMAF_TENSOR_LAYOUT_NCHW, VMAF_TENSOR_DTYPE_F32, 2, 2, NULL,
                                  NULL, dst, 2);
    mu_assert("to_luma ok", err == 0);
    mu_assert("negative clamps to 0", dst[0] == 0);
    mu_assert(">1.0 clamps to 255", dst[1] == 255);
    /* 0.5 * 255 = 127.5 → round-to-even → 128. */
    mu_assert("0.5 rounds to 128", dst[2] == 128);
    mu_assert("0.0 maps to 0", dst[3] == 0);
    return NULL;
}

static char *test_to_luma_f16_path(void)
{
    /* Drive the F16 source branch (lines 179-180). 0x3c00 is fp16 1.0,
     * 0x0000 is fp16 0. */
    uint16_t src[4] = {0x0000u, 0x3c00u, 0x3800u, 0x0000u}; /* 0, 1, 0.5, 0 */
    uint8_t dst[4] = {99, 99, 99, 99};
    int err = vmaf_tensor_to_luma(src, VMAF_TENSOR_LAYOUT_NCHW, VMAF_TENSOR_DTYPE_F16, 2, 2, NULL,
                                  NULL, dst, 2);
    mu_assert("f16 to_luma ok", err == 0);
    mu_assert("fp16 0 → 0", dst[0] == 0);
    mu_assert("fp16 1.0 → 255", dst[1] == 255);
    /* 0.5 * 255 = 127.5 → round-to-even → 128. */
    mu_assert("fp16 0.5 → 128", dst[2] == 128);
    return NULL;
}

static char *test_plane16_10bit_roundtrip(void)
{
    /* ADR-0170 / T6-4: a packed uint16 LE 10-bit plane must round-trip
     * through vmaf_tensor_from_plane16 + vmaf_tensor_to_plane16 with
     * value preservation (modulo round-to-even on the exact .5 boundary
     * which cannot appear for integer inputs divided by 1023). */
    const int W = 4;
    const int H = 2;
    const int BPC = 10;
    const uint16_t src[8] = {0, 100, 511, 512, 1023, 1022, 256, 768};
    float tensor[8];
    uint16_t dst[8] = {0};

    int err =
        vmaf_tensor_from_plane16(src, W * sizeof(uint16_t), W, H, BPC, VMAF_TENSOR_LAYOUT_NCHW,
                                 VMAF_TENSOR_DTYPE_F32, NULL, NULL, tensor);
    mu_assert("from_plane16 failed", err == 0);
    /* Spot-check normalisation against (1<<bpc)-1 = 1023. */
    mu_assert("0 → 0.0f", tensor[0] == 0.0f);
    mu_assert("1023 → 1.0f", tensor[4] == 1.0f);

    err = vmaf_tensor_to_plane16(tensor, VMAF_TENSOR_LAYOUT_NCHW, VMAF_TENSOR_DTYPE_F32, W, H, BPC,
                                 NULL, NULL, dst, W * sizeof(uint16_t));
    mu_assert("to_plane16 failed", err == 0);
    for (int i = 0; i < W * H; ++i) {
        mu_assert("10-bit round-trip byte-identical", dst[i] == src[i]);
    }
    return NULL;
}

static char *test_plane16_rejects_bad_bpc(void)
{
    const uint16_t src[4] = {0};
    float tensor[4];
    int err = vmaf_tensor_from_plane16(src, 4u * sizeof(uint16_t), 2, 2, 8, /* bpc too low */
                                       VMAF_TENSOR_LAYOUT_NCHW, VMAF_TENSOR_DTYPE_F32, NULL, NULL,
                                       tensor);
    mu_assert("bpc=8 must be rejected (plane16 is for >=9 bits)", err < 0);
    err = vmaf_tensor_from_plane16(src, 4u * sizeof(uint16_t), 2, 2, 17, /* bpc too high */
                                   VMAF_TENSOR_LAYOUT_NCHW, VMAF_TENSOR_DTYPE_F32, NULL, NULL,
                                   tensor);
    mu_assert("bpc=17 must be rejected", err < 0);
    return NULL;
}

static char *test_plane16_12bit_clamps(void)
{
    /* 12-bit max = 4095. An out-of-range float should clamp rather than
     * overflow the uint16 write. */
    const float tensor[4] = {-0.25f, 0.0f, 1.0f, 1.5f};
    uint16_t dst[4] = {0};
    int err = vmaf_tensor_to_plane16(tensor, VMAF_TENSOR_LAYOUT_NCHW, VMAF_TENSOR_DTYPE_F32, 2, 2,
                                     12, NULL, NULL, dst, 2u * sizeof(uint16_t));
    mu_assert("to_plane16 12-bit failed", err == 0);
    mu_assert("-0.25 → clamped to 0", dst[0] == 0);
    mu_assert("0.0 → 0", dst[1] == 0);
    mu_assert("1.0 → 4095 (12-bit max)", dst[2] == 4095);
    mu_assert("1.5 → clamped to 4095", dst[3] == 4095);
    return NULL;
}

static char *test_plane16_f16_roundtrip(void)
{
    const uint16_t src[4] = {0, 341, 682, 1023};
    uint16_t tensor[4] = {0};
    uint16_t dst[4] = {0};
    int err =
        vmaf_tensor_from_plane16(src, 2u * sizeof(uint16_t), 2, 2, 10, VMAF_TENSOR_LAYOUT_NCHW,
                                 VMAF_TENSOR_DTYPE_F16, NULL, NULL, tensor);
    mu_assert("from_plane16 f16 failed", err == 0);
    err = vmaf_tensor_to_plane16(tensor, VMAF_TENSOR_LAYOUT_NCHW, VMAF_TENSOR_DTYPE_F16, 2, 2, 10,
                                 NULL, NULL, dst, 2u * sizeof(uint16_t));
    mu_assert("to_plane16 f16 failed", err == 0);
    mu_assert("0 survives f16 plane path", dst[0] == 0);
    mu_assert("max survives f16 plane path", dst[3] == 1023);
    return NULL;
}

static char *test_plane16_rejects_more_bad_args(void)
{
    const uint16_t src[4] = {0};
    float tensor[4] = {0};
    uint16_t dst[4] = {0};
    float zero = 0.0f;
    int err =
        vmaf_tensor_from_plane16(src, 2u * sizeof(uint16_t), 2, 2, 10, VMAF_TENSOR_LAYOUT_NCHW,
                                 VMAF_TENSOR_DTYPE_F32, NULL, &zero, tensor);
    mu_assert("from_plane16 zero std rejected", err == -EINVAL);
    err = vmaf_tensor_from_plane16(src, 2u * sizeof(uint16_t), 2, 2, 10, VMAF_TENSOR_LAYOUT_NCHW,
                                   (VmafTensorDType)99, NULL, NULL, tensor);
    mu_assert("from_plane16 unknown dtype rejected", err == -EINVAL);
    err = vmaf_tensor_to_plane16(tensor, VMAF_TENSOR_LAYOUT_NCHW, (VmafTensorDType)99, 2, 2, 10,
                                 NULL, NULL, dst, 2u * sizeof(uint16_t));
    mu_assert("to_plane16 unknown dtype rejected", err == -EINVAL);
    err = vmaf_tensor_to_plane16(tensor, VMAF_TENSOR_LAYOUT_NCHW, VMAF_TENSOR_DTYPE_F32, 2, 2, 10,
                                 NULL, NULL, dst, sizeof(uint16_t));
    mu_assert("to_plane16 short stride rejected", err == -EINVAL);
    return NULL;
}

/* --- ADR-0550 — auto-resize for NR tiny-model NCHW dispatch --- */

static char *test_resize_identity_matches_legacy(void)
{
    /* When src dims already equal dst dims, the resize helper must be
     * bit-identical to vmaf_tensor_from_luma — the no-resize fast
     * path forwards to it verbatim. */
    uint8_t src[16] = {0, 32, 64, 96, 128, 160, 192, 224, 16, 48, 80, 112, 144, 176, 208, 240};
    float legacy[16];
    float resized[16];
    int e1 = vmaf_tensor_from_luma(src, 4u, 4, 4, VMAF_TENSOR_LAYOUT_NCHW, VMAF_TENSOR_DTYPE_F32,
                                   NULL, NULL, legacy);
    int e2 = vmaf_tensor_from_luma_resize(src, 4u, 4, 4, 4, 4, VMAF_TENSOR_LAYOUT_NCHW,
                                          VMAF_TENSOR_DTYPE_F32, NULL, NULL,
                                          VMAF_TINY_RESIZE_BILINEAR, resized);
    mu_assert("identity legacy call failed", e1 == 0);
    mu_assert("identity resize call failed", e2 == 0);
    for (int i = 0; i < 16; ++i) {
        /* Intentional exact float equality: the identity-resize path must
         * produce bit-identical output to the legacy path. */
        mu_assert("identity path must be bit-identical", legacy[i] == resized[i]); /* bit-exact */
    }
    return NULL;
}

static char *test_resize_disabled_returns_einval(void)
{
    /* The DISABLED mode is consumed at the libvmaf.c call site (it
     * routes to -ERANGE there). When passed directly to the helper
     * it must -EINVAL so a programming error surfaces. */
    uint8_t src[4] = {10, 20, 30, 40};
    float dst[16] = {0};
    int rc = vmaf_tensor_from_luma_resize(src, 2u, 2, 2, 4, 4, VMAF_TENSOR_LAYOUT_NCHW,
                                          VMAF_TENSOR_DTYPE_F32, NULL, NULL,
                                          VMAF_TINY_RESIZE_DISABLED, dst);
    mu_assert("DISABLED must -EINVAL when reached", rc == -EINVAL);
    return NULL;
}

static char *test_resize_bilinear_2x_upsample(void)
{
    /* 2x bilinear upsample of a 2x2 plane. Corner outputs should
     * clamp to the matching source corner (half-pixel-centre + edge
     * replicate); interior outputs should sit strictly inside the
     * corner-value bounding box. */
    uint8_t src[4] = {0, 200, 100, 50};
    float dst[16] = {0};
    int rc = vmaf_tensor_from_luma_resize(src, 2u, 2, 2, 4, 4, VMAF_TENSOR_LAYOUT_NCHW,
                                          VMAF_TENSOR_DTYPE_F32, NULL, NULL,
                                          VMAF_TINY_RESIZE_BILINEAR, dst);
    mu_assert("bilinear 2x upsample failed", rc == 0);
    mu_assert("top-left corner clamps to source[0,0]", dst[0] == 0.0f);
    /* Tolerance: the helper computes `(p * (1/255))` whereas the
     * reference is `p / 255`; the two are mathematically equal but
     * not bit-identical under fp32. 1 ULP at this magnitude is well
     * under 1e-6. */
    {
        const float expected = (float)(50.0 / 255.0);
        const float got = dst[15];
        const float diff = got > expected ? got - expected : expected - got;
        mu_assert("bottom-right corner clamps to source[1,1]", diff < 1e-5f);
    }
    /* dst[5] = (dx=1, dy=1) — heavily weighted toward source[0,0] but
     * interpolating with the other three corners. Sanity-check it
     * lives strictly inside the source corner bounding box. */
    mu_assert("interior sample is between corners", dst[5] > 0.0f && dst[5] < (200.0f / 255.0f));
    return NULL;
}

static char *test_resize_nearest_downsample(void)
{
    /* 4x4 source -> 2x2 nearest downsample. Half-pixel-centre coord
     * means dst[dy,dx] samples src[floor(2*dy+0.5), floor(2*dx+0.5)]
     * = src[2*dy, 2*dx]. With this checkerboard src, dst[0,0] picks
     * src[0,0]=10 and dst[1,1] picks src[2,2]=30 — each output is a
     * deterministic single source sample. */
    uint8_t src[16] = {10, 11, 12, 13, 14, 15, 16, 17, 20, 21, 22, 23, 24, 25, 26, 27};
    float dst[4] = {0};
    int rc = vmaf_tensor_from_luma_resize(src, 4u, 4, 4, 2, 2, VMAF_TENSOR_LAYOUT_NCHW,
                                          VMAF_TENSOR_DTYPE_F32, NULL, NULL,
                                          VMAF_TINY_RESIZE_NEAREST, dst);
    mu_assert("nearest 2x downsample failed", rc == 0);
    /* dst[0,0] -> src[0,0]=10; dst[0,1] -> src[0,2]=12;
     * dst[1,0] -> src[2,0]=20; dst[1,1] -> src[2,2]=22. */
    const float tol = 1e-5f;
    const float e00 = 10.0f / 255.0f;
    const float e01 = 12.0f / 255.0f;
    const float e10 = 20.0f / 255.0f;
    const float e11 = 22.0f / 255.0f;
    mu_assert("nearest dst[0,0] = src[0,0]", fabsf(dst[0] - e00) < tol);
    mu_assert("nearest dst[0,1] = src[0,2]", fabsf(dst[1] - e01) < tol);
    mu_assert("nearest dst[1,0] = src[2,0]", fabsf(dst[2] - e10) < tol);
    mu_assert("nearest dst[1,1] = src[2,2]", fabsf(dst[3] - e11) < tol);
    return NULL;
}

static char *test_resize_bicubic_and_f16_paths(void)
{
    uint8_t src[9] = {0, 30, 60, 90, 120, 150, 180, 210, 240};
    uint16_t dst[16] = {0};
    int rc = vmaf_tensor_from_luma_resize(src, 3u, 3, 3, 4, 4, VMAF_TENSOR_LAYOUT_NCHW,
                                          VMAF_TENSOR_DTYPE_F16, NULL, NULL,
                                          VMAF_TINY_RESIZE_BICUBIC, dst);
    mu_assert("bicubic f16 resize failed", rc == 0);
    mu_assert("bicubic f16 writes nonzero interior", dst[5] != 0u);
    mu_assert("bicubic f16 writes nonzero tail", dst[15] != 0u);
    return NULL;
}

static char *test_resize_zero_std_rejected(void)
{
    uint8_t src[4] = {0, 64, 128, 255};
    float dst[16] = {0};
    float zero = 0.0f;
    int rc = vmaf_tensor_from_luma_resize(src, 2u, 2, 2, 4, 4, VMAF_TENSOR_LAYOUT_NCHW,
                                          VMAF_TENSOR_DTYPE_F32, NULL, &zero,
                                          VMAF_TINY_RESIZE_BILINEAR, dst);
    mu_assert("resize zero std rejected", rc == -EINVAL);
    return NULL;
}

static char *test_resize_unknown_dtype_rejected_after_sampling(void)
{
    uint8_t src[4] = {0, 64, 128, 255};
    float dst[16] = {0};
    int rc = vmaf_tensor_from_luma_resize(src, 2u, 2, 2, 4, 4, VMAF_TENSOR_LAYOUT_NCHW,
                                          (VmafTensorDType)99, NULL, NULL,
                                          VMAF_TINY_RESIZE_BILINEAR, dst);
    mu_assert("resize unknown dtype rejected after dispatch", rc == -EINVAL);
    return NULL;
}

static char *test_resize_rejects_bad_args(void)
{
    uint8_t src[4] = {0};
    float dst[16] = {0};
    mu_assert("NULL src must -EINVAL",
              vmaf_tensor_from_luma_resize(NULL, 2u, 2, 2, 4, 4, VMAF_TENSOR_LAYOUT_NCHW,
                                           VMAF_TENSOR_DTYPE_F32, NULL, NULL,
                                           VMAF_TINY_RESIZE_BILINEAR, dst) == -EINVAL);
    mu_assert("dst_w<=0 must -EINVAL",
              vmaf_tensor_from_luma_resize(src, 2u, 2, 2, 0, 4, VMAF_TENSOR_LAYOUT_NCHW,
                                           VMAF_TENSOR_DTYPE_F32, NULL, NULL,
                                           VMAF_TINY_RESIZE_BILINEAR, dst) == -EINVAL);
    mu_assert("unknown mode must -EINVAL",
              vmaf_tensor_from_luma_resize(src, 2u, 2, 2, 4, 4, VMAF_TENSOR_LAYOUT_NCHW,
                                           VMAF_TENSOR_DTYPE_F32, NULL, NULL, (VmafTinyResize)99,
                                           dst) == -EINVAL);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_luma_to_f32_unnormalized);
    mu_run_test(test_f16_roundtrip);
    mu_run_test(test_luma_roundtrip);
    mu_run_test(test_rejects_bad_args);
    mu_run_test(test_rgb_imagenet_known_values);
    mu_run_test(test_rgb_imagenet_nchw_layout);
    mu_run_test(test_rgb_imagenet_rejects_bad_args);
    mu_run_test(test_f16_special_values);
    mu_run_test(test_f16_finite_overflow_to_inf);
    mu_run_test(test_f16_subnormal_range);
    mu_run_test(test_f16_to_f32_subnormal);
    mu_run_test(test_from_luma_zero_std_rejected);
    mu_run_test(test_from_luma_f16_path);
    mu_run_test(test_from_luma_invalid_dtype);
    mu_run_test(test_to_luma_rejects_bad_args);
    mu_run_test(test_to_luma_clamps_out_of_range);
    mu_run_test(test_to_luma_f16_path);
    mu_run_test(test_plane16_10bit_roundtrip);
    mu_run_test(test_plane16_rejects_bad_bpc);
    mu_run_test(test_plane16_12bit_clamps);
    mu_run_test(test_plane16_f16_roundtrip);
    mu_run_test(test_plane16_rejects_more_bad_args);
    /* ADR-0550 — auto-resize for NR tiny-model NCHW dispatch. */
    mu_run_test(test_resize_identity_matches_legacy);
    mu_run_test(test_resize_disabled_returns_einval);
    mu_run_test(test_resize_bilinear_2x_upsample);
    mu_run_test(test_resize_nearest_downsample);
    mu_run_test(test_resize_bicubic_and_f16_paths);
    mu_run_test(test_resize_zero_std_rejected);
    mu_run_test(test_resize_unknown_dtype_rejected_after_sampling);
    mu_run_test(test_resize_rejects_bad_args);
    return NULL;
}
