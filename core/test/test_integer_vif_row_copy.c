/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  integer_vif copies each luma row's samples, not the picture's stride
 *  (Netflix/vmaf 9f4bd165f).
 *
 *  A caller's picture may have a stride far larger than its rows (a wrapped
 *  decoder frame, a crop of a larger plane), and its last row need not extend
 *  to a full stride. integer_vif used to copy `stride` bytes per row into its
 *  own buffer: it read past the end of such a picture and wrote past the rows
 *  of its buffer. Each case here lays the luma plane out with a wide stride
 *  in memory that ends exactly at the last row's last sample, followed by an
 *  inaccessible page (anonymous mmap + mprotect, or VirtualAlloc +
 *  VirtualProtect; the page size is the system's, 16 KiB on Apple silicon),
 *  so an over-read faults; it then
 *  requires every VIF score to equal, bit for bit, the score of the same
 *  samples at the library's own stride. 8-bit and 10-bit, the wide plane on
 *  the reference, on the distorted picture and on both.
 */

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

#include "test.h"

#include "feature/feature_collector.h"
#include "feature/feature_extractor.h"
#include "libvmaf/picture.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe. ADR-1138. */

#define ROW_W 72u
#define ROW_H 40u
#define WIDE_FACTOR 5u /* the wide stride is five rows' worth of bytes */
#define SCORES 4u

static const char *const SCORE_KEYS[SCORES] = {
    "VMAF_integer_feature_vif_scale0_score",
    "VMAF_integer_feature_vif_scale1_score",
    "VMAF_integer_feature_vif_scale2_score",
    "VMAF_integer_feature_vif_scale3_score",
};

/* `bytes` writable bytes that end exactly where an inaccessible page begins. */
typedef struct GuardedPlane {
    uint8_t *data;
    void *base;
    size_t len;
} GuardedPlane;

static size_t page_size(void)
{
#if defined(_WIN32)
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return (size_t)info.dwPageSize;
#else
    const long page = sysconf(_SC_PAGESIZE);
    return page > 0 ? (size_t)page : 4096u;
#endif
}

#if !defined(_WIN32)
/* `len` bytes of private anonymous memory. macOS and the BSDs spell the flag
 * MAP_ANON, glibc and POSIX.1-2024 MAP_ANONYMOUS; a system with neither maps
 * /dev/zero, which macOS refuses (ENODEV), so that is the last resort only. */
static void *map_anonymous(size_t len)
{
#if defined(MAP_ANONYMOUS)
    return mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#elif defined(MAP_ANON)
    return mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
#else
    const int fd = open("/dev/zero", O_RDWR);
    void *const base =
        fd < 0 ? MAP_FAILED : mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    if (fd >= 0 && close(fd) != 0 && base != MAP_FAILED) {
        (void)munmap(base, len);
        return MAP_FAILED;
    }
    return base;
#endif
}
#endif

static int guarded_alloc(GuardedPlane *g, size_t bytes)
{
    const size_t page = page_size();
    g->len = (((bytes + page - 1u) / page) + 1u) * page;
#if defined(_WIN32)
    g->base = VirtualAlloc(NULL, g->len, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    DWORD old = 0;
    if (!g->base ||
        !VirtualProtect((uint8_t *)g->base + g->len - page, page, PAGE_NOACCESS, &old)) {
        return -1;
    }
#else
    g->base = map_anonymous(g->len);
    if (g->base == MAP_FAILED || g->base == NULL ||
        mprotect((uint8_t *)g->base + g->len - page, page, PROT_NONE)) {
        return -1;
    }
#endif
    g->data = (uint8_t *)g->base + (g->len - page - bytes);
    return 0;
}

static void guarded_free(GuardedPlane *g)
{
#if defined(_WIN32)
    if (g->base) {
        (void)VirtualFree(g->base, 0, MEM_RELEASE);
    }
#else
    if (g->base && g->base != MAP_FAILED) {
        (void)munmap(g->base, g->len);
    }
#endif
    g->base = NULL;
}

/* xorshift32: deterministic texture without rand() (banned, principles.md). */
static uint32_t next_random(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

/* One sample of plane `p` at (x, y), 8-bit or 16-bit storage. */
static void put_sample(VmafPicture *pic, unsigned p, unsigned x, unsigned y, unsigned v)
{
    uint8_t *row = (uint8_t *)pic->data[p] + ((size_t)y * (size_t)pic->stride[p]);
    if (pic->bpc == 8u) {
        row[x] = (uint8_t)v;
    } else {
        ((uint16_t *)row)[x] = (uint16_t)v;
    }
}

/* Fill every plane of `pic`; the distorted picture adds noise to the luma. */
static void fill_picture(VmafPicture *pic, int distorted)
{
    const unsigned levels = 1u << pic->bpc;
    uint32_t seed = distorted ? 0x9e3779b9u : 0x85ebca6bu;
    for (unsigned p = 0; p < 3u; p++) {
        const int noisy = distorted && p == 0u;
        for (unsigned y = 0; y < pic->h[p]; y++) {
            for (unsigned x = 0; x < pic->w[p]; x++) {
                const unsigned v = (x * 37u + y * 11u) % levels;
                put_sample(pic, p, x, y, noisy ? (v + (next_random(&seed) % 23u)) % levels : v);
            }
        }
    }
}

/* A view of `pic` whose luma lies in `g` with a wide stride: the same samples,
 * the last row ending at the guard page. */
static int wide_view(const VmafPicture *pic, GuardedPlane *g, VmafPicture *view)
{
    const size_t row_bytes = (size_t)pic->w[0] << (pic->bpc > 8u);
    const size_t stride = row_bytes * WIDE_FACTOR;
    if (guarded_alloc(g, (stride * (pic->h[0] - 1u)) + row_bytes)) {
        return -1;
    }
    for (unsigned y = 0; y < pic->h[0]; y++) {
        memcpy(g->data + ((size_t)y * stride),
               (const uint8_t *)pic->data[0] + ((size_t)y * (size_t)pic->stride[0]), row_bytes);
    }
    *view = *pic;
    view->data[0] = g->data;
    view->stride[0] = (ptrdiff_t)stride;
    return 0;
}

/* The four VIF scores of one frame of `ref` against `dist`. */
static int vif_scores(VmafPicture *ref, VmafPicture *dist, double out[SCORES])
{
    VmafFeatureExtractorContext *ctx = NULL;
    VmafFeatureCollector *fc = NULL;
    int err = vmaf_feature_extractor_context_create(&ctx, vmaf_get_feature_extractor_by_name("vif"),
                                                    NULL);
    if (!err) {
        err = vmaf_feature_collector_init(&fc);
    }
    if (!err) {
        err = vmaf_feature_extractor_context_extract(ctx, ref, NULL, dist, NULL, 0u, fc);
    }
    for (unsigned k = 0; k < SCORES && !err; k++) {
        err = vmaf_feature_collector_get_score(fc, SCORE_KEYS[k], &out[k], 0u);
    }
    if (ctx) {
        (void)vmaf_feature_extractor_context_close(ctx);
        (void)vmaf_feature_extractor_context_destroy(ctx);
    }
    if (fc) {
        vmaf_feature_collector_destroy(fc);
    }
    return err;
}

static int same_bits(double a, double b)
{
    uint64_t x = 0;
    uint64_t y = 0;
    memcpy(&x, &a, sizeof(x));
    memcpy(&y, &b, sizeof(y));
    return x == y;
}

/* Which picture of the pair gets the wide luma. */
enum WideSide { WIDE_REF = 1, WIDE_DIST = 2, WIDE_BOTH = 3 };

/* The scores of `ref` against `dist` with the luma of `side` in guarded wide
 * planes. Sets `*guarded` when a plane could not be mapped; then nothing is
 * scored and the result is -1. */
static int wide_scores(VmafPicture *ref, VmafPicture *dist, enum WideSide side, double out[SCORES],
                       int *guarded)
{
    GuardedPlane g_ref = {0};
    GuardedPlane g_dist = {0};
    VmafPicture ref_view = *ref;
    VmafPicture dist_view = *dist;
    int unmapped = 0;
    if (side & WIDE_REF) {
        unmapped |= wide_view(ref, &g_ref, &ref_view);
    }
    if (side & WIDE_DIST) {
        unmapped |= wide_view(dist, &g_dist, &dist_view);
    }
    *guarded = unmapped;
    const int err = unmapped ? -1 : vif_scores(&ref_view, &dist_view, out);
    guarded_free(&g_ref);
    guarded_free(&g_dist);
    return err;
}

/* Every score finite, and each wide score the same bits as its tight one. */
static char *same_scores(const double tight[SCORES], const double wide[SCORES])
{
    for (unsigned k = 0; k < SCORES; k++) {
        mu_assert("vif score is finite", isfinite(tight[k]));
        mu_assert("a wide stride changes no VIF score", same_bits(tight[k], wide[k]));
    }
    return NULL;
}

/* The scores with the wide luma on `side` equal the scores at the library's
 * stride. */
static char *check_wide(unsigned bpc, enum WideSide side)
{
    VmafPicture ref;
    VmafPicture dist;
    mu_assert("alloc ref", !vmaf_picture_alloc(&ref, VMAF_PIX_FMT_YUV420P, bpc, ROW_W, ROW_H));
    mu_assert("alloc dist", !vmaf_picture_alloc(&dist, VMAF_PIX_FMT_YUV420P, bpc, ROW_W, ROW_H));
    fill_picture(&ref, 0);
    fill_picture(&dist, 1);

    double tight[SCORES] = {0};
    double wide[SCORES] = {0};
    int guarded = 0;
    const int scored = vif_scores(&ref, &dist, tight);
    const int scored_wide = scored ? -1 : wide_scores(&ref, &dist, side, wide, &guarded);
    (void)vmaf_picture_unref(&ref);
    (void)vmaf_picture_unref(&dist);
    mu_assert("scoring at the library's stride failed", !scored);
    mu_assert("the guarded plane could not be mapped", !guarded);
    mu_assert("scoring the wide stride failed", !scored_wide);
    return same_scores(tight, wide);
}

static char *test_8bit_wide_strides(void)
{
    char *msg = check_wide(8u, WIDE_REF);
    if (!msg) {
        msg = check_wide(8u, WIDE_DIST);
    }
    return msg ? msg : check_wide(8u, WIDE_BOTH);
}

static char *test_10bit_wide_strides(void)
{
    char *msg = check_wide(10u, WIDE_REF);
    if (!msg) {
        msg = check_wide(10u, WIDE_DIST);
    }
    return msg ? msg : check_wide(10u, WIDE_BOTH);
}

char *run_tests(void)
{
    mu_run_test(test_8bit_wide_strides);
    mu_run_test(test_10bit_wide_strides);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
