/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * float_vif CPU vs. CUDA parity (ADR-0947; exact since ADR-1412).
 *
 * The float-path VIF extractor is implemented in core/src/feature/float_vif.c
 * (CPU) and core/src/feature/cuda/float_vif_cuda.c (CUDA). Since ADR-1412 the
 * twin computes the CPU's arithmetic and adds in the CPU's order, so this test
 * asserts equality, not a tolerance: every output of every frame has the
 * CPU's bits.
 *
 * Cases: the default options on the build's fixture (256x144, or 960x540 in
 * the `_large` variant); `debug=true`, which adds the frame ratio and the
 * eleven numerator / denominator sums; the options a model sets
 * (`vif_enhn_gain_limit`, `vif_sigma_nsq`, ADR-1217), `vif_skip_scale0`, and
 * the per-scale floors (`vif_scale1..3_min_val`); 10-bit input; and a 50x38
 * frame, whose planes are smaller than the kernels' 16x16 tiles and not a
 * multiple of them at any scale.
 *
 * Before ADR-1412 the default case was up to 8.8e-6 from the CPU on this fixture
 * (the kernel carried a table of Gaussian taps that is not the one
 * vif_get_filter() computes), so every case here fails on the old twin.
 *
 * Skip behaviour: if vmaf_cuda_state_init() fails (no driver / no device) the
 * test emits "[skip: no CUDA device]" and passes.
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "test.h"

#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_cuda.h"
#include "libvmaf/picture.h"

#ifndef FIXTURE_W
#define FIXTURE_W 256u
#endif
#ifndef FIXTURE_H
#define FIXTURE_H 144u
#endif
#define NUM_FRAMES 3u
#define MAX_KEYS 15u
#define MAX_OPTS 3u

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. ADR-1138. */

typedef struct VifOption {
    const char *key;
    const char *value;
} VifOption;

typedef struct ParityCase {
    const char *name;
    unsigned w;
    unsigned h;
    unsigned bpc;
    VifOption opts[MAX_OPTS];
    unsigned n_keys;
    const char *keys[MAX_KEYS];
} ParityCase;

typedef struct Scores {
    double v[NUM_FRAMES][MAX_KEYS];
} Scores;

static void put_sample(VmafPicture *pic, unsigned plane, unsigned row, unsigned col, unsigned v)
{
    const unsigned peak = (1u << pic->bpc) - 1u;
    uint8_t *line = (uint8_t *)pic->data[plane] + (size_t)row * (size_t)pic->stride[plane];
    if (pic->bpc <= 8u) {
        line[col] = (uint8_t)(v & peak);
    } else {
        ((uint16_t *)line)[col] = (uint16_t)(v & peak);
    }
}

/* A gradient with a frame-dependent phase for the reference; the distorted
 * frame adds a small periodic error. Chroma differs from luma so an
 * accidental chroma read shows (VIF is luma only). */
static int fill_picture(VmafPicture *pic, const ParityCase *c, unsigned frame_idx, bool distorted)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, c->bpc, c->w, c->h);
    if (err)
        return err;
    const unsigned gain = 1u << (c->bpc - 8u);
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            unsigned v = (row + col + frame_idx * 7u) * gain + (row * col) % gain;
            if (distorted)
                v += ((row * 2u + col + frame_idx * 3u) % 13u) * gain;
            put_sample(pic, 0u, row, col, v);
        }
    }
    for (unsigned p = 1; p < 3; p++) {
        for (unsigned row = 0; row < pic->h[p]; row++) {
            for (unsigned col = 0; col < pic->w[p]; col++)
                put_sample(pic, p, row, col, (row * 3u + col * 5u + p * 17u + frame_idx) * gain);
        }
    }
    return 0;
}

static char *feed_and_read(VmafContext *vmaf, const ParityCase *c, Scores *out)
{
    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        VmafPicture ref;
        VmafPicture dist;
        mu_assert("fill reference failed", !fill_picture(&ref, c, i, false));
        mu_assert("fill distorted failed", !fill_picture(&dist, c, i, true));
        mu_assert("vmaf_read_pictures failed", !vmaf_read_pictures(vmaf, &ref, &dist, i));
    }
    mu_assert("vmaf_read_pictures(EOS) failed", !vmaf_read_pictures(vmaf, NULL, NULL, 0));
    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        for (unsigned k = 0; k < c->n_keys; k++) {
            if (vmaf_feature_score_at_index(vmaf, c->keys[k], &out->v[i][k], i)) {
                (void)fprintf(stderr, "\n%s: no score for %s at frame %u\n", c->name, c->keys[k],
                              i);
                return "vmaf_feature_score_at_index failed";
            }
        }
    }
    return NULL;
}

