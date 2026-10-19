/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * motion_five_frame_window on a GPU twin of `motion` and `motion_v2`
 * (ADR-1491): fixture, cases and comparison shared by the CUDA, SYCL and HIP
 * tests.
 *
 * With the option the CPU takes the SAD of frame n against frame n-2 and
 * derives motion2 / motion3 over the five-frame window (Netflix a2b59b77,
 * ADR-1478). A twin has to return the same bits: the SAD from the device,
 * the window from the CPU's own function. Each case feeds the same frames to
 * the CPU extractor and to the twin and compares every output of every frame
 * with ==.
 *
 * Frame counts: 11 crosses the CUDA twin's readback batch (eight frames,
 * ADR-0845) and leaves a tail for flush; 1 and 2 have no SAD at all; 3 has
 * one, which is motion2 of the last frame and motion3 of all three. A twin
 * that takes the SAD against frame n-1, that reads a plane it never filled,
 * or that scores the first two frames fails here.
 *
 * Frame by frame (ADR-2090): motion2 and motion3 of frame i are final before
 * the flush, once the frame `lag` reads after its window's last SAD is read
 * (CPU 1; a device twin's SAD lands one read later, CUDA `motion`'s at its
 * readback batch), and they keep the value they had then. A twin that
 * derives them only at the flush fails here.
 *
 * A test file defines an MftBackend (how to open its device state and bind
 * it to a context, and its lags) and calls mft_failed_cases().
 */

#ifndef LIBVMAF_TEST_MOTION_FIVE_FRAME_TWIN_PARITY_H_
#define LIBVMAF_TEST_MOTION_FIVE_FRAME_TWIN_PARITY_H_

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "float_bits.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/picture.h"

/* NOLINTBEGIN(modernize-use-nullptr): C header. The fork builds C as C23,
 * where clang-tidy also proposes the `nullptr` keyword, but MSVC's documented
 * /std:clatest C23 feature set does not include `nullptr` while the required
 * Windows build compiles the tests including this header with cl.exe.
 * ADR-1138. */

#define MFT_W 256u
#define MFT_H 144u
#define MFT_MAX_FRAMES 11u
#define MFT_MAX_KEYS 4u
#define MFT_MAX_OPTS 7u

typedef struct MftOption {
    const char *key;
    const char *val;
} MftOption;

/* One option set of one extractor and the outputs the CPU has under it. */
typedef struct MftCase {
    const char *name;
    bool v2;                        /* motion_v2 and its twin; else motion */
    bool nonzero;                   /* the SAD score must not be 0 on every frame */
    MftOption opts[MFT_MAX_OPTS];   /* NULL-key terminated */
    const char *keys[MFT_MAX_KEYS]; /* NULL-terminated; the SAD score first */
} MftCase;

/* How one backend's test reaches its device. */
typedef struct MftBackend {
    const char *label;     /* "cuda" */
    const char *motion;    /* twin of `motion` */
    const char *motion_v2; /* twin of `motion_v2` */
    /* Create the device state; nonzero when there is no device. */
    int (*open)(void **state);
    /* Bind the state to a context. */
    int (*import)(VmafContext *vmaf, void *state);
    /* Release the state; 0 on success. */
    int (*close)(void *state);
    /* ADR-2090: reads after the one that brings the SAD of frame
     * max(i + 1, 2) by which frame i is final, plus one (1 = that read). */
    unsigned lag_motion;
    unsigned lag_motion_v2;
} MftBackend;

