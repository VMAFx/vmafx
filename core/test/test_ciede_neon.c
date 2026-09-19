/**
 * Copyright 2016-2026 Netflix, Inc.
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
 *      probe below places each plane row immediately before an inaccessible
 *      guard page and reports the exact number of bytes past `w` the kernel
 *      touches. The guard page is `mmap` + `PROT_NONE` on POSIX and
 *      `VirtualAlloc` + `PAGE_NOACCESS` on Windows; the fault is caught with
 *      `sigsetjmp` / `siglongjmp` and with SEH `__try` / `__except`
 *      respectively, so the Windows ARM64 lane (ADR-1260) runs the same
 *      probe as the Linux and macOS aarch64 builds.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "test.h"

#if ARCH_AARCH64
#if defined(_WIN32)
#if !defined(_MSC_VER) && !defined(__clang__)
#error "the guard-page probe needs structured exception handling (__try) on Windows"
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <setjmp.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

#include "feature/arm64/ciede_neon.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe, and this file mirrors
 * the C spelling of the surface it exercises. ADR-1138. */

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
 * Each plane row is placed so that its last byte abuts an inaccessible page.
 * A kernel that reads even one byte past the row faults; the fault is caught
 * and the probe retries with one more readable byte of slack. The first slack
 * value that survives IS the kernel's over-read extent.
 *
 * The platform layer below is two implementations of the same five entry
 * points: `probe_page_size`, `guarded_row_alloc`, `guarded_row_free`,
 * `fault_trap_install` / `fault_trap_restore`, and `run_kernel_guarded`.
 * ---------------------------------------------------------------------- */

typedef struct GuardedRow {
    uint8_t *map;   /* base of the two-page mapping */
    size_t map_len; /* total mapped bytes */
    uint8_t *row;   /* row start; row + readable == guard page */
} GuardedRow;

static void run_kernel(const GuardedRow g[3], float *const out[3], int w, int elem_size)
{
    if (elem_size == 1) {
        ciede_preprocess_8_neon(g[0].row, g[1].row, g[2].row, out[0], out[1], out[2], w);
    } else {
        ciede_preprocess_16_neon((const uint16_t *)g[0].row, (const uint16_t *)g[1].row,
                                 (const uint16_t *)g[2].row, out[0], out[1], out[2], w);
    }
}

#if defined(_WIN32)

/* SEH needs no process-wide state; the struct only keeps both platform
 * variants of the probe on the same call shape. */
typedef struct FaultTrap {
    int armed;
} FaultTrap;

static long probe_page_size(void)
{
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return (long)info.dwPageSize;
}

/* Commits [readable-page][PAGE_NOACCESS page] and returns a row whose last
 * readable byte is the last byte of the first page. Returns 0 on success. */
static int guarded_row_alloc(GuardedRow *g, size_t readable, long page)
{
    DWORD old_protect = 0;
    g->map_len = (size_t)page * 2u;
    g->map = VirtualAlloc(NULL, g->map_len, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!g->map) {
        return -1;
    }
    if (!VirtualProtect(g->map + page, (size_t)page, PAGE_NOACCESS, &old_protect)) {
        (void)VirtualFree(g->map, 0, MEM_RELEASE);
        g->map = NULL;
        return -1;
    }
    g->row = g->map + (size_t)page - readable;
    return 0;
}

static void guarded_row_free(GuardedRow *g)
{
    if (g->map) {
        (void)VirtualFree(g->map, 0, MEM_RELEASE);
    }
    g->map = NULL;
}

static int fault_trap_install(FaultTrap *trap)
{
    trap->armed = 1;
    return 0;
}

static void fault_trap_restore(FaultTrap *trap)
{
    trap->armed = 0;
}

/* Returns 1 if the kernel ran to completion, 0 if it faulted on the guard. Any
 * exception other than an access violation is not ours and keeps propagating. */
static int run_kernel_guarded(const GuardedRow g[3], float *const out[3], int w, int elem_size)
{
    __try {
        run_kernel(g, out, w, elem_size);
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER :
                                                                   EXCEPTION_CONTINUE_SEARCH) {
        return 0;
    }
    return 1;
}

#else /* POSIX */

typedef struct FaultTrap {
    struct sigaction old_segv;
    struct sigaction old_bus;
} FaultTrap;

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

static long probe_page_size(void)
{
    return sysconf(_SC_PAGESIZE);
}

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
    if (g->map) {
        (void)munmap(g->map, g->map_len);
    }
    g->map = NULL;
}

