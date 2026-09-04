/**
 *
 *  Copyright 2016-2020 Netflix, Inc.
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

#include <assert.h>
#include <errno.h>
#include <float.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "cpu.h"
#include "log.h"
#include "mem.h"
#include "common/convolution.h"
#include "vif_options.h"
#include "vif_tools.h"

#define MIN(x, y) (((x) < (y)) ? (x) : (y))
#define MAX(x, y) (((x) > (y)) ? (x) : (y))

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#ifdef VIF_OPT_FAST_LOG2 // option to replace log2 calculation with faster speed

static const float log2_poly_s[9] = {-0.012671635276421, 0.064841182402670,  -0.157048836463065,
                                     0.257167726303123,  -0.353800560300520, 0.480131410397451,
                                     -0.721314327952201, 1.442694803896991,  0};

static float horner_s(const float *poly, float x, int n)
{
    float var = 0;
    int i;

    for (i = 0; i < n; ++i) {
        var = var * x + poly[i];
    }
    return var;
}

static float log2f_approx(float x)
{
    const uint32_t exp_zero_const = 0x3F800000UL;

    const uint32_t exp_expo_mask = 0x7F800000UL;
    const uint32_t exp_mant_mask = 0x007FFFFFUL;

    float remain;
    float log_base;
    float log_remain;
    uint32_t u32;
    uint32_t u32remain;
    uint32_t exponent;
    uint32_t mant;

    if (x == 0)
        return -INFINITY;
    if (x < 0)
        return NAN;

    memcpy(&u32, &x, sizeof(float));

    exponent = (u32 & exp_expo_mask) >> 23;
    mant = (u32 & exp_mant_mask) >> 0;
    u32remain = mant | exp_zero_const;

    memcpy(&remain, &u32remain, sizeof(float));

    log_base = (int32_t)exponent - 127;
    log_remain = horner_s(log2_poly_s, (remain - 1.0f), sizeof(log2_poly_s) / sizeof(float));

    return log_base + log_remain;
}

#define log2f log2f_approx

#endif /* VIF_FAST_LOG2 */

static int round_up_to_odd(float f)
{
    int ceiling = ceilf(f);
    if (ceiling % 2 == 0) {
        return ceiling + 1;
    } else {
        return ceiling;
    }
}

static float get_gaussian_pdf(float x, float mean, float stdev)
{
    float num = exp(-0.5 * (x - mean) / stdev * (x - mean) / stdev);
    float den = 1 / (stdev * sqrt(2 * M_PI));
    return num / den;
}

static void get_1d_gaussian_kernel(float *out, int size, float stdev)
{
    assert(size % 2 != 0);

    float sum = 0;
    int k = (size - 1) / 2;
    for (int i = 0; i < size; i++) {
        int curr = i - k;
        out[i] = get_gaussian_pdf(curr, 0, stdev);
        sum += out[i];
    }
    for (int i = 0; i < size; i++) {
        out[i] /= sum;
    }
}

bool vif_validate_kernelscale(float kernelscale)
{
    for (int i = 0; i < NUM_KERNELSCALES; i++) {
        if (fabsf(kernelscale - valid_kernelscales[i]) < 1e-3) {
            return true;
        }
    }
    return false;
}

int vif_get_filter_size(int scale, float kernelscale)
{
    assert(scale <= 4);

    int n = (1 << (4 - scale)) + 1;
    return MAX(round_up_to_odd(n * kernelscale), 3);
}

