/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * float_ssim CPU vs. Metal: the twin must return the CPU's scores bit for bit
 * (T-GPU-TWIN-PARITY-GAPS-OUTSIDE-CUDA-2026-09-30 item (1); the CUDA, HIP and
 * SYCL twins are exact, ADR-1399, ADR-1464, ADR-1463). First added as a 1e-3
 * parity test on one 8-bit frame (ADR-0589), which any reduction passes.
 *
 * The Metal twin implements scale 1 only (a separate row), so every frame here
 * resolves to scale 1: either its short side is below 384 (the automatic
 * scale is round(min(w, h) / 256), at least 1) or `scale=1` is set on BOTH
 * sides. Every output of every frame is compared with `==`; a float_ssim
 * output that is not finite on the CPU fails the case too.
 *
 *   order    iqa_ssim() adds the window terms into one double per output in
 *            raster order. The 64x64 pair of float_ssim_order_frame.h (the CPU
 *            scores 0xb4e2b622; a per-group sum scores a neighbouring float)
 *            and the seeded noise pairs of ssim_order_noise.h are scored with
 *            and without enable_lcs (float_ssim, float_ssim_l / _c / _s).
 *   texture  textured frames with noise at 8, 10 and 12 bits, an odd frame,
 *            scale=1 on a 960x540 frame, enable_lcs, enable_db and clip_db.
 *   flat     identical flat 64x64 frames (value 128 at 8 bits, 512 at 10 bits)
 *            with enable_db: the CPU divides double numerators by fp32
 *            denominators and scores 72.247198959355487 dB, not +inf. The
 *            Metal kernel computes l, c and s in fp32 and forces 1 on a zero
 *            denominator. The CPU's value is asserted as well as `==`, with
 *            clip_db too (its ceiling lies above that value).
 *
 * Skip behaviour: a case reports the skip without a Metal device; the run
 * exits 77 there. Under VMAF_METAL_TWIN_SELFTEST the CPU extractor stands in
 * for the twin and every case compares it with itself (metal_twin.h).
 */

#include "float_bits.h"
#include "metal_twin.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "feature/feature_extractor.h"
#include "libvmaf/feature.h"
#include "libvmaf/picture.h"

#include "float_ssim_order_frame.h"
#include "ssim_order_noise.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy proposes `nullptr`, but the required MSVC C build does
 * not provide that keyword. Preserve the portable C spelling. ADR-1138. */

#define MAX_FRAMES 3u
#define N_KEYS 4u

/* float_ssim of identical flat frames with enable_db on the CPU. */
#define FLAT_DB_CPU 72.247198959355487

typedef enum SsimSource { SRC_TEXTURE, SRC_FLAT, SRC_HEADER, SRC_NOISE } SsimSource;

typedef struct SsimCase {
    const char *label;
    const char *scale; /* NULL = automatic */
    uint64_t seed;     /* SRC_NOISE pair */
    double expect_cpu; /* float_ssim of frame 0 on the CPU, or 0 = not asserted */
    SsimSource src;
    unsigned w;
    unsigned h;
    unsigned bpc;
    unsigned frames;
    unsigned flat;        /* SRC_FLAT sample value */
    uint32_t expect_bits; /* float bits of float_ssim of frame 0 on the CPU, or 0 */
    bool lcs;
    bool db;
    bool clip;
} SsimCase;

static const char *const key_names[N_KEYS] = {"float_ssim", "float_ssim_l", "float_ssim_c",
                                              "float_ssim_s"};

static unsigned texture_at(unsigned row, unsigned col, unsigned bpc, unsigned salt)
{
    const unsigned max = (1u << bpc) - 1u;
    const unsigned base = (((row ^ col) * 3u + row / 3u) << (bpc - 8u)) & max;
    if (!salt) {
        return base;
    }
    const unsigned hash = (row * 2654435761u) ^ (col * 40503u) ^ (salt * 97u);
    const int noise = (int)((hash >> 7) % 33u) - 16;
    const int value = (int)base + noise * (int)(1u << (bpc - 8u));
    return value < 0 ? 0u : ((unsigned)value > max ? max : (unsigned)value);
}

