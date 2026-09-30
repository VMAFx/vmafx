/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * ADR-1384 — device-free replay of the HIP SpEED kernels against speed.c.
 *
 * speed/speed_hip_device.h holds the arithmetic every speed_pipeline.hip
 * work-item runs; speed_internal_gpu_configure() derives the init-time setup
 * and speed_hip_params_fill() / speed_hip_taps_fill() /
 * speed_hip_bindings_*() build the parameter block the device receives, the
 * same calls the extractors make. This test compiles the header for the host (with
 * -ffp-contract=off, like the kernel build) and replays a frame's chain in
 * launch order with the kernels' decomposition: every output of the
 * prescale, decimation, centring, mean and solve kernels, each covariance
 * work-group lane by lane with the same pair-arithmetic tree, the 25x25
 * linear algebra as one work-group of one lane (every phase element is
 * written by exactly one lane from the previous phase, so the result does
 * not depend on the lane count), and the score work-group. The reference is
 * the registered CPU extractor (speed_chroma, speed_temporal) on the same
 * frames.
 *
 * Asserted per frame: speed_chroma_u / _v / _uv and speed_temporal equal the
 * CPU's bit for bit when the replay uses the CPU's own log2f (the one
 * operation whose rounding depends on the host libm: glibc misrounds about
 * 0.4% of arguments, libimf almost none), and the device's log2,
 * speed_hd_log2_rn(), is correctly rounded over a sweep of the arguments SpEED
 * produces. Fixtures: the Netflix-derived 576x324 pair in testdata/
 * (48 frames), a 10-bit synthetic frame (the 2-byte picture_copy() path),
 * prescale with nearest, bilinear and bicubic, every chroma weighting mode,
 * speed_use_ref_diff, and a flat chroma plane (the singular-matrix rule). The
 * lanczos4 prescale is out of scope: the reference evaluates its weights in
 * fp64 (ADR-1358). The device run itself is test_hip_speed_*_parity on an AMD
 * device and scripts/dev/speed_gpu_parity.py.
 */

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"

/* Replay with the CPU extractor's own log2f, so every other operation is
 * compared bit for bit; speed_hd_log2_rn(), the device's log2, is checked
 * separately below (see speed_hip_device.h). */
#define SPEED_HD_HOST_LIBM_LOG2 1

#include "feature/feature_collector.h"
#include "feature/feature_extractor.h"
#include "feature/hip/speed_hip_pipeline.h"
#include "feature/speed_internal.h"
#include "libvmaf/feature.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/picture.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. ADR-1138. */

#ifndef SPEED_TESTDATA_DIR
#define SPEED_TESTDATA_DIR "testdata"
#endif

#define SP_PAIR_W 576u
#define SP_PAIR_H 324u
#define SP_PAIR_FRAMES 48u

/* ------------------------------------------------------------------ */
/* The replayed device state (the pipeline's arena, on the host).      */
/* ------------------------------------------------------------------ */

typedef struct SpEmu {
    SpeedHipParams p;
    SpeedGpuFrameResult result;
    unsigned nonzero; /* frames with a non-zero replayed score */
    size_t plane_bytes;
    unsigned char *raw;
    float *buffers[13];
    int32_t *status;
} SpEmu;

static float *sp_alloc_floats(SpEmu *e, unsigned slot, size_t count)
{
    e->buffers[slot] = calloc(count ? count : 1u, sizeof(float));
    return e->buffers[slot];
}