static int fault_trap_install(FaultTrap *trap)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = fault_handler;
    (void)sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_NODEFER;
    if (sigaction(SIGSEGV, &sa, &trap->old_segv) != 0) {
        return -1;
    }
    if (sigaction(SIGBUS, &sa, &trap->old_bus) != 0) {
        (void)sigaction(SIGSEGV, &trap->old_segv, NULL);
        return -1;
    }
    return 0;
}

static void fault_trap_restore(FaultTrap *trap)
{
    (void)sigaction(SIGSEGV, &trap->old_segv, NULL);
    (void)sigaction(SIGBUS, &trap->old_bus, NULL);
}

/* Returns 1 if the kernel ran to completion, 0 if it faulted on the guard.
 * `survived` is live across the siglongjmp, so it must be volatile: a
 * register-allocated local would be restored to its pre-sigsetjmp value when
 * the handler unwinds (C11 7.13.2.1p3). */
static int run_kernel_guarded(const GuardedRow g[3], float *const out[3], int w, int elem_size)
{
    volatile int survived = 0;
    g_fault_armed = 1;
    if (sigsetjmp(g_fault_jmp, 1) == 0) {
        run_kernel(g, out, w, elem_size);
        survived = 1;
    }
    g_fault_armed = 0;
    return survived;
}

#endif /* _WIN32 */

/* Fills three guarded rows with `readable` pseudo-random bytes and runs the
 * kernel over them. Returns 1 if the kernel survived, 0 if it faulted, -1 if a
 * row could not be mapped. */
static int probe_slack(int w, int elem_size, int slack, long page, float *const out[3])
{
    const size_t readable = ((size_t)w + (size_t)slack) * (size_t)elem_size;
    GuardedRow g[3] = {{NULL, 0, NULL}, {NULL, 0, NULL}, {NULL, 0, NULL}};
    uint32_t seed = 0xC1EDE000u ^ (uint32_t)(w * 7 + slack);
    int mapped = 0;
    int survived = -1;

    for (int k = 0; k < 3; k++) {
        if (guarded_row_alloc(&g[k], readable, page) != 0) {
            break;
        }
        for (size_t b = 0; b < readable; b++) {
            g[k].row[b] = (uint8_t)xorshift32(&seed);
        }
        mapped++;
    }
    if (mapped == 3) {
        survived = run_kernel_guarded(g, out, w, elem_size);
    }
    for (int k = 0; k < mapped; k++) {
        guarded_row_free(&g[k]);
    }
    return survived;
}

static int probe_outputs_alloc(float *out[3], int w)
{
    for (int k = 0; k < 3; k++) {
        out[k] = calloc((size_t)w + 8u, sizeof(float));
        if (!out[k]) {
            return -1;
        }
    }
    return 0;
}

static void probe_outputs_free(float *out[3])
{
    for (int k = 0; k < 3; k++) {
        free(out[k]);
        out[k] = NULL;
    }
}

/* Returns the smallest number of readable bytes past `w` elements that lets
 * the kernel run without faulting, or -1 if it exceeds `max_slack` or the
 * probe could not be set up. */
static int probe_overread(int w, int elem_size, int max_slack)
{
    const long page = probe_page_size();
    float *out[3] = {NULL, NULL, NULL};
    FaultTrap trap;
    int result = -1;

    if (probe_outputs_alloc(out, w) == 0 && fault_trap_install(&trap) == 0) {
        for (int slack = 0; slack <= max_slack; slack++) {
            const size_t readable = ((size_t)w + (size_t)slack) * (size_t)elem_size;
            int survived;
            if (readable > (size_t)page) {
                break;
            }
            survived = probe_slack(w, elem_size, slack, page, out);
            if (survived < 0) {
                break;
            }
            if (survived) {
                result = slack * elem_size;
                break;
            }
        }
        fault_trap_restore(&trap);
    }
    probe_outputs_free(out);
    return result;
}

/* -------------------------------------------------------------------------
 * Value parity.
 * ---------------------------------------------------------------------- */

typedef struct ParityBuffers {
    void *in[3];    /* packed input rows, uint8_t or uint16_t */
    float *ref[3];  /* scalar output */
    float *simd[3]; /* NEON output */
} ParityBuffers;

static void parity_buffers_free(ParityBuffers *b)
{
    for (int k = 0; k < 3; k++) {
        free(b->in[k]);
        free(b->ref[k]);
        free(b->simd[k]);
        b->in[k] = NULL;
        b->ref[k] = NULL;
        b->simd[k] = NULL;
    }
}

