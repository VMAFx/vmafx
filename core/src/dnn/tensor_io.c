/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 */

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "tensor_io.h"

/* IEEE-754 binary16 <-> binary32 conversion without hw intrinsics. Round-to-
 * nearest-even on the down-convert; exact on the up-convert. */

static uint16_t f32_to_f16_one(float f)
{
    uint32_t x;
    memcpy(&x, &f, sizeof(x));
    uint32_t sign = (x >> 31) & 0x1u;
    int32_t exp = (int32_t)((x >> 23) & 0xffu) - 127 + 15;
    uint32_t mant = x & 0x7fffffu;

    if (exp >= 31) {
        /* The f32 biased exponent 0xff marks inf/NaN; any other exponent
         * here is a large *finite* float that overflows the f16 range.
         * Only propagate a NaN mantissa for a true f32 NaN — overflowing
         * finite values must map to a clean ±inf, not NaN. Mirrors the
         * exp_f==128 (inf/nan) vs exp_f>15 (overflow) split in
         * ort_backend.c:fp32_to_fp16. */
        const bool input_is_nan = (((x >> 23) & 0xffu) == 0xffu) && (mant != 0u);
        uint16_t nan_mant = input_is_nan ? 0x200u : 0u;
        return (uint16_t)((sign << 15) | (0x1fu << 10) | nan_mant);
    }
    if (exp <= 0) {
        if (exp < -10) {
            return (uint16_t)(sign << 15);
        }
        mant = (mant | 0x800000u) >> (1 - exp);
        uint32_t round = (mant & 0x1000u) != 0u ? 1u : 0u;
        return (uint16_t)((sign << 15) | ((mant >> 13) + round));
    }
    uint32_t round = (mant & 0x1000u) != 0u ? 1u : 0u;
    uint32_t h = (sign << 15) | ((uint32_t)exp << 10) | (mant >> 13);
    return (uint16_t)(h + round);
}

static float f16_to_f32_one(uint16_t h)
{
    uint32_t sign = (h >> 15) & 0x1u;
    uint32_t exp = (h >> 10) & 0x1fu;
    uint32_t mant = h & 0x3ffu;
    uint32_t out;
    if (exp == 0u) {
        if (mant == 0u) {
            out = sign << 31;
        } else {
            /* Normalise the subnormal mantissa: shift left until the hidden
             * bit (bit 10) appears, tracking the exponent adjustment with a
             * signed counter to avoid unsigned overflow that trips
             * -fsanitize=integer.  f16 mantissa is 10 bits wide, so at most
             * 10 shifts are needed; exp_adj is bounded to [-9, 1]. */
            int32_t exp_adj = 1;
            while ((mant & 0x400u) == 0u) {
                mant <<= 1;
                exp_adj--;
            }
            mant &= 0x3ffu;
            out = (sign << 31) | (((uint32_t)(exp_adj + 112)) << 23) | (mant << 13);
        }
    } else if (exp == 0x1fu) {
        out = (sign << 31) | 0x7f800000u | (mant << 13);
    } else {
        out = (sign << 31) | ((exp + 112u) << 23) | (mant << 13);
    }
    float f;
    memcpy(&f, &out, sizeof(f));
    return f;
}

void vmaf_f32_to_f16(const float *src, uint16_t *dst, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        dst[i] = f32_to_f16_one(src[i]);
    }
}

void vmaf_f16_to_f32(const uint16_t *src, float *dst, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        dst[i] = f16_to_f32_one(src[i]);
    }
}

