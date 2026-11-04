/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * float_ssim CPU vs. SYCL: the twin returns the CPU's scores bit for bit
 * (ADR-1463; first added as a places=3 parity test, Research-0985, ADR-1370).
 *
 * `float_ssim_sycl` (vmaf_fex_float_ssim_sycl in integer_ssim_sycl.cpp)
 * decimates on the device exactly as ssim.c does, runs the separable 11-tap
 * Gaussian and forms iqa/ssim_tools.c's per-window terms: `lv` and `cv` as
 * the CPU's doubles (in 64-bit integers, a SYCL kernel has no fp64 type) and
 * the fp32 `sv`. The CPU adds `lv * cv * sv`, lv, cv and sv into one double
 * each, window after window in raster order, and returns the means as float.
 * Since ADR-1463 the twin stores the terms per window and the host adds them
 * in that order. Before, it added fixed-point terms per work-group, which is
 * another sum of slightly different terms: the float mean differed by one
 * step on frames whose mean lies next to a rounding boundary.
 *
 * Order cases (one frame each, every output compared by its float bits with
 * the same build's CPU extractor, with and without enable_lcs):
 *   - the constructed 64x64 pair of float_ssim_order_frame.h, shared with
 *     the CUDA and HIP tests: the CPU scores 0xb4e2b622, the old twin scored
 *     0xb4e2b621;
 *   - noise pairs found by search, regenerated here from their seeds: 64x64
 *     and 176x176 pairs on which `float_ssim` differed and a 64x64 pair on
 *     which `float_ssim_l` differed on the old twin.
 * Each order case fails on the old twin. The host checks run the kernels'
 * arithmetic and the host's sums without a device
 * (vmaf_sycl_float_ssim_host_means) on the same frames and on noise.
 *
 * Coverage (every case scores the same frames on both sides, per frame,
 * with ==):
 *   positive  the FIXTURE_W x FIXTURE_H auto case (320x180 = scale 1; the
 *             meson `_large` variant is 960x540 = auto scale 2), auto scale
 *             2 on an odd width (853x480), explicit scales 2, 3, 5 and 10,
 *             an odd scale on odd dimensions, 10- and 12-bit samples, and
 *             enable_lcs at scales 1 and 3 (float_ssim_l / _c / _s too);
 *   boundary  a plane decimated to exactly the 11x11 Gaussian (110x110 at
 *             scale 10), and 3840x2160 auto (scale 8) on the twin gate;
 *   negative  a plane decimated below 11x11 (100x100 at scale 10): the
 *             twin gate refuses it and a direct float_ssim_sycl run fails.
 *
 * Skip behaviour: the host checks always run; without a SYCL device the
 * device cases report the skip and the run exits 77.
 */

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test.h"
#include "mu_table.h"

#include "feature/feature_extractor.h"
#include "libvmaf/feature.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_sycl.h"
#include "libvmaf/picture.h"

#include "float_ssim_order_frame.h"
#include "ssim_order_noise.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

/* 320×180 stays at auto scale 1 (180/256 ≈ 0.7); the `_large` meson variant
 * rebuilds this file at 960x540 (auto scale 2, ADR-1206). */
#ifndef FIXTURE_W
#define FIXTURE_W 320u
#endif
#ifndef FIXTURE_H
#define FIXTURE_H 180u
#endif

#define N_FRAMES 3u
#define N_SCORES 4u

typedef struct ParityCase {
    const char *scale; /* NULL = auto */
    unsigned w;
    unsigned h;
    unsigned bpc;
    bool enable_lcs;
} ParityCase;

static const char *const score_names[N_SCORES] = {"float_ssim", "float_ssim_l", "float_ssim_c",
                                                  "float_ssim_s"};

/* Deterministic texture plus hashed noise on the distorted side, in the
 * picture's sample range. */
static unsigned sample_at(unsigned row, unsigned col, unsigned bpc, unsigned salt)
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

static void fill_plane(VmafPicture *pic, unsigned p, const ParityCase *pc, unsigned salt)
{
    for (unsigned row = 0; row < pic->h[p]; row++) {
        uint8_t *line = (uint8_t *)pic->data[p] + (size_t)row * pic->stride[p];
        for (unsigned col = 0; col < pic->w[p]; col++) {
            const unsigned v = p ? (128u << (pc->bpc - 8u)) : sample_at(row, col, pc->bpc, salt);
            if (pc->bpc > 8u) {
                ((uint16_t *)line)[col] = (uint16_t)v;
            } else {
                line[col] = (uint8_t)v;
            }
        }
    }
}

static int fill_pic(VmafPicture *pic, const ParityCase *pc, unsigned salt)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, pc->bpc, pc->w, pc->h);
    if (err) {
        return err;
    }
    for (unsigned p = 0; p < 3u; p++) {
        fill_plane(pic, p, pc, salt);
    }
    return 0;
}

