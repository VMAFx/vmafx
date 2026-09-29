/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * vif_sycl minimum frame size and CPU fallback
 * (T-INTEGER-VIF-TINY-FRAME-GUARD-2026-09-29).
 *
 * Every scale of integer VIF reflects its filter taps once, which stays inside
 * the plane only while floor(dim / 2^s) exceeds the tap half-width: 16 pixels
 * for the {17, 9, 5, 3} filters. Below that the SYCL twin read outside its
 * device buffers and lost the device (8x8 on an Arc B580 and a UHD 770).
 * vif_sycl now declares the bound through the ADR-1324 first-picture gate, so
 * model dispatch computes smaller frames with the CPU `vif`, and a direct
 * `vif_sycl` request below it fails init with -EINVAL instead of faulting.
 *
 * The boundary is recomputed here from the filter widths rather than copied
 * from the extractor. The GPU cases register the VIF features of a model,
 * which is the path allowed to fall back, and compare every scale with the
 * CPU extractor on the same noise frames: bit-identical below the bound, where
 * the CPU extractor runs, and within the SYCL VIF parity tolerance above it.
 * They skip when no SYCL device is visible.
 */

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mu_table.h"
#include "test.h"

#include "feature/feature_extractor.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_sycl.h"
#include "libvmaf/model.h"
#include "libvmaf/picture.h"
#include "model.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

/* ADR-0214 places=4 contract for vif. */
#define PARITY_TOL 5e-5
#define NUM_FRAMES 2u
#define NUM_SCALES 4u

static const char *const SCALE_KEYS[NUM_SCALES] = {
    "VMAF_integer_feature_vif_scale0_score",
    "VMAF_integer_feature_vif_scale1_score",
    "VMAF_integer_feature_vif_scale2_score",
    "VMAF_integer_feature_vif_scale3_score",
};

/* floor(dim / 2^s) >= half_width + 1 for the scale filters {17, 9, 5, 3} and
 * the decimation filters {9, 5, 3} of scales 0-2. */
static unsigned expected_min_dim(void)
{
    static const unsigned widths[NUM_SCALES] = {17u, 9u, 5u, 3u};
    static const unsigned rd_widths[NUM_SCALES] = {9u, 5u, 3u, 0u};
    unsigned min_dim = 1u;
    for (unsigned s = 0; s < NUM_SCALES; s++) {
        const unsigned need = (widths[s] / 2u + 1u) << s;
        const unsigned rd_need = rd_widths[s] ? (rd_widths[s] / 2u + 1u) << s : 1u;
        min_dim = need > min_dim ? need : min_dim;
        min_dim = rd_need > min_dim ? rd_need : min_dim;
    }
    return min_dim;
}

typedef struct {
    unsigned w;
    unsigned h;
} Geometry;

static uint8_t noise_sample(unsigned row, unsigned col, unsigned salt)
{
    uint32_t x = ((uint32_t)row << 16) ^ (uint32_t)col ^ (salt * 0x9E3779B9u);
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return (uint8_t)(x >> 24);
}

static int fill_picture(VmafPicture *pic, Geometry g, unsigned salt)
{
    const int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, 8u, g.w, g.h);
    if (err) {
        return err;
    }
    for (unsigned p = 0; p < 3u; p++) {
        uint8_t *plane = (uint8_t *)pic->data[p];
        for (unsigned row = 0; row < pic->h[p]; row++) {
            for (unsigned col = 0; col < pic->w[p]; col++) {
                plane[(row * pic->stride[p]) + col] = noise_sample(row, col, salt + (p << 8));
            }
        }
    }
    return 0;
}

/* Feed NUM_FRAMES noise pairs, flush, read every scale of every frame. */
static char *score_frames(VmafContext *vmaf, Geometry g, double out[NUM_FRAMES][NUM_SCALES])
{
    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        VmafPicture ref;
        VmafPicture dist;
        mu_assert("reference picture allocation failed", !fill_picture(&ref, g, 1u + i));
        if (fill_picture(&dist, g, 101u + i)) {
            (void)vmaf_picture_unref(&ref);
            return "distorted picture allocation failed";
        }
        mu_assert("vmaf_read_pictures failed", !vmaf_read_pictures(vmaf, &ref, &dist, i));
    }
    mu_assert("vmaf_read_pictures(EOS) failed", !vmaf_read_pictures(vmaf, NULL, NULL, 0));
    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        for (unsigned k = 0; k < NUM_SCALES; k++) {
            mu_assert("VIF score missing",
                      !vmaf_feature_score_at_index(vmaf, SCALE_KEYS[k], &out[i][k], i));
        }
    }
    return NULL;
}

