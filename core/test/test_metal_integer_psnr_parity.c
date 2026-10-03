/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * psnr CPU vs. Metal: `integer_psnr_metal` must return the CPU's scores bit
 * for bit and honour the CPU's options
 * (T-BUG048-GPU-OPTION-PARITY-REMAINDER-2026-09-26: integer_psnr_metal lacks
 * min_sse, enable_mse, reduced_hbd_peak and enable_apsnr;
 * T-GPU-TWIN-PARITY-GAPS-OUTSIDE-CUDA-2026-09-30 item (2): no enable_apsnr
 * and no VMAF_FEATURE_EXTRACTOR_TEMPORAL flag). First added as a 1e-4 dB
 * parity test on one 8-bit frame (ADR-0214), which cannot see either gap.
 *
 * psnr.c sums the squared differences of a plane as integers and derives the
 * score through psnr_score.h, so the twin has no reason to differ by a bit:
 * every comparison is `==`. The cases are the psnr cases of the CUDA test
 * test_cuda_twin_option_parity.c, on a Metal state:
 *   - psnr_y / psnr_cb / psnr_cr at 8, 10, 12 and 16 bits, an odd 4:2:0
 *     frame and an odd 10-bit 4:2:2 frame (test_psnr_*_exact);
 *   - test_psnr_options_bit_exact: min_sse, enable_mse, reduced_hbd_peak and
 *     enable_apsnr together: psnr_*, mse_* per frame and apsnr_* aggregates;
 *   - test_psnr_min_sse_identical_frames: SSE 0 reports the min_sse ceiling;
 *   - test_psnr_apsnr_with_subsample: `--subsample 2` still sums every frame
 *     into apsnr_*, which needs VMAF_FEATURE_EXTRACTOR_TEMPORAL on the twin;
 *   - test_psnr_defaults_add_no_outputs: without the options no mse_* is
 *     emitted and psnr_* is unchanged.
 * The aggregates have no public getter and are read back from the JSON output
 * at round-trip precision (%.17g), as the CUDA test does.
 *
 * Skip behaviour: a case reports the skip without a Metal device; the run
 * exits 77 there. Under VMAF_METAL_TWIN_SELFTEST the CPU extractor stands in
 * for the twin and every case compares it with itself (metal_twin.h).
 */

#include "metal_twin.h"

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <process.h>
#define METAL_PSNR_PID() ((long)_getpid())
#else
#include <unistd.h>
#define METAL_PSNR_PID() ((long)getpid())
#endif

#include "feature/feature_extractor.h"
#include "libvmaf/feature.h"
#include "libvmaf/picture.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy proposes `nullptr`, but the required MSVC C build does
 * not provide that keyword. Preserve the portable C spelling. ADR-1138. */

#define NAME_LEN 64u
#define PATH_LEN 512u
#define N_PLANE_KEYS 3u

typedef struct Fixture {
    enum VmafPixelFormat pix_fmt;
    unsigned bpc;
    unsigned w;
    unsigned h;
    unsigned frames;
    bool identical;
} Fixture;

static const Fixture FX_8 = {VMAF_PIX_FMT_YUV420P, 8u, 320u, 180u, 3u, false};
static const Fixture FX_10 = {VMAF_PIX_FMT_YUV420P, 10u, 320u, 180u, 3u, false};
static const Fixture FX_12 = {VMAF_PIX_FMT_YUV420P, 12u, 320u, 180u, 3u, false};
static const Fixture FX_16 = {VMAF_PIX_FMT_YUV420P, 16u, 320u, 180u, 3u, false};
/* Odd 4:2:0 (ceil chroma), odd-width 10-bit 4:2:2, and an identical pair. */
static const Fixture FX_ODD8 = {VMAF_PIX_FMT_YUV420P, 8u, 161u, 91u, 4u, false};
static const Fixture FX_ODD10 = {VMAF_PIX_FMT_YUV422P, 10u, 129u, 67u, 4u, false};
static const Fixture FX_SAME8 = {VMAF_PIX_FMT_YUV420P, 8u, 161u, 91u, 3u, true};

typedef struct Pair {
    VmafContext *cpu;
    VmafContext *gpu;
    void *state;
} Pair;

