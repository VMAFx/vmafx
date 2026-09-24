/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Bit-exactness contract (ADR-0138, ADR-0140): the SIMD `iqa_convolve_*`
 * variants — AVX2, AVX-512, and NEON — produce byte-for-byte the same
 * output as the scalar reference `iqa_convolve` under IQA_CONVOLVE_1D,
 * for any (src, w, h, kernel) in the SSIM/MS-SSIM supported space:
 *   - 11-tap Gaussian (odd kernel, kw_even == 0)
 *   - 8-tap box       (even kernel, kw_even == 1)
 *   - image dimensions where `w >= kw` and `h >= kh`
 *
 * Coverage:
 *   - SIMD inner region (1920x1080, 576x324).
 *   - Tail sizes that hit the masked 4-lane AVX2 tail (1..3 cols) and
 *     the masked 8-lane AVX-512 tail (1..7 cols).
 *   - Odd dimensions (33x17, 61x41) to exercise off-stride tails.
 *   - Minimum-size cases equal to the kernel footprint (11x11, 8x8).
 *
 * The assertion is strict byte-equality via memcmp.
 */

#include <float.h>
#include <math.h>
#include <stdint.h>

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe, and this file mirrors
 * the C spelling of the surface it exercises. ADR-1138. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "feature/ms_ssim_decimate.h"
#include "feature/picture_copy.h"
#include "feature/pu21_math.h"
#include "feature/iqa/convolve.h"
#include "feature/iqa/ssim_tools.h"
#include "libvmaf/picture.h"
#if ARCH_X86
#include "feature/x86/convolve_avx2.h"
#if HAVE_AVX512
#include "feature/x86/convolve_avx512.h"
#endif
#endif
#if ARCH_AARCH64
#include "cpu.h"
#include "feature/arm64/convolve_neon.h"
#endif
#include "test.h"
#if ARCH_X86
#include "x86/cpu.h"
#endif

#if ARCH_X86
static int g_has_avx2 = 0;
static int g_has_avx512 = 0;
#endif
#if ARCH_AARCH64
static int g_has_neon = 0;
#endif

/* 11-tap Gaussian — matches g_gaussian_window_{h,v} in ssim_tools.h. */
static const float kernel_gauss11[11] = {0.001028f, 0.007599f, 0.036001f, 0.109361f,
                                         0.213006f, 0.266012f, 0.213006f, 0.109361f,
                                         0.036001f, 0.007599f, 0.001028f};

/* 8-tap box — matches g_square_window_{h,v} in ssim_tools.h. */
static const float kernel_box8[8] = {0.125f, 0.125f, 0.125f, 0.125f,
                                     0.125f, 0.125f, 0.125f, 0.125f};

/* Production-domain bounds for CodeQL alert 1005. `picture_copy()` keeps
 * supported integer samples below 2^8. Four signed MS-SSIM decimations each
 * have induced L-infinity gain below 2, so the last pyramid level is below
 * 2^12 and its squared/cross terms are below 2^24. PU21 stays below 2^10,
 * making its squared/cross terms smaller still. Research-2031. */
#define SSIM_SAMPLE_BOUND 0x1p8f
#define MS_SSIM_LEVEL_BOUND 0x1p12f
#define MS_SSIM_STATS_BOUND 0x1p24f
#define PU21_SAMPLE_BOUND 0x1p10

/* Deterministic pseudo-random fill — reproducible across runs. */
static void fill_pattern(float *buf, size_t n, uint32_t seed)
{
    uint32_t state = seed;
    for (size_t i = 0; i < n; ++i) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        buf[i] = ((float)(int32_t)state) / (float)INT32_MAX;
    }
}

static int copy_max_sample(unsigned bpc, float *sample)
{
    VmafPicture pic = {0};
    const int err = vmaf_picture_alloc(&pic, VMAF_PIX_FMT_YUV400P, bpc, 1, 1);
    if (err)
        return err;

    if (bpc == 8U) {
        *(uint8_t *)pic.data[0] = UINT8_MAX;
    } else {
        *(uint16_t *)pic.data[0] = (uint16_t)((1U << bpc) - 1U);
    }
    picture_copy(sample, (ptrdiff_t)sizeof(*sample), &pic, 0, bpc, 0);
    return vmaf_picture_unref(&pic);
}

