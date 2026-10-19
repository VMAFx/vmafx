/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * integer_adm_metal's kernels and host logic on the host, against the CPU
 * extractor `adm`, without a Metal device (ADR-1806;
 * T-METAL-INTEGER-ADM-TWIN-DEFECTS-2026-10-05).
 *
 * The unmodified core/src/feature/metal/integer_adm.metal runs through the
 * Metal Shading Language host shim (metal_integer_adm_host_replay_kernels.cpp);
 * everything else is the twin's own code: integer_adm_metal_host.c gives the
 * geometry, the buffer sizes and element types, the dispatches of each scale,
 * the uniforms and the conclusion of the reductions into scores, exactly as
 * integer_adm_metal.mm uses them. The test allocates the buffers at those
 * sizes (each with a guard band that must stay untouched), runs the plan with
 * one thread per threadgroup and compares every score with the CPU's with
 * `==`, on the geometries and options of test_metal_integer_adm_parity.
 *
 * What it found (the M4 Pro report of issue #2118, every exact case failing):
 * the reductions wrote their slots twice as far apart as the host read them,
 * scale 1 read the int16 scale-0 band as int32, the scales-1-3 masking terms
 * added +2^31 where the CPU adds INT32_MIN, the scales-1-3 denominator rounded
 * its squares with 2^(shift-1) where the CPU adds 2^shift, the AIM numerator of
 * scale 0 entered the sum under adm_skip_scale0, and the noise floor multiplied
 * the area by the noise weight in float.
 *
 * What it cannot see (ADR-1806): anything that depends on more than one lane
 * (a barrier, a race, the lane count), and anything the Metal compiler does
 * that a C++ compiler does not. The device run is test_metal_integer_adm_parity.
 *
 * The Netflix crop case reads src01_hrc0[01]_576x324.yuv from
 * VMAF_REPLAY_YUV_DIR (python/test/resource/yuv, set by core/test/meson.build)
 * and reports a skip for that case only when the files are not there.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "float_bits.h"
#include "metal_integer_adm_host_replay.h"
#include "test.h"

#include "feature/adm_options.h"
#include "libvmaf/feature.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/picture.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy proposes `nullptr`, but the required MSVC C build does
 * not provide that keyword. Preserve the portable C spelling. ADR-1138. */

#define MAX_KEYS 32u
#define MAX_OPTS 6u
#define KEY_LEN 96u
#define GUARD_MIN 256u
#define GUARD_FILL 0xA5u
/* Sources, DWT rows, CSF planes; per scale two bands and the reduction slots
 * of each of the two viewing distances (ADR-2795). */
#define N_BUFFERS (6u + 4u * IADM_METAL_NUM_SCALES)

typedef unsigned (*SampleFn)(unsigned row, unsigned col, bool distorted, unsigned bpc,
                             const void *ctx);

typedef struct ReplayCase {
    const char *name;
    SampleFn sample;
    const void *ctx;    /* the sample function's data */
    const char *suffix; /* feature-name suffix of the options, NULL for none */
    size_t n_opts;
    const char *opts[MAX_OPTS][2];
    unsigned w;
    unsigned h;
    unsigned bpc;
    bool scalar; /* the CPU leg runs without SIMD */
    bool debug;
    const char *nvde;    /* a second viewing distance (ADR-2795), NULL for none */
    const char *suffix2; /* feature-name suffix of the second distance */
} ReplayCase;

/* --- pictures ----------------------------------------------------------- */

static uint32_t position_hash(unsigned row, unsigned col, unsigned salt)
{
    uint32_t x = ((uint32_t)row << 16) ^ (uint32_t)col ^ (salt * 0x9E3779B9u);
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

static unsigned textured_sample(unsigned row, unsigned col, bool d, unsigned bpc, const void *ctx)
{
    (void)ctx;
    unsigned v = ((row * 3u + col * 2u) & 0xFFu) ^ (((row >> 2) * (col >> 3)) & 0x1Fu);
    if (d) {
        v += 9u + ((row * 5u + col * 7u) % 11u);
    }
    return v << (bpc - 8u);
}

static unsigned sparse_sample(unsigned row, unsigned col, bool d, unsigned bpc, const void *ctx)
{
    (void)ctx;
    unsigned v = 128u + (position_hash(row, col, 1u) % 200u == 0u ? 1u : 0u);
    if (d) {
        v += position_hash(row, col, 2u) & 1u;
    }
    return v << (bpc - 8u);
}

static unsigned ramp_sample(unsigned row, unsigned col, bool d, unsigned bpc, const void *ctx)
{
    (void)bpc;
    (void)ctx;
    unsigned v = (row * 5u + col * 3u) & 0xFFu;
    if (d) {
        v = (v + ((row * 7u + col) % 17u)) & 0xFFu;
    }
    return v;
}

static unsigned noise_sample(unsigned row, unsigned col, bool d, unsigned bpc, const void *ctx)
{
    (void)bpc;
    (void)ctx;
    return position_hash(row, col, d ? 1u : 0u) >> 24;
}

static unsigned bright16_sample(unsigned row, unsigned col, bool d, unsigned bpc, const void *ctx)
{
    (void)bpc;
    (void)ctx;
    return 49152u + (position_hash(row, col, d ? 1u : 0u) >> 18);
}

static unsigned patch_sample(unsigned row, unsigned col, bool d, unsigned bpc, const void *ctx)
{
    (void)bpc;
    (void)ctx;
    const unsigned patch_row = (row + 13u) % 16u;
    const unsigned patch_col = (col + 13u) % 16u;
    if (!d || patch_row > 1u || patch_col > 3u) {
        return 128u;
    }
    return (patch_col == 0u) ? 255u : 0u;
}

static unsigned enhanced_sample(unsigned row, unsigned col, bool d, unsigned bpc, const void *ctx)
{
    (void)bpc;
    (void)ctx;
    const int noise = (int)(position_hash(row, col, 0u) >> 26) - 32;
    return (unsigned)(128 + (d ? 2 * noise : noise));
}

/* A 256x144 crop at (100, 60) of the Netflix pair's first frame: ctx is the
 * two 576x324 luma planes, reference first. */
static unsigned crop_sample(unsigned row, unsigned col, bool d, unsigned bpc, const void *ctx)
{
    (void)bpc;
    const uint8_t *luma = (const uint8_t *)ctx;
    return luma[(d ? 576u * 324u : 0u) + ((row + 60u) * 576u) + col + 100u];
}

static void put_sample(VmafPicture *pic, unsigned plane, unsigned row, unsigned col, unsigned v)
{
    const unsigned peak = (1u << pic->bpc) - 1u;
    uint8_t *line = (uint8_t *)pic->data[plane] + ((size_t)row * (size_t)pic->stride[plane]);
    if (pic->bpc <= 8u) {
        line[col] = (uint8_t)(v > peak ? peak : v);
    } else {
        ((uint16_t *)line)[col] = (uint16_t)(v > peak ? peak : v);
    }
}

static int fill_picture(VmafPicture *pic, const ReplayCase *c, bool distorted)
{
    const int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, c->bpc, c->w, c->h);
    if (err) {
        return err;
    }
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            put_sample(pic, 0u, row, col, c->sample(row, col, distorted, c->bpc, c->ctx));
        }
    }
    for (unsigned p = 1; p < 3; p++) {
        for (unsigned row = 0; row < pic->h[p]; row++) {
            for (unsigned col = 0; col < pic->w[p]; col++) {
                put_sample(pic, p, row, col, 1u << (c->bpc - 1u));
            }
        }
    }
    return 0;
}

