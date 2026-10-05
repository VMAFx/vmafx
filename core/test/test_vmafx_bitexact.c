/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * VMAFx scores equal libvmaf scores bit for bit (ADR-1852, RC4 WP2).
 *
 * For the Netflix 576x324 golden pair, both 1080p checkerboard pairs and the
 * 10-bit sparks pair, and for the models vmaf_v1.0.16_3d0h and vmaf_v0.6.1,
 * one session runs through libvmaf (vmaf_init, vmaf_picture_alloc,
 * vmaf_read_pictures) and one through VMAFx (vmafx_context_create, frames
 * borrowed from the file buffers with vmafx_frame_wrap_host, vmafx_submit).
 * Compared with memcmp of the doubles: the model score of every frame, the
 * pooled model score for every pool method, every feature the collector
 * holds at every frame, and every feature pooled with every method. The two
 * sessions must also hold the same feature names.
 *
 * The YUV files live in python/test/resource/yuv (VMAFX_TEST_YUV_DIR); the
 * test is skipped (77) when they are missing.
 *
 * Failing first: on the WP1 base the vmafx_ functions do not exist. Measured
 * with planted defects on this branch: a borrowed frame whose chroma planes
 * are offset by one row fails the feature comparison of every input; a pool
 * method passed off by one fails the pooled comparison.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "feature/feature_collector.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/model.h"
#include "libvmaf_priv.h"
#include "mu_table.h"
#include "test.h"
#include "vmafx/libvmaf_bridge.h"
#include "vmafx/vmafx.h"
#include "vmafx_test_util.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

typedef struct Input {
    const char *ref;
    const char *dist;
    uint32_t w, h, bpc;
} Input;

static const Input inputs[] = {
    {"src01_hrc00_576x324.yuv", "src01_hrc01_576x324.yuv", 576, 324, 8},
    {"checkerboard_1920_1080_10_3_0_0.yuv", "checkerboard_1920_1080_10_3_1_0.yuv", 1920, 1080, 8},
    {"checkerboard_1920_1080_10_3_0_0.yuv", "checkerboard_1920_1080_10_3_10_0.yuv", 1920, 1080, 8},
    {"sparks_ref_480x270.yuv42010le.yuv", "sparks_dis_480x270.yuv42010le.yuv", 480, 270, 10},
};

static const char *const models[] = {"vmaf_v1.0.16_3d0h", "vmaf_v0.6.1"};

static const uint32_t all_pools[] = {
    VMAFX_POOL_MIN,    VMAFX_POOL_MAX,   VMAFX_POOL_MEAN,   VMAFX_POOL_HARMONIC_MEAN,
    VMAFX_POOL_MEDIAN, VMAFX_POOL_PERC5, VMAFX_POOL_PERC10, VMAFX_POOL_PERC20,
};
#define N_POOLS (sizeof(all_pools) / sizeof(all_pools[0]))

/* Values compared, over the whole run. */
static unsigned long compared;

typedef struct Clip {
    VmafxFrameDesc desc;
    uint8_t *ref;
    uint8_t *dist;
    unsigned n_frames;
} Clip;

/* The whole file `name` of the fixture directory, or NULL. */
static uint8_t *read_fixture(const char *name, size_t *size)
{
    const char *dir = getenv("VMAFX_TEST_YUV_DIR");
    char path[4096];
    const int n = snprintf(path, sizeof(path), "%s/%s", dir ? dir : ".", name);
    FILE *file = n > 0 && (size_t)n < sizeof(path) ? fopen(path, "rb") : NULL;
    if (!file) {
        return NULL;
    }
    uint8_t *data = NULL;
    if (fseek(file, 0, SEEK_END) == 0) {
        const long end = ftell(file);
        *size = end > 0 ? (size_t)end : 0u;
        data = *size && fseek(file, 0, SEEK_SET) == 0 ? malloc(*size) : NULL;
    }
    if (data && fread(data, 1, *size, file) != *size) {
        free(data);
        data = NULL;
    }
    (void)fclose(file);
    return data;
}

