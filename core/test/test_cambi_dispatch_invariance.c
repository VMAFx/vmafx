/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  End-to-end dispatch invariance for CAMBI: the same input must give the same
 *  score bits whichever CPU kernels the extractor binds.
 *
 *  CAMBI dispatches every stage per ISA (anti-dithering, derivative row,
 *  spatial-mask dp / mask rows, decimate, mode filter, c-values). The kernel
 *  parity tests (test_cambi_stage_simd, test_cambi_spatial_mask_simd) compare
 *  each kernel with its scalar stage in isolation; this test drives the whole
 *  extractor through the public API, as the CLI's --cpumask does, and asserts
 *  the per-frame scores are bit-identical across dispatch levels:
 *
 *    x86:     host (AVX-512 where present), AVX2 only (AVX-512 flags masked,
 *             which also runs the AVX2 scanned c-values driver), scalar (every
 *             flag masked)
 *    aarch64: host (NEON) and scalar
 *
 *  test_feature_isa_invariance covers one 8-bit default-option fixture for
 *  every extractor; this one covers the CAMBI option paths that route through
 *  different stages: 10-bit input (no anti-dithering), full-reference mode
 *  (two pictures per frame), max_log_contrast 5 (32 contrast steps), the
 *  cambi_high_res_speedup early decimation, a non-default window, 4:4:4,
 *  odd sizes, and ramps across the first and onto the last value of the
 *  scored band. Fixtures are banded gradients with a textured block, so the
 *  spatial mask is mixed and the scores are not zero.
 *
 *  Bit-identical is the assertion, not a tolerance: every CAMBI kernel is
 *  bit-exact against its scalar stage (ADR-1256, ADR-1207).
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test.h"

#include "config.h"
#include "libvmaf/feature.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/picture.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

#define MAX_FRAMES 2u
#define MAX_KEYS 3u

typedef struct {
    const char *name;
    enum VmafPixelFormat pix_fmt;
    unsigned bpc;
    unsigned w;
    unsigned h;
    unsigned frames;
    const char *opt_key[2]; /* up to two cambi options, NULL-terminated */
    const char *opt_val[2];
    /* Collector keys to compare, NULL-terminated: the extractor's name for the
     * default options, an option-suffixed alias otherwise. */
    const char *keys[MAX_KEYS];
    /* Ramp start code and code step per band; 0 means 8-bit code 40 in steps
     * of one 8-bit code, scaled to the bit depth. */
    unsigned start;
    unsigned step;
} CambiCase;

static const CambiCase CASES[] = {
    /* clang-format off */
    /* One case per line. clang-format spreads a braced initializer over one
     * line per field, and the expanded table was a 96-line brace block, which
     * the HISS-04 size check counts as a function. Same treatment as the HIP
     * option tables. Designated initializers: an omitted field is zero. */
    {.name = "8-bit 4:2:0 odd size", .pix_fmt = VMAF_PIX_FMT_YUV420P, .bpc = 8, .w = 333, .h = 217, .frames = 2, .keys = {"Cambi_feature_cambi_score"}},
    {.name = "10-bit 4:2:0", .pix_fmt = VMAF_PIX_FMT_YUV420P, .bpc = 10, .w = 640, .h = 360, .frames = 2, .keys = {"Cambi_feature_cambi_score"}},
    {.name = "10-bit full reference", .pix_fmt = VMAF_PIX_FMT_YUV420P, .bpc = 10, .w = 480, .h = 270, .frames = 2, .opt_key = {"full_ref", NULL}, .opt_val = {"true", NULL}, .keys = {"Cambi_feature_cambi_score", "cambi_source", "cambi_full_reference"}},
    {.name = "8-bit max_log_contrast 5", .pix_fmt = VMAF_PIX_FMT_YUV420P, .bpc = 8, .w = 480, .h = 270, .frames = 2, .opt_key = {"max_log_contrast", NULL}, .opt_val = {"5", NULL}, .keys = {"cambi_mlc_5"}},
    {.name = "8-bit 1080p high-res speedup", .pix_fmt = VMAF_PIX_FMT_YUV420P, .bpc = 8, .w = 1920, .h = 1080, .frames = 1, .opt_key = {"cambi_high_res_speedup", NULL}, .opt_val = {"1080", NULL}, .keys = {"cambi_hrs_1080"}},
    {.name = "10-bit 4:4:4 odd size, window 33", .pix_fmt = VMAF_PIX_FMT_YUV444P, .bpc = 10, .w = 575, .h = 323, .frames = 1, .opt_key = {"window_size", NULL}, .opt_val = {"33", NULL}, .keys = {"cambi_ws_33"}},
    {.name = "10-bit ramp across the first value of the scored band", .pix_fmt = VMAF_PIX_FMT_YUV420P, .bpc = 10, .w = 480, .h = 270, .frames = 2, .opt_key = {"cambi_vis_lum_threshold", NULL}, .opt_val = {"0.06", NULL}, .keys = {"cambi_vlt_0.06"}, .start = 61, .step = 1},
    {.name = "10-bit ramp to the top of the scored band", .pix_fmt = VMAF_PIX_FMT_YUV420P, .bpc = 10, .w = 480, .h = 270, .frames = 2, .keys = {"Cambi_feature_cambi_score"}, .start = 541, .step = 1},
    /* clang-format on */
};
#define NUM_CASES (sizeof(CASES) / sizeof(CASES[0]))