static int feed_frames(VmafContext *vmaf, const ParityCase *pc)
{
    for (unsigned i = 0; i < N_FRAMES; i++) {
        VmafPicture ref;
        VmafPicture dist;
        int err = fill_pic(&ref, pc, 0u);
        if (err)
            return err;
        err = fill_pic(&dist, pc, i + 1u);
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

static VmafFeatureDictionary *case_options(const ParityCase *pc)
{
    VmafFeatureDictionary *opts = NULL;
    if (pc->scale && vmaf_feature_dictionary_set(&opts, "scale", pc->scale))
        return NULL;
    if (pc->enable_lcs && vmaf_feature_dictionary_set(&opts, "enable_lcs", "true"))
        return NULL;
    return opts;
}

/* One SYCL state per context, as test_sycl_twin_option_parity.c does. */
static VmafSyclState *open_device(void)
{
    VmafSyclState *sycl = NULL;
    VmafSyclConfiguration sycl_cfg = {.device_index = -1};
    if (vmaf_sycl_state_init(&sycl, sycl_cfg) != 0)
        return NULL;
    return sycl;
}

static bool device_present(void)
{
    VmafSyclState *sycl = open_device();
    if (!sycl) {
        (void)fprintf(stderr, "[skip: no SYCL device] ");
        mu_skipped = 1;
        return false;
    }
    vmaf_sycl_state_free(&sycl);
    return true;
}

static char *collect_scores(VmafContext *vmaf, const ParityCase *pc,
                            double scores[N_FRAMES][N_SCORES])
{
    const unsigned n_scores = pc->enable_lcs ? N_SCORES : 1u;
    for (unsigned i = 0; i < N_FRAMES; i++) {
        for (unsigned k = 0; k < n_scores; k++) {
            mu_assert("score missing",
                      !vmaf_feature_score_at_index(vmaf, score_names[k], &scores[i][k], i));
        }
    }
    return NULL;
}

/* Runs `extractor` over the case, on a fresh SYCL state when `on_device`;
 * scores[frame][k] follow score_names. *err receives the
 * vmaf_read_pictures() status instead of failing, so the negative case can
 * assert it. */
static char *run_case(bool on_device, const char *extractor, const ParityCase *pc,
                      double scores[N_FRAMES][N_SCORES], int *err)
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    VmafSyclState *sycl = on_device ? open_device() : NULL;
    mu_assert("SYCL state init failed", sycl || !on_device);
    mu_assert("vmaf_init failed", !vmaf_init(&vmaf, cfg));
    if (sycl)
        mu_assert("vmaf_sycl_import_state failed", !vmaf_sycl_import_state(vmaf, sycl));
    mu_assert("vmaf_use_feature failed", !vmaf_use_feature(vmaf, extractor, case_options(pc)));
    *err = feed_frames(vmaf, pc);
    char *msg = *err ? NULL : collect_scores(vmaf, pc, scores);
    mu_assert("vmaf_close failed", !vmaf_close(vmaf));
    if (sycl)
        vmaf_sycl_state_free(&sycl);
    return msg;
}

static char *compare_case(const ParityCase *pc)
{
    double cpu[N_FRAMES][N_SCORES] = {{0.0}};
    double gpu[N_FRAMES][N_SCORES] = {{0.0}};
    int err = 0;
    char *msg = run_case(false, "float_ssim", pc, cpu, &err);
    if (msg)
        return msg;
    mu_assert("CPU float_ssim run failed", !err);
    msg = run_case(true, "float_ssim_sycl", pc, gpu, &err);
    if (msg)
        return msg;
    mu_assert("SYCL float_ssim_sycl run failed", !err);
    const unsigned n_scores = pc->enable_lcs ? N_SCORES : 1u;
    for (unsigned i = 0; i < N_FRAMES; i++) {
        for (unsigned k = 0; k < n_scores; k++) {
            if (cpu[i][k] != gpu[i][k]) {
                (void)fprintf(stderr,
                              "\n%s %ux%u %u-bit scale=%s frame %u: cpu=%.17g sycl=%.17g "
                              "delta=%.3e\n",
                              score_names[k], pc->w, pc->h, pc->bpc, pc->scale ? pc->scale : "auto",
                              i, cpu[i][k], gpu[i][k], fabs(cpu[i][k] - gpu[i][k]));
            }
            mu_assert("float_ssim_sycl differs from the CPU extractor (ADR-1463)",
                      isfinite(cpu[i][k]) && cpu[i][k] == gpu[i][k]);
        }
    }
    return NULL;
}

static char *test_float_ssim_sycl_registered(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("float_ssim_sycl");
    mu_assert("float_ssim_sycl extractor must be registered", fex != NULL);
    mu_assert("float_ssim_sycl name matches", !strcmp(fex->name, "float_ssim_sycl"));
    return NULL;
}

#define CASE(width, height, depth, scale_opt, lcs)                                                 \
    {.scale = (scale_opt), .w = (width), .h = (height), .bpc = (depth), .enable_lcs = (lcs)}

static const ParityCase parity_cases[] = {
    CASE(FIXTURE_W, FIXTURE_H, 8u, NULL, false), /* auto: 1, or 2 in the _large build */
    CASE(853u, 480u, 8u, NULL, false),           /* auto 2, odd width */
    CASE(320u, 180u, 8u, "2", false),
    CASE(320u, 180u, 8u, "3", false),
    CASE(321u, 181u, 8u, "5", false), /* odd scale, odd dimensions */
    CASE(320u, 180u, 8u, "10", false),
    CASE(110u, 110u, 8u, "10", false), /* boundary: decimates to exactly 11x11 */
    CASE(400u, 224u, 10u, "3", false),
    CASE(400u, 224u, 12u, "2", false),
    CASE(320u, 180u, 8u, "3", true), /* enable_lcs on a decimated plane */
    CASE(320u, 180u, 8u, NULL, true),
    CASE(400u, 224u, 12u, NULL, true),
};

static char *test_float_ssim_cpu_sycl_parity(void)
{
    if (!device_present())
        return NULL;
    char *msg = NULL;
    for (size_t i = 0; i < sizeof(parity_cases) / sizeof(parity_cases[0]) && !msg; i++)
        msg = compare_case(&parity_cases[i]);
    return msg;
}

/* Gate verdict of the SYCL twin for a CPU float_ssim request (ADR-1324). */
static int twin_verdict(unsigned w, unsigned h, const char *scale, const char **twin)
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    VmafSyclState *sycl = open_device();
    if (!sycl)
        return -ENODEV;
    if (vmaf_init(&vmaf, cfg)) {
        vmaf_sycl_state_free(&sycl);
        return -ENOMEM;
    }
    int err = vmaf_sycl_import_state(vmaf, sycl);
    const ParityCase pc = CASE(w, h, 8u, scale, false);
    VmafFeatureDictionary *opts = case_options(&pc);
    const VmafPictureConfiguration pic_cfg = {
        .pic_params = {.w = w, .h = h, .bpc = 8u, .pix_fmt = VMAF_PIX_FMT_YUV420P}};
    if (!err)
        err = vmaf_feature_backend_twin(vmaf, "float_ssim", opts, &pic_cfg, twin, NULL);
    (void)vmaf_feature_dictionary_free(&opts);
    (void)vmaf_close(vmaf);
    vmaf_sycl_state_free(&sycl);
    return err;
}

