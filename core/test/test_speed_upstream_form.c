/**
 *  Copyright 2016-2025 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  ADR-1477: SpEED evaluates Netflix's statements.
 *
 *  The fork's port of the SpEED extractors (#213) had turned three places of
 *  Netflix's speed.c from fp64 into fp32: the Givens rotation's
 *  `1.0 / sqrt(1 + t * t)`, the `log2()` of update_entropy() and the `log2()`
 *  weights of get_speed_score(). Against Netflix master the CPU extractors
 *  then differed on most frames (up to 2.3e-5 for speed_chroma, 6.6e-4 for
 *  speed_temporal). The tests below pin Netflix's form, with no tolerance.
 *
 *  On every C library:
 *    1. speed.c's create_givens(), update_entropy() and get_speed_score()
 *       return, bit for bit, what Netflix's statements return for the same
 *       inputs. Netflix's statements are transcribed below with their file
 *       and line and evaluated here with the host's own sqrt() and log2(),
 *       so the comparison holds whatever those return. speed.c keeps the
 *       three functions static; speed_internal_cpu_create_givens(),
 *       speed_internal_cpu_update_entropy() and
 *       speed_internal_cpu_speed_score() (speed_internal.h) call them
 *       unchanged;
 *    2. speed_givens_unit() (feature/speed_givens.h), the fp32-only routine
 *       the GPU twins use for the rotation, returns Netflix's float on every
 *       input the rotation can produce: all 2^23 + 1 floats of [1, 2]. It
 *       needs a correctly rounded sqrt() and fmaf(), which IEEE 754 and the C
 *       standard require of every C library, and no log2();
 *    3. speed_internal_gpu_tail_scores(), the host tail the GPU twins form
 *       their entropies and score with, returns what Netflix's
 *       update_entropy() and get_speed_score() return for the same
 *       eigenvalues and variances, in every weighting mode, again with the
 *       host's log2() on both sides.
 *
 *  On glibc only:
 *    4. the CPU extractors return, on the tracked 576x324 pair, the values a
 *       build of Netflix/vmaf returns for the same frames (cea2b4d8, whose
 *       SpEED sources are those of master 9e48141b). Those values are what
 *       Netflix's speed.c computes with glibc's log2(): measured with glibc
 *       2.44 on x86-64, and the fork's aarch64 build returns the same bits
 *       under qemu-aarch64 with glibc 2.44. glibc 2.39, 2.41, 2.43 and 2.44
 *       return the same bits for each of the 180,565 log2() arguments this
 *       test evaluates. Another C library's log2() (the MSVC runtime's,
 *       Apple's, musl's) may return another last bit of a double, and the
 *       float rounded from it can then differ as well, so elsewhere the test
 *       reports this check as skipped and says why. The meson variant
 *       test_speed_upstream_form_foreign_libm defines
 *       VMAF_TEST_ASSUME_FOREIGN_LIBM and runs that path on glibc too.
 */

#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "float_bits.h"
#include "test.h"

#include "dict.h"
#include "feature/feature_collector.h"
#include "feature/feature_extractor.h"
#include "feature/feature_name.h"
#include "feature/speed_givens.h"
#include "feature/speed_internal.h"
#include "libvmaf/picture.h"
#include "compat/path_utf8.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. ADR-1138. */

/* Test 4 holds where Netflix's values were measured: glibc. Elsewhere this
 * is the reason it is skipped. */
#if defined(__GLIBC__) && !defined(VMAF_TEST_ASSUME_FOREIGN_LIBM)
#define UF_NETFLIX_VALUES_SKIP_REASON NULL
#elif defined(VMAF_TEST_ASSUME_FOREIGN_LIBM)
#define UF_NETFLIX_VALUES_SKIP_REASON                                                              \
    "VMAF_TEST_ASSUME_FOREIGN_LIBM is defined: built as for a C library other than glibc, "        \
    "whose log2() Netflix's values were not measured with"
#else
#define UF_NETFLIX_VALUES_SKIP_REASON                                                              \
    "this C library is not glibc; Netflix's values were measured with glibc 2.44's log2()"
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_E
#define M_E 2.71828182845904523536
#endif

#ifndef SPEED_TESTDATA_DIR
#define SPEED_TESTDATA_DIR "testdata"
#endif

#define UF_PAIR_W 576u
#define UF_PAIR_H 324u
#define UF_FRAMES 11u

/* ------------------------------------------------------------------ */
/* 4. The CPU extractors against Netflix master (glibc).               */
/* ------------------------------------------------------------------ */

typedef struct UfCase {
    const char *extractor;
    const char *feature;    /* collector key with the default options */
    const char *options[8]; /* "key", "value" pairs; NULL ends the list */
    double expected[UF_FRAMES];
} UfCase;

/* Frames 0..10 of testdata/{ref,dis}_576x324_48f.yuv, from a GCC release
 * build of Netflix/vmaf cea2b4d8 (its `libvmaf/src/feature/speed.c` and
 * `vif_tools.c` are master 9e48141b's) linked against glibc 2.44 on x86-64,
 * read through the C API at %.17g. Frames 0..2 score 0 in every case: the
 * pair's first frames are identical. */
