/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * float_ms_ssim CPU vs. Metal: the twin must return the CPU's scores bit for
 * bit (T-GPU-FLOAT-MS-SSIM-CPU-ARITHMETIC-2026-10-01; the CUDA, HIP and SYCL
 * twins are exact, ADR-1403, ADR-1465, ADR-1414). First added as a 1e-3
 * parity test on one 8-bit frame (ADR-0589), which any reduction passes.
 *
 * ms_ssim.c runs iqa_ssim() once per scale; iqa_ssim() adds every window's l,
 * c and s into one double each, in raster order, and returns the means as
 * float. A twin that reduces the terms per group, or forms a window sum in
 * another arithmetic, ends on the other side of a float rounding boundary on
 * some frames. Every output of every frame is compared with `==`:
 * float_ms_ssim and, with enable_lcs, the fifteen float_ms_ssim_{l,c,s}_scale
 * {0..4}; with enable_chroma float_ms_ssim_cb and float_ms_ssim_cr.
 *
 *   order    the 176x176 pair of float_ms_ssim_order_frame.h (the CPU's
 *            float_ms_ssim_c_scale1 is 0x3f7c49a0, a per-group sum scores
 *            0x3f7c499f) and a pair rebuilt from a formula (frame 12 376 132
 *            of the splitmix64 family of test_cuda_float_ms_ssim_order.c, on
 *            which float_ms_ssim_l_scale0 is 0x3f7cd999 on the CPU), with
 *            enable_lcs. The CPU's recorded bits are asserted first.
 *   exact    640x480 textured frames at 8 and 10 bits with enable_lcs, as the
 *            exact-twin test of the CUDA backend scores them, and the
 *            minimum frame (176x176).
 *   options  enable_db, clip_db (identical frames score the geometry-derived
 *            ceiling, 105 dB at 512x384 and 8 bits, asserted on the CPU) and
 *            enable_chroma (float_ms_ssim_cb / _cr), all at `==`.
 *
 * Skip behaviour: a case reports the skip without a Metal device; the run
 * exits 77 there. Under VMAF_METAL_TWIN_SELFTEST the CPU extractor stands in
 * for the twin and every case compares it with itself (metal_twin.h).
 * test_metal_ms_ssim_option_semantics.c is a separate test.
 */

#include "metal_twin.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "feature/feature_extractor.h"
#include "libvmaf/feature.h"
#include "libvmaf/picture.h"

#include "float_ms_ssim_order_frame.h"
#include "ssim_order_noise.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy proposes `nullptr`, but the required MSVC C build does
 * not provide that keyword. Preserve the portable C spelling. ADR-1138. */

#define MAX_FRAMES 3u
#define N_SCALES 5u
/* float_ms_ssim, l / c / s of every scale, then _cb and _cr. */
#define N_LCS_KEYS (1u + (3u * N_SCALES))
#define N_KEYS (N_LCS_KEYS + 2u)
#define NAME_BYTES 40u

/* The formula pair of test_cuda_float_ms_ssim_order.c. */
#define FORMULA_FRAME UINT64_C(12376132)
#define FORMULA_KEY "float_ms_ssim_l_scale0"
#define FORMULA_CPU_BITS 0x3f7cd999u

/* Ceiling of clip_db at 512x384, 8 bits: ceil(10 * log10(255^2 / (0.5 / (512 * 384)))). */
#define CLIP_CEILING_512X384 105.0

typedef enum MsSource { SRC_TEXTURE, SRC_HEADER, SRC_FORMULA } MsSource;

typedef struct MsCase {
    const char *label;
    MsSource src;
    unsigned w;
    unsigned h;
    unsigned bpc;
    unsigned frames;
    bool identical; /* the distorted picture is the reference */
    bool lcs;
    bool db;
    bool clip;
    bool chroma;
    const char *key;   /* a CPU output with recorded float bits, or NULL */
    uint32_t key_bits; /* its bits on frame 0 */
    double expect_cpu; /* float_ms_ssim of frame 0 on the CPU, or 0 = not asserted */
} MsCase;

static void key_name(unsigned k, char name[NAME_BYTES])
{
    static const char terms[3] = {'l', 'c', 's'};
    if (k == 0u) {
        (void)snprintf(name, NAME_BYTES, "float_ms_ssim");
    } else if (k < N_LCS_KEYS) {
        (void)snprintf(name, NAME_BYTES, "float_ms_ssim_%c_scale%u", terms[(k - 1u) / N_SCALES],
                       (k - 1u) % N_SCALES);
    } else {
        (void)snprintf(name, NAME_BYTES, k == N_LCS_KEYS ? "float_ms_ssim_cb" : "float_ms_ssim_cr");
    }
}