static const MftCase mft_cases[] = {
    {"motion, window",
     false,
     true,
     {{"motion_five_frame_window", "true"}},
     {"VMAF_integer_feature_motion_sad_score_mffw", "integer_motion2_mffw",
      "integer_motion3_mffw"}},
    {"motion, window, moving average, debug",
     false,
     true,
     {{"motion_five_frame_window", "true"}, {"motion_moving_average", "true"}, {"debug", "true"}},
     {"VMAF_integer_feature_motion_sad_score_mffw_mma", "integer_motion_mffw_mma",
      "integer_motion2_mffw_mma", "integer_motion3_mffw_mma"}},
    {"motion, window, every option",
     false,
     true,
     {{"motion_five_frame_window", "true"},
      {"motion_moving_average", "true"},
      {"motion_fps_weight", "0.3"},
      {"motion_max_val", "0.5"},
      {"motion_blend_factor", "0.5"},
      {"motion_blend_offset", "0.2"},
      {"debug", "true"}},
     {"VMAF_integer_feature_motion_sad_score_mbf_0.5_mbo_0.2_mffw_mfw_0.3_mmxv_0.5_mma",
      "integer_motion_mbf_0.5_mbo_0.2_mffw_mfw_0.3_mmxv_0.5_mma",
      "integer_motion2_mbf_0.5_mbo_0.2_mffw_mfw_0.3_mmxv_0.5_mma",
      "integer_motion3_mbf_0.5_mbo_0.2_mffw_mfw_0.3_mmxv_0.5_mma"}},
    {"motion, window, force zero",
     false,
     false,
     {{"motion_five_frame_window", "true"}, {"motion_force_zero", "true"}},
     {"VMAF_integer_feature_motion_sad_score_mffw_force_0", "integer_motion2_mffw_force_0",
      "integer_motion3_mffw_force_0"}},
    {"motion_v2, window",
     true,
     true,
     {{"motion_five_frame_window", "true"}},
     {"VMAF_integer_feature_motion_v2_sad_score_mffw", "VMAF_integer_feature_motion2_v2_score_mffw",
      "VMAF_integer_feature_motion3_v2_score_mffw"}},
    {"motion_v2, window, every option",
     true,
     true,
     {{"motion_five_frame_window", "true"},
      {"motion_moving_average", "true"},
      {"motion_fps_weight", "0.3"},
      {"motion_max_val", "0.5"},
      {"motion_blend_factor", "0.5"},
      {"motion_blend_offset", "0.2"}},
     {"VMAF_integer_feature_motion_v2_sad_score_mbf_0.5_mbo_0.2_mffw_mfw_0.3_mmxv_0.5_mma",
      "VMAF_integer_feature_motion2_v2_score_mbf_0.5_mbo_0.2_mffw_mfw_0.3_mmxv_0.5_mma",
      "VMAF_integer_feature_motion3_v2_score_mbf_0.5_mbo_0.2_mffw_mfw_0.3_mmxv_0.5_mma"}},
};
#define MFT_N_CASES (sizeof(mft_cases) / sizeof(mft_cases[0]))

static const unsigned mft_frame_counts[] = {MFT_MAX_FRAMES, 1u, 2u, 3u};
#define MFT_N_FRAME_COUNTS (sizeof(mft_frame_counts) / sizeof(mft_frame_counts[0]))

static inline size_t mft_key_count(const MftCase *c)
{
    size_t n = 0u;
    while (n < MFT_MAX_KEYS && c->keys[n] != NULL) {
        n++;
    }
    return n;
}

static inline void mft_put_sample(VmafPicture *pic, unsigned plane, unsigned row, unsigned col,
                                  unsigned v)
{
    uint8_t *line = (uint8_t *)pic->data[plane] + ((size_t)row * (size_t)pic->stride[plane]);
    if (pic->bpc <= 8u) {
        line[col] = (uint8_t)v;
    } else {
        ((uint16_t *)line)[col] = (uint16_t)v;
    }
}

/* Texture that moves down three rows and right one column a frame, with a
 * step that grows with the frame, so the SADs two frames apart differ from
 * the SADs one frame apart and from each other. */
static inline unsigned mft_luma(unsigned row, unsigned col, unsigned frame, unsigned bpc)
{
    const unsigned r = row + (frame * 3u);
    const unsigned c = col + frame;
    const unsigned v = (((r * 3u) + (c * 2u)) & 0xFFu) ^ (((r >> 2) * (c >> 3)) & 0x1Fu);
    return ((v + (frame * frame)) & 0xFFu) << (bpc - 8u);
}

static inline int mft_fill_picture(VmafPicture *pic, unsigned bpc, unsigned frame)
{
    const int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, bpc, MFT_W, MFT_H);
    if (err) {
        return err;
    }
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            mft_put_sample(pic, 0u, row, col, mft_luma(row, col, frame, bpc));
        }
    }
    for (unsigned p = 1; p < 3u; p++) {
        for (unsigned row = 0; row < pic->h[p]; row++) {
            for (unsigned col = 0; col < pic->w[p]; col++) {
                mft_put_sample(pic, p, row, col, 1u << (bpc - 1u));
            }
        }
    }
    return 0;
}

/* Frame `frame` through `vmaf`, which takes both pictures. Motion reads the
 * reference only; the distorted picture is the same frame. */
