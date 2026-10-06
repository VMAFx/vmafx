/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * ciede2000 CPU vs. GPU twin: the fixtures, the cases and the comparison a
 * twin's parity test wraps (ADR-1436 for SYCL; the cases are those of
 * test_cuda_ciede_parity.c, ADR-1426, and of the SYCL test's former format
 * variants).
 *
 * A twin that evaluates ciede.c's expressions at the reference's precision
 * and adds the per-pixel values in the reference's raster order differs from
 * the CPU only where a pixel's value rounds to the neighbouring float: a
 * math-library call that is not correctly rounded, or, on a device without
 * fp64, a value an fp32 pair does not decide. A pixel one float step off
 * moves `45 - 20 * log10(mean)` by at most
 * 8.7 * 1.2e-7 * (value / mean) / pixels: 2.8e-11 times the pixel's weight at
 * 256x144. So the comparison is a tolerance, and a tight one: 1e-8 leaves
 * room for many such pixels, and a twin that computes in fp32 is 7e-8 to
 * 3e-7 away on these fixtures (1e-5 on video).
 *
 * A test describes its backend in one CiedeTwin and wraps the ciede_twin_*()
 * cases. Each comparison opens its own device state. Without a device a case
 * is skipped and the test exits 77.
 */

#ifndef LIBVMAF_TEST_CIEDE_TWIN_PARITY_H_
#define LIBVMAF_TEST_CIEDE_TWIN_PARITY_H_

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test.h"

#include "feature/feature_extractor.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/picture.h"

/* NOLINTBEGIN(modernize-use-nullptr): included by C translation units. The
 * fork builds C as C23, where clang-tidy proposes `nullptr`, but the required
 * MSVC C build does not provide that keyword. Preserve the portable C
 * spelling. ADR-1138. */

#ifndef FIXTURE_W
#define FIXTURE_W 256u
#endif
#ifndef FIXTURE_H
#define FIXTURE_H 144u
#endif

/* A test may tighten it before including this header (the Metal test holds
 * its twin to the parity gate's LIBM_TWINS bound, ADR-1496). */
#ifndef CIEDE_TWIN_TOL
#define CIEDE_TWIN_TOL 1e-8
#endif

/* One GPU backend's twin, as the shared cases drive it. */
typedef struct CiedeTwin {
    const char *extractor; /* registry name, e.g. "ciede_sycl" */
    const char *backend;   /* for messages, e.g. "SYCL" */
    /* Opens a device state. Non-zero: no device, the case is skipped. */
    int (*open)(void **state);
    int (*import)(VmafContext *vmaf, void *state);
    int (*close)(void *state);
} CiedeTwin;

typedef struct CiedeTwinCase {
    const char *what;
    unsigned w;
    unsigned h;
    unsigned bpc;
    enum VmafPixelFormat pix_fmt;
} CiedeTwinCase;

/* Deterministic position hash. */
static inline unsigned ciede_twin_hash(unsigned plane, unsigned row, unsigned col, unsigned salt)
{
    unsigned x = row * 73856093u ^ col * 19349663u ^ (salt * 4u + plane) * 83492791u;
    x ^= x >> 13;
    x *= 0x5bd1e995u;
    x ^= x >> 15;
    return x;
}

/* Sample in 8-bit levels: every plane varies, and the distorted picture is
 * the reference plus an error of a few levels, so the pixel differences span
 * near-neutral and saturated colours and small and large hue differences. */
static inline unsigned ciede_twin_sample(unsigned plane, unsigned row, unsigned col, bool distorted)
{
    const unsigned lo = (plane == 0u) ? 16u : 40u;
    const unsigned span = (plane == 0u) ? 220u : 176u;
    unsigned v = lo + ciede_twin_hash(plane, row >> 1, col >> 1, 1u) % span;
    if (distorted)
        v += ciede_twin_hash(plane, row, col, 2u) % 9u;
    return v;
}