/* Dispatch levels, as VmafConfiguration.cpumask values (bits to disable). */
typedef struct {
    const char *name;
    uint64_t cpumask;
} DispatchLevel;

static const DispatchLevel LEVELS[] = {
    {"host", 0u},
#if ARCH_X86
    /* AVX512 (bit 4) and AVX512ICL (bit 5): AVX2 kernels only. */
    {"avx2-only", 16u | 32u},
#endif
    {"scalar", UINT64_MAX},
};
#define NUM_LEVELS (sizeof(LEVELS) / sizeof(LEVELS[0]))

/* A diagonal ramp with a textured block and a per-frame shift. The distorted
 * picture is the banded one: one step of two 8-bit codes (scaled to the bit
 * depth) every 2 * `period` pixels, with sparse one-LSB noise at 10 bits. The
 * reference is the same ramp at the finest step the bit depth allows, so
 * full-reference mode scores the banding the distortion added. `period` keeps
 * bands wide enough for the 7x7 spatial mask to call them flat and narrow
 * enough for several to fall in one CAMBI window, which grows with the frame
 * size as the period does; the ramp wraps (sawtooth) after 24 codes. */
static unsigned sample(const CambiCase *c, unsigned x, unsigned y, unsigned frame, bool distorted)
{
    const unsigned max = (1u << c->bpc) - 1u;
    const unsigned scale = 1u << (c->bpc - 8u);
    if (x < c->w / 6u && y < c->h / 5u)
        return ((x ^ y) & 4u) ? max / 4u : max / 2u; /* texture */
    const unsigned start = c->step ? c->start : 40u * scale;
    const unsigned step = c->step ? c->step : scale;
    const unsigned period = (c->w + 2u * c->h) / 256u > 4u ? (c->w + 2u * c->h) / 256u : 4u;
    const unsigned phase = (x + 2u * y + 3u * frame) % (24u * period);
    unsigned v = start + (phase * step) / period;
    if (distorted) {
        v = start + ((phase / period) & ~1u) * step;
        v += (c->bpc > 8u && (x * 7u + y * 13u) % 29u == 0u) ? 1u : 0u;
    }
    return v > max ? max : v;
}

static void fill_plane(const CambiCase *c, VmafPicture *pic, unsigned p, unsigned frame,
                       bool distorted)
{
    for (unsigned y = 0; y < pic->h[p]; y++) {
        for (unsigned x = 0; x < pic->w[p]; x++) {
            const unsigned v = p == 0 ? sample(c, x, y, frame, distorted) : (1u << (c->bpc - 1u));
            if (c->bpc > 8u) {
                uint16_t *row = (uint16_t *)((uint8_t *)pic->data[p] + (size_t)y * pic->stride[p]);
                row[x] = (uint16_t)v;
            } else {
                uint8_t *row = (uint8_t *)pic->data[p] + (size_t)y * pic->stride[p];
                row[x] = (uint8_t)v;
            }
        }
    }
}

