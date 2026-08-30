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
 * Bit-exactness contract test for the SSIMULACRA 2 SIMD kernels.
 *
 * Each vectorised kernel must produce byte-for-byte identical output
 * to its scalar counterpart on reproducible pseudo-random inputs
 * (ADR-0161). Scalar references are duplicated in-file because the
 * extractor keeps them `static` — the test TU is the audit surface
 * for the bit-exact contract.
 */

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "test.h"

#include "feature/ssimulacra2_math.h"
#include "feature/ssimulacra2_simd_common.h"

/* Suppress FP contraction in the scalar reference functions below. The
 * AVX2 / AVX-512 SIMD kernels under test use explicit `_mm*_mul_ps` +
 * `_mm*_add_ps` intrinsics (no `_mm*_fmadd_ps`), so the scalar
 * reference must produce identical mul-then-add sequences for the
 * memcmp() bit-exactness assertion to hold. GCC defaults to
 * `-ffp-contract=off` and the per-TU `_simd_strict_fp_args` flag
 * reinforces that, but icx 2025.3 / 2026.0 IGNORE both `-fp-model=precise`
 * and `#pragma STDC FP_CONTRACT OFF` for inline scalar code — they
 * still contract `a*b + c` to `vfmadd231ps` and diverge from the SIMD
 * by ~1 ULP per lane. icx (being clang-based) DOES honour
 * `#pragma clang fp contract(off)`, which empirically suppresses the
 * vfmadd emission (verified by disassembling the resulting object
 * under icx 2026.0.0 with `-ffp-contract=off -fp-model=precise`). GCC
 * warns "unknown pragma" — wrap with `-Wunknown-pragmas` suppression
 * mirroring the established pattern in
 * `core/src/feature/x86/ssimulacra2_host_avx2.c`. See ADR for full
 * rationale + reproduction. */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunknown-pragmas"
#endif
#pragma clang fp contract(off)
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

#if ARCH_X86
#include "feature/x86/ssimulacra2_avx2.h"
#include "feature/x86/ssimulacra2_host_avx2.h"
#if HAVE_AVX512
#include "feature/x86/ssimulacra2_avx512.h"
#endif
#include "x86/cpu.h"
#endif
#if ARCH_AARCH64
#include "arm/cpu.h"
#include "feature/arm64/ssimulacra2_neon.h"
#include "feature/arm64/ssimulacra2_host_neon.h"
#if HAVE_SVE2
#include "feature/arm64/ssimulacra2_sve2.h"
#endif
#endif

static const float kM00 = 0.30f;
static const float kM02 = 0.078f;
static const float kM10 = 0.23f;
static const float kM12 = 0.078f;
static const float kM20 = 0.24342268924547819f;
static const float kM21 = 0.20476744424496821f;
static const float kOpsinBias = 0.0037930732552754493f;
static const float kC2 = 0.0009f;

/* xorshift32 for reproducible float input generation. */
static uint32_t xorshift32(uint32_t *state)
{
    uint32_t s = *state;
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    *state = s;
    return s;
}

/* Fill `n` floats with pseudo-random values in [lo, hi). */
static void fill_random(float *buf, size_t n, float lo, float hi, uint32_t seed)
{
    uint32_t state = seed;
    const float range = hi - lo;
    for (size_t i = 0; i < n; i++) {
        uint32_t r = xorshift32(&state);
        buf[i] = lo + range * ((float)(r & 0x00ffffff) / (float)0x01000000);
    }
}

/* Scalar reference: multiply_3plane */
static void ref_multiply_3plane(const float *a, const float *b, float *mul, unsigned w, unsigned h)
{
    const size_t n = 3u * (size_t)w * (size_t)h;
    for (size_t i = 0; i < n; i++) {
        mul[i] = a[i] * b[i];
    }
}

/* Scalar reference: linear_rgb_to_xyb */
static void ref_linear_rgb_to_xyb(const float *lin, float *xyb, unsigned w, unsigned h)
{
    const size_t plane_sz = (size_t)w * (size_t)h;
    const float *rp = lin;
    const float *gp = lin + plane_sz;
    const float *bp = lin + 2 * plane_sz;
    float *xp = xyb;
    float *yp = xyb + plane_sz;
    float *bxp = xyb + 2 * plane_sz;
    const float m01 = 1.0f - kM00 - kM02;
    const float m11 = 1.0f - kM10 - kM12;
    const float m22 = 1.0f - kM20 - kM21;
    const float cbrt_bias = vmaf_ss2_cbrtf(kOpsinBias);
    for (size_t i = 0; i < plane_sz; i++) {
        float r = rp[i];
        float g = gp[i];
        float b = bp[i];
        float l = kM00 * r + m01 * g + kM02 * b + kOpsinBias;
        float m = kM10 * r + m11 * g + kM12 * b + kOpsinBias;
        float s = kM20 * r + kM21 * g + m22 * b + kOpsinBias;
        if (l < 0.0f)
            l = 0.0f;
        if (m < 0.0f)
            m = 0.0f;
        if (s < 0.0f)
            s = 0.0f;
        float L = vmaf_ss2_cbrtf(l) - cbrt_bias;
        float M = vmaf_ss2_cbrtf(m) - cbrt_bias;
        float S = vmaf_ss2_cbrtf(s) - cbrt_bias;
        float X = 0.5f * (L - M);
        float Y = 0.5f * (L + M);
        float B = S;
        B = (B - Y) + 0.55f;
        X = X * 14.0f + 0.42f;
        Y = Y + 0.01f;
        xp[i] = X;
        yp[i] = Y;
        bxp[i] = B;
    }
}

/* Scalar reference: downsample_2x2. Kept as a line-for-line copy of the
 * extractor's scalar reference for bit-exactness auditability. */
// NOLINTNEXTLINE(readability-function-size,google-readability-function-size) — test scaffolding (ADR-0141)
static void ref_downsample_2x2(const float *in, unsigned iw, unsigned ih, float *out,
                               unsigned *ow_out, unsigned *oh_out)
{
    const unsigned ow = (iw + 1) / 2;
    const unsigned oh = (ih + 1) / 2;
    *ow_out = ow;
    *oh_out = oh;
    const size_t in_plane = (size_t)iw * (size_t)ih;
    const size_t out_plane = (size_t)ow * (size_t)oh;
    for (int c = 0; c < 3; c++) {
        const float *ip = in + (size_t)c * in_plane;
        float *op = out + (size_t)c * out_plane;
        for (unsigned oy = 0; oy < oh; oy++) {
            for (unsigned ox = 0; ox < ow; ox++) {
                float sum = 0.0f;
                for (unsigned dy = 0; dy < 2; dy++) {
                    for (unsigned dx = 0; dx < 2; dx++) {
                        unsigned ix = ox * 2 + dx;
                        unsigned iy = oy * 2 + dy;
                        if (ix >= iw)
                            ix = iw - 1;
                        if (iy >= ih)
                            iy = ih - 1;
                        sum += ip[(size_t)iy * iw + ix];
                    }
                }
                op[(size_t)oy * ow + ox] = sum * 0.25f;
            }
        }
    }
}

