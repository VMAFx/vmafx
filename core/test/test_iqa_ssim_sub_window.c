/**
 * Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 *
 * `iqa_ssim()` on planes smaller than, equal to and larger than its 11x11
 * Gaussian window, with every SIMD dispatch the host has against the scalar
 * path, bit for bit (T-FLOAT-SSIM-SUB-WINDOW-SIMD-COUNT-2026-10-02).
 *
 * A plane smaller than the window has no window. The convolution reports the
 * extents `w - 11 + 1` and `h - 11 + 1`, which are then non-positive, and the
 * scalar loops of `iqa/ssim_tools.c` visit nothing: the sums stay 0 and the
 * result is `0 / (w * h)`, as in Netflix's `iqa_ssim()`. The SIMD kernels
 * take one flat element count, and the dispatch site passed them the product
 * of the two extents, which is positive when both are negative: 4 for an 8x8
 * plane, 49 for 4x4, 81 for 2x2. They then read window statistics that no
 * convolution had written (the `float_ssim` of an 8x8 frame came out as 0.46
 * or 0.81, differently from run to run), and for a plane of 4x4 or less the
 * count exceeds the workspace, so the variance kernel wrote past it.
 *
 * The cases below hold the dispatch site to the scalar result at those sizes
 * (negative), at the sizes where exactly one extent or both are zero or one
 * is negative (boundary: the results are -0 and NaN, and must be the same
 * -0 and NaN), at 11x11 where there is exactly one window (boundary) and at
 * sizes with many windows (positive). The 8x8 case also pins the value
 * itself, 0 with a clear sign bit, which is what Netflix master returns.
 *
 * A dispatch whose instruction set the host lacks is skipped; on a host with
 * no SIMD dispatch at all the test reports a skip instead of a pass.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "test.h"

#include "cpu.h"
#include "feature/feature_collector.h"
#include "feature/feature_extractor.h"
#include "feature/iqa/convolve.h"
#include "feature/iqa/ssim_simd.h"
#include "feature/iqa/ssim_tools.h"
#include "libvmaf/picture.h"

#if ARCH_X86
#include "feature/x86/convolve_avx2.h"
#include "feature/x86/ssim_avx2.h"
#if HAVE_AVX512
#include "feature/x86/convolve_avx512.h"
#include "feature/x86/ssim_avx512.h"
#endif
#endif

#if ARCH_AARCH64
#include "feature/arm64/convolve_neon.h"
#include "feature/arm64/ssim_neon.h"
#endif

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe. ADR-1138. */

#define SUBW_MAX_SIDE 40
#define SUBW_MAX_PIXELS (SUBW_MAX_SIDE * SUBW_MAX_SIDE)

typedef struct SsimDispatch {
    const char *name;
    unsigned cpu_flag;
    ssim_precompute_fn precompute;
    ssim_variance_fn variance;
    ssim_accumulate_fn accumulate;
    iqa_convolve_fn convolve;
} SsimDispatch;

#if ARCH_X86 && HAVE_AVX512
static const SsimDispatch k_dispatches[] = {
    {"avx2", VMAF_X86_CPU_FLAG_AVX2, ssim_precompute_avx2, ssim_variance_avx2, ssim_accumulate_avx2,
     iqa_convolve_avx2},
    {"avx512", VMAF_X86_CPU_FLAG_AVX512, ssim_precompute_avx512, ssim_variance_avx512,
     ssim_accumulate_avx512, iqa_convolve_avx512},
    /* Sentinel: keeps the array non-empty on a host without a SIMD dispatch. */
    {NULL, 0u, NULL, NULL, NULL, NULL},
};
#elif ARCH_X86
static const SsimDispatch k_dispatches[] = {
    {"avx2", VMAF_X86_CPU_FLAG_AVX2, ssim_precompute_avx2, ssim_variance_avx2, ssim_accumulate_avx2,
     iqa_convolve_avx2},
    /* Sentinel: keeps the array non-empty on a host without a SIMD dispatch. */
    {NULL, 0u, NULL, NULL, NULL, NULL},
};
#elif ARCH_AARCH64
static const SsimDispatch k_dispatches[] = {
    {"neon", VMAF_ARM_CPU_FLAG_NEON, ssim_precompute_neon, ssim_variance_neon, ssim_accumulate_neon,
     iqa_convolve_neon},
    /* Sentinel: keeps the array non-empty on a host without a SIMD dispatch. */
    {NULL, 0u, NULL, NULL, NULL, NULL},
};
#else
static const SsimDispatch k_dispatches[] = {
    /* Sentinel: keeps the array non-empty on a host without a SIMD dispatch. */
    {NULL, 0u, NULL, NULL, NULL, NULL},
};
#endif
#define SUBW_NUM_DISPATCHES ((int)(sizeof(k_dispatches) / sizeof(k_dispatches[0])) - 1)

typedef struct PlaneSize {
    int w;
    int h;
} PlaneSize;