static char *test_float_ssim_sycl_gate(void)
{
    if (!device_present())
        return NULL;
    const char *twin = NULL;
    const int uhd = twin_verdict(3840u, 2160u, NULL, &twin);
    const char *tiny_twin = NULL;
    const int tiny = twin_verdict(100u, 100u, "10", &tiny_twin);
    double scores[N_FRAMES][N_SCORES] = {{0.0}};
    int err = 0;
    const ParityCase below = CASE(100u, 100u, 8u, "10", false);
    char *msg = run_case(true, "float_ssim_sycl", &below, scores, &err);
    if (msg)
        return msg;
    mu_assert("the twin must serve 3840x2160 at auto scale 8", uhd == 0);
    mu_assert("the twin name is float_ssim_sycl", twin && !strcmp(twin, "float_ssim_sycl"));
    mu_assert("a plane decimated below 11x11 must fall back", tiny == -ENOTSUP);
    mu_assert("a direct run below 11x11 must fail", err != 0);
    return NULL;
}

/* The host copy of the twin after decimation (integer_ssim_sycl.cpp): the
 * kernels' per-window terms and the host's sums, without a device. `means`
 * receives SSIM as the default kernel forms it, L, C, S, and SSIM as the
 * enable_lcs path forms it. */
int vmaf_sycl_float_ssim_host_means(const float *reference, const float *comparison, unsigned width,
                                    unsigned height, double means[5]);

