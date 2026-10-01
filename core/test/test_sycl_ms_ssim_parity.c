/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * SYCL kernel coverage round 2 — MS-SSIM CPU vs. SYCL parity test
 * (ADR-0884).
 *
 * MS-SSIM (5-scale Multi-Scale SSIM with exponent weighting) is
 * computed by float_ms_ssim.c (CPU scalar) and by
 * integer_ms_ssim_sycl.cpp::vmaf_fex_float_ms_ssim_sycl (SYCL kernel
 * — a stack of 5 dyadic Gaussian + downsample passes culminating in
 * an exponent-weighted product). Before this test there was NO
 * cross-backend parity gate for float_ms_ssim_sycl.
 *
 * The 5-scale exponent stack is the most numerically delicate of the
 * SYCL SSIM family — a single off-by-one in the pyramid-stride
 * calculation produces a ~percent-level shift at scale 4 that
 * vanishes at scale 0, so a single-point check would catch it where
 * cross-backend ULP-by-ULP gates would not.
 *
 * Since ADR-1414 the twin is the CPU's arithmetic: the last test compares
 * every output of three frames (the score, the 15 per-scale l / c / s means
 * and both chroma scores) with the CPU extractor bit for bit.
 *
 * Skip behaviour: if vmaf_sycl_state_init() fails (no oneAPI runtime
 * or no device visible) the test emits "[skip: no SYCL device]" and
 * passes, mirroring test_sycl_motion3_parity.c.
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test.h"

#include "feature/feature_extractor.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_sycl.h"
#include "libvmaf/picture.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

/* Fixture must accommodate the 5-scale dyadic pyramid combined with
 * the 11-tap Gaussian: min dimension is `MS_SSIM_GAUSSIAN_LEN << (MS_SSIM_SCALES - 1)`
 * = 11 << 4 = 176. We use 256x192 — both dimensions ≥ 176 and the
 * 256/192 ratio still resembles a typical 16:9 content shape. */
#ifndef FIXTURE_W
#define FIXTURE_W 512u
#endif
#ifndef FIXTURE_H
#define FIXTURE_H 384u
#endif
#define FIXTURE_BPC 8u
/* MS-SSIM has more variance across the 5-scale exponent stack than
 * single-scale SSIM; ADR-0214 places=4 (1e-4) still applies. */
#define PARITY_TOL 1e-4

/* Plane `p` of the fixture: `(row * a + col * b + salt * c) & 0xFF`. */
static void fill_plane(VmafPicture *pic, unsigned p, unsigned salt, const unsigned coeff[3])
{
    uint8_t *plane = (uint8_t *)pic->data[p];
    for (unsigned row = 0; row < pic->h[p]; row++) {
        for (unsigned col = 0; col < pic->w[p]; col++) {
            plane[row * pic->stride[p] + col] =
                (uint8_t)((row * coeff[0] + col * coeff[1] + salt * coeff[2]) & 0xFFu);
        }
    }
}

static int fill_pic(VmafPicture *pic, unsigned salt)
{
    /* Distinct gradient patterns from other parity tests, and one per plane,
     * so the test exercises a different ms_ssim score region. */
    static const unsigned coeffs[3][3] = {{2u, 3u, 19u}, {3u, 5u, 7u}, {7u, 11u, 13u}};
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;
    for (unsigned p = 0; p < 3; p++) {
        fill_plane(pic, p, salt, coeffs[p]);
    }
    return 0;
}

static int feed_frame(VmafContext *vmaf, bool identical)
{
    VmafPicture ref;
    VmafPicture dist;
    int err = fill_pic(&ref, 0u);
    if (err)
        return err;
    /* An identical pair drives ms_ssim to 1.0, which is where the ADR-1221
     * dB ceiling actually binds. */
    err = fill_pic(&dist, identical ? 0u : 1u);
    if (err) {
        vmaf_picture_unref(&ref);
        return err;
    }
    int err2 = vmaf_read_pictures(vmaf, &ref, &dist, 0u);
    if (err2) {
        (void)fprintf(stderr, "vmaf_read_pictures returned %d\n", err2);
    }
    return err2;
}

/* ADR-1221 — `enable_db` / `clip_db` opt into the dB-domain score with a
 * geometry-derived ceiling. Neither is a VMAF_OPT_FLAG_FEATURE_PARAM, so the
 * collector key stays `float_ms_ssim`. */