static inline double quartic_d(double x)
{
    x *= x;
    return x * x;
}

/* Scalar reference: ssim_map */
static void ref_ssim_map(const float *m1, const float *m2, const float *s11, const float *s22,
                         const float *s12, unsigned w, unsigned h, double plane_averages[6])
{
    const size_t plane = (size_t)w * (size_t)h;
    const double one_per_pixels = 1.0 / (double)plane;
    for (int c = 0; c < 3; c++) {
        double sum_l1 = 0.0;
        double sum_l4 = 0.0;
        const float *rm1 = m1 + (size_t)c * plane;
        const float *rm2 = m2 + (size_t)c * plane;
        const float *rs11 = s11 + (size_t)c * plane;
        const float *rs22 = s22 + (size_t)c * plane;
        const float *rs12 = s12 + (size_t)c * plane;
        for (size_t i = 0; i < plane; i++) {
            float mu1 = rm1[i];
            float mu2 = rm2[i];
            float mu11 = mu1 * mu1;
            float mu22 = mu2 * mu2;
            float mu12 = mu1 * mu2;
            float num_m = 1.0f - (mu1 - mu2) * (mu1 - mu2);
            float num_s = 2.0f * (rs12[i] - mu12) + kC2;
            float denom_s = (rs11[i] - mu11) + (rs22[i] - mu22) + kC2;
            double d = 1.0 - ((double)num_m * (double)num_s / (double)denom_s);
            if (d < 0.0)
                d = 0.0;
            sum_l1 += d;
            sum_l4 += quartic_d(d);
        }
        plane_averages[c * 2 + 0] = one_per_pixels * sum_l1;
        plane_averages[c * 2 + 1] = sqrt(sqrt(one_per_pixels * sum_l4));
    }
}

/* Scalar reference: edge_diff_map */
static void ref_edge_diff_map(const float *img1, const float *mu1, const float *img2,
                              const float *mu2, unsigned w, unsigned h, double plane_averages[12])
{
    const size_t plane = (size_t)w * (size_t)h;
    const double one_per_pixels = 1.0 / (double)plane;
    for (int c = 0; c < 3; c++) {
        double s0 = 0.0;
        double s1 = 0.0;
        double s2 = 0.0;
        double s3 = 0.0;
        const float *r1 = img1 + (size_t)c * plane;
        const float *rm1 = mu1 + (size_t)c * plane;
        const float *r2 = img2 + (size_t)c * plane;
        const float *rm2 = mu2 + (size_t)c * plane;
        for (size_t i = 0; i < plane; i++) {
            double ed1 = fabs((double)r1[i] - (double)rm1[i]);
            double ed2 = fabs((double)r2[i] - (double)rm2[i]);
            double d = (1.0 + ed2) / (1.0 + ed1) - 1.0;
            double art = d > 0.0 ? d : 0.0;
            double det = d < 0.0 ? -d : 0.0;
            s0 += art;
            s1 += quartic_d(art);
            s2 += det;
            s3 += quartic_d(det);
        }
        plane_averages[c * 4 + 0] = one_per_pixels * s0;
        plane_averages[c * 4 + 1] = sqrt(sqrt(one_per_pixels * s1));
        plane_averages[c * 4 + 2] = one_per_pixels * s2;
        plane_averages[c * 4 + 3] = sqrt(sqrt(one_per_pixels * s3));
    }
}

#define W 33 /* deliberately non-multiple of 8 / 16 to exercise tails */
#define H 21
#define PLANE_SZ ((size_t)W * (size_t)H)
#define RGB_SZ (3u * PLANE_SZ)

/* SIMD dispatcher: pick AVX2 on x86, NEON on aarch64. Returns NULL on
 * platforms where no SIMD variant is available (the test will skip). */
typedef void (*mul3_fn_t)(const float *, const float *, float *, unsigned, unsigned);
typedef void (*xyb_fn_t)(const float *, float *, unsigned, unsigned);
typedef void (*down_fn_t)(const float *, unsigned, unsigned, float *, unsigned *, unsigned *);
typedef void (*ssim_fn_t)(const float *, const float *, const float *, const float *, const float *,
                          unsigned, unsigned, double[6]);
typedef void (*edge_fn_t)(const float *, const float *, const float *, const float *, unsigned,
                          unsigned, double[12]);