static char *score_cpu(Geometry g, double out[NUM_FRAMES][NUM_SCALES])
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    mu_assert("CPU: vmaf_init failed", !vmaf_init(&vmaf, cfg));
    mu_assert("CPU: vmaf_use_feature(vif) failed", !vmaf_use_feature(vmaf, "vif", NULL));
    char *msg = score_frames(vmaf, g, out);
    (void)vmaf_close(vmaf);
    return msg;
}

/* Keep only the model's VIF features, in front, and return how many there
 * were so the caller can restore n_features before vmaf_model_destroy(). */
static unsigned keep_vif_features(VmafModel *model)
{
    const unsigned total = model->n_features;
    unsigned kept = 0;
    for (unsigned i = 0; i < total; i++) {
        if (strstr(model->feature[i].name, "vif")) {
            const VmafModelFeature tmp = model->feature[kept];
            model->feature[kept] = model->feature[i];
            model->feature[i] = tmp;
            kept++;
        }
    }
    model->n_features = kept;
    return total;
}

/* The model path: the one that may replace vif_sycl with the CPU `vif`. */
static char *score_sycl_model(Geometry g, double out[NUM_FRAMES][NUM_SCALES], int *skipped)
{
    VmafSyclState *state = NULL;
    VmafSyclConfiguration sycl_cfg = {.device_index = -1};
    if (vmaf_sycl_state_init(&state, sycl_cfg) != 0 || state == NULL) {
        *skipped = 1;
        return NULL;
    }
    VmafModel *model = NULL;
    VmafModelConfig model_cfg = {0};
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    char *msg = NULL;
    unsigned total_features = 0;
    if (vmaf_model_load(&model, &model_cfg, "vmaf_v0.6.1")) {
        msg = "SYCL: loading vmaf_v0.6.1 failed";
    } else {
        total_features = keep_vif_features(model);
        if (model->n_features != NUM_SCALES) {
            msg = "SYCL: vmaf_v0.6.1 no longer carries four VIF features";
        } else if (vmaf_init(&vmaf, cfg) || vmaf_sycl_import_state(vmaf, state)) {
            msg = "SYCL: context setup failed";
        } else if (vmaf_use_features_from_model(vmaf, model)) {
            msg = "SYCL: vmaf_use_features_from_model failed";
        } else {
            msg = score_frames(vmaf, g, out);
        }
    }
    if (vmaf) {
        (void)vmaf_close(vmaf);
    }
    if (model) {
        model->n_features = total_features ? total_features : model->n_features;
        vmaf_model_destroy(model);
    }
    vmaf_sycl_state_free(&state);
    return msg;
}

static char *test_vif_sycl_declares_min_dim(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("vif_sycl");
    mu_assert("vif_sycl is not registered", fex != NULL);
    mu_assert("vif_sycl must declare an ADR-1324 context check", fex->context_check != NULL);
    mu_assert("vif_sycl must fall back to the CPU `vif`",
              fex->context_fallback_name && !strcmp(fex->context_fallback_name, "vif"));
    VmafFeatureExtractor *cpu = vmaf_get_feature_extractor_by_name(fex->context_fallback_name);
    const unsigned device_flags = VMAF_FEATURE_EXTRACTOR_CUDA | VMAF_FEATURE_EXTRACTOR_SYCL |
                                  VMAF_FEATURE_EXTRACTOR_HIP | VMAF_FEATURE_EXTRACTOR_METAL;
    mu_assert("the fallback must be a CPU extractor",
              cpu != NULL && (cpu->flags & device_flags) == 0);

    const unsigned min_dim = expected_min_dim();
    mu_assert("integer VIF filter footprint is not 16 pixels", min_dim == 16u);
    const Geometry accepted[] = {{min_dim, min_dim}, {min_dim, 4096u}, {1920u, 1080u}};
    const Geometry rejected[] = {
        {min_dim - 1u, min_dim},
        {min_dim, min_dim - 1u},
        {min_dim - 1u, min_dim - 1u},
        {8u, 8u},
        {3u, 3u},
        {4096u, 9u},
    };
    for (size_t i = 0; i < sizeof(accepted) / sizeof(accepted[0]); i++) {
        mu_assert("vif_sycl must accept frames at or above the minimum",
                  fex->context_check(fex, VMAF_PIX_FMT_YUV420P, 8u, accepted[i].w, accepted[i].h) ==
                      0);
    }
    for (size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); i++) {
        mu_assert("vif_sycl must route frames below the minimum to the CPU",
                  fex->context_check(fex, VMAF_PIX_FMT_YUV420P, 8u, rejected[i].w, rejected[i].h) ==
                      -ENOTSUP);
    }
    return NULL;
}