/* --- keys --------------------------------------------------------------- */

/* The emitted names, in the order replay_values() lists the twin's values:
 * adm2, aim, adm3, the four scale ratios, then (debug) the ratio, its
 * numerator and denominator and the per-scale numerators and denominators. */
static size_t case_keys(const ReplayCase *c, char keys[MAX_KEYS][KEY_LEN])
{
    static const char *const plain[7] = {"VMAF_integer_feature_adm2_score",
                                         "VMAF_integer_feature_aim_score",
                                         "VMAF_integer_feature_adm3_score",
                                         "integer_adm_scale0",
                                         "integer_adm_scale1",
                                         "integer_adm_scale2",
                                         "integer_adm_scale3"};
    static const char *const stem[7] = {
        "integer_adm2",       "integer_aim",        "integer_adm3",      "integer_adm_scale0",
        "integer_adm_scale1", "integer_adm_scale2", "integer_adm_scale3"};
    const char *sfx = c->suffix ? c->suffix : "";
    size_t n = 0u;
    for (; n < 7u; n++) {
        (void)snprintf(keys[n], KEY_LEN, "%s%s", c->suffix ? stem[n] : plain[n], sfx);
    }
    if (c->debug) {
        (void)snprintf(keys[n++], KEY_LEN, "integer_adm%s", sfx);
        (void)snprintf(keys[n++], KEY_LEN, "integer_adm_num%s", sfx);
        (void)snprintf(keys[n++], KEY_LEN, "integer_adm_den%s", sfx);
        for (unsigned s = 0u; s < IADM_METAL_NUM_SCALES; s++) {
            (void)snprintf(keys[n++], KEY_LEN, "integer_adm_num_scale%u%s", s, sfx);
            (void)snprintf(keys[n++], KEY_LEN, "integer_adm_den_scale%u%s", s, sfx);
        }
    }
    /* The second distance's seven scores, after the first distance's. */
    for (unsigned i = 0u; c->nvde && i < 7u; i++) {
        (void)snprintf(keys[n++], KEY_LEN, "%s%s", stem[i], c->suffix2);
    }
    return n;
}