static mul3_fn_t pick_mul3(void)
{
#if ARCH_X86
    const unsigned flags = vmaf_get_cpu_flags_x86();
#if HAVE_AVX512
    if (flags & VMAF_X86_CPU_FLAG_AVX512)
        return ssimulacra2_multiply_3plane_avx512;
#endif
    if (flags & VMAF_X86_CPU_FLAG_AVX2)
        return ssimulacra2_multiply_3plane_avx2;
#endif
#if ARCH_AARCH64
#if HAVE_SVE2
    if (vmaf_get_cpu_flags_arm() & VMAF_ARM_CPU_FLAG_SVE2)
        return ssimulacra2_multiply_3plane_sve2;
#endif
    return ssimulacra2_multiply_3plane_neon;
#endif
    return NULL;
}
static xyb_fn_t pick_xyb(void)
{
#if ARCH_X86
    const unsigned flags = vmaf_get_cpu_flags_x86();
#if HAVE_AVX512
    if (flags & VMAF_X86_CPU_FLAG_AVX512)
        return ssimulacra2_linear_rgb_to_xyb_avx512;
#endif
    if (flags & VMAF_X86_CPU_FLAG_AVX2)
        return ssimulacra2_linear_rgb_to_xyb_avx2;
#endif
#if ARCH_AARCH64
#if HAVE_SVE2
    if (vmaf_get_cpu_flags_arm() & VMAF_ARM_CPU_FLAG_SVE2)
        return ssimulacra2_linear_rgb_to_xyb_sve2;
#endif
    return ssimulacra2_linear_rgb_to_xyb_neon;
#endif
    return NULL;
}
static down_fn_t pick_down(void)
{
#if ARCH_X86
    const unsigned flags = vmaf_get_cpu_flags_x86();
#if HAVE_AVX512
    if (flags & VMAF_X86_CPU_FLAG_AVX512)
        return ssimulacra2_downsample_2x2_avx512;
#endif
    if (flags & VMAF_X86_CPU_FLAG_AVX2)
        return ssimulacra2_downsample_2x2_avx2;
#endif
#if ARCH_AARCH64
#if HAVE_SVE2
    if (vmaf_get_cpu_flags_arm() & VMAF_ARM_CPU_FLAG_SVE2)
        return ssimulacra2_downsample_2x2_sve2;
#endif
    return ssimulacra2_downsample_2x2_neon;
#endif
    return NULL;
}
static ssim_fn_t pick_ssim(void)
{
#if ARCH_X86
    const unsigned flags = vmaf_get_cpu_flags_x86();
#if HAVE_AVX512
    if (flags & VMAF_X86_CPU_FLAG_AVX512)
        return ssimulacra2_ssim_map_avx512;
#endif
    if (flags & VMAF_X86_CPU_FLAG_AVX2)
        return ssimulacra2_ssim_map_avx2;
#endif
#if ARCH_AARCH64
#if HAVE_SVE2
    if (vmaf_get_cpu_flags_arm() & VMAF_ARM_CPU_FLAG_SVE2)
        return ssimulacra2_ssim_map_sve2;
#endif
    return ssimulacra2_ssim_map_neon;
#endif
    return NULL;
}
static edge_fn_t pick_edge(void)
{
#if ARCH_X86
    const unsigned flags = vmaf_get_cpu_flags_x86();
#if HAVE_AVX512
    if (flags & VMAF_X86_CPU_FLAG_AVX512)
        return ssimulacra2_edge_diff_map_avx512;
#endif
    if (flags & VMAF_X86_CPU_FLAG_AVX2)
        return ssimulacra2_edge_diff_map_avx2;
#endif
#if ARCH_AARCH64
#if HAVE_SVE2
    if (vmaf_get_cpu_flags_arm() & VMAF_ARM_CPU_FLAG_SVE2)
        return ssimulacra2_edge_diff_map_sve2;
#endif
    return ssimulacra2_edge_diff_map_neon;
#endif
    return NULL;
}

static char *test_multiply(void)
{
    mul3_fn_t fn = pick_mul3();
    if (!fn)
        return NULL;
    float *a = malloc(RGB_SZ * sizeof(float));
    float *b = malloc(RGB_SZ * sizeof(float));
    float *out_ref = malloc(RGB_SZ * sizeof(float));
    float *out_simd = malloc(RGB_SZ * sizeof(float));
    if (!a || !b || !out_ref || !out_simd) {
        free(a);
        free(b);
        free(out_ref);
        free(out_simd);
        return "malloc failed";
    }
    fill_random(a, RGB_SZ, -2.0f, 2.0f, 0xdeadbeefu);
    fill_random(b, RGB_SZ, -2.0f, 2.0f, 0x12345678u);
    ref_multiply_3plane(a, b, out_ref, W, H);
    fn(a, b, out_simd, W, H);
    /* memcmp on float buffers is deliberate: ADR-0161's SIMD contract
     * requires byte-for-byte equality under FLT_EVAL_METHOD == 0. */
    // NOLINTNEXTLINE(bugprone-suspicious-memory-comparison,cert-exp42-c,cert-flp37-c)
    int match = memcmp(out_ref, out_simd, RGB_SZ * sizeof(float)) == 0;
    free(a);
    free(b);
    free(out_ref);
    free(out_simd);
    mu_assert("multiply_3plane SIMD not bit-identical to scalar", match);
    return NULL;
}

static char *test_xyb(void)
{
    xyb_fn_t fn = pick_xyb();
    if (!fn)
        return NULL;
    float *lin = malloc(RGB_SZ * sizeof(float));
    float *out_ref = malloc(RGB_SZ * sizeof(float));
    float *out_simd = malloc(RGB_SZ * sizeof(float));
    if (!lin || !out_ref || !out_simd) {
        free(lin);
        free(out_ref);
        free(out_simd);
        return "malloc failed";
    }
    fill_random(lin, RGB_SZ, 0.0f, 1.0f, 0xabcdef01u);
    ref_linear_rgb_to_xyb(lin, out_ref, W, H);
    fn(lin, out_simd, W, H);
    // NOLINTNEXTLINE(bugprone-suspicious-memory-comparison,cert-exp42-c,cert-flp37-c) ADR-0161 byte-exact
    int match = memcmp(out_ref, out_simd, RGB_SZ * sizeof(float)) == 0;
    free(lin);
    free(out_ref);
    free(out_simd);
    mu_assert("linear_rgb_to_xyb SIMD not bit-identical to scalar", match);
    return NULL;
}

static char *test_downsample(void)
{
    down_fn_t fn = pick_down();
    if (!fn)
        return NULL;
    float *in = malloc(RGB_SZ * sizeof(float));
    const size_t out_plane = (size_t)((W + 1) / 2) * (size_t)((H + 1) / 2);
    float *out_ref = malloc(3 * out_plane * sizeof(float));
    float *out_simd = malloc(3 * out_plane * sizeof(float));
    if (!in || !out_ref || !out_simd) {
        free(in);
        free(out_ref);
        free(out_simd);
        return "malloc failed";
    }
    fill_random(in, RGB_SZ, -1.0f, 1.0f, 0x5a5a5a5au);
    unsigned ow_r = 0;
    unsigned oh_r = 0;
    unsigned ow_s = 0;
    unsigned oh_s = 0;
    ref_downsample_2x2(in, W, H, out_ref, &ow_r, &oh_r);
    fn(in, W, H, out_simd, &ow_s, &oh_s);
    // NOLINTNEXTLINE(bugprone-suspicious-memory-comparison,cert-exp42-c,cert-flp37-c) ADR-0161 byte-exact
    const int mem_eq = memcmp(out_ref, out_simd, 3 * out_plane * sizeof(float)) == 0;
    int match = (ow_r == ow_s) && (oh_r == oh_s) && mem_eq;
    free(in);
    free(out_ref);
    free(out_simd);
    mu_assert("downsample_2x2 SIMD not bit-identical to scalar", match);
    return NULL;
}