static int sp_alloc(SpEmu *e, const SpeedHipConfig *c)
{
    const SpeedGpuGeometry *g = &c->shared.geometry;
    const size_t ch = c->channels;
    e->plane_bytes = speed_hip_plane_bytes(g);
    e->raw = calloc(e->plane_bytes * c->raw_planes, 1u);
    e->status = calloc(ch * 2u, sizeof(int32_t));
    SpeedHipParams *p = &e->p;
    p->taps = sp_alloc_floats(e, 0u, 2u * SPEED_HIP_MAX_TAPS);
    p->scaled = sp_alloc_floats(e, 1u, ch * g->scaled_w * g->scaled_h);
    p->down = sp_alloc_floats(e, 2u, ch * g->down_w * g->down_h);
    p->centered = sp_alloc_floats(e, 3u, ch * g->trunc_w * g->trunc_h);
    p->indterm = sp_alloc_floats(e, 4u, ch * SPEED_HIP_N * g->blocks);
    p->means = sp_alloc_floats(e, 5u, ch * SPEED_HIP_N);
    p->cov = sp_alloc_floats(e, 6u, ch * SPEED_HIP_MATRIX);
    p->eig = sp_alloc_floats(e, 7u, ch * SPEED_HIP_N);
    p->qmat = sp_alloc_floats(e, 8u, ch * SPEED_HIP_MATRIX);
    p->rmat = sp_alloc_floats(e, 9u, ch * SPEED_HIP_MATRIX);
    p->var = sp_alloc_floats(e, 10u, ch * g->blocks);
    p->ent = sp_alloc_floats(e, 11u, ch * g->blocks);
    p->contrib = sp_alloc_floats(e, 12u, (ch / 2u) * g->blocks);
    int ok = e->raw && e->status;
    for (unsigned i = 0u; i < 13u; i++)
        ok = ok && e->buffers[i];
    return ok ? 0 : -ENOMEM;
}

static void sp_free(SpEmu *e)
{
    free(e->raw);
    free(e->status);
    for (unsigned i = 0u; i < 13u; i++)
        free(e->buffers[i]);
}

/* speed_hip_pipeline.c's parameter block, on host buffers: the scalars and
 * taps from the pipeline's own fill routines, the pointers into this arena. */
static int sp_create(SpEmu *e, const SpeedHipConfig *c, const SpeedHipBindingSets *b)
{
    memset(e, 0, sizeof(*e));
    const int err = sp_alloc(e, c);
    if (err)
        return err;
    SpeedHipParams *p = &e->p;
    speed_hip_params_fill(p, c, b);
    speed_hip_taps_fill(p->taps, &c->shared.filters);
    p->raw = e->raw;
    p->status = e->status;
    p->result = &e->result;
    return 0;
}

/* The upload of vmaf_hip_picture_upload_staged(): the plane, packed, into
 * raw slot `slot`. */
static void sp_stage(SpEmu *e, uint32_t slot, const VmafPicture *pic, unsigned plane)
{
    const SpeedGpuGeometry *g = &e->p.geometry;
    const size_t row_bytes = (size_t)g->src_w * g->bytes_per_sample;
    unsigned char *dst = e->raw + (size_t)slot * e->plane_bytes;
    const unsigned char *src = pic->data[plane];
    for (uint32_t row = 0u; row < g->src_h; row++)
        memcpy(dst + (size_t)row * row_bytes, src + (ptrdiff_t)row * pic->stride[plane], row_bytes);
}

/* ------------------------------------------------------------------ */
/* The kernels, in launch order.                                       */
/* ------------------------------------------------------------------ */

static void sp_pixels(const SpEmu *e, uint32_t set)
{
    const SpeedHipParams *p = &e->p;
    const SpeedGpuGeometry *g = &p->geometry;
    for (uint32_t ch = 0u; ch < p->channels && g->prescale != 0; ch++) {
        for (uint32_t y = 0u; y < g->scaled_h; y++) {
            for (uint32_t x = 0u; x < g->scaled_w; x++)
                p->scaled[((size_t)ch * g->scaled_h + y) * g->scaled_w + x] =
                    speed_hd_scale_sample(p, set, ch, y, x);
        }
    }
    for (uint32_t ch = 0u; ch < p->channels; ch++) {
        for (uint32_t i = 0u; i < g->down_h; i++) {
            for (uint32_t j = 0u; j < g->down_w; j++)
                p->down[((size_t)ch * g->down_h + i) * g->down_w + j] =
                    speed_hd_antialias_at(p, set, ch, i, j);
        }
    }
    for (uint32_t ch = 0u; ch < p->channels; ch++) {
        for (uint32_t i = 0u; i < g->trunc_h; i++) {
            for (uint32_t j = 0u; j < g->trunc_w; j++)
                speed_hd_centre_item(p, ch, i, j);
        }
    }
}