static unsigned luma_at(const SsimCase *c, unsigned side, unsigned frame, unsigned row,
                        unsigned col)
{
    const size_t i = (size_t)row * c->w + col;
    switch (c->src) {
    case SRC_FLAT:
        return c->flat;
    case SRC_HEADER:
        return side ? float_ssim_order_frame_dis[i] : float_ssim_order_frame_ref[i];
    case SRC_NOISE:
        return ssim_order_noise_luma(c->seed, side, i);
    default:
        return texture_at(row, col, c->bpc, side ? frame + 1u : 0u);
    }
}

static void put_sample(VmafPicture *pic, unsigned p, unsigned row, unsigned col, unsigned v)
{
    uint8_t *line = (uint8_t *)pic->data[p] + (size_t)row * pic->stride[p];
    if (pic->bpc > 8u) {
        ((uint16_t *)line)[col] = (uint16_t)v;
    } else {
        line[col] = (uint8_t)v;
    }
}

static int fill_pic(VmafPicture *pic, const SsimCase *c, unsigned side, unsigned frame)
{
    const int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, c->bpc, c->w, c->h);
    if (err) {
        return err;
    }
    for (unsigned p = 0; p < 3u; p++) {
        for (unsigned row = 0; row < pic->h[p]; row++) {
            for (unsigned col = 0; col < pic->w[p]; col++) {
                const unsigned v = p ? (128u << (c->bpc - 8u)) : luma_at(c, side, frame, row, col);
                put_sample(pic, p, row, col, v);
            }
        }
    }
    return 0;
}

static int feed_frames(VmafContext *vmaf, const SsimCase *c)
{
    for (unsigned i = 0; i < c->frames; i++) {
        VmafPicture ref;
        VmafPicture dist;
        int err = fill_pic(&ref, c, 0u, i);
        if (err) {
            return err;
        }
        err = fill_pic(&dist, c, 1u, i);
        if (err) {
            (void)vmaf_picture_unref(&ref);
            return err;
        }
        err = vmaf_read_pictures(vmaf, &ref, &dist, i);
        if (err) {
            return err;
        }
    }
    return vmaf_read_pictures(vmaf, NULL, NULL, 0);
}

static int case_options(const SsimCase *c, VmafFeatureDictionary **opts)
{
    int err = 0;
    if (c->scale) {
        err = vmaf_feature_dictionary_set(opts, "scale", c->scale);
    }
    if (!err && c->lcs) {
        err = vmaf_feature_dictionary_set(opts, "enable_lcs", "true");
    }
    if (!err && c->db) {
        err = vmaf_feature_dictionary_set(opts, "enable_db", "true");
    }
    if (!err && c->clip) {
        err = vmaf_feature_dictionary_set(opts, "clip_db", "true");
    }
    return err;
}

/* Runs the CPU extractor, or the twin on a fresh Metal state, over the case;
 * out[frame][key] follow key_names. */
static char *run_scores(bool twin, const SsimCase *c, double out[MAX_FRAMES][N_KEYS])
{
    const VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    VmafFeatureDictionary *opts = NULL;
    void *state = NULL;
    int err = twin ? metal_twin_open(&state) : 0;
    mu_assert("Metal state init failed", !err && (state || !twin));
    err = vmaf_init(&vmaf, cfg);
    if (!err && twin) {
        err = metal_twin_import(vmaf, state);
    }
    if (!err) {
        err = case_options(c, &opts);
    }
    if (!err) {
        err = vmaf_use_feature(
            vmaf, twin ? METAL_TWIN("float_ssim_metal", "float_ssim") : "float_ssim", opts);
    }
    if (!err) {
        err = feed_frames(vmaf, c);
    }
    for (unsigned i = 0; i < c->frames * N_KEYS && !err; i++) {
        const unsigned k = i % N_KEYS;
        if (k == 0u || c->lcs) {
            err = vmaf_feature_score_at_index(vmaf, key_names[k], &out[i / N_KEYS][k], i / N_KEYS);
        }
    }
    const int closed = vmaf ? vmaf_close(vmaf) : 0;
    if (twin) {
        (void)metal_twin_close(state);
    }
    mu_assert("a float_ssim run failed", !err && !closed);
    return NULL;
}