static char *test_ssim(void)
{
    ssim_fn_t fn = pick_ssim();
    if (!fn)
        return NULL;
    float *m1 = malloc(RGB_SZ * sizeof(float));
    float *m2 = malloc(RGB_SZ * sizeof(float));
    float *s11 = malloc(RGB_SZ * sizeof(float));
    float *s22 = malloc(RGB_SZ * sizeof(float));
    float *s12 = malloc(RGB_SZ * sizeof(float));
    if (!m1 || !m2 || !s11 || !s22 || !s12) {
        free(m1);
        free(m2);
        free(s11);
        free(s22);
        free(s12);
        return "malloc failed";
    }
    fill_random(m1, RGB_SZ, -0.5f, 0.5f, 0x11111111u);
    fill_random(m2, RGB_SZ, -0.5f, 0.5f, 0x22222222u);
    fill_random(s11, RGB_SZ, 0.0f, 1.0f, 0x33333333u);
    fill_random(s22, RGB_SZ, 0.0f, 1.0f, 0x44444444u);
    fill_random(s12, RGB_SZ, 0.0f, 1.0f, 0x55555555u);
    double ref[6] = {0};
    double simd[6] = {0};
    ref_ssim_map(m1, m2, s11, s22, s12, W, H, ref);
    fn(m1, m2, s11, s22, s12, W, H, simd);
    // NOLINTNEXTLINE(bugprone-suspicious-memory-comparison,cert-exp42-c,cert-flp37-c) ADR-0161 byte-exact
    int match = memcmp(ref, simd, sizeof(ref)) == 0;
    free(m1);
    free(m2);
    free(s11);
    free(s22);
    free(s12);
    mu_assert("ssim_map SIMD averages not bit-identical to scalar", match);
    return NULL;
}

/* Scalar reference: fast_gaussian_1d — verbatim copy of the extractor's
 * static function. */
static void ref_fast_gaussian_1d(const float n2[3], const float d1[3], int radius, const float *in,
                                 float *out, ptrdiff_t xsize)
{
    float prev1_0 = 0.f;
    float prev1_1 = 0.f;
    float prev1_2 = 0.f;
    float prev2_0 = 0.f;
    float prev2_1 = 0.f;
    float prev2_2 = 0.f;
    const float n2_0 = n2[0];
    const float n2_1 = n2[1];
    const float n2_2 = n2[2];
    const float d1_0 = d1[0];
    const float d1_1 = d1[1];
    const float d1_2 = d1[2];
    const ptrdiff_t N = (ptrdiff_t)radius;
    for (ptrdiff_t n = -N + 1; n < xsize; n++) {
        const ptrdiff_t left = n - N - 1;
        const ptrdiff_t right = n + N - 1;
        const float lv = (left >= 0) ? in[left] : 0.f;
        const float rv = (right < xsize) ? in[right] : 0.f;
        const float sum = lv + rv;
        const float o0 = n2_0 * sum - d1_0 * prev1_0 - prev2_0;
        const float o1 = n2_1 * sum - d1_1 * prev1_1 - prev2_1;
        const float o2 = n2_2 * sum - d1_2 * prev1_2 - prev2_2;
        prev2_0 = prev1_0;
        prev2_1 = prev1_1;
        prev2_2 = prev1_2;
        prev1_0 = o0;
        prev1_1 = o1;
        prev1_2 = o2;
        if (n >= 0) {
            out[n] = o0 + o1 + o2;
        }
    }
}

/* Scalar reference: blur_plane. */
// NOLINTNEXTLINE(readability-function-size,google-readability-function-size) — test scaffolding (ADR-0141)
static void ref_blur_plane(const float n2[3], const float d1[3], int radius, float *col_state,
                           const float *in, float *out, float *scratch, unsigned w, unsigned h)
{
    for (unsigned y = 0; y < h; y++) {
        ref_fast_gaussian_1d(n2, d1, radius, in + (size_t)y * w, scratch + (size_t)y * w,
                             (ptrdiff_t)w);
    }
    const size_t xsize = (size_t)w;
    float *prev1_0 = col_state + 0u * xsize;
    float *prev1_1 = col_state + 1u * xsize;
    float *prev1_2 = col_state + 2u * xsize;
    float *prev2_0 = col_state + 3u * xsize;
    float *prev2_1 = col_state + 4u * xsize;
    float *prev2_2 = col_state + 5u * xsize;
    memset(col_state, 0, 6u * xsize * sizeof(float));
    const float n2_0 = n2[0];
    const float n2_1 = n2[1];
    const float n2_2 = n2[2];
    const float d1_0 = d1[0];
    const float d1_1 = d1[1];
    const float d1_2 = d1[2];
    const ptrdiff_t N = (ptrdiff_t)radius;
    const ptrdiff_t ysize = (ptrdiff_t)h;
    for (ptrdiff_t n = -N + 1; n < ysize; n++) {
        const ptrdiff_t left = n - N - 1;
        const ptrdiff_t right = n + N - 1;
        const float *lrow = (left >= 0) ? (scratch + (size_t)left * xsize) : NULL;
        const float *rrow = (right < ysize) ? (scratch + (size_t)right * xsize) : NULL;
        float *orow = (n >= 0) ? (out + (size_t)n * xsize) : NULL;
        for (size_t x = 0; x < xsize; x++) {
            const float lv = lrow ? lrow[x] : 0.f;
            const float rv = rrow ? rrow[x] : 0.f;
            const float sum = lv + rv;
            const float o0 = n2_0 * sum - d1_0 * prev1_0[x] - prev2_0[x];
            const float o1 = n2_1 * sum - d1_1 * prev1_1[x] - prev2_1[x];
            const float o2 = n2_2 * sum - d1_2 * prev1_2[x] - prev2_2[x];
            prev2_0[x] = prev1_0[x];
            prev2_1[x] = prev1_1[x];
            prev2_2[x] = prev1_2[x];
            prev1_0[x] = o0;
            prev1_1[x] = o1;
            prev1_2[x] = o2;
            if (orow) {
                orow[x] = o0 + o1 + o2;
            }
        }
    }
}

typedef void (*blur_fn_t)(const float n2[3], const float d1[3], int radius, float *col_state,
                          const float *in, float *out, float *scratch, unsigned w, unsigned h);

static blur_fn_t pick_blur(void)
{
#if ARCH_X86
    const unsigned flags = vmaf_get_cpu_flags_x86();
#if HAVE_AVX512
    if (flags & VMAF_X86_CPU_FLAG_AVX512)
        return ssimulacra2_blur_plane_avx512;
#endif
    if (flags & VMAF_X86_CPU_FLAG_AVX2)
        return ssimulacra2_blur_plane_avx2;
#endif
#if ARCH_AARCH64
#if HAVE_SVE2
    if (vmaf_get_cpu_flags_arm() & VMAF_ARM_CPU_FLAG_SVE2)
        return ssimulacra2_blur_plane_sve2;
#endif
    return ssimulacra2_blur_plane_neon;
#endif
    return NULL;
}