/* Allocates every buffer first and only then checks, so a partial failure
 * releases what it got (consolidated guard, core/test/AGENTS.md). The two
 * output sets start from different fill bytes so an untouched element cannot
 * pass as a match. Returns 0 on success. */
static int parity_buffers_alloc(ParityBuffers *b, size_t in_bytes, size_t out_bytes)
{
    int ok = 1;
    for (int k = 0; k < 3; k++) {
        b->in[k] = malloc(in_bytes);
        b->ref[k] = malloc(out_bytes);
        b->simd[k] = malloc(out_bytes);
        ok = ok && b->in[k] && b->ref[k] && b->simd[k];
    }
    if (!ok) {
        parity_buffers_free(b);
        return -1;
    }
    for (int k = 0; k < 3; k++) {
        memset(b->ref[k], 0xA5, out_bytes);
        memset(b->simd[k], 0x5A, out_bytes);
    }
    return 0;
}

/* Bit-for-bit float comparison: parity means the same 32 bits, so -0.0f and
 * +0.0f differ and two NaNs with the same payload agree. */
static int float_bits_differ(float a, float b)
{
    uint32_t ua;
    uint32_t ub;
    memcpy(&ua, &a, sizeof(ua));
    memcpy(&ub, &b, sizeof(ub));
    return ua != ub;
}

static int count_mismatches(const ParityBuffers *b, int w)
{
    int mismatches = 0;
    for (int k = 0; k < 3; k++) {
        for (int j = 0; j < w; j++) {
            if (!float_bits_differ(b->ref[k][j], b->simd[k][j])) {
                continue;
            }
            if (mismatches < 4) {
                (void)fprintf(stderr, "  w=%d plane %d idx %d: scalar %.9g != neon %.9g\n", w, k, j,
                              (double)b->ref[k][j], (double)b->simd[k][j]);
            }
            mismatches++;
        }
    }
    return mismatches;
}

static int check_parity_8(int w, uint32_t seed)
{
    ParityBuffers b;
    int mismatches;

    if (parity_buffers_alloc(&b, (size_t)w, (size_t)w * sizeof(float)) != 0) {
        return -1;
    }
    for (int k = 0; k < 3; k++) {
        uint8_t *in = b.in[k];
        for (int j = 0; j < w; j++) {
            in[j] = (uint8_t)xorshift32(&seed);
        }
    }

    ciede_preprocess_8_scalar(b.in[0], b.in[1], b.in[2], b.ref[0], b.ref[1], b.ref[2], w);
    ciede_preprocess_8_neon(b.in[0], b.in[1], b.in[2], b.simd[0], b.simd[1], b.simd[2], w);

    mismatches = count_mismatches(&b, w);
    parity_buffers_free(&b);
    return mismatches;
}

static int check_parity_16(int w, uint32_t seed)
{
    ParityBuffers b;
    int mismatches;

    if (parity_buffers_alloc(&b, (size_t)w * sizeof(uint16_t), (size_t)w * sizeof(float)) != 0) {
        return -1;
    }
    for (int k = 0; k < 3; k++) {
        uint16_t *in = b.in[k];
        for (int j = 0; j < w; j++) {
            in[j] = (uint16_t)xorshift32(&seed);
        }
    }

    ciede_preprocess_16_scalar(b.in[0], b.in[1], b.in[2], b.ref[0], b.ref[1], b.ref[2], w);
    ciede_preprocess_16_neon(b.in[0], b.in[1], b.in[2], b.simd[0], b.simd[1], b.simd[2], w);

    mismatches = count_mismatches(&b, w);
    parity_buffers_free(&b);
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
        if ((size_t)w + 32u > (size_t)probe_page_size()) {
            continue;
        }
        over = probe_overread(w, 1, 32);
        mu_assert("ciede_preprocess_8_neon reads more than 32 bytes past the row", over >= 0);
        if (over > 0) {
            (void)fprintf(stderr, "  w=%d: ciede_preprocess_8_neon reads %d byte(s) past buf+w\n",
                          w, over);
            if (over > worst) {
                worst = over;
            }
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
        if (((size_t)w + 32u) * 2u > (size_t)probe_page_size()) {
            continue;
        }
        over = probe_overread(w, 2, 32);
        mu_assert("ciede_preprocess_16_neon reads more than 64 bytes past the row", over >= 0);
        if (over > 0) {
            (void)fprintf(stderr, "  w=%d: ciede_preprocess_16_neon reads %d byte(s) past buf+w\n",
                          w, over);
            if (over > worst) {
                worst = over;
            }
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

/* NOLINTEND(modernize-use-nullptr) */
