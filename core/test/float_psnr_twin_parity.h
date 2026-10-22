/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * float_psnr CPU vs. a GPU twin: fixtures, the comparison and the cases, for
 * a backend's parity test to instantiate (ADR-1450 for SYCL; the design of
 * ADR-1440).
 *
 * float_psnr.c forms each squared difference in float and adds the terms in
 * double, row by row. Every term is a float and a multiple of 1 / scaler^2
 * (scaler = 2^(bpc - 8)), so that sum is exact while it is below 2^53 of
 * those units, and a twin returns the same score exactly when its own sum is
 * exact. A twin that adds a work-group's terms in fp32 is exact only while
 * the group's sum fits 24 bits in that unit: always at 8 bits (256 squares
 * below 2^16), and at 10, 12 and 16 bits only while the differences are
 * small.
 *
 * Natural content does not reach that, and the repository's high-bit-depth
 * fixtures are an 8-bit clip shifted left. The fixtures here are noise,
 * independent for the reference and the distorted frame, over a range of the
 * bit depth, so every group's sum of squares is far above 2^24 units. Two
 * frames per case, compared with ==.
 *
 * The cases past 2^53 are the range in which the CPU's own sum is no longer
 * exact: a 16-bit frame whose sum of squares passes 2^53 units (a mean
 * squared error above 2^37 / (w * h) on the 8-bit scale, a PSNR below 6 dB
 * at 3840x2160). Every row's sum stays exact there, and the CPU's adds of the
 * rows round; the twins add the rows' exact sums in the CPU's order
 * (feature/float_psnr_rows.h, ADR-1499). The cases are a 3840x2160 frame of
 * independent noise in the upper and the lower half of the range, and frames
 * whose exact sum of squares is 2^53 - 1, 2^53, and 2^53 followed by three
 * rows whose sum is 1 each, adds that the CPU drops (a tie at 2^53 goes to
 * the even value). Each compares with ==, and checks on the host that the
 * frame reaches its range and, where the CPU rounds, that the CPU's sum is
 * not the exact one, so a twin that returns the exact sum fails.
 *
 * A test includes this header once, after defining FIXTURE_W / FIXTURE_H if
 * it wants another size for the noise cases, and provides a FloatPsnrTwin.
 * A missing device skips the case and the run exits 77.
 */

#ifndef LIBVMAF_TEST_FLOAT_PSNR_TWIN_PARITY_H_
#define LIBVMAF_TEST_FLOAT_PSNR_TWIN_PARITY_H_

#include <errno.h>
#include <float.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test.h"
#include "float_bits.h"

#include "feature/feature_extractor.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/picture.h"

/* NOLINTBEGIN(modernize-use-nullptr): this is a C23 translation unit, but the
 * required MSVC C lane does not provide the C nullptr spelling clang-tidy
 * proposes. Keep the portable C API form under ADR-1138. */

#ifndef FIXTURE_W
#define FIXTURE_W 256u
#endif
#ifndef FIXTURE_H
#define FIXTURE_H 144u
#endif

enum { FLOAT_PSNR_TWIN_FRAMES = 2 };

/* The backend under test. */
typedef struct FloatPsnrTwin {
    const char *extractor; /* "float_psnr_sycl", ... */
    const char *backend;   /* for messages */
    /* Opens a device state. Non-zero: no device, the case is skipped. */
    int (*open)(void **state);
    int (*import)(VmafContext *vmaf, void *state);
    int (*close)(void *state);
} FloatPsnrTwin;

/* Luma of the reference uniform in [ref_lo, ref_hi] and of the distorted
 * frame in [dis_lo, dis_hi], independent noise; `same` makes the distorted
 * frame the reference. */
typedef struct FloatPsnrTwinCase {
    const char *name;
    unsigned w;
    unsigned h;
    unsigned bpc;
    unsigned ref_lo;
    unsigned ref_hi;
    unsigned dis_lo;
    unsigned dis_hi;
    bool same;
    const char *option; /* a boolean option set to true, or NULL */
    /* With a target: the distorted luma is 0 and the reference's float
     * squares add up to exactly `target` units of 1 / scaler^2 in raster
     * order, then each of the next `ones` rows holds one sample of 1
     * (float_psnr_twin_fill_sum()). */
    uint64_t target;
    unsigned ones;
    bool rounds; /* the CPU's sum of the rows rounds on the frame */
} FloatPsnrTwinCase;

