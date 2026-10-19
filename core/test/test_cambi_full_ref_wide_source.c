/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  CAMBI full-reference mode with a source larger than the picture
 *  (docs/state.md T-CAMBI-10BIT-FULLREF-WIDE-SOURCE-ROWS-2026-10-05).
 *
 *  Under full_ref, cambi.c allocates its working picture MAX(src, enc) wide,
 *  so with src_width / src_height above the picture size the working row
 *  stride is larger than the input's. The 10-bit same-size conversion copied
 *  the distorted plane with one memcpy of input stride x height samples, so
 *  every row after the first landed at the wrong offset and the distorted
 *  score depended on the source options (10-bit Sparks frame 0: 0.0048 with
 *  src_width=960:src_height=540, 0.3734 without).
 *
 *  `cambi` is the distorted picture's score at the encode size
 *  (cambi.c::extract, docs/metrics/cambi.md "Full-reference mode"); the source
 *  options only size the reference's run. These tests hold that:
 *
 *    - vmaf_cambi_preprocessing(), the conversion cambi.c and the Metal twin
 *      run, keeps every 10-bit sample at its row and column whichever of the
 *      input and working strides is the larger;
 *    - through the public API, at 8, 10 and 12 bits, `cambi` with full_ref,
 *      with and without a wider source, equals `cambi` without full_ref bit
 *      for bit, and `cambi_full_reference` is MAX(0, cambi - cambi_source).
 */

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test.h"
#include "float_bits.h"

#include "feature/cambi_internal.h"
#include "libvmaf/feature.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/picture.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

/* ------------------------------------------------------------------------ */
/* The 10-bit same-size conversion.                                          */
/* ------------------------------------------------------------------------ */

/* 64 samples are one 64-sample row alignment (picture.c), so a picture
 * allocated twice as wide has twice the stride. */
#define CONV_W 64u
#define CONV_H 48u

static uint16_t conv_sample(unsigned row, unsigned col)
{
    return (uint16_t)(((row * 37u) + (col * 11u)) & 1023u);
}

static void fill_conv_input(VmafPicture *pic)
{
    const ptrdiff_t stride = pic->stride[0] / 2;
    uint16_t *data = pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            data[((ptrdiff_t)row * stride) + col] = conv_sample(row, col);
        }
    }
}

/* Samples of the CONV_W x CONV_H output that are not the input's. */
static unsigned conv_mismatches(const VmafPicture *out)
{
    const ptrdiff_t stride = out->stride[0] / 2;
    const uint16_t *data = out->data[0];
    unsigned bad = 0u;
    for (unsigned row = 0; row < CONV_H; row++) {
        for (unsigned col = 0; col < CONV_W; col++) {
            bad += (data[((ptrdiff_t)row * stride) + col] != conv_sample(row, col)) ? 1u : 0u;
        }
    }
    return bad;
}

/* Converts a 10-bit CONV_W x CONV_H plane whose rows are `in_alloc_w` samples
 * apart into a working picture allocated `out_alloc_w` x `out_alloc_h`, as
 * cambi.c does at the encode size, and returns the misplaced samples (or
 * UINT32_MAX when a call failed). The output is at least as large as the
 * input's memory, so the pre-fix single copy stays inside it. */
static unsigned convert_misplaced(unsigned in_alloc_w, unsigned out_alloc_w, unsigned out_alloc_h)
{
    VmafPicture in;
    VmafPicture out;
    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    if (vmaf_picture_alloc(&in, VMAF_PIX_FMT_YUV400P, 10u, in_alloc_w, CONV_H) != 0)
        return UINT32_MAX;
    in.w[0] = CONV_W; /* a plane narrower than its rows, as a caller may pass */
    fill_conv_input(&in);
    unsigned bad = UINT32_MAX;
    if (vmaf_picture_alloc(&out, VMAF_PIX_FMT_YUV400P, 10u, out_alloc_w, out_alloc_h) == 0) {
        const int err = vmaf_cambi_preprocessing(&in, &out, (int)CONV_W, (int)CONV_H, 10);
        bad = err ? UINT32_MAX : conv_mismatches(&out);
        if (vmaf_picture_unref(&out) != 0)
            bad = UINT32_MAX;
    }
    if (vmaf_picture_unref(&in) != 0)
        bad = UINT32_MAX;
    return bad;
}