static char *use_feature(VmafContext *vmaf, const ParityCase *c, const char *extractor)
{
    VmafFeatureDictionary *opts = NULL;
    for (unsigned i = 0; i < MAX_OPTS && c->opts[i].key; i++) {
        mu_assert("vmaf_feature_dictionary_set failed",
                  !vmaf_feature_dictionary_set(&opts, c->opts[i].key, c->opts[i].value));
    }
    const int err = vmaf_use_feature(vmaf, extractor, opts);
    if (err)
        (void)vmaf_feature_dictionary_free(&opts);
    mu_assert("vmaf_use_feature failed", !err);
    return NULL;
}

static char *run_cpu(const ParityCase *c, Scores *out)
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    mu_assert("CPU: vmaf_init failed", !vmaf_init(&vmaf, cfg));
    mu_assert_msg(use_feature(vmaf, c, "float_vif"));
    mu_assert_msg(feed_and_read(vmaf, c, out));
    mu_assert("CPU: vmaf_close failed", !vmaf_close(vmaf));
    return NULL;
}

static char *run_cuda(const ParityCase *c, Scores *out, int *skipped)
{
    *skipped = 0;
    VmafCudaState *cu_state = NULL;
    VmafCudaConfiguration cuda_cfg = {0};
    if (vmaf_cuda_state_init(&cu_state, cuda_cfg) != 0 || cu_state == NULL) {
        (void)fprintf(stderr, "[skip: no CUDA device] ");
        *skipped = 1;
        return NULL;
    }
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    mu_assert("CUDA: vmaf_init failed", !vmaf_init(&vmaf, cfg));
    mu_assert("CUDA: vmaf_cuda_import_state failed", !vmaf_cuda_import_state(vmaf, cu_state));
    mu_assert_msg(use_feature(vmaf, c, "float_vif_cuda"));
    mu_assert_msg(feed_and_read(vmaf, c, out));
    mu_assert("CUDA: vmaf_close failed", !vmaf_close(vmaf));
    mu_assert("CUDA: vmaf_cuda_state_free failed", !vmaf_cuda_state_free(cu_state));
    return NULL;
}

/* Every output of every frame equal, bit for bit. */
static char *check_case(const ParityCase *c)
{
    static Scores cpu;
    static Scores cuda;
    int skipped = 0;
    mu_assert_msg(run_cpu(c, &cpu));
    mu_assert_msg(run_cuda(c, &cuda, &skipped));
    if (skipped)
        return NULL;

    unsigned mismatches = 0u;
    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        for (unsigned k = 0; k < c->n_keys; k++) {
            mu_assert("CPU float_vif output is non-finite", isfinite(cpu.v[i][k]));
            if (cpu.v[i][k] == cuda.v[i][k])
                continue;
            mismatches++;
            (void)fprintf(stderr, "\n%s frame %u %s: cpu=%.17g cuda=%.17g delta=%.3e\n", c->name, i,
                          c->keys[k], cpu.v[i][k], cuda.v[i][k], fabs(cpu.v[i][k] - cuda.v[i][k]));
        }
    }
    mu_assert("float_vif_cuda differs from the CPU extractor", mismatches == 0u);
    return NULL;
}

#define SCALE_KEYS(suffix)                                                                         \
    "vif_scale0" suffix, "vif_scale1" suffix, "vif_scale2" suffix, "vif_scale3" suffix

static char *test_default_options_exact(void)
{
    static const ParityCase c = {
        .name = "default",
        .w = FIXTURE_W,
        .h = FIXTURE_H,
        .bpc = 8u,
        .n_keys = 4u,
        .keys = {"VMAF_feature_vif_scale0_score", "VMAF_feature_vif_scale1_score",
                 "VMAF_feature_vif_scale2_score", "VMAF_feature_vif_scale3_score"},
    };
    return check_case(&c);
}

/* debug=true publishes the frame ratio and every per-scale sum the ratio is
 * formed from; the sums are the reference's fp32 accumulators. */
static char *test_debug_outputs_exact(void)
{
    static const ParityCase c = {
        .name = "debug",
        .w = FIXTURE_W,
        .h = FIXTURE_H,
        .bpc = 8u,
        .opts = {{"debug", "true"}},
        .n_keys = 15u,
        .keys = {"VMAF_feature_vif_scale0_score", "VMAF_feature_vif_scale1_score",
                 "VMAF_feature_vif_scale2_score", "VMAF_feature_vif_scale3_score", "vif", "vif_num",
                 "vif_den", "vif_num_scale0", "vif_den_scale0", "vif_num_scale1", "vif_den_scale1",
                 "vif_num_scale2", "vif_den_scale2", "vif_num_scale3", "vif_den_scale3"},
    };
    return check_case(&c);
}

