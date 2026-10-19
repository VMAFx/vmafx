/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 */

/*
 * The scores of a second ADM viewing distance are the scores of an extractor
 * at that distance alone, bit for bit (Netflix/vmaf cffd5b77d, ADR-2795).
 *
 * One `adm` context evaluates the DWT and decouple once per scale and the
 * CSF, denominator and contrast-masking stages per distance. Three runs per
 * configuration score the same frames:
 *
 *   shared   one context, adm_norm_view_dist=3, adm_norm_view_dist_extra=5;
 *   merged   two vmaf_use_feature() calls, nvd 3 and nvd 5, which the
 *            registry folds into one context;
 *   alone    nvd 3 and nvd 5 each in a VmafContext of its own.
 *
 * Every one of the seven scores of each distance must have the same bits in
 * all three, on textured 8-bit and 10-bit frames, on the scalar path and on
 * the host's SIMD path.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test.h"

#include "libvmaf/libvmaf.h"
#include "libvmaf/picture.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe. ADR-1138. */

#define FRAMES 3u
#define SCORES 7u

/* The collector keys of the seven scores of each distance: nvd 3 is the
 * default and keeps the provided-feature names, nvd 5 takes the alias with
 * its suffix. */
static const char *const SCORE_NAMES[2][SCORES] = {
    {"VMAF_integer_feature_adm2_score", "VMAF_integer_feature_aim_score",
     "VMAF_integer_feature_adm3_score", "integer_adm_scale0", "integer_adm_scale1",
     "integer_adm_scale2", "integer_adm_scale3"},
    {"integer_adm2_nvd_5", "integer_aim_nvd_5", "integer_adm3_nvd_5", "integer_adm_scale0_nvd_5",
     "integer_adm_scale1_nvd_5", "integer_adm_scale2_nvd_5", "integer_adm_scale3_nvd_5"},
};

typedef struct {
    unsigned w;
    unsigned h;
    unsigned bpc;
    uint64_t cpumask; /* all bits set: scalar; 0: every SIMD level the host has */
} Config;

/* Every score of both distances over FRAMES frames. */
typedef struct {
    double v[2][FRAMES][SCORES];
} Scores;

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
    uint8_t *row = (uint8_t *)pic->data[p] + (y * pic->stride[p]);
    if (pic->bpc == 8u) {
        row[x] = (uint8_t)v;
    } else {
        ((uint16_t *)row)[x] = (uint16_t)v;
    }
}

/* A textured plane; the distorted frame adds frame-dependent noise. */
static void fill_plane(VmafPicture *pic, unsigned p, unsigned frame, int distorted, uint32_t *seed)
{
    const unsigned levels = 1u << pic->bpc;
    for (unsigned y = 0; y < pic->h[p]; y++) {
        for (unsigned x = 0; x < pic->w[p]; x++) {
            unsigned v = (x * 37u + y * 11u + frame * 5u) % levels;
            if (distorted) {
                v = (v + (next_random(seed) % 17u)) % levels;
            }
            put_sample(pic, p, x, y, v);
        }
    }
}

static int make_picture(VmafPicture *pic, const Config *c, unsigned frame, int distorted)
{
    const int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, c->bpc, c->w, c->h);
    if (err) {
        return err;
    }
    uint32_t seed = 0x9e3779b9u ^ (frame * 7919u) ^ (distorted ? 0x51u : 0u);
    for (unsigned p = 0; p < 3u; p++) {
        fill_plane(pic, p, frame, distorted, &seed);
    }
    return 0;
}

static int feed(VmafContext *vmaf, const Config *c)
{
    for (unsigned f = 0; f < FRAMES; f++) {
        VmafPicture ref;
        VmafPicture dis;
        int err = make_picture(&ref, c, f, 0);
        if (err) {
            return err;
        }
        err = make_picture(&dis, c, f, 1);
        if (err) {
            (void)vmaf_picture_unref(&ref);
            return err;
        }
        err = vmaf_read_pictures(vmaf, &ref, &dis, f);
        if (err) {
            return err;
        }
    }
    return vmaf_read_pictures(vmaf, NULL, NULL, 0);
}

/* `adm` at `nvd`, with `nvde` as its second distance when not NULL. */
static int use_adm(VmafContext *vmaf, const char *nvd, const char *nvde)
{
    VmafFeatureDictionary *opts = NULL;
    int err = vmaf_feature_dictionary_set(&opts, "adm_norm_view_dist", nvd);
    if (!err && nvde) {
        err = vmaf_feature_dictionary_set(&opts, "adm_norm_view_dist_extra", nvde);
    }
    if (!err) {
        err = vmaf_use_feature(vmaf, "adm", opts);
    }
    if (err && opts) {
        (void)vmaf_feature_dictionary_free(&opts);
    }
    return err;
}

/* Read the scores of distance `view` (0: nvd 3, 1: nvd 5). */
static int read_view(VmafContext *vmaf, unsigned view, Scores *out)
{
    for (unsigned f = 0; f < FRAMES; f++) {
        for (unsigned i = 0; i < SCORES; i++) {
            const int err =
                vmaf_feature_score_at_index(vmaf, SCORE_NAMES[view][i], &out->v[view][f][i], f);
            if (err) {
                return err;
            }
        }
    }
    return 0;
}

