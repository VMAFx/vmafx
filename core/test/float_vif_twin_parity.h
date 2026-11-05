/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * float_vif CPU vs. GPU twin: the fixtures, the cases and the bit-for-bit
 * comparison a twin's parity test wraps (ADR-1412 for CUDA, ADR-1422 for
 * SYCL).
 *
 * A twin that computes the CPU's arithmetic and adds in the CPU's order
 * returns the CPU extractor's doubles, so every comparison here is exact:
 * every output of every frame has the CPU's bits. The cases:
 *
 *   - the default options on the build's fixture (256x144, or what a
 *     `_large` variant sets);
 *   - `debug=true`, which adds the frame ratio and the eleven numerator /
 *     denominator sums, the reference's fp32 accumulators themselves;
 *   - the options a model sets (`vif_enhn_gain_limit`, `vif_sigma_nsq`,
 *     ADR-1217), with a variance that is not a power of two so the fp64
 *     arithmetic around it matters;
 *   - `vif_skip_scale0` and the per-scale floors (`vif_scale1..3_min_val`);
 *   - 10-bit input;
 *   - a 50x38 frame, whose planes are smaller than a 16x16 tile and not a
 *     multiple of one at any scale.
 *
 * A test describes its backend in one VifTwin and wraps the vif_twin_*()
 * cases. Each comparison opens its own device state: a SYCL state keeps the
 * geometry of its first frame. Without a device a case is skipped and the
 * test exits 77.
 */

#ifndef LIBVMAF_TEST_FLOAT_VIF_TWIN_PARITY_H_
#define LIBVMAF_TEST_FLOAT_VIF_TWIN_PARITY_H_

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test.h"

#include "feature/feature_extractor.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/picture.h"

/* NOLINTBEGIN(modernize-use-nullptr): included by C translation units. The
 * fork builds C as C23, where clang-tidy proposes `nullptr`, but the required
 * MSVC C build does not provide that keyword. Preserve the portable C
 * spelling. ADR-1138. */

#ifndef FIXTURE_W
#define FIXTURE_W 256u
#endif
#ifndef FIXTURE_H
#define FIXTURE_H 144u
#endif

#define VIF_TWIN_FRAMES 3u
#define VIF_TWIN_MAX_KEYS 15u
#define VIF_TWIN_MAX_OPTS 3u

/* One GPU backend's twin, as the shared cases drive it. */
typedef struct VifTwin {
    const char *extractor; /* registry name, e.g. "float_vif_sycl" */
    const char *backend;   /* for messages, e.g. "SYCL" */
    /* Opens a device state. Non-zero: no device, the case is skipped. */
    int (*open)(void **state);
    int (*import)(VmafContext *vmaf, void *state);
    int (*close)(void *state);
} VifTwin;

typedef struct VifTwinOption {
    const char *key;
    const char *value;
} VifTwinOption;

typedef struct VifTwinCase {
    const char *name;
    unsigned w;
    unsigned h;
    unsigned bpc;
    VifTwinOption opts[VIF_TWIN_MAX_OPTS];
    unsigned n_keys;
    const char *keys[VIF_TWIN_MAX_KEYS];
} VifTwinCase;

typedef struct VifTwinScores {
    double v[VIF_TWIN_FRAMES][VIF_TWIN_MAX_KEYS];
} VifTwinScores;

#define VIF_TWIN_SCALE_KEYS(suffix)                                                                \
    "vif_scale0" suffix, "vif_scale1" suffix, "vif_scale2" suffix, "vif_scale3" suffix

#define VIF_TWIN_DEFAULT_KEYS                                                                      \
    "VMAF_feature_vif_scale0_score", "VMAF_feature_vif_scale1_score",                              \
        "VMAF_feature_vif_scale2_score", "VMAF_feature_vif_scale3_score"

#define VIF_TWIN_DEBUG_KEYS                                                                        \
    VIF_TWIN_DEFAULT_KEYS, "vif", "vif_num", "vif_den", "vif_num_scale0", "vif_den_scale0",        \
        "vif_num_scale1", "vif_den_scale1", "vif_num_scale2", "vif_den_scale2", "vif_num_scale3",  \
        "vif_den_scale3"

static inline void vif_twin_put_sample(VmafPicture *pic, unsigned plane, unsigned row, unsigned col,
                                       unsigned value)
{
    const unsigned peak = (1u << pic->bpc) - 1u;
    uint8_t *line = (uint8_t *)pic->data[plane] + (size_t)row * (size_t)pic->stride[plane];
    if (pic->bpc <= 8u) {
        line[col] = (uint8_t)(value & peak);
    } else {
        ((uint16_t *)line)[col] = (uint16_t)(value & peak);
    }
}

