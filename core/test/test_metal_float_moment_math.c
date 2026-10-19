/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * The second-moment term of float_moment_metal (core/src/feature/metal/
 * metal_float_moment_math.h, ADR-1498; T-GPU-FLOAT-MOMENT-16BIT-SQUARES-
 * 2026-10-02) against moment.c, on the host. The header is the kernel's
 * arithmetic, compiled here as C.
 *
 * moment.c::compute_2nd_moment() forms each square in float from the sample
 * picture_copy() divided by scaler = 2^(bpc - 8). The cases:
 *   - every sample value at 8, 10, 12 and 16 bits, converted by the CPU's
 *     picture_copy() and squared by the CPU's compute_2nd_moment() on a
 *     one-pixel plane: the twin's term is that square times scaler^2,
 *     exactly. At 16 bits most squares are not the integer square;
 *   - full-range noise at 576x324 for every depth and a bright 16-bit
 *     1920x1080 frame (samples 56000 to 64000, the cases of
 *     float_moment_twin_parity.h): compute_1st_moment() and
 *     compute_2nd_moment() over the plane equal the twin's moments (uint64
 *     sums of the samples and of the header's squares, divided as
 *     float_moment_metal.mm divides them), while exact integer squares, the
 *     kernel's former term, give another second moment at 16 bits.
 *
 * Host-only: no device.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"
#include "float_bits.h"

#include "feature/metal/metal_float_moment_math.h"
#include "feature/moment.h"
#include "feature/picture_copy.h"
#include "libvmaf/picture.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy proposes `nullptr`, but the required MSVC C build does
 * not provide that keyword. Preserve the portable C spelling. ADR-1138. */

/* A frame of luma samples in [lo, hi]. */
typedef struct MomentFrame {
    unsigned w;
    unsigned h;
    unsigned bpc;
    unsigned lo;
    unsigned hi;
} MomentFrame;

static double scaler_of(unsigned bpc)
{
    return (double)(1u << (bpc - 8u));
}

/* A one-plane picture over caller-owned samples, as picture_copy() reads it. */
static VmafPicture plane_picture(void *data, unsigned bpc, unsigned w, unsigned h)
{
    VmafPicture pic;
    memset(&pic, 0, sizeof(pic));
    pic.pix_fmt = VMAF_PIX_FMT_YUV400P;
    pic.bpc = bpc;
    pic.w[0] = w;
    pic.h[0] = h;
    pic.stride[0] = (ptrdiff_t)w * (bpc > 8u ? 2 : 1);
    pic.data[0] = data;
    return pic;
}

static void put_sample(void *data, unsigned bpc, size_t i, unsigned v)
{
    if (bpc > 8u) {
        ((uint16_t *)data)[i] = (uint16_t)v;
    } else {
        ((uint8_t *)data)[i] = (uint8_t)v;
    }
}

static unsigned get_sample(const void *data, unsigned bpc, size_t i)
{
    return bpc > 8u ? ((const uint16_t *)data)[i] : ((const uint8_t *)data)[i];
}

/* Terms that are not the CPU's (`bad`) and squares the float rounds
 * (`rounded`), for the samples 0 .. n - 1 converted into `f`. */
static void compare_squares(const float *f, unsigned bpc, unsigned n, unsigned *bad,
                            unsigned *rounded)
{
    const double s2 = scaler_of(bpc) * scaler_of(bpc);
    for (unsigned v = 0; v < n; v++) {
        double cpu = -1.0;
        const int err = compute_2nd_moment(&f[v], 1, 1, (int)sizeof(float), &cpu);
        const vmaf_mtl_u32 term = vmaf_mtl_moment_float_square(v);
        if (err != 0 || !vmaf_test_identical_f64((double)term, cpu * s2)) {
            (*bad)++;
        }
        if ((uint64_t)term != (uint64_t)v * (uint64_t)v) {
            (*rounded)++;
        }
    }
}

/* Every sample value at `bpc`; `rounded` counts the squares the float rounds. */
static char *check_every_sample(unsigned bpc, unsigned *rounded)
{
    const unsigned n = 1u << bpc;
    void *raw = malloc((size_t)n * 2u);
    float *f = malloc((size_t)n * sizeof(float));
    unsigned bad = 0u;
    *rounded = 0u;
    const int allocated = raw && f;
    if (allocated) {
        for (unsigned v = 0; v < n; v++) {
            put_sample(raw, bpc, v, v);
        }
        VmafPicture pic = plane_picture(raw, bpc, n, 1u);
        picture_copy(f, (ptrdiff_t)(n * sizeof(float)), &pic, 0, bpc, 0);
        compare_squares(f, bpc, n, &bad, rounded);
    }
    free(raw);
    free(f);
    mu_assert("allocation failed", allocated);
    if (bad) {
        (void)fprintf(stderr, "\n  %u-bit: %u squares differ from the CPU's\n", bpc, bad);
    }
    mu_assert("a term is not moment.c's float square times scaler^2", bad == 0u);
    return NULL;
}

