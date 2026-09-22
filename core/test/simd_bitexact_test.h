/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Shared bit-exact / tolerance test harness for SIMD parity tests
 * (ADR-0245).
 *
 * Each `test_*_simd.c` / `test_*_avx2.c` / `test_*_neon.c` repeats
 * roughly the same scaffolding: a `xorshift32` PRNG, a portable
 * aligned allocator (POSIX vs MinGW/MSVC), an x86 AVX2-CPUID gate,
 * and a memcmp / per-element tolerance assertion. Centralising the
 * scaffolding here cuts every new SIMD parity test from
 * ~50-100 LOC of boilerplate to ~20 LOC of test bodies.
 *
 * The harness is bit-exactness-preserving: the helpers do not
 * touch the SIMD data path itself; they only own setup, assertion,
 * and CPU-feature gating. Callers retain full control over the
 * fixture, the scalar reference, and the SIMD candidate.
 *
 * Usage:
 *   #include "test.h"
 *   #include "simd_bitexact_test.h"
 *
 *   static char *check_kernel(uint32_t seed) {
 *       uint8_t *buf = simd_test_aligned_malloc(N, 32);
 *       simd_test_fill_random_u8(buf, N, seed);
 *       run_scalar(buf, scalar_out);
 *       run_simd(buf, simd_out);
 *       simd_test_aligned_free(buf);
 *       SIMD_BITEXACT_ASSERT_MEMCMP(scalar_out, simd_out,
 *                                   sizeof(scalar_out),
 *                                   "kernel divergence");
 *       return NULL;
 *   }
 *
 * Power-of-10 compliance:
 *   - All macros are single-statement do/while (rule 1: no goto, no
 *     surprises in flow control).
 *   - No dynamic memory beyond the explicit `aligned_malloc` helper
 *     (rule 3: callers own the lifetime).
 *   - Loop bounds in the fill helpers are caller-supplied counts;
 *     the helpers have no internal unbounded loops (rule 2).
 *   - All non-void return values are checked or `(void)`-discarded
 *     in the macros (rule 7).
 */

#ifndef LIBVMAF_TEST_SIMD_BITEXACT_TEST_H
#define LIBVMAF_TEST_SIMD_BITEXACT_TEST_H

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER) || defined(__MINGW32__)
#include <malloc.h>
#endif

#include "config.h"
/* NOTE: callers must `#include "test.h"` themselves before including
 * this harness — `test.h` has no header guard and would clash on
 * `mu_report` if included twice from one TU. */

#if ARCH_X86
#include "x86/cpu.h"
#endif

/* ---------------------------------------------------------------------
 * xorshift32 PRNG.
 *
 * Reproducible across every host the test suite runs on: the state
 * transition is pure 32-bit integer arithmetic with no compiler-
 * dependent ordering (no float, no FMA, no reduction). Lifted from
 * Marsaglia 2003. Lives here as `static inline` so multiple TUs can
 * include this header without ODR collisions or unused-function
 * warnings.
 * ------------------------------------------------------------------- */

static inline uint32_t simd_test_xorshift32(uint32_t *state)
{
    uint32_t s = *state;
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    *state = s;
    return s;
}

/* ---------------------------------------------------------------------
 * Portable aligned allocator.
 *
 * C11 `aligned_alloc` is unavailable on MinGW (no feature-test macro
 * exposes it under `-std=c11 -pedantic`) and on MSVC (which never
 * shipped it). Mirrors the wrapper in `core/src/mem.cpp` so the
 * tests stay free of an internal-header dependency. See PR #198.
 * ------------------------------------------------------------------- */

static inline void *simd_test_aligned_malloc(size_t size, size_t alignment)
{
#if defined(_MSC_VER) || defined(__MINGW32__)
    return _aligned_malloc(size, alignment);
#else
    void *ptr = NULL;
    if (posix_memalign(&ptr, alignment, size) != 0) {
        return NULL;
    }
    return ptr;
#endif
}

static inline void simd_test_aligned_free(void *ptr)
{
#if defined(_MSC_VER) || defined(__MINGW32__)
    _aligned_free(ptr);
#else
    free(ptr);
#endif
}

/* ---------------------------------------------------------------------
 * Deterministic fill helpers.
 *
 * Adversarial coverage is feature-specific (e.g. negative-diff
 * fixtures for `motion_v2`, opsin-range floats for `ssimulacra2`),
 * so callers compose adversarial fillers themselves. The helpers
 * here cover the common reproducible-random case.
 * ------------------------------------------------------------------- */