static int make_pic(const CambiCase *c, VmafPicture *pic, unsigned frame, bool distorted)
{
    const int err = vmaf_picture_alloc(pic, c->pix_fmt, c->bpc, c->w, c->h);
    if (err)
        return err;
    for (unsigned p = 0; p < 3; p++) {
        fill_plane(c, pic, p, frame, distorted);
    }
    return 0;
}

static int use_cambi(VmafContext *vmaf, const CambiCase *c)
{
    VmafFeatureDictionary *opts = NULL;
    for (unsigned i = 0; i < 2 && c->opt_key[i]; i++) {
        const int err = vmaf_feature_dictionary_set(&opts, c->opt_key[i], c->opt_val[i]);
        if (err)
            return err;
    }
    /* vmaf_use_feature takes ownership of opts. */
    return vmaf_use_feature(vmaf, "cambi", opts);
}

static int feed_frames(VmafContext *vmaf, const CambiCase *c)
{
    for (unsigned i = 0; i < c->frames; i++) {
        VmafPicture ref;
        VmafPicture dist;
        int err = make_pic(c, &ref, i, false);
        if (err)
            return err;
        err = make_pic(c, &dist, i, true);
        if (err) {
            (void)vmaf_picture_unref(&ref);
            return err;
        }
        err = vmaf_read_pictures(vmaf, &ref, &dist, i);
        if (err)
            return err;
    }
    return vmaf_read_pictures(vmaf, NULL, NULL, 0);
}

/* scores[frame][key] for one dispatch level. */
typedef struct {
    double v[MAX_FRAMES][MAX_KEYS];
} Scores;

static int run_case(const CambiCase *c, uint64_t cpumask, Scores *out)
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE, .cpumask = cpumask};
    VmafContext *vmaf = NULL;
    int err = vmaf_init(&vmaf, cfg);
    if (err)
        return err;
    err = use_cambi(vmaf, c);
    if (!err)
        err = feed_frames(vmaf, c);
    for (unsigned f = 0; !err && f < c->frames; f++) {
        for (unsigned k = 0; !err && k < MAX_KEYS && c->keys[k]; k++) {
            err = vmaf_feature_score_at_index(vmaf, c->keys[k], &out->v[f][k], f);
        }
    }
    (void)vmaf_close(vmaf);
    return err;
}

static bool bits_equal(double a, double b)
{
    uint64_t ab = 0;
    uint64_t bb = 0;
    memcpy(&ab, &a, sizeof(ab));
    memcpy(&bb, &b, sizeof(bb));
    return ab == bb;
}

/* Returns the number of scores that differ from the host run. */
static unsigned compare_level(const CambiCase *c, const DispatchLevel *lvl, const Scores *host,
                              const Scores *s)
{
    unsigned diffs = 0;
    for (unsigned f = 0; f < c->frames; f++) {
        for (unsigned k = 0; k < MAX_KEYS && c->keys[k]; k++) {
            if (bits_equal(host->v[f][k], s->v[f][k]))
                continue;
            (void)fprintf(stderr, "\n  %s: %s frame %u: host=%.17g %s=%.17g", c->name, c->keys[k],
                          f, host->v[f][k], lvl->name, s->v[f][k]);
            diffs++;
        }
    }
    return diffs;
}

static char *check_case(const CambiCase *c)
{
    Scores host;
    memset(&host, 0, sizeof(host));
    mu_assert("cambi host run failed", run_case(c, LEVELS[0].cpumask, &host) == 0);
    /* A zero score would make the comparison vacuous. */
    mu_assert("cambi fixture scored zero", host.v[0][0] > 0.0 && isfinite(host.v[0][0]));
    unsigned diffs = 0;
    for (unsigned l = 1; l < NUM_LEVELS; l++) {
        Scores s;
        memset(&s, 0, sizeof(s));
        mu_assert("cambi masked run failed", run_case(c, LEVELS[l].cpumask, &s) == 0);
        diffs += compare_level(c, &LEVELS[l], &host, &s);
    }
    mu_assert("cambi scores depend on the dispatched CPU kernels", diffs == 0);
    return NULL;
}

static char *test_cambi_dispatch_invariance(void)
{
    for (unsigned i = 0; i < NUM_CASES; i++) {
        char *err = check_case(&CASES[i]);
        if (err) {
            (void)fprintf(stderr, "[%s] ", CASES[i].name);
            return err;
        }
    }
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_cambi_dispatch_invariance);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