enum {
    ORDER_MEANS = 5,
    ORDER_LCS_SSIM = 4,
    ORDER_MAX_SAMPLES = 176 * 176,
    ORDER_SWEEP_FRAMES = 24,
};

/* One 8-bit 4:2:0 frame pair on which the order of the frame sum decides the
 * float mean: the planes of float_ssim_order_frame.h (`ref` and `dis`), or
 * noise regenerated from `seed` when they are NULL. */
typedef struct OrderCase {
    const char *what;
    unsigned w;
    unsigned h;
    const unsigned char *ref;
    const unsigned char *dis;
    uint64_t seed;
} OrderCase;

static const OrderCase order_frame = {"constructed pair",         FLOAT_SSIM_ORDER_FRAME_W,
                                      FLOAT_SSIM_ORDER_FRAME_H,   float_ssim_order_frame_ref,
                                      float_ssim_order_frame_dis, 0u};

/* Found by a search over noise pairs for a float mean that the CPU's
 * sequential sum and an exact sum of the same terms round differently. */
static const OrderCase order_noise[] = {
    {"noise 64x64, float_ssim", 64u, 64u, NULL, NULL, 17217594u},
    {"noise 64x64, float_ssim_l", 64u, 64u, NULL, NULL, 19119610u},
    {"noise 176x176, float_ssim", 176u, 176u, NULL, NULL, 138433u},
};

/* Luma sample `i` (raster order) of the case's reference (`which` 0) or
 * distorted (1) picture. */
static unsigned order_luma(const OrderCase *c, unsigned which, size_t i)
{
    if (c->ref) {
        return which ? c->dis[i] : c->ref[i];
    }
    return ssim_order_noise_luma(c->seed, which, i);
}

/* The case's picture: the header's Y, U and V bytes, or seeded luma with
 * mid-grey chroma (float_ssim reads luma only). */
static int order_fill(VmafPicture *pic, const OrderCase *c, unsigned which)
{
    const int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, 8u, c->w, c->h);
    if (err) {
        return err;
    }
    const unsigned char *bytes = which ? c->dis : c->ref;
    size_t next = 0u;
    for (unsigned p = 0; p < 3u; p++) {
        for (unsigned row = 0; row < pic->h[p]; row++) {
            uint8_t *line = (uint8_t *)pic->data[p] + (size_t)row * pic->stride[p];
            for (unsigned col = 0; col < pic->w[p]; col++, next++) {
                const unsigned seeded = p ? 128u : order_luma(c, which, next);
                line[col] = (uint8_t)(bytes ? bytes[next] : seeded);
            }
        }
    }
    return 0;
}

static int order_feed(VmafContext *vmaf, const OrderCase *c)
{
    VmafPicture ref;
    VmafPicture dist;
    int err = order_fill(&ref, c, 0u);
    if (err) {
        return err;
    }
    err = order_fill(&dist, c, 1u);
    if (err) {
        (void)vmaf_picture_unref(&ref);
        return err;
    }
    err = vmaf_read_pictures(vmaf, &ref, &dist, 0u);
    return err ? err : vmaf_read_pictures(vmaf, NULL, NULL, 0);
}

/* The case's scores from the CPU extractor or, on a fresh SYCL state, the
 * twin; float_ssim alone, or all four with `lcs`. */