/* speed_hip_covariance: every lane's partial, then the pair-arithmetic tree
 * level by level, exactly as the work-group reduces them. */
static void sp_covariance_entry(const SpeedHipParams *p, uint32_t ch, uint32_t entry)
{
    float hi[SPEED_HIP_GROUP];
    float lo[SPEED_HIP_GROUP];
    uint32_t x = 0u;
    uint32_t y = 0u;
    speed_hd_triangle_entry(entry, &x, &y);
    const uint32_t group = p->cov_group;
    for (uint32_t lid = 0u; lid < group; lid++) {
        const SpeedHdFf part = speed_hd_covariance_partial(p, ch, x, y, lid, group);
        hi[lid] = part.hi;
        lo[lid] = part.lo;
    }
    for (uint32_t span = group / 2u; span > 0u; span >>= 1u) {
        for (uint32_t lid = 0u; lid < span; lid++) {
            const SpeedHdFf merged = speed_hd_ff_add(speed_hd_ff(hi[lid], lo[lid]),
                                                     speed_hd_ff(hi[lid + span], lo[lid + span]));
            hi[lid] = merged.hi;
            lo[lid] = merged.lo;
        }
    }
    speed_hd_covariance_store(p, ch, x, y, speed_hd_ff(hi[0], lo[0]));
}

static void sp_statistics(const SpEmu *e)
{
    const SpeedHipParams *p = &e->p;
    for (uint32_t ch = 0u; ch < p->channels; ch++) {
        for (uint32_t element = 0u; element < SPEED_HIP_N; element++)
            p->means[ch * SPEED_HIP_N + element] = speed_hd_submatrix_mean(p, ch, element);
    }
    for (uint32_t ch = 0u; ch < p->channels; ch++) {
        for (uint32_t entry = 0u; entry < SPEED_HIP_TRIANGLE; entry++)
            sp_covariance_entry(p, ch, entry);
    }
    static float slm[SPEED_HIP_SLM_FLOATS];
    const SpeedHdLanes lanes = {0u, 1u};
    for (uint32_t ch = 0u; ch < p->channels; ch++)
        speed_hd_linalg_group(&lanes, p, ch, slm);
}

static void sp_scoring(const SpEmu *e)
{
    const SpeedHipParams *p = &e->p;
    for (uint32_t ch = 0u; ch < p->channels; ch++) {
        for (uint32_t block = 0u; block < p->geometry.blocks; block++)
            speed_hd_block_statistics(p, ch, block);
    }
    for (uint32_t pair = 0u; pair < p->channels / 2u; pair++) {
        for (uint32_t b = 0u; b < p->geometry.blocks; b++)
            p->contrib[(size_t)pair * p->geometry.blocks + b] = speed_hd_block_score(p, pair, b);
        speed_hd_score_finish(p, pair);
    }
}

static void sp_chain(SpEmu *e, uint32_t set)
{
    sp_pixels(e, set);
    sp_statistics(e);
    sp_scoring(e);
    e->nonzero += e->result.score[0] != 0.0f || e->result.score[1] != 0.0f;
}

/* ------------------------------------------------------------------ */
/* Frames.                                                             */
/* ------------------------------------------------------------------ */

typedef struct SpCase {
    const char *name;
    int temporal;
    unsigned w; /* 0: the testdata pair */
    unsigned h;
    unsigned bpc;
    unsigned first; /* first frame index (0 for speed_temporal) */
    unsigned frames;
    int flat_v;             /* synthetic: a constant V plane (singular chroma) */
    const char *options[2]; /* "key=value" extractor options, or NULL */
} SpCase;