static unsigned sample_at(unsigned x, unsigned y, unsigned frame, unsigned plane, unsigned bpc)
{
    const unsigned xs = x + frame * frame * 2u;
    const unsigned base = ((xs * 7u + y * 13u + plane * 31u) ^ ((xs * y) >> 3)) & 0xFFu;
    if (bpc == 8u) {
        return base;
    }
    return (base << (bpc - 8u)) | (xs & ((1u << (bpc - 8u)) - 1u));
}

static int noise_at(unsigned x, unsigned y, unsigned frame)
{
    const uint32_t h = (x * 73856093u) ^ (y * 19349663u) ^ (frame * 83492791u);
    return (int)(((h * 1103515245u + 12345u) >> 16) & 7u) - 3;
}

static unsigned distort(unsigned value, unsigned x, unsigned y, unsigned frame, unsigned bpc)
{
    const int max = (1 << bpc) - 1;
    const int shifted = (int)value + noise_at(x, y, frame) * (1 << (bpc - 8u));
    return (unsigned)(shifted < 0 ? 0 : (shifted > max ? max : shifted));
}

static void fill_plane(VmafPicture *pic, unsigned plane, unsigned frame, bool distorted)
{
    for (unsigned y = 0; y < pic->h[plane]; y++) {
        uint8_t *line = (uint8_t *)pic->data[plane] + (size_t)y * pic->stride[plane];
        for (unsigned x = 0; x < pic->w[plane]; x++) {
            unsigned v = sample_at(x, y, frame, plane, pic->bpc);
            if (distorted) {
                v = distort(v, x, y, frame, pic->bpc);
            }
            if (pic->bpc == 8u) {
                line[x] = (uint8_t)v;
            } else {
                ((uint16_t *)line)[x] = (uint16_t)v;
            }
        }
    }
}

static int make_picture(const Fixture *fx, unsigned frame, bool distorted, VmafPicture *pic)
{
    const int err = vmaf_picture_alloc(pic, fx->pix_fmt, fx->bpc, fx->w, fx->h);
    if (err) {
        return err;
    }
    for (unsigned p = 0; p < 3u; p++) {
        fill_plane(pic, p, frame, distorted);
    }
    return 0;
}

static int feed(VmafContext *vmaf, const Fixture *fx)
{
    for (unsigned i = 0; i < fx->frames; i++) {
        VmafPicture ref;
        VmafPicture dist;
        int err = make_picture(fx, i, false, &ref);
        if (err) {
            return err;
        }
        err = make_picture(fx, i, !fx->identical, &dist);
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

static int use_feature(VmafContext *vmaf, const char *name, const char *const *opts)
{
    VmafFeatureDictionary *dict = NULL;
    for (unsigned i = 0; opts && opts[i]; i += 2u) {
        const int err = vmaf_feature_dictionary_set(&dict, opts[i], opts[i + 1u]);
        if (err) {
            (void)vmaf_feature_dictionary_free(&dict);
            return err;
        }
    }
    return vmaf_use_feature(vmaf, name, dict);
}

/* The Metal state is freed only after its context is closed. */
static void pair_close(Pair *pair)
{
    if (pair->cpu) {
        (void)vmaf_close(pair->cpu);
    }
    if (pair->gpu) {
        (void)vmaf_close(pair->gpu);
    }
    if (pair->state) {
        (void)metal_twin_close(pair->state);
    }
    pair->cpu = NULL;
    pair->gpu = NULL;
    pair->state = NULL;
}

/* 0 when the Metal state and the twin context are ready, 1 when there is no
 * Metal device (the only reason to skip), a negative value for any other
 * setup failure. */
static int open_gpu(Pair *pair, unsigned n_subsample)
{
    if (metal_twin_open(&pair->state) != 0 || !pair->state) {
        return 1;
    }
    const VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE, .n_subsample = n_subsample};
    if (vmaf_init(&pair->gpu, cfg)) {
        return -1;
    }
    return metal_twin_import(pair->gpu, pair->state) == 0 ? 0 : -1;
}

/* Both extractors over `fx`; the message of the first step that failed. */
static mu_message_t pair_setup(Pair *pair, const Fixture *fx, const char *const *opts,
                               unsigned n_subsample)
{
    const VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE, .n_subsample = n_subsample};
    mu_assert("CPU vmaf_init failed", !vmaf_init(&pair->cpu, cfg));
    mu_assert("CPU extractor rejected the options", !use_feature(pair->cpu, "psnr", opts));
    mu_assert("Metal twin rejected the options",
              !use_feature(pair->gpu, METAL_TWIN("integer_psnr_metal", "psnr"), opts));
    mu_assert("CPU run failed", !feed(pair->cpu, fx));
    mu_assert("Metal run failed", !feed(pair->gpu, fx));
    return NULL;
}