/* A gradient with a frame-dependent phase for the reference; the distorted
 * frame adds a small periodic error. Chroma differs from luma so an
 * accidental chroma read shows (VIF is luma only). */
static inline int vif_twin_fill_picture(VmafPicture *pic, const VifTwinCase *c, unsigned frame,
                                        bool distorted)
{
    const int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, c->bpc, c->w, c->h);
    if (err)
        return err;
    const unsigned gain = 1u << (c->bpc - 8u);
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            unsigned value = (row + col + frame * 7u) * gain + (row * col) % gain;
            if (distorted)
                value += ((row * 2u + col + frame * 3u) % 13u) * gain;
            vif_twin_put_sample(pic, 0u, row, col, value);
        }
    }
    for (unsigned plane = 1; plane < 3; plane++) {
        for (unsigned row = 0; row < pic->h[plane]; row++) {
            for (unsigned col = 0; col < pic->w[plane]; col++) {
                vif_twin_put_sample(pic, plane, row, col,
                                    (row * 3u + col * 5u + plane * 17u + frame) * gain);
            }
        }
    }
    return 0;
}

static inline mu_message_t vif_twin_feed(VmafContext *vmaf, const VifTwinCase *c)
{
    for (unsigned frame = 0; frame < VIF_TWIN_FRAMES; frame++) {
        VmafPicture ref;
        VmafPicture dist;
        mu_assert("fill reference failed", !vif_twin_fill_picture(&ref, c, frame, false));
        mu_assert("fill distorted failed", !vif_twin_fill_picture(&dist, c, frame, true));
        mu_assert("vmaf_read_pictures failed", !vmaf_read_pictures(vmaf, &ref, &dist, frame));
    }
    mu_assert("vmaf_read_pictures(EOS) failed", !vmaf_read_pictures(vmaf, NULL, NULL, 0));
    return NULL;
}

static inline mu_message_t vif_twin_read_scores(VmafContext *vmaf, const VifTwinCase *c,
                                                VifTwinScores *out)
{
    for (unsigned frame = 0; frame < VIF_TWIN_FRAMES; frame++) {
        for (unsigned key = 0; key < c->n_keys; key++) {
            if (vmaf_feature_score_at_index(vmaf, c->keys[key], &out->v[frame][key], frame)) {
                (void)fprintf(stderr, "\n%s: no score for %s at frame %u\n", c->name, c->keys[key],
                              frame);
                return "vmaf_feature_score_at_index failed";
            }
        }
    }
    return NULL;
}

/* A context scoring float_vif on the CPU (`state` NULL) or on the twin, with
 * the case's options. */
static inline mu_message_t vif_twin_open_context(const VifTwin *twin, void *state,
                                                 const VifTwinCase *c, VmafContext **vmaf)
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    mu_assert("vmaf_init failed", !vmaf_init(vmaf, cfg));
    if (state)
        mu_assert("importing the device state failed", !twin->import(*vmaf, state));
    VmafFeatureDictionary *opts = NULL;
    for (unsigned i = 0; i < VIF_TWIN_MAX_OPTS && c->opts[i].key; i++) {
        mu_assert("vmaf_feature_dictionary_set failed",
                  !vmaf_feature_dictionary_set(&opts, c->opts[i].key, c->opts[i].value));
    }
    /* vmaf_use_feature() takes the dictionary over, on failure too. */
    mu_assert("vmaf_use_feature failed",
              !vmaf_use_feature(*vmaf, state ? twin->extractor : "float_vif", opts));
    return NULL;
}

/* Every output of every frame on the CPU (`state` NULL) or on the twin. */
static inline mu_message_t vif_twin_score(const VifTwin *twin, void *state, const VifTwinCase *c,
                                          VifTwinScores *out)
{
    VmafContext *vmaf = NULL;
    mu_message_t msg = vif_twin_open_context(twin, state, c, &vmaf);
    if (!msg)
        msg = vif_twin_feed(vmaf, c);
    if (!msg)
        msg = vif_twin_read_scores(vmaf, c, out);
    if (vmaf != NULL && vmaf_close(vmaf) != 0 && !msg)
        msg = "vmaf_close failed";
    return msg;
}