static char *test_edge(void)
{
    edge_fn_t fn = pick_edge();
    if (!fn)
        return NULL;
    float *img1 = malloc(RGB_SZ * sizeof(float));
    float *mu1 = malloc(RGB_SZ * sizeof(float));
    float *img2 = malloc(RGB_SZ * sizeof(float));
    float *mu2 = malloc(RGB_SZ * sizeof(float));
    if (!img1 || !mu1 || !img2 || !mu2) {
        free(img1);
        free(mu1);
        free(img2);
        free(mu2);
        return "malloc failed";
    }
    fill_random(img1, RGB_SZ, -1.0f, 1.0f, 0x66666666u);
    fill_random(mu1, RGB_SZ, -1.0f, 1.0f, 0x77777777u);
    fill_random(img2, RGB_SZ, -1.0f, 1.0f, 0x88888888u);
    fill_random(mu2, RGB_SZ, -1.0f, 1.0f, 0x99999999u);
    double ref[12] = {0};
    double simd[12] = {0};
    ref_edge_diff_map(img1, mu1, img2, mu2, W, H, ref);
    fn(img1, mu1, img2, mu2, W, H, simd);
    // NOLINTNEXTLINE(bugprone-suspicious-memory-comparison,cert-exp42-c,cert-flp37-c) ADR-0161 byte-exact
    int match = memcmp(ref, simd, sizeof(ref)) == 0;
    free(img1);
    free(mu1);
    free(img2);
    free(mu2);
    mu_assert("edge_diff_map SIMD averages not bit-identical to scalar", match);
    return NULL;
}

/* Blur plane bit-exactness test. Uses realistic IIR coefficients
 * computed via the libjxl recursive-gaussian formula for sigma=1.5. */
static char *test_blur(void)
{
    blur_fn_t fn = pick_blur();
    if (!fn)
        return NULL;
    /* Pre-computed coefficients for sigma=1.5 (from libjxl create_recursive_gaussian
     * reference output). Exact values are not critical for the bit-exactness test
     * — what matters is that scalar ref and SIMD use the same. */
    const float n2[3] = {-1.15927751f, -0.95251870f, -0.42578530f};
    const float d1[3] = {-1.89840269f, -1.64985192f, -1.19907868f};
    const int radius = 4;
    const size_t plane = PLANE_SZ;
    float *in = malloc(plane * sizeof(float));
    float *out_ref = malloc(plane * sizeof(float));
    float *out_simd = malloc(plane * sizeof(float));
    float *scratch_ref = malloc(plane * sizeof(float));
    float *scratch_simd = malloc(plane * sizeof(float));
    float *col_state_ref = malloc(6u * (size_t)W * sizeof(float));
    float *col_state_simd = malloc(6u * (size_t)W * sizeof(float));
    if (!in || !out_ref || !out_simd || !scratch_ref || !scratch_simd || !col_state_ref ||
        !col_state_simd) {
        free(in);
        free(out_ref);
        free(out_simd);
        free(scratch_ref);
        free(scratch_simd);
        free(col_state_ref);
        free(col_state_simd);
        return "malloc failed";
    }
    fill_random(in, plane, -0.5f, 0.5f, 0xcafebabeu);
    ref_blur_plane(n2, d1, radius, col_state_ref, in, out_ref, scratch_ref, W, H);
    fn(n2, d1, radius, col_state_simd, in, out_simd, scratch_simd, W, H);
    // NOLINTNEXTLINE(bugprone-suspicious-memory-comparison,cert-exp42-c,cert-flp37-c) ADR-0162 byte-exact
    const int match = memcmp(out_ref, out_simd, plane * sizeof(float)) == 0;
    free(in);
    free(out_ref);
    free(out_simd);
    free(scratch_ref);
    free(scratch_simd);
    free(col_state_ref);
    free(col_state_simd);
    mu_assert("blur_plane SIMD not bit-identical to scalar", match);
    return NULL;
}

/* Scalar reference: sRGB EOTF — verbatim copy of extractor's inline helper. */
static inline float ref_srgb_to_linear(float v)
{
    return vmaf_ss2_srgb_eotf(v);
}

/* Scalar reference: read_plane — handles all chroma ratios + 8/16-bit. */
static inline float ref_read_plane(const simd_plane_t *p, unsigned lw, unsigned lh, int x, int y,
                                   unsigned bpc)
{
    const unsigned pw = p->w;
    const unsigned ph = p->h;
    int sx;
    int sy;
    if (pw == lw) {
        sx = x;
    } else if (pw * 2 == lw) {
        sx = x >> 1;
    } else {
        sx = (int)((int64_t)x * (int64_t)pw / (int64_t)lw);
    }
    if (ph == lh) {
        sy = y;
    } else if (ph * 2 == lh) {
        sy = y >> 1;
    } else {
        sy = (int)((int64_t)y * (int64_t)ph / (int64_t)lh);
    }
    if (sx < 0)
        sx = 0;
    if (sy < 0)
        sy = 0;
    if ((unsigned)sx >= pw)
        sx = (int)pw - 1;
    if ((unsigned)sy >= ph)
        sy = (int)ph - 1;
    if (bpc > 8) {
        const uint16_t *row = (const uint16_t *)((const uint8_t *)p->data + (size_t)sy * p->stride);
        return (float)row[sx];
    }
    const uint8_t *row = (const uint8_t *)p->data + (size_t)sy * p->stride;
    return (float)row[sx];
}

