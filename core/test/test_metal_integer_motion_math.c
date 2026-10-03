/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * The arithmetic of integer_motion_metal (core/src/feature/metal/
 * metal_integer_motion_math.h, ADR-1498; T-METAL-MOTION-BLUR-THEN-DIFF-
 * 2026-09-29) against the CPU `motion` extractor, on the host. The header is
 * the kernel's arithmetic, compiled here as C, and twin_sad() runs it in the
 * kernel's layout: 16x16 threadgroups, a 20x20 tile of prev - cur loaded
 * through vmaf_mtl_motion_mirror(), the vertical pass at 16 rows and 20
 * columns, the horizontal pass and |h| per pixel, a uint sum per threadgroup
 * and a uint64 sum over them.
 *
 * The cases:
 *   - the mirror: for every size from 3 to 80 and every index a tile loads,
 *     the result is in the plane, and within two samples of it the result is
 *     the CPU's reflect-101 (integer_motion.c::mirror());
 *   - the SAD score of frame 1, the CPU extractor's
 *     VMAF_integer_feature_motion_sad_score through the public API against
 *     integer_motion_metal.mm's formula on twin_sad(), at 3x3, 4x5, 17x17,
 *     19x23, 33x33, 31x9, 64x64 and 257x145, at 8, 10, 12 and 16 bits, on
 *     full-range noise, a uniform full-scale step and full-scale stripes;
 *   - the same frames blurred first and differenced after, the kernel's
 *     former order, give another SAD on some of them.
 *
 * Host-only: no device.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "test.h"

#include "feature/integer_motion.h"
#include "feature/metal/metal_integer_motion_math.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/picture.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy proposes `nullptr`, but the required MSVC C build does
 * not provide that keyword. Preserve the portable C spelling. ADR-1138. */

enum { GROUP = 16, HALF = 2, TILE = 20 };

typedef enum Kind { KIND_NOISE, KIND_STEP, KIND_STRIPES } Kind;

typedef struct Fixture {
    Kind kind;
    unsigned bpc;
    unsigned w;
    unsigned h;
} Fixture;