static char *test_picture_copy_sample_bound(void)
{
    static const unsigned bpcs[] = {8U, 10U, 12U, 16U};
    for (size_t i = 0; i < sizeof(bpcs) / sizeof(bpcs[0]); ++i) {
        float sample = 0.0f;
        mu_assert("picture_copy fixture failed", copy_max_sample(bpcs[i], &sample) == 0);
        mu_assert("picture_copy sample must be finite", isfinite(sample));
        mu_assert("picture_copy sample must be in [0, 2^8)",
                  sample >= 0.0f && sample < SSIM_SAMPLE_BOUND);
    }
    return NULL;
}

/* Derive the production 9x9 decimator's effective interior impulse response
 * through its real scalar implementation. Its L1 norm bounds L-infinity gain;
 * the signed 9/7 coefficients overshoot unity, but remain strictly below 2. */
static char *test_ms_ssim_decimate_gain_bound(void)
{
    enum { SRC_SIDE = 17, DST_SIDE = 9, HALF = 4 };
    float src[SRC_SIDE * SRC_SIDE];
    float dst[DST_SIDE * DST_SIDE];
    double impulse_l1 = 0.0;
    for (int y = HALF; y < SRC_SIDE - HALF; ++y) {
        for (int x = HALF; x < SRC_SIDE - HALF; ++x) {
            memset(src, 0, sizeof(src));
            src[y * SRC_SIDE + x] = 1.0f;
            int rw = 0;
            int rh = 0;
            mu_assert("ms_ssim_decimate_scalar failed",
                      ms_ssim_decimate_scalar(src, SRC_SIDE, SRC_SIDE, dst, &rw, &rh) == 0);
            mu_assert("ms_ssim_decimate_scalar geometry changed", rw == DST_SIDE && rh == DST_SIDE);
            impulse_l1 += fabs((double)dst[HALF * DST_SIDE + HALF]);
        }
    }
    mu_assert("signed MS-SSIM filter should have gain above unity", impulse_l1 > 1.0);
    mu_assert("MS-SSIM decimator gain must stay below 2", impulse_l1 < 2.0);
    return NULL;
}

static char *test_pu21_sample_bound(void)
{
    for (int variant = 0; variant < PU21_VARIANT_COUNT; ++variant) {
        const double *p = pu21_params[variant];
        mu_assert("PU21 coefficients must keep the rational term monotone",
                  p[0] > 0.0 && p[1] > 0.0 && p[1] > p[0] * p[2] && p[2] >= 0.0);
        mu_assert("PU21 exponents and scale must be positive",
                  p[3] > 0.0 && p[4] > 0.0 && p[6] > 0.0);
        const double endpoint = pu21_encode(PU21_L_MAX, p);
        mu_assert("PU21 endpoint must be finite", isfinite(endpoint));
        mu_assert("PU21 production samples must stay in [0, 2^10)",
                  endpoint >= 0.0 && endpoint < PU21_SAMPLE_BOUND);
    }
    return NULL;
}

static int kernel_is_subunit(const float *kernel, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        if (fabsf(kernel[i]) > 1.0f)
            return 0;
    }
    return 1;
}

static int kernels_equal(const float *actual, const float *expected, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        if (actual[i] != expected[i])
            return 0;
    }
    return 1;
}