/* The bit pattern of a score: two scores are the same value, to the last bit
 * and including infinities, exactly when their patterns are equal. */
static inline uint64_t vif_twin_bits(double score)
{
    uint64_t bits = 0u;
    memcpy(&bits, &score, sizeof(bits));
    return bits;
}

/* Bit-for-bit comparison of every output; reports each one that differs. */
static inline mu_message_t vif_twin_require_identical(const VifTwin *twin, const VifTwinCase *c,
                                                      const VifTwinScores *cpu,
                                                      const VifTwinScores *gpu)
{
    unsigned differing = 0u;
    for (unsigned frame = 0; frame < VIF_TWIN_FRAMES; frame++) {
        for (unsigned key = 0; key < c->n_keys; key++) {
            const double a = cpu->v[frame][key];
            const double b = gpu->v[frame][key];
            mu_assert("the CPU float_vif output is not finite", isfinite(a));
            if (vif_twin_bits(a) == vif_twin_bits(b))
                continue;
            differing++;
            (void)fprintf(stderr, "\n%s frame %u %s: cpu=%.17g %s=%.17g delta=%.3e", c->name, frame,
                          c->keys[key], a, twin->backend, b, fabs(a - b));
        }
    }
    if (differing != 0u)
        (void)fprintf(stderr, "\n");
    mu_assert("the float_vif twin must return the CPU's float_vif outputs bit for bit",
              differing == 0u);
    return NULL;
}

/* One case on the CPU and on the twin, compared exactly. `cpu_out`, when not
 * NULL, receives the CPU's scores. */
static inline mu_message_t vif_twin_compare(const VifTwin *twin, const VifTwinCase *c,
                                            VifTwinScores *cpu_out)
{
    void *state = NULL;
    if (twin->open(&state) != 0 || state == NULL) {
        (void)fprintf(stderr, "[skip: no %s device] ", twin->backend);
        mu_skipped = 1;
        return NULL;
    }
    static VifTwinScores cpu;
    static VifTwinScores gpu;
    mu_message_t msg = vif_twin_score(twin, NULL, c, &cpu);
    if (!msg)
        msg = vif_twin_score(twin, state, c, &gpu);
    if (!msg)
        msg = vif_twin_require_identical(twin, c, &cpu, &gpu);
    if (cpu_out != NULL)
        *cpu_out = cpu;
    const int close_err = twin->close(state);
    if (!msg && close_err)
        msg = "closing the device state failed";
    return msg;
}

static inline mu_message_t vif_twin_registered(const VifTwin *twin)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name(twin->extractor);
    mu_assert("the float_vif twin must be registered", fex != NULL);
    mu_assert("the float_vif twin's name matches", !strcmp(fex->name, twin->extractor));
    return NULL;
}

static inline mu_message_t vif_twin_default_identical(const VifTwin *twin)
{
    static const VifTwinCase c = {
        .name = "default",
        .w = FIXTURE_W,
        .h = FIXTURE_H,
        .bpc = 8u,
        .n_keys = 4u,
        .keys = {VIF_TWIN_DEFAULT_KEYS},
    };
    return vif_twin_compare(twin, &c, NULL);
}

/* debug=true publishes the frame ratio and every per-scale sum the ratio is
 * formed from; the sums are the reference's fp32 accumulators. */
static inline mu_message_t vif_twin_debug_identical(const VifTwin *twin)
{
    static const VifTwinCase c = {
        .name = "debug",
        .w = FIXTURE_W,
        .h = FIXTURE_H,
        .bpc = 8u,
        .opts = {{"debug", "true"}},
        .n_keys = 15u,
        .keys = {VIF_TWIN_DEBUG_KEYS},
    };
    return vif_twin_compare(twin, &c, NULL);
}

/* ADR-1217: vif_enhn_gain_limit / vif_sigma_nsq must reach the kernel. The
 * values model/vmaf_float_v0.6.1neg.json ships (`vif_enhn_gain_limit = 1.0`)
 * plus a neural-noise variance that is not a power of two. Both are feature
 * parameters, so the score is filed under a derived key: alias base +
 * `_<alias>_<%g>` per option, sorted by option name. */
static inline mu_message_t vif_twin_model_options_identical(const VifTwin *twin)
{
    static const VifTwinCase c = {
        .name = "egl=1 snsq=1.5",
        .w = FIXTURE_W,
        .h = FIXTURE_H,
        .bpc = 8u,
        .opts = {{"vif_enhn_gain_limit", "1.0"}, {"vif_sigma_nsq", "1.5"}},
        .n_keys = 4u,
        .keys = {VIF_TWIN_SCALE_KEYS("_egl_1_snsq_1.5")},
    };
    return vif_twin_compare(twin, &c, NULL);
}