static const UfCase uf_cases[] = {
    {"speed_chroma",
     "Speed_chroma_feature_speed_chroma_u_score",
     {NULL},
     {0, 0, 0, 2.867142915725708, 3.0421664714813232, 1.5541658401489258, 1.1829468011856079,
      1.9895018339157104, 1.9895018339157104, 3.4691162109375, 3.1736118793487549}},
    {"speed_chroma",
     "Speed_chroma_feature_speed_chroma_v_score",
     {NULL},
     {0, 0, 0, 1.2760382890701294, 2.4710533618927002, 1.7423654794692993, 1.5859894752502441,
      1.2649073600769043, 1.253635048866272, 1.2539100646972656, 1.666878342628479}},
    {"speed_chroma",
     "Speed_chroma_feature_speed_chroma_uv_score",
     {NULL},
     {0, 0, 0, 2.0715906620025635, 2.7566099166870117, 1.6482656002044678, 1.3844680786132812,
      1.6272046566009521, 1.6215684413909912, 2.3615131378173828, 2.4202451705932617}},
    /* The options vmaf_v1.0.16 gives speed_chroma: weighting mode 5, whose
     * log2() argument is an fp64 sum. */
    {"speed_chroma",
     "Speed_chroma_feature_speed_chroma_uv_score",
     {"speed_nn_floor", "0.1", "speed_sigma_nn", "0.19", "speed_weight_var_mode", "5",
      "speed_max_val", "45.0"},
     {0, 0, 0, 0.98447263240814209, 1.1037528514862061, 0.91833144426345825, 0.77587556838989258,
      0.48319655656814575, 0.48088476061820984, 0.95609354972839355, 0.89928412437438965}},
    /* Weighting mode 3: the fp64 halving `(ref + dis) / 2.0`. */
    {"speed_chroma",
     "Speed_chroma_feature_speed_chroma_u_score",
     {"speed_weight_var_mode", "3"},
     {0, 0, 0, 0.50976079702377319, 1.0089049339294434, 1.1963173151016235, 1.0242036581039429,
      0.55706566572189331, 0.55706566572189331, 1.1012519598007202, 0.96472018957138062}},
    {"speed_temporal",
     "Speed_temporal_feature_speed_temporal_score",
     {NULL},
     {0, 0, 0, 0.85538017749786377, 1.1709119081497192, 1.9228254556655884, 1.4098173379898071,
      1.5475591421127319, 0, 0.37112537026405334, 2.123035192489624}},
};

/* Frame `index` of an 8-bit 4:2:0 raw file of the testdata pair. */
static int uf_read(FILE *f, unsigned index, VmafPicture *pic)
{
    const size_t frame_bytes = (size_t)UF_PAIR_W * UF_PAIR_H * 3u / 2u;
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, 8u, UF_PAIR_W, UF_PAIR_H);
    if (err)
        return err;
    if (fseek(f, (long)(frame_bytes * index), SEEK_SET) != 0)
        err = -EIO;
    for (unsigned plane = 0u; plane < 3u && !err; plane++) {
        for (unsigned row = 0u; row < pic->h[plane] && !err; row++) {
            uint8_t *dst = (uint8_t *)pic->data[plane] + row * pic->stride[plane];
            err = fread(dst, 1u, pic->w[plane], f) == pic->w[plane] ? 0 : -EIO;
        }
    }
    if (err)
        (void)vmaf_picture_unref(pic);
    return err;
}

static int uf_extract(VmafFeatureExtractorContext *ctx, VmafFeatureCollector *fc, FILE *ref_file,
                      FILE *dis_file, unsigned index)
{
    VmafPicture ref;
    VmafPicture dis;
    int err = uf_read(ref_file, index, &ref);
    if (err)
        return err;
    err = uf_read(dis_file, index, &dis);
    if (err) {
        (void)vmaf_picture_unref(&ref);
        return err;
    }
    err = vmaf_feature_extractor_context_extract(ctx, &ref, NULL, &dis, NULL, index, fc);
    const int err_ref = vmaf_picture_unref(&ref);
    const int err_dis = vmaf_picture_unref(&dis);
    if (err)
        return err;
    return err_ref ? err_ref : err_dis;
}

static int uf_context(const UfCase *c, VmafFeatureExtractorContext **ctx)
{
    *ctx = NULL;
    const VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name(c->extractor);
    if (!fex)
        return -EINVAL;
    VmafDictionary *opts = NULL;
    int err = 0;
    for (unsigned i = 0u; i + 1u < 8u && c->options[i] && !err; i += 2u)
        err = vmaf_dictionary_set(&opts, c->options[i], c->options[i + 1u], 0);
    if (!err)
        err = vmaf_feature_extractor_context_create(ctx, fex, opts);
    if (err && opts)
        (void)vmaf_dictionary_free(&opts);
    return err;
}