/* Fill `n` floats in [lo, hi) using the xorshift PRNG. */
static inline void simd_test_fill_random_f32(float *buf, size_t n, float lo, float hi,
                                             uint32_t seed)
{
    uint32_t state = seed;
    const float scale = (hi - lo) / (float)0x01000000;
    for (size_t i = 0; i < n; ++i) {
        const uint32_t r = simd_test_xorshift32(&state) & 0x00ffffffu;
        buf[i] = lo + (float)r * scale;
    }
}

/* Fill `n` uint16 values masked to `mask` (e.g. 0x3FF for 10-bit). */
static inline void simd_test_fill_random_u16(uint16_t *buf, size_t n, uint16_t mask, uint32_t seed)
{
    uint32_t state = seed;
    for (size_t i = 0; i < n; ++i) {
        buf[i] = (uint16_t)(simd_test_xorshift32(&state) & mask);
    }
}

/* Fill `n` int32 values in [0, modulo) (e.g. 4096 for 12-bit DCT input). */
static inline void simd_test_fill_random_i32_mod(int32_t *buf, size_t n, uint32_t modulo,
                                                 uint32_t seed)
{
    uint32_t state = seed;
    for (size_t i = 0; i < n; ++i) {
        buf[i] = (int32_t)(simd_test_xorshift32(&state) % modulo);
    }
}

/* ---------------------------------------------------------------------
 * x86 AVX2 CPUID gate.
 *
 * Returns 1 if the host CPU exposes AVX2, 0 otherwise (and prints
 * a "skipping" line to stderr). Tests on x86 should bail out of
 * `run_tests` when this returns 0; on non-x86 it is a no-op
 * (returns 1 — the caller's `#if ARCH_X86` already gated the path).
 * ------------------------------------------------------------------- */

static inline int simd_test_have_avx2(void)
{
#if ARCH_X86
    const unsigned cpu_flags = vmaf_get_cpu_flags_x86();
    if (!(cpu_flags & VMAF_X86_CPU_FLAG_AVX2)) {
        (void)fprintf(stderr, "skipping: CPU lacks AVX2\n");
        return 0;
    }
    return 1;
#else
    return 1;
#endif
}

/* ---------------------------------------------------------------------
 * x86 AVX-512 CPUID gate.
 *
 * Returns 1 if the host CPU exposes VMAF_X86_CPU_FLAG_AVX512, 0
 * otherwise (and prints a "skipping" line to stderr). Tests that
 * exercise AVX-512 kernels should bail out of `run_tests` when this
 * returns 0. See ADR-0854.
 * ------------------------------------------------------------------- */

static inline int simd_test_have_avx512(void)
{
#if ARCH_X86
    const unsigned cpu_flags = vmaf_get_cpu_flags_x86();
    if (!(cpu_flags & VMAF_X86_CPU_FLAG_AVX512)) {
        (void)fprintf(stderr, "skipping: CPU lacks AVX-512\n");
        return 0;
    }
    return 1;
#else
    return 1;
#endif
}

/* ---------------------------------------------------------------------
 * Bit-exact assertion macros.
 *
 * `SIMD_BITEXACT_ASSERT_MEMCMP` byte-compares two buffers of equal
 * size; on divergence prints the first diverging byte index so the
 * test log identifies the lane/element under audit. The comparison is
 * deliberately on the object representation, not on values: for float
 * buffers it distinguishes -0.0 from +0.0 and NaN payloads, which is
 * exactly what a scalar-vs-SIMD bit-exactness gate must catch
 * (ADR-0138 / ADR-0139). The buffers are viewed as unsigned char before
 * the memcmp so the intent is explicit to readers and to clang-tidy.
 *
 * `SIMD_BITEXACT_ASSERT_RELATIVE` compares two doubles within a
 * relative tolerance — the moment-style "tolerance-bounded, not
 * bit-exact" contract per ADR-0179.
 *
 * Both macros are mu_assert-compatible (return-on-fail) and mu_run
 * preserves the human-readable label.
 * ------------------------------------------------------------------- */

#define SIMD_BITEXACT_ASSERT_MEMCMP(scalar_buf, simd_buf, n_bytes, label)                          \
    do {                                                                                           \
        const unsigned char *_s_p = (const unsigned char *)(scalar_buf);                           \
        const unsigned char *_v_p = (const unsigned char *)(simd_buf);                             \
        const size_t _n = (size_t)(n_bytes);                                                       \
        if (memcmp(_s_p, _v_p, _n) != 0) {                                                         \
            size_t _i = 0;                                                                         \
            while (_i < _n && _s_p[_i] == _v_p[_i]) {                                              \
                ++_i;                                                                              \
            }                                                                                      \
            (void)fprintf(stderr,                                                                  \
                          "  %s: first diverging byte at offset %zu / %zu "                        \
                          "(scalar=0x%02x simd=0x%02x)\n",                                         \
                          (label), _i, _n, (unsigned)_s_p[_i], (unsigned)_v_p[_i]);                \
            return (label);                                                                        \
        }                                                                                          \
    } while (0)