static inline int mft_feed_frame(VmafContext *vmaf, unsigned bpc, unsigned frame)
{
    VmafPicture ref;
    VmafPicture dist;
    int err = mft_fill_picture(&ref, bpc, frame);
    if (err) {
        return err;
    }
    err = mft_fill_picture(&dist, bpc, frame);
    if (err) {
        (void)vmaf_picture_unref(&ref);
        return err;
    }
    return vmaf_read_pictures(vmaf, &ref, &dist, frame);
}

/* The read after which frame i is final (ADR-2090): the five-frame window
 * needs the SAD of frame max(i + 1, 2), which lands `lag - 1` reads later. */
static inline unsigned mft_final_after(unsigned i, unsigned lag)
{
    return ((i + 1u > 2u) ? i + 1u : 2u) + lag - 1u;
}

static inline unsigned mft_lag(const MftCase *c, const MftBackend *backend, const void *state)
{
    if (state == NULL) {
        return 1u;
    }
    return c->v2 ? backend->lag_motion_v2 : backend->lag_motion;
}

/* After read `frame`: every frame due by then has motion2 and motion3 (the
 * last two keys), recorded in `early` (frame-major, MFT_MAX_KEYS a frame). */
static inline int mft_check_early(VmafContext *vmaf, const MftCase *c, unsigned frame, unsigned lag,
                                  double *early)
{
    const size_t count = mft_key_count(c);
    for (unsigned i = 0; i <= frame; i++) {
        for (size_t k = count - 2u; k < count && mft_final_after(i, lag) <= frame; k++) {
            if (vmaf_feature_score_at_index(vmaf, c->keys[k],
                                            &early[((size_t)i * MFT_MAX_KEYS) + k], i)) {
                (void)fprintf(stderr, "\n%s: %s of frame %u not final after frame %u\n", c->name,
                              c->keys[k], i, frame);
                return -EAGAIN;
            }
        }
    }
    return 0;
}

/* The values read early are the values after the flush. */
static inline int mft_check_unchanged(const MftCase *c, unsigned frames, unsigned lag,
                                      const double *early, const double *out)
{
    const size_t count = mft_key_count(c);
    for (unsigned i = 0; i < frames && mft_final_after(i, lag) < frames; i++) {
        for (size_t k = count - 2u; k < count; k++) {
            if (!vmaf_test_identical_f64(early[((size_t)i * MFT_MAX_KEYS) + k],
                                         out[((size_t)i * count) + k])) {
                (void)fprintf(stderr, "\n%s: %s of frame %u changed after it was final\n", c->name,
                              c->keys[k], i);
                return -EINVAL;
            }
        }
    }
    return 0;
}

/* A context with the case's CPU extractor, or with its twin on `state`. */
static inline int mft_case_context(VmafContext **vmaf, const MftCase *c, const MftBackend *backend,
                                   void *state)
{
    const VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafFeatureDictionary *opts = NULL;
    int err = vmaf_init(vmaf, cfg);
    if (!err && state) {
        err = backend->import(*vmaf, state);
    }
    for (unsigned i = 0; i < MFT_MAX_OPTS && !err && c->opts[i].key != NULL; i++) {
        err = vmaf_feature_dictionary_set(&opts, c->opts[i].key, c->opts[i].val);
    }
    if (err) {
        (void)vmaf_feature_dictionary_free(&opts);
        return err;
    }
    const char *cpu_name = c->v2 ? "motion_v2" : "motion";
    const char *twin_name = c->v2 ? backend->motion_v2 : backend->motion;
    /* vmaf_use_feature() takes the dictionary over, on failure too. */
    return vmaf_use_feature(*vmaf, state ? twin_name : cpu_name, opts);
}

/* `frames` frames through one extractor, and every key of the case of every
 * frame read into `out` (frame-major). Returns the first error. */
static inline int mft_case_scores(const MftCase *c, const MftBackend *backend, void *state,
                                  unsigned bpc, unsigned frames, double *out)
{
    const size_t count = mft_key_count(c);
    const unsigned lag = mft_lag(c, backend, state);
    double early[MFT_MAX_KEYS * MFT_MAX_FRAMES] = {0.0};
    VmafContext *vmaf = NULL;
    int err = mft_case_context(&vmaf, c, backend, state);
    for (unsigned frame = 0; frame < frames && !err; frame++) {
        err = mft_feed_frame(vmaf, bpc, frame);
        if (!err) {
            err = mft_check_early(vmaf, c, frame, lag, early);
        }
    }
    if (!err) {
        err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    }
    for (size_t i = 0; i < count * frames && !err; i++) {
        err = vmaf_feature_score_at_index(vmaf, c->keys[i % count], &out[i], (unsigned)(i / count));
        if (err) {
            (void)fprintf(stderr, "\n%s (%s, %u frames): no score for %s at frame %u\n",
                          state ? backend->label : "cpu", c->name, frames, c->keys[i % count],
                          (unsigned)(i / count));
        }
    }
    if (!err) {
        err = mft_check_unchanged(c, frames, lag, early, out);
    }
    const int closed = vmaf ? vmaf_close(vmaf) : 0;
    return err ? err : closed;
}