/* Where the second distance's values start: after the first's keys. */
static size_t second_view_at(const ReplayCase *c)
{
    return c->debug ? 18u : 7u;
}

static void replay_values(const IadmMetalScores *r, double out[MAX_KEYS])
{
    out[0] = r->score;
    out[1] = r->score_aim;
    out[2] = r->score_adm3;
    for (unsigned s = 0u; s < IADM_METAL_NUM_SCALES; s++) {
        out[3u + s] = r->scale_scores[s];
    }
    out[7] = r->score;
    out[8] = r->score_num;
    out[9] = r->score_den;
    for (unsigned i = 0u; i < 2u * IADM_METAL_NUM_SCALES; i++) {
        out[10u + i] = r->scores[i];
    }
}

/* --- the CPU extractor ---------------------------------------------------- */

static int cpu_context(VmafContext **vmaf, const ReplayCase *c)
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    if (c->scalar) {
        cfg.cpumask = ~(uint64_t)0;
    }
    int err = vmaf_init(vmaf, cfg);
    VmafFeatureDictionary *opts = NULL;
    for (size_t i = 0; i < c->n_opts && !err; i++) {
        err = vmaf_feature_dictionary_set(&opts, c->opts[i][0], c->opts[i][1]);
    }
    if (!err && c->debug) {
        err = vmaf_feature_dictionary_set(&opts, "debug", "true");
    }
    if (!err && c->nvde) {
        err = vmaf_feature_dictionary_set(&opts, "adm_norm_view_dist_extra", c->nvde);
    }
    if (!err) {
        /* vmaf_use_feature() takes the dictionary over, on failure too. */
        err = vmaf_use_feature(*vmaf, "adm", opts);
    } else {
        (void)vmaf_feature_dictionary_free(&opts);
    }
    return err;
}

static int cpu_scores(const ReplayCase *c, char keys[MAX_KEYS][KEY_LEN], size_t n_keys,
                      double out[MAX_KEYS])
{
    VmafContext *vmaf = NULL;
    VmafPicture ref;
    VmafPicture dist;
    int err = cpu_context(&vmaf, c);
    if (!err) {
        err = fill_picture(&ref, c, false);
    }
    if (!err) {
        err = fill_picture(&dist, c, true);
        if (err) {
            (void)vmaf_picture_unref(&ref);
        }
    }
    if (!err) {
        err = vmaf_read_pictures(vmaf, &ref, &dist, 0u);
    }
    if (!err) {
        err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    }
    for (size_t k = 0; k < n_keys && !err; k++) {
        err = vmaf_feature_score_at_index(vmaf, keys[k], &out[k], 0u);
    }
    const int closed = vmaf ? vmaf_close(vmaf) : 0;
    return err ? err : closed;
}