int vif_get_min_dim(float kernelscale)
{
    /* The scale ladder in compute_vif() halves the working dimension once per
     * scale (VIF_OPT_HANDLE_BORDERS keeps the full plane, so
     * `dim_s = dim_0 >> s`), then convolves at that size with the scale's own
     * Gaussian.  The reflect-101 mirror needs `dim_s >= filter_width_s/2 + 1`,
     * i.e. `dim_0 >= (filter_width_s/2 + 1) << s`; the binding scale is the
     * last one.  At the default kernelscale of 1.0 the widths are
     * {17, 9, 5, 3} and the bound is max(9, 10, 12, 16) = 16 — well above the
     * scale-0-only floor of 9 the guard used to carry, which is why 9..15px
     * input walked the mirror out of bounds (Netflix/vmaf#1582).
     */
    int min_dim = 1;
    for (int scale = 0; scale < 4; scale++) {
        const int filter_width = vif_get_filter_size(scale, kernelscale);
        const int need = (filter_width / 2 + 1) << scale;
        if (need > min_dim)
            min_dim = need;
    }
    return min_dim;
}

void vif_get_filter(float *out, int scale, float kernelscale)
{
    int window_size = vif_get_filter_size(scale, kernelscale);
    get_1d_gaussian_kernel(out, window_size, window_size / 5.0f);
}

void speed_get_antialias_filter(float *out, int scale, float kernelscale)
{
    // sigma_trick logic replication: antialias filter always has the size of scale 1 filter
    int window_size = vif_get_filter_size(1, kernelscale);
    get_1d_gaussian_kernel(out, window_size, sqrt(scale) * window_size / 5.0f);
}

void vif_dec2_s(const float *src, float *dst, int src_w, int src_h, int src_stride, int dst_stride)
{
    const int src_px_stride = src_stride / sizeof(float); // src_stride is in bytes
    const int dst_px_stride = dst_stride / sizeof(float);

    // decimation by 2 in each direction (after gaussian blur? check)
    for (int i = 0; i < src_h / 2; ++i) {
        for (int j = 0; j < src_w / 2; ++j) {
            dst[i * dst_px_stride + j] = src[(i * 2) * src_px_stride + (j * 2)];
        }
    }
}

void vif_dec16_s(const float *src, float *dst, int src_w, int src_h, int src_stride, int dst_stride)
{
    const int src_px_stride = src_stride / sizeof(float); // src_stride is in bytes
    const int dst_px_stride = dst_stride / sizeof(float);

    // decimation by 16 in each direction
    for (int i = 0; i < src_h / 16; ++i) {
        for (int j = 0; j < src_w / 16; ++j) {
            dst[i * dst_px_stride + j] = src[(i * 16) * src_px_stride + (j * 16)];
        }
    }
}

float vif_sum_s(const float *x, int w, int h, int stride)
{
    const int px_stride = stride / sizeof(float);

    float accum = 0;

    for (int i = 0; i < h; ++i) {
        float accum_inner = 0;

        for (int j = 0; j < w; ++j) {
            accum_inner += x[i * px_stride + j];
        } // having an inner accumulator help reduce numerical error (no accumulation of near-0 terms)

        accum += accum_inner;
    }

    return accum;
}

/* ==== vif_stat_mode = 'matching_c' (upstream reference, not shipped) ====
 *
 *  if (sigma1_sq < vif_sigma_nsq) {
 *      num_val = 1.0 - sigma2_sq * sigma_max_inv;
 *      den_val = 1.0;
 *  }
 *  else {
 *      num_log_num = (sigma2_sq + vif_sigma_nsq) * sigma1_sq;
 *      if (sigma12 < 0)
 *      {
 *          num_val = 0.0;
 *      }
 *      else
 *      {
 *          num_log_den = num_log_num - sigma12 * sigma12;
 *          num_val = log2f(num_log_num / num_log_den);
 *      }
 *      den_val = log2f(1.0f + sigma1_sq / vif_sigma_nsq);
 *  }
 *
 * The shipped path below is vif_stat_mode = 'matching_matlab'; the variable
 * names follow the Matlab code vifp_mscale.m. */