/* The outputs whose twin value is not the CPU's, each one reported, plus one
 * when the SAD score is 0 on every frame of a case that must move. */
static inline unsigned mft_count_mismatches(const MftCase *c, const MftBackend *backend,
                                            unsigned bpc, unsigned frames, const double *cpu,
                                            const double *gpu)
{
    const size_t count = mft_key_count(c);
    unsigned mismatches = 0u;
    bool any_nonzero = false;
    for (size_t i = 0; i < count * frames; i++) {
        any_nonzero = any_nonzero || ((i % count) == 0u && cpu[i] != 0.0);
        if (isfinite(cpu[i]) && vmaf_test_identical_f64(cpu[i], gpu[i])) {
            continue;
        }
        mismatches++;
        (void)fprintf(stderr,
                      "\n%s, %u-bit, %u frames, frame %u %s: cpu=%.17g %s=%.17g delta=%.3e\n",
                      c->name, bpc, frames, (unsigned)(i / count), c->keys[i % count], cpu[i],
                      backend->label, gpu[i], fabs(cpu[i] - gpu[i]));
    }
    /* A sequence of one or two frames has no SAD by definition. */
    if (c->nonzero && frames > 2u && !any_nonzero) {
        (void)fprintf(stderr, "\n%s, %u-bit, %u frames: %s is 0 on every frame\n", c->name, bpc,
                      frames, c->keys[0]);
        mismatches++;
    }
    return mismatches;
}

/* Mismatches of one case at one bit depth and frame count; UINT32_MAX when a
 * run failed; 0 with *skipped set when there is no device. */
static inline unsigned mft_case_mismatches(const MftCase *c, const MftBackend *backend,
                                           unsigned bpc, unsigned frames, bool *skipped)
{
    double cpu[MFT_MAX_KEYS * MFT_MAX_FRAMES] = {0.0};
    double gpu[MFT_MAX_KEYS * MFT_MAX_FRAMES] = {0.0};
    void *state = NULL;
    if (backend->open(&state) != 0 || state == NULL) {
        (void)fprintf(stderr, "[skip: no %s device] ", backend->label);
        *skipped = true;
        return 0u;
    }
    const int gpu_err = mft_case_scores(c, backend, state, bpc, frames, gpu);
    const int free_err = backend->close(state);
    /* A build whose twins are scaffolds has nothing to compare. */
    if (gpu_err == -ENOSYS) {
        (void)fprintf(stderr, "[skip: %s extractor is a scaffold (-ENOSYS)] ", backend->label);
        *skipped = true;
        return 0u;
    }
    const int cpu_err = gpu_err ? 0 : mft_case_scores(c, backend, NULL, bpc, frames, cpu);
    if (gpu_err || cpu_err || free_err) {
        (void)fprintf(stderr, "\n%s, %u-bit, %u frames: run failed (%s %d, cpu %d, free %d)\n",
                      c->name, bpc, frames, backend->label, gpu_err, cpu_err, free_err);
        return UINT32_MAX;
    }
    return mft_count_mismatches(c, backend, bpc, frames, cpu, gpu);
}

/* Every case at every frame count at one bit depth; all of them run, so one
 * failure does not hide the next. Returns the number of failed cases. */
static inline unsigned mft_failed_cases(const MftBackend *backend, unsigned bpc, bool *skipped)
{
    unsigned failed = 0u;
    for (size_t i = 0; i < MFT_N_CASES && !*skipped; i++) {
        for (size_t f = 0; f < MFT_N_FRAME_COUNTS && !*skipped; f++) {
            const unsigned bad =
                mft_case_mismatches(&mft_cases[i], backend, bpc, mft_frame_counts[f], skipped);
            failed += (bad != 0u) ? 1u : 0u;
        }
    }
    return failed;
}

/* NOLINTEND(modernize-use-nullptr) */

#endif /* LIBVMAF_TEST_MOTION_FIVE_FRAME_TWIN_PARITY_H_ */