/* The full_ref case: the working picture is twice as wide as the input. */
static char *test_same_size_10b_wider_working_stride(void)
{
    const unsigned bad = convert_misplaced(CONV_W, 2u * CONV_W, 2u * CONV_H);
    if (bad != 0u)
        (void)fprintf(stderr, "\n  wider working stride: %u misplaced samples", bad);
    mu_assert("10-bit same-size conversion misplaces rows when the working stride is larger",
              bad == 0u);
    return NULL;
}

/* A caller's picture whose rows are twice as far apart as the working
 * picture's; the working picture has twice the rows, so its memory equals the
 * input's. */
static char *test_same_size_10b_wider_input_stride(void)
{
    const unsigned bad = convert_misplaced(2u * CONV_W, CONV_W, 2u * CONV_H);
    if (bad != 0u)
        (void)fprintf(stderr, "\n  wider input stride: %u misplaced samples", bad);
    mu_assert("10-bit same-size conversion misplaces rows when the input stride is larger",
              bad == 0u);
    return NULL;
}

/* ------------------------------------------------------------------------ */
/* The extractor through the public API.                                     */
/* ------------------------------------------------------------------------ */

/* 320x240 clears CAMBI's 216-sample minimum; the source, 640x480, is twice
 * the picture in both directions and inside the src_width / src_height
 * ranges. */
#define PIC_W 320u
#define PIC_H 240u
#define NUM_FRAMES 2u

/* One run of the extractor and the keys its scores are stored under. */
typedef struct {
    const char *name;
    const char *opts[7]; /* key, value, ...; NULL-terminated */
    const char *dist_feature;
    bool full_ref;
} CambiRun;

static const CambiRun RUN_NR = {
    .name = "no-reference",
    .opts = {NULL},
    .dist_feature = "Cambi_feature_cambi_score",
};
static const CambiRun RUN_FR = {
    .name = "full_ref",
    .opts = {"full_ref", "true", NULL},
    .dist_feature = "Cambi_feature_cambi_score",
    .full_ref = true,
};
static const CambiRun RUN_FR_WIDE = {
    .name = "full_ref, source 640x480",
    .opts = {"full_ref", "true", "src_width", "640", "src_height", "480", NULL},
    .dist_feature = "cambi_srch_480_srcw_640",
    .full_ref = true,
};

typedef struct {
    double dist[NUM_FRAMES];
    double source[NUM_FRAMES];
    double full_reference[NUM_FRAMES];
} CambiScores;

/* Left half: a shallow ramp, one 10-bit code every two columns, shifted per
 * frame, which is banding for CAMBI at every depth. Right half: texture. The
 * distorted ramp is one 10-bit code brighter, so full_ref scores differ. */
static unsigned luma(unsigned row, unsigned col, unsigned frame, bool distorted, unsigned bpc)
{
    const unsigned v10 = col < PIC_W / 2u ? 200u + ((col + frame) / 2u) + (distorted ? 1u : 0u) :
                                            256u + ((((row * 3u) + (col * 5u)) & 0x3Fu) << 2);
    return bpc >= 10u ? v10 << (bpc - 10u) : v10 >> (10u - bpc);
}

static void put_sample(VmafPicture *pic, unsigned plane, unsigned row, unsigned col, unsigned v)
{
    uint8_t *line = (uint8_t *)pic->data[plane] + ((size_t)row * (size_t)pic->stride[plane]);
    if (pic->bpc <= 8u) {
        line[col] = (uint8_t)v;
    } else {
        ((uint16_t *)line)[col] = (uint16_t)v;
    }
}

static int fill_picture(VmafPicture *pic, unsigned bpc, unsigned frame, bool distorted)
{
    const int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, bpc, PIC_W, PIC_H);
    if (err)
        return err;
    for (unsigned p = 0; p < 3u; p++) {
        for (unsigned row = 0; row < pic->h[p]; row++) {
            for (unsigned col = 0; col < pic->w[p]; col++) {
                const unsigned v = p ? 1u << (bpc - 1u) : luma(row, col, frame, distorted, bpc);
                put_sample(pic, p, row, col, v);
            }
        }
    }
    return 0;
}

static int feed_frames(VmafContext *vmaf, unsigned bpc)
{
    for (unsigned frame = 0; frame < NUM_FRAMES; frame++) {
        VmafPicture ref;
        VmafPicture dist;
        int err = fill_picture(&ref, bpc, frame, false);
        if (err)
            return err;
        err = fill_picture(&dist, bpc, frame, true);
        if (err) {
            (void)vmaf_picture_unref(&ref);
            return err;
        }
        err = vmaf_read_pictures(vmaf, &ref, &dist, frame);
        if (err)
            return err;
    }
    return vmaf_read_pictures(vmaf, NULL, NULL, 0);
}

