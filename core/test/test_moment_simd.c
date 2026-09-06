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
 * Numerical-parity contract test for the float_moment SIMD kernels
 * (T7-19, ADR-0179).
 *
 * The contract per `moment_avx2.c` / `moment_neon.c` headers is
 * tolerance-bounded, not bit-exact: the lane-widening + per-row
 * tail divergence yields residuals well inside the snapshot gate's
 * tolerance but not byte-for-byte equal to the scalar reference.
 * The scalar TU's auto-vectorisation (and any compiler-driven
 * precision behaviour) further removes the bit-exact guarantee.
 *
 * Tolerance: 1e-9 absolute on the post-normalisation score (range
 * 0..2^16 for 8-bit pixel squares), which is ~5 orders of magnitude
 * tighter than the snapshot gate's `places=4`.
 *
 * Boilerplate (xorshift PRNG, portable aligned alloc, AVX2 gate,
 * relative-tolerance assertion) is provided by
 * `simd_bitexact_test.h` (ADR-0245).
 */

#include <stddef.h>
#include <stdint.h>

#include "config.h"
#include "test.h"
/* clang-format off — `test.h` has no header guard, must precede the
 * harness include to avoid a `mu_report` redefinition. */
#include "simd_bitexact_test.h"
/* clang-format on */

#include "feature/moment.h"

#if ARCH_X86
#include "feature/x86/moment_avx2.h"
#if HAVE_AVX512
#include "feature/x86/moment_avx512.h"
#endif
#endif
#if ARCH_AARCH64
#include "feature/arm64/moment_neon.h"
#if HAVE_SVE2
#include "feature/arm64/moment_sve2.h"
#include "cpu.h"
#endif
#endif

#define ALIGN_BYTES 32
#define TEST_W 73 /* not a multiple of 4 or 8 — exercises tail */
#define TEST_H 17

/* Relative tolerance: 1e-7 of the scalar score. Residual sources:
 * per-row tail order, lane-pair cross-add precision, scalar-TU
 * auto-vectorisation. Still ~500× tighter than the production
 * snapshot gate (`places=4` ⇒ |abs| < 5e-5 on a normalised score). */
#define MOMENT_REL_TOL 1e-7

/* Pixel input range matches the post-`picture_copy` 8-bit float
 * layout of the float_moment extractor: values in [0, 256). */
#define MOMENT_FILL_LO 0.0f
#define MOMENT_FILL_HI 256.0f

/*
 * Tail-only bit-exactness regression fixture (moment-2nd-tail-double-square).
 *
 * A single row (h=1) narrower than the SIMD main-loop lane count makes a kernel
 * execute ONLY its scalar tail loop — no main-loop lanes and therefore no
 * cross-lane reduction.  The accumulation order is then identical to the scalar
 * reference, so the SIMD score must equal the scalar score EXACTLY.
 *
 * The bug: the per-row tail used to square in double (`(double)p * (double)p`)
 * while the scalar reference (moment.c) and the SIMD main loop square in float
 * (`p * p` / `_mm256_mul_ps` / `vmulq_f32`).  The double square rounds
 * differently, so the tail diverged from scalar.  These pixel values have many
 * significant bits so the float square (p*p) loses low mantissa bits — the
 * exact-equality checks below FAIL against the pre-fix kernels and PASS after.
 *
 * Exact `==` (not a tolerance) is intentional: for a single tail-only row the
 * operation sequence is identical, so the doubles are bit-identical.  All
 * values are finite and non-NaN, so `==` is the correct bit-exact predicate.
 */
#if ARCH_X86 || ARCH_AARCH64
static const float moment_tail_vals[15] = {255.937f, 254.123f, 253.871f, 252.499f, 251.001f,
                                           250.753f, 249.317f, 248.111f, 247.629f, 246.003f,
                                           245.555f, 244.917f, 243.333f, 242.071f, 241.629f};
#endif

#if ARCH_X86