static void vif_pixel_statistic_s(float mu1_val, float mu2_val, float xx_filt_val,
                                  float yy_filt_val, float xy_filt_val, float sigma_max_inv,
                                  float vif_enhn_gain_limit_f, double vif_sigma_nsq, float *num_val,
                                  float *den_val)
{
    const float eps = 1.0e-10f;
    const float mu1_sq_val = mu1_val * mu1_val;
    const float mu2_sq_val = mu2_val * mu2_val;
    const float mu1_mu2_val = mu1_val * mu2_val;

    float sigma1_sq = xx_filt_val - mu1_sq_val;
    float sigma2_sq = yy_filt_val - mu2_sq_val;
    const float sigma12 = xy_filt_val - mu1_mu2_val;

    sigma1_sq = MAX(sigma1_sq, 0.0f);
    sigma2_sq = MAX(sigma2_sq, 0.0f);

    float g = sigma12 / (sigma1_sq + eps);
    float sv_sq = sigma2_sq - g * sigma12;

    if (sigma1_sq < eps) {
        g = 0.0f;
        sv_sq = sigma2_sq;
        sigma1_sq = 0.0f;
    }

    if (sigma2_sq < eps) {
        g = 0.0f;
        sv_sq = 0.0f;
    }

    if (g < 0.0f) {
        sv_sq = sigma2_sq;
        g = 0.0f;
    }
    sv_sq = MAX(sv_sq, eps);

    g = MIN(g, vif_enhn_gain_limit_f);

    *num_val = log2f(1.0f + (g * g * sigma1_sq) / (sv_sq + vif_sigma_nsq));
    *den_val = log2f(1.0f + (sigma1_sq) / (vif_sigma_nsq));

    if (sigma12 < 0.0f) {
        *num_val = 0.0f;
    }

    if (sigma1_sq < vif_sigma_nsq) {
        *num_val = 1.0f - sigma2_sq * sigma_max_inv;
        *den_val = 1.0f;
    }
}

void vif_statistic_s(const float *mu1, const float *mu2, const float *xx_filt, const float *yy_filt,
                     const float *xy_filt, float *num, float *den, int w, int h, int mu1_stride,
                     int mu2_stride, int xx_filt_stride, int yy_filt_stride, int xy_filt_stride,
                     double vif_enhn_gain_limit, double vif_sigma_nsq)
{
    const float sigma_max_inv = powf(vif_sigma_nsq, 2.0f) / (255.0 * 255.0);

    const int mu1_px_stride = mu1_stride / sizeof(float);
    const int mu2_px_stride = mu2_stride / sizeof(float);
    const int xx_filt_px_stride = xx_filt_stride / sizeof(float);
    const int yy_filt_px_stride = yy_filt_stride / sizeof(float);
    const int xy_filt_px_stride = xy_filt_stride / sizeof(float);

    const float vif_enhn_gain_limit_f = (float)vif_enhn_gain_limit;

    float accum_num = 0.0f;
    float accum_den = 0.0f;

    for (int i = 0; i < h; ++i) {
        float accum_inner_num = 0;
        float accum_inner_den = 0;
        for (int j = 0; j < w; ++j) {
            float num_val = 0.0f;
            float den_val = 0.0f;
            vif_pixel_statistic_s(mu1[i * mu1_px_stride + j], mu2[i * mu2_px_stride + j],
                                  xx_filt[i * xx_filt_px_stride + j],
                                  yy_filt[i * yy_filt_px_stride + j],
                                  xy_filt[i * xy_filt_px_stride + j], sigma_max_inv,
                                  vif_enhn_gain_limit_f, vif_sigma_nsq, &num_val, &den_val);

            accum_inner_num += num_val;
            accum_inner_den += den_val;
        }

        accum_num += accum_inner_num;
        accum_den += accum_inner_den;
    }
    num[0] = accum_num;
    den[0] = accum_den;
}

#if ARCH_X86
/* Float VIF path uses AVX2 only for golden-score parity with upstream
 * Netflix/vmaf (which has no AVX-512 float convolution).  The AVX-512
 * path was removed here because its wider FMA partial-sum tree produces
 * a different rounding than AVX2, causing the Netflix golden assertion
 * (76.66740433333332) to fail on AVX-512 CPUs at places=4 tolerance.
 * The AVX-512 integer-VIF path (vif_avx512.c) is unaffected — that path
 * is covered by ADR-0214's explicitly accepted integer-path ULP divergence.
 * See ADR-0504 (acceptance criterion was wrong: claim "golden assertions
 * still pass at places=4" is false on AVX-512 hardware). */