/* lowbias32 hash of the position, the frame and the picture: stateless, so
 * both runs see the same pictures. */
static inline uint32_t float_psnr_twin_hash(unsigned row, unsigned col, unsigned salt)
{
    uint32_t x = ((uint32_t)row << 16) ^ (uint32_t)col ^ (salt * 0x9E3779B9u);
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

static inline void float_psnr_twin_put(VmafPicture *pic, unsigned plane, unsigned row, unsigned col,
                                       unsigned v)
{
    uint8_t *line = (uint8_t *)pic->data[plane] + ((size_t)row * (size_t)pic->stride[plane]);
    if (pic->bpc <= 8u) {
        line[col] = (uint8_t)v;
    } else {
        ((uint16_t *)line)[col] = (uint16_t)v;
    }
}

/* The CPU's term for a 16-bit sample difference, in units of 1 / scaler^2:
 * the float square float_psnr.c forms (one fp32 product), an integer below
 * 2^32. */
static inline uint32_t float_psnr_twin_float_square(int diff)
{
    const float d = (float)diff;
    const float square = d * d;
    return (uint32_t)square;
}

/* The largest 16-bit sample whose float square is at most `rest`. */
static inline unsigned float_psnr_twin_largest_fitting(uint64_t rest)
{
    unsigned lo = 0u;
    unsigned hi = 65535u;
    for (unsigned i = 0; i < 17u && lo < hi; i++) {
        const unsigned mid = (lo + hi + 1u) / 2u;
        if ((uint64_t)float_psnr_twin_float_square((int)mid) <= rest) {
            lo = mid;
        } else {
            hi = mid - 1u;
        }
    }
    return lo;
}

/* Reference luma of a case with a target: in raster order the largest
 * samples whose float squares still fit what is left of `target` until it is
 * reached exactly, then one sample of 1 at the start of each of the next
 * `ones` rows, zeros elsewhere. The distorted luma is 0, so a sample is its
 * difference. */
static inline void float_psnr_twin_fill_sum(VmafPicture *pic, const FloatPsnrTwinCase *c)
{
    uint64_t rest = c->target;
    unsigned ones = c->ones;
    for (unsigned row = 0; row < pic->h[0]; row++) {
        const bool spent = rest == 0u;
        for (unsigned col = 0; col < pic->w[0]; col++) {
            unsigned v = 0u;
            if (rest > 0u) {
                v = float_psnr_twin_largest_fitting(rest);
                rest -= float_psnr_twin_float_square((int)v);
            } else if (spent && col == 0u && ones > 0u) {
                v = 1u;
                ones--;
            }
            float_psnr_twin_put(pic, 0u, row, col, v);
        }
    }
}

/* Noise in [lo, hi] on luma (or the target fill on the reference, zeros on
 * the distorted picture), mid-grey on chroma (float_psnr is luma only);
 * `salt` separates the frames and the two pictures of a frame. */
static inline int float_psnr_twin_fill(VmafPicture *pic, const FloatPsnrTwinCase *c, unsigned lo,
                                       unsigned hi, unsigned salt)
{
    const int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, c->bpc, c->w, c->h);
    if (err) {
        return err;
    }
    if (c->target != 0u && salt % 16u == 1u) {
        float_psnr_twin_fill_sum(pic, c);
    }
    const unsigned span = hi - lo + 1u;
    for (unsigned row = 0; row < pic->h[0] && (c->target == 0u || salt % 16u != 1u); row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            float_psnr_twin_put(pic, 0u, row, col,
                                lo + (float_psnr_twin_hash(row, col, salt) % span));
        }
    }
    for (unsigned p = 1; p < 3u; p++) {
        for (unsigned row = 0; row < pic->h[p]; row++) {
            for (unsigned col = 0; col < pic->w[p]; col++) {
                float_psnr_twin_put(pic, p, row, col, 1u << (c->bpc - 1u));
            }
        }
    }
    return 0;
}