/* A direct request gets no fallback: init() refuses before touching the
 * device state or the private state, so a zeroed state needs no close. */
static char *test_vif_sycl_direct_init_rejects_below_min(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("vif_sycl");
    mu_assert("vif_sycl is not registered", fex != NULL);
    const Geometry rejected[] = {{15u, 15u}, {15u, 64u}, {64u, 15u}, {8u, 8u}};
    for (size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); i++) {
        void *priv = calloc(1, fex->priv_size);
        mu_assert("calloc failed", priv != NULL);
        fex->priv = priv;
        const int rc = fex->init(fex, VMAF_PIX_FMT_YUV420P, 8u, rejected[i].w, rejected[i].h);
        free(priv);
        fex->priv = NULL;
        mu_assert("direct vif_sycl must reject frames below the minimum with -EINVAL",
                  rc == -EINVAL);
    }
    return NULL;
}

/* Largest device-path difference seen, printed for the record. */
static double worst_device_delta;

static char *check_boundary_case(Geometry g, int *skipped)
{
    double cpu[NUM_FRAMES][NUM_SCALES];
    double gpu[NUM_FRAMES][NUM_SCALES];
    char *msg = score_cpu(g, cpu);
    if (msg) {
        return msg;
    }
    msg = score_sycl_model(g, gpu, skipped);
    if (msg || *skipped) {
        return msg;
    }
    const int on_cpu = g.w < expected_min_dim() || g.h < expected_min_dim();
    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        for (unsigned k = 0; k < NUM_SCALES; k++) {
            const double delta = fabs(cpu[i][k] - gpu[i][k]);
            const int ok = on_cpu ? (delta == 0.0) : (delta <= PARITY_TOL);
            if (!on_cpu && delta > worst_device_delta) {
                worst_device_delta = delta;
            }
            if (!ok) {
                (void)fprintf(stderr, "\n  %ux%u frame %u %s: cpu=%.10f model=%.10f\n", g.w, g.h, i,
                              SCALE_KEYS[k], cpu[i][k], gpu[i][k]);
                return on_cpu ? "below the minimum the model path must run the CPU `vif`" :
                                "above the minimum vif_sycl must match the CPU `vif`";
            }
        }
    }
    return NULL;
}

static char *test_vif_sycl_model_boundary(void)
{
    const unsigned m = expected_min_dim();
    /* 17x17 and 853x480 have odd widths below scale 0 (853 -> 426 -> 213),
     * which the rd stride used to skew (T-SYCL-VIF-ODD-WIDTH-RD-STRIDE). */
    const Geometry cases[] = {{m, m},   {m - 1u, m - 1u}, {m - 1u, 64u}, {64u, m - 1u},
                              {8u, 8u}, {m + 1u, m + 1u}, {96u, 64u},    {853u, 480u}};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int skipped = 0;
        char *msg = check_boundary_case(cases[i], &skipped);
        if (msg) {
            return msg;
        }
        if (skipped) {
            (void)fprintf(stderr, "[skip: no SYCL device] ");
            mu_skipped = 1;
            return NULL;
        }
    }
    (void)fprintf(stderr, "[device path max |delta| %.3e] ", worst_device_delta);
    return NULL;
}

char *run_tests(void)
{
    static const MuTest tests[] = {
        MU_TEST(test_vif_sycl_declares_min_dim),
        MU_TEST(test_vif_sycl_direct_init_rejects_below_min),
        MU_TEST(test_vif_sycl_model_boundary),
    };
    return mu_run_table(tests, MU_TABLE_LEN(tests));
}

/* NOLINTEND(modernize-use-nullptr) */