static bool key_active(const MsCase *c, unsigned k)
{
    if (k == 0u) {
        return true;
    }
    return k < N_LCS_KEYS ? c->lcs : c->chroma;
}

static unsigned texture_at(unsigned plane, unsigned row, unsigned col, unsigned bpc, unsigned salt)
{
    const unsigned max = (1u << bpc) - 1u;
    const unsigned base = (((row ^ col) * 3u + row / 3u + plane * 17u) << (bpc - 8u)) & max;
    if (!salt) {
        return base;
    }
    const unsigned hash = (row * 2654435761u) ^ (col * 40503u) ^ (salt * 97u) ^ (plane * 7919u);
    const int noise = (int)((hash >> 7) % 33u) - 16;
    const int value = (int)base + noise * (int)(1u << (bpc - 8u));
    return value < 0 ? 0u : ((unsigned)value > max ? max : (unsigned)value);
}

static unsigned formula_at(unsigned side, size_t i)
{
    const uint64_t z = ((UINT64_C(2) * FORMULA_FRAME + side) << 32) | (uint64_t)i;
    return (unsigned)(ssim_order_mix64(z) >> 56);
}

/* Sample of plane `p`; the order sources have mid-grey chroma (luma only). */
static unsigned sample_at(const MsCase *c, unsigned p, unsigned side, unsigned frame, unsigned row,
                          unsigned col)
{
    const size_t i = (size_t)row * c->w + col;
    if (c->src == SRC_TEXTURE) {
        return texture_at(p, row, col, c->bpc, side ? frame + 1u : 0u);
    }
    if (p) {
        return 128u;
    }
    if (c->src == SRC_HEADER) {
        return side ? float_ms_ssim_order_dis_luma[i] : float_ms_ssim_order_ref_luma[i];
    }
    return formula_at(side, i);
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

static int fill_pic(VmafPicture *pic, const MsCase *c, unsigned side, unsigned frame)
{
    const int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, c->bpc, c->w, c->h);
    if (err) {
        return err;
    }
    for (unsigned p = 0; p < 3u; p++) {
        for (unsigned row = 0; row < pic->h[p]; row++) {
            for (unsigned col = 0; col < pic->w[p]; col++) {
                put_sample(pic, p, row, col, sample_at(c, p, side, frame, row, col));
            }
        }
    }
    return 0;
}

static int feed_frames(VmafContext *vmaf, const MsCase *c)
{
    for (unsigned i = 0; i < c->frames; i++) {
        VmafPicture ref;
        VmafPicture dist;
        int err = fill_pic(&ref, c, 0u, i);
        if (err) {
            return err;
        }
        err = fill_pic(&dist, c, c->identical ? 0u : 1u, i);
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

static int case_options(const MsCase *c, VmafFeatureDictionary **opts)
{
    int err = 0;
    if (c->lcs) {
        err = vmaf_feature_dictionary_set(opts, "enable_lcs", "true");
    }
    if (!err && c->db) {
        err = vmaf_feature_dictionary_set(opts, "enable_db", "true");
    }
    if (!err && c->clip) {
        err = vmaf_feature_dictionary_set(opts, "clip_db", "true");
    }
    if (!err && c->chroma) {
        err = vmaf_feature_dictionary_set(opts, "enable_chroma", "true");
    }
    return err;
}

static int read_scores(VmafContext *vmaf, const MsCase *c, double out[MAX_FRAMES][N_KEYS])
{
    int err = 0;
    for (unsigned i = 0; i < c->frames * N_KEYS && !err; i++) {
        char name[NAME_BYTES];
        const unsigned k = i % N_KEYS;
        key_name(k, name);
        if (key_active(c, k)) {
            err = vmaf_feature_score_at_index(vmaf, name, &out[i / N_KEYS][k], i / N_KEYS);
        }
    }
    return err;
}

/* Runs the CPU extractor, or the twin on a fresh Metal state, over the case;
 * out[frame][key] follow key_name(). */
static char *run_scores(bool twin, const MsCase *c, double out[MAX_FRAMES][N_KEYS])
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
            vmaf, twin ? METAL_TWIN("float_ms_ssim_metal", "float_ms_ssim") : "float_ms_ssim",
            opts);
    }
    if (!err) {
        err = feed_frames(vmaf, c);
    }
    if (!err) {
        err = read_scores(vmaf, c, out);
    }
    const int closed = vmaf ? vmaf_close(vmaf) : 0;
    if (twin) {
        (void)metal_twin_close(state);
    }
    mu_assert("a float_ms_ssim run failed", !err && !closed);
    return NULL;
}