static bool vif_use_avx2_convolution(int fwidth)
{
    const unsigned flags = vmaf_get_cpu_flags();
    return (flags & VMAF_X86_CPU_FLAG_AVX2) && fwidth <= MAX_FWIDTH_AVX_CONV;
}
#endif

/* Reflect-101 mirror of index idx into [0, n). */
static int vif_mirror_index(int idx, int n)
{
    return idx < 0 ? -idx : (idx >= n ? 2 * n - idx - 2 : idx);
}

/* Vertical pass of the separable filter for source row i into tmp[0..w). */
static void vif_filter1d_vertical_s(const float *f, int fwidth, const float *src, int src_px_stride,
                                    int w, int h, int i, float *tmp)
{
    for (int j = 0; j < w; ++j) {
        float accum = 0;

        for (int fi = 0; fi < fwidth; ++fi) {
            const float fcoeff = f[fi];
            const int ii = vif_mirror_index(i - fwidth / 2 + fi, h);
            const float imgcoeff = src[ii * src_px_stride + j];

            accum += fcoeff * imgcoeff;
        }

        tmp[j] = accum;
    }
}

/* Vertical pass over the squared source (src * src) for row i into tmp[0..w). */
static void vif_filter1d_vertical_sq_s(const float *f, int fwidth, const float *src,
                                       int src_px_stride, int w, int h, int i, float *tmp)
{
    for (int j = 0; j < w; ++j) {
        float accum = 0;

        for (int fi = 0; fi < fwidth; ++fi) {
            const float fcoeff = f[fi];
            const int ii = vif_mirror_index(i - fwidth / 2 + fi, h);
            const float imgcoeff = src[ii * src_px_stride + j];

            accum += fcoeff * (imgcoeff * imgcoeff);
        }

        tmp[j] = accum;
    }
}

/* Vertical pass over the cross product (src1 * src2) for row i into tmp[0..w). */
static void vif_filter1d_vertical_xy_s(const float *f, int fwidth, const float *src1,
                                       const float *src2, int src1_px_stride, int src2_px_stride,
                                       int w, int h, int i, float *tmp)
{
    for (int j = 0; j < w; ++j) {
        float accum = 0;

        for (int fi = 0; fi < fwidth; ++fi) {
            const float fcoeff = f[fi];
            const int ii = vif_mirror_index(i - fwidth / 2 + fi, h);
            const float imgcoeff1 = src1[ii * src1_px_stride + j];
            const float imgcoeff2 = src2[ii * src2_px_stride + j];

            accum += fcoeff * (imgcoeff1 * imgcoeff2);
        }

        tmp[j] = accum;
    }
}

/* Horizontal pass of the separable filter from tmp[0..w) into one dst row. */
static void vif_filter1d_horizontal_s(const float *f, int fwidth, const float *tmp, int w,
                                      float *dst_row)
{
    for (int j = 0; j < w; ++j) {
        float accum = 0;

        for (int fj = 0; fj < fwidth; ++fj) {
            const float fcoeff = f[fj];
            const int jj = vif_mirror_index(j - fwidth / 2 + fj, w);
            const float imgcoeff = tmp[jj];

            accum += fcoeff * imgcoeff;
        }

        dst_row[j] = accum;
    }
}