static char *test_convolve_kernel_bound(void)
{
    mu_assert("test Gaussian horizontal taps drifted from production",
              kernels_equal(kernel_gauss11, g_gaussian_window_h,
                            sizeof(kernel_gauss11) / sizeof(kernel_gauss11[0])));
    mu_assert("test Gaussian vertical taps drifted from production",
              kernels_equal(kernel_gauss11, g_gaussian_window_v,
                            sizeof(kernel_gauss11) / sizeof(kernel_gauss11[0])));
    mu_assert("test box horizontal taps drifted from production",
              kernels_equal(kernel_box8, g_square_window_h,
                            sizeof(kernel_box8) / sizeof(kernel_box8[0])));
    mu_assert("test box vertical taps drifted from production",
              kernels_equal(kernel_box8, g_square_window_v,
                            sizeof(kernel_box8) / sizeof(kernel_box8[0])));
    mu_assert(
        "Gaussian taps must stay at or below unity",
        kernel_is_subunit(kernel_gauss11, sizeof(kernel_gauss11) / sizeof(kernel_gauss11[0])));
    mu_assert("box taps must stay at or below unity",
              kernel_is_subunit(kernel_box8, sizeof(kernel_box8) / sizeof(kernel_box8[0])));
    mu_assert("2D production taps must stay below unity",
              fabsf(g_gaussian_window[5][5]) < 1.0f && fabsf(g_square_window[0][0]) < 1.0f);
    return NULL;
}

static char *test_convolve_product_bound(void)
{
    mu_assert("four MS-SSIM gain bounds must map 2^8 to 2^12",
              ldexpf(SSIM_SAMPLE_BOUND, SCALES - 1) == MS_SSIM_LEVEL_BOUND);
    mu_assert("MS-SSIM squared/cross bound must be 2^24",
              MS_SSIM_LEVEL_BOUND * MS_SSIM_LEVEL_BOUND == MS_SSIM_STATS_BOUND);
    mu_assert("PU21 stats must fit inside the MS-SSIM stats bound",
              PU21_SAMPLE_BOUND * PU21_SAMPLE_BOUND <= MS_SSIM_STATS_BOUND);

    /* Even if every one of eleven horizontal products reached 2^24, the
     * float cache remains below 2^28. The flagged vertical float multiply has
     * a <=1 tap, leaving a 100-binary-exponent margin below FLT_MAX. */
    const float flagged_product_bound = 0x1p28f;
    mu_assert("eleven worst-case horizontal terms must fit below 2^28",
              11.0 * (double)MS_SSIM_STATS_BOUND < (double)flagged_product_bound);
    mu_assert("float format lacks the required exponent range", FLT_MAX_EXP > 28);
    mu_assert("flagged product bound must be finite",
              isfinite(flagged_product_bound) && flagged_product_bound < FLT_MAX);
    return NULL;
}

#if ARCH_X86 || ARCH_AARCH64

/* Only referenced from check_simd_variant() below. */
static int compare_bitexact(const float *a, const float *b, size_t n)
{
    return memcmp(a, b, n * sizeof(float)) == 0;
}

typedef void (*convolve_simd_fn)(float *img, int w, int h, const float *kernel_h,
                                 const float *kernel_v, int kw, int kh, int normalized,
                                 float *workspace, float *result, int *rw, int *rh);

// test exits process on failure path via mu_assert; analyzer can't see
// exit; small allocations leak by design at test end. Test scaffolding
// per ADR-0141 §2; ADR-0278 cite form.
// NOLINTBEGIN(clang-analyzer-unix.Malloc) — ADR-0141 / ADR-0278
static char *check_simd_variant(const float *src, int w, int h, const float *kernel_h,
                                const float *kernel_v, int kw, int kh, const float *dst_scalar,
                                size_t dst_n, convolve_simd_fn fn, int poison, char *fail_cmp)
{
    float *src_copy = (float *)malloc((size_t)w * (size_t)h * sizeof(float));
    float *dst = (float *)malloc(dst_n * sizeof(float));
    float *workspace = (float *)malloc((size_t)w * (size_t)h * sizeof(float));
    mu_assert("simd malloc failed", src_copy && dst && workspace);
    memcpy(src_copy, src, (size_t)w * (size_t)h * sizeof(float));
    memset(dst, poison, dst_n * sizeof(float));

    int rw = 0;
    int rh = 0;
    fn(src_copy, w, h, kernel_h, kernel_v, kw, kh, /*normalized=*/1, workspace, dst, &rw, &rh);
    mu_assert("simd rw mismatch", rw == w - kw + 1);
    mu_assert("simd rh mismatch", rh == h - kh + 1);
    mu_assert(fail_cmp, compare_bitexact(dst_scalar, dst, dst_n));

    free(src_copy);
    free(dst);
    free(workspace);
    return NULL;
}
// NOLINTEND(clang-analyzer-unix.Malloc)
#endif /* ARCH_X86 || ARCH_AARCH64 */