static bool clip_open(Clip *clip, const Input *in)
{
    clip->desc = vt_desc(VMAFX_PIXEL_FORMAT_YUV420P, in->bpc, in->w, in->h);
    size_t ref_size = 0;
    size_t dist_size = 0;
    clip->ref = read_fixture(in->ref, &ref_size);
    clip->dist = read_fixture(in->dist, &dist_size);
    const size_t frame = vt_frame_bytes(&clip->desc);
    clip->n_frames = (unsigned)(ref_size / frame);
    return clip->ref && clip->dist && ref_size == dist_size && clip->n_frames > 0;
}

static void clip_close(Clip *clip)
{
    free(clip->ref);
    free(clip->dist);
}

/* ---- The libvmaf session -------------------------------------------------------- */

static bool read_picture(VmafPicture *pic, const Clip *clip, const uint8_t *src)
{
    if (vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, clip->desc.bpc, clip->desc.w, clip->desc.h)) {
        return false;
    }
    const size_t bytes = clip->desc.bpc > 8u ? 2u : 1u;
    for (unsigned p = 0; p < 3u; p++) {
        const size_t row = (size_t)pic->w[p] * bytes;
        for (unsigned y = 0; y < pic->h[p]; y++, src += row) {
            memcpy((uint8_t *)pic->data[p] + (size_t)y * (size_t)pic->stride[p], src, row);
        }
    }
    return true;
}

static VmafContext *run_libvmaf(const Clip *clip, VmafModel *model)
{
    VmafConfiguration cfg;
    memset(&cfg, 0, sizeof(cfg));
    VmafContext *vmaf = NULL;
    if (vmaf_init(&vmaf, cfg) || vmaf_use_features_from_model(vmaf, model)) {
        return NULL;
    }
    const size_t frame = vt_frame_bytes(&clip->desc);
    bool ok = true;
    for (unsigned i = 0; i < clip->n_frames && ok; i++) {
        VmafPicture ref;
        VmafPicture dist;
        ok = read_picture(&ref, clip, clip->ref + i * frame) &&
             read_picture(&dist, clip, clip->dist + i * frame) &&
             vmaf_read_pictures(vmaf, &ref, &dist, i) == 0;
    }
    return ok && vmaf_read_pictures(vmaf, NULL, NULL, 0) == 0 ? vmaf : NULL;
}

/* ---- The VMAFx session ---------------------------------------------------------- */

static VmafxContext *run_vmafx(Clip *clip, VmafxModel *model)
{
    VmafxContext *context = NULL;
    if (vmafx_context_create(NULL, &context, NULL) != VMAFX_OK ||
        vmafx_context_use_model(context, model, NULL) != VMAFX_OK) {
        return NULL;
    }
    const size_t frame = vt_frame_bytes(&clip->desc);
    bool ok = true;
    for (unsigned i = 0; i < clip->n_frames && ok; i++) {
        VmafxFrame *ref = vt_wrap_frame(&clip->desc, clip->ref + i * frame, NULL);
        VmafxFrame *dist = vt_wrap_frame(&clip->desc, clip->dist + i * frame, NULL);
        ok = vmafx_submit(context, ref, dist, i, NULL) == VMAFX_OK;
    }
    return ok && vmafx_flush(context, NULL) == VMAFX_OK ? context : NULL;
}

/* ---- Comparisons --------------------------------------------------------------- */

static bool same_bits(double a, double b)
{
    compared++;
    return memcmp(&a, &b, sizeof(a)) == 0;
}

static char *compare_model(VmafContext *vmaf, VmafModel *legacy, VmafxContext *context,
                           const VmafxModel *model, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        double a = 0.0;
        VmafxScore b = VMAFX_SCORE_INIT;
        mu_assert("model score", vmaf_score_at_index(vmaf, legacy, &a, i) == 0 &&
                                     vmafx_score_frame(context, model, i, &b, NULL) == VMAFX_OK &&
                                     same_bits(a, b.value));
    }
    for (size_t p = 0; p < N_POOLS; p++) {
        double a = 0.0;
        VmafxPooledScore b = VMAFX_POOLED_SCORE_INIT;
        const enum VmafPoolingMethod method = (enum VmafPoolingMethod)all_pools[p];
        mu_assert("pooled model score",
                  vmaf_score_pooled(vmaf, legacy, method, &a, 0, n - 1) == 0 &&
                      vmafx_score_pooled(context, model, all_pools[p], 0, n - 1, &b, NULL) ==
                          VMAFX_OK &&
                      same_bits(a, b.value));
    }
    return NULL;
}