void vif_filter1d_s(const float *f, const float *src, float *dst, float *tmpbuf, int w, int h,
                    int src_stride, int dst_stride, int fwidth)
{
    const int src_px_stride = src_stride / sizeof(float);
    const int dst_px_stride = dst_stride / sizeof(float);

#if ARCH_X86
    if (vif_use_avx2_convolution(fwidth)) {
        convolution_f32_avx_s(f, fwidth, src, dst, tmpbuf, w, h, src_px_stride, dst_px_stride);
        return;
    }
#else
    (void)tmpbuf;
#endif

    /* fall back */

    float *tmp = aligned_malloc(ALIGN_CEIL(w * sizeof(float)), MAX_ALIGN);
    if (!tmp) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "vif_filter1d_s: out of memory (w=%d)\n", w);
        return;
    }

    for (int i = 0; i < h; ++i) {
        vif_filter1d_vertical_s(f, fwidth, src, src_px_stride, w, h, i, tmp);
        vif_filter1d_horizontal_s(f, fwidth, tmp, w, dst + (ptrdiff_t)i * dst_px_stride);
    }

    aligned_free(tmp);
}

// Code optimized by adding intrinsic code for the functions,
// vif_filter1d_sq and vif_filter1d_sq

void vif_filter1d_sq_s(const float *f, const float *src, float *dst, float *tmpbuf, int w, int h,
                       int src_stride, int dst_stride, int fwidth)
{
    const int src_px_stride = src_stride / sizeof(float);
    const int dst_px_stride = dst_stride / sizeof(float);

#if ARCH_X86
    if (vif_use_avx2_convolution(fwidth)) {
        convolution_f32_avx_sq_s(f, fwidth, src, dst, tmpbuf, w, h, src_px_stride, dst_px_stride);
        return;
    }
#else
    (void)tmpbuf;
#endif

    /* fall back */

    float *tmp = aligned_malloc(ALIGN_CEIL(w * sizeof(float)), MAX_ALIGN);
    if (!tmp) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "vif_filter1d_sq_s: out of memory (w=%d)\n", w);
        return;
    }

    for (int i = 0; i < h; ++i) {
        vif_filter1d_vertical_sq_s(f, fwidth, src, src_px_stride, w, h, i, tmp);
        vif_filter1d_horizontal_s(f, fwidth, tmp, w, dst + (ptrdiff_t)i * dst_px_stride);
    }

    aligned_free(tmp);
}

void vif_filter1d_xy_s(const float *f, const float *src1, const float *src2, float *dst,
                       float *tmpbuf, int w, int h, int src1_stride, int src2_stride,
                       int dst_stride, int fwidth)
{
    const int src1_px_stride = src1_stride / sizeof(float);
    const int src2_px_stride = src2_stride / sizeof(float);
    const int dst_px_stride = dst_stride / sizeof(float);

#if ARCH_X86
    if (vif_use_avx2_convolution(fwidth)) {
        convolution_f32_avx_xy_s(f, fwidth, src1, src2, dst, tmpbuf, w, h, src1_px_stride,
                                 src2_px_stride, dst_px_stride);
        return;
    }
#else
    (void)tmpbuf;
#endif

    /* fall back */

    float *tmp = aligned_malloc(ALIGN_CEIL(w * sizeof(float)), MAX_ALIGN);
    if (!tmp) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "vif_filter1d_xy_s: out of memory (w=%d)\n", w);
        return;
    }

    for (int i = 0; i < h; ++i) {
        vif_filter1d_vertical_xy_s(f, fwidth, src1, src2, src1_px_stride, src2_px_stride, w, h, i,
                                   tmp);
        vif_filter1d_horizontal_s(f, fwidth, tmp, w, dst + (ptrdiff_t)i * dst_px_stride);
    }

    aligned_free(tmp);
}

int vif_get_scaling_method(char *scaling_method_str, enum vif_scaling_method *scale_method)
{
    if (!strcmp(scaling_method_str, "nearest")) {
        *scale_method = vif_scale_nearest;
    } else if (!strcmp(scaling_method_str, "bilinear")) {
        *scale_method = vif_scale_bilinear;
    } else if (!strcmp(scaling_method_str, "bicubic")) {
        *scale_method = vif_scale_bicubic;
    } else if (!strcmp(scaling_method_str, "lanczos4")) {
        *scale_method = vif_scale_lanczos4;
    } else {
        vmaf_log(
            VMAF_LOG_LEVEL_ERROR,
            "Invalid scale method %s. Supported scale methods: [nearest, bilinear, bicubic, lanczos4]\n",
            scaling_method_str);
        return -EINVAL;
    }

    return 0;
}