/* --- the twin on the host ------------------------------------------------- */

typedef struct ReplayFrame {
    IadmReplayBuffers b;
    uint8_t *accum_x[IADM_METAL_NUM_SCALES]; /* the second viewing distance's slots */
    uint8_t *all[N_BUFFERS];
    size_t bytes[N_BUFFERS];
    unsigned count;
} ReplayFrame;

/* As long as the buffer itself and at least GUARD_MIN: a layout that writes
 * twice as far as the buffer reaches stays inside the guard. */
static size_t guard_bytes(size_t bytes)
{
    return bytes > GUARD_MIN ? bytes : GUARD_MIN;
}

/* A zeroed buffer of `bytes`, followed by a guard band of GUARD_FILL. */
static uint8_t *frame_buffer(ReplayFrame *f, size_t bytes)
{
    uint8_t *p = (uint8_t *)calloc(bytes + guard_bytes(bytes), 1u);
    if (p) {
        (void)memset(p + bytes, (int)GUARD_FILL, guard_bytes(bytes));
        f->all[f->count] = p;
        f->bytes[f->count] = bytes;
        f->count++;
    }
    return p;
}

static bool frame_alloc(ReplayFrame *f, const IadmMetalGeometry *g)
{
    (void)memset(f, 0, sizeof(*f));
    const size_t src = iadm_metal_buffer_bytes(g, IADM_METAL_BUF_SOURCE, 0);
    const size_t dwt = iadm_metal_buffer_bytes(g, IADM_METAL_BUF_DWT_TMP, 0);
    const size_t csf = iadm_metal_buffer_bytes(g, IADM_METAL_BUF_CSF, 0);
    f->b.src_ref = frame_buffer(f, src);
    f->b.src_dis = frame_buffer(f, src);
    f->b.dwt_tmp_ref = frame_buffer(f, dwt);
    f->b.dwt_tmp_dis = frame_buffer(f, dwt);
    f->b.csf_a = frame_buffer(f, csf);
    f->b.csf_f = frame_buffer(f, csf);
    for (int s = 0; s < IADM_METAL_NUM_SCALES; s++) {
        const size_t band = iadm_metal_buffer_bytes(g, IADM_METAL_BUF_BAND, s);
        f->b.ref_band[s] = frame_buffer(f, band);
        f->b.dis_band[s] = frame_buffer(f, band);
        f->b.accum[s] = frame_buffer(f, iadm_metal_buffer_bytes(g, IADM_METAL_BUF_ACCUM, s));
        f->accum_x[s] = frame_buffer(f, iadm_metal_buffer_bytes(g, IADM_METAL_BUF_ACCUM, s));
    }
    return f->count == N_BUFFERS;
}

/* Buffers whose guard band a kernel wrote into. */
static unsigned frame_overruns(const ReplayFrame *f)
{
    unsigned overruns = 0u;
    for (unsigned i = 0u; i < f->count; i++) {
        for (size_t k = 0u; k < guard_bytes(f->bytes[i]); k++) {
            if (f->all[i][f->bytes[i] + k] != GUARD_FILL) {
                overruns++;
                break;
            }
        }
    }
    return overruns;
}

static void frame_free(ReplayFrame *f)
{
    for (unsigned i = 0u; i < f->count; i++) {
        free(f->all[i]);
    }
    f->count = 0u;
}

/* The two input planes as integer_adm_metal.mm's fill_raw_plane() packs them,
 * each sample clamped to the depth as put_sample() stores it. */