int vmaf_tensor_from_luma(const uint8_t *src, size_t stride_src, int width, int height,
                          VmafTensorLayout layout, VmafTensorDType dtype, const float *mean,
                          const float *std, void *dst)
{
    if (!src || !dst || width <= 0 || height <= 0 || stride_src < (size_t)width) {
        return -EINVAL;
    }
    /* Luma is single-channel; NCHW vs NHWC layout is equivalent for C=1. */
    (void)layout;

    const float m = mean ? mean[0] : 0.0f;
    const float s = std ? std[0] : 1.0f;
    if (s == 0.0f) {
        return -EINVAL;
    }
    const float inv_s = 1.0f / s;

    const size_t n = (size_t)width * (size_t)height;
    if (dtype == VMAF_TENSOR_DTYPE_F32) {
        float *out = (float *)dst;
        for (int y = 0; y < height; ++y) {
            const uint8_t *row = src + (size_t)y * stride_src;
            for (int x = 0; x < width; ++x) {
                out[(size_t)y * (size_t)width + (size_t)x] =
                    (((float)row[x] * (1.0f / 255.0f)) - m) * inv_s;
            }
        }
    } else if (dtype == VMAF_TENSOR_DTYPE_F16) {
        uint16_t *out = (uint16_t *)dst;
        for (int y = 0; y < height; ++y) {
            const uint8_t *row = src + (size_t)y * stride_src;
            for (int x = 0; x < width; ++x) {
                float v = (((float)row[x] * (1.0f / 255.0f)) - m) * inv_s;
                out[(size_t)y * (size_t)width + (size_t)x] = f32_to_f16_one(v);
            }
        }
    } else {
        return -EINVAL;
    }
    (void)n;
    return 0;
}

/* --- ADR-0550 — auto-resize for NR tiny-model NCHW dispatch ----------
 *
 * Three deterministic filters, all separable, half-pixel-centre coord
 * convention. The choice intentionally mirrors OpenCV `INTER_*` and the
 * torchvision `Resize(..., antialias=False)` default, so a model
 * trained against either pipeline sees the same input statistics at
 * inference time. See [docs/ai/inference.md] §Auto-resize for the
 * external contract.
 */