/* Frame `frame` of the case through `vmaf`, which takes both pictures. */
static inline int float_psnr_twin_feed(VmafContext *vmaf, const FloatPsnrTwinCase *c,
                                       unsigned frame)
{
    VmafPicture ref;
    VmafPicture dist;
    const unsigned ref_salt = (frame * 16u) + 1u;
    int err = float_psnr_twin_fill(&ref, c, c->ref_lo, c->ref_hi, ref_salt);
    if (err) {
        return err;
    }
    err = c->same ? float_psnr_twin_fill(&dist, c, c->ref_lo, c->ref_hi, ref_salt) :
                    float_psnr_twin_fill(&dist, c, c->dis_lo, c->dis_hi, (frame * 16u) + 8u);
    if (err) {
        (void)vmaf_picture_unref(&ref);
        return err;
    }
    return vmaf_read_pictures(vmaf, &ref, &dist, frame);
}

/* The case's frames through the CPU `float_psnr` (`state` NULL) or the twin,
 * and the score of every frame read into `out`. Returns the first error. */
static inline int float_psnr_twin_scores(const FloatPsnrTwin *twin, void *state,
                                         const FloatPsnrTwinCase *c, double *out)
{
    const VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    VmafFeatureDictionary *options = NULL;
    int err = vmaf_init(&vmaf, cfg);
    if (!err && state) {
        err = twin->import(vmaf, state);
    }
    if (!err && c->option) {
        err = vmaf_feature_dictionary_set(&options, c->option, "true");
    }
    if (!err) {
        err = vmaf_use_feature(vmaf, state ? twin->extractor : "float_psnr", options);
    }
    for (unsigned frame = 0; frame < FLOAT_PSNR_TWIN_FRAMES && !err; frame++) {
        err = float_psnr_twin_feed(vmaf, c, frame);
    }
    if (!err) {
        err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    }
    for (unsigned frame = 0; frame < FLOAT_PSNR_TWIN_FRAMES && !err; frame++) {
        err = vmaf_feature_score_at_index(vmaf, "float_psnr", &out[frame], frame);
    }
    const int closed = vmaf ? vmaf_close(vmaf) : 0;
    return err ? err : closed;
}

/* The CPU's and the twin's scores for one case. Returns 0, a negative errno
 * when a run failed, or 1 when the twin's leg was skipped (no device). */
static inline int float_psnr_twin_run(const FloatPsnrTwin *twin, const FloatPsnrTwinCase *c,
                                      double *cpu, double *gpu)
{
    void *state = NULL;
    if (twin->open(&state) != 0 || state == NULL) {
        (void)fprintf(stderr, "[skip: no %s device] ", twin->backend);
        mu_skipped = 1;
        return 1;
    }
    const int gpu_err = float_psnr_twin_scores(twin, state, c, gpu);
    const int close_err = twin->close(state);
    if (gpu_err == -ENOSYS) {
        /* A build without the device kernels (HIP with enable_hipcc=false). */
        (void)fprintf(stderr, "[skip: %s kernels not built] ", twin->backend);
        mu_skipped = 1;
        return 1;
    }
    const int cpu_err = gpu_err ? 0 : float_psnr_twin_scores(twin, NULL, c, cpu);
    if (gpu_err || cpu_err || close_err) {
        (void)fprintf(stderr, "\n%s: run failed (%s %d, cpu %d, close %d)\n", c->name,
                      twin->backend, gpu_err, cpu_err, close_err);
        return gpu_err ? gpu_err : (cpu_err ? cpu_err : -EIO);
    }
    return 0;
}

/* Frames of the case whose twin score is not the CPU's, each one reported;
 * UINT32_MAX when a run failed. A skipped leg counts as 0. `first` returns
 * the CPU's score of frame 0. */
static inline unsigned float_psnr_twin_mismatches(const FloatPsnrTwin *twin,
                                                  const FloatPsnrTwinCase *c, double *first)
{
    double cpu[FLOAT_PSNR_TWIN_FRAMES] = {0.0};
    double gpu[FLOAT_PSNR_TWIN_FRAMES] = {0.0};
    const int run = float_psnr_twin_run(twin, c, cpu, gpu);
    if (run != 0) {
        return run > 0 ? 0u : UINT32_MAX;
    }
    unsigned mismatches = 0u;
    for (unsigned frame = 0; frame < FLOAT_PSNR_TWIN_FRAMES; frame++) {
        if (isfinite(cpu[frame]) && vmaf_test_identical_f64(cpu[frame], gpu[frame])) {
            continue;
        }
        mismatches++;
        (void)fprintf(stderr, "\n%s %ux%u %u-bit frame %u: cpu=%.17g %s=%.17g delta=%.3e\n",
                      c->name, c->w, c->h, c->bpc, frame, cpu[frame], twin->backend, gpu[frame],
                      fabs(cpu[frame] - gpu[frame]));
    }
    if (first) {
        *first = cpu[0];
    }
    return mismatches;
}