/* Run the scalar reference convolve into `dst_scalar`. Mutates a
 * disposable copy of `src` because `iqa_convolve` may touch its
 * input; the caller's `src` buffer is preserved for the SIMD passes. */
static char *run_scalar_reference(const float *src, int w, int h, int kw, const float *kernel_h,
                                  const float *kernel_v, size_t src_n, float *dst_scalar)
{
    struct iqa_kernel k;
    k.kernel = NULL;
    k.kernel_h = (float *)kernel_h;
    k.kernel_v = (float *)kernel_v;
    k.w = kw;
    k.h = kw;
    k.normalized = 1;
    k.bnd_opt = NULL;
    k.bnd_const = 0.0f;

    float *src_scalar_copy = (float *)malloc(src_n * sizeof(float));
    mu_assert("malloc failed", src_scalar_copy != NULL);
    memcpy(src_scalar_copy, src, src_n * sizeof(float));
    iqa_convolve(src_scalar_copy, w, h, &k, dst_scalar, NULL, NULL);
    free(src_scalar_copy);
    return NULL;
}

/* Run all configured SIMD convolves for this host and bit-compare
 * each against `dst_scalar`. Returns NULL on success or the first
 * variant's mismatch message. */
static char *check_all_simd_variants(const float *src, int w, int h, int kw, const float *kernel_h,
                                     const float *kernel_v, const float *dst_scalar, size_t dst_n)
{
#if ARCH_X86
    if (g_has_avx2) {
        char *msg = check_simd_variant(src, w, h, kernel_h, kernel_v, kw, kw, dst_scalar, dst_n,
                                       iqa_convolve_avx2, 0x55,
                                       "avx2 convolve output not bit-identical to scalar");
        if (msg)
            return msg;
    }
#if HAVE_AVX512
    if (g_has_avx512) {
        char *msg = check_simd_variant(src, w, h, kernel_h, kernel_v, kw, kw, dst_scalar, dst_n,
                                       iqa_convolve_avx512, 0x33,
                                       "avx512 convolve output not bit-identical to scalar");
        if (msg)
            return msg;
    }
#endif
#endif
#if ARCH_AARCH64
    if (g_has_neon) {
        char *msg = check_simd_variant(src, w, h, kernel_h, kernel_v, kw, kw, dst_scalar, dst_n,
                                       iqa_convolve_neon, 0x77,
                                       "neon convolve output not bit-identical to scalar");
        if (msg)
            return msg;
    }
#endif
    (void)src;
    (void)w;
    (void)h;
    (void)kw;
    (void)kernel_h;
    (void)kernel_v;
    (void)dst_scalar;
    (void)dst_n;
    return NULL;
}

// test exits process on failure path via mu_assert; analyzer can't see
// exit; small allocations leak by design at test end. Test scaffolding
// per ADR-0141 §2; ADR-0278 cite form.
// NOLINTBEGIN(clang-analyzer-unix.Malloc) — ADR-0141 / ADR-0278
static char *check_case(int w, int h, int kw, const float *kernel_h, const float *kernel_v,
                        uint32_t seed)
{
    const int dst_w = w - kw + 1;
    const int dst_h = h - kw + 1;
    const size_t src_n = (size_t)w * (size_t)h;
    const size_t dst_n = (size_t)dst_w * (size_t)dst_h;

    float *src = (float *)malloc(src_n * sizeof(float));
    float *dst_scalar = (float *)malloc(dst_n * sizeof(float));
    mu_assert("malloc failed", src && dst_scalar);
    fill_pattern(src, src_n, seed);
    memset(dst_scalar, 0xAA, dst_n * sizeof(float));
    (void)dst_h;

    char *msg = run_scalar_reference(src, w, h, kw, kernel_h, kernel_v, src_n, dst_scalar);
    if (!msg)
        msg = check_all_simd_variants(src, w, h, kw, kernel_h, kernel_v, dst_scalar, dst_n);

    free(src);
    free(dst_scalar);
    return msg;
}
// NOLINTEND(clang-analyzer-unix.Malloc)