static void frame_sources(ReplayFrame *f, const ReplayCase *c)
{
    const unsigned peak = (1u << c->bpc) - 1u;
    for (unsigned row = 0u; row < c->h; row++) {
        for (unsigned col = 0u; col < c->w; col++) {
            const size_t at = ((size_t)row * c->w) + col;
            unsigned r = c->sample(row, col, false, c->bpc, c->ctx);
            unsigned d = c->sample(row, col, true, c->bpc, c->ctx);
            r = r > peak ? peak : r;
            d = d > peak ? peak : d;
            if (c->bpc <= 8u) {
                f->b.src_ref[at] = (uint8_t)r;
                f->b.src_dis[at] = (uint8_t)d;
            } else {
                ((uint16_t *)f->b.src_ref)[at] = (uint16_t)r;
                ((uint16_t *)f->b.src_dis)[at] = (uint16_t)d;
            }
        }
    }
}

static int parse_opt(IadmMetalOptions *o, const char *name, const char *value)
{
    char *end = NULL;
    const double v = strtod(value, &end);
    if (!strcmp(name, "adm_skip_scale0")) {
        o->adm_skip_scale0 = !strcmp(value, "true");
        return 0;
    }
    if (end == value) {
        return -EINVAL;
    }
    if (!strcmp(name, "adm_csf_mode")) {
        o->adm_csf_mode = (int)v;
    } else if (!strcmp(name, "adm_dlm_weight")) {
        o->adm_dlm_weight = v;
    } else if (!strcmp(name, "adm_enhn_gain_limit")) {
        o->adm_enhn_gain_limit = v;
    } else if (!strcmp(name, "adm_min_val")) {
        o->adm_min_val = v;
    } else if (!strcmp(name, "adm_noise_weight")) {
        o->adm_noise_weight = v;
    } else if (!strcmp(name, "adm_p_norm")) {
        o->adm_p_norm = v;
    } else {
        return -EINVAL;
    }
    return 0;
}

/* integer_adm_metal's option defaults (its table is integer_adm.c's), then
 * the case's options. */
static int replay_options(const ReplayCase *c, IadmMetalOptions *o)
{
    (void)memset(o, 0, sizeof(*o));
    o->adm_enhn_gain_limit = DEFAULT_ADM_ENHN_GAIN_LIMIT;
    o->adm_norm_view_dist = DEFAULT_ADM_NORM_VIEW_DIST;
    o->adm_csf_scale = DEFAULT_ADM_CSF_SCALE;
    o->adm_csf_diag_scale = DEFAULT_ADM_CSF_DIAG_SCALE;
    o->adm_noise_weight = DEFAULT_ADM_NOISE_WEIGHT;
    o->adm_min_val = DEFAULT_ADM_MIN_VAL;
    o->adm_dlm_weight = 0.5;
    o->adm_p_norm = 3.0;
    o->adm_ref_display_height = DEFAULT_ADM_REF_DISPLAY_HEIGHT;
    o->adm_csf_mode = DEFAULT_ADM_CSF_MODE;
    int err = 0;
    for (size_t i = 0u; i < c->n_opts && !err; i++) {
        err = parse_opt(o, c->opts[i][0], c->opts[i][1]);
    }
    return err;
}

/* integer_adm_metal.mm's submit_fex_metal() on the host: each scale's DWT
 * once, then the other stages per viewing distance, the second (`o[1]`, when
 * `views` is 2) into its own reduction slots. */
static void replay_frame(const IadmMetalOptions o[2], unsigned views, const IadmMetalGeometry *g,
                         ReplayFrame *f)
{
    IadmReplayBuffers second = f->b;
    for (int scale = 0; scale < IADM_METAL_NUM_SCALES; scale++) {
        second.accum[scale] = f->accum_x[scale];
    }
    for (int scale = 0; scale < IADM_METAL_NUM_SCALES; scale++) {
        for (unsigned v = 0u; v < views; v++) {
            IadmDims d;
            IadmCsf c;
            iadm_metal_uniforms(&o[v], g, scale, &d, &c);
            IadmMetalStage stages[IADM_METAL_MAX_STAGES];
            const unsigned count = iadm_metal_view_stages(&o[v], g, scale, v, stages);
            for (unsigned i = 0u; i < count; i++) {
                iadm_replay_stage(&stages[i], scale, &d, &c, v ? &second : &f->b);
            }
        }
    }
}