static uint32_t sp_lcg(uint32_t x)
{
    return x * 1664525u + 1013904223u;
}

static void sp_put(VmafPicture *pic, unsigned plane, unsigned row, unsigned col, unsigned v)
{
    if (pic->bpc <= 8u)
        ((uint8_t *)pic->data[plane])[row * pic->stride[plane] + col] = (uint8_t)v;
    else
        ((uint16_t *)pic->data[plane])[row * (pic->stride[plane] / 2u) + col] = (uint16_t)v;
}

/* Smooth texture plus deterministic noise; the distorted side is noisier. */
static int sp_synthetic(VmafPicture *pic, const SpCase *c, unsigned frame, int distort)
{
    const int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, c->bpc, c->w, c->h);
    const unsigned max_val = (1u << c->bpc) - 1u;
    for (unsigned plane = 0u; plane < 3u && !err; plane++) {
        for (unsigned row = 0u; row < pic->h[plane]; row++) {
            for (unsigned col = 0u; col < pic->w[plane]; col++) {
                const uint32_t h = sp_lcg(row * 7919u + col * 104729u + frame * 31u + plane);
                const unsigned base = (row * 5u + col * 3u + frame * 7u + plane * 11u) % 200u;
                const unsigned noise = (h >> 26) % (distort ? 17u : 5u);
                const unsigned v8 = (c->flat_v && plane == 2u) ? 128u : 20u + base + noise;
                /* Above 8 bits, hash bits fill the low bits, so picture_copy()'s
                 * division by 2^(bpc - 8) is not exact. */
                const unsigned low = (c->flat_v && plane == 2u) ? 0u : (h >> 3);
                const unsigned v = (v8 << (c->bpc - 8u)) | (low & ((1u << (c->bpc - 8u)) - 1u));
                sp_put(pic, plane, row, col, v & max_val);
            }
        }
    }
    return err;
}

/* Frame `index` of an 8-bit 4:2:0 raw file of the testdata pair. */
static int sp_read(FILE *f, unsigned index, VmafPicture *pic)
{
    const size_t frame_bytes = (size_t)SP_PAIR_W * SP_PAIR_H * 3u / 2u;
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, 8u, SP_PAIR_W, SP_PAIR_H);
    if (err)
        return err;
    if (fseek(f, (long)(frame_bytes * index), SEEK_SET) != 0)
        return -EIO;
    for (unsigned plane = 0u; plane < 3u && !err; plane++) {
        for (unsigned row = 0u; row < pic->h[plane] && !err; row++) {
            uint8_t *dst = (uint8_t *)pic->data[plane] + row * pic->stride[plane];
            err = fread(dst, 1u, pic->w[plane], f) == pic->w[plane] ? 0 : -EIO;
        }
    }
    return err;
}

typedef struct SpSource {
    FILE *ref;
    FILE *dis;
} SpSource;

static int sp_frame(const SpSource *src, const SpCase *c, unsigned index, VmafPicture *ref,
                    VmafPicture *dis)
{
    if (c->w != 0u) {
        const int err = sp_synthetic(ref, c, index, 0);
        return err ? err : sp_synthetic(dis, c, index, 1);
    }
    const int err = sp_read(src->ref, index, ref);
    return err ? err : sp_read(src->dis, index, dis);
}

/* ------------------------------------------------------------------ */
/* The CPU extractor.                                                  */
/* ------------------------------------------------------------------ */

typedef struct SpCpu {
    VmafFeatureExtractorContext *ctx;
    VmafFeatureCollector *fc;
} SpCpu;

/* Add one "key=value" option to the extractor's dictionary. */
static int sp_dict_option(VmafFeatureDictionary **opts, const char *option)
{
    char key[64];
    const char *eq = strchr(option, '=');
    const size_t len = eq ? (size_t)(eq - option) : 0u;
    if (!eq || len >= sizeof(key))
        return -EINVAL;
    memcpy(key, option, len);
    key[len] = '\0';
    return vmaf_feature_dictionary_set(opts, key, eq + 1);
}

