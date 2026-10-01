/**
 *
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 */

/*
 * float_ms_ssim_hip is the CPU extractor's arithmetic (ADR-1403), checked
 * without an AMD device.
 *
 * The kernels in ms_ssim_score.hip compute every sample through
 * feature/hip/integer_ms_ssim/ms_ssim_arith.h, and integer_ms_ssim_hip.c
 * turns their sums into scores through the host helpers of the same header.
 * This test compiles those lines for the host, replays a frame through them
 * the way the kernels and the extractor do, and holds all 16 outputs of
 * `enable_lcs` (the score and the per-scale l / c / s means) against the CPU
 * extractor, bit for bit.
 *
 * Before ADR-1403 the twin accumulated `sum += sample * tap` in fp32 in the
 * decimate and in both window passes, divided fp64 numerators by fp64
 * denominators, and combined unrounded means; it was 5e-8 to 3e-6 from the
 * CPU on a gfx1036, on every frame.
 *
 * The replay adds l / c / s per 16x8 block and then over the blocks. The
 * device adds lanes, then waves, then blocks, and the CPU in raster order:
 * three orders, one fp32 mean each, because the per-scale mean is rounded to
 * fp32 and the orders differ far below that.
 */

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"

#include "feature/hip/integer_ms_ssim/ms_ssim_arith.h"
#include "feature/picture_copy.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/picture.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe, and this file mirrors
 * the C spelling of the surface it exercises. ADR-1138. */

#define MS_KEYS 16u
#define MS_SCALES VMAF_HIP_MS_SSIM_SCALES
#define MS_TRIM (VMAF_HIP_MS_SSIM_WINDOW - 1)
#define MS_BLOCK_X 16
#define MS_BLOCK_Y 8
#define MS_MAX_FRAMES 2u
#define MS_PAIR_WINDOWS 200000u

static const char *const ms_keys[MS_KEYS] = {
    "float_ms_ssim",          "float_ms_ssim_l_scale0", "float_ms_ssim_l_scale1",
    "float_ms_ssim_l_scale2", "float_ms_ssim_l_scale3", "float_ms_ssim_l_scale4",
    "float_ms_ssim_c_scale0", "float_ms_ssim_c_scale1", "float_ms_ssim_c_scale2",
    "float_ms_ssim_c_scale3", "float_ms_ssim_c_scale4", "float_ms_ssim_s_scale0",
    "float_ms_ssim_s_scale1", "float_ms_ssim_s_scale2", "float_ms_ssim_s_scale3",
    "float_ms_ssim_s_scale4",
};

typedef struct MsCase {
    unsigned w;
    unsigned h;
    unsigned bpc;
    unsigned frames;
} MsCase;

typedef struct MsScores {
    double v[MS_MAX_FRAMES][MS_KEYS];
} MsScores;

/* One pyramid level of both pictures. */
typedef struct MsLevel {
    int w;
    int h;
    float *ref;
    float *cmp;
} MsLevel;

/* The five horizontal-pass planes of one level, (w - 10) x h each. */
typedef struct MsHorizontal {
    int w;
    int h;
    float *plane[5];
} MsHorizontal;

static uint32_t ms_lcg(uint32_t *state)
{
    *state = (*state * 1664525u) + 1013904223u;
    return *state >> 8;
}

static void ms_put(VmafPicture *pic, unsigned plane, unsigned x, unsigned y, unsigned value)
{
    if (pic->bpc <= 8u) {
        ((uint8_t *)pic->data[plane])[(y * pic->stride[plane]) + x] = (uint8_t)value;
    } else {
        ((uint16_t *)pic->data[plane])[(y * (pic->stride[plane] / 2u)) + x] = (uint16_t)value;
    }
}

/* A ramp with texture and noise; the distorted picture is a dimmed copy with
 * a block pattern, so l, c and s all move away from 1 at every scale. */