/* Runs `psnr` and its twin with the same options over `fx`, both contexts
 * with `n_subsample`. Reports the skip and returns NULL with *ran == false
 * when there is no device; on a failure everything is closed and the message
 * returned. On *ran the caller closes the pair. */
static mu_message_t pair_run(Pair *pair, const Fixture *fx, const char *const *opts,
                             unsigned n_subsample, bool *ran)
{
    *ran = false;
    memset(pair, 0, sizeof(*pair));
    const int opened = open_gpu(pair, n_subsample);
    if (opened > 0) {
        pair_close(pair);
        (void)fprintf(stderr, "[skip: no Metal device] ");
        mu_skipped = 1;
        return NULL;
    }
    mu_message_t msg = opened < 0 ? "Metal context setup failed" : NULL;
    if (!msg) {
        msg = pair_setup(pair, fx, opts, n_subsample);
    }
    if (msg) {
        pair_close(pair);
        return msg;
    }
    *ran = true;
    return NULL;
}

/* Every frame of `name` equal on both sides and finite. */
static mu_message_t expect_exact(const Pair *pair, const char *name, unsigned frames)
{
    for (unsigned i = 0; i < frames; i++) {
        double cpu = NAN;
        double gpu = NAN;
        mu_assert("feature missing on the CPU",
                  !vmaf_feature_score_at_index(pair->cpu, name, &cpu, i));
        mu_assert("feature missing on the Metal twin",
                  !vmaf_feature_score_at_index(pair->gpu, name, &gpu, i));
        if (!isfinite(cpu) || cpu != gpu) {
            (void)fprintf(stderr, "\n%s[%u]: cpu=%.17g %s=%.17g\n", name, i, cpu,
                          METAL_TWIN_BACKEND, gpu);
            return "the Metal twin differs from the CPU extractor";
        }
    }
    return NULL;
}

static mu_message_t expect_all(const Pair *pair, const char *name, unsigned frames, double value)
{
    for (unsigned i = 0; i < frames; i++) {
        double cpu = NAN;
        double gpu = NAN;
        mu_assert("feature missing on the CPU",
                  !vmaf_feature_score_at_index(pair->cpu, name, &cpu, i));
        mu_assert("feature missing on the Metal twin",
                  !vmaf_feature_score_at_index(pair->gpu, name, &gpu, i));
        mu_assert("the CPU value differs from the expected boundary value", cpu == value);
        mu_assert("the Metal value differs from the CPU's boundary value", gpu == value);
    }
    return NULL;
}

static bool feature_present(VmafContext *vmaf, const char *name)
{
    double score = 0.0;
    return vmaf_feature_score_at_index(vmaf, name, &score, 0u) == 0;
}

static mu_message_t expect_absent(const Pair *pair, const char *name)
{
    mu_assert("the CPU emitted a feature its option leaves off", !feature_present(pair->cpu, name));
    mu_assert("the Metal twin emitted a feature its option leaves off",
              !feature_present(pair->gpu, name));
    return NULL;
}

/* Aggregates have no public getter: read them back from the JSON output at
 * round-trip precision. Returns 0 and sets *value, or an errno value. */