static int ms_ssim_db_opts(VmafFeatureDictionary **opts)
{
    int err = vmaf_feature_dictionary_set(opts, "enable_db", "true");
    if (err)
        return err;
    return vmaf_feature_dictionary_set(opts, "clip_db", "true");
}

// NOLINTNEXTLINE(readability-function-size): test scaffolding (ADR-0141 / ADR-0278) — the body walks the whole allocate / fill / run-CPU / run-SYCL / compare / free sequence in one place so a parity failure points at the exact stage that diverged; splitting it hides which assertion fired.
static char *run_cpu_ms_ssim(bool db, bool identical, bool chroma, double *score, double *score_cb,
                             double *score_cr)
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    int err = vmaf_init(&vmaf, cfg);
    mu_assert("CPU: vmaf_init failed", !err);
    VmafFeatureDictionary *opts = NULL;
    if (db) {
        err = ms_ssim_db_opts(&opts);
        mu_assert("CPU: ms_ssim_db_opts failed", !err);
    }
    if (chroma) {
        err = vmaf_feature_dictionary_set(&opts, "enable_chroma", "true");
        mu_assert("CPU: ms_ssim chroma opt failed", !err);
    }
    err = vmaf_use_feature(vmaf, "float_ms_ssim", opts);
    if (err)
        (void)vmaf_feature_dictionary_free(&opts);
    mu_assert("CPU: vmaf_use_feature(float_ms_ssim) failed", !err);
    err = feed_frame(vmaf, identical);
    mu_assert("CPU: feed_frame failed", !err);
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("CPU: vmaf_read_pictures(EOS) failed", !err);
    err = vmaf_feature_score_at_index(vmaf, "float_ms_ssim", score, 0u);
    mu_assert("CPU: float_ms_ssim score missing", !err);
    if (chroma) {
        err = vmaf_feature_score_at_index(vmaf, "float_ms_ssim_cb", score_cb, 0u);
        mu_assert("CPU: float_ms_ssim_cb score missing", !err);
        err = vmaf_feature_score_at_index(vmaf, "float_ms_ssim_cr", score_cr, 0u);
        mu_assert("CPU: float_ms_ssim_cr score missing", !err);
    }
    err = vmaf_close(vmaf);
    mu_assert("CPU: vmaf_close failed", !err);
    return NULL;
}

// NOLINTNEXTLINE(readability-function-size): test scaffolding (ADR-0141 / ADR-0278) — the body walks the whole allocate / fill / run-CPU / run-SYCL / compare / free sequence in one place so a parity failure points at the exact stage that diverged; splitting it hides which assertion fired.
static char *run_sycl_ms_ssim(bool db, bool identical, bool chroma, double *score, double *score_cb,
                              double *score_cr)
{
    *score = NAN;
    *score_cb = NAN;
    *score_cr = NAN;
    VmafSyclState *sycl_state = NULL;
    VmafSyclConfiguration sycl_cfg = {.device_index = -1};
    int err = vmaf_sycl_state_init(&sycl_state, sycl_cfg);
    if (err != 0 || sycl_state == NULL) {
        (void)fprintf(stderr, "[skip: no SYCL device] ");
        return NULL;
    }
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    err = vmaf_init(&vmaf, cfg);
    mu_assert("SYCL: vmaf_init failed", !err);
    err = vmaf_sycl_import_state(vmaf, sycl_state);
    mu_assert("SYCL: vmaf_sycl_import_state failed", !err);
    VmafFeatureDictionary *opts = NULL;
    if (db) {
        err = ms_ssim_db_opts(&opts);
        mu_assert("SYCL: ms_ssim_db_opts failed", !err);
    }
    if (chroma) {
        err = vmaf_feature_dictionary_set(&opts, "enable_chroma", "true");
        mu_assert("SYCL: ms_ssim chroma opt failed", !err);
    }
    err = vmaf_use_feature(vmaf, "float_ms_ssim_sycl", opts);
    if (err)
        (void)vmaf_feature_dictionary_free(&opts);
    mu_assert("SYCL: vmaf_use_feature(float_ms_ssim_sycl) failed", !err);
    err = feed_frame(vmaf, identical);
    mu_assert("SYCL: feed_frame failed", !err);
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("SYCL: vmaf_read_pictures(EOS) failed", !err);
    err = vmaf_feature_score_at_index(vmaf, "float_ms_ssim", score, 0u);
    mu_assert("SYCL: float_ms_ssim score missing", !err);
    if (chroma) {
        err = vmaf_feature_score_at_index(vmaf, "float_ms_ssim_cb", score_cb, 0u);
        mu_assert("SYCL: float_ms_ssim_cb score missing", !err);
        err = vmaf_feature_score_at_index(vmaf, "float_ms_ssim_cr", score_cr, 0u);
        mu_assert("SYCL: float_ms_ssim_cr score missing", !err);
    }
    err = vmaf_close(vmaf);
    mu_assert("SYCL: vmaf_close failed", !err);
    vmaf_sycl_state_free(&sycl_state);
    return NULL;
}