static uint32_t float_bits(double score)
{
    const float value = (float)score;
    uint32_t bits = 0u;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

/* The outputs of the twin that are not the CPU's, each reported. */
static unsigned count_differences(const MsCase *c, double cpu[MAX_FRAMES][N_KEYS],
                                  double gpu[MAX_FRAMES][N_KEYS])
{
    unsigned differing = 0u;
    for (unsigned i = 0; i < c->frames; i++) {
        for (unsigned k = 0; k < N_KEYS; k++) {
            char name[NAME_BYTES];
            key_name(k, name);
            if (!key_active(c, k) || (isfinite(cpu[i][k]) && cpu[i][k] == gpu[i][k])) {
                continue;
            }
            differing++;
            (void)fprintf(stderr, "\n%s: %s frame %u: cpu=%.17g (0x%08x) %s=%.17g (0x%08x)\n",
                          c->label, name, i, cpu[i][k], (unsigned)float_bits(cpu[i][k]),
                          METAL_TWIN_BACKEND, gpu[i][k], (unsigned)float_bits(gpu[i][k]));
        }
    }
    return differing;
}

/* The recorded CPU values of the case, so a fixture that drifted fails here
 * and not as a puzzling twin difference. */
static char *check_expectation(const MsCase *c, double cpu[MAX_FRAMES][N_KEYS])
{
    for (unsigned k = 0; c->key && k < N_KEYS; k++) {
        char name[NAME_BYTES];
        key_name(k, name);
        if (key_active(c, k) && !strcmp(name, c->key) && float_bits(cpu[0][k]) != c->key_bits) {
            (void)fprintf(stderr, "\n%s: the CPU scores %s 0x%08x, expected 0x%08x\n", c->label,
                          name, (unsigned)float_bits(cpu[0][k]), (unsigned)c->key_bits);
            return "a fixture no longer has its recorded bits on the CPU float_ms_ssim";
        }
    }
    if (c->expect_cpu != 0.0 && cpu[0][0] != c->expect_cpu) {
        (void)fprintf(stderr, "\n%s: the CPU scores %.17g, expected %.17g\n", c->label, cpu[0][0],
                      c->expect_cpu);
    }
    mu_assert("the CPU's float_ms_ssim is no longer the value the fixture was recorded with",
              c->expect_cpu == 0.0 || cpu[0][0] == c->expect_cpu);
    return NULL;
}

static char *compare_case(const MsCase *c)
{
    double cpu[MAX_FRAMES][N_KEYS] = {{0.0}};
    double gpu[MAX_FRAMES][N_KEYS] = {{0.0}};
    mu_assert_msg(run_scores(false, c, cpu));
    mu_assert_msg(check_expectation(c, cpu));
    mu_assert_msg(run_scores(true, c, gpu));
    mu_assert("float_ms_ssim_metal is not bit-identical to the CPU extractor",
              count_differences(c, cpu, gpu) == 0u);
    return NULL;
}

static char *compare_all(const MsCase *cases, size_t n)
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
    mu_assert("float_ms_ssim_metal differs from the CPU extractor", failed == 0u);
    return NULL;
}

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

static char *test_float_ms_ssim_metal_registered(void)
{
    VmafFeatureExtractor *fex =
        vmaf_get_feature_extractor_by_name(METAL_TWIN("float_ms_ssim_metal", "float_ms_ssim"));
    mu_assert("the float_ms_ssim twin must be registered", fex != NULL);
    return NULL;
}

/* Per-scale sums in raster order: all sixteen outputs, enable_lcs. */
static char *test_float_ms_ssim_order_frame_exact(void)
{
    const MsCase cases[] = {
        {"order frame lcs", SRC_HEADER, FLOAT_MS_SSIM_ORDER_W, FLOAT_MS_SSIM_ORDER_H, 8u, 1u, false,
         true, false, false, false, FLOAT_MS_SSIM_ORDER_KEY, FLOAT_MS_SSIM_ORDER_CPU_BITS, 0.0},
        {"order frame", SRC_HEADER, FLOAT_MS_SSIM_ORDER_W, FLOAT_MS_SSIM_ORDER_H, 8u, 1u, false,
         false, false, false, false, NULL, 0u, 0.0},
    };
    return compare_all(cases, ARRAY_LEN(cases));
}