static int sp_cpu_open(SpCpu *cpu, const SpCase *c)
{
    const VmafFeatureExtractor *fex =
        vmaf_get_feature_extractor_by_name(c->temporal ? "speed_temporal" : "speed_chroma");
    VmafFeatureDictionary *opts = NULL;
    int err = fex ? 0 : -EINVAL;
    for (unsigned i = 0u; i < 2u && !err && c->options[i]; i++)
        err = sp_dict_option(&opts, c->options[i]);
    if (!err)
        err = vmaf_feature_extractor_context_create(&cpu->ctx, fex, (VmafDictionary *)opts);
    if (!err)
        err = vmaf_feature_collector_init(&cpu->fc);
    return err;
}

static void sp_cpu_close(SpCpu *cpu)
{
    if (cpu->ctx) {
        (void)vmaf_feature_extractor_context_close(cpu->ctx);
        (void)vmaf_feature_extractor_context_destroy(cpu->ctx);
    }
    if (cpu->fc)
        vmaf_feature_collector_destroy(cpu->fc);
}

/* The CPU score of output `name` at frame `index`. With non-default options
 * the collector key is the alias plus the option suffixes (e.g.
 * speed_chroma_u_ps_1.5), so `alias_prefix` ("speed_chroma_u_") matches it. */
static int sp_cpu_score(const SpCpu *cpu, const char *name, const char *alias_prefix,
                        unsigned index, double *score)
{
    for (unsigned j = 0u; j < cpu->fc->cnt; j++) {
        FeatureVector *fv = cpu->fc->feature_vector[j];
        if (!strcmp(fv->name, name) || !strncmp(fv->name, alias_prefix, strlen(alias_prefix)))
            return vmaf_feature_vector_get_score(fv, score, index);
    }
    return -EINVAL;
}

/* ------------------------------------------------------------------ */
/* The HIP configuration, as the extractors' init() builds it.         */
/* ------------------------------------------------------------------ */

typedef struct SpOptions {
    SpeedInternalOptions opt;
    char method[16];
    int use_ref_diff;
} SpOptions;

static int sp_apply_option(const char *option, SpOptions *o)
{
    const char *value = strchr(option, '=');
    if (!value)
        return -EINVAL;
    value++;
    if (!strncmp(option, "speed_prescale=", 15u)) {
        o->opt.speed_prescale = strtod(value, NULL);
    } else if (!strncmp(option, "speed_prescale_method=", 22u)) {
        if (strlen(value) >= sizeof(o->method))
            return -EINVAL;
        memcpy(o->method, value, strlen(value) + 1u);
    } else if (!strncmp(option, "speed_weight_var_mode=", 22u)) {
        o->opt.speed_weight_var_mode = (int)strtol(value, NULL, 10);
    } else if (!strncmp(option, "speed_kernelscale=", 18u)) {
        o->opt.speed_kernelscale = strtod(value, NULL);
    } else if (!strncmp(option, "speed_use_ref_diff=", 19u)) {
        o->use_ref_diff = 1;
    } else {
        return -EINVAL;
    }
    return 0;
}

static int sp_options(const SpCase *c, SpOptions *o)
{
    memset(o, 0, sizeof(*o));
    memcpy(o->method, "nearest", 8u);
    o->opt.speed_kernelscale = 1.0;
    o->opt.speed_prescale = 1.0;
    o->opt.speed_prescale_method = o->method;
    o->opt.speed_sigma_nn = 0.29;
    int err = 0;
    for (unsigned i = 0u; i < 2u && !err && c->options[i]; i++)
        err = sp_apply_option(c->options[i], o);
    return err;
}