static int read_aggregate(VmafContext *vmaf, const char *path, const char *name, double *value)
{
    if (vmaf_write_output_with_format(vmaf, path, VMAF_OUTPUT_FORMAT_JSON, "%.17g")) {
        return -EIO;
    }
    FILE *fh = fopen(path, "rb");
    if (!fh) {
        return -EIO;
    }
    static char buf[1u << 16];
    const size_t n = fread(buf, 1u, sizeof(buf) - 1u, fh);
    (void)fclose(fh);
    (void)remove(path);
    buf[n] = '\0';
    char key[NAME_LEN + 8u];
    (void)snprintf(key, sizeof(key), "\"%s\": ", name);
    const char *section = strstr(buf, "\"aggregate_metrics\"");
    const char *at = section ? strstr(section, key) : NULL;
    if (!at) {
        return -ENOENT;
    }
    char *end = NULL;
    *value = strtod(at + strlen(key), &end);
    return end == at + strlen(key) ? -ENOENT : 0;
}

/* A file name in the temporary directory (TMPDIR, TMP or TEMP; /tmp or the
 * current directory when none is set), unique per process and side, so the
 * tester's working directory may be read-only. */
static void temp_path(char *path, size_t size, const char *side)
{
    const char *dir = getenv("TMPDIR");
    dir = dir ? dir : getenv("TMP");
    dir = dir ? dir : getenv("TEMP");
#ifdef _WIN32
    dir = dir ? dir : ".";
    const char sep = '\\';
#else
    dir = dir ? dir : "/tmp";
    const char sep = '/';
#endif
    (void)snprintf(path, size, "%s%cmetal_psnr_parity_%ld_%s.json", dir, sep, METAL_PSNR_PID(),
                   side);
}

static mu_message_t expect_aggregate(const Pair *pair, const char *name)
{
    double cpu = NAN;
    double gpu = NAN;
    char cpu_path[PATH_LEN];
    char gpu_path[PATH_LEN];
    temp_path(cpu_path, sizeof(cpu_path), "cpu");
    temp_path(gpu_path, sizeof(gpu_path), "metal");
    mu_assert("the CPU aggregate is missing", !read_aggregate(pair->cpu, cpu_path, name, &cpu));
    mu_assert("the Metal aggregate is missing", !read_aggregate(pair->gpu, gpu_path, name, &gpu));
    if (cpu != gpu) {
        (void)fprintf(stderr, "\n%s: cpu=%.17g %s=%.17g\n", name, cpu, METAL_TWIN_BACKEND, gpu);
        return "the Metal aggregate differs from the CPU's";
    }
    return NULL;
}

/* The default outputs of the case at `==`, with `opts` on both sides. */
static mu_message_t psnr_exact(const Fixture *fx, const char *const *opts)
{
    static const char *const names[N_PLANE_KEYS] = {"psnr_y", "psnr_cb", "psnr_cr"};
    Pair pair;
    bool ran = false;
    mu_assert_msg(pair_run(&pair, fx, opts, 1u, &ran));
    mu_message_t msg = NULL;
    for (size_t i = 0; ran && !msg && i < N_PLANE_KEYS; i++) {
        msg = expect_exact(&pair, names[i], fx->frames);
    }
    pair_close(&pair);
    return msg;
}

static char *test_integer_psnr_metal_registered(void)
{
    VmafFeatureExtractor *fex =
        vmaf_get_feature_extractor_by_name(METAL_TWIN("integer_psnr_metal", "psnr"));
    mu_assert("the psnr twin must be registered", fex != NULL);
    return NULL;
}

static char *test_psnr_8bit_exact(void)
{
    return psnr_exact(&FX_8, NULL);
}

static char *test_psnr_10bit_exact(void)
{
    return psnr_exact(&FX_10, NULL);
}

static char *test_psnr_12bit_exact(void)
{
    return psnr_exact(&FX_12, NULL);
}

static char *test_psnr_16bit_exact(void)
{
    return psnr_exact(&FX_16, NULL);
}

static char *test_psnr_odd_frame_exact(void)
{
    mu_assert_msg(psnr_exact(&FX_ODD8, NULL));
    return psnr_exact(&FX_ODD10, NULL);
}

static char *test_psnr_identical_frames_exact(void)
{
    return psnr_exact(&FX_SAME8, NULL);
}