/* One feature at every frame and pooled with every method: same status, and
 * the same bits where both scored. */
static char *compare_feature(VmafContext *vmaf, VmafxContext *context, const char *name, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        double a = 0.0;
        VmafxScore b = VMAFX_SCORE_INIT;
        const int err = vmaf_feature_score_at_index(vmaf, name, &a, i);
        const VmafxStatus status = vmafx_feature_score(context, name, i, &b, NULL);
        mu_assert("feature status", (err == 0) == (status == VMAFX_OK));
        mu_assert("feature score", err != 0 || same_bits(a, b.value));
    }
    for (size_t p = 0; p < N_POOLS; p++) {
        double a = 0.0;
        VmafxPooledScore b = VMAFX_POOLED_SCORE_INIT;
        const int err = vmaf_feature_score_pooled(vmaf, name, (enum VmafPoolingMethod)all_pools[p],
                                                  &a, 0, n - 1);
        const VmafxStatus status =
            vmafx_feature_score_pooled(context, name, all_pools[p], 0, n - 1, &b, NULL);
        mu_assert("pooled feature status", (err == 0) == (status == VMAFX_OK));
        mu_assert("pooled feature", err != 0 || same_bits(a, b.value));
    }
    return NULL;
}

static char *compare_features(VmafContext *vmaf, VmafxContext *context, unsigned n)
{
    const VmafFeatureCollector *a = vmaf_feature_collector_get(vmaf);
    const VmafFeatureCollector *b =
        vmaf_feature_collector_get(vmafx_context_libvmaf_handle(context));
    mu_assert("collectors", a && b && a->cnt == b->cnt && a->cnt > 0);
    for (unsigned f = 0; f < a->cnt; f++) {
        const char *name = a->feature_vector[f]->name;
        mu_assert("same feature names", !strcmp(name, b->feature_vector[f]->name));
        mu_assert_msg(compare_feature(vmaf, context, name, n));
    }
    return NULL;
}

static char *compare_case(Clip *clip, const char *version)
{
    VmafModelConfig mcfg = {0};
    VmafModel *legacy = NULL;
    VmafxModel *model = NULL;
    mu_assert("models", vmaf_model_load(&legacy, &mcfg, version) == 0 &&
                            vmafx_model_load(NULL, version, &model, NULL) == VMAFX_OK);
    VmafContext *vmaf = run_libvmaf(clip, legacy);
    VmafxContext *context = run_vmafx(clip, model);
    mu_assert("sessions", vmaf && context);
    mu_assert_msg(compare_model(vmaf, legacy, context, model, clip->n_frames));
    mu_assert_msg(compare_features(vmaf, context, clip->n_frames));
    mu_assert("close", vmaf_close(vmaf) == 0 && vmafx_context_destroy(context, NULL) == VMAFX_OK);
    vmaf_model_destroy(legacy);
    vmafx_model_unref(model);
    return NULL;
}

static char *test_vmafx_equals_libvmaf(void)
{
    unsigned cases = 0;
    for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); i++) {
        Clip clip;
        if (!clip_open(&clip, &inputs[i])) {
            clip_close(&clip);
            continue;
        }
        for (size_t m = 0; m < sizeof(models) / sizeof(models[0]); m++) {
            mu_assert_msg(compare_case(&clip, models[m]));
            cases++;
        }
        clip_close(&clip);
    }
    (void)fprintf(stderr, "[%u cases, %lu values compared] ", cases, compared);
    mu_skipped = cases == 0;
    mu_assert("every input present or none (partial fixture set)", cases == 0 || cases == 8u);
    return NULL;
}

char *run_tests(void)
{
    static const MuTest tests[] = {
        MU_TEST(test_vmafx_equals_libvmaf),
    };
    return mu_run_table(tests, MU_TABLE_LEN(tests));
}

/* NOLINTEND(modernize-use-nullptr) */