static int sp_configure(const SpCase *c, unsigned w, unsigned h, unsigned bpc,
                        SpeedHipConfig *config, SpeedHipBindingSets *b)
{
    SpOptions o;
    unsigned pw = w;
    unsigned ph = h;
    int err = sp_options(c, &o);
    if (!err && !c->temporal)
        err = speed_chroma_dimensions(w, h, VMAF_PIX_FMT_YUV420P, &pw, &ph);
    SpeedInternalDimensions dim;
    if (!err)
        err = speed_internal_init_dimensions(&dim, (int)pw, (int)ph, o.opt.speed_prescale);
    if (!err)
        err = speed_internal_gpu_configure(&dim, &o.opt, bpc, &config->shared);
    config->channels = c->temporal ? 2u : 4u;
    config->raw_planes = 4u;
    config->staged = c->temporal ? 2u : 4u;
    if (c->temporal)
        speed_hip_bindings_temporal(o.use_ref_diff, b);
    else
        speed_hip_bindings_chroma(b);
    if (!err && !speed_hip_config_valid(config, b))
        err = -EINVAL;
    return err;
}

/* ------------------------------------------------------------------ */
/* Comparison.                                                         */
/* ------------------------------------------------------------------ */

static double sp_clamped(float score)
{
    return (double)score > 1000.0 ? 1000.0 : (double)score;
}

static int sp_expect(const SpCpu *cpu, const SpCase *c, const char *output, unsigned frame,
                     double replay)
{
    char name[96];
    char alias[64];
    const char *family = strncmp(output, "speed_chroma", 12u) ? "temporal" : "chroma";
    (void)snprintf(name, sizeof(name), "Speed_%s_feature_%s_score", family, output);
    (void)snprintf(alias, sizeof(alias), "%s_", output);
    double reference = NAN;
    const int err = sp_cpu_score(cpu, name, alias, frame, &reference);
    if (!err && reference == replay)
        return 0;
    (void)fprintf(stderr, "\n%s frame %u %s: device replay %.17g, CPU %.17g (err %d)\n", c->name,
                  frame, output, replay, reference, err);
    return 1;
}

static int sp_compare_chroma(const SpEmu *e, const SpCpu *cpu, const SpCase *c, unsigned frame)
{
    const SpeedGpuFrameResult *r = &e->result;
    const int singular_u = r->singular[0] != 0 || r->singular[1] != 0;
    const int singular_v = r->singular[2] != 0 || r->singular[3] != 0;
    float uv = (r->score[0] + r->score[1]) * 0.5f;
    if (singular_u && !singular_v)
        uv = r->score[1];
    if (singular_v && !singular_u)
        uv = r->score[0];
    int fail = sp_expect(cpu, c, "speed_chroma_u", frame, sp_clamped(r->score[0]));
    fail |= sp_expect(cpu, c, "speed_chroma_v", frame, sp_clamped(r->score[1]));
    fail |= sp_expect(cpu, c, "speed_chroma_uv", frame, sp_clamped(uv));
    return fail;
}

/* Stage, replay and compare one frame. */
static int sp_step(SpEmu *e, const SpCpu *cpu, const SpCase *c, unsigned frame, VmafPicture *ref,
                   VmafPicture *dis)
{
    int fail = 0;
    if (c->temporal) {
        const uint32_t set = frame % 2u;
        sp_stage(e, 2u * set, ref, 0u);
        sp_stage(e, 2u * set + 1u, dis, 0u);
        if (frame > 0u) {
            sp_chain(e, set);
            fail = sp_expect(cpu, c, "speed_temporal", frame, sp_clamped(e->result.score[0]));
        }
        return fail;
    }
    sp_stage(e, 0u, ref, 1u);
    sp_stage(e, 1u, dis, 1u);
    sp_stage(e, 2u, ref, 2u);
    sp_stage(e, 3u, dis, 2u);
    sp_chain(e, 0u);
    return sp_compare_chroma(e, cpu, c, frame);
}