/* The scores of viewing distance `view` from its reduction slots. */
static int replay_view_scores(const IadmMetalOptions *o, const IadmMetalGeometry *g,
                              const ReplayFrame *f, unsigned view, IadmMetalScores *r)
{
    uint8_t *const *slots = view ? f->accum_x : f->b.accum;
    const uint32_t *const accum[IADM_METAL_NUM_SCALES] = {
        (const uint32_t *)slots[0], (const uint32_t *)slots[1], (const uint32_t *)slots[2],
        (const uint32_t *)slots[3]};
    return iadm_metal_scores(o, g, accum, 0u, r);
}

/* The case's options at each of its viewing distances; their count. */
static int replay_view_options(const ReplayCase *c, IadmMetalOptions o[2], unsigned *views)
{
    int err = replay_options(c, &o[0]);
    *views = c->nvde ? 2u : 1u;
    o[1] = o[0];
    if (!err && c->nvde) {
        char *end = NULL;
        o[1].adm_norm_view_dist = strtod(c->nvde, &end);
        err = (end == c->nvde) ? -EINVAL : 0;
    }
    for (unsigned v = 0u; v < *views && !err; v++) {
        err = iadm_metal_check_options(&o[v]);
    }
    return err;
}

/* The twin's values of the case, and how many buffers it overran. */
static int replay_scores(const ReplayCase *c, double out[MAX_KEYS], unsigned *overruns)
{
    IadmMetalOptions o[2];
    unsigned views = 1u;
    int err = replay_view_options(c, o, &views);
    if (err) {
        return err;
    }
    IadmMetalGeometry g;
    iadm_metal_geometry(&g, c->w, c->h, c->bpc);
    ReplayFrame f;
    if (!frame_alloc(&f, &g)) {
        frame_free(&f);
        return -ENOMEM;
    }
    frame_sources(&f, c);
    replay_frame(o, views, &g, &f);
    *overruns = frame_overruns(&f);
    IadmMetalScores r;
    err = replay_view_scores(&o[0], &g, &f, 0u, &r);
    if (!err) {
        replay_values(&r, out);
    }
    if (!err && views > 1u) {
        err = replay_view_scores(&o[1], &g, &f, 1u, &r);
        double second[MAX_KEYS];
        replay_values(&r, second);
        for (size_t i = 0u; i < 7u; i++) {
            out[second_view_at(c) + i] = second[i];
        }
    }
    frame_free(&f);
    return err;
}

/* --- comparison ----------------------------------------------------------- */

/* Outputs of the case whose replayed value is not the CPU's, plus one per
 * overrun buffer; UINT32_MAX when a run failed. */
static unsigned case_mismatches(const ReplayCase *c)
{
    char keys[MAX_KEYS][KEY_LEN];
    const size_t n_keys = case_keys(c, keys);
    double cpu[MAX_KEYS] = {0.0};
    double twin[MAX_KEYS] = {0.0};
    unsigned overruns = 0u;
    const int twin_err = replay_scores(c, twin, &overruns);
    const int cpu_err = cpu_scores(c, keys, n_keys, cpu);
    if (twin_err || cpu_err) {
        (void)fprintf(stderr, "\n%s %ux%u: run failed (replay %d, cpu %d)\n", c->name, c->w, c->h,
                      twin_err, cpu_err);
        return UINT32_MAX;
    }
    unsigned bad = overruns;
    if (overruns) {
        (void)fprintf(stderr, "\n%s %ux%u: %u buffers written past their end\n", c->name, c->w,
                      c->h, overruns);
    }
    for (size_t k = 0u; k < n_keys; k++) {
        if (vmaf_test_identical_f64(cpu[k], twin[k])) {
            continue;
        }
        bad++;
        (void)fprintf(stderr, "\n%s %ux%u %u-bit %s: cpu=%.17g replay=%.17g\n", c->name, c->w, c->h,
                      c->bpc, keys[k], cpu[k], twin[k]);
    }
    return bad;
}

static unsigned list_mismatches(const ReplayCase *list, size_t count)
{
    unsigned bad = 0u;
    for (size_t i = 0u; i < count; i++) {
        const unsigned m = case_mismatches(&list[i]);
        bad = (m == UINT32_MAX || bad == UINT32_MAX) ? UINT32_MAX : bad + m;
    }
    return bad;
}