/* Exercise the exact alert-1005 expression at the largest production-domain
 * magnitude and require the supported implementation to stay finite and
 * byte-identical across scalar/SIMD paths. The separate Gaussian fixtures are
 * the red-capable detector for a pre-widening mutation. ADR-0138 / Research-2031. */
static char *test_domain_ceiling_convolve(void)
{
    enum { SIDE = 12, KERNEL_SIDE = 11, DST_SIDE = 2 };
    float src[SIDE * SIDE];
    float dst_scalar[DST_SIDE * DST_SIDE];
    const size_t src_n = (size_t)SIDE * (size_t)SIDE;
    const size_t dst_n = (size_t)DST_SIDE * (size_t)DST_SIDE;
    fill_pattern(src, src_n, 0x1005c0deU);
    const float ceiling = nextafterf(MS_SSIM_STATS_BOUND, 0.0f);
    for (size_t i = 0; i < sizeof(src) / sizeof(src[0]); ++i)
        src[i] *= ceiling;

    char *msg = run_scalar_reference(src, SIDE, SIDE, KERNEL_SIDE, kernel_gauss11, kernel_gauss11,
                                     src_n, dst_scalar);
    if (msg)
        return msg;
    for (size_t i = 0; i < sizeof(dst_scalar) / sizeof(dst_scalar[0]); ++i)
        mu_assert("domain-ceiling convolve output must stay finite", isfinite(dst_scalar[i]));
    return check_all_simd_variants(src, SIDE, SIDE, KERNEL_SIDE, kernel_gauss11, kernel_gauss11,
                                   dst_scalar, dst_n);
}

/* Gaussian (11-tap, kw_even=0) cases. */
static char *test_gauss_11x11(void)
{
    return check_case(11, 11, 11, kernel_gauss11, kernel_gauss11, 0x11111111u);
}
static char *test_gauss_12x12(void)
{
    return check_case(12, 12, 11, kernel_gauss11, kernel_gauss11, 0x22222222u);
}
/* AVX-512 tail sizes 1..7: dst_w = w - 10; we want dst_w % 8 ∈ {1..7}. */
static char *test_gauss_19x19(void) /* dst_w=9 -> tail 1 (8-lane) / 1 (4-lane) */
{
    return check_case(19, 19, 11, kernel_gauss11, kernel_gauss11, 0x33333333u);
}
static char *test_gauss_20x20(void) /* dst_w=10 -> tail 2 */
{
    return check_case(20, 20, 11, kernel_gauss11, kernel_gauss11, 0x44444444u);
}
static char *test_gauss_25x25(void) /* dst_w=15 -> tail 7 (8-lane) / 3 (4-lane) */
{
    return check_case(25, 25, 11, kernel_gauss11, kernel_gauss11, 0x55555555u);
}
static char *test_gauss_33x17(void) /* odd, dst_w=23 -> tail 7 / 3 */
{
    return check_case(33, 17, 11, kernel_gauss11, kernel_gauss11, 0x66666666u);
}
static char *test_gauss_61x41(void)
{
    return check_case(61, 41, 11, kernel_gauss11, kernel_gauss11, 0x77777777u);
}
static char *test_gauss_576x324(void)
{
    return check_case(576, 324, 11, kernel_gauss11, kernel_gauss11, 0x11112222u);
}
static char *test_gauss_1920x1080(void)
{
    return check_case(1920, 1080, 11, kernel_gauss11, kernel_gauss11, 0x33334444u);
}