/* The bits of the score rounded to float, the type the CPU computes it in. */
static uint32_t float_bits(double score)
{
    return vmaf_test_bits_f32((float)score);
}

/* The outputs of the twin that are not the CPU's, each reported. */
static unsigned count_differences(const SsimCase *c, double cpu[MAX_FRAMES][N_KEYS],
                                  double gpu[MAX_FRAMES][N_KEYS])
{
    unsigned differing = 0u;
    for (unsigned i = 0; i < c->frames; i++) {
        for (unsigned k = 0; k < (c->lcs ? N_KEYS : 1u); k++) {
            if (isfinite(cpu[i][k]) && vmaf_test_identical_f64(cpu[i][k], gpu[i][k])) {
                continue;
            }
            differing++;
            (void)fprintf(stderr,
                          "\n%s: %s frame %u: cpu=%.17g (0x%08x) %s=%.17g (0x%08x) delta=%.3e\n",
                          c->label, key_names[k], i, cpu[i][k], (unsigned)float_bits(cpu[i][k]),
                          METAL_TWIN_BACKEND, gpu[i][k], (unsigned)float_bits(gpu[i][k]),
                          fabs(cpu[i][k] - gpu[i][k]));
        }
    }
    return differing;
}

static char *check_expectation(const SsimCase *c, double cpu[MAX_FRAMES][N_KEYS])
{
    const bool recorded = c->expect_cpu == 0.0 || vmaf_test_identical_f64(cpu[0][0], c->expect_cpu);
    if (!recorded) {
        (void)fprintf(stderr, "\n%s: the CPU scores %.17g, expected %.17g\n", c->label, cpu[0][0],
                      c->expect_cpu);
    }
    mu_assert("the CPU's float_ssim is no longer the value the fixture was recorded with",
              recorded);
    if (c->expect_bits != 0u && float_bits(cpu[0][0]) != c->expect_bits) {
        (void)fprintf(stderr, "\n%s: the CPU scores 0x%08x, expected 0x%08x\n", c->label,
                      (unsigned)float_bits(cpu[0][0]), (unsigned)c->expect_bits);
    }
    mu_assert("the CPU's float_ssim is no longer the bits the fixture was recorded with",
              c->expect_bits == 0u || float_bits(cpu[0][0]) == c->expect_bits);
    return NULL;
}

static char *compare_case(const SsimCase *c)
{
    double cpu[MAX_FRAMES][N_KEYS] = {{0.0}};
    double gpu[MAX_FRAMES][N_KEYS] = {{0.0}};
    mu_assert_msg(run_scores(false, c, cpu));
    mu_assert_msg(check_expectation(c, cpu));
    mu_assert_msg(run_scores(true, c, gpu));
    mu_assert("float_ssim_metal is not bit-identical to the CPU extractor",
              count_differences(c, cpu, gpu) == 0u);
    return NULL;
}

static char *compare_all(const SsimCase *cases, size_t n)
{
    if (!metal_twin_have_device()) {
        return NULL;
    }
    unsigned failed = 0u;
    for (size_t i = 0; i < n; i++) {
        const char *msg = compare_case(&cases[i]);
        if (msg) {
            failed++;
            (void)fprintf(stderr, "\n%s: %s\n", cases[i].label, msg);
        }
    }
    mu_assert("float_ssim_metal differs from the CPU extractor", failed == 0u);
    return NULL;
}

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

static char *test_float_ssim_metal_registered(void)
{
    VmafFeatureExtractor *fex =
        vmaf_get_feature_extractor_by_name(METAL_TWIN("float_ssim_metal", "float_ssim"));
    mu_assert("the float_ssim twin must be registered", fex != NULL);
    return NULL;
}