static int sp_frames(SpEmu *e, SpCpu *cpu, const SpCase *c, const SpSource *src)
{
    int fail = 0;
    for (unsigned frame = c->first; frame < c->first + c->frames && !fail; frame++) {
        VmafPicture ref;
        VmafPicture dis;
        memset(&ref, 0, sizeof(ref));
        memset(&dis, 0, sizeof(dis));
        fail = sp_frame(src, c, frame, &ref, &dis) != 0;
        if (!fail)
            fail = vmaf_feature_extractor_context_extract(cpu->ctx, &ref, NULL, &dis, NULL, frame,
                                                          cpu->fc) != 0;
        if (!fail)
            fail = sp_step(e, cpu, c, frame, &ref, &dis);
        if (ref.ref)
            (void)vmaf_picture_unref(&ref);
        if (dis.ref)
            (void)vmaf_picture_unref(&dis);
    }
    return fail;
}

static int sp_run_case(const SpCase *c, const SpSource *src)
{
    const unsigned w = c->w ? c->w : SP_PAIR_W;
    const unsigned h = c->w ? c->h : SP_PAIR_H;
    const unsigned bpc = c->w ? c->bpc : 8u;
    SpeedHipConfig config;
    SpeedHipBindingSets bindings;
    SpCpu cpu = {NULL, NULL};
    SpEmu e;
    memset(&e, 0, sizeof(e));
    int fail = sp_configure(c, w, h, bpc, &config, &bindings) != 0;
    fail = fail || sp_create(&e, &config, &bindings) != 0 || sp_cpu_open(&cpu, c) != 0;
    if (fail)
        (void)fprintf(stderr, "\n%s: setup failed\n", c->name);
    fail = fail || sp_frames(&e, &cpu, c, src);
    /* A replay of zeros proves little: every case but the singular one must
     * score something. */
    if (!fail && !c->flat_v && e.nonzero == 0u) {
        (void)fprintf(stderr, "\n%s: every replayed score is 0\n", c->name);
        fail = 1;
    }
    sp_free(&e);
    sp_cpu_close(&cpu);
    return fail;
}

/* ------------------------------------------------------------------ */
/* Tests.                                                              */
/* ------------------------------------------------------------------ */

/*  name, temporal, w, h, bpc, first, frames, flat_v, options. The early frames
 *  of the pair's chroma score 0 (singular or flat), so the chroma option cases
 *  start at frame 20; speed_temporal must start at frame 0. */
static const SpCase sp_pair_cases[] = {
    {"chroma, 576x324 pair", 0, 0u, 0u, 8u, 0u, SP_PAIR_FRAMES, 0, {NULL, NULL}},
    {"temporal, 576x324 pair", 1, 0u, 0u, 8u, 0u, SP_PAIR_FRAMES, 0, {NULL, NULL}},
    {"temporal, prescale 0.5 nearest", 1, 0u, 0u, 8u, 0u, 30u, 0, {"speed_prescale=0.5", NULL}},
    {"chroma, prescale 1.5 bilinear",
     0,
     0u,
     0u,
     8u,
     20u,
     8u,
     0,
     {"speed_prescale=1.5", "speed_prescale_method=bilinear"}},
    {"temporal, prescale 0.75 bicubic",
     1,
     0u,
     0u,
     8u,
     0u,
     30u,
     0,
     {"speed_prescale=0.75", "speed_prescale_method=bicubic"}},
    {"chroma, kernelscale 0.5", 0, 0u, 0u, 8u, 20u, 8u, 0, {"speed_kernelscale=0.5", NULL}},
    {"temporal, speed_use_ref_diff", 1, 0u, 0u, 8u, 0u, 30u, 0, {"speed_use_ref_diff=true", NULL}},
    {"chroma, weight mode 1", 0, 0u, 0u, 8u, 20u, 8u, 0, {"speed_weight_var_mode=1", NULL}},
    {"chroma, weight mode 2", 0, 0u, 0u, 8u, 20u, 8u, 0, {"speed_weight_var_mode=2", NULL}},
    {"chroma, weight mode 3", 0, 0u, 0u, 8u, 20u, 8u, 0, {"speed_weight_var_mode=3", NULL}},
    {"chroma, weight mode 4", 0, 0u, 0u, 8u, 20u, 8u, 0, {"speed_weight_var_mode=4", NULL}},
    {"chroma, weight mode 5", 0, 0u, 0u, 8u, 20u, 8u, 0, {"speed_weight_var_mode=5", NULL}},
    {"chroma, weight mode 6", 0, 0u, 0u, 8u, 20u, 8u, 0, {"speed_weight_var_mode=6", NULL}},
};