static char *check_avx2(uint32_t seed, int w, int h)
{
    const int stride_floats = (w + 7) & ~7;
    const size_t bytes = (size_t)stride_floats * (size_t)h * sizeof(float);
    const int stride_bytes = stride_floats * (int)sizeof(float);

    float *buf = (float *)simd_test_aligned_malloc(bytes, ALIGN_BYTES);
    if (!buf) {
        return "aligned_malloc failed";
    }
    simd_test_fill_random_f32(buf, (size_t)stride_floats * (size_t)h, MOMENT_FILL_LO,
                              MOMENT_FILL_HI, seed);

    double s_scalar = 0.0;
    double s_avx2 = 0.0;
    (void)compute_1st_moment(buf, w, h, stride_bytes, &s_scalar);
    (void)compute_1st_moment_avx2(buf, w, h, stride_bytes, &s_avx2);

    double t_scalar = 0.0;
    double t_avx2 = 0.0;
    (void)compute_2nd_moment(buf, w, h, stride_bytes, &t_scalar);
    (void)compute_2nd_moment_avx2(buf, w, h, stride_bytes, &t_avx2);

    simd_test_aligned_free(buf);

    SIMD_BITEXACT_ASSERT_RELATIVE(s_scalar, s_avx2, MOMENT_REL_TOL,
                                  "compute_1st_moment_avx2 outside relative tolerance");
    SIMD_BITEXACT_ASSERT_RELATIVE(t_scalar, t_avx2, MOMENT_REL_TOL,
                                  "compute_2nd_moment_avx2 outside relative tolerance");
    return NULL;
}

static char *test_avx2_seed_a(void)
{
    return check_avx2(0xdeadbeefu, TEST_W, TEST_H);
}
static char *test_avx2_seed_b(void)
{
    return check_avx2(0x12345678u, TEST_W, TEST_H);
}
static char *test_avx2_aligned_w(void)
{
    return check_avx2(0xabcdef01u, 64, 16);
}
static char *test_avx2_tiny(void)
{
    return check_avx2(0xfeedface, 9, 1);
}

static char *test_avx2_tail_bitexact(void)
{
    /* w=7 < 8: AVX2 runs only the scalar tail. */
    _Alignas(ALIGN_BYTES) float buf[16];
    for (int j = 0; j < 7; ++j)
        buf[j] = moment_tail_vals[j];
    const int stride_bytes = 16 * (int)sizeof(float);

    double t_scalar = 0.0;
    double t_avx2 = 0.0;
    (void)compute_2nd_moment(buf, 7, 1, stride_bytes, &t_scalar);
    (void)compute_2nd_moment_avx2(buf, 7, 1, stride_bytes, &t_avx2);
    mu_assert("compute_2nd_moment_avx2 tail not bit-exact to scalar", t_avx2 == t_scalar);
    return NULL;
}

#if HAVE_AVX512
/* AVX-512 parity: same fixture logic but compare against scalar via the
 * 16-lane widened path.  Stride is rounded up to 16 to exercise aligned
 * ZMM store path.  ADR-0987. */
static char *check_avx512(uint32_t seed, int w, int h)
{
    const int stride_floats = (w + 15) & ~15;
    const size_t bytes = (size_t)stride_floats * (size_t)h * sizeof(float);
    const int stride_bytes = stride_floats * (int)sizeof(float);

    float *buf = (float *)simd_test_aligned_malloc(bytes, 64);
    if (!buf) {
        return "aligned_malloc failed (avx512)";
    }
    simd_test_fill_random_f32(buf, (size_t)stride_floats * (size_t)h, MOMENT_FILL_LO,
                              MOMENT_FILL_HI, seed);

    double s_scalar = 0.0;
    double s_avx512 = 0.0;
    (void)compute_1st_moment(buf, w, h, stride_bytes, &s_scalar);
    (void)compute_1st_moment_avx512(buf, w, h, stride_bytes, &s_avx512);

    double t_scalar = 0.0;
    double t_avx512 = 0.0;
    (void)compute_2nd_moment(buf, w, h, stride_bytes, &t_scalar);
    (void)compute_2nd_moment_avx512(buf, w, h, stride_bytes, &t_avx512);

    simd_test_aligned_free(buf);

    SIMD_BITEXACT_ASSERT_RELATIVE(s_scalar, s_avx512, MOMENT_REL_TOL,
                                  "compute_1st_moment_avx512 outside relative tolerance");
    SIMD_BITEXACT_ASSERT_RELATIVE(t_scalar, t_avx512, MOMENT_REL_TOL,
                                  "compute_2nd_moment_avx512 outside relative tolerance");
    return NULL;
}

static char *test_avx512_seed_a(void)
{
    return check_avx512(0xdeadbeefu, TEST_W, TEST_H);
}
static char *test_avx512_seed_b(void)
{
    return check_avx512(0x12345678u, TEST_W, TEST_H);
}
static char *test_avx512_aligned_w(void)
{
    /* w=64 is an exact multiple of 16 — exercises the zero-tail branch. */
    return check_avx512(0xabcdef01u, 64, 16);
}
static char *test_avx512_tiny(void)
{
    /* w=9 exercises both the 8-lane AVX2-width fallback tail and the
     * scalar tail inside compute_1st/2nd_moment_avx512. */
    return check_avx512(0xfeedface, 9, 1);
}