static char *order_scores(bool on_device, const OrderCase *c, bool lcs, double scores[N_SCORES])
{
    const VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    VmafFeatureDictionary *opts = NULL;
    VmafSyclState *sycl = on_device ? open_device() : NULL;
    mu_assert("SYCL state init failed", sycl || !on_device);
    mu_assert("vmaf_init failed", !vmaf_init(&vmaf, cfg));
    int err = sycl ? vmaf_sycl_import_state(vmaf, sycl) : 0;
    if (!err && lcs) {
        err = vmaf_feature_dictionary_set(&opts, "enable_lcs", "true");
    }
    if (!err) {
        err = vmaf_use_feature(vmaf, on_device ? "float_ssim_sycl" : "float_ssim", opts);
    }
    if (!err) {
        err = order_feed(vmaf, c);
    }
    for (unsigned k = 0; k < (lcs ? N_SCORES : 1u) && !err; k++) {
        err = vmaf_feature_score_at_index(vmaf, score_names[k], &scores[k], 0u);
    }
    const int closed = vmaf_close(vmaf);
    if (sycl) {
        vmaf_sycl_state_free(&sycl);
    }
    mu_assert("an order case did not run", !err && !closed);
    return NULL;
}

/* The bits of the float a score is. */
static uint32_t float_bits(double score)
{
    const float value = (float)score;
    uint32_t bits = 0u;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

/* 1 when `got` is not the CPU's float, reported. */
static unsigned order_differs(const OrderCase *c, const char *who, const char *name, double cpu,
                              double got)
{
    if (isfinite(cpu) && float_bits(cpu) == float_bits(got)) {
        return 0u;
    }
    (void)fprintf(stderr, "\n%s, %s %s: cpu=%.17g (0x%08x) got=%.17g (0x%08x)\n", c->what, who,
                  name, cpu, (unsigned)float_bits(cpu), got, (unsigned)float_bits(got));
    return 1u;
}

/* The twin's scores of the case on the device, with and without enable_lcs,
 * against the same build's CPU extractor. */
static char *check_order_device(const OrderCase *c)
{
    double cpu[N_SCORES] = {0.0};
    double plain[N_SCORES] = {0.0};
    double lcs[N_SCORES] = {0.0};
    mu_assert_msg(order_scores(false, c, true, cpu));
    mu_assert_msg(order_scores(true, c, false, plain));
    mu_assert_msg(order_scores(true, c, true, lcs));
    unsigned differing = order_differs(c, "sycl", score_names[0], cpu[0], plain[0]);
    for (unsigned k = 0; k < N_SCORES; k++) {
        differing += order_differs(c, "sycl enable_lcs", score_names[k], cpu[k], lcs[k]);
    }
    mu_assert("float_ssim_sycl differs from the CPU extractor on an order case (ADR-1463)",
              differing == 0u);
    return NULL;
}

/* The same without a device: the kernels' arithmetic and the host's sums on
 * the case's luma planes, against the CPU extractor. */
static char *check_order_host(const OrderCase *c)
{
    static float ref[ORDER_MAX_SAMPLES];
    static float dis[ORDER_MAX_SAMPLES];
    const size_t samples = (size_t)c->w * c->h;
    mu_assert("an order case is larger than the test's planes", samples <= ORDER_MAX_SAMPLES);
    for (size_t i = 0u; i < samples; i++) {
        ref[i] = (float)order_luma(c, 0u, i);
        dis[i] = (float)order_luma(c, 1u, i);
    }
    double cpu[N_SCORES] = {0.0};
    double means[ORDER_MEANS] = {0.0};
    mu_assert_msg(order_scores(false, c, true, cpu));
    mu_assert("the host copy of the twin failed",
              !vmaf_sycl_float_ssim_host_means(ref, dis, c->w, c->h, means));
    unsigned differing =
        order_differs(c, "host", "float_ssim (lcs path)", cpu[0], means[ORDER_LCS_SSIM]);
    for (unsigned k = 0; k < N_SCORES; k++) {
        differing += order_differs(c, "host", score_names[k], cpu[k], means[k]);
    }
    mu_assert("the twin's arithmetic and sums differ from the CPU extractor (ADR-1463)",
              differing == 0u);
    return NULL;
}

/* The constructed pair, without a device: the CPU's score is the pinned one
 * and the twin's arithmetic gives it too. */
static char *test_float_ssim_order_frame_host(void)
{
    double cpu[N_SCORES] = {0.0};
    mu_assert_msg(order_scores(false, &order_frame, false, cpu));
    mu_assert("the CPU no longer scores the constructed pair 0xb4e2b622",
              float_bits(cpu[0]) == FLOAT_SSIM_ORDER_FRAME_CPU_BITS);
    return check_order_host(&order_frame);
}

static char *test_float_ssim_order_noise_host(void)
{
    for (size_t i = 0; i < sizeof(order_noise) / sizeof(order_noise[0]); i++) {
        mu_assert_msg(check_order_host(&order_noise[i]));
    }
    /* Other noise pairs, at sizes that are not a multiple of anything. */
    for (unsigned i = 0; i < ORDER_SWEEP_FRAMES; i++) {
        const OrderCase c = {"noise sweep", 37u + 5u * (i % 4u), 53u + 3u * (i % 5u), NULL, NULL,
                             1000u + i};
        mu_assert_msg(check_order_host(&c));
    }
    return NULL;
}

/* The constructed pair on the device: the pinned CPU float, with and without
 * enable_lcs, and every output equal to this build's CPU extractor. */
static char *test_float_ssim_order_frame_device(void)
{
    if (!device_present()) {
        return NULL;
    }
    double plain[N_SCORES] = {0.0};
    double lcs[N_SCORES] = {0.0};
    mu_assert_msg(order_scores(true, &order_frame, false, plain));
    mu_assert_msg(order_scores(true, &order_frame, true, lcs));
    const bool pinned = float_bits(plain[0]) == FLOAT_SSIM_ORDER_FRAME_CPU_BITS &&
                        float_bits(lcs[0]) == FLOAT_SSIM_ORDER_FRAME_CPU_BITS;
    if (!pinned) {
        (void)fprintf(stderr, "\nconstructed pair, sycl float_ssim: 0x%08x, enable_lcs 0x%08x\n",
                      (unsigned)float_bits(plain[0]), (unsigned)float_bits(lcs[0]));
    }
    mu_assert("float_ssim_sycl does not score the constructed pair 0xb4e2b622 (ADR-1463)", pinned);
    return check_order_device(&order_frame);
}

static char *test_float_ssim_order_noise_device(void)
{
    if (!device_present()) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(order_noise) / sizeof(order_noise[0]); i++) {
        mu_assert_msg(check_order_device(&order_noise[i]));
    }
    return NULL;
}