/* Box (8-tap, kw_even=1) cases. */
static char *test_box_8x8(void)
{
    return check_case(8, 8, 8, kernel_box8, kernel_box8, 0x88888888u);
}
static char *test_box_16x16(void)
{
    return check_case(16, 16, 8, kernel_box8, kernel_box8, 0x99999999u);
}
static char *test_box_21x13(void) /* dst_w=14 -> tail 6/2 */
{
    return check_case(21, 13, 8, kernel_box8, kernel_box8, 0xaaaaaaaau);
}
static char *test_box_576x324(void)
{
    return check_case(576, 324, 8, kernel_box8, kernel_box8, 0xbbbbbbbbu);
}

/* Detect the SIMD features available on this host. Returns 1 if any
 * variant is runnable (test suite proceeds), 0 otherwise (skip). */
static int detect_simd_support(void)
{
#if ARCH_X86
    const unsigned cpu_flags = vmaf_get_cpu_flags_x86();
    g_has_avx2 = (cpu_flags & VMAF_X86_CPU_FLAG_AVX2) ? 1 : 0;
    g_has_avx512 = (cpu_flags & VMAF_X86_CPU_FLAG_AVX512) ? 1 : 0;
    if (!g_has_avx2 && !g_has_avx512) {
        (void)fprintf(stderr, "skipping: CPU has neither AVX2 nor AVX-512\n");
        return 0;
    }
    return 1;
#elif ARCH_AARCH64
    const unsigned cpu_flags = vmaf_get_cpu_flags();
    g_has_neon = (cpu_flags & VMAF_ARM_CPU_FLAG_NEON) ? 1 : 0;
    if (!g_has_neon) {
        (void)fprintf(stderr, "skipping: aarch64 CPU lacks NEON\n");
        return 0;
    }
    return 1;
#else
    (void)fprintf(stderr, "skipping: non-x86, non-aarch64 arch\n");
    return 0;
#endif
}

/* Small (<=25 px/side) Gaussian cases: kernel-footprint minimums and
 * masked-tail lane counts. */
static char *run_gauss_small_tests(void)
{
    mu_run_test(test_gauss_11x11);
    mu_run_test(test_gauss_12x12);
    mu_run_test(test_gauss_19x19);
    mu_run_test(test_gauss_20x20);
    mu_run_test(test_gauss_25x25);
    return NULL;
}

/* Medium-and-large Gaussian cases: odd-stride tails plus the two
 * production-equivalent resolutions. */
static char *run_gauss_large_tests(void)
{
    mu_run_test(test_gauss_33x17);
    mu_run_test(test_gauss_61x41);
    mu_run_test(test_gauss_576x324);
    mu_run_test(test_gauss_1920x1080);
    return NULL;
}

static char *run_gauss_tests(void)
{
    char *msg = run_gauss_small_tests();
    if (msg)
        return msg;
    return run_gauss_large_tests();
}

static char *run_box_tests(void)
{
    mu_run_test(test_box_8x8);
    mu_run_test(test_box_16x16);
    mu_run_test(test_box_21x13);
    mu_run_test(test_box_576x324);
    return NULL;
}

static char *run_domain_bound_tests(void)
{
    mu_run_test(test_picture_copy_sample_bound);
    mu_run_test(test_ms_ssim_decimate_gain_bound);
    mu_run_test(test_pu21_sample_bound);
    mu_run_test(test_convolve_kernel_bound);
    mu_run_test(test_convolve_product_bound);
    return NULL;
}

char *run_tests(void)
{
    char *msg = run_domain_bound_tests();
    if (msg)
        return msg;
    const int has_simd = detect_simd_support();
    mu_run_test(test_domain_ceiling_convolve);
    if (!has_simd)
        return NULL;
    msg = run_gauss_tests();
    if (msg)
        return msg;
    return run_box_tests();
}

/* NOLINTEND(modernize-use-nullptr) */