static char *test_ms_ssim_sycl_registered(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("float_ms_ssim_sycl");
    mu_assert("float_ms_ssim_sycl extractor must be registered", fex != NULL);
    mu_assert("float_ms_ssim_sycl name matches", !strcmp(fex->name, "float_ms_ssim_sycl"));
    return NULL;
}

static char *test_ms_ssim_cpu_sycl_parity(void)
{
    double cpu_score = 0.0;
    double sycl_score = NAN;
    double cpu_cb = 0.0;
    double cpu_cr = 0.0;
    char *msg = run_cpu_ms_ssim(false, false, false, &cpu_score, &cpu_cb, &cpu_cr);
    if (msg)
        return msg;
    double sycl_cb = NAN;
    double sycl_cr = NAN;
    msg = run_sycl_ms_ssim(false, false, false, &sycl_score, &sycl_cb, &sycl_cr);
    if (msg)
        return msg;
    if (isnan(sycl_score))
        return NULL;
    double delta = fabs(cpu_score - sycl_score);
    if (delta > PARITY_TOL) {
        (void)fprintf(stderr,
                      "\nfloat_ms_ssim parity FAIL: cpu=%.8f sycl=%.8f delta=%.2e tol=%.2e\n",
                      cpu_score, sycl_score, delta, PARITY_TOL);
    }
    mu_assert("float_ms_ssim CPU vs. SYCL delta exceeds places=4 tolerance (1e-4)",
              delta <= PARITY_TOL);
    return NULL;
}

/* ADR-1221 — clip_db is a CEILING on the dB output, not a clamp on the linear
 * score. float_ms_ssim.c derives `max_db = ceil(10*log10(peak*peak/mse))` with
 * `mse = 0.5/(w*h)` and returns `MIN(-10*log10(1 - score), max_db)`,
 * short-circuiting to `max_db` when score >= 1.0. This twin used to clamp the
 * LINEAR score into [0, 1] and then convert with no ceiling, which returns
 * +Inf on an identical reference/distorted pair — an ordinary thing to score.
 * The default-options test above cannot see it: with enable_db off, neither
 * path converts at all. */

/* Y, Cb, Cr scores of one run. */
typedef struct PlaneScores {
    double y;
    double cb;
    double cr;
} PlaneScores;

/* The three planes carry different content, so their scores must differ from
 * 1.0 and from each other: a twin that scored luma three times would not. */
static char *check_planes_distinct(const PlaneScores *s)
{
    mu_assert("Cb score must be distinguishable from 1.0 (perfect)", fabs(s->cb - 1.0) > 0.01);
    mu_assert("Cr score must be distinguishable from 1.0 (perfect)", fabs(s->cr - 1.0) > 0.01);
    mu_assert("Y and Cb scores must differ", fabs(s->y - s->cb) > 0.01);
    mu_assert("Cb and Cr scores must differ", fabs(s->cb - s->cr) > 0.01);
    return NULL;
}

static char *check_planes_close(const PlaneScores *cpu, const PlaneScores *sycl)
{
    mu_assert("float_ms_ssim delta exceeds tolerance", fabs(cpu->y - sycl->y) <= PARITY_TOL);
    mu_assert("float_ms_ssim_cb delta exceeds tolerance", fabs(cpu->cb - sycl->cb) <= PARITY_TOL);
    mu_assert("float_ms_ssim_cr delta exceeds tolerance", fabs(cpu->cr - sycl->cr) <= PARITY_TOL);
    return NULL;
}

static char *test_ms_ssim_cpu_sycl_parity_chroma(void)
{
    PlaneScores cpu = {0.0, 0.0, 0.0};
    PlaneScores sycl = {NAN, NAN, NAN};

    char *msg = run_cpu_ms_ssim(false, false, true, &cpu.y, &cpu.cb, &cpu.cr);
    if (msg)
        return msg;
    msg = run_sycl_ms_ssim(false, false, true, &sycl.y, &sycl.cb, &sycl.cr);
    if (msg)
        return msg;
    if (isnan(sycl.y))
        return NULL;

    msg = check_planes_distinct(&sycl);
    return msg ? msg : check_planes_close(&cpu, &sycl);
}

