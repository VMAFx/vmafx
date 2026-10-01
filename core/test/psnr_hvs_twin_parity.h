/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * psnr_hvs CPU vs. GPU twin: the fixtures and the bit-for-bit comparison that
 * test_cuda_psnr_hvs_parity, test_sycl_psnr_hvs_parity and
 * test_hip_psnr_hvs_parity share (ADR-1397, ADR-1401).
 *
 * calc_psnrhvs() (third_party/xiph/psnr_hvs.c) adds every masked coefficient
 * error of a plane into one running float, so its result depends on the order
 * of the additions. A twin stores the same terms and the host adds them in
 * that order; every comparison here is therefore exact, on psnr_hvs_y /
 * psnr_hvs_cb / psnr_hvs_cr and the combined psnr_hvs. The 3840x2160 cases
 * are the ones a twin that sums per block fails by the widest margin: about
 * 10.8 million luma terms, where the CPU's float sum is furthest from the
 * exact one.
 *
 * A test describes its backend in one HvsTwin and wraps the hvs_twin_*()
 * cases. Each comparison opens its own device state: a SYCL state keeps the
 * geometry of its first frame. Without a device, or with a twin that is not
 * built (-ENOSYS, the HIP scaffold contract), a case is skipped and the test
 * exits 77.
 */

#ifndef LIBVMAF_TEST_PSNR_HVS_TWIN_PARITY_H_
#define LIBVMAF_TEST_PSNR_HVS_TWIN_PARITY_H_

#include <errno.h>
#include <math.h>
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

#define HVS_FEATURES 4u

/* One GPU backend's twin, as the shared cases drive it. */
typedef struct HvsTwin {
    const char *extractor; /* registry name, e.g. "psnr_hvs_cuda" */
    const char *backend;   /* for messages, e.g. "CUDA" */
    /* Opens a device state. Non-zero: no device, the case is skipped. */
    int (*open)(void **state);
    int (*import)(VmafContext *vmaf, void *state);
    int (*close)(void *state);
    /* Non-zero when the twin scores 4:0:0 input (luma only) as the CPU does;
     * zero when it refuses the format. */
    int scores_yuv400;
} HvsTwin;

enum HvsPattern {
    HVS_PATTERN_RAMP,  /* shifted ramps: mostly DC error */
    HVS_PATTERN_MIXED, /* xor-scrambled ramps over the full sample range */
    HVS_PATTERN_NOISE, /* smooth reference, pseudo-random error of a few codes */
};

typedef struct HvsFixture {
    enum VmafPixelFormat fmt;
    unsigned bpc;
    unsigned w;
    unsigned h;
    enum HvsPattern pattern;
} HvsFixture;

static const char *const hvs_features[HVS_FEATURES] = {"psnr_hvs_y", "psnr_hvs_cb", "psnr_hvs_cr",
                                                       "psnr_hvs"};

/* Deterministic 32-bit mix of a sample position (no libc rand). */
static inline unsigned hvs_hash(unsigned col, unsigned row, unsigned plane)
{
    unsigned h = (col * 0x9E3779B1u) ^ (row * 0x85EBCA77u) ^ (plane * 0xC2B2AE3Du);
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    return h;
}

static inline unsigned hvs_sample(const HvsFixture *fx, unsigned col, unsigned row, unsigned plane,
                                  unsigned salt)
{
    const unsigned range = 1u << fx->bpc;
    if (fx->pattern == HVS_PATTERN_RAMP) {
        const unsigned luma = row * 5u + col + salt * 11u;
        const unsigned chroma = row + col * (1u + plane) + salt * 7u;
        return (plane == 0u ? luma : chroma) % range;
    }
    if (fx->pattern == HVS_PATTERN_MIXED)
        return ((col * 37u + row * 11u + plane * 5u) ^ (salt * 91u)) % range;
    /* A slow ramp well inside the range, plus up to +-4 codes on the distorted
     * picture: little masking, so most coefficient errors reach the sum. */
    const unsigned base = range / 4u + ((col + 2u * row + 3u * plane) / 8u) % (range / 2u);
    return salt == 0u ? base : base + hvs_hash(col, row, plane) % 9u - 4u;
}

static inline void hvs_fill_row(uint8_t *line, unsigned width, const HvsFixture *fx, unsigned row,
                                unsigned plane, unsigned salt)
{
    for (unsigned col = 0; col < width; col++) {
        const unsigned v = hvs_sample(fx, col, row, plane, salt);
        if (fx->bpc > 8u) {
            ((uint16_t *)line)[col] = (uint16_t)v;
        } else {
            line[col] = (uint8_t)v;
        }
    }
}