static mu_message_t psnr_all_options(const Fixture *fx)
{
    static const char *const opts[] = {
        "enable_mse", "true",    "enable_apsnr", "true", "reduced_hbd_peak",
        "true",       "min_sse", "0.5",          NULL};
    static const char *const names[] = {"psnr_y", "psnr_cb", "psnr_cr",
                                        "mse_y",  "mse_cb",  "mse_cr"};
    static const char *const aggregates[] = {"apsnr_y", "apsnr_cb", "apsnr_cr"};
    Pair pair;
    bool ran = false;
    mu_assert_msg(pair_run(&pair, fx, opts, 1u, &ran));
    mu_message_t msg = NULL;
    for (size_t i = 0; ran && !msg && i < sizeof(names) / sizeof(names[0]); i++) {
        msg = expect_exact(&pair, names[i], fx->frames);
    }
    for (size_t i = 0; ran && !msg && i < sizeof(aggregates) / sizeof(aggregates[0]); i++) {
        msg = expect_aggregate(&pair, aggregates[i]);
    }
    pair_close(&pair);
    return msg;
}

static char *test_psnr_options_bit_exact(void)
{
    mu_assert_msg(psnr_all_options(&FX_ODD8));
    mu_assert_msg(psnr_all_options(&FX_ODD10));
    return NULL;
}

/* Identical frames: SSE == 0 on every plane, so psnr_y and apsnr_y both
 * report the min_sse ceiling ceil(10 * log10(255^2 / (0.5 / (w * h)))). */
static char *test_psnr_min_sse_identical_frames(void)
{
    static const char *const opts[] = {"min_sse", "0.5", "enable_apsnr", "true", NULL};
    const double ceiling = ceil(10.0 * log10(255.0 * 255.0 / (0.5 / (161.0 * 91.0))));
    Pair pair;
    bool ran = false;
    mu_assert_msg(pair_run(&pair, &FX_SAME8, opts, 1u, &ran));
    mu_message_t msg = ran ? expect_all(&pair, "psnr_y", FX_SAME8.frames, ceiling) : NULL;
    if (ran && !msg) {
        msg = expect_aggregate(&pair, "apsnr_y");
    }
    pair_close(&pair);
    return msg;
}

/* `--subsample 2` on a TEMPORAL extractor still feeds every frame: the CPU
 * psnr sums all four frames into apsnr_*, and the twin must too (without
 * VMAF_FEATURE_EXTRACTOR_TEMPORAL it sums every second frame). */
static char *test_psnr_apsnr_with_subsample(void)
{
    static const char *const opts[] = {"enable_apsnr", "true", NULL};
    static const char *const aggregates[] = {"apsnr_y", "apsnr_cb", "apsnr_cr"};
    Pair pair;
    bool ran = false;
    mu_assert_msg(pair_run(&pair, &FX_ODD8, opts, 2u, &ran));
    mu_message_t msg = NULL;
    for (size_t i = 0; ran && !msg && i < sizeof(aggregates) / sizeof(aggregates[0]); i++) {
        msg = expect_aggregate(&pair, aggregates[i]);
    }
    pair_close(&pair);
    return msg;
}

static char *test_psnr_defaults_add_no_outputs(void)
{
    Pair pair;
    bool ran = false;
    mu_assert_msg(pair_run(&pair, &FX_ODD8, NULL, 1u, &ran));
    mu_message_t msg = ran ? expect_absent(&pair, "mse_y") : NULL;
    if (ran && !msg) {
        msg = expect_exact(&pair, "psnr_y", FX_ODD8.frames);
    }
    pair_close(&pair);
    return msg;
}

static void run_depth_cases(void)
{
    metal_run_case(test_integer_psnr_metal_registered);
    metal_run_case(test_psnr_8bit_exact);
    metal_run_case(test_psnr_10bit_exact);
    metal_run_case(test_psnr_12bit_exact);
    metal_run_case(test_psnr_16bit_exact);
    metal_run_case(test_psnr_odd_frame_exact);
    metal_run_case(test_psnr_identical_frames_exact);
}

static void run_option_cases(void)
{
    metal_run_case(test_psnr_options_bit_exact);
    metal_run_case(test_psnr_min_sse_identical_frames);
    metal_run_case(test_psnr_apsnr_with_subsample);
    metal_run_case(test_psnr_defaults_add_no_outputs);
}

char *run_tests(void)
{
    run_depth_cases();
    run_option_cases();
    return metal_first_failure;
}

/* NOLINTEND(modernize-use-nullptr) */