#define LIST_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define CASE(name_, w_, h_, bpc_, fn_)                                                             \
    .name = (name_), .w = (w_), .h = (h_), .bpc = (bpc_), .sample = (fn_)
#define MODEL_OPTS                                                                                 \
    .n_opts = 6u,                                                                                  \
    .opts = {{"adm_csf_mode", "2"},  {"adm_dlm_weight", "0.7"},    {"adm_enhn_gain_limit", "1.0"}, \
             {"adm_min_val", "0.5"}, {"adm_noise_weight", "0.02"}, {"adm_p_norm", "2.0"}}
#define MODEL_SUFFIX "_csf_2_dlmw_0.7_egl_1_min_0.5_nw_0.02_apn_2"

/* --- cases ---------------------------------------------------------------- */

/* Default options, every output including the per-scale sums. */
static char *test_default_options(void)
{
    static const ReplayCase list[] = {
        {CASE("default", 256u, 144u, 8u, textured_sample), .debug = true},
        {CASE("10-bit", 256u, 144u, 10u, textured_sample), .debug = true},
        {CASE("odd frame", 322u, 182u, 8u, textured_sample), .debug = true},
        {CASE("sparse detail", 640u, 360u, 8u, sparse_sample), .debug = true},
    };
    mu_assert("integer_adm_metal's replay differs from the CPU under the default options",
              list_mismatches(list, LIST_LEN(list)) == 0u);
    return NULL;
}

/* The default model's options (adm_noise_weight 0.02: the noise floor of the
 * noise picture shows the float product), Barten and the MAE blend. */
static char *test_model_options(void)
{
    static const ReplayCase list[] = {
        {CASE("model options", 256u, 144u, 8u, textured_sample), .suffix = MODEL_SUFFIX,
         MODEL_OPTS},
        {CASE("model options, noise", 256u, 144u, 8u, noise_sample), .suffix = MODEL_SUFFIX,
         MODEL_OPTS},
        {CASE("Barten", 256u, 144u, 8u, textured_sample), .suffix = "_csf_1", .n_opts = 1u,
         .opts = {{"adm_csf_mode", "1"}}},
        {CASE("blend MAE", 256u, 144u, 8u, textured_sample), .suffix = "_csf_3", .n_opts = 1u,
         .opts = {{"adm_csf_mode", "3"}}},
    };
    mu_assert("integer_adm_metal's replay differs from the CPU under the model options",
              list_mismatches(list, LIST_LEN(list)) == 0u);
    return NULL;
}

/* ADR-2795: a second viewing distance on the DWT of the first, at 8 and 10
 * bits and under the model options (the vmaf_v1.0.16_3d0h / _5d0h pair). */
static char *test_two_viewing_distances(void)
{
    static const ReplayCase list[] = {
        {CASE("two views", 256u, 144u, 8u, textured_sample), .debug = true, .nvde = "5",
         .suffix2 = "_nvd_5"},
        {CASE("two views, 10-bit", 256u, 144u, 10u, textured_sample), .debug = true, .nvde = "5",
         .suffix2 = "_nvd_5"},
        {CASE("two views, model options", 256u, 144u, 8u, textured_sample), .suffix = MODEL_SUFFIX,
         MODEL_OPTS, .nvde = "5", .suffix2 = "_csf_2_dlmw_0.7_egl_1_min_0.5_nw_0.02_nvd_5_apn_2"},
    };
    mu_assert("integer_adm_metal's replay differs from the CPU at two viewing distances",
              list_mismatches(list, LIST_LEN(list)) == 0u);
    return NULL;
}

static char *test_skip_scale0(void)
{
    static const ReplayCase list[] = {
        {CASE("adm_skip_scale0", 256u, 144u, 8u, textured_sample), .debug = true, .suffix = "_ssz",
         .n_opts = 1u, .opts = {{"adm_skip_scale0", "true"}}},
    };
    mu_assert("integer_adm_metal's replay differs from the CPU with adm_skip_scale0",
              list_mismatches(list, LIST_LEN(list)) == 0u);
    return NULL;
}