static char *test_float_ssim_order_frame_exact(void)
{
    const SsimCase cases[] = {
        {.label = "order frame",
         .src = SRC_HEADER,
         .w = FLOAT_SSIM_ORDER_FRAME_W,
         .h = FLOAT_SSIM_ORDER_FRAME_H,
         .bpc = 8u,
         .frames = 1u,
         .expect_bits = FLOAT_SSIM_ORDER_FRAME_CPU_BITS},
        {.label = "order frame lcs",
         .src = SRC_HEADER,
         .w = FLOAT_SSIM_ORDER_FRAME_W,
         .h = FLOAT_SSIM_ORDER_FRAME_H,
         .bpc = 8u,
         .frames = 1u,
         .lcs = true,
         .expect_bits = FLOAT_SSIM_ORDER_FRAME_CPU_BITS},
    };
    return compare_all(cases, ARRAY_LEN(cases));
}

static char *test_float_ssim_order_noise_exact(void)
{
    const SsimCase cases[] = {
        {.label = "noise 64x64 a",
         .src = SRC_NOISE,
         .w = 64u,
         .h = 64u,
         .bpc = 8u,
         .frames = 1u,
         .seed = 17217594u,
         .lcs = true},
        {.label = "noise 64x64 b",
         .src = SRC_NOISE,
         .w = 64u,
         .h = 64u,
         .bpc = 8u,
         .frames = 1u,
         .seed = 19119610u,
         .lcs = true},
        {.label = "noise 176x176",
         .src = SRC_NOISE,
         .w = 176u,
         .h = 176u,
         .bpc = 8u,
         .frames = 1u,
         .seed = 138433u,
         .lcs = true},
        {.label = "noise 176x176 plain",
         .src = SRC_NOISE,
         .w = 176u,
         .h = 176u,
         .bpc = 8u,
         .frames = 1u,
         .seed = 138433u},
    };
    return compare_all(cases, ARRAY_LEN(cases));
}

static char *test_float_ssim_texture_8bit_exact(void)
{
    const SsimCase cases[] = {
        {.label = "320x180 8-bit",
         .src = SRC_TEXTURE,
         .w = 320u,
         .h = 180u,
         .bpc = 8u,
         .frames = MAX_FRAMES},
        {.label = "320x180 8-bit lcs",
         .src = SRC_TEXTURE,
         .w = 320u,
         .h = 180u,
         .bpc = 8u,
         .frames = MAX_FRAMES,
         .lcs = true},
        {.label = "321x181 odd 8-bit",
         .src = SRC_TEXTURE,
         .w = 321u,
         .h = 181u,
         .bpc = 8u,
         .frames = MAX_FRAMES,
         .lcs = true},
        {.label = "960x540 scale=1",
         .src = SRC_TEXTURE,
         .w = 960u,
         .h = 540u,
         .bpc = 8u,
         .frames = MAX_FRAMES,
         .scale = "1",
         .lcs = true},
    };
    return compare_all(cases, ARRAY_LEN(cases));
}

static char *test_float_ssim_texture_hbd_exact(void)
{
    const SsimCase cases[] = {
        {.label = "320x180 10-bit",
         .src = SRC_TEXTURE,
         .w = 320u,
         .h = 180u,
         .bpc = 10u,
         .frames = MAX_FRAMES,
         .lcs = true},
        {.label = "320x180 12-bit",
         .src = SRC_TEXTURE,
         .w = 320u,
         .h = 180u,
         .bpc = 12u,
         .frames = MAX_FRAMES,
         .lcs = true},
        {.label = "321x181 odd 10-bit",
         .src = SRC_TEXTURE,
         .w = 321u,
         .h = 181u,
         .bpc = 10u,
         .frames = MAX_FRAMES},
    };
    return compare_all(cases, ARRAY_LEN(cases));
}