#define SIMD_BITEXACT_ASSERT_RELATIVE(scalar_val, simd_val, rel_tol, label)                        \
    do {                                                                                           \
        const double _s = (double)(scalar_val);                                                    \
        const double _v = (double)(simd_val);                                                      \
        const double _rel = fabs(_v - _s) / (fabs(_s) + 1e-30);                                    \
        if (!(_rel < (rel_tol))) {                                                                 \
            (void)fprintf(stderr, "  %s: scalar=%.17g simd=%.17g rel=%.3g tol=%.3g\n", (label),    \
                          _s, _v, _rel, (double)(rel_tol));                                        \
            return (label);                                                                        \
        }                                                                                          \
    } while (0)

/* ---------------------------------------------------------------------
 * Guard-band (untouched-outside-region) helpers.
 *
 * A bit-exact comparison only covers the region a kernel is meant to
 * write. A kernel that also stores past it -- a vector loop whose last
 * iteration runs beyond `right`, a last-row store that lands in the next
 * band of a shared slab -- passes such a comparison, because nothing
 * ever looks there. Netflix/vmaf 03b5562c5 (adm_decouple_avx2) and
 * ea012e387 (adm_dwt2_8_neon) were both of that shape and both went
 * unnoticed by the fork's parity tests.
 *
 * The pattern: fill the whole output allocation with
 * SIMD_TEST_GUARD_BYTE (`simd_test_guard_fill`), run the kernel, then
 * count the elements outside the declared write rectangle that no longer
 * hold the guard pattern (`simd_test_guard_count_outside`). Allocate a
 * few trailing slack elements past the last row and include them in the
 * count so a store past the end of the plane is caught too.
 * ------------------------------------------------------------------- */

#define SIMD_TEST_GUARD_BYTE 0xA5u

/* Rows [row0, row1) x columns [col0, col1), in elements. */
typedef struct SimdTestRect {
    size_t row0;
    size_t row1;
    size_t col0;
    size_t col1;
} SimdTestRect;

/* A plane of `rows` x `stride` elements of `elem_size` bytes, followed by
 * `slack` further elements that nothing may write. */
typedef struct SimdTestPlane {
    const void *base;
    size_t elem_size;
    size_t stride;
    size_t rows;
    size_t slack;
} SimdTestPlane;

static inline void simd_test_guard_fill(void *buf, size_t n_bytes)
{
    (void)memset(buf, (int)SIMD_TEST_GUARD_BYTE, n_bytes);
}

static inline int simd_test_guard_elem_intact(const unsigned char *elem, size_t elem_size)
{
    for (size_t b = 0; b < elem_size; ++b) {
        if (elem[b] != SIMD_TEST_GUARD_BYTE) {
            return 0;
        }
    }
    return 1;
}

static inline int simd_test_rect_contains(SimdTestRect rect, size_t row, size_t col)
{
    return row >= rect.row0 && row < rect.row1 && col >= rect.col0 && col < rect.col1;
}

/* Number of elements of `plane` (rows x stride plus the trailing slack)
 * that lie outside `rect` and no longer hold the guard pattern. */
static inline size_t simd_test_guard_count_outside(SimdTestPlane plane, SimdTestRect rect)
{
    const unsigned char *bytes = (const unsigned char *)plane.base;
    const size_t total = (plane.rows * plane.stride) + plane.slack;
    size_t touched = 0;

    for (size_t k = 0; k < total; ++k) {
        const size_t row = k / plane.stride;
        const size_t col = k % plane.stride;
        if (row < plane.rows && simd_test_rect_contains(rect, row, col)) {
            continue;
        }
        if (!simd_test_guard_elem_intact(bytes + (k * plane.elem_size), plane.elem_size)) {
            ++touched;
        }
    }
    return touched;
}

/* mu_assert-compatible: fails when `count` elements outside the region
 * were written. */
#define SIMD_GUARD_ASSERT_UNTOUCHED(count, label)                                                  \
    do {                                                                                           \
        const size_t _touched = (size_t)(count);                                                   \
        if (_touched != 0) {                                                                       \
            (void)fprintf(stderr, "  %s: %zu element(s) written outside the declared region\n",    \
                          (label), _touched);                                                      \
            return (label);                                                                        \
        }                                                                                          \
    } while (0)

#endif /* LIBVMAF_TEST_SIMD_BITEXACT_TEST_H */