/* Scalar reference: picture_to_linear_rgb. */
// NOLINTNEXTLINE(readability-function-size,google-readability-function-size) — test scaffolding (ADR-0141)
static void ref_picture_to_linear_rgb(int yuv_matrix, unsigned bpc, unsigned w, unsigned h,
                                      const simd_plane_t planes[3], float *out)
{
    const size_t plane_sz = (size_t)w * (size_t)h;
    float *rp = out;
    float *gp = out + plane_sz;
    float *bp = out + 2 * plane_sz;
    const float peak = (float)((1u << bpc) - 1u);
    const float inv_peak = 1.0f / peak;
    float kr;
    float kg;
    float kb;
    int limited = 1;
    switch (yuv_matrix) {
    case 2:
        limited = 0; /* fall through */
        /* fall through */
    case 0:
        kr = 0.2126f;
        kg = 0.7152f;
        kb = 0.0722f;
        break;
    case 3:
        limited = 0;
        kr = 0.299f;
        kg = 0.587f;
        kb = 0.114f;
        break;
    case 1:
    default:
        kr = 0.299f;
        kg = 0.587f;
        kb = 0.114f;
        break;
    }
    const float cr_r = 2.0f * (1.0f - kr);
    const float cb_b = 2.0f * (1.0f - kb);
    const float cb_g = -(2.0f * kb * (1.0f - kb)) / kg;
    const float cr_g = -(2.0f * kr * (1.0f - kr)) / kg;
    const float y_scale = limited ? (255.0f / 219.0f) : 1.0f;
    const float c_scale = limited ? (255.0f / 224.0f) : 1.0f;
    const float y_off = limited ? (16.0f / 255.0f) : 0.0f;
    const float c_off = 0.5f;
    for (unsigned y = 0; y < h; y++) {
        for (unsigned x = 0; x < w; x++) {
            const float Y = ref_read_plane(&planes[0], w, h, (int)x, (int)y, bpc) * inv_peak;
            const float U = ref_read_plane(&planes[1], w, h, (int)x, (int)y, bpc) * inv_peak;
            const float V = ref_read_plane(&planes[2], w, h, (int)x, (int)y, bpc) * inv_peak;
            const float Yn = (Y - y_off) * y_scale;
            const float Un = (U - c_off) * c_scale;
            const float Vn = (V - c_off) * c_scale;
            /* ADR-0891: explicit fmaf() — icx + `-mfma` may contract
             * plain `a + b*c` to FMA even under `-fp-model=precise`,
             * diverging from the SIMD implementation. Preserves the
             * left-to-right associativity of the G computation. */
            float R = fmaf(cr_r, Vn, Yn);
            float G = fmaf(cb_g, Un, Yn);
            G = fmaf(cr_g, Vn, G);
            float B = fmaf(cb_b, Un, Yn);
            if (R < 0.0f)
                R = 0.0f;
            if (R > 1.0f)
                R = 1.0f;
            if (G < 0.0f)
                G = 0.0f;
            if (G > 1.0f)
                G = 1.0f;
            if (B < 0.0f)
                B = 0.0f;
            if (B > 1.0f)
                B = 1.0f;
            const size_t idx = (size_t)y * w + x;
            rp[idx] = ref_srgb_to_linear(R);
            gp[idx] = ref_srgb_to_linear(G);
            bp[idx] = ref_srgb_to_linear(B);
        }
    }
}

typedef void (*ptlr_fn_t)(int, unsigned, unsigned, unsigned, const simd_plane_t[3], float *);

static ptlr_fn_t pick_ptlr(void)
{
#if ARCH_X86
    const unsigned flags = vmaf_get_cpu_flags_x86();
#if HAVE_AVX512
    if (flags & VMAF_X86_CPU_FLAG_AVX512)
        return ssimulacra2_picture_to_linear_rgb_avx512;
#endif
    if (flags & VMAF_X86_CPU_FLAG_AVX2)
        return ssimulacra2_picture_to_linear_rgb_avx2;
#endif
#if ARCH_AARCH64
#if HAVE_SVE2
    if (vmaf_get_cpu_flags_arm() & VMAF_ARM_CPU_FLAG_SVE2)
        return ssimulacra2_picture_to_linear_rgb_sve2;
#endif
    return ssimulacra2_picture_to_linear_rgb_neon;
#endif
    return NULL;
}

/* Test all 6 common (yuv_matrix × subsampling) combinations on small frames. */
/* Test helper — drives all 5 format variants (420/422/444 × 8/10-bit)
 * through one parameterised entry point. Splitting would duplicate
 * the per-plane fixture setup + 3× xorshift fill + shell. */
// NOLINTNEXTLINE(readability-function-size,google-readability-function-size) — test scaffolding (ADR-0141)
static char *test_ptlr_one(int yuv_matrix, unsigned bpc, unsigned uw_div, unsigned uh_div)
{
    /*
     * TODO(ssimulacra2-ptlr-mingw): scalar fmaf() on MinGW-w64's libm
     * (compiled without -mfma) is not guaranteed to be correctly
     * single-rounded, so scalar-vs-AVX2 / scalar-vs-AVX-512
     * bit-exactness fails on Windows MinGW64 CI even after the
     * ADR-0891 FMA unification. Skip the whole test there for now;
     * Linux/macOS libm fmaf() is correctly rounded and the test runs
     * fine on those hosts. Mirrors the existing skip in
     * core/test/test_ms_ssim_decimate.c (see TODO(ms-ssim-mingw)).
     */
#if defined(_WIN32) || defined(__MINGW32__) || defined(__MINGW64__)
    (void)yuv_matrix;
    (void)bpc;
    (void)uw_div;
    (void)uh_div;
    (void)fprintf(stderr, "skipping: Windows libm fmaf not bit-exact with hw FMA "
                          "(see TODO(ssimulacra2-ptlr-mingw))\n");
    return NULL;
#else
    ptlr_fn_t fn = pick_ptlr();
    if (!fn)
        return NULL;
    const unsigned LW = 24;
    const unsigned LH = 16;
    const unsigned UW = LW / uw_div;
    const unsigned UH = LH / uh_div;
    const size_t y_sz = (size_t)LW * LH;
    const size_t c_sz = (size_t)UW * UH;
    const size_t elem = (bpc > 8) ? sizeof(uint16_t) : sizeof(uint8_t);
    void *y_buf = calloc(y_sz, elem);
    void *u_buf = calloc(c_sz, elem);
    void *v_buf = calloc(c_sz, elem);
    if (!y_buf || !u_buf || !v_buf) {
        free(y_buf);
        free(u_buf);
        free(v_buf);
        return "calloc failed";
    }
    /* Fill with pseudo-random 8/16-bit pixel values. */
    uint32_t s = 0xabadcafeu ^ (uint32_t)(yuv_matrix * 7 + bpc * 13 + uw_div * 5 + uh_div);
    const unsigned maxv = (1u << bpc) - 1u;
    for (size_t i = 0; i < y_sz; i++) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        const unsigned v = s % (maxv + 1);
        if (bpc > 8) {
            ((uint16_t *)y_buf)[i] = (uint16_t)v;
        } else {
            ((uint8_t *)y_buf)[i] = (uint8_t)v;
        }
    }
    for (size_t i = 0; i < c_sz; i++) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        const unsigned v = s % (maxv + 1);
        if (bpc > 8) {
            ((uint16_t *)u_buf)[i] = (uint16_t)v;
        } else {
            ((uint8_t *)u_buf)[i] = (uint8_t)v;
        }
    }
    for (size_t i = 0; i < c_sz; i++) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        const unsigned v = s % (maxv + 1);
        if (bpc > 8) {
            ((uint16_t *)v_buf)[i] = (uint16_t)v;
        } else {
            ((uint8_t *)v_buf)[i] = (uint8_t)v;
        }
    }
    const simd_plane_t planes[3] = {
        {y_buf, (ptrdiff_t)LW * (ptrdiff_t)elem, LW, LH},
        {u_buf, (ptrdiff_t)UW * (ptrdiff_t)elem, UW, UH},
        {v_buf, (ptrdiff_t)UW * (ptrdiff_t)elem, UW, UH},
    };
    const size_t out_sz = 3u * y_sz;
    float *out_ref = malloc(out_sz * sizeof(float));
    float *out_simd = malloc(out_sz * sizeof(float));
    if (!out_ref || !out_simd) {
        free(y_buf);
        free(u_buf);
        free(v_buf);
        free(out_ref);
        free(out_simd);
        return "malloc failed";
    }
    ref_picture_to_linear_rgb(yuv_matrix, bpc, LW, LH, planes, out_ref);
    fn(yuv_matrix, bpc, LW, LH, planes, out_simd);
    // NOLINTNEXTLINE(bugprone-suspicious-memory-comparison,cert-exp42-c,cert-flp37-c) ADR-0163 byte-exact
    const int match = memcmp(out_ref, out_simd, out_sz * sizeof(float)) == 0;
    free(y_buf);
    free(u_buf);
    free(v_buf);
    free(out_ref);
    free(out_simd);
    mu_assert("picture_to_linear_rgb SIMD not bit-identical to scalar", match);
    return NULL;