static inline mu_message_t vif_twin_skip_scale0_identical(const VifTwin *twin)
{
    static const VifTwinCase c = {
        .name = "vif_skip_scale0",
        .w = FIXTURE_W,
        .h = FIXTURE_H,
        .bpc = 8u,
        .opts = {{"vif_skip_scale0", "true"}},
        .n_keys = 4u,
        .keys = {VIF_TWIN_SCALE_KEYS("_ssclz")},
    };
    static VifTwinScores cpu;
    mu_assert_msg(vif_twin_compare(twin, &c, &cpu));
    if (!mu_skipped) {
        mu_assert("float_vif scale 0 must be exactly 0 with vif_skip_scale0=true",
                  cpu.v[1][0] == 0.0);
    }
    return NULL;
}

/* The per-scale floors of the CPU option table: a ratio below the floor is
 * published as the floor. 1.0 lifts every frame of scale 1; 0.5 leaves
 * scale 3 alone. */
static inline mu_message_t vif_twin_scale_minimums_identical(const VifTwin *twin)
{
    static const VifTwinCase c = {
        .name = "min_val",
        .w = FIXTURE_W,
        .h = FIXTURE_H,
        .bpc = 8u,
        .opts = {{"vif_scale1_min_val", "1.0"}, {"vif_scale3_min_val", "0.5"}},
        .n_keys = 4u,
        .keys = {VIF_TWIN_SCALE_KEYS("_s1miv_1_s3miv_0.5")},
    };
    static VifTwinScores cpu;
    mu_assert_msg(vif_twin_compare(twin, &c, &cpu));
    if (!mu_skipped) {
        mu_assert("vif_scale1_min_val=1.0 must publish 1.0 for scale 1", cpu.v[1][1] == 1.0);
    }
    return NULL;
}

static inline mu_message_t vif_twin_bit_depth_identical(const VifTwin *twin, unsigned bpc)
{
    const VifTwinCase c = {
        .name = "bpc",
        .w = FIXTURE_W,
        .h = FIXTURE_H,
        .bpc = bpc,
        .n_keys = 4u,
        .keys = {VIF_TWIN_DEFAULT_KEYS},
    };
    return vif_twin_compare(twin, &c, NULL);
}

static inline mu_message_t vif_twin_9bit_identical(const VifTwin *twin)
{
    return vif_twin_bit_depth_identical(twin, 9u);
}

static inline mu_message_t vif_twin_10bit_identical(const VifTwin *twin)
{
    return vif_twin_bit_depth_identical(twin, 10u);
}

static inline mu_message_t vif_twin_11bit_identical(const VifTwin *twin)
{
    return vif_twin_bit_depth_identical(twin, 11u);
}

static inline mu_message_t vif_twin_12bit_identical(const VifTwin *twin)
{
    return vif_twin_bit_depth_identical(twin, 12u);
}

static inline mu_message_t vif_twin_13bit_identical(const VifTwin *twin)
{
    return vif_twin_bit_depth_identical(twin, 13u);
}

static inline mu_message_t vif_twin_14bit_identical(const VifTwin *twin)
{
    return vif_twin_bit_depth_identical(twin, 14u);
}

static inline mu_message_t vif_twin_15bit_identical(const VifTwin *twin)
{
    return vif_twin_bit_depth_identical(twin, 15u);
}

static inline mu_message_t vif_twin_16bit_identical(const VifTwin *twin)
{
    return vif_twin_bit_depth_identical(twin, 16u);
}

/* 50x38 halves to 25x19, 12x9 and 6x4: no scale is a multiple of a 16x16
 * tile, and from scale 2 on the plane is smaller than one. */
static inline mu_message_t vif_twin_small_odd_frame_identical(const VifTwin *twin)
{
    static const VifTwinCase c = {
        .name = "50x38",
        .w = 50u,
        .h = 38u,
        .bpc = 8u,
        .opts = {{"debug", "true"}},
        .n_keys = 15u,
        .keys = {VIF_TWIN_DEBUG_KEYS},
    };
    return vif_twin_compare(twin, &c, NULL);
}

/* NOLINTEND(modernize-use-nullptr) */

#endif /* LIBVMAF_TEST_FLOAT_VIF_TWIN_PARITY_H_ */