static char *test_every_sample_8_to_12bit(void)
{
    static const unsigned depths[3] = {8u, 10u, 12u};
    for (unsigned i = 0; i < 3u; i++) {
        unsigned rounded = 1u;
        mu_assert_msg(check_every_sample(depths[i], &rounded));
        mu_assert("squares up to 12 bits are exact in float", rounded == 0u);
    }
    return NULL;
}

/* At 16 bits the CPU's float rounds most squares above 2^24; the twin must
 * return those rounded values. */
static char *test_every_sample_16bit(void)
{
    unsigned rounded = 0u;
    mu_assert_msg(check_every_sample(16u, &rounded));
    mu_assert("16-bit squares above 2^24 are rounded on the CPU", rounded > 0u);
    return NULL;
}

/* lowbias32 hash of a position. */
static unsigned frame_sample(const MomentFrame *c, size_t i)
{
    uint32_t x = (uint32_t)i * 0x9E3779B9u;
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return c->lo + (x % (c->hi - c->lo + 1u));
}

/* The twin's moments: uint64 sums of the samples, of the header's squares
 * and (for the former kernel) of the exact integer squares, divided as
 * float_moment_metal.mm::collect_fex_metal() divides them. */
static void twin_moments(const void *raw, const MomentFrame *c, double m[3])
{
    const size_t n = (size_t)c->w * c->h;
    uint64_t sum1 = 0u;
    uint64_t sum2 = 0u;
    uint64_t exact2 = 0u;
    for (size_t i = 0; i < n; i++) {
        const unsigned v = get_sample(raw, c->bpc, i);
        sum1 += v;
        sum2 += c->bpc > 8u ? vmaf_mtl_moment_float_square(v) : (uint64_t)v * v;
        exact2 += (uint64_t)v * v;
    }
    const double n_pix = (double)c->w * (double)c->h;
    const double denom1 = n_pix * scaler_of(c->bpc);
    const double denom2 = denom1 * scaler_of(c->bpc);
    m[0] = (double)sum1 / denom1;
    m[1] = (double)sum2 / denom2;
    m[2] = (double)exact2 / denom2;
}

/* The frame through the CPU's functions and through the twin's arithmetic;
 * `exact_differs` says whether integer squares give another second moment. */
static char *check_frame(const MomentFrame *c, int *exact_differs)
{
    const size_t n = (size_t)c->w * c->h;
    void *raw = malloc(n * 2u);
    float *f = malloc(n * sizeof(float));
    double cpu1 = 0.0;
    double cpu2 = 0.0;
    double twin[3] = {-1.0, -1.0, -1.0};
    int err = -1;
    const int allocated = raw && f;
    if (allocated) {
        for (size_t i = 0; i < n; i++) {
            put_sample(raw, c->bpc, i, frame_sample(c, i));
        }
        VmafPicture pic = plane_picture(raw, c->bpc, c->w, c->h);
        const int stride = (int)(c->w * sizeof(float));
        picture_copy(f, stride, &pic, 0, c->bpc, 0);
        err = compute_1st_moment(f, (int)c->w, (int)c->h, stride, &cpu1);
        err |= compute_2nd_moment(f, (int)c->w, (int)c->h, stride, &cpu2);
        twin_moments(raw, c, twin);
    }
    free(raw);
    free(f);
    mu_assert("allocation failed", allocated);
    mu_assert("the CPU's moment functions failed", err == 0);
    *exact_differs = !vmaf_test_identical_f64(twin[2], cpu2);
    mu_assert("the twin's first moment is not moment.c's", vmaf_test_identical_f64(twin[0], cpu1));
    mu_assert("the twin's second moment is not moment.c's", vmaf_test_identical_f64(twin[1], cpu2));
    return NULL;
}

static char *test_noise_frames(void)
{
    static const unsigned depths[4] = {8u, 10u, 12u, 16u};
    int exact_differs[4] = {0, 0, 0, 0};
    for (unsigned i = 0; i < 4u; i++) {
        const MomentFrame c = {576u, 324u, depths[i], 0u, (1u << depths[i]) - 1u};
        mu_assert_msg(check_frame(&c, &exact_differs[i]));
    }
    mu_assert("integer squares are the CPU's up to 12 bits",
              !exact_differs[0] && !exact_differs[1] && !exact_differs[2]);
    mu_assert("the 16-bit noise frame shows the float squares", exact_differs[3]);
    return NULL;
}

static char *test_bright_16bit_1080p(void)
{
    const MomentFrame c = {1920u, 1080u, 16u, 56000u, 64000u};
    int exact_differs = 0;
    mu_assert_msg(check_frame(&c, &exact_differs));
    mu_assert("the bright 16-bit frame shows the float squares", exact_differs);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_every_sample_8_to_12bit);
    mu_run_test(test_every_sample_16bit);
    mu_run_test(test_noise_frames);
    mu_run_test(test_bright_16bit_1080p);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