static inline mu_message_t float_psnr_twin_registered(const FloatPsnrTwin *twin)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name(twin->extractor);
    mu_assert("the float_psnr twin must be registered", fex != NULL);
    mu_assert("the float_psnr twin's name matches", !strcmp(fex->name, twin->extractor));
    return NULL;
}

/* Full-range noise at `bpc` bits, with `option` set when it is not NULL. */
static inline mu_message_t float_psnr_twin_noise_exact(const FloatPsnrTwin *twin, unsigned bpc,
                                                       const char *option)
{
    const unsigned peak = (1u << bpc) - 1u;
    const FloatPsnrTwinCase c = {"noise", FIXTURE_W, FIXTURE_H, bpc, 0u, peak, 0u,
                                 peak,    false,     option,    0u,  0u, false};
    mu_assert("the float_psnr twin is not bit-identical to the CPU on noise",
              float_psnr_twin_mismatches(twin, &c, NULL) == 0u);
    return NULL;
}

/* A bright 16-bit 1920x1080 pair: large samples, moderate differences. */
static inline mu_message_t float_psnr_twin_bright_1080p_exact(const FloatPsnrTwin *twin)
{
    const FloatPsnrTwinCase c = {"16-bit bright", 1920u, 1080u, 16u, 56000u, 64000u, 56000u,
                                 64000u,          false, NULL,  0u,  0u,     false};
    mu_assert("the float_psnr twin is not bit-identical to the CPU on a bright 16-bit frame",
              float_psnr_twin_mismatches(twin, &c, NULL) == 0u);
    return NULL;
}

/* Identical frames: zero noise, the score is the psnr_max sentinel. */
static inline mu_message_t float_psnr_twin_identical_exact(const FloatPsnrTwin *twin, unsigned bpc,
                                                           double psnr_max)
{
    const unsigned peak = (1u << bpc) - 1u;
    const FloatPsnrTwinCase c = {"identical", FIXTURE_W, FIXTURE_H, bpc, 0u, peak, 0u,
                                 peak,        true,      NULL,      0u,  0u, false};
    double first = 0.0;
    mu_assert("the float_psnr twin differs from the CPU on identical frames",
              float_psnr_twin_mismatches(twin, &c, &first) == 0u);
    mu_assert("identical frames must score psnr_max",
              mu_skipped || vmaf_test_expect_identical_f64("frame 0", first, psnr_max));
    return NULL;
}

/* Every depth the engine reads that has no case of its own above: 9, 11, 13, 14 and 15 bits
 * (ADR-2145), noise and identical frames (the ceiling 6 * bpc + 12), twin equal to the CPU. */
static inline mu_message_t float_psnr_twin_odd_depths_exact(const FloatPsnrTwin *twin)
{
    static const unsigned depths[] = {9u, 11u, 13u, 14u, 15u};
    for (size_t k = 0; k < sizeof(depths) / sizeof(depths[0]); k++) {
        mu_message_t msg = float_psnr_twin_noise_exact(twin, depths[k], NULL);
        if (msg) {
            return msg;
        }
        msg = float_psnr_twin_identical_exact(twin, depths[k], 6.0 * (double)depths[k] + 12.0);
        if (msg) {
            return msg;
        }
    }
    return NULL;
}

/* The 16-bit cases past 2^53 units and at the boundary (see the top of the
 * file). */