static char *test_ms_ssim_clip_db_ceiling(void)
{
    double cpu = 0.0;
    double gpu = NAN;

    double dummy_cb = NAN;
    double dummy_cr = NAN;
    char *msg = run_cpu_ms_ssim(true, true, false, &cpu, &dummy_cb, &dummy_cr);
    if (msg)
        return msg;
    msg = run_sycl_ms_ssim(true, true, false, &gpu, &dummy_cb, &dummy_cr);
    if (msg)
        return msg;
    if (isnan(gpu))
        return NULL;

    mu_assert("CPU float_ms_ssim dB score is non-finite", isfinite(cpu));
    mu_assert("SYCL float_ms_ssim dB score is non-finite -- clip_db must cap it at max_db",
              isfinite(gpu));

    const double delta = fabs(cpu - gpu);
    if (delta > PARITY_TOL) {
        (void)fprintf(stderr,
                      "\nfloat_ms_ssim enable_db+clip_db parity FAIL: cpu=%.8f sycl=%.8f "
                      "delta=%.2e tol=%.2e\n",
                      cpu, gpu, delta, PARITY_TOL);
    }
    mu_assert("float_ms_ssim dB score drifts from the CPU reference", delta <= PARITY_TOL);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* ADR-1414 — float_ms_ssim_sycl is the CPU's arithmetic, bit for bit.   */
/*                                                                     */
/* The kernels reproduce ms_ssim_decimate.c (one fused multiply-add per */
/* tap), iqa_convolve() (fp32 products summed in fp64, carried as exact */
/* fp32 pairs) and ssim_accumulate_default_scalar() (fp32 denominators, */
/* l and c as pairs, s as an fp32 quotient); the frame sums are exact   */
/* int64 fixed point and the host rounds each per-scale mean to fp32    */
/* and combines as ms_ssim.c does. Before that the twin ran fp32 window */
/* sums, an fp32 l / c / s and unrounded means, and was 6.9e-8 to       */
/* 3.0e-6 from the CPU on the Netflix pair, the checkerboards and BBB   */
/* 4K.                                                                  */
/*                                                                     */
/* The CPU side runs with every SIMD flag masked: the reference is the  */
/* scalar arithmetic, and the SIMD paths' agreement with it has its own */
/* tests (test_ssim_x86_simd, test_feature_isa_invariance). The SSIM    */
/* SIMD dispatch is installed once per process, by the first context    */
/* that initialises the extractor, so this test must run before any     */
/* other CPU run of this binary; run_tests() keeps it first.            */
/* ------------------------------------------------------------------ */
#define MS_EXACT_FRAMES 3u
#define MS_EXACT_KEYS 18u
#define MS_EXACT_SCALAR_CPUMASK UINT64_MAX

typedef struct MsExactScores {
    double v[MS_EXACT_FRAMES][MS_EXACT_KEYS];
} MsExactScores;

static const char *const ms_exact_keys[MS_EXACT_KEYS] = {
    "float_ms_ssim",          "float_ms_ssim_l_scale0", "float_ms_ssim_l_scale1",
    "float_ms_ssim_l_scale2", "float_ms_ssim_l_scale3", "float_ms_ssim_l_scale4",
    "float_ms_ssim_c_scale0", "float_ms_ssim_c_scale1", "float_ms_ssim_c_scale2",
    "float_ms_ssim_c_scale3", "float_ms_ssim_c_scale4", "float_ms_ssim_s_scale0",
    "float_ms_ssim_s_scale1", "float_ms_ssim_s_scale2", "float_ms_ssim_s_scale3",
    "float_ms_ssim_s_scale4", "float_ms_ssim_cb",       "float_ms_ssim_cr",
};

/* Three different picture pairs, then the flush. */
static int ms_exact_feed(VmafContext *vmaf)
{
    for (unsigned i = 0; i < MS_EXACT_FRAMES; i++) {
        VmafPicture ref;
        VmafPicture dist;
        int err = fill_pic(&ref, 2u * i);
        if (err)
            return err;
        err = fill_pic(&dist, 2u * i + 1u);
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

static int ms_exact_collect(VmafContext *vmaf, MsExactScores *out)
{
    for (unsigned k = 0; k < MS_EXACT_KEYS; k++) {
        for (unsigned i = 0; i < MS_EXACT_FRAMES; i++) {
            const int err = vmaf_feature_score_at_index(vmaf, ms_exact_keys[k], &out->v[i][k], i);
            if (err)
                return err;
        }
    }
    return 0;
}

static int ms_exact_opts(VmafFeatureDictionary **opts)
{
    int err = vmaf_feature_dictionary_set(opts, "enable_lcs", "true");
    if (err)
        return err;
    return vmaf_feature_dictionary_set(opts, "enable_chroma", "true");
}

/* Every frame's 18 outputs from one extractor; `sycl_state` NULL runs the
 * scalar CPU extractor. */
static int ms_exact_score(VmafSyclState *sycl_state, MsExactScores *out)
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE,
                             .cpumask = sycl_state ? 0u : MS_EXACT_SCALAR_CPUMASK};
    VmafContext *vmaf = NULL;
    int err = vmaf_init(&vmaf, cfg);
    if (err)
        return err;
    if (sycl_state)
        err = vmaf_sycl_import_state(vmaf, sycl_state);
    VmafFeatureDictionary *opts = NULL;
    if (!err)
        err = ms_exact_opts(&opts);
    if (!err) {
        err = vmaf_use_feature(vmaf, sycl_state ? "float_ms_ssim_sycl" : "float_ms_ssim", opts);
        opts = err ? opts : NULL; /* taken on success */
    }
    if (opts)
        (void)vmaf_feature_dictionary_free(&opts);
    if (!err)
        err = ms_exact_feed(vmaf);
    if (!err)
        err = ms_exact_collect(vmaf, out);
    const int closed = vmaf_close(vmaf);
    return err ? err : closed;
}

static uint64_t ms_exact_bits(double v)
{
    uint64_t bits = 0u;
    memcpy(&bits, &v, sizeof(bits));
    return bits;
}

/* The outputs that are not the CPU's bit for bit, each one reported. */
static unsigned ms_exact_mismatches(const MsExactScores *cpu, const MsExactScores *gpu)
{
    unsigned differing = 0u;
    for (unsigned i = 0; i < MS_EXACT_FRAMES; i++) {
        for (unsigned k = 0; k < MS_EXACT_KEYS; k++) {
            if (ms_exact_bits(cpu->v[i][k]) == ms_exact_bits(gpu->v[i][k]))
                continue;
            differing++;
            (void)fprintf(stderr, "\n%s frame %u: cpu=%.17g sycl=%.17g delta=%.3e",
                          ms_exact_keys[k], i, cpu->v[i][k], gpu->v[i][k],
                          fabs(cpu->v[i][k] - gpu->v[i][k]));
        }
    }
    return differing;
}

static char *test_ms_ssim_matches_cpu_bit_for_bit(void)
{
    VmafSyclState *sycl_state = NULL;
    VmafSyclConfiguration sycl_cfg = {.device_index = -1};
    if (vmaf_sycl_state_init(&sycl_state, sycl_cfg) != 0 || !sycl_state) {
        (void)fprintf(stderr, "[skip: no SYCL device] ");
        return NULL;
    }
    MsExactScores cpu;
    MsExactScores gpu;
    const int cpu_err = ms_exact_score(NULL, &cpu);
    const int gpu_err = ms_exact_score(sycl_state, &gpu);
    vmaf_sycl_state_free(&sycl_state);
    mu_assert("CPU float_ms_ssim (enable_lcs, enable_chroma) run failed", !cpu_err);
    mu_assert("SYCL float_ms_ssim (enable_lcs, enable_chroma) run failed", !gpu_err);
    /* A fixture that scored 1.0 everywhere would compare nothing. */
    mu_assert("the fixture pair is identical", cpu.v[0][0] < 0.999);

    const unsigned differing = ms_exact_mismatches(&cpu, &gpu);
    if (differing) {
        (void)fprintf(stderr, "\n%u of %u outputs differ\n", differing,
                      MS_EXACT_FRAMES * MS_EXACT_KEYS);
    }
    mu_assert("float_ms_ssim_sycl does not return the CPU extractor's values bit for bit",
              differing == 0u);
    return NULL;
}

char *run_tests(void)
{
    /* First: its scalar CPU reference needs the process-wide SSIM dispatch
     * still uninstalled (see the comment above the test). */
    mu_run_test(test_ms_ssim_matches_cpu_bit_for_bit);
    mu_run_test(test_ms_ssim_sycl_registered);
    mu_run_test(test_ms_ssim_cpu_sycl_parity);
    mu_run_test(test_ms_ssim_cpu_sycl_parity_chroma);
    mu_run_test(test_ms_ssim_clip_db_ceiling);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