static char *test_float_ms_ssim_order_formula_exact(void)
{
    const MsCase cases[] = {
        {"order formula lcs", SRC_FORMULA, FLOAT_MS_SSIM_ORDER_W, FLOAT_MS_SSIM_ORDER_H, 8u, 1u,
         false, true, false, false, false, FORMULA_KEY, FORMULA_CPU_BITS, 0.0},
    };
    return compare_all(cases, ARRAY_LEN(cases));
}

static char *test_float_ms_ssim_640x480_exact(void)
{
    const MsCase cases[] = {
        {"640x480 8-bit lcs", SRC_TEXTURE, 640u, 480u, 8u, MAX_FRAMES, false, true, false, false,
         false, NULL, 0u, 0.0},
        {"640x480 10-bit lcs", SRC_TEXTURE, 640u, 480u, 10u, MAX_FRAMES, false, true, false, false,
         false, NULL, 0u, 0.0},
        {"640x480 12-bit", SRC_TEXTURE, 640u, 480u, 12u, MAX_FRAMES, false, false, false, false,
         false, NULL, 0u, 0.0},
    };
    return compare_all(cases, ARRAY_LEN(cases));
}

static char *test_float_ms_ssim_minimum_and_odd_frame_exact(void)
{
    const MsCase cases[] = {
        {"176x176 minimum lcs", SRC_TEXTURE, 176u, 176u, 8u, MAX_FRAMES, false, true, false, false,
         false, NULL, 0u, 0.0},
        {"353x251 odd lcs", SRC_TEXTURE, 353u, 251u, 8u, MAX_FRAMES, false, true, false, false,
         false, NULL, 0u, 0.0},
    };
    return compare_all(cases, ARRAY_LEN(cases));
}

static char *test_float_ms_ssim_db_options_exact(void)
{
    const MsCase cases[] = {
        {"512x384 enable_db", SRC_TEXTURE, 512u, 384u, 8u, MAX_FRAMES, false, false, true, false,
         false, NULL, 0u, 0.0},
        {"512x384 enable_db clip_db", SRC_TEXTURE, 512u, 384u, 8u, MAX_FRAMES, false, false, true,
         true, false, NULL, 0u, 0.0},
        {"512x384 enable_db lcs 10-bit", SRC_TEXTURE, 512u, 384u, 10u, MAX_FRAMES, false, true,
         true, false, false, NULL, 0u, 0.0},
    };
    return compare_all(cases, ARRAY_LEN(cases));
}

/* Identical frames with clip_db score the geometry-derived ceiling. */
static char *test_float_ms_ssim_clip_db_ceiling_exact(void)
{
    const MsCase cases[] = {
        {"512x384 identical enable_db clip_db", SRC_TEXTURE, 512u, 384u, 8u, 1u, true, false, true,
         true, false, NULL, 0u, CLIP_CEILING_512X384},
    };
    return compare_all(cases, ARRAY_LEN(cases));
}

static char *test_float_ms_ssim_chroma_exact(void)
{
    const MsCase cases[] = {
        {"512x384 enable_chroma", SRC_TEXTURE, 512u, 384u, 8u, MAX_FRAMES, false, false, false,
         false, true, NULL, 0u, 0.0},
        {"512x384 enable_chroma enable_db 10-bit", SRC_TEXTURE, 512u, 384u, 10u, MAX_FRAMES, false,
         false, true, false, true, NULL, 0u, 0.0},
    };
    return compare_all(cases, ARRAY_LEN(cases));
}

char *run_tests(void)
{
    metal_run_case(test_float_ms_ssim_metal_registered);
    metal_run_case(test_float_ms_ssim_order_frame_exact);
    metal_run_case(test_float_ms_ssim_order_formula_exact);
    metal_run_case(test_float_ms_ssim_640x480_exact);
    metal_run_case(test_float_ms_ssim_minimum_and_odd_frame_exact);
    metal_run_case(test_float_ms_ssim_db_options_exact);
    metal_run_case(test_float_ms_ssim_clip_db_ceiling_exact);
    metal_run_case(test_float_ms_ssim_chroma_exact);
    return metal_first_failure;
}

/* NOLINTEND(modernize-use-nullptr) */