static char *test_float_ssim_db_options_exact(void)
{
    const SsimCase cases[] = {
        {.label = "320x180 enable_db",
         .src = SRC_TEXTURE,
         .w = 320u,
         .h = 180u,
         .bpc = 8u,
         .frames = MAX_FRAMES,
         .db = true},
        {.label = "320x180 enable_db clip_db",
         .src = SRC_TEXTURE,
         .w = 320u,
         .h = 180u,
         .bpc = 8u,
         .frames = MAX_FRAMES,
         .db = true,
         .clip = true},
        {.label = "320x180 enable_db lcs",
         .src = SRC_TEXTURE,
         .w = 320u,
         .h = 180u,
         .bpc = 10u,
         .frames = MAX_FRAMES,
         .lcs = true,
         .db = true},
    };
    return compare_all(cases, ARRAY_LEN(cases));
}

/* Flat identical frames: the CPU's dB value is 72.247198959355487 (the score
 * is 1 - 2^-24, not exactly 1), and the twin must reach it. */
static char *test_float_ssim_flat_db_exact(void)
{
    const SsimCase cases[] = {
        {.label = "flat 8-bit enable_db",
         .src = SRC_FLAT,
         .w = 64u,
         .h = 64u,
         .bpc = 8u,
         .frames = 1u,
         .flat = 128u,
         .db = true,
         .expect_cpu = FLAT_DB_CPU},
        {.label = "flat 10-bit enable_db",
         .src = SRC_FLAT,
         .w = 64u,
         .h = 64u,
         .bpc = 10u,
         .frames = 1u,
         .flat = 512u,
         .db = true,
         .expect_cpu = FLAT_DB_CPU},
        {.label = "flat 8-bit lcs enable_db",
         .src = SRC_FLAT,
         .w = 64u,
         .h = 64u,
         .bpc = 8u,
         .frames = 1u,
         .flat = 128u,
         .lcs = true,
         .db = true,
         .expect_cpu = FLAT_DB_CPU},
    };
    return compare_all(cases, ARRAY_LEN(cases));
}

static char *test_float_ssim_flat_clip_db_exact(void)
{
    const SsimCase cases[] = {
        {.label = "flat 8-bit enable_db clip_db",
         .src = SRC_FLAT,
         .w = 64u,
         .h = 64u,
         .bpc = 8u,
         .frames = 1u,
         .flat = 128u,
         .db = true,
         .clip = true,
         .expect_cpu = FLAT_DB_CPU},
        {.label = "flat 10-bit enable_db clip_db",
         .src = SRC_FLAT,
         .w = 64u,
         .h = 64u,
         .bpc = 10u,
         .frames = 1u,
         .flat = 512u,
         .db = true,
         .clip = true,
         .expect_cpu = FLAT_DB_CPU},
    };
    return compare_all(cases, ARRAY_LEN(cases));
}

/* T-METAL-UINT-PLANE-INDEX-2026-10-05: the five (w - 10) x h moment planes are
 * indexed in uint, so float_ssim_metal refuses the picture cap at scale=1
 * (32758 x 32768 samples per plane, past 858,993,459). */
static char *test_float_ssim_refuses_uint_index_overflow(void)
{
    if (metal_twin_device_only() || !metal_twin_have_device()) {
        return NULL;
    }
    mu_assert("float_ssim_metal accepted the picture cap at scale=1",
              metal_twin_init_status("float_ssim_metal", "scale", "1", METAL_TWIN_PIC_DIM_MAX,
                                     METAL_TWIN_PIC_DIM_MAX) == -EINVAL);
    return NULL;
}

char *run_tests(void)
{
    metal_run_case(test_float_ssim_metal_registered);
    metal_run_case(test_float_ssim_refuses_uint_index_overflow);
    metal_run_case(test_float_ssim_order_frame_exact);
    metal_run_case(test_float_ssim_order_noise_exact);
    metal_run_case(test_float_ssim_texture_8bit_exact);
    metal_run_case(test_float_ssim_texture_hbd_exact);
    metal_run_case(test_float_ssim_db_options_exact);
    metal_run_case(test_float_ssim_flat_db_exact);
    metal_run_case(test_float_ssim_flat_clip_db_exact);
    return metal_first_failure;
}

/* NOLINTEND(modernize-use-nullptr) */