static inline int hvs_fill_pic(VmafPicture *pic, const HvsFixture *fx, unsigned salt)
{
    const int err = vmaf_picture_alloc(pic, fx->fmt, fx->bpc, fx->w, fx->h);
    if (err)
        return err;
    const unsigned planes = (fx->fmt == VMAF_PIX_FMT_YUV400P) ? 1u : 3u;
    for (unsigned p = 0; p < planes; p++) {
        for (unsigned row = 0; row < pic->h[p]; row++) {
            uint8_t *line = (uint8_t *)pic->data[p] + (size_t)row * (size_t)pic->stride[p];
            hvs_fill_row(line, pic->w[p], fx, row, p, salt);
        }
    }
    return 0;
}

/* A context scoring psnr_hvs on the CPU (`state` NULL) or on the twin.
 * `*unbuilt` is set when the twin's registration reports -ENOSYS. */
static inline mu_message_t hvs_open_context(const HvsTwin *twin, void *state, VmafContext **vmaf,
                                            int *unbuilt)
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    mu_assert("vmaf_init failed", !vmaf_init(vmaf, cfg));
    if (state)
        mu_assert("importing the device state failed", !twin->import(*vmaf, state));
    const int err = vmaf_use_feature(*vmaf, state ? twin->extractor : "psnr_hvs", NULL);
    if (state && err == -ENOSYS) {
        *unbuilt = 1;
    } else {
        mu_assert("vmaf_use_feature failed", !err);
    }
    return NULL;
}

/* Reads the emitted scores of frame 0. A 4:0:0 fixture has no chroma scores;
 * those slots stay 0 on both sides. */
static inline mu_message_t hvs_read_scores(VmafContext *vmaf, const HvsFixture *fx,
                                           double scores[HVS_FEATURES])
{
    const int luma_only = (fx->fmt == VMAF_PIX_FMT_YUV400P);
    for (unsigned i = 0; i < HVS_FEATURES; i++) {
        scores[i] = 0.0;
        if (luma_only && (i == 1u || i == 2u))
            continue;
        mu_assert("score", !vmaf_feature_score_at_index(vmaf, hvs_features[i], &scores[i], 0u));
    }
    return NULL;
}

/* Feeds the fixture's frame. `*unbuilt` is set when the twin reports -ENOSYS
 * at its first frame. */
static inline mu_message_t hvs_feed(VmafContext *vmaf, const HvsFixture *fx, int on_twin,
                                    int *unbuilt)
{
    VmafPicture ref;
    VmafPicture dist;
    mu_assert("ref alloc", !hvs_fill_pic(&ref, fx, 0u));
    mu_assert("dist alloc", !hvs_fill_pic(&dist, fx, 1u));
    const int err = vmaf_read_pictures(vmaf, &ref, &dist, 0u);
    if (on_twin && err == -ENOSYS) {
        *unbuilt = 1;
    } else {
        mu_assert("read", !err);
    }
    return NULL;
}

static inline mu_message_t hvs_collect(VmafContext *vmaf, const HvsFixture *fx,
                                       double scores[HVS_FEATURES])
{
    mu_assert("flush", !vmaf_read_pictures(vmaf, NULL, NULL, 0));
    return hvs_read_scores(vmaf, fx, scores);
}

/* Scores of one frame on the CPU (`state` NULL) or on the twin. `*unbuilt` is
 * set when the twin reports -ENOSYS: built without its device kernels (the
 * HIP scaffold contract, at registration or at the first frame). */
static inline mu_message_t hvs_score(const HvsTwin *twin, void *state, const HvsFixture *fx,
                                     double scores[HVS_FEATURES], int *unbuilt)
{
    VmafContext *vmaf = NULL;
    mu_message_t msg = hvs_open_context(twin, state, &vmaf, unbuilt);
    if (!msg && !*unbuilt)
        msg = hvs_feed(vmaf, fx, state != NULL, unbuilt);
    if (!msg && !*unbuilt)
        msg = hvs_collect(vmaf, fx, scores);
    if (vmaf != NULL && vmaf_close(vmaf) != 0 && !msg)
        msg = "vmaf_close failed";
    return msg;
}

/* The bit pattern of a score: two scores are the same value, to the last bit
 * and including infinities, exactly when their patterns are equal. */
static inline uint64_t hvs_score_bits(double score)
{
    uint64_t bits = 0u;
    memcpy(&bits, &score, sizeof(bits));
    return bits;
}

/* Bit-for-bit comparison of every score; reports each one that differs. */
static inline mu_message_t hvs_require_identical(const HvsTwin *twin, const HvsFixture *fx,
                                                 const double cpu[HVS_FEATURES],
                                                 const double gpu[HVS_FEATURES])
{
    unsigned differing = 0u;
    for (unsigned i = 0; i < HVS_FEATURES; i++) {
        if (hvs_score_bits(cpu[i]) == hvs_score_bits(gpu[i]))
            continue;
        differing++;
        (void)fprintf(stderr, "\n%ux%u %u-bit %s: cpu=%.17g %s=%.17g delta=%.3e", fx->w, fx->h,
                      fx->bpc, hvs_features[i], cpu[i], twin->backend, gpu[i],
                      fabs(cpu[i] - gpu[i]));
    }
    if (differing != 0u)
        (void)fprintf(stderr, "\n");
    mu_assert("the psnr_hvs twin must return the CPU's psnr_hvs scores bit for bit (ADR-1397)",
              differing == 0u);
    return NULL;
}