/* Both extents negative: the product of the extents is positive. */
static const PlaneSize k_below_window[] = {{1, 1}, {2, 2}, {4, 4}, {8, 8}, {9, 5}, {3, 10}};
/* One extent zero or negative, the other not: the product is zero or negative. */
static const PlaneSize k_mixed_extents[] = {{10, 10}, {8, 20}, {20, 8}, {10, 40}, {40, 10}};
/* At least one window. 11x11 is exactly one. */
static const PlaneSize k_with_windows[] = {{11, 11}, {11, 12}, {12, 11},
                                           {13, 17}, {32, 32}, {40, 23}};

#define SUBW_COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

typedef struct SsimResult {
    float score;
    float l;
    float c;
    float s;
} SsimResult;

static uint32_t float_bits(float v)
{
    uint32_t bits = 0u;
    memcpy(&bits, &v, sizeof(bits));
    return bits;
}

/* A fixed pseudo-random 8-bit plane: content with luminance, contrast and
 * structure differences, so a window that is visited contributes a value
 * that is neither 0 nor 1. */
static void fill_plane(float *plane, int n, uint32_t seed)
{
    uint32_t state = seed;
    for (int i = 0; i < n; i++) {
        state = state * 1664525u + 1013904223u;
        plane[i] = (float)((state >> 24) & 0xffu);
    }
}

static void gaussian_window(struct iqa_kernel *window)
{
    window->kernel = (float *)g_gaussian_window;
    window->kernel_h = (float *)g_gaussian_window_h;
    window->kernel_v = (float *)g_gaussian_window_v;
    window->w = GAUSSIAN_LEN;
    window->h = GAUSSIAN_LEN;
    window->normalized = 1;
    window->bnd_opt = KBND_SYMMETRIC;
    window->bnd_const = 0.0f;
}

/* iqa_ssim() over a fresh copy of the same two planes with the dispatch
 * installed by the caller. */
static SsimResult run_iqa_ssim(PlaneSize size)
{
    static float ref[SUBW_MAX_PIXELS];
    static float cmp[SUBW_MAX_PIXELS];
    fill_plane(ref, size.w * size.h, 0x1234u + (uint32_t)size.w);
    fill_plane(cmp, size.w * size.h, 0x9876u + (uint32_t)size.h);

    struct iqa_kernel window;
    gaussian_window(&window);

    SsimResult r = {0.0f, 0.0f, 0.0f, 0.0f};
    r.score = iqa_ssim(ref, cmp, size.w, size.h, &window, NULL, NULL, &r.l, &r.c, &r.s);
    return r;
}

static SsimResult run_scalar(PlaneSize size)
{
    iqa_ssim_set_dispatch(NULL, NULL, NULL);
    iqa_convolve_set_dispatch(NULL);
    return run_iqa_ssim(size);
}

static SsimResult run_dispatch(const SsimDispatch *d, PlaneSize size)
{
    iqa_ssim_set_dispatch(d->precompute, d->variance, d->accumulate);
    iqa_convolve_set_dispatch(d->convolve);
    const SsimResult r = run_iqa_ssim(size);
    iqa_ssim_set_dispatch(NULL, NULL, NULL);
    iqa_convolve_set_dispatch(NULL);
    return r;
}

static int same_bits(SsimResult a, SsimResult b)
{
    return float_bits(a.score) == float_bits(b.score) && float_bits(a.l) == float_bits(b.l) &&
           float_bits(a.c) == float_bits(b.c) && float_bits(a.s) == float_bits(b.s);
}

/* Every size of `sizes` on dispatch `d` against the scalar path. */
static char *check_sizes(const SsimDispatch *d, const PlaneSize *sizes, int count)
{
    for (int i = 0; i < count; i++) {
        const SsimResult want = run_scalar(sizes[i]);
        const SsimResult got = run_dispatch(d, sizes[i]);
        if (!same_bits(want, got)) {
            (void)fprintf(stderr,
                          "\n%s %dx%d: scalar %.9g (l %.9g c %.9g s %.9g), "
                          "dispatch %.9g (l %.9g c %.9g s %.9g)\n",
                          d->name, sizes[i].w, sizes[i].h, (double)want.score, (double)want.l,
                          (double)want.c, (double)want.s, (double)got.score, (double)got.l,
                          (double)got.c, (double)got.s);
            return "a SIMD dispatch of iqa_ssim differs from the scalar path";
        }
    }
    return NULL;
}

/* Runs `sizes` on every dispatch the host has; a host with none skips. */
static char *for_each_dispatch(const PlaneSize *sizes, int count)
{
    const unsigned flags = vmaf_get_cpu_flags();
    int ran = 0;
    for (int k = 0; k < SUBW_NUM_DISPATCHES; k++) {
        if (!(flags & k_dispatches[k].cpu_flag)) {
            (void)fprintf(stderr, "[skip %s: not supported by this CPU] ", k_dispatches[k].name);
            continue;
        }
        mu_assert_msg(check_sizes(&k_dispatches[k], sizes, count));
        ran++;
    }
    if (!ran) {
        mu_skipped = 1;
    }
    return NULL;
}

static char *test_plane_below_window_matches_scalar(void)
{
    return for_each_dispatch(k_below_window, SUBW_COUNT(k_below_window));
}