/* Run the case's extractor over the first UF_FRAMES frames of the pair. */
static int uf_scores(const UfCase *c, double scores[UF_FRAMES])
{
    FILE *ref_file = vmaf_fopen_utf8(SPEED_TESTDATA_DIR "/ref_576x324_48f.yuv", "rb");
    FILE *dis_file = vmaf_fopen_utf8(SPEED_TESTDATA_DIR "/dis_576x324_48f.yuv", "rb");
    VmafFeatureExtractorContext *ctx = NULL;
    VmafFeatureCollector *fc = NULL;
    int err = (ref_file && dis_file) ? uf_context(c, &ctx) : -ENOENT;
    if (!err)
        err = vmaf_feature_collector_init(&fc);
    for (unsigned i = 0u; i < UF_FRAMES && !err; i++)
        err = uf_extract(ctx, fc, ref_file, dis_file, i);
    char *name = (err || !ctx) ?
                     NULL :
                     vmaf_feature_name_from_options(c->feature, ctx->fex->options, ctx->fex->priv);
    if (!err && !name)
        err = -ENOMEM;
    for (unsigned i = 0u; i < UF_FRAMES && !err; i++)
        err = vmaf_feature_collector_get_score(fc, name, &scores[i], i);
    free(name);
    if (ctx) {
        (void)vmaf_feature_extractor_context_close(ctx);
        (void)vmaf_feature_extractor_context_destroy(ctx);
    }
    if (fc)
        vmaf_feature_collector_destroy(fc);
    if (ref_file)
        (void)fclose(ref_file);
    if (dis_file)
        (void)fclose(dis_file);
    return err;
}

static unsigned uf_mismatches(const UfCase *c, const double scores[UF_FRAMES])
{
    unsigned mismatches = 0u;
    for (unsigned i = 0u; i < UF_FRAMES; i++) {
        if (!vmaf_test_identical_f64(scores[i], c->expected[i])) {
            (void)fprintf(stderr, "\n%s frame %u: %.17g, Netflix master %.17g\n", c->feature, i,
                          scores[i], c->expected[i]);
            mismatches++;
        }
    }
    return mismatches;
}

