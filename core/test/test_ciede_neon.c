/**
 * Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 * NEON-vs-scalar bit-exactness and read-bounds coverage for the CIEDE2000
 * plane-preprocessing kernels (`core/src/feature/arm64/ciede_neon.c`).
 *
 * `ciede_preprocess_{8,16}_neon` widen a packed uint8/uint16 plane row into a
 * float row that feeds the per-pixel dE2000 loop in ciede.c. The widening is
 * exact for every input value (uint8 and uint16 both fit in float's 24-bit
 * significand), so scalar and NEON must agree on every bit — there is no
 * rounding freedom to hide behind.
 *
 * The x86 counterpart is covered by test_ciede_simd_parity.c; the NEON kernels
 * had no test on any architecture. `ciede.c` installs them unconditionally
 * whenever VMAF_ARM_CPU_FLAG_NEON is set, with no width guard at all, so every
 * width from 1 upward reaches them.
 *
 * Two properties are asserted:
 *
 *   1. Value parity — bit-exact float output vs. the scalar fallback loop,
 *      over widths that are and are not multiples of the vector stride.
 *
 *   2. Read bounds — the kernel must not read past `buf + w`. This is the
 *      NEON analogue of the stride/width mismatch that ADR-1057 and the
 *      adm_dwt2 vertical-pass tail were: `ciede_preprocess_8_neon` stepped
 *      four pixels per iteration but issued an eight-byte `vld1_u8`, so the
 *      final vector iteration always read four bytes it never consumed. The
 *      probe below places each plane row immediately before a PROT_NONE guard
 *      page and reports the exact number of bytes past `w` the kernel touches.
 */

#include <setjmp.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "test.h"

#if ARCH_AARCH64
#include <sys/mman.h>
#include <unistd.h>

#include "feature/arm64/ciede_neon.h"

/* Widths chosen to straddle every plausible vector stride (4, 8, 16) and to
 * include the pathological small cases where no vector iteration runs at all. */
static const int kWidths[] = {1,  2,  3,  4,  5,  6,  7,  8,  9,  11,  15,  16,  17,  23,
                              24, 31, 32, 33, 43, 63, 64, 65, 96, 127, 128, 129, 576, 1920};
#define K_NUM_WIDTHS ((int)(sizeof(kWidths) / sizeof(kWidths[0])))

/* Verbatim copies of the ciede.c fallback loops. The production ones are
 * inlined into `extract()`, so the oracle is transcribed rather than linked. */
static void ciede_preprocess_8_scalar(const uint8_t *y_buf, const uint8_t *u_buf,
                                      const uint8_t *v_buf, float *out_y, float *out_u,
                                      float *out_v, int w)
{
    for (int j = 0; j < w; j++) {
        out_y[j] = (float)y_buf[j];
        out_u[j] = (float)u_buf[j];
        out_v[j] = (float)v_buf[j];
    }
}

static void ciede_preprocess_16_scalar(const uint16_t *y_buf, const uint16_t *u_buf,
                                       const uint16_t *v_buf, float *out_y, float *out_u,
                                       float *out_v, int w)
{
    for (int j = 0; j < w; j++) {
        out_y[j] = (float)y_buf[j];
        out_u[j] = (float)u_buf[j];
        out_v[j] = (float)v_buf[j];
    }
}