/* Tiny frames, full-range noise, bright 16-bit samples and isolated patches,
 * against the scalar CPU. */
static char *test_tiny_and_extreme(void)
{
    static const ReplayCase list[] = {
        {CASE("tiny", 17u, 17u, 8u, ramp_sample), .scalar = true},
        {CASE("tiny", 32u, 32u, 8u, ramp_sample), .scalar = true},
        {CASE("tiny", 20u, 64u, 8u, ramp_sample), .scalar = true},
        {CASE("tiny", 64u, 20u, 8u, ramp_sample), .scalar = true},
        {CASE("tiny", 31u, 48u, 8u, ramp_sample), .scalar = true},
        {CASE("noise", 96u, 64u, 8u, noise_sample), .scalar = true},
        {CASE("bright 16-bit", 96u, 64u, 16u, bright16_sample), .scalar = true},
        {CASE("isolated patches", 64u, 64u, 8u, patch_sample), .scalar = true},
    };
    mu_assert("integer_adm_metal's replay differs from the scalar CPU on tiny or extreme frames",
              list_mismatches(list, LIST_LEN(list)) == 0u);
    return NULL;
}

static char *test_gain_limits(void)
{
    static const ReplayCase list[] = {
        {CASE("gain limit 1.2", 96u, 64u, 8u, enhanced_sample), .scalar = true, .debug = true,
         .suffix = "_egl_1.2", .n_opts = 1u, .opts = {{"adm_enhn_gain_limit", "1.2"}}},
        {CASE("gain limit 1.5", 96u, 64u, 8u, enhanced_sample), .scalar = true, .debug = true,
         .suffix = "_egl_1.5", .n_opts = 1u, .opts = {{"adm_enhn_gain_limit", "1.5"}}},
    };
    mu_assert("integer_adm_metal's replay differs from the scalar CPU at fractional gain limits",
              list_mismatches(list, LIST_LEN(list)) == 0u);
    return NULL;
}

/* The two luma planes of the Netflix pair's first frame, or NULL. */
static uint8_t *netflix_luma(void)
{
    static const char *const names[2] = {"src01_hrc00_576x324.yuv", "src01_hrc01_576x324.yuv"};
    const size_t plane = (size_t)576u * 324u;
    uint8_t *luma = (uint8_t *)malloc(2u * plane);
    for (unsigned i = 0u; i < 2u && luma; i++) {
        char path[1024];
        const int len = snprintf(path, sizeof(path), "%s/%s", VMAF_REPLAY_YUV_DIR, names[i]);
        FILE *fp = (len > 0 && (size_t)len < sizeof(path)) ? fopen(path, "rb") : NULL;
        const size_t got = fp ? fread(luma + ((size_t)i * plane), 1u, plane, fp) : 0u;
        if (fp) {
            (void)fclose(fp);
        }
        if (got != plane) {
            free(luma);
            luma = NULL;
        }
    }
    return luma;
}

/* The default model's options on a crop of the Netflix pair. */
static char *test_netflix_crop(void)
{
    uint8_t *luma = netflix_luma();
    if (!luma) {
        (void)fprintf(stderr, "[skip: no src01_hrc0[01]_576x324.yuv in %s] ", VMAF_REPLAY_YUV_DIR);
        return NULL;
    }
    ReplayCase list[] = {
        {CASE("Netflix crop, model options", 256u, 144u, 8u, crop_sample), .ctx = luma,
         .suffix = MODEL_SUFFIX, MODEL_OPTS},
        {CASE("Netflix crop", 256u, 144u, 8u, crop_sample), .ctx = luma, .debug = true},
    };
    const unsigned bad = list_mismatches(list, LIST_LEN(list));
    free(luma);
    mu_assert("integer_adm_metal's replay differs from the CPU on the Netflix crop", bad == 0u);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_default_options);
    mu_run_test(test_model_options);
    mu_run_test(test_skip_scale0);
    mu_run_test(test_two_viewing_distances);
    mu_run_test(test_tiny_and_extreme);
    mu_run_test(test_gain_limits);
    mu_run_test(test_netflix_crop);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