/* lowbias32 hash of a position and a frame. */
static uint32_t hash32(unsigned row, unsigned col, unsigned frame)
{
    uint32_t x = ((uint32_t)row << 16) ^ (uint32_t)col ^ (frame * 0x9E3779B9u);
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

/* Luma sample of frame `frame` (0 = prev, 1 = cur). */
static int sample(const Fixture *fx, unsigned frame, unsigned row, unsigned col)
{
    const int max = (1 << fx->bpc) - 1;
    if (fx->kind == KIND_NOISE) {
        return (int)(hash32(row, col, frame) >> (32u - fx->bpc));
    }
    if (fx->kind == KIND_STEP) {
        return frame ? 0 : max;
    }
    const int on = ((col / 3u) & 1u) != 0u;
    return (on != (int)frame) ? max : 0;
}

/* prev - cur at a mirrored position, through the kernel's tile index. */
static int tile_diff(const Fixture *fx, int y, int x)
{
    const int my = vmaf_mtl_motion_mirror(y, (int)fx->h);
    const int mx = vmaf_mtl_motion_mirror(x, (int)fx->w);
    return sample(fx, 0u, (unsigned)my, (unsigned)mx) - sample(fx, 1u, (unsigned)my, (unsigned)mx);
}

/* One threadgroup of integer_motion.metal: its uint sum of |h|. */
static uint32_t twin_group(const Fixture *fx, unsigned gx, unsigned gy)
{
    int diff[TILE * TILE];
    int vert[GROUP * TILE];
    const int oy = (int)(gy * GROUP) - HALF;
    const int ox = (int)(gx * GROUP) - HALF;
    for (unsigned i = 0; i < (unsigned)(TILE * TILE); i++) {
        diff[i] = tile_diff(fx, oy + (int)(i / TILE), ox + (int)(i % TILE));
    }
    for (unsigned i = 0; i < (unsigned)(GROUP * TILE); i++) {
        const unsigned top = ((i / TILE) * TILE) + (i % TILE);
        vert[i] = vmaf_mtl_motion_vertical(diff[top], diff[top + TILE], diff[top + (2 * TILE)],
                                           diff[top + (3 * TILE)], diff[top + (4 * TILE)], fx->bpc);
    }
    uint32_t total = 0u;
    for (unsigned ly = 0; ly < (unsigned)GROUP; ly++) {
        for (unsigned lx = 0; lx < (unsigned)GROUP; lx++) {
            if ((gx * GROUP) + lx >= fx->w || (gy * GROUP) + ly >= fx->h) {
                continue;
            }
            const unsigned b = (ly * TILE) + lx;
            total += vmaf_mtl_motion_abs_h(vert[b], vert[b + 1u], vert[b + 2u], vert[b + 3u],
                                           vert[b + 4u]);
        }
    }
    return total;
}

static uint64_t twin_sad(const Fixture *fx)
{
    uint64_t sad = 0u;
    for (unsigned gy = 0; gy < (fx->h + GROUP - 1u) / GROUP; gy++) {
        for (unsigned gx = 0; gx < (fx->w + GROUP - 1u) / GROUP; gx++) {
            sad += twin_group(fx, gx, gy);
        }
    }
    return sad;
}

/* integer_motion_metal.mm::sad_score() at the default options. */
static double twin_score(const Fixture *fx)
{
    const unsigned w = fx->w;
    const unsigned h = fx->h;
    const double score = (double)twin_sad(fx) / 256. / (w * h) * 1.0;
    return (score < 10000.0) ? score : 10000.0;
}

/* The CPU's reflect-101 within two samples of a plane. */
static int reflect_101(int idx, int size)
{
    if (idx < 0) {
        return -idx;
    }
    return (idx >= size) ? ((2 * size) - idx - 2) : idx;
}

/* The kernel's former order: each frame blurred (vertical then horizontal,
 * each rounded), the blurred frames differenced. */
static int64_t blurred(const Fixture *fx, unsigned frame, int y, int x)
{
    int64_t acc = 0;
    for (int k = 0; k < 5; k++) {
        const int col = reflect_101(x - HALF + k, (int)fx->w);
        int64_t v = 0;
        for (int m = 0; m < 5; m++) {
            const int row = reflect_101(y - HALF + m, (int)fx->h);
            v += (int64_t)filter[m] * sample(fx, frame, (unsigned)row, (unsigned)col);
        }
        acc += (int64_t)filter[k] * ((v + ((int64_t)1 << (fx->bpc - 1u))) >> fx->bpc);
    }
    return (acc + 32768) >> 16;
}

static uint64_t old_sad(const Fixture *fx)
{
    uint64_t sad = 0u;
    for (unsigned y = 0; y < fx->h; y++) {
        for (unsigned x = 0; x < fx->w; x++) {
            const int64_t d = blurred(fx, 0u, (int)y, (int)x) - blurred(fx, 1u, (int)y, (int)x);
            sad += (uint64_t)(d < 0 ? -d : d);
        }
    }
    return sad;
}

static int put_frame(VmafPicture *pic, const Fixture *fx, unsigned frame)
{
    const int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, fx->bpc, fx->w, fx->h);
    for (unsigned p = 0; !err && p < 3u; p++) {
        for (unsigned row = 0; row < pic->h[p]; row++) {
            uint8_t *line = (uint8_t *)pic->data[p] + ((size_t)row * (size_t)pic->stride[p]);
            for (unsigned col = 0; col < pic->w[p]; col++) {
                const int v = p ? (1 << (fx->bpc - 1u)) : sample(fx, frame, row, col);
                if (fx->bpc > 8u) {
                    ((uint16_t *)line)[col] = (uint16_t)v;
                } else {
                    line[col] = (uint8_t)v;
                }
            }
        }
    }
    return err;
}