static int use_cambi(VmafContext *vmaf, const CambiRun *run)
{
    VmafFeatureDictionary *opts = NULL;
    for (unsigned i = 0; run->opts[i] != NULL; i += 2u) {
        const int err = vmaf_feature_dictionary_set(&opts, run->opts[i], run->opts[i + 1u]);
        if (err)
            return err;
    }
    /* vmaf_use_feature() takes the dictionary over, on failure too. */
    return vmaf_use_feature(vmaf, "cambi", opts);
}

static int read_scores(VmafContext *vmaf, const CambiRun *run, CambiScores *out)
{
    int err = 0;
    for (unsigned f = 0; f < NUM_FRAMES && !err; f++) {
        err = vmaf_feature_score_at_index(vmaf, run->dist_feature, &out->dist[f], f);
        if (!err && run->full_ref)
            err = vmaf_feature_score_at_index(vmaf, "cambi_source", &out->source[f], f);
        if (!err && run->full_ref) {
            err = vmaf_feature_score_at_index(vmaf, "cambi_full_reference", &out->full_reference[f],
                                              f);
        }
    }
    return err;
}

static int run_cambi(const CambiRun *run, unsigned bpc, CambiScores *out)
{
    const VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    int err = vmaf_init(&vmaf, cfg);
    if (err)
        return err;
    err = use_cambi(vmaf, run);
    if (!err)
        err = feed_frames(vmaf, bpc);
    if (!err)
        err = read_scores(vmaf, run, out);
    const int closed = vmaf_close(vmaf);
    return err ? err : closed;
}

/* Scores of `run` that are not the no-reference distorted score, or whose
 * combined score is not MAX(0, dist - source); each one reported. */
static unsigned run_mismatches(const CambiRun *run, unsigned bpc, const CambiScores *nr,
                               const CambiScores *s)
{
    unsigned bad = 0u;
    for (unsigned f = 0; f < NUM_FRAMES; f++) {
        const double combined = fmax(0.0, s->dist[f] - s->source[f]);
        const bool dist_ok = vmaf_test_identical_f64(s->dist[f], nr->dist[f]);
        const bool combined_ok = vmaf_test_identical_f64(s->full_reference[f], combined);
        if (!dist_ok || !combined_ok) {
            (void)fprintf(stderr,
                          "\n  %u-bit %s frame %u: cambi=%.17g (no-reference %.17g) "
                          "source=%.17g full_reference=%.17g",
                          bpc, run->name, f, s->dist[f], nr->dist[f], s->source[f],
                          s->full_reference[f]);
        }
        bad += (dist_ok ? 0u : 1u) + (combined_ok ? 0u : 1u);
    }
    return bad;
}

static char *check_depth(unsigned bpc)
{
    CambiScores nr;
    CambiScores fr;
    CambiScores wide;
    memset(&nr, 0, sizeof(nr));
    memset(&fr, 0, sizeof(fr));
    memset(&wide, 0, sizeof(wide));
    mu_assert("cambi no-reference run failed", run_cambi(&RUN_NR, bpc, &nr) == 0);
    mu_assert("cambi full_ref run failed", run_cambi(&RUN_FR, bpc, &fr) == 0);
    mu_assert("cambi full_ref wide-source run failed", run_cambi(&RUN_FR_WIDE, bpc, &wide) == 0);
    /* A zero score would make the comparison vacuous. */
    mu_assert("cambi fixture scored zero", nr.dist[0] > 0.0 && isfinite(nr.dist[0]));
    const unsigned bad =
        run_mismatches(&RUN_FR, bpc, &nr, &fr) + run_mismatches(&RUN_FR_WIDE, bpc, &nr, &wide);
    mu_assert("cambi's distorted score depends on full_ref or the source size", bad == 0u);
    return NULL;
}

static char *test_full_ref_wide_source_8bit(void)
{
    return check_depth(8u);
}

static char *test_full_ref_wide_source_10bit(void)
{
    return check_depth(10u);
}

static char *test_full_ref_wide_source_12bit(void)
{
    return check_depth(12u);
}

char *run_tests(void)
{
    mu_run_test(test_same_size_10b_wider_working_stride);
    mu_run_test(test_same_size_10b_wider_input_stride);
    mu_run_test(test_full_ref_wide_source_8bit);
    mu_run_test(test_full_ref_wide_source_10bit);
    mu_run_test(test_full_ref_wide_source_12bit);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