static int ms_fill(VmafPicture *pic, const MsCase *c, unsigned frame, bool distorted)
{
    const int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, c->bpc, c->w, c->h);
    if (err) {
        return err;
    }
    const unsigned peak = (1u << c->bpc) - 1u;
    const unsigned unit = peak / 255u;
    uint32_t state = 0x9E3779B9u ^ (frame * 2654435761u);
    for (unsigned y = 0; y < c->h; y++) {
        for (unsigned x = 0; x < c->w; x++) {
            const unsigned noise = ms_lcg(&state) & 31u;
            unsigned value = ((((x * 5u) + (y * 3u) + (frame * 11u)) & 127u) + 64u + noise) * unit;
            if (distorted) {
                value = value - (value / 9u) + ((((x / 8u) ^ (y / 8u)) & 1u) * 6u * unit);
            }
            ms_put(pic, 0u, x, y, value > peak ? peak : value);
        }
    }
    for (unsigned p = 1u; p < 3u; p++) {
        for (unsigned y = 0; y < pic->h[p]; y++) {
            for (unsigned x = 0; x < pic->w[p]; x++) {
                ms_put(pic, p, x, y, (peak + 1u) / 2u);
            }
        }
    }
    return 0;
}

static int ms_fill_pair(VmafPicture *ref, VmafPicture *dist, const MsCase *c, unsigned frame)
{
    int err = ms_fill(ref, c, frame, false);
    if (err) {
        return err;
    }
    err = ms_fill(dist, c, frame, true);
    if (err) {
        (void)vmaf_picture_unref(ref);
    }
    return err;
}

/* ---- The CPU extractor ---- */

static int ms_cpu_feed(VmafContext *vmaf, const MsCase *c)
{
    for (unsigned frame = 0; frame < c->frames; frame++) {
        VmafPicture ref;
        VmafPicture dist;
        int err = ms_fill_pair(&ref, &dist, c, frame);
        if (!err) {
            /* vmaf_read_pictures() takes ownership of both pictures. */
            err = vmaf_read_pictures(vmaf, &ref, &dist, frame);
        }
        if (err) {
            return err;
        }
    }
    return vmaf_read_pictures(vmaf, NULL, NULL, 0);
}

static int ms_cpu_collect(VmafContext *vmaf, const MsCase *c, MsScores *out)
{
    for (unsigned frame = 0; frame < c->frames; frame++) {
        for (unsigned k = 0; k < MS_KEYS; k++) {
            const int err = vmaf_feature_score_at_index(vmaf, ms_keys[k], &out->v[frame][k], frame);
            if (err) {
                return err;
            }
        }
    }
    return 0;
}

static int ms_cpu_scores(const MsCase *c, MsScores *out)
{
    const VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    int err = vmaf_init(&vmaf, cfg);
    if (err) {
        return err;
    }
    VmafFeatureDictionary *opts = NULL;
    err = vmaf_feature_dictionary_set(&opts, "enable_lcs", "true");
    if (!err) {
        err = vmaf_use_feature(vmaf, "float_ms_ssim", opts);
        opts = err ? opts : NULL; /* taken on success */
    }
    if (opts) {
        (void)vmaf_feature_dictionary_free(&opts);
    }
    if (!err) {
        err = ms_cpu_feed(vmaf, c);
    }
    if (!err) {
        err = ms_cpu_collect(vmaf, c, out);
    }
    const int closed = vmaf_close(vmaf);
    return err ? err : closed;
}

/* ---- The replay of the kernels ---- */

static void ms_level_free(MsLevel *lv)
{
    free(lv->ref);
    free(lv->cmp);
    lv->ref = NULL;
    lv->cmp = NULL;
}

static int ms_level_alloc(MsLevel *lv, int w, int h)
{
    lv->w = w;
    lv->h = h;
    lv->ref = malloc((size_t)w * (size_t)h * sizeof(float));
    lv->cmp = malloc((size_t)w * (size_t)h * sizeof(float));
    if (lv->ref && lv->cmp) {
        return 0;
    }
    ms_level_free(lv);
    return -1;
}

/* ms_ssim_decimate: one sample per thread of the kernel. */
static void ms_decimate_plane(const float *src, const MsLevel *in, float *dst, const MsLevel *out)
{
    for (int y = 0; y < out->h; y++) {
        for (int x = 0; x < out->w; x++) {
            dst[((size_t)y * (size_t)out->w) + (size_t)x] =
                vmaf_hip_ms_ssim_decimate_sample(src, in->w, in->h, x, y);
        }
    }
}