static float bicubic_kernel(float t)
{
    float a = -0.75;
    if (t < 0) {
        t = -t;
    }
    if (t < 1) {
        return ((a + 2) * t - (a + 3)) * t * t + 1;
    }
    if (t < 2) {
        return (((t - 5) * t + 8) * t - 4) * a;
    }
    return 0;
}

static float mirror(float i, float left, float right)
{
    return (i < left ? -i : i > right ? 2 * right - i : i);
}

static float bicubic_interpolation(const float *src, int width, int height, int src_stride, float x,
                                   float y)
{
    int x0 = floorf(x);
    int y0 = floorf(y);

    float dx = x - x0;
    float dy = y - y0;

    float weights_x[4];
    float weights_y[4];

    for (int i = -1; i <= 2; i++) {
        weights_x[i + 1] = bicubic_kernel(i - dx);
        weights_y[i + 1] = bicubic_kernel(i - dy);
    }

    float interp_val = 0.0;
    for (int j = -1; j <= 2; j++) {
        for (int i = -1; i <= 2; i++) {
            int x_index = (int)mirror(x0 + i, 0, width - 1);
            int y_index = (int)mirror(y0 + j, 0, height - 1);
            float weight = weights_x[i + 1] * weights_y[j + 1];
            interp_val += src[y_index * src_stride + x_index] * weight;
        }
    }

    return interp_val;
}

static void vif_scale_frame_bicubic_s(const float *src, float *dst, int src_w, int src_h,
                                      int src_stride, int dst_w, int dst_h, int dst_stride)
{
    // if the input and output sizes are the same
    if (src_w == dst_w && src_h == dst_h) {
        memcpy(dst, src, (size_t)dst_stride * (size_t)dst_h * sizeof(float));
        return;
    }

    float ratio_x = (float)src_w / dst_w;
    float ratio_y = (float)src_h / dst_h;

    for (int y = 0; y < dst_h; y++) {
        float yy = (y + 0.5) * ratio_y - 0.5;
        for (int x = 0; x < dst_w; x++) {
            float xx = (x + 0.5) * ratio_x - 0.5;
            dst[y * dst_stride + x] = bicubic_interpolation(src, src_w, src_h, src_stride, xx, yy);
        }
    }
}

static float lanczos4_kernel(float x, float a)
{
    if (x == 0.0)
        return 1.0;
    if (x > -a && x < a) {
        return a * sin(M_PI * x) * sin(M_PI * x / a) / (M_PI * M_PI * x * x);
    }
    return 0.0;
}

static float lanczos4_interpolation(const float *src, int width, int height, int src_stride,
                                    float x, float y)
{
    int a = 4;
    int x0 = floorf(x);
    int y0 = floorf(y);

    float dx = x - x0;
    float dy = y - y0;

    float value = 0.0;
    float weight_sum = 0.0;

    float weights_x[9];
    float weights_y[9];
    for (int i = -a; i <= a; i++) {
        weights_x[i + a] = lanczos4_kernel(i - dx, (float)a);
        weights_y[i + a] = lanczos4_kernel(i - dy, (float)a);
    }

    for (int iy = -a; iy <= a; iy++) {
        for (int ix = -a; ix <= a; ix++) {
            float weight = weights_x[ix + a] * weights_y[iy + a];
            weight_sum += weight;

            int x_index = (int)mirror(x0 + ix, 0, width - 1);
            int y_index = (int)mirror(y0 + iy, 0, height - 1);
            value += src[y_index * src_stride + x_index] * weight;
        }
    }

    return value / weight_sum;
}