static char *test_plane_with_mixed_extents_matches_scalar(void)
{
    return for_each_dispatch(k_mixed_extents, SUBW_COUNT(k_mixed_extents));
}

static char *test_plane_with_windows_matches_scalar(void)
{
    return for_each_dispatch(k_with_windows, SUBW_COUNT(k_with_windows));
}

/* The scalar path is the reference the dispatches are held to; pin what it
 * returns so the comparison above cannot pass on two wrong answers. */
static char *test_scalar_reference_values(void)
{
    const PlaneSize below = {8, 8};
    const SsimResult r8 = run_scalar(below);
    mu_assert("scalar 8x8 score is +0", float_bits(r8.score) == 0u);
    mu_assert("scalar 8x8 l, c, s are +0",
              float_bits(r8.l) == 0u && float_bits(r8.c) == 0u && float_bits(r8.s) == 0u);

    const PlaneSize one = {11, 11};
    const SsimResult r11 = run_scalar(one);
    mu_assert("scalar 11x11 has one window with a score in (0, 1)",
              r11.score > 0.0f && r11.score < 1.0f);

    const PlaneSize none = {10, 10};
    const SsimResult r10 = run_scalar(none);
    mu_assert("scalar 10x10 divides 0 by 0 windows", isnan(r10.score));
    return NULL;
}

static char *fill_picture(VmafPicture *pic, uint32_t seed)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV400P, 8, 8u, 8u);
    mu_assert("picture alloc", err == 0);
    uint32_t state = seed;
    for (unsigned y = 0; y < pic->h[0]; y++) {
        uint8_t *row = (uint8_t *)pic->data[0] + (ptrdiff_t)y * pic->stride[0];
        for (unsigned x = 0; x < pic->w[0]; x++) {
            state = state * 1664525u + 1013904223u;
            row[x] = (uint8_t)(state >> 24);
        }
    }
    return NULL;
}

/* One frame of the extractor run: two random 8x8 pictures in, +0 out. */
static char *extract_8x8_frame(VmafFeatureExtractorContext *ctx, VmafFeatureCollector *fc,
                               unsigned index)
{
    VmafPicture ref;
    VmafPicture dist;
    mu_assert_msg(fill_picture(&ref, 0x51u + index));
    mu_assert_msg(fill_picture(&dist, 0xa7u + index));
    int err = vmaf_feature_extractor_context_extract(ctx, &ref, NULL, &dist, NULL, index, fc);
    const int ref_unref = vmaf_picture_unref(&ref);
    const int dist_unref = vmaf_picture_unref(&dist);
    mu_assert("extract ok", err == 0);
    mu_assert("picture unref", ref_unref == 0 && dist_unref == 0);
    double score = NAN;
    err = vmaf_feature_collector_get_score(fc, "float_ssim", &score, index);
    mu_assert("get float_ssim", err == 0);
    mu_assert("float_ssim of an 8x8 frame is 0", score == 0.0 && !signbit(score));
    return NULL;
}

/* An initialised `float_ssim` context for 8x8 8-bit 4:0:0 frames. */
static char *open_float_ssim_8x8(VmafFeatureExtractorContext **ctx)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("float_ssim");
    mu_assert("float_ssim extractor present", fex != NULL);
    int err = vmaf_feature_extractor_context_create(ctx, fex, NULL);
    mu_assert("context_create", err == 0);
    err = vmaf_feature_extractor_context_init(*ctx, VMAF_PIX_FMT_YUV400P, 8u, 8u, 8u);
    mu_assert("context_init", err == 0);
    return NULL;
}

/* The extractor itself, with the dispatch its init installs for this host:
 * `float_ssim` of an 8x8 frame is 0, on every frame of a run. */
static char *test_float_ssim_extractor_8x8_is_zero(void)
{
    VmafFeatureExtractorContext *ctx = NULL;
    mu_assert_msg(open_float_ssim_8x8(&ctx));
    VmafFeatureCollector *fc = NULL;
    const int err = vmaf_feature_collector_init(&fc);
    mu_assert("collector_init", err == 0);

    char *failure = NULL;
    for (unsigned index = 0; index < 4u && failure == NULL; index++) {
        failure = extract_8x8_frame(ctx, fc, index);
    }

    const int closed = vmaf_feature_extractor_context_close(ctx);
    const int destroyed = vmaf_feature_extractor_context_destroy(ctx);
    vmaf_feature_collector_destroy(fc);
    mu_assert_msg(failure);
    mu_assert("context close and destroy", closed == 0 && destroyed == 0);
    return NULL;
}

char *run_tests(void)
{
    /* vmaf_get_cpu_flags() is 0 until the probe has run; the library runs it
     * in vmaf_init(), which a test of the extractor layer never calls. */
    vmaf_init_cpu();
    /* First: it installs the host dispatch through the extractor's
     * once-guard, which the cases after it replace and clear by hand. */
    mu_run_test(test_float_ssim_extractor_8x8_is_zero);
    mu_run_test(test_scalar_reference_values);
    mu_run_test(test_plane_below_window_matches_scalar);
    mu_run_test(test_plane_with_mixed_extents_matches_scalar);
    mu_run_test(test_plane_with_windows_matches_scalar);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