/* Frame `frame` through the context, as reference and distorted picture. */
static int feed(VmafContext *vmaf, const Fixture *fx, unsigned frame)
{
    VmafPicture ref;
    VmafPicture dist;
    int err = put_frame(&ref, fx, frame);
    if (err) {
        return err;
    }
    err = put_frame(&dist, fx, frame);
    if (err) {
        (void)vmaf_picture_unref(&ref);
        return err;
    }
    return vmaf_read_pictures(vmaf, &ref, &dist, frame);
}

/* The CPU `motion` extractor's SAD score of frame 1. */
static int cpu_score(const Fixture *fx, double *score)
{
    VmafContext *vmaf = NULL;
    const VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    int err = vmaf_init(&vmaf, cfg);
    if (!err) {
        err = vmaf_use_feature(vmaf, "motion", NULL);
    }
    for (unsigned frame = 0; !err && frame < 2u; frame++) {
        err = feed(vmaf, fx, frame);
    }
    if (!err) {
        err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    }
    if (!err) {
        err = vmaf_feature_score_at_index(vmaf, "VMAF_integer_feature_motion_sad_score", score, 1);
    }
    const int closed = vmaf ? vmaf_close(vmaf) : 0;
    return err ? err : closed;
}

static char *test_mirror_stays_in_the_plane(void)
{
    for (int size = 3; size <= 80; size++) {
        const int last = (((size + GROUP - 1) / GROUP) * GROUP) + GROUP + 1;
        for (int idx = -HALF; idx <= last; idx++) {
            const int m = vmaf_mtl_motion_mirror(idx, size);
            mu_assert("a tile index leaves the plane", m >= 0 && m < size);
            mu_assert("the mirror is not the CPU's reflect-101 next to the plane",
                      idx > size + 1 || m == reflect_101(idx, size));
        }
    }
    return NULL;
}

static const unsigned SIZES[8][2] = {{3u, 3u},   {4u, 5u},  {17u, 17u}, {19u, 23u},
                                     {33u, 33u}, {31u, 9u}, {64u, 64u}, {257u, 145u}};
static const unsigned DEPTHS[4] = {8u, 10u, 12u, 16u};

/* One fixture: `bad` counts twin scores that are not the CPU's, `old_off`
 * former-order SADs that are not the twin's. */
static char *check_fixture(const Fixture *fx, unsigned *bad, unsigned *old_off)
{
    double cpu = -1.0;
    mu_assert("the CPU motion extractor failed", cpu_score(fx, &cpu) == 0);
    const double twin = twin_score(fx);
    if (twin != cpu) {
        (*bad)++;
        (void)fprintf(stderr, "\n  %ux%u %u-bit kind %d: cpu=%.17g twin=%.17g\n", fx->w, fx->h,
                      fx->bpc, (int)fx->kind, cpu, twin);
    }
    *old_off += old_sad(fx) != twin_sad(fx);
    return NULL;
}

static char *test_sad_score_is_the_cpus(void)
{
    unsigned bad = 0u;
    unsigned old_off = 0u;
    unsigned cases = 0u;
    for (unsigned k = 0; k < 3u; k++) {
        for (unsigned d = 0; d < 4u; d++) {
            for (unsigned s = 0; s < 8u; s++) {
                const Fixture fx = {(Kind)k, DEPTHS[d], SIZES[s][0], SIZES[s][1]};
                mu_assert_msg(check_fixture(&fx, &bad, &old_off));
                cases++;
            }
        }
    }
    (void)fprintf(stderr, "[%u cases, former order off on %u] ", cases, old_off);
    mu_assert("the twin's SAD score is not the CPU motion extractor's", bad == 0u);
    mu_assert("blurring first never moves the SAD: the cases test nothing", old_off > 0u);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_mirror_stays_in_the_plane);
    mu_run_test(test_sad_score_is_the_cpus);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