#endif /* _WIN32 / MINGW */
}

static char *test_ptlr_420_8(void)
{
    return test_ptlr_one(0, 8, 2, 2);
}
static char *test_ptlr_420_10(void)
{
    return test_ptlr_one(0, 10, 2, 2);
}
static char *test_ptlr_444_8(void)
{
    return test_ptlr_one(1, 8, 1, 1);
}
static char *test_ptlr_444_10(void)
{
    return test_ptlr_one(3, 10, 1, 1);
}
static char *test_ptlr_422_8(void)
{
    return test_ptlr_one(2, 8, 2, 1);
}

/* ------------------------------------------------------------------ */
/* ADR-0242: host-kernel bit-exactness tests (plane_stride form).     */
/* These cover the Vulkan extractor's pyramid buffer layout where each  */
/* plane slot is plane_stride floats wide even at downsampled scales.  */
/* ------------------------------------------------------------------ */

/* Scalar reference: linear_rgb_to_xyb with explicit plane_stride. */
static void ref_host_linear_rgb_to_xyb(const float *lin, float *xyb, unsigned w, unsigned h,
                                       size_t plane_stride)
{
    const float m01 = 1.0f - kM00 - kM02;
    const float m11 = 1.0f - kM10 - kM12;
    const float m22 = 1.0f - kM20 - kM21;
    const float cbrt_bias = vmaf_ss2_cbrtf(kOpsinBias);
    const float *rp = lin;
    const float *gp = lin + plane_stride;
    const float *bp = lin + 2u * plane_stride;
    float *xp = xyb;
    float *yp = xyb + plane_stride;
    float *bxp = xyb + 2u * plane_stride;
    const size_t scale_pixels = (size_t)w * (size_t)h;
    for (size_t i = 0; i < scale_pixels; i++) {
        float r = rp[i];
        float g = gp[i];
        float b = bp[i];
        float l = kM00 * r + m01 * g + kM02 * b + kOpsinBias;
        float mv = kM10 * r + m11 * g + kM12 * b + kOpsinBias;
        float sv = kM20 * r + kM21 * g + m22 * b + kOpsinBias;
        if (l < 0.0f)
            l = 0.0f;
        if (mv < 0.0f)
            mv = 0.0f;
        if (sv < 0.0f)
            sv = 0.0f;
        float L = vmaf_ss2_cbrtf(l) - cbrt_bias;
        float M = vmaf_ss2_cbrtf(mv) - cbrt_bias;
        float S = vmaf_ss2_cbrtf(sv) - cbrt_bias;
        float X = 0.5f * (L - M);
        float Y = 0.5f * (L + M);
        float B = S;
        B = (B - Y) + 0.55f;
        X = X * 14.0f + 0.42f;
        Y = Y + 0.01f;
        xp[i] = X;
        yp[i] = Y;
        bxp[i] = B;
    }
}

/* Scalar reference: downsample_2x2 with explicit plane_stride. */
static void ref_host_downsample_2x2(const float *in, unsigned iw, unsigned ih, float *out,
                                    unsigned ow, unsigned oh, size_t plane_stride)
{
    for (int c = 0; c < 3; c++) {
        const float *ip = in + (size_t)c * plane_stride;
        float *op = out + (size_t)c * plane_stride;
        for (unsigned oy = 0; oy < oh; oy++) {
            for (unsigned ox = 0; ox < ow; ox++) {
                unsigned ix0 = ox * 2;
                unsigned iy0 = oy * 2;
                unsigned ix1 = (ix0 + 1 < iw) ? ix0 + 1 : iw - 1;
                unsigned iy1 = (iy0 + 1 < ih) ? iy0 + 1 : ih - 1;
                float sum = ip[(size_t)iy0 * iw + ix0] + ip[(size_t)iy0 * iw + ix1] +
                            ip[(size_t)iy1 * iw + ix0] + ip[(size_t)iy1 * iw + ix1];
                op[(size_t)oy * ow + ox] = sum * 0.25f;
            }
        }
    }
}

typedef void (*host_xyb_fn_t)(const float *, float *, unsigned, unsigned, size_t);
typedef void (*host_down_fn_t)(const float *, unsigned, unsigned, float *, unsigned, unsigned,
                               size_t);

static host_xyb_fn_t pick_host_xyb(void)
{
#if ARCH_X86
    const unsigned flags = vmaf_get_cpu_flags_x86();
    if (flags & VMAF_X86_CPU_FLAG_AVX2)
        return ssimulacra2_host_linear_rgb_to_xyb_avx2;
#endif
#if ARCH_AARCH64
    return ssimulacra2_host_linear_rgb_to_xyb_neon;
#endif
    return NULL;
}