static uint32_t xorshift32(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

/* -------------------------------------------------------------------------
 * Guard-page probe.
 *
 * Each plane row is placed so that its last byte abuts a PROT_NONE page. A
 * kernel that reads even one byte past the row faults; the handler unwinds
 * back into the probe, which retries with one more readable byte of slack.
 * The first slack value that survives IS the kernel's over-read extent.
 * ---------------------------------------------------------------------- */

static sigjmp_buf g_fault_jmp;
static volatile sig_atomic_t g_fault_armed;

static void fault_handler(int sig)
{
    if (g_fault_armed) {
        g_fault_armed = 0;
        siglongjmp(g_fault_jmp, sig);
    }
    _exit(128 + sig);
}

typedef struct GuardedRow {
    uint8_t *map;   /* base of the two-page mapping */
    size_t map_len; /* total mapped bytes */
    uint8_t *row;   /* row start; row + readable == guard page */
} GuardedRow;

/* Maps [readable-page][PROT_NONE page] and returns a row whose last readable
 * byte is the last byte of the first page. Returns 0 on success. */
static int guarded_row_alloc(GuardedRow *g, size_t readable, long page)
{
    g->map_len = (size_t)page * 2u;
    g->map = mmap(NULL, g->map_len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (g->map == MAP_FAILED) {
        g->map = NULL;
        return -1;
    }
    if (mprotect(g->map + page, (size_t)page, PROT_NONE) != 0) {
        (void)munmap(g->map, g->map_len);
        g->map = NULL;
        return -1;
    }
    g->row = g->map + (size_t)page - readable;
    return 0;
}

static void guarded_row_free(GuardedRow *g)
{
    if (g->map)
        (void)munmap(g->map, g->map_len);
    g->map = NULL;
}

/* Returns the smallest number of readable bytes past `w` elements that lets
 * the kernel run without faulting, or -1 if it exceeds `max_slack`. */
static int probe_overread(int w, int elem_size, int max_slack)
{
    const long page = sysconf(_SC_PAGESIZE);
    struct sigaction sa, old_segv, old_bus;
    float *out[3] = {NULL, NULL, NULL};
    int result = -1;

    for (int k = 0; k < 3; k++) {
        out[k] = calloc((size_t)w + 8u, sizeof(float));
        if (!out[k])
            goto done;
    }

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = fault_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_NODEFER;
    if (sigaction(SIGSEGV, &sa, &old_segv) != 0)
        goto done;
    if (sigaction(SIGBUS, &sa, &old_bus) != 0) {
        (void)sigaction(SIGSEGV, &old_segv, NULL);
        goto done;
    }

    /* `slack`, `mapped` and `survived` are live across the siglongjmp, so they
     * must be volatile: a register-allocated local would be restored to its
     * pre-sigsetjmp value when the handler unwinds (C11 7.13.2.1p3). */
    for (volatile int slack = 0; slack <= max_slack; slack++) {
        const size_t readable = (size_t)(w + slack) * (size_t)elem_size;
        GuardedRow g[3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
        uint32_t seed = 0xC1EDE000u ^ (uint32_t)(w * 7 + slack);
        volatile int mapped = 0;
        volatile int survived = 0;

        if (readable > (size_t)page)
            break;
        for (int k = 0; k < 3; k++) {
            if (guarded_row_alloc(&g[k], readable, page) != 0)
                goto unmap;
            for (size_t b = 0; b < readable; b++)
                g[k].row[b] = (uint8_t)xorshift32(&seed);
            mapped++;
        }

        g_fault_armed = 1;
        if (sigsetjmp(g_fault_jmp, 1) == 0) {
            if (elem_size == 1)
                ciede_preprocess_8_neon(g[0].row, g[1].row, g[2].row, out[0], out[1], out[2], w);
            else
                ciede_preprocess_16_neon((const uint16_t *)g[0].row, (const uint16_t *)g[1].row,
                                         (const uint16_t *)g[2].row, out[0], out[1], out[2], w);
            survived = 1;
        } else {
            survived = 0;
        }
        g_fault_armed = 0;

    unmap:
        for (int k = 0; k < mapped; k++)
            guarded_row_free(&g[k]);
        if (mapped < 3)
            break;
        if (survived) {
            result = slack * elem_size;
            break;
        }
    }

    (void)sigaction(SIGSEGV, &old_segv, NULL);
    (void)sigaction(SIGBUS, &old_bus, NULL);

done:
    for (int k = 0; k < 3; k++)
        free(out[k]);
    return result;
}

/* -------------------------------------------------------------------------
 * Value parity.
 * ---------------------------------------------------------------------- */

static int check_parity_8(int w, uint32_t seed)
{
    const size_t out_bytes = (size_t)w * sizeof(float);
    uint8_t *in[3];
    float *ref[3], *simd[3];
    int mismatches = 0;

    for (int k = 0; k < 3; k++) {
        in[k] = malloc((size_t)w);
        ref[k] = malloc(out_bytes);
        simd[k] = malloc(out_bytes);
        if (!in[k] || !ref[k] || !simd[k])
            return -1;
        memset(ref[k], 0xA5, out_bytes);
        memset(simd[k], 0x5A, out_bytes);
        for (int j = 0; j < w; j++)
            in[k][j] = (uint8_t)xorshift32(&seed);
    }

    ciede_preprocess_8_scalar(in[0], in[1], in[2], ref[0], ref[1], ref[2], w);
    ciede_preprocess_8_neon(in[0], in[1], in[2], simd[0], simd[1], simd[2], w);

    for (int k = 0; k < 3; k++) {
        for (int j = 0; j < w; j++) {
            if (memcmp(&ref[k][j], &simd[k][j], sizeof(float)) != 0) {
                if (mismatches < 4)
                    (void)fprintf(stderr, "  w=%d plane %d idx %d: scalar %.9g != neon %.9g\n", w,
                                  k, j, (double)ref[k][j], (double)simd[k][j]);
                mismatches++;
            }
        }
        free(in[k]);
        free(ref[k]);
        free(simd[k]);
    }
    return mismatches;
}

static int check_parity_16(int w, uint32_t seed)
{
    const size_t out_bytes = (size_t)w * sizeof(float);
    uint16_t *in[3];
    float *ref[3], *simd[3];
    int mismatches = 0;

    for (int k = 0; k < 3; k++) {
        in[k] = malloc((size_t)w * sizeof(uint16_t));
        ref[k] = malloc(out_bytes);
        simd[k] = malloc(out_bytes);
        if (!in[k] || !ref[k] || !simd[k])
            return -1;
        memset(ref[k], 0xA5, out_bytes);
        memset(simd[k], 0x5A, out_bytes);
        for (int j = 0; j < w; j++)
            in[k][j] = (uint16_t)xorshift32(&seed);
    }

    ciede_preprocess_16_scalar(in[0], in[1], in[2], ref[0], ref[1], ref[2], w);
    ciede_preprocess_16_neon(in[0], in[1], in[2], simd[0], simd[1], simd[2], w);

    for (int k = 0; k < 3; k++) {
        for (int j = 0; j < w; j++) {
            if (memcmp(&ref[k][j], &simd[k][j], sizeof(float)) != 0) {
                if (mismatches < 4)
                    (void)fprintf(stderr, "  w=%d plane %d idx %d: scalar %.9g != neon %.9g\n", w,
                                  k, j, (double)ref[k][j], (double)simd[k][j]);
                mismatches++;
            }
        }
        free(in[k]);
        free(ref[k]);
        free(simd[k]);
    }
    return mismatches;
}
#endif /* ARCH_AARCH64 */

static char *test_ciede_preprocess_8_neon_parity(void)
{
#if !ARCH_AARCH64
    return NULL;
#else
    int total = 0;
    for (int i = 0; i < K_NUM_WIDTHS; i++) {
        const int m = check_parity_8(kWidths[i], 0x8B17u ^ (uint32_t)kWidths[i]);
        mu_assert("ciede_preprocess_8_neon parity: allocation failed", m >= 0);
        total += m;
    }
    mu_assert("ciede_preprocess_8_neon output diverges from the scalar reference", total == 0);
    return NULL;
#endif
}

static char *test_ciede_preprocess_16_neon_parity(void)
{
#if !ARCH_AARCH64
    return NULL;
#else
    int total = 0;
    for (int i = 0; i < K_NUM_WIDTHS; i++) {
        const int m = check_parity_16(kWidths[i], 0x16B1u ^ (uint32_t)kWidths[i]);
        mu_assert("ciede_preprocess_16_neon parity: allocation failed", m >= 0);
        total += m;
    }
    mu_assert("ciede_preprocess_16_neon output diverges from the scalar reference", total == 0);
    return NULL;
#endif
}

/* The contract is `w` elements in, `w` floats out. ciede.c hands the kernel a
 * bare row pointer into a VmafPicture whose stride is only ceil(w/64)*64, so a
 * row whose width is a multiple of 64 has NO slack after it, and the final row
 * of the V plane has nothing after it at all. Any over-read is a heap
 * over-read on exactly the geometries VMAF is usually run on (576, 1280, 1920,
 * 3840 are all multiples of 64). */
static char *test_ciede_preprocess_8_neon_read_bounds(void)
{
#if !ARCH_AARCH64
    return NULL;
#else
    int worst = 0;
    for (int i = 0; i < K_NUM_WIDTHS; i++) {
        const int w = kWidths[i];
        int over;
        if ((size_t)(w + 32) > (size_t)sysconf(_SC_PAGESIZE))
            continue;
        over = probe_overread(w, 1, 32);
        mu_assert("ciede_preprocess_8_neon reads more than 32 bytes past the row", over >= 0);
        if (over > 0) {
            (void)fprintf(stderr, "  w=%d: ciede_preprocess_8_neon reads %d byte(s) past buf+w\n",
                          w, over);
            if (over > worst)
                worst = over;
        }
    }
    mu_assert("ciede_preprocess_8_neon reads past the end of the plane row", worst == 0);
    return NULL;
#endif
}

static char *test_ciede_preprocess_16_neon_read_bounds(void)
{
#if !ARCH_AARCH64
    return NULL;
#else
    int worst = 0;
    for (int i = 0; i < K_NUM_WIDTHS; i++) {
        const int w = kWidths[i];
        int over;
        if ((size_t)(w + 32) * 2u > (size_t)sysconf(_SC_PAGESIZE))
            continue;
        over = probe_overread(w, 2, 32);
        mu_assert("ciede_preprocess_16_neon reads more than 64 bytes past the row", over >= 0);
        if (over > 0) {
            (void)fprintf(stderr, "  w=%d: ciede_preprocess_16_neon reads %d byte(s) past buf+w\n",
                          w, over);
            if (over > worst)
                worst = over;
        }
    }
    mu_assert("ciede_preprocess_16_neon reads past the end of the plane row", worst == 0);
    return NULL;
#endif
}

char *run_tests(void)
{
#if ARCH_AARCH64
    mu_run_test(test_ciede_preprocess_8_neon_parity);
    mu_run_test(test_ciede_preprocess_16_neon_parity);
    mu_run_test(test_ciede_preprocess_8_neon_read_bounds);
    mu_run_test(test_ciede_preprocess_16_neon_read_bounds);
#else
    (void)fprintf(stderr, "skipping: non-aarch64 arch\n");
    (void)test_ciede_preprocess_8_neon_parity;
    (void)test_ciede_preprocess_16_neon_parity;
    (void)test_ciede_preprocess_8_neon_read_bounds;
    (void)test_ciede_preprocess_16_neon_read_bounds;
#endif
    return NULL;
}
