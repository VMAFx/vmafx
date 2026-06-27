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
 * Bit-exactness contract for the float-ADM NEON DWT2 kernel (ADR-1057).
 *
 * `float_adm_dwt2_neon` (core/src/feature/arm64/float_adm_dwt2_neon.c) is the
 * FMA-safe replacement for the original `vmlaq_laneq_f32`-based NEON DWT2.
 * The original kernel emitted AArch64 `fmla` (fused multiply-add, single
 * rounding); the scalar reference `adm_dwt2_s` (core/src/feature/adm_tools.c)
 * uses separate multiply-then-add (two roundings). That divergence produced a
 * 1-ULP gap on ARM CI (PR #685 -> reverted by PR #695). The fix replaces every
 * `vmlaq_laneq_f32` with an explicit `vmulq_laneq_f32` + `vaddq_f32` pair, and
 * the kernel's translation unit is compiled with `-ffp-contract=off`
 * (`arm64_adm_dwt2_neon_lib` carve-out in core/src/meson.build) plus
 * `#pragma clang fp contract(off)` / GCC `optimize("-ffp-contract=off")`
 * guards so the scalar tail is not auto-fused into `fmla`.
 *
 * This test asserts that the NEON DWT2 output is *byte-for-byte* identical to
 * the scalar reference across a range of dimensions (the SIMD inner region,
 * borders narrower than the 4-tap kernel, and odd width/height that exercise
 * the (w/h + 1)/2 rounding paths and the NEON 4-wide vertical tail). A single
 * ULP difference fails the test via memcmp.
 *
 * This is the load-bearing follow-up test that ADR-1057 §Consequences calls
 * for. On non-aarch64 hosts the NEON kernel does not exist, so the test only
 * exercises the scalar reference and reports a skip for the NEON comparison.
 *
 * Adversarial property: keep this kernel FMA-free. If a future edit
 * reintroduces a `vmlaq*`/`vfmaq*` intrinsic or drops the `-ffp-contract=off`
 * carve-out, this test goes red.
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "feature/adm_tools.h"
#if ARCH_AARCH64
#include "cpu.h"
#include "feature/arm64/float_adm_neon.h"
#endif
#include "test.h"
#include "simd_bitexact_test.h"

/* Each DWT2 band output is (h + 1) / 2 rows by (w + 1) / 2 columns. We over-
 * allocate the column stride to `(w + 1) / 2` floats; the kernels write exactly
 * `[i * dst_px_stride + j]` for i in [0, (h+1)/2), j in [0, (w+1)/2). */
typedef struct dwt_fixture {
    int w;
    int h;
    int half_w; /* (w + 1) / 2 */
    int half_h; /* (h + 1) / 2 */
    int src_stride_bytes;
    int dst_stride_bytes;
    float *src;
    int *ind_y[4];
    int *ind_x[4];
    /* Scalar reference bands (band_a/h/v/d) and NEON candidate bands. */
    float *ref_a, *ref_h, *ref_v, *ref_d;
    float *neon_a, *neon_h, *neon_v, *neon_d;
} dwt_fixture;

static void band_bind(adm_dwt_band_t_s *band, float *a, float *h, float *v, float *d)
{
    band->band_a = a;
    band->band_h = h;
    band->band_v = v;
    band->band_d = d;
}

static void fixture_free(dwt_fixture *fx)
{
    if (!fx)
        return;
    simd_test_aligned_free(fx->src);
    for (int k = 0; k < 4; ++k) {
        simd_test_aligned_free(fx->ind_y[k]);
        simd_test_aligned_free(fx->ind_x[k]);
    }
    simd_test_aligned_free(fx->ref_a);
    simd_test_aligned_free(fx->ref_h);
    simd_test_aligned_free(fx->ref_v);
    simd_test_aligned_free(fx->ref_d);
    simd_test_aligned_free(fx->neon_a);
    simd_test_aligned_free(fx->neon_h);
    simd_test_aligned_free(fx->neon_v);
    simd_test_aligned_free(fx->neon_d);
}

/* Returns 0 on success, -1 on allocation failure (caller asserts). */
static int fixture_alloc(dwt_fixture *fx, int w, int h, uint32_t seed)
{
    memset(fx, 0, sizeof(*fx));
    fx->w = w;
    fx->h = h;
    fx->half_w = (w + 1) / 2;
    fx->half_h = (h + 1) / 2;
    /* The DWT2 kernels read `src_stride / sizeof(float)` as the row pitch and
     * `dst_stride / sizeof(float)` as the band pitch. Use packed strides. */
    fx->src_stride_bytes = (int)(w * sizeof(float));
    fx->dst_stride_bytes = (int)(fx->half_w * sizeof(float));

    const size_t src_n = (size_t)w * (size_t)h;
    const size_t band_n = (size_t)fx->half_w * (size_t)fx->half_h;

    fx->src = (float *)simd_test_aligned_malloc(src_n * sizeof(float), 64);
    fx->ref_a = (float *)simd_test_aligned_malloc(band_n * sizeof(float), 64);
    fx->ref_h = (float *)simd_test_aligned_malloc(band_n * sizeof(float), 64);
    fx->ref_v = (float *)simd_test_aligned_malloc(band_n * sizeof(float), 64);
    fx->ref_d = (float *)simd_test_aligned_malloc(band_n * sizeof(float), 64);
    fx->neon_a = (float *)simd_test_aligned_malloc(band_n * sizeof(float), 64);
    fx->neon_h = (float *)simd_test_aligned_malloc(band_n * sizeof(float), 64);
    fx->neon_v = (float *)simd_test_aligned_malloc(band_n * sizeof(float), 64);
    fx->neon_d = (float *)simd_test_aligned_malloc(band_n * sizeof(float), 64);
    if (!fx->src || !fx->ref_a || !fx->ref_h || !fx->ref_v || !fx->ref_d || !fx->neon_a ||
        !fx->neon_h || !fx->neon_v || !fx->neon_d) {
        return -1;
    }

    for (int k = 0; k < 4; ++k) {
        fx->ind_y[k] = (int *)simd_test_aligned_malloc((size_t)fx->half_h * sizeof(int), 64);
        fx->ind_x[k] = (int *)simd_test_aligned_malloc((size_t)fx->half_w * sizeof(int), 64);
        if (!fx->ind_y[k] || !fx->ind_x[k]) {
            return -1;
        }
    }

    simd_test_fill_random_f32(fx->src, src_n, -1.0f, 1.0f, seed);

    /* Poison the band buffers with distinct patterns so that a kernel which
     * fails to write a cell does not accidentally match. */
    memset(fx->ref_a, 0xAA, band_n * sizeof(float));
    memset(fx->ref_h, 0xAA, band_n * sizeof(float));
    memset(fx->ref_v, 0xAA, band_n * sizeof(float));
    memset(fx->ref_d, 0xAA, band_n * sizeof(float));
    memset(fx->neon_a, 0x55, band_n * sizeof(float));
    memset(fx->neon_h, 0x55, band_n * sizeof(float));
    memset(fx->neon_v, 0x55, band_n * sizeof(float));
    memset(fx->neon_d, 0x55, band_n * sizeof(float));

    /* Compute the shared reflect-index buffers once (identical for both
     * kernels — they consume `ind_y`/`ind_x`, they do not compute them). */
    dwt2_src_indices_filt_s(fx->ind_y, fx->ind_x, w, h);
    return 0;
}

static char *check_case(int w, int h, uint32_t seed)
{
    dwt_fixture fx;
    const int alloc_rc = fixture_alloc(&fx, w, h, seed);
    if (alloc_rc != 0) {
        fixture_free(&fx);
        return "fixture allocation failed";
    }

    adm_dwt_band_t_s ref_band;
    adm_dwt_band_t_s neon_band;
    band_bind(&ref_band, fx.ref_a, fx.ref_h, fx.ref_v, fx.ref_d);
    band_bind(&neon_band, fx.neon_a, fx.neon_h, fx.neon_v, fx.neon_d);

    const int rc_scalar = adm_dwt2_s(fx.src, &ref_band, fx.ind_y, fx.ind_x, w, h,
                                     fx.src_stride_bytes, fx.dst_stride_bytes);

    char *msg = NULL;
    if (rc_scalar != 0) {
        msg = "scalar adm_dwt2_s failed";
        goto done;
    }

#if ARCH_AARCH64
    if (vmaf_get_cpu_flags() & VMAF_ARM_CPU_FLAG_NEON) {
        /* The NEON kernel returns void (it silently no-ops on OOM, mirroring
         * the AVX-512 sibling). For the small fixtures here the internal
         * `aligned_malloc(w floats)` never fails, so a no-op would leave the
         * 0x55 poison in place and fail the memcmp below — which is the
         * desired behaviour (a silent no-op is a regression). */
        float_adm_dwt2_neon(fx.src, &neon_band, fx.ind_y, fx.ind_x, w, h, fx.src_stride_bytes,
                            fx.dst_stride_bytes);

        const size_t band_bytes = (size_t)fx.half_w * (size_t)fx.half_h * sizeof(float);
        if (memcmp(fx.ref_a, fx.neon_a, band_bytes) != 0) {
            msg = "NEON band_a not bit-identical to scalar (FMA contract broken?)";
            goto done;
        }
        if (memcmp(fx.ref_h, fx.neon_h, band_bytes) != 0) {
            msg = "NEON band_h not bit-identical to scalar (FMA contract broken?)";
            goto done;
        }
        if (memcmp(fx.ref_v, fx.neon_v, band_bytes) != 0) {
            msg = "NEON band_v not bit-identical to scalar (FMA contract broken?)";
            goto done;
        }
        if (memcmp(fx.ref_d, fx.neon_d, band_bytes) != 0) {
            msg = "NEON band_d not bit-identical to scalar (FMA contract broken?)";
            goto done;
        }
    }
#endif

done:
    fixture_free(&fx);
    return msg;
}

/* Tiny — narrower than the 4-tap kernel; all-scalar tail in the NEON path. */
static char *test_2x2(void)
{
    return check_case(2, 2, 0x12345678u);
}
static char *test_3x3(void)
{
    return check_case(3, 3, 0x87654321u);
}
/* Exactly one NEON 4-wide vector, no tail. */
static char *test_4x4(void)
{
    return check_case(4, 4, 0xdeadbeefu);
}
/* One vector + a 1-column scalar tail (the FMA-prone tail path). */
static char *test_5x5(void)
{
    return check_case(5, 5, 0xcafebabeu);
}
/* Odd width/height exercise the (w/h + 1)/2 rounding + reflect indices. */
static char *test_7x9(void)
{
    return check_case(7, 9, 0x00bab10cu);
}
static char *test_15x17(void)
{
    return check_case(15, 17, 0xfeedfaceu);
}
static char *test_64x64(void)
{
    return check_case(64, 64, 0x55556666u);
}
/* First ADM scale of the Netflix golden 576x324 pair. */
static char *test_576x324(void)
{
    return check_case(576, 324, 0x11112222u);
}
/* 1080p inner region — many full NEON vectors. */
static char *test_1920x1080(void)
{
    return check_case(1920, 1080, 0x33334444u);
}

char *run_tests(void)
{
#if !ARCH_AARCH64
    (void)fprintf(stderr, "note: not aarch64 — float_adm_dwt2_neon absent; "
                          "exercising scalar adm_dwt2_s only\n");
#endif
    mu_run_test(test_2x2);
    mu_run_test(test_3x3);
    mu_run_test(test_4x4);
    mu_run_test(test_5x5);
    mu_run_test(test_7x9);
    mu_run_test(test_15x17);
    mu_run_test(test_64x64);
    mu_run_test(test_576x324);
    mu_run_test(test_1920x1080);
    return NULL;
}