static inline void ciede_twin_put_sample(VmafPicture *pic, unsigned plane, unsigned row,
                                         unsigned col, unsigned v)
{
    uint8_t *line = (uint8_t *)pic->data[plane] + (size_t)row * (size_t)pic->stride[plane];
    if (pic->bpc <= 8u) {
        line[col] = (uint8_t)v;
    } else {
        ((uint16_t *)line)[col] = (uint16_t)v;
    }
}

static inline int ciede_twin_fill_picture(VmafPicture *pic, const CiedeTwinCase *c, bool distorted)
{
    const int err = vmaf_picture_alloc(pic, c->pix_fmt, c->bpc, c->w, c->h);
    if (err)
        return err;
    const unsigned gain = 1u << (c->bpc - 8u);
    for (unsigned p = 0; p < 3; p++) {
        for (unsigned row = 0; row < pic->h[p]; row++) {
            for (unsigned col = 0; col < pic->w[p]; col++) {
                ciede_twin_put_sample(pic, p, row, col,
                                      ciede_twin_sample(p, row, col, distorted) * gain);
            }
        }
    }
    return 0;
}

/* Feed the case's frame, flush, and read the score. */
static inline mu_message_t ciede_twin_read(VmafContext *vmaf, const CiedeTwinCase *c, double *score)
{
    VmafPicture ref;
    VmafPicture dist;
    mu_assert("fill reference failed", !ciede_twin_fill_picture(&ref, c, false));
    mu_assert("fill distorted failed", !ciede_twin_fill_picture(&dist, c, true));
    mu_assert("vmaf_read_pictures failed", !vmaf_read_pictures(vmaf, &ref, &dist, 0u));
    mu_assert("vmaf_read_pictures(EOS) failed", !vmaf_read_pictures(vmaf, NULL, NULL, 0));
    mu_assert("vmaf_feature_score_at_index(ciede2000) failed",
              !vmaf_feature_score_at_index(vmaf, "ciede2000", score, 0u));
    return NULL;
}

/* The case's score on the CPU (`state` NULL) or on the twin. */
static inline mu_message_t ciede_twin_score(const CiedeTwin *twin, void *state,
                                            const CiedeTwinCase *c, double *score)
{
    VmafContext *vmaf = NULL;
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    mu_assert("vmaf_init failed", !vmaf_init(&vmaf, cfg));
    mu_message_t msg = NULL;
    if (state && twin->import(vmaf, state))
        msg = "importing the device state failed";
    if (!msg && vmaf_use_feature(vmaf, state ? twin->extractor : "ciede", NULL))
        msg = "vmaf_use_feature failed";
    if (!msg)
        msg = ciede_twin_read(vmaf, c, score);
    if (vmaf_close(vmaf) != 0 && !msg)
        msg = "vmaf_close failed";
    return msg;
}

/* The twin's score is within CIEDE_TWIN_TOL of the CPU's. A missing device
 * skips the case. */
static inline mu_message_t ciede_twin_check(const CiedeTwin *twin, const CiedeTwinCase *c)
{
    double cpu = 0.0;
    double gpu = NAN;
    mu_message_t msg = ciede_twin_score(twin, NULL, c, &cpu);
    if (msg)
        return msg;
    void *state = NULL;
    if (twin->open(&state) != 0 || state == NULL) {
        (void)fprintf(stderr, "[skip: no %s device] ", twin->backend);
        mu_skipped = 1;
        return NULL;
    }
    msg = ciede_twin_score(twin, state, c, &gpu);
    const int close_err = twin->close(state);
    if (msg)
        return msg;
    mu_assert("closing the device state failed", close_err == 0);
    mu_assert("CPU ciede2000 is not finite", isfinite(cpu));
    const double delta = fabs(cpu - gpu);
    if (!(delta <= CIEDE_TWIN_TOL)) {
        (void)fprintf(stderr, "\n%s %ux%u %u-bit: cpu=%.17g %s=%.17g delta=%.3e tol=%.1e\n",
                      c->what, c->w, c->h, c->bpc, cpu, twin->backend, gpu, delta, CIEDE_TWIN_TOL);
    }
    mu_assert("the ciede twin is further from the CPU extractor than a few float steps explain",
              delta <= CIEDE_TWIN_TOL);
    return NULL;
}