/* Replace `lv` with the next pyramid level. */
static int ms_level_next(MsLevel *lv)
{
    MsLevel next;
    if (ms_level_alloc(&next, (lv->w / 2) + (lv->w & 1), (lv->h / 2) + (lv->h & 1))) {
        return -1;
    }
    ms_decimate_plane(lv->ref, lv, next.ref, &next);
    ms_decimate_plane(lv->cmp, lv, next.cmp, &next);
    ms_level_free(lv);
    *lv = next;
    return 0;
}

static void ms_horizontal_free(MsHorizontal *hz)
{
    for (unsigned p = 0; p < 5u; p++) {
        free(hz->plane[p]);
        hz->plane[p] = NULL;
    }
}

/* ms_ssim_horiz over one level. */
static int ms_horizontal_run(MsHorizontal *hz, const MsLevel *lv)
{
    hz->w = lv->w - MS_TRIM;
    hz->h = lv->h;
    bool ok = true;
    for (unsigned p = 0; p < 5u; p++) {
        hz->plane[p] = malloc((size_t)hz->w * (size_t)hz->h * sizeof(float));
        ok = ok && hz->plane[p] != NULL;
    }
    if (!ok) {
        ms_horizontal_free(hz);
        return -1;
    }
    for (int y = 0; y < hz->h; y++) {
        for (int x = 0; x < hz->w; x++) {
            const size_t src = ((size_t)y * (size_t)lv->w) + (size_t)x;
            const size_t dst = ((size_t)y * (size_t)hz->w) + (size_t)x;
            const VmafHipMsWindow sums = vmaf_hip_ms_ssim_horizontal(lv->ref + src, lv->cmp + src);
            hz->plane[0][dst] = sums.ref_mu;
            hz->plane[1][dst] = sums.cmp_mu;
            hz->plane[2][dst] = sums.ref_sq;
            hz->plane[3][dst] = sums.cmp_sq;
            hz->plane[4][dst] = sums.refcmp;
        }
    }
    return 0;
}

/* ms_ssim_vert_lcs for one 16x8 block: the l, c and s sums of its samples. */
static void ms_block_sums(const MsHorizontal *hz, int bx, int by, const float *consts, double *sums)
{
    const VmafHipMsPlanes planes = {hz->plane[0], hz->plane[1], hz->plane[2], hz->plane[3],
                                    hz->plane[4]};
    const int h_final = hz->h - MS_TRIM;
    for (int ty = 0; ty < MS_BLOCK_Y; ty++) {
        for (int tx = 0; tx < MS_BLOCK_X; tx++) {
            const int x = (bx * MS_BLOCK_X) + tx;
            const int y = (by * MS_BLOCK_Y) + ty;
            if (x >= hz->w || y >= h_final) {
                continue;
            }
            const VmafHipMsWindow stats = vmaf_hip_ms_ssim_vertical(
                &planes, ((size_t)y * (size_t)hz->w) + (size_t)x, (size_t)hz->w);
            const VmafHipMsLcs terms =
                vmaf_hip_ms_ssim_lcs(&stats, consts[0], consts[1], consts[2]);
            sums[0] += terms.l;
            sums[1] += terms.c;
            sums[2] += terms.s;
        }
    }
}

/* The l, c and s means of one level, as collect() forms them from the
 * per-block sums. */
static int ms_level_means(const MsLevel *lv, const float *consts, double *means)
{
    MsHorizontal hz = {0};
    if (ms_horizontal_run(&hz, lv)) {
        return -1;
    }
    const int h_final = hz.h - MS_TRIM;
    double total[3] = {0.0, 0.0, 0.0};
    for (int by = 0; by < (h_final + MS_BLOCK_Y - 1) / MS_BLOCK_Y; by++) {
        for (int bx = 0; bx < (hz.w + MS_BLOCK_X - 1) / MS_BLOCK_X; bx++) {
            double block[3] = {0.0, 0.0, 0.0};
            ms_block_sums(&hz, bx, by, consts, block);
            total[0] += block[0];
            total[1] += block[1];
            total[2] += block[2];
        }
    }
    const double samples = (double)hz.w * (double)h_final;
    for (unsigned t = 0; t < 3u; t++) {
        means[t] = vmaf_hip_ms_ssim_scale_mean(total[t], samples);
    }
    ms_horizontal_free(&hz);
    return 0;
}