static host_down_fn_t pick_host_down(void)
{
#if ARCH_X86
    const unsigned flags = vmaf_get_cpu_flags_x86();
    if (flags & VMAF_X86_CPU_FLAG_AVX2)
        return ssimulacra2_host_downsample_2x2_avx2;
#endif
#if ARCH_AARCH64
    return ssimulacra2_host_downsample_2x2_neon;
#endif
    return NULL;
}

/* Use a plane_stride larger than w*h to exercise the Vulkan pyramid layout
 * where the slot is fixed at full-resolution even at downsampled scales. */
#define HOST_W 17
#define HOST_H 11
#define HOST_STRIDE (HOST_W * HOST_H + 64u) /* deliberately oversized */
#define HOST_PLANE_SZ ((size_t)HOST_STRIDE)
#define HOST_BUF_SZ (3u * HOST_PLANE_SZ)

static char *test_host_xyb(void)
{
    host_xyb_fn_t fn = pick_host_xyb();
    if (!fn)
        return NULL;
    float *lin = calloc(HOST_BUF_SZ, sizeof(float));
    float *out_ref = calloc(HOST_BUF_SZ, sizeof(float));
    float *out_simd = calloc(HOST_BUF_SZ, sizeof(float));
    if (!lin || !out_ref || !out_simd) {
        free(lin);
        free(out_ref);
        free(out_simd);
        mu_assert("test_host_xyb: calloc failed", 0);
        return NULL;
    }
    /* Fill only the active pixel region of each plane. */
    for (int c = 0; c < 3; c++) {
        uint32_t seed = 0xdeadbeefu ^ (uint32_t)(c * 0x1234u);
        fill_random(lin + (size_t)c * HOST_PLANE_SZ, (size_t)HOST_W * HOST_H, 0.0f, 1.0f, seed);
    }
    ref_host_linear_rgb_to_xyb(lin, out_ref, HOST_W, HOST_H, HOST_PLANE_SZ);
    fn(lin, out_simd, HOST_W, HOST_H, HOST_PLANE_SZ);
    /* Compare only the active pixel region of each output plane — the
     * padding slots beyond w*h are not written by either implementation. */
    int match = 1;
    for (int c = 0; c < 3; c++) {
        // NOLINTNEXTLINE(bugprone-suspicious-memory-comparison,cert-exp42-c,cert-flp37-c) ADR-0242
        if (memcmp(out_ref + (size_t)c * HOST_PLANE_SZ, out_simd + (size_t)c * HOST_PLANE_SZ,
                   (size_t)HOST_W * HOST_H * sizeof(float)) != 0) {
            match = 0;
        }
    }
    free(lin);
    free(out_ref);
    free(out_simd);
    mu_assert("host_linear_rgb_to_xyb SIMD not bit-identical to scalar (plane_stride form)", match);
    return NULL;
}

static char *test_host_downsample(void)
{
    host_down_fn_t fn = pick_host_down();
    if (!fn)
        return NULL;
    const unsigned OW = (HOST_W + 1) / 2;
    const unsigned OH = (HOST_H + 1) / 2;
    float *in = calloc(HOST_BUF_SZ, sizeof(float));
    float *out_ref = calloc(HOST_BUF_SZ, sizeof(float));
    float *out_simd = calloc(HOST_BUF_SZ, sizeof(float));
    if (!in || !out_ref || !out_simd) {
        free(in);
        free(out_ref);
        free(out_simd);
        mu_assert("test_host_downsample: calloc failed", 0);
        return NULL;
    }
    for (int c = 0; c < 3; c++) {
        uint32_t seed = 0x5a5a5a5au ^ (uint32_t)(c * 0x789u);
        fill_random(in + (size_t)c * HOST_PLANE_SZ, (size_t)HOST_W * HOST_H, -1.0f, 1.0f, seed);
    }
    ref_host_downsample_2x2(in, HOST_W, HOST_H, out_ref, OW, OH, HOST_PLANE_SZ);
    fn(in, HOST_W, HOST_H, out_simd, OW, OH, HOST_PLANE_SZ);
    int match = 1;
    for (int c = 0; c < 3; c++) {
        // NOLINTNEXTLINE(bugprone-suspicious-memory-comparison,cert-exp42-c,cert-flp37-c) ADR-0242
        if (memcmp(out_ref + (size_t)c * HOST_PLANE_SZ, out_simd + (size_t)c * HOST_PLANE_SZ,
                   (size_t)OW * OH * sizeof(float)) != 0) {
            match = 0;
        }
    }
    free(in);
    free(out_ref);
    free(out_simd);
    mu_assert("host_downsample_2x2 SIMD not bit-identical to scalar (plane_stride form)", match);
    return NULL;
}

/* Flat mu_run_test list — one line per subtest by design, not a
 * complexity violation. */
// NOLINTNEXTLINE(readability-function-size,google-readability-function-size) — test scaffolding (ADR-0141)
char *run_tests(void)
{
#if ARCH_AARCH64
    /* T7-38 diagnostic: surface which aarch64 SIMD path the dispatcher
     * has selected so qemu / native runs can be told apart in CI logs.
     * Pure stderr — no test assertion depends on the value. */
    const unsigned arm_flags = vmaf_get_cpu_flags_arm();
    (void)fprintf(stderr, "ssimulacra2 simd dispatch: NEON=%u SVE2=%u\n",
                  (arm_flags & VMAF_ARM_CPU_FLAG_NEON) ? 1u : 0u,
#if HAVE_SVE2
                  (arm_flags & VMAF_ARM_CPU_FLAG_SVE2) ? 1u : 0u
#else
                  0u
#endif
    );
#endif
#if ARCH_X86 || ARCH_AARCH64
    mu_run_test(test_multiply);
    mu_run_test(test_xyb);
    mu_run_test(test_downsample);
    mu_run_test(test_ssim);
    mu_run_test(test_edge);
    mu_run_test(test_blur);
    mu_run_test(test_ptlr_420_8);
    mu_run_test(test_ptlr_420_10);
    mu_run_test(test_ptlr_444_8);
    mu_run_test(test_ptlr_444_10);
    mu_run_test(test_ptlr_422_8);
    /* ADR-0242: host-kernel tests (Vulkan pyramid plane_stride layout). */
    mu_run_test(test_host_xyb);
    mu_run_test(test_host_downsample);
#else
    (void)fprintf(stderr, "skipping: arch without ssimulacra2 SIMD\n");
#endif
    return NULL;
}