/* One fixture on the CPU and on the twin, compared exactly. */
static inline mu_message_t hvs_compare(const HvsTwin *twin, const HvsFixture *fx)
{
    void *state = NULL;
    if (twin->open(&state) != 0 || state == NULL) {
        (void)fprintf(stderr, "[skip: no %s device] ", twin->backend);
        mu_skipped = 1;
        return NULL;
    }
    double cpu[HVS_FEATURES] = {0.0, 0.0, 0.0, 0.0};
    double gpu[HVS_FEATURES] = {NAN, NAN, NAN, NAN};
    int unbuilt = 0;
    mu_message_t msg = hvs_score(twin, NULL, fx, cpu, &unbuilt);
    if (!msg)
        msg = hvs_score(twin, state, fx, gpu, &unbuilt);
    if (!msg && unbuilt) {
        (void)fprintf(stderr, "[skip: %s twin not built (-ENOSYS)] ", twin->backend);
        mu_skipped = 1;
    } else if (!msg) {
        msg = hvs_require_identical(twin, fx, cpu, gpu);
    }
    const int close_err = twin->close(state);
    if (!msg && close_err)
        msg = "closing the device state failed";
    return msg;
}

static inline mu_message_t hvs_twin_registered(const HvsTwin *twin)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name(twin->extractor);
    mu_assert("the psnr_hvs twin must be registered", fex != NULL);
    mu_assert("the psnr_hvs twin's name matches", !strcmp(fex->name, twin->extractor));
    return NULL;
}

/* FIXTURE_W x FIXTURE_H at 8 bits: 256x144, or what a `_large` variant sets. */
static inline mu_message_t hvs_twin_identical(const HvsTwin *twin)
{
    const HvsFixture ramp = {VMAF_PIX_FMT_YUV420P, 8u, FIXTURE_W, FIXTURE_H, HVS_PATTERN_RAMP};
    mu_assert_msg(hvs_compare(twin, &ramp));
    const HvsFixture noise = {VMAF_PIX_FMT_YUV420P, 8u, FIXTURE_W, FIXTURE_H, HVS_PATTERN_NOISE};
    return hvs_compare(twin, &noise);
}

/* 9- to 12-bit input: calc_psnrhvs() uses the raw sample at every depth. The
 * twins used to convert only 10- and 12-bit samples back exactly and scored
 * 9 / 11 bits on 16 times the sample (-1.57 against 22.47 dB at 9 bits). */
static inline mu_message_t hvs_twin_every_depth_identical(const HvsTwin *twin)
{
    for (unsigned bpc = 9u; bpc <= 12u; bpc++) {
        const HvsFixture fx = {VMAF_PIX_FMT_YUV420P, bpc, 64u, 48u, HVS_PATTERN_MIXED};
        mu_assert_msg(hvs_compare(twin, &fx));
    }
    return NULL;
}

/* 4:2:2 and 4:4:4 change the chroma block counts and with them the length of
 * each chroma sum. 4:0:0 input: the CPU extractor scores luma only; a twin
 * that does the same (`scores_yuv400`) is compared on it too. */
static inline mu_message_t hvs_twin_every_layout_identical(const HvsTwin *twin)
{
    static const enum VmafPixelFormat layouts[] = {VMAF_PIX_FMT_YUV422P, VMAF_PIX_FMT_YUV444P,
                                                   VMAF_PIX_FMT_YUV400P};
    const unsigned count = twin->scores_yuv400 ? 3u : 2u;
    for (unsigned i = 0; i < count; i++) {
        const HvsFixture fx = {layouts[i], 8u, 64u, 48u, HVS_PATTERN_MIXED};
        mu_assert_msg(hvs_compare(twin, &fx));
    }
    return NULL;
}

/* 3840x2160: 10.8 million luma terms in one running float. A twin that sums
 * each block first is 1e-2 dB away from the CPU here on real content. */
static inline mu_message_t hvs_twin_2160p_identical(const HvsTwin *twin)
{
    const HvsFixture uhd8 = {VMAF_PIX_FMT_YUV420P, 8u, 3840u, 2160u, HVS_PATTERN_NOISE};
    mu_assert_msg(hvs_compare(twin, &uhd8));
    const HvsFixture uhd10 = {VMAF_PIX_FMT_YUV420P, 10u, 3840u, 2160u, HVS_PATTERN_NOISE};
    return hvs_compare(twin, &uhd10);
}

/* NOLINTEND(modernize-use-nullptr) */

#endif /* LIBVMAF_TEST_PSNR_HVS_TWIN_PARITY_H_ */