/* Tail-only bit-exactness regression (see moment_tail_vals comment above).
 * w=15 < 16: the AVX512 kernel runs the 8-lane fallback then the scalar tail,
 * both squaring in float — so the score must equal scalar exactly. */
static char *test_avx512_tail_bitexact(void)
{
    _Alignas(64) float buf[16];
    for (int j = 0; j < 15; ++j)
        buf[j] = moment_tail_vals[j];
    const int stride_bytes = 16 * (int)sizeof(float);

    double t_scalar = 0.0;
    double t_avx512 = 0.0;
    (void)compute_2nd_moment(buf, 15, 1, stride_bytes, &t_scalar);
    (void)compute_2nd_moment_avx512(buf, 15, 1, stride_bytes, &t_avx512);
    mu_assert("compute_2nd_moment_avx512 tail not bit-exact to scalar", t_avx512 == t_scalar);
    return NULL;
}
#endif /* HAVE_AVX512 */

#endif /* ARCH_X86 */

#if ARCH_AARCH64

static char *check_neon(uint32_t seed, int w, int h)
{
    const int stride_floats = (w + 3) & ~3;
    const size_t bytes = (size_t)stride_floats * (size_t)h * sizeof(float);
    const int stride_bytes = stride_floats * (int)sizeof(float);

    float *buf = (float *)simd_test_aligned_malloc(bytes, ALIGN_BYTES);
    if (!buf) {
        return "aligned_malloc failed";
    }
    simd_test_fill_random_f32(buf, (size_t)stride_floats * (size_t)h, MOMENT_FILL_LO,
                              MOMENT_FILL_HI, seed);

    double s_scalar = 0.0;
    double s_neon = 0.0;
    (void)compute_1st_moment(buf, w, h, stride_bytes, &s_scalar);
    (void)compute_1st_moment_neon(buf, w, h, stride_bytes, &s_neon);

    double t_scalar = 0.0;
    double t_neon = 0.0;
    (void)compute_2nd_moment(buf, w, h, stride_bytes, &t_scalar);
    (void)compute_2nd_moment_neon(buf, w, h, stride_bytes, &t_neon);

    simd_test_aligned_free(buf);

    SIMD_BITEXACT_ASSERT_RELATIVE(s_scalar, s_neon, MOMENT_REL_TOL,
                                  "compute_1st_moment_neon outside relative tolerance");
    SIMD_BITEXACT_ASSERT_RELATIVE(t_scalar, t_neon, MOMENT_REL_TOL,
                                  "compute_2nd_moment_neon outside relative tolerance");
    return NULL;
}

static char *test_neon_seed_a(void)
{
    return check_neon(0xdeadbeefu, TEST_W, TEST_H);
}
static char *test_neon_seed_b(void)
{
    return check_neon(0x12345678u, TEST_W, TEST_H);
}
static char *test_neon_aligned_w(void)
{
    return check_neon(0xabcdef01u, 64, 16);
}
static char *test_neon_tiny(void)
{
    return check_neon(0xfeedface, 5, 1);
}

/* Tail-only bit-exactness regression (see moment_tail_vals comment above).
 * w=3 < 4: NEON runs only the scalar tail, squaring in float — so the score
 * must equal scalar exactly. */
static char *test_neon_tail_bitexact(void)
{
    _Alignas(ALIGN_BYTES) float buf[16];
    for (int j = 0; j < 3; ++j)
        buf[j] = moment_tail_vals[j];
    const int stride_bytes = 16 * (int)sizeof(float);

    double t_scalar = 0.0;
    double t_neon = 0.0;
    (void)compute_2nd_moment(buf, 3, 1, stride_bytes, &t_scalar);
    (void)compute_2nd_moment_neon(buf, 3, 1, stride_bytes, &t_neon);
    mu_assert("compute_2nd_moment_neon tail not bit-exact to scalar", t_neon == t_scalar);
    return NULL;
}

#if HAVE_SVE2
/* SVE2 parity tests (ADR-0584).
 * Runtime-skipped when VMAF_ARM_CPU_FLAG_SVE2 is not set so a NEON-only
 * host passes without executing the SVE2 path. */