static char *test_float_ssim_depth(unsigned bpc)
{
    if (!device_present())
        return NULL;
    const ParityCase c = CASE(400u, 224u, bpc, "2", false);
    return compare_case(&c);
}

static char *test_float_ssim_8bit_exact(void)
{
    return test_float_ssim_depth(8u);
}

static char *test_float_ssim_9bit_exact(void)
{
    return test_float_ssim_depth(9u);
}

static char *test_float_ssim_10bit_exact(void)
{
    return test_float_ssim_depth(10u);
}

static char *test_float_ssim_11bit_exact(void)
{
    return test_float_ssim_depth(11u);
}

static char *test_float_ssim_12bit_exact(void)
{
    return test_float_ssim_depth(12u);
}

static char *test_float_ssim_13bit_exact(void)
{
    return test_float_ssim_depth(13u);
}

static char *test_float_ssim_14bit_exact(void)
{
    return test_float_ssim_depth(14u);
}

static char *test_float_ssim_15bit_exact(void)
{
    return test_float_ssim_depth(15u);
}

static char *test_float_ssim_16bit_exact(void)
{
    return test_float_ssim_depth(16u);
}

static char *run_bit_depth_cases(void)
{
    static const MuTest tests[] = {
        MU_TEST(test_float_ssim_8bit_exact),  MU_TEST(test_float_ssim_9bit_exact),
        MU_TEST(test_float_ssim_10bit_exact), MU_TEST(test_float_ssim_11bit_exact),
        MU_TEST(test_float_ssim_12bit_exact), MU_TEST(test_float_ssim_13bit_exact),
        MU_TEST(test_float_ssim_14bit_exact), MU_TEST(test_float_ssim_15bit_exact),
        MU_TEST(test_float_ssim_16bit_exact),
    };
    return mu_run_table(tests, MU_TABLE_LEN(tests));
}

char *run_tests(void)
{
    mu_run_test(test_float_ssim_sycl_registered);
    mu_run_test(test_float_ssim_order_frame_device);
    mu_run_test(test_float_ssim_order_noise_device);
    mu_run_test(test_float_ssim_order_frame_host);
    mu_run_test(test_float_ssim_order_noise_host);
    mu_run_test(test_float_ssim_cpu_sycl_parity);
    mu_assert_msg(run_bit_depth_cases());
    mu_run_test(test_float_ssim_sycl_gate);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