static int open_context(VmafContext **vmaf, const Config *c)
{
    const VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE, .cpumask = c->cpumask};
    return vmaf_init(vmaf, cfg);
}

/* shared (`merged` == 0) or merged (`merged` == 1): both distances, one run. */
static int run_both(const Config *c, int merged, Scores *out)
{
    VmafContext *vmaf = NULL;
    int err = open_context(&vmaf, c);
    if (err) {
        return err;
    }
    err = merged ? use_adm(vmaf, "3", NULL) : use_adm(vmaf, "3", "5");
    if (!err && merged) {
        err = use_adm(vmaf, "5", NULL);
    }
    if (!err) {
        err = feed(vmaf, c);
    }
    if (!err) {
        err = read_view(vmaf, 0u, out);
    }
    if (!err) {
        err = read_view(vmaf, 1u, out);
    }
    const int close_err = vmaf_close(vmaf);
    return err ? err : close_err;
}

/* alone: one distance per VmafContext. */
static int run_alone(const Config *c, Scores *out)
{
    static const char *const NVD[2] = {"3", "5"};
    for (unsigned view = 0; view < 2u; view++) {
        VmafContext *vmaf = NULL;
        int err = open_context(&vmaf, c);
        if (err) {
            return err;
        }
        err = use_adm(vmaf, NVD[view], NULL);
        if (!err) {
            err = feed(vmaf, c);
        }
        if (!err) {
            err = read_view(vmaf, view, out);
        }
        const int close_err = vmaf_close(vmaf);
        if (err || close_err) {
            return err ? err : close_err;
        }
    }
    return 0;
}

static int same_bits(double a, double b)
{
    uint64_t x = 0;
    uint64_t y = 0;
    (void)memcpy(&x, &a, sizeof(x));
    (void)memcpy(&y, &b, sizeof(y));
    return x == y;
}

/* The first score whose bits differ, described in `buf`, or NULL. */
static const char *first_difference(const Scores *a, const Scores *b, char *buf, size_t n)
{
    for (unsigned k = 0; k < 2u * FRAMES * SCORES; k++) {
        const unsigned v = k / (FRAMES * SCORES);
        const unsigned f = (k / SCORES) % FRAMES;
        const unsigned i = k % SCORES;
        if (!same_bits(a->v[v][f][i], b->v[v][f][i])) {
            (void)snprintf(buf, n, "frame %u %s: %.17g vs %.17g", f, SCORE_NAMES[v][i],
                           a->v[v][f][i], b->v[v][f][i]);
            return buf;
        }
    }
    return NULL;
}

static char *check_config(const Config *c)
{
    Scores shared;
    Scores merged;
    Scores alone;
    char diff[160];
    mu_assert("shared run failed", !run_both(c, 0, &shared));
    mu_assert("merged run failed", !run_both(c, 1, &merged));
    mu_assert("separate runs failed", !run_alone(c, &alone));
    const char *d = first_difference(&shared, &alone, diff, sizeof(diff));
    if (d) {
        (void)fprintf(stderr, "\n  shared vs alone: %s\n", d);
    }
    mu_assert("a second distance scores as an extractor at it alone", d == NULL);
    d = first_difference(&merged, &alone, diff, sizeof(diff));
    if (d) {
        (void)fprintf(stderr, "\n  merged vs alone: %s\n", d);
    }
    mu_assert("two registrations score as two extractors", d == NULL);
    return NULL;
}

/* positive: 8-bit, odd frame size, scalar and SIMD. */
static char *test_8bit_frames(void)
{
    const Config scalar = {177u, 145u, 8u, ~(uint64_t)0};
    const Config simd = {177u, 145u, 8u, 0u};
    char *msg = check_config(&scalar);
    return msg ? msg : check_config(&simd);
}

/* positive: 10-bit frames, scalar and SIMD. */
static char *test_10bit_frames(void)
{
    const Config scalar = {128u, 96u, 10u, ~(uint64_t)0};
    const Config simd = {128u, 96u, 10u, 0u};
    char *msg = check_config(&scalar);
    return msg ? msg : check_config(&simd);
}

/* Run one context with `nvde` as its second distance; returns the frame error. */
static int run_with_extra(const char *nvde)
{
    const Config c = {64u, 64u, 8u, 0u};
    VmafContext *vmaf = NULL;
    int err = open_context(&vmaf, &c);
    if (err) {
        return err;
    }
    err = use_adm(vmaf, "3", nvde);
    if (!err) {
        err = feed(vmaf, &c);
    }
    (void)vmaf_close(vmaf);
    return err;
}

/* negative and boundary: a second distance below the 1080p-at-3H floor fails
 * the frame, as an extractor at it alone does; one equal to the first, or
 * one whose `%g` name is the first's, is refused (its scores would take the
 * first's names); the next distance with a name of its own scores. */
static char *test_invalid_second_distance(void)
{
    mu_assert("a second distance below the floor fails", run_with_extra("2.5") != 0);
    mu_assert("a second distance equal to the first fails", run_with_extra("3") != 0);
    mu_assert("a second distance named like the first fails", run_with_extra("3.0000001") != 0);
    mu_assert("a second distance with a name of its own scores", run_with_extra("3.00001") == 0);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_8bit_frames);
    mu_run_test(test_10bit_frames);
    mu_run_test(test_invalid_second_distance);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