static void vif_scale_frame_lanczos4_s(const float *src, float *dst, int src_w, int src_h,
                                       int src_stride, int dst_w, int dst_h, int dst_stride)
{
    // if the input and output sizes are the same
    if (src_w == dst_w && src_h == dst_h) {
        memcpy(dst, src, (size_t)dst_stride * (size_t)dst_h * sizeof(float));
        return;
    }

    float ratio_x = (float)src_w / dst_w;
    float ratio_y = (float)src_h / dst_h;

    for (int y = 0; y < dst_h; y++) {
        float yy = (y + 0.5) * ratio_y - 0.5;
        for (int x = 0; x < dst_w; x++) {
            float xx = (x + 0.5) * ratio_x - 0.5;
            dst[y * dst_stride + x] = lanczos4_interpolation(src, src_w, src_h, src_stride, xx, yy);
        }
    }
}

static float bilinear_interpolation(const float *src, int width, int height, int src_stride,
                                    float x, float y)
{
    int x1 = (int)mirror(floorf(x), 0, (float)(width - 1));
    int x2 = (int)mirror(ceilf(x), 0, (float)(width - 1));
    int y1 = (int)mirror(floorf(y), 0, (float)(height - 1));
    int y2 = (int)mirror(ceilf(y), 0, (float)(height - 1));

    float dx = x - x1;
    float dy = y - y1;

    return ((1 - dy) * (1 - dx) * src[y1 * src_stride + x1] +
            (1 - dy) * dx * src[y1 * src_stride + x2] + dy * (1 - dx) * src[y2 * src_stride + x1] +
            dy * dx * src[y2 * src_stride + x2]);
}

static void vif_scale_frame_bilinear_s(const float *src, float *dst, int src_w, int src_h,
                                       int src_stride, int dst_w, int dst_h, int dst_stride)
{
    // if the input and output sizes are the same
    if (src_w == dst_w && src_h == dst_h) {
        memcpy(dst, src, (size_t)dst_stride * (size_t)dst_h * sizeof(float));
        return;
    }

    float ratio_x = (float)src_w / dst_w;
    float ratio_y = (float)src_h / dst_h;

    for (int y = 0; y < dst_h; y++) {
        float yy = (y + 0.5) * ratio_y - 0.5;
        for (int x = 0; x < dst_w; x++) {
            float xx = (x + 0.5) * ratio_x - 0.5;
            dst[y * dst_stride + x] = bilinear_interpolation(src, src_w, src_h, src_stride, xx, yy);
        }
    }
}

static void vif_scale_frame_nearest_s(const float *src, float *dst, int src_w, int src_h,
                                      int src_stride, int dst_w, int dst_h, int dst_stride)
{
    // if the input and output sizes are the same
    if (src_w == dst_w && src_h == dst_h) {
        memcpy(dst, src, (size_t)dst_stride * (size_t)dst_h * sizeof(float));
        return;
    }

    float ratio_x = (float)src_w / dst_w;
    float ratio_y = (float)src_h / dst_h;

    for (int y = 0; y < dst_h; y++) {
        for (int x = 0; x < dst_w; x++) {
            int rounded_y = (int)(y * ratio_y);
            int rounded_x = (int)(x * ratio_x);
            dst[y * dst_stride + x] = src[rounded_y * src_stride + rounded_x];
        }
    }
}

void vif_scale_frame_s(enum vif_scaling_method scale_method, const float *src, float *dst,
                       int src_w, int src_h, int src_stride, int dst_w, int dst_h, int dst_stride)
{
    if (scale_method == vif_scale_nearest) {
        vif_scale_frame_nearest_s(src, dst, src_w, src_h, src_stride, dst_w, dst_h, dst_stride);
    } else if (scale_method == vif_scale_bilinear) {
        vif_scale_frame_bilinear_s(src, dst, src_w, src_h, src_stride, dst_w, dst_h, dst_stride);
    } else if (scale_method == vif_scale_bicubic) {
        vif_scale_frame_bicubic_s(src, dst, src_w, src_h, src_stride, dst_w, dst_h, dst_stride);
    } else if (scale_method == vif_scale_lanczos4) {
        vif_scale_frame_lanczos4_s(src, dst, src_w, src_h, src_stride, dst_w, dst_h, dst_stride);
    }
}