static const SpCase sp_synthetic_cases[] = {
    {"chroma, 10-bit 960x540", 0, 960u, 540u, 10u, 0u, 2u, 0, {NULL, NULL}},
    {"temporal, 10-bit 960x540", 1, 960u, 540u, 10u, 0u, 3u, 0, {NULL, NULL}},
    {"chroma, flat V plane (singular)", 0, 640u, 480u, 8u, 0u, 2u, 1, {NULL, NULL}},
};

static char *test_replay_matches_cpu_on_the_testdata_pair(void)
{
    SpSource src = {fopen(SPEED_TESTDATA_DIR "/ref_576x324_48f.yuv", "rb"),
                    fopen(SPEED_TESTDATA_DIR "/dis_576x324_48f.yuv", "rb")};
    const int present = src.ref && src.dis;
    int fail = 0;
    const size_t n = sizeof(sp_pair_cases) / sizeof(sp_pair_cases[0]);
    for (size_t i = 0u; i < n && present; i++)
        fail |= sp_run_case(&sp_pair_cases[i], &src);
    if (src.ref)
        (void)fclose(src.ref);
    if (src.dis)
        (void)fclose(src.dis);
    mu_assert("testdata/{ref,dis}_576x324_48f.yuv missing", present);
    mu_assert("HIP SpEED device replay differs from the CPU extractor", !fail);
    return NULL;
}

static char *test_replay_matches_cpu_on_synthetic_frames(void)
{
    const SpSource src = {NULL, NULL};
    int fail = 0;
    const size_t n = sizeof(sp_synthetic_cases) / sizeof(sp_synthetic_cases[0]);
    for (size_t i = 0u; i < n; i++)
        fail |= sp_run_case(&sp_synthetic_cases[i], &src);
    mu_assert("HIP SpEED device replay differs from the CPU extractor", !fail);
    return NULL;
}

/* The device's log2 equals log2 rounded once to fp32 (the fp64 result
 * rounded to fp32 is the oracle, as in ADR-1358) on every 61st pattern from
 * 2^-6 to 2^24, which covers l * variance + sigma_nn and 1 + variance. */
static char *test_device_log2_is_correctly_rounded(void)
{
    unsigned mismatches = 0u;
    unsigned checked = 0u;
    for (uint32_t bits = 0x3c800000u; bits <= 0x4b800000u; bits += 61u) {
        float x = 0.0f;
        memcpy(&x, &bits, sizeof(x));
        const float oracle = (float)log2((double)x);
        mismatches += speed_hd_log2_rn(x) != oracle;
        checked++;
    }
    if (mismatches)
        (void)fprintf(stderr, "\nspeed_hd_log2_rn: %u of %u arguments misrounded\n", mismatches,
                      checked);
    mu_assert("device log2 is not correctly rounded", mismatches == 0u);
    mu_assert("device log2: special values", speed_hd_log2_rn(1.0f) == 0.0f &&
                                                 speed_hd_log2_rn(0.0f) == -HUGE_VALF &&
                                                 speed_hd_log2_rn(0x1p-140f) == -140.0f);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_device_log2_is_correctly_rounded);
    mu_run_test(test_replay_matches_cpu_on_the_testdata_pair);
    mu_run_test(test_replay_matches_cpu_on_synthetic_frames);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