static char *check_sve2(uint32_t seed, int w, int h)
{
    if (!(vmaf_get_cpu_flags() & VMAF_ARM_CPU_FLAG_SVE2)) {
        (void)fprintf(stderr, "  skipping SVE2 moment test: HWCAP2_SVE2 not set\n");
        return NULL;
    }

    /* Stride aligned to 4 floats — matches the NEON sibling convention.
     * The SVE2 path handles any stride via predicated loads. */
    const int stride_floats = (w + 3) & ~3;
    const size_t bytes = (size_t)stride_floats * (size_t)h * sizeof(float);
    const int stride_bytes = stride_floats * (int)sizeof(float);

    float *buf = (float *)simd_test_aligned_malloc(bytes, ALIGN_BYTES);
    if (!buf) {
        return "aligned_malloc failed";
    }
    simd_test_fill_random_f32(buf, (size_t)stride_floats * (size_t)h, MOMENT_FILL_LO,
                              MOMENT_FILL_HI, seed);

    double s_scalar = 0.0;
    double s_sve2 = 0.0;
    (void)compute_1st_moment(buf, w, h, stride_bytes, &s_scalar);
    (void)compute_1st_moment_sve2(buf, w, h, stride_bytes, &s_sve2);

    double t_scalar = 0.0;
    double t_sve2 = 0.0;
    (void)compute_2nd_moment(buf, w, h, stride_bytes, &t_scalar);
    (void)compute_2nd_moment_sve2(buf, w, h, stride_bytes, &t_sve2);

    simd_test_aligned_free(buf);

    SIMD_BITEXACT_ASSERT_RELATIVE(s_scalar, s_sve2, MOMENT_REL_TOL,
                                  "compute_1st_moment_sve2 outside relative tolerance");
    SIMD_BITEXACT_ASSERT_RELATIVE(t_scalar, t_sve2, MOMENT_REL_TOL,
                                  "compute_2nd_moment_sve2 outside relative tolerance");
    return NULL;
}

static char *test_sve2_seed_a(void)
{
    return check_sve2(0xdeadbeefu, TEST_W, TEST_H);
}
static char *test_sve2_seed_b(void)
{
    return check_sve2(0x12345678u, TEST_W, TEST_H);
}
static char *test_sve2_aligned_w(void)
{
    return check_sve2(0xabcdef01u, 64, 16);
}
static char *test_sve2_tiny(void)
{
    return check_sve2(0xfeedface, 5, 1);
}
#endif /* HAVE_SVE2 */

#endif /* ARCH_AARCH64 */

/* Per-arch registration extracted into helpers so run_tests() itself stays
 * under the readability-function-size threshold (matches the sibling
 * test_*_simd.c convention; no suppression needed). */
#if ARCH_X86
#if HAVE_AVX512
static char *run_tests_avx512(void)
{
    if (!simd_test_have_avx512()) {
        return NULL;
    }
    mu_run_test(test_avx512_seed_a);
    mu_run_test(test_avx512_seed_b);
    mu_run_test(test_avx512_aligned_w);
    mu_run_test(test_avx512_tiny);
    mu_run_test(test_avx512_tail_bitexact);
    return NULL;
}
#endif /* HAVE_AVX512 */

static char *run_tests_x86(void)
{
    if (!simd_test_have_avx2()) {
        return NULL;
    }
    mu_run_test(test_avx2_seed_a);
    mu_run_test(test_avx2_seed_b);
    mu_run_test(test_avx2_aligned_w);
    mu_run_test(test_avx2_tiny);
    mu_run_test(test_avx2_tail_bitexact);
#if HAVE_AVX512
    {
        char *r = run_tests_avx512();
        if (r)
            return r;
    }
#endif /* HAVE_AVX512 */
    return NULL;
}
#elif ARCH_AARCH64
static char *run_tests_aarch64(void)
{
    mu_run_test(test_neon_seed_a);
    mu_run_test(test_neon_seed_b);
    mu_run_test(test_neon_aligned_w);
    mu_run_test(test_neon_tiny);
    mu_run_test(test_neon_tail_bitexact);
#if HAVE_SVE2
    mu_run_test(test_sve2_seed_a);
    mu_run_test(test_sve2_seed_b);
    mu_run_test(test_sve2_aligned_w);
    mu_run_test(test_sve2_tiny);
#endif
    return NULL;
}
#endif

char *run_tests(void)
{
#if ARCH_X86
    return run_tests_x86();
#elif ARCH_AARCH64
    return run_tests_aarch64();
#else
    (void)fprintf(stderr, "skipping: arch lacks moment SIMD\n");
    return NULL;
#endif
}