static inline mu_message_t ciede_twin_registered(const CiedeTwin *twin)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name(twin->extractor);
    mu_assert("the ciede twin must be registered", fex != NULL);
    mu_assert("the ciede twin's name matches", !strcmp(fex->name, twin->extractor));
    return NULL;
}

/* 4:2:0 on the build's fixture at one bit depth. */
static inline mu_message_t ciede_twin_bit_depth(const CiedeTwin *twin, unsigned bpc)
{
    const CiedeTwinCase c = {"ciede 4:2:0", FIXTURE_W, FIXTURE_H, bpc, VMAF_PIX_FMT_YUV420P};
    return ciede_twin_check(twin, &c);
}

/* 9, 11, 13, 14 and 15 bits (ADR-2145): the CPU `ciede` scores them now and the twin equals it. */
static inline mu_message_t ciede_twin_odd_depths(const CiedeTwin *twin)
{
    static const unsigned depths[] = {9u, 11u, 13u, 14u, 15u};
    for (size_t k = 0; k < sizeof(depths) / sizeof(depths[0]); k++) {
        const mu_message_t msg = ciede_twin_bit_depth(twin, depths[k]);
        if (msg) {
            return msg;
        }
    }
    return NULL;
}

/* Odd in both dimensions: the last column and row have chroma of their own. */
static inline mu_message_t ciede_twin_odd_frame(const CiedeTwin *twin)
{
    const CiedeTwinCase c = {"ciede 4:2:0 odd", 323u, 181u, 8u, VMAF_PIX_FMT_YUV420P};
    return ciede_twin_check(twin, &c);
}

/* 577x325: the chroma planes are 289 wide, the ceiling of the half width
 * (the ADR-1213 hazard). */
static inline mu_message_t ciede_twin_odd_ceil_chroma(const CiedeTwin *twin)
{
    const CiedeTwinCase c = {"ciede 4:2:0 577x325", 577u, 325u, 8u, VMAF_PIX_FMT_YUV420P};
    return ciede_twin_check(twin, &c);
}

static inline mu_message_t ciede_twin_422(const CiedeTwin *twin)
{
    const CiedeTwinCase c = {"ciede 4:2:2", FIXTURE_W, FIXTURE_H, 8u, VMAF_PIX_FMT_YUV422P};
    return ciede_twin_check(twin, &c);
}

/* Horizontal-only subsampling with the 16-bit sample path, odd size. */
static inline mu_message_t ciede_twin_422_10bit_odd(const CiedeTwin *twin)
{
    const CiedeTwinCase c = {"ciede 4:2:2 577x325", 577u, 325u, 10u, VMAF_PIX_FMT_YUV422P};
    return ciede_twin_check(twin, &c);
}

static inline mu_message_t ciede_twin_444(const CiedeTwin *twin)
{
    const CiedeTwinCase c = {"ciede 4:4:4", FIXTURE_W, FIXTURE_H, 10u, VMAF_PIX_FMT_YUV444P};
    return ciede_twin_check(twin, &c);
}

static inline mu_message_t ciede_twin_1080p(const CiedeTwin *twin)
{
    const CiedeTwinCase c = {"ciede 1080p", 1920u, 1080u, 8u, VMAF_PIX_FMT_YUV420P};
    return ciede_twin_check(twin, &c);
}

/* NOLINTEND(modernize-use-nullptr) */

#endif /* LIBVMAF_TEST_CIEDE_TWIN_PARITY_H_ */