static char *test_cpu_extractors_return_netflix_masters_values(void)
{
    unsigned mismatches = 0u;
    for (size_t k = 0u; k < sizeof(uf_cases) / sizeof(uf_cases[0]); k++) {
        double scores[UF_FRAMES] = {0.0};
        mu_assert("running a CPU SpEED extractor on the testdata pair failed",
                  uf_scores(&uf_cases[k], scores) == 0);
        mismatches += uf_mismatches(&uf_cases[k], scores);
    }
    mu_assert("a CPU SpEED score is not Netflix master's", mismatches == 0u);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Netflix's statements, and the fp32 forms port #213 had.             */
/* ------------------------------------------------------------------ */

/* The fields of Netflix's SpeedDimensions and SpeedResultBuffers
 * (libvmaf/src/feature/speed.c at 9e48141b) the statements below read, under
 * Netflix's names so the statements read as Netflix's do. */
typedef struct SpeedDimensions {
    size_t num_blocks_horizontal;
    size_t num_blocks_vertical;
    size_t num_blocks;
    size_t elements_in_block;
} SpeedDimensions;

typedef struct SpeedResultBuffers {
    float *entropies;
    float *variances;
} SpeedResultBuffers;

#define UF_BLOCKS 37u
#define UF_ELEMENTS 25u

static float uf_float(uint32_t bits)
{
    float value = 0.0f;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

// NOLINTBEGIN(performance-type-promotion-in-math-fn) ADR-1477: upstream's double form.

/* Netflix/vmaf libvmaf/src/feature/speed.c:411-427 (9e48141b). */
static void uf_upstream_create_givens(const float a, const float b, float *c, float *s)
{
    if (b == 0) {
        *c = 1;
        *s = 0;
    } else if (fabsf(b) > fabsf(a)) {
        float t = -a / b;
        float s1 = (float)(1.0 / sqrt(1 + t * t));
        *s = s1;
        *c = s1 * t;
    } else {
        float t = -b / a;
        float c1 = (float)(1.0 / sqrt(1 + t * t));
        *c = c1;
        *s = c1 * t;
    }
}

/* The statement of speed.c:418 and :423 above, with the float `1 + t * t`
 * passed in as `u`. */
static float uf_upstream_givens_unit(float u)
{
    float s1 = (float)(1.0 / sqrt(u));
    return s1;
}

/* Netflix/vmaf libvmaf/src/feature/speed.c:796-806 (9e48141b). */
static void uf_upstream_update_entropy(SpeedDimensions dim, float *entropy, const float *S, float L,
                                       float sigma_nn)
{
    for (size_t i = 0; i < dim.num_blocks_vertical; i++) {
        for (size_t j = 0; j < dim.num_blocks_horizontal; j++) {
            entropy[i * dim.num_blocks_horizontal + j] =
                (float)(entropy[i * dim.num_blocks_horizontal + j] +
                        (log2(L * S[i * dim.num_blocks_horizontal + j] + sigma_nn) +
                         log2(2 * M_PI * M_E)));
        }
    }
}

/* Netflix/vmaf libvmaf/src/feature/speed.c:906-927 (9e48141b): the two
 * weighted entropies of get_speed_score() for one block, every mode but the
 * last branch's -EINVAL, which uf_upstream_speed_score() keeps. */
static void uf_upstream_spatial(SpeedResultBuffers ref_results, SpeedResultBuffers dis_results,
                                size_t i, int speed_weight_var_mode, float *ref, float *dis)
{
    float spatial_ref = 0.0;
    float spatial_dis = 0.0;
    if (speed_weight_var_mode == 0) {
        spatial_ref = (float)(ref_results.entropies[i] * log2(1 + ref_results.variances[i]));
        spatial_dis = (float)(dis_results.entropies[i] * log2(1 + dis_results.variances[i]));
    } else if (speed_weight_var_mode == 1) {
        spatial_ref = (float)(ref_results.entropies[i] * log2(1 + ref_results.variances[i]));
        spatial_dis = (float)(dis_results.entropies[i] * log2(1 + ref_results.variances[i]));
    } else if (speed_weight_var_mode == 2) {
        spatial_ref = (float)(ref_results.entropies[i] * log2(1 + dis_results.variances[i]));
        spatial_dis = (float)(dis_results.entropies[i] * log2(1 + dis_results.variances[i]));
    } else if (speed_weight_var_mode == 3) {
        spatial_ref =
            (float)(ref_results.entropies[i] *
                    log2(1 + (ref_results.variances[i] + dis_results.variances[i]) / 2.0));
        spatial_dis =
            (float)(dis_results.entropies[i] *
                    log2(1 + (ref_results.variances[i] + dis_results.variances[i]) / 2.0));
    } else if (speed_weight_var_mode == 4) {
        spatial_ref = (float)(ref_results.entropies[i] * log2(1 + ref_results.variances[i]));
        spatial_dis =
            (float)(dis_results.entropies[i] *
                    log2(1 + (ref_results.variances[i] + dis_results.variances[i]) / 2.0));
    } else if (speed_weight_var_mode == 5) {
        spatial_ref = (float)(ref_results.entropies[i] * log2(1 + ref_results.variances[i]));
        spatial_dis =
            (float)(dis_results.entropies[i] *
                    log2(1 + (0.75 * ref_results.variances[i] + 0.25 * dis_results.variances[i])));
    } else {
        spatial_ref = (float)(ref_results.entropies[i] * log2(1 + ref_results.variances[i]));
        spatial_dis =
            (float)(dis_results.entropies[i] *
                    log2(1 + (0.25 * ref_results.variances[i] + 0.75 * dis_results.variances[i])));
    }
    *ref = spatial_ref;
    *dis = spatial_dis;
}

/* Netflix/vmaf libvmaf/src/feature/speed.c:892-938 (9e48141b), with the
 * per-block branch of :906-927 in uf_upstream_spatial(). */
static float uf_upstream_speed_score(SpeedDimensions dim, SpeedResultBuffers ref_results,
                                     SpeedResultBuffers dis_results, float sigma_nn, float nn_floor,
                                     int speed_weight_var_mode)
{
    float score = 0;
    float base_entropy =
        (float)(dim.elements_in_block *
                (log2((double)((1 + nn_floor) * sigma_nn)) + log2(2 * M_PI * M_E)));
    for (size_t i = 0; i < dim.num_blocks; i++) {
        if ((ref_results.entropies[i] < base_entropy) &&
            (dis_results.entropies[i] < base_entropy)) {
            score += 0;
        } else {
            if (speed_weight_var_mode < 0 || speed_weight_var_mode > 6)
                return -EINVAL;
            float spatial_ref = 0.0;
            float spatial_dis = 0.0;
            uf_upstream_spatial(ref_results, dis_results, i, speed_weight_var_mode, &spatial_ref,
                                &spatial_dis);
            score += fabsf(spatial_ref - spatial_dis);
        }
    }
    return score / dim.num_blocks;
}

// NOLINTEND(performance-type-promotion-in-math-fn)

/* The rotation of port #213 (32f275788): `1.0f / sqrtf(1 + t * t)`. */
static void uf_port213_create_givens(const float a, const float b, float *c, float *s)
{
    if (b == 0) {
        *c = 1;
        *s = 0;
    } else if (fabsf(b) > fabsf(a)) {
        const float t = -a / b;
        const float s1 = 1.0f / sqrtf(1 + t * t);
        *s = s1;
        *c = s1 * t;
    } else {
        const float t = -b / a;
        const float c1 = 1.0f / sqrtf(1 + t * t);
        *c = c1;
        *s = c1 * t;
    }
}

/* A dimension record of `rows` x `cols` blocks of 5x5 elements. */
static SpeedDimensions uf_dims(size_t rows, size_t cols)
{
    SpeedDimensions dim;
    memset(&dim, 0, sizeof(dim));
    dim.num_blocks_vertical = rows;
    dim.num_blocks_horizontal = cols;
    dim.num_blocks = rows * cols;
    dim.elements_in_block = UF_ELEMENTS;
    return dim;
}

/* sigma_nn and nn_floor: the extractors' defaults and vmaf_v1.0.16's. */
static const float uf_scoring[2][2] = {{0.29f, 0.0f}, {0.19f, 0.1f}};

static uint32_t uf_lcg(uint32_t *state)
{
    *state = *state * 1664525u + 1013904223u;
    return *state >> 8u;
}

/* ------------------------------------------------------------------ */
/* 1. speed.c's three statements against Netflix's.                    */
/* ------------------------------------------------------------------ */

/* A float of either sign with a magnitude between 2^-24 and 2^25. */
static float uf_operand(uint32_t *state)
{
    const uint32_t mantissa = uf_lcg(state) & 0x007fffffu;
    const uint32_t exponent = 103u + uf_lcg(state) % 50u;
    const uint32_t sign = (uf_lcg(state) & 1u) << 31u;
    return uf_float(sign | (exponent << 23u) | mantissa);
}

/* positive and boundary: both branches, b == 0, a == 0, |a| == |b|, and
 * 200,000 operand pairs over 49 binades. */
static char *test_cpu_create_givens_is_upstreams(void)
{
    static const float edges[][2] = {{1.0f, 0.0f},  {0.0f, 1.0f},    {0.0f, -3.5f}, {2.0f, 2.0f},
                                     {2.0f, -2.0f}, {-1e-30f, 1.0f}, {1e30f, 1.0f}, {3.0f, 4.0f}};
    unsigned mismatches = 0u;
    unsigned port_differs = 0u;
    uint32_t state = 11u;
    const unsigned pairs = 200000u + (unsigned)(sizeof(edges) / sizeof(edges[0]));
    for (unsigned k = 0u; k < pairs; k++) {
        const int edge = k < sizeof(edges) / sizeof(edges[0]);
        const float a = edge ? edges[k][0] : uf_operand(&state);
        const float b = edge ? edges[k][1] : uf_operand(&state);
        float fork[2] = {0.0f, 0.0f};
        float up[2] = {0.0f, 0.0f};
        float port[2] = {0.0f, 0.0f};
        speed_internal_cpu_create_givens(a, b, &fork[0], &fork[1]);
        uf_upstream_create_givens(a, b, &up[0], &up[1]);
        uf_port213_create_givens(a, b, &port[0], &port[1]);
        mismatches += vmaf_test_bits_f32(fork[0]) != vmaf_test_bits_f32(up[0]) ||
                      vmaf_test_bits_f32(fork[1]) != vmaf_test_bits_f32(up[1]);
        port_differs += vmaf_test_bits_f32(port[0]) != vmaf_test_bits_f32(up[0]) ||
                        vmaf_test_bits_f32(port[1]) != vmaf_test_bits_f32(up[1]);
    }
    mu_assert("speed.c's create_givens() is not upstream's", mismatches == 0u);
    mu_assert("the operands cannot tell upstream's rotation from port #213's", port_differs != 0u);
    return NULL;
}

/* Eigenvalues from -2 to about 4000 (a few negative, which est_params()
 * clamps to 0) and variances from 0 to about 0.03. */
static void uf_fill_statistics(float *eig, size_t n_eig, float *var, size_t n_var, uint32_t seed)
{
    uint32_t state = seed;
    for (size_t i = 0u; i < n_eig; i++)
        eig[i] = (float)uf_lcg(&state) * 0x1p-12f - 2.0f;
    for (size_t i = 0u; i < n_var; i++)
        var[i] = (float)uf_lcg(&state) * 0x1p-29f;
}

/* est_params() step 9 with speed.c's update_entropy() and with upstream's,
 * on a 5 x 7 block grid, both default sigma_nn values. */
static char *test_cpu_update_entropy_is_upstreams(void)
{
    const SpeedDimensions dim = uf_dims(5u, 7u);
    unsigned mismatches = 0u;
    for (uint32_t seed = 1u; seed <= 8u; seed++) {
        float eig[UF_ELEMENTS];
        float var[35];
        float fork[35] = {0.0f};
        float upstream[35] = {0.0f};
        uf_fill_statistics(eig, UF_ELEMENTS, var, 35u, seed);
        const float sigma_nn = seed % 2u ? 0.29f : 0.19f;
        for (size_t k = 0u; k < UF_ELEMENTS; k++) {
            const float L = eig[k] < 0 ? 0 : eig[k];
            speed_internal_cpu_update_entropy(5u, 7u, fork, var, L, sigma_nn);
            uf_upstream_update_entropy(dim, upstream, var, L, sigma_nn);
        }
        for (size_t i = 0u; i < 35u; i++)
            mismatches += vmaf_test_bits_f32(fork[i]) != vmaf_test_bits_f32(upstream[i]);
    }
    mu_assert("speed.c's update_entropy() is not upstream's", mismatches == 0u);
    return NULL;
}

/* One side's entropies from Netflix's update_entropy(), and its variances. */
static void uf_side(SpeedDimensions dim, uint32_t seed, float sigma_nn, float *entropies,
                    float *variances)
{
    float eig[UF_ELEMENTS];
    uf_fill_statistics(eig, UF_ELEMENTS, variances, dim.num_blocks, seed);
    for (size_t i = 0u; i < dim.num_blocks; i += 4u)
        variances[i] = 0.0f; /* below the entropy floor on both sides */
    memset(entropies, 0, dim.num_blocks * sizeof(float));
    for (size_t k = 0u; k < UF_ELEMENTS; k++)
        uf_upstream_update_entropy(dim, entropies, variances, eig[k] < 0 ? 0 : eig[k], sigma_nn);
}

/* positive and negative: get_speed_score() of speed.c and upstream's, every
 * weighting mode 0..6, the default scoring options and vmaf_v1.0.16's, and
 * mode 7, where both return -EINVAL as a float. */
static char *test_cpu_speed_score_is_upstreams(void)
{
    const SpeedDimensions dim = uf_dims(1u, UF_BLOCKS);
    unsigned mismatches = 0u;
    unsigned nonzero = 0u;
    for (uint32_t o = 0u; o < 2u; o++) {
        float ref_e[UF_BLOCKS];
        float ref_v[UF_BLOCKS];
        float dis_e[UF_BLOCKS];
        float dis_v[UF_BLOCKS];
        uf_side(dim, 3u + o, uf_scoring[o][0], ref_e, ref_v);
        uf_side(dim, 5u + o, uf_scoring[o][0], dis_e, dis_v);
        const SpeedResultBuffers ref = {ref_e, ref_v};
        const SpeedResultBuffers dis = {dis_e, dis_v};
        for (int mode = 0; mode <= 7; mode++) {
            const SpeedInternalScoreSide fork_ref = {ref_e, ref_v};
            const SpeedInternalScoreSide fork_dis = {dis_e, dis_v};
            const float fork =
                speed_internal_cpu_speed_score(dim.num_blocks, dim.elements_in_block, fork_ref,
                                               fork_dis, uf_scoring[o][0], uf_scoring[o][1], mode);
            const float up =
                uf_upstream_speed_score(dim, ref, dis, uf_scoring[o][0], uf_scoring[o][1], mode);
            mismatches += vmaf_test_bits_f32(fork) != vmaf_test_bits_f32(up);
            nonzero += up > 0.0f;
        }
    }
    mu_assert("speed.c's get_speed_score() is not upstream's", mismatches == 0u);
    mu_assert("a synthetic score is 0: the fixture scores nothing", nonzero == 14u);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* 2. The Givens rotation's fp64 statement in fp32.                    */
/* ------------------------------------------------------------------ */

/* create_givens() divides the smaller magnitude by the larger, so |t| <= 1
 * and u = 1 + t * t is one of the 2^23 + 1 floats of [1, 2]. */
static char *test_givens_unit_is_upstreams_on_every_input(void)
{
    unsigned mismatches = 0u;
    unsigned fp32_form_differs = 0u;
    for (uint32_t bits = 0x3f800000u; bits <= 0x40000000u; bits++) {
        const float u = uf_float(bits);
        const float upstream = uf_upstream_givens_unit(u);
        mismatches += vmaf_test_bits_f32(speed_givens_unit(u)) != vmaf_test_bits_f32(upstream);
        fp32_form_differs += vmaf_test_bits_f32(1.0f / sqrtf(u)) != vmaf_test_bits_f32(upstream);
    }
    mu_assert("speed_givens_unit() is not upstream's 1.0 / sqrt(1 + t * t)", mismatches == 0u);
    /* 2,907,055 inputs when measured: the form port #213 had is another
     * function, so this test tells the two apart. */
    mu_assert("1.0f / sqrtf(u) equals upstream's form on every input", fp32_form_differs != 0u);
    mu_assert("a NaN operand must stay a NaN", isnan(speed_givens_unit(NAN)));
    return NULL;
}

/* ------------------------------------------------------------------ */
/* 3. The host tail of the GPU twins against upstream's statements.    */
/* ------------------------------------------------------------------ */

/* A tail block of 4 channels and its parts (SpeedGpuTailLayout). */
typedef struct UfTail {
    uint32_t words[4u * 2u + 4u * UF_ELEMENTS + 4u * UF_BLOCKS];
    int32_t *status;
    float *eig;
    float *var;
} UfTail;

/* The part of a tail block that starts at byte `offset`. */
static void *uf_tail_at(uint32_t *words, uint32_t offset)
{
    // SAFETY: the offsets are speed_gpu_tail_layout()'s for a block of at most
    // the four channels and UF_BLOCKS blocks UfTail holds, and each is a
    // multiple of four in an array of 4-byte words.
    return (unsigned char *)words + offset;
}

/* Eigenvalues from -2 to about 4000 (a few negative, which est_params()
 * clamps to 0) and variances from 0 to about 0.03; every fourth block of
 * channel 0 and 1 has variance 0, which leaves both entropies below the
 * entropy floor. */
static void uf_fill_tail(UfTail *t, uint32_t channels, uint32_t seed)
{
    memset(t, 0, sizeof(*t));
    const SpeedGpuTailLayout layout = speed_gpu_tail_layout(channels, UF_BLOCKS);
    t->status = uf_tail_at(t->words, layout.status);
    t->eig = uf_tail_at(t->words, layout.eig);
    t->var = uf_tail_at(t->words, layout.var);
    uint32_t state = seed;
    for (uint32_t i = 0u; i < channels * UF_ELEMENTS; i++)
        t->eig[i] = (float)uf_lcg(&state) * 0x1p-12f - 2.0f;
    for (uint32_t i = 0u; i < channels * UF_BLOCKS; i++) {
        const int flat = i < 2u * UF_BLOCKS && (i % UF_BLOCKS) % 4u == 0u;
        t->var[i] = flat ? 0.0f : (float)uf_lcg(&state) * 0x1p-29f;
    }
}

/* est_params() steps 8 and 9 with upstream's update_entropy(), then
 * upstream's get_speed_score(), for score pair `pair` of the tail. */
static float uf_upstream_pair_score(UfTail *t, uint32_t pair, const SpeedGpuScoring *s)
{
    const SpeedDimensions dim = uf_dims(1u, UF_BLOCKS);
    float entropies[2][UF_BLOCKS];
    for (uint32_t side = 0u; side < 2u; side++) {
        const size_t ch = (size_t)2u * pair + side;
        memset(entropies[side], 0, sizeof(entropies[side]));
        for (uint32_t k = 0u; k < UF_ELEMENTS; k++) {
            const float eigenvalue = t->eig[ch * UF_ELEMENTS + k];
            float L = eigenvalue < 0 ? 0 : eigenvalue;
            uf_upstream_update_entropy(dim, entropies[side], t->var + ch * UF_BLOCKS, L,
                                       s->sigma_nn);
        }
    }
    const SpeedResultBuffers ref_results = {entropies[0], t->var + (size_t)2u * pair * UF_BLOCKS};
    const SpeedResultBuffers dis_results = {entropies[1],
                                            t->var + ((size_t)2u * pair + 1u) * UF_BLOCKS};
    return uf_upstream_speed_score(dim, ref_results, dis_results, s->sigma_nn, s->nn_floor,
                                   s->weight_mode);
}

static SpeedGpuConfig uf_config(float sigma_nn, float nn_floor, int32_t mode, uint32_t blocks)
{
    SpeedGpuConfig config;
    memset(&config, 0, sizeof(config));
    config.geometry.blocks = blocks;
    config.scoring.sigma_nn = sigma_nn;
    config.scoring.nn_floor = nn_floor;
    config.scoring.weight_mode = mode;
    return config;
}

/* positive: both score pairs of a 4-channel tail, every weighting mode, the
 * default scoring options and vmaf_v1.0.16's. */
static char *test_host_tail_is_upstreams_entropy_and_score(void)
{
    unsigned mismatches = 0u;
    unsigned nonzero = 0u;
    for (uint32_t o = 0u; o < 2u; o++) {
        for (int32_t mode = 0; mode <= 6; mode++) {
            const SpeedGpuConfig config =
                uf_config(uf_scoring[o][0], uf_scoring[o][1], mode, UF_BLOCKS);
            UfTail tail;
            uf_fill_tail(&tail, 4u, 7u + (uint32_t)mode + 16u * o);
            float entropies[2u * UF_BLOCKS];
            SpeedGpuFrameResult result;
            mu_assert("the host tail failed",
                      speed_internal_gpu_tail_scores(&config, 4u, tail.words, entropies, &result) ==
                          0);
            for (uint32_t pair = 0u; pair < 2u; pair++) {
                const float upstream = uf_upstream_pair_score(&tail, pair, &config.scoring);
                mismatches +=
                    vmaf_test_bits_f32(result.score[pair]) != vmaf_test_bits_f32(upstream);
                nonzero += upstream > 0.0f;
            }
        }
    }
    mu_assert("the host tail's score is not upstream's", mismatches == 0u);
    mu_assert("every synthetic score is 0: the fixture scores nothing", nonzero == 28u);
    return NULL;
}

/* The fixture must tell the fp64 form from the fp32 form of port #213: one
 * entropy term of each, on the fixture's first channel. */
static char *test_host_tail_fixture_separates_the_two_forms(void)
{
    UfTail tail;
    uf_fill_tail(&tail, 2u, 7u);
    unsigned differs = 0u;
    for (uint32_t i = 0u; i < UF_BLOCKS; i++) {
        float fp64_form = 3.25f;
        float fp32_form = 3.25f;
        const float L = tail.eig[1] < 0 ? 0 : tail.eig[1];
        uf_upstream_update_entropy(uf_dims(1u, 1u), &fp64_form, &tail.var[i], L, 0.29f);
        fp32_form += log2f(L * tail.var[i] + 0.29f) + log2f(2.0f * (float)M_PI * (float)M_E);
        differs += vmaf_test_bits_f32(fp64_form) != vmaf_test_bits_f32(fp32_form);
    }
    mu_assert("the fixture cannot tell upstream's log2() from log2f()", differs != 0u);
    return NULL;
}

/* boundary: speed_extract_score() scores 0 when exactly one side could not be
 * inverted, and scores normally when both or neither could; the status words
 * are passed through. */
static char *test_host_tail_applies_the_singular_rule(void)
{
    const SpeedGpuConfig config = uf_config(0.29f, 0.0f, 0, UF_BLOCKS);
    UfTail tail;
    float entropies[2u * UF_BLOCKS];
    SpeedGpuFrameResult result;

    uf_fill_tail(&tail, 4u, 99u);
    tail.status[0] = 1; /* pair 0: the reference only */
    tail.status[3] = 1; /* channel 1 hit the iteration cap */
    tail.status[4] = 1; /* pair 1: both sides */
    tail.status[6] = 1;
    mu_assert("the host tail failed",
              speed_internal_gpu_tail_scores(&config, 4u, tail.words, entropies, &result) == 0);
    mu_assert("exactly one singular side must score 0", vmaf_test_bits_f32(result.score[0]) == 0u);
    mu_assert("two singular sides score as upstream does",
              vmaf_test_bits_f32(result.score[1]) ==
                  vmaf_test_bits_f32(uf_upstream_pair_score(&tail, 1u, &config.scoring)));
    mu_assert("status words must be passed through",
              result.singular[0] == 1 && result.singular[1] == 0 && result.singular[2] == 1 &&
                  result.singular[3] == 1 && result.iteration_cap[1] == 1 &&
                  result.iteration_cap[0] == 0);
    return NULL;
}

/* boundary: flat blocks (both entropies below the floor: nothing visible) and
 * a geometry of one block are valid inputs and score 0. */
static char *test_host_tail_scores_flat_and_single_blocks(void)
{
    UfTail tail;
    float entropies[2u * UF_BLOCKS];
    SpeedGpuFrameResult result;

    uf_fill_tail(&tail, 2u, 5u);
    memset(tail.var, 0, (size_t)2u * UF_BLOCKS * sizeof(float));
    const SpeedGpuConfig floor = uf_config(0.19f, 0.1f, 5, UF_BLOCKS);
    mu_assert("the host tail failed",
              speed_internal_gpu_tail_scores(&floor, 2u, tail.words, entropies, &result) == 0);
    mu_assert("flat blocks must score 0", vmaf_test_bits_f32(result.score[0]) == 0u);

    const SpeedGpuConfig single = uf_config(0.29f, 0.0f, 3, 1u);
    uint32_t words[2u * 2u + 2u * UF_ELEMENTS + 2u];
    memset(words, 0, sizeof(words));
    mu_assert("the host tail failed on a single block",
              speed_internal_gpu_tail_scores(&single, 2u, words, entropies, &result) == 0);
    mu_assert("a single flat block scores 0", vmaf_test_bits_f32(result.score[0]) == 0u);
    return NULL;
}

/* negative: a NULL argument. */
static char *test_host_tail_refuses_null_arguments(void)
{
    const SpeedGpuConfig good = uf_config(0.29f, 0.0f, 0, UF_BLOCKS);
    UfTail tail;
    uf_fill_tail(&tail, 4u, 1u);
    float entropies[2u * UF_BLOCKS];
    SpeedGpuFrameResult result;
    const void *words = tail.words;
    mu_assert("NULL config",
              speed_internal_gpu_tail_scores(NULL, 2u, words, entropies, &result) == -EINVAL);
    mu_assert("NULL tail",
              speed_internal_gpu_tail_scores(&good, 2u, NULL, entropies, &result) == -EINVAL);
    mu_assert("NULL scratch",
              speed_internal_gpu_tail_scores(&good, 2u, words, NULL, &result) == -EINVAL);
    mu_assert("NULL result",
              speed_internal_gpu_tail_scores(&good, 2u, words, entropies, NULL) == -EINVAL);
    return NULL;
}

/* negative: a channel count other than 2 or 4, a weighting mode outside
 * 0..6, a geometry without blocks. */
static char *test_host_tail_refuses_invalid_shapes(void)
{
    const SpeedGpuConfig good = uf_config(0.29f, 0.0f, 0, UF_BLOCKS);
    const SpeedGpuConfig bad_mode = uf_config(0.29f, 0.0f, 7, UF_BLOCKS);
    const SpeedGpuConfig negative_mode = uf_config(0.29f, 0.0f, -1, UF_BLOCKS);
    const SpeedGpuConfig no_blocks = uf_config(0.29f, 0.0f, 0, 0u);
    UfTail tail;
    uf_fill_tail(&tail, 4u, 1u);
    float entropies[2u * UF_BLOCKS];
    SpeedGpuFrameResult result;
    const void *words = tail.words;
    mu_assert("three channels",
              speed_internal_gpu_tail_scores(&good, 3u, words, entropies, &result) == -EINVAL);
    mu_assert("no channels",
              speed_internal_gpu_tail_scores(&good, 0u, words, entropies, &result) == -EINVAL);
    mu_assert("weighting mode 7",
              speed_internal_gpu_tail_scores(&bad_mode, 2u, words, entropies, &result) == -EINVAL);
    mu_assert("weighting mode -1", speed_internal_gpu_tail_scores(&negative_mode, 2u, words,
                                                                  entropies, &result) == -EINVAL);
    mu_assert("no blocks",
              speed_internal_gpu_tail_scores(&no_blocks, 2u, words, entropies, &result) == -EINVAL);
    return NULL;
}

static char *run_host_tail_tests(void)
{
    mu_run_test(test_host_tail_is_upstreams_entropy_and_score);
    mu_run_test(test_host_tail_fixture_separates_the_two_forms);
    mu_run_test(test_host_tail_applies_the_singular_rule);
    mu_run_test(test_host_tail_scores_flat_and_single_blocks);
    mu_run_test(test_host_tail_refuses_null_arguments);
    mu_run_test(test_host_tail_refuses_invalid_shapes);
    return NULL;
}

/* Test 4 where Netflix's values apply; elsewhere a visible skip with the
 * reason, never a silent pass. The test is compiled on every C library. The
 * body is the same in every build, the reason aside: an `#if` here gave the
 * two meson variants two different functions of one name, and CodeQL then
 * reported one of them unreachable (cpp/unused-static-function). */
static const char *const uf_netflix_values_skip_reason = UF_NETFLIX_VALUES_SKIP_REASON;

static char *run_netflix_value_tests(void)
{
    const char *const skip_reason = uf_netflix_values_skip_reason;
    if (skip_reason == NULL) {
        mu_run_test(test_cpu_extractors_return_netflix_masters_values);
        return NULL;
    }
    (void)fprintf(
        stderr, "test_cpu_extractors_return_netflix_masters_values: \033[33mskipped\033[0m (%s)\n",
        skip_reason);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_cpu_create_givens_is_upstreams);
    mu_run_test(test_cpu_update_entropy_is_upstreams);
    mu_run_test(test_cpu_speed_score_is_upstreams);
    mu_run_test(test_givens_unit_is_upstreams_on_every_input);
    mu_assert_msg(run_host_tail_tests());
    mu_assert_msg(run_netflix_value_tests());
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