static const FloatPsnrTwinCase FLOAT_PSNR_TWIN_PAST_CASES[] = {
    {"16-bit 3840x2160 halves past 2^53", 3840u, 2160u, 16u, 32768u, 65535u, 0u, 32767u, false,
     NULL, 0u, 0u, true},
    {"16-bit sum 2^53 - 1", 2048u, 1040u, 16u, 0u, 0u, 0u, 0u, false, NULL,
     ((uint64_t)1 << 53) - 1u, 0u, false},
    {"16-bit sum 2^53", 2048u, 1040u, 16u, 0u, 0u, 0u, 0u, false, NULL, (uint64_t)1 << 53, 0u,
     false},
    {"16-bit sum 2^53 and three rows of 1", 2048u, 1040u, 16u, 0u, 0u, 0u, 0u, false, NULL,
     (uint64_t)1 << 53, 3u, true},
};
#define FLOAT_PSNR_TWIN_PAST_CASE_COUNT                                                            \
    (sizeof(FLOAT_PSNR_TWIN_PAST_CASES) / sizeof(FLOAT_PSNR_TWIN_PAST_CASES[0]))

/* Exact sum, and float_psnr.c's sum (the rows' exact sums added into a
 * double, row after row), of frame 0 of the case, in units of 1 / scaler^2.
 * Returns non-zero when a picture cannot be made. */
static inline int float_psnr_twin_sums(const FloatPsnrTwinCase *c, uint64_t *exact, double *cpu)
{
    VmafPicture ref;
    VmafPicture dis;
    const bool target = c->target != 0u;
    if (float_psnr_twin_fill(&ref, c, c->ref_lo, c->ref_hi, 1u)) {
        return -1;
    }
    if (float_psnr_twin_fill(&dis, c, target ? 0u : c->dis_lo, target ? 0u : c->dis_hi, 8u)) {
        (void)vmaf_picture_unref(&ref);
        return -1;
    }
    *exact = 0u;
    *cpu = 0.0;
    for (unsigned row = 0; row < ref.h[0]; row++) {
        const uint16_t *r =
            (const uint16_t *)((const uint8_t *)ref.data[0] + (size_t)row * ref.stride[0]);
        const uint16_t *d =
            (const uint16_t *)((const uint8_t *)dis.data[0] + (size_t)row * dis.stride[0]);
        uint64_t row_sum = 0u;
        for (unsigned col = 0; col < ref.w[0]; col++) {
            row_sum += float_psnr_twin_float_square((int)r[col] - (int)d[col]);
        }
        *exact += row_sum;
        *cpu += (double)row_sum;
    }
    (void)vmaf_picture_unref(&ref);
    (void)vmaf_picture_unref(&dis);
    return 0;
}

/* The case reaches its range: its target exactly (or a sum past 2^53 units),
 * and, where the case says the CPU rounds, a CPU sum that is not the exact
 * one (a twin returning the exact sum would fail). */
static inline bool float_psnr_twin_reaches(const FloatPsnrTwinCase *c)
{
    uint64_t exact = 0u;
    double cpu = 0.0;
    if (float_psnr_twin_sums(c, &exact, &cpu)) {
        return false;
    }
    const bool range = c->target != 0u ? exact == c->target + c->ones : exact > ((uint64_t)1 << 53);
    const bool rounds = !vmaf_test_identical_f64(cpu, (double)exact);
    if (range && rounds == c->rounds) {
        return true;
    }
    (void)fprintf(stderr, "\n%s: exact sum %llu (2^%.4f), CPU sum %.17g\n", c->name,
                  (unsigned long long)exact, log2((double)exact), cpu);
    return false;
}

/* 16-bit frames past 2^53 units and at the boundary: every frame equal to the
 * CPU's, and every case in its range. */
static inline mu_message_t float_psnr_twin_past_2_53_exact(const FloatPsnrTwin *twin)
{
    unsigned mismatches = 0u;
    for (size_t k = 0; k < FLOAT_PSNR_TWIN_PAST_CASE_COUNT; k++) {
        const FloatPsnrTwinCase *c = &FLOAT_PSNR_TWIN_PAST_CASES[k];
        mu_assert("a case past 2^53 does not reach its range", float_psnr_twin_reaches(c));
        const unsigned m = float_psnr_twin_mismatches(twin, c, NULL);
        mu_assert("the runs past 2^53 failed", m != UINT32_MAX);
        mismatches += m;
    }
    mu_assert("the float_psnr twin is not bit-identical to the CPU past 2^53", mismatches == 0u);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */

#endif /* LIBVMAF_TEST_FLOAT_PSNR_TWIN_PARITY_H_ */