/* Level 0 of both pictures as the extractor uploads it: picture_copy(). */
static int ms_level_from_pictures(MsLevel *lv, const MsCase *c, unsigned frame)
{
    VmafPicture ref;
    VmafPicture dist;
    int err = ms_fill_pair(&ref, &dist, c, frame);
    if (err) {
        return err;
    }
    err = ms_level_alloc(lv, (int)c->w, (int)c->h);
    if (!err) {
        const ptrdiff_t stride = (ptrdiff_t)((size_t)c->w * sizeof(float));
        picture_copy(lv->ref, stride, &ref, 0, c->bpc, 0);
        picture_copy(lv->cmp, stride, &dist, 0, c->bpc, 0);
    }
    (void)vmaf_picture_unref(&ref);
    (void)vmaf_picture_unref(&dist);
    return err;
}

/* The 16 outputs of one frame through the kernel arithmetic. */
static int ms_replay_frame(const MsCase *c, unsigned frame, const float *consts, double *out)
{
    MsLevel lv = {0};
    int err = ms_level_from_pictures(&lv, c, frame);
    double l[MS_SCALES] = {0.0};
    double cc[MS_SCALES] = {0.0};
    double s[MS_SCALES] = {0.0};
    for (unsigned i = 0; i < MS_SCALES && !err; i++) {
        double means[3] = {0.0, 0.0, 0.0};
        err = ms_level_means(&lv, consts, means);
        l[i] = means[0];
        cc[i] = means[1];
        s[i] = means[2];
        if (!err && i + 1u < MS_SCALES) {
            err = ms_level_next(&lv);
        }
    }
    ms_level_free(&lv);
    out[0] = vmaf_hip_ms_ssim_combine(l, cc, s);
    for (unsigned i = 0; i < MS_SCALES; i++) {
        out[1u + i] = l[i];
        out[1u + MS_SCALES + i] = cc[i];
        out[1u + (2u * MS_SCALES) + i] = s[i];
    }
    return err;
}

static int ms_replay_scores(const MsCase *c, const float *consts, MsScores *out)
{
    for (unsigned frame = 0; frame < c->frames; frame++) {
        const int err = ms_replay_frame(c, frame, consts, out->v[frame]);
        if (err) {
            return err;
        }
    }
    return 0;
}

static uint64_t ms_bits(double v)
{
    uint64_t bits = 0u;
    memcpy(&bits, &v, sizeof(bits));
    return bits;
}

/* The outputs of the replay that are not the CPU's bit for bit. */
static unsigned ms_mismatches(const MsCase *c, const MsScores *cpu, const MsScores *replay,
                              bool report)
{
    unsigned differing = 0u;
    for (unsigned frame = 0; frame < c->frames; frame++) {
        for (unsigned k = 0; k < MS_KEYS; k++) {
            if (ms_bits(cpu->v[frame][k]) == ms_bits(replay->v[frame][k])) {
                continue;
            }
            differing++;
            if (report) {
                (void)fprintf(stderr, "\n%ux%u %u-bit %s frame %u: cpu=%.17g replay=%.17g", c->w,
                              c->h, c->bpc, ms_keys[k], frame, cpu->v[frame][k],
                              replay->v[frame][k]);
            }
        }
    }
    return differing;
}

/* Number of outputs of `c` that differ from the CPU extractor's with the
 * constants `consts`, or UINT32_MAX when a run failed. */
static unsigned ms_case_mismatches(const MsCase *c, const float *consts, bool report)
{
    MsScores cpu;
    MsScores replay;
    memset(&cpu, 0, sizeof(cpu));
    memset(&replay, 0, sizeof(replay));
    if (ms_cpu_scores(c, &cpu) != 0 || ms_replay_scores(c, consts, &replay) != 0) {
        return UINT32_MAX;
    }
    if (!(isfinite(cpu.v[0][0]) && cpu.v[0][0] > 0.0 && cpu.v[0][0] < 1.0)) {
        return UINT32_MAX;
    }
    return ms_mismatches(c, &cpu, &replay, report);
}

static char *test_replay_matches_cpu_8bit(void)
{
    const MsCase c = {.w = 352u, .h = 288u, .bpc = 8u, .frames = 2u};
    float consts[3] = {0.0f, 0.0f, 0.0f};
    vmaf_hip_ms_ssim_constants(&consts[0], &consts[1], &consts[2]);
    mu_assert("8-bit 352x288: the kernel arithmetic is not the CPU extractor's bit for bit",
              ms_case_mismatches(&c, consts, true) == 0u);
    return NULL;
}