static inline int clamp_int(int v, int lo, int hi)
{
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

static float sample_luma_nearest(const uint8_t *src, size_t stride_src, int src_w, int src_h,
                                 float sx, float sy)
{
    /* Floor matches OpenCV `INTER_NEAREST` (note: cv2 uses floor, not
     * round, despite the docs sometimes implying otherwise). */
    int ix = (int)floorf(sx);
    int iy = (int)floorf(sy);
    ix = clamp_int(ix, 0, src_w - 1);
    iy = clamp_int(iy, 0, src_h - 1);
    return (float)src[(size_t)iy * stride_src + (size_t)ix];
}

static float sample_luma_bilinear(const uint8_t *src, size_t stride_src, int src_w, int src_h,
                                  float sx, float sy)
{
    int x0 = (int)floorf(sx);
    int y0 = (int)floorf(sy);
    float fx = sx - (float)x0;
    float fy = sy - (float)y0;
    int x1 = x0 + 1;
    int y1 = y0 + 1;
    x0 = clamp_int(x0, 0, src_w - 1);
    x1 = clamp_int(x1, 0, src_w - 1);
    y0 = clamp_int(y0, 0, src_h - 1);
    y1 = clamp_int(y1, 0, src_h - 1);
    float p00 = (float)src[(size_t)y0 * stride_src + (size_t)x0];
    float p01 = (float)src[(size_t)y0 * stride_src + (size_t)x1];
    float p10 = (float)src[(size_t)y1 * stride_src + (size_t)x0];
    float p11 = (float)src[(size_t)y1 * stride_src + (size_t)x1];
    float top = p00 + (p01 - p00) * fx;
    float bot = p10 + (p11 - p10) * fx;
    return top + (bot - top) * fy;
}

/* Catmull-Rom cubic with a = -0.5 (the OpenCV default). */
static inline float cubic_weight(float t)
{
    const float a = -0.5f;
    float at = t < 0.f ? -t : t;
    float at2 = at * at;
    float at3 = at2 * at;
    if (at <= 1.f) {
        return (a + 2.f) * at3 - (a + 3.f) * at2 + 1.f;
    }
    if (at < 2.f) {
        return a * at3 - 5.f * a * at2 + 8.f * a * at - 4.f * a;
    }
    return 0.f;
}

static float sample_luma_bicubic(const uint8_t *src, size_t stride_src, int src_w, int src_h,
                                 float sx, float sy)
{
    int x0 = (int)floorf(sx);
    int y0 = (int)floorf(sy);
    float fx = sx - (float)x0;
    float fy = sy - (float)y0;
    float wx[4];
    float wy[4];
    wx[0] = cubic_weight(1.f + fx);
    wx[1] = cubic_weight(0.f + fx);
    wx[2] = cubic_weight(1.f - fx);
    wx[3] = cubic_weight(2.f - fx);
    wy[0] = cubic_weight(1.f + fy);
    wy[1] = cubic_weight(0.f + fy);
    wy[2] = cubic_weight(1.f - fy);
    wy[3] = cubic_weight(2.f - fy);
    float acc = 0.f;
    for (int j = 0; j < 4; ++j) {
        int yy = clamp_int(y0 - 1 + j, 0, src_h - 1);
        const uint8_t *row = src + (size_t)yy * stride_src;
        float row_acc = 0.f;
        for (int i = 0; i < 4; ++i) {
            int xx = clamp_int(x0 - 1 + i, 0, src_w - 1);
            row_acc += wx[i] * (float)row[xx];
        }
        acc += wy[j] * row_acc;
    }
    /* The cubic kernel can ring; clamp to [0, 255] before normalising so
     * the downstream `(v / 255 - mean) / std` lives in the same domain
     * the model saw during training. */
    if (acc < 0.f)
        acc = 0.f;
    if (acc > 255.f)
        acc = 255.f;
    return acc;
}

/* Dispatch helper — picks the per-pixel sampler based on the requested
 * filter. */
static inline float sample_luma_dispatch(const uint8_t *src, size_t stride_src, int src_w,
                                         int src_h, float sx, float sy, VmafTinyResize mode)
{
    switch (mode) {
    case VMAF_TINY_RESIZE_NEAREST:
        return sample_luma_nearest(src, stride_src, src_w, src_h, sx, sy);
    case VMAF_TINY_RESIZE_BICUBIC:
        return sample_luma_bicubic(src, stride_src, src_w, src_h, sx, sy);
    case VMAF_TINY_RESIZE_BILINEAR:
    default:
        return sample_luma_bilinear(src, stride_src, src_w, src_h, sx, sy);
    }
}

/* Per-pixel write — collapses the (dtype, dst) branch out of the
 * resize hot loop. Returns 0 on success, -EINVAL on unsupported dtype. */
static inline int store_normalised(void *dst, size_t off, float p, float m, float inv_s,
                                   VmafTensorDType dtype)
{
    const float v = ((p * (1.0f / 255.0f)) - m) * inv_s;
    if (dtype == VMAF_TENSOR_DTYPE_F32) {
        ((float *)dst)[off] = v;
        return 0;
    }
    if (dtype == VMAF_TENSOR_DTYPE_F16) {
        ((uint16_t *)dst)[off] = f32_to_f16_one(v);
        return 0;
    }
    return -EINVAL;
}

int vmaf_tensor_from_luma_resize(const uint8_t *src, size_t stride_src, int src_w, int src_h,
                                 int dst_w, int dst_h, VmafTensorLayout layout,
                                 VmafTensorDType dtype, const float *mean, const float *std,
                                 VmafTinyResize mode, void *dst)
{
    if (!src || !dst || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0 ||
        stride_src < (size_t)src_w) {
        return -EINVAL;
    }
    /* The disabled mode is consumed at the call site (libvmaf.c routes
     * it to -ERANGE). Reaching here means a programming error — bail
     * rather than silently fall back to a default filter. */
    if (mode == VMAF_TINY_RESIZE_DISABLED) {
        return -EINVAL;
    }
    if (mode != VMAF_TINY_RESIZE_NEAREST && mode != VMAF_TINY_RESIZE_BILINEAR &&
        mode != VMAF_TINY_RESIZE_BICUBIC) {
        return -EINVAL;
    }
    (void)layout; /* C=1 → NCHW and NHWC coincide. */

    /* Bit-identical fast path when no scaling is needed. */
    if (src_w == dst_w && src_h == dst_h) {
        return vmaf_tensor_from_luma(src, stride_src, dst_w, dst_h, layout, dtype, mean, std, dst);
    }

    const float m = mean ? mean[0] : 0.0f;
    const float s = std ? std[0] : 1.0f;
    if (s == 0.0f) {
        return -EINVAL;
    }
    const float inv_s = 1.0f / s;
    const float scale_x = (float)src_w / (float)dst_w;
    const float scale_y = (float)src_h / (float)dst_h;

    for (int dy = 0; dy < dst_h; ++dy) {
        const float sy = ((float)dy + 0.5f) * scale_y - 0.5f;
        for (int dx = 0; dx < dst_w; ++dx) {
            const float sx = ((float)dx + 0.5f) * scale_x - 0.5f;
            const float p = sample_luma_dispatch(src, stride_src, src_w, src_h, sx, sy, mode);
            const size_t off = (size_t)dy * (size_t)dst_w + (size_t)dx;
            const int rc = store_normalised(dst, off, p, m, inv_s, dtype);
            if (rc < 0)
                return rc;
        }
    }
    return 0;
}

/* ImageNet / torchvision normalization constants. Keep in sync with the
 * export pipeline in ai/ — any divergence silently biases every learned
 * RGB model (LPIPS, MobileSal, TransNet-V2). */
static const float IMAGENET_MEAN[3] = {0.485f, 0.456f, 0.406f};
static const float IMAGENET_STD[3] = {0.229f, 0.224f, 0.225f};

int vmaf_tensor_from_rgb_imagenet(const uint8_t *src_r, size_t stride_r, const uint8_t *src_g,
                                  size_t stride_g, const uint8_t *src_b, size_t stride_b, int width,
                                  int height, float *dst)
{
    if (!src_r || !src_g || !src_b || !dst || width <= 0 || height <= 0)
        return -EINVAL;
    if (stride_r < (size_t)width || stride_g < (size_t)width || stride_b < (size_t)width)
        return -EINVAL;

    const size_t plane = (size_t)width * (size_t)height;
    const uint8_t *const srcs[3] = {src_r, src_g, src_b};
    const size_t strides[3] = {stride_r, stride_g, stride_b};

    for (int c = 0; c < 3; ++c) {
        const float m = IMAGENET_MEAN[c];
        const float inv_s = 1.0f / IMAGENET_STD[c];
        float *out_c = dst + (size_t)c * plane;
        for (int y = 0; y < height; ++y) {
            const uint8_t *row = srcs[c] + (size_t)y * strides[c];
            float *out_row = out_c + (size_t)y * (size_t)width;
            for (int x = 0; x < width; ++x) {
                out_row[x] = (((float)row[x] * (1.0f / 255.0f)) - m) * inv_s;
            }
        }
    }
    return 0;
}

int vmaf_tensor_from_plane16(const uint16_t *src, size_t stride_src, int width, int height, int bpc,
                             VmafTensorLayout layout, VmafTensorDType dtype, const float *mean,
                             const float *std, void *dst)
{
    if (!src || !dst || width <= 0 || height <= 0 || bpc < DNN_MIN_BIT_DEPTH || bpc > 16 ||
        stride_src < (size_t)width * sizeof(uint16_t)) {
        return -EINVAL;
    }
    (void)layout; /* C=1 → NCHW and NHWC coincide. */

    const float m = mean ? mean[0] : 0.0f;
    const float s = std ? std[0] : 1.0f;
    if (s == 0.0f) {
        return -EINVAL;
    }
    const float inv_s = 1.0f / s;
    const float inv_max = 1.0f / (float)((1u << bpc) - 1u);

    if (dtype == VMAF_TENSOR_DTYPE_F32) {
        float *out = (float *)dst;
        for (int y = 0; y < height; ++y) {
            const uint16_t *row = (const uint16_t *)((const uint8_t *)src + (size_t)y * stride_src);
            for (int x = 0; x < width; ++x) {
                out[(size_t)y * (size_t)width + (size_t)x] =
                    (((float)row[x] * inv_max) - m) * inv_s;
            }
        }
    } else if (dtype == VMAF_TENSOR_DTYPE_F16) {
        uint16_t *out = (uint16_t *)dst;
        for (int y = 0; y < height; ++y) {
            const uint16_t *row = (const uint16_t *)((const uint8_t *)src + (size_t)y * stride_src);
            for (int x = 0; x < width; ++x) {
                float v = (((float)row[x] * inv_max) - m) * inv_s;
                out[(size_t)y * (size_t)width + (size_t)x] = f32_to_f16_one(v);
            }
        }
    } else {
        return -EINVAL;
    }
    return 0;
}

int vmaf_tensor_to_plane16(const void *src, VmafTensorLayout layout, VmafTensorDType dtype,
                           int width, int height, int bpc, const float *mean, const float *std,
                           uint16_t *dst, size_t stride_dst)
{
    if (!src || !dst || width <= 0 || height <= 0 || bpc < DNN_MIN_BIT_DEPTH || bpc > 16 ||
        stride_dst < (size_t)width * sizeof(uint16_t)) {
        return -EINVAL;
    }
    (void)layout;

    const float m = mean ? mean[0] : 0.0f;
    const float s = std ? std[0] : 1.0f;
    const float max_val = (float)((1u << bpc) - 1u);
    const int max_int = (int)((1u << bpc) - 1u);

    for (int y = 0; y < height; ++y) {
        uint16_t *row = (uint16_t *)((uint8_t *)dst + (size_t)y * stride_dst);
        for (int x = 0; x < width; ++x) {
            float v;
            if (dtype == VMAF_TENSOR_DTYPE_F32) {
                v = ((const float *)src)[(size_t)y * (size_t)width + (size_t)x];
            } else if (dtype == VMAF_TENSOR_DTYPE_F16) {
                v = f16_to_f32_one(((const uint16_t *)src)[(size_t)y * (size_t)width + (size_t)x]);
            } else {
                return -EINVAL;
            }
            float denorm = ((v * s) + m) * max_val;
            float rounded = nearbyintf(denorm);
            int ri = (int)rounded;
            if (ri < 0)
                ri = 0;
            if (ri > max_int)
                ri = max_int;
            row[x] = (uint16_t)ri;
        }
    }
    return 0;
}

int vmaf_tensor_to_luma(const void *src, VmafTensorLayout layout, VmafTensorDType dtype, int width,
                        int height, const float *mean, const float *std, uint8_t *dst,
                        size_t stride_dst)
{
    if (!src || !dst || width <= 0 || height <= 0 || stride_dst < (size_t)width) {
        return -EINVAL;
    }
    (void)layout;

    const float m = mean ? mean[0] : 0.0f;
    const float s = std ? std[0] : 1.0f;

    for (int y = 0; y < height; ++y) {
        uint8_t *row = dst + (size_t)y * stride_dst;
        for (int x = 0; x < width; ++x) {
            float v;
            if (dtype == VMAF_TENSOR_DTYPE_F32) {
                v = ((const float *)src)[(size_t)y * (size_t)width + (size_t)x];
            } else if (dtype == VMAF_TENSOR_DTYPE_F16) {
                v = f16_to_f32_one(((const uint16_t *)src)[(size_t)y * (size_t)width + (size_t)x]);
            } else {
                return -EINVAL;
            }
            float denorm = ((v * s) + m) * 255.0f;
            float rounded = nearbyintf(denorm);
            int ri = (int)rounded;
            if (ri < 0)
                ri = 0;
            if (ri > 255)
                ri = 255;
            row[x] = (uint8_t)ri;
        }
    }
    return 0;
}