/* ADR-1217 — vif_enhn_gain_limit / vif_sigma_nsq must reach the kernel: the
 * values model/vmaf_float_v0.6.1neg.json ships (`vif_enhn_gain_limit = 1.0`)
 * plus a non-default neural-noise variance that is not a power of two, so the
 * fp64 arithmetic around vif_sigma_nsq matters. Both are feature parameters,
 * so the score is filed under a derived key: alias base + `_<alias>_<%g>` per
 * option, sorted by option name. */
static char *test_model_options_exact(void)
{
    static const ParityCase c = {
        .name = "egl=1 snsq=1.5",
        .w = FIXTURE_W,
        .h = FIXTURE_H,
        .bpc = 8u,
        .opts = {{"vif_enhn_gain_limit", "1.0"}, {"vif_sigma_nsq", "1.5"}},
        .n_keys = 4u,
        .keys = {SCALE_KEYS("_egl_1_snsq_1.5")},
    };
    return check_case(&c);
}

static char *test_skip_scale0_exact(void)
{
    static const ParityCase c = {
        .name = "vif_skip_scale0",
        .w = FIXTURE_W,
        .h = FIXTURE_H,
        .bpc = 8u,
        .opts = {{"vif_skip_scale0", "true"}},
        .n_keys = 4u,
        .keys = {SCALE_KEYS("_ssclz")},
    };
    mu_assert_msg(check_case(&c));

    static Scores cpu;
    mu_assert_msg(run_cpu(&c, &cpu));
    mu_assert("float_vif scale 0 must be exactly 0 with vif_skip_scale0=true", cpu.v[1][0] == 0.0);
    return NULL;
}

/* The per-scale floors of the CPU option table: a ratio below the floor is
 * published as the floor. 1.0 lifts every frame of scale 1; 0.5 leaves
 * scale 3 alone. */
static char *test_scale_minimums_exact(void)
{
    static const ParityCase c = {
        .name = "min_val",
        .w = FIXTURE_W,
        .h = FIXTURE_H,
        .bpc = 8u,
        .opts = {{"vif_scale1_min_val", "1.0"}, {"vif_scale3_min_val", "0.5"}},
        .n_keys = 4u,
        .keys = {SCALE_KEYS("_s1miv_1_s3miv_0.5")},
    };
    mu_assert_msg(check_case(&c));

    static Scores cpu;
    mu_assert_msg(run_cpu(&c, &cpu));
    mu_assert("vif_scale1_min_val=1.0 must publish 1.0 for scale 1", cpu.v[1][1] == 1.0);
    return NULL;
}

static char *test_10bit_exact(void)
{
    static const ParityCase c = {
        .name = "10-bit",
        .w = FIXTURE_W,
        .h = FIXTURE_H,
        .bpc = 10u,
        .n_keys = 4u,
        .keys = {"VMAF_feature_vif_scale0_score", "VMAF_feature_vif_scale1_score",
                 "VMAF_feature_vif_scale2_score", "VMAF_feature_vif_scale3_score"},
    };
    return check_case(&c);
}

/* 50x38 halves to 25x19, 12x9 and 6x4: no scale is a multiple of the 16x16
 * block, and from scale 2 on the plane is smaller than one tile, so the
 * padding loads rely on the index clamp. */
static char *test_small_odd_frame_exact(void)
{
    static const ParityCase c = {
        .name = "50x38",
        .w = 50u,
        .h = 38u,
        .bpc = 8u,
        .opts = {{"debug", "true"}},
        .n_keys = 15u,
        .keys = {"VMAF_feature_vif_scale0_score", "VMAF_feature_vif_scale1_score",
                 "VMAF_feature_vif_scale2_score", "VMAF_feature_vif_scale3_score", "vif", "vif_num",
                 "vif_den", "vif_num_scale0", "vif_den_scale0", "vif_num_scale1", "vif_den_scale1",
                 "vif_num_scale2", "vif_den_scale2", "vif_num_scale3", "vif_den_scale3"},
    };
    return check_case(&c);
}

char *run_tests(void)
{
    mu_run_test(test_default_options_exact);
    mu_run_test(test_debug_outputs_exact);
    mu_run_test(test_model_options_exact);
    mu_run_test(test_skip_scale0_exact);
    mu_run_test(test_scale_minimums_exact);
    mu_run_test(test_10bit_exact);
    mu_run_test(test_small_odd_frame_exact);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