/* Odd width and height: every pyramid level rounds up, and the mirror of the
 * decimate reaches past both edges. */
static char *test_replay_matches_cpu_10bit_odd(void)
{
    const MsCase c = {.w = 203u, .h = 181u, .bpc = 10u, .frames = 1u};
    float consts[3] = {0.0f, 0.0f, 0.0f};
    vmaf_hip_ms_ssim_constants(&consts[0], &consts[1], &consts[2]);
    mu_assert("10-bit 203x181: the kernel arithmetic is not the CPU extractor's bit for bit",
              ms_case_mismatches(&c, consts, true) == 0u);
    return NULL;
}

/* The comparison has teeth: stabilisation constants two parts in ten million
 * off the reference's fp32 ones move outputs of this fixture. The twin's
 * constants before ADR-1403 were fp64 values, off by about that much. */
static char *test_perturbed_constants_are_detected(void)
{
    const MsCase c = {.w = 352u, .h = 288u, .bpc = 8u, .frames = 1u};
    float consts[3] = {0.0f, 0.0f, 0.0f};
    vmaf_hip_ms_ssim_constants(&consts[0], &consts[1], &consts[2]);
    for (unsigned i = 0; i < 3u; i++) {
        consts[i] *= 1.0000002f;
    }
    const unsigned differing = ms_case_mismatches(&c, consts, false);
    mu_assert("the run with perturbed constants failed", differing != UINT32_MAX);
    mu_assert("constants 2e-7 off the reference's are not seen by the comparison", differing > 0u);
    return NULL;
}

/* One two-sum step is exact: hi + lo is the sum of the two operands, with no
 * rounding at all. This is what a contracted or reassociated build breaks. */
static char *test_pair_step_is_exact(void)
{
    uint32_t state = 0x1234567u;
    unsigned inexact = 0u;
    for (unsigned i = 0; i < MS_PAIR_WINDOWS; i++) {
        const float a = (float)ms_lcg(&state) * (1.0f / 1024.0f);
        const float b = ((float)ms_lcg(&state) * (1.0f / 16777216.0f)) - 0.4f;
        VmafHipMsPair sum = {a, 0.0f};
        vmaf_hip_ms_pair_add(&sum, b);
        inexact += ((double)sum.hi + (double)sum.lo) != ((double)a + (double)b);
    }
    mu_assert("a two-sum step lost part of the sum", inexact == 0u);
    return NULL;
}

/* An eleven-term window sum through the pair rounds to the fp32 value of the
 * reference's fp64 sum, except within about 2^-18 ulp of a rounding boundary:
 * at most one window in ten thousand may differ, by one ulp. A plain fp32
 * running sum differs on almost every second window (95024 of the 200000
 * here; the pair on none). */
static char *test_pair_window_sum_rounds_like_fp64(void)
{
    uint32_t state = 0xBADC0DEu;
    unsigned differing = 0u;
    unsigned fp32_differing = 0u;
    for (unsigned i = 0; i < MS_PAIR_WINDOWS; i++) {
        VmafHipMsPair pair = {0.0f, 0.0f};
        double reference = 0.0;
        float plain = 0.0f;
        for (int u = 0; u < VMAF_HIP_MS_SSIM_WINDOW; u++) {
            const float sample = (float)(ms_lcg(&state) & 0xFFFFu) * (1.0f / 256.0f);
            const float prod = sample * vmaf_hip_ms_ssim_window_tap(u);
            vmaf_hip_ms_pair_add(&pair, prod);
            reference += (double)prod;
            plain += prod;
        }
        differing += vmaf_hip_ms_pair_round(&pair) != (float)reference;
        fp32_differing += plain != (float)reference;
    }
    mu_assert("the pair sum does not round like the reference's fp64 sum",
              differing * 10000u <= MS_PAIR_WINDOWS);
    mu_assert("the fixture cannot tell an fp32 running sum from the reference's",
              fp32_differing * 4u >= MS_PAIR_WINDOWS);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_pair_step_is_exact);
    mu_run_test(test_pair_window_sum_rounds_like_fp64);
    mu_run_test(test_replay_matches_cpu_8bit);
    mu_run_test(test_replay_matches_cpu_10bit_odd);
    mu_run_test(test_perturbed_constants_are_detected);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
