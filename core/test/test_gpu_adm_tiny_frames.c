/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Integer-ADM GPU twins against the scalar CPU on tiny frames and on
 * full-range content.
 *
 * Tiny frames (T-GPU-ADM-TINY-FRAME-SHIFT-2026-09-18): frames 17 to 32 pixels
 * wide give scale-0 bands of 9 to 16 samples, where the horizontal and
 * vertical cube shift is exactly 0. The CUDA and HIP host code computed that
 * shift's rounding constant as 1 << (shift - 1), which is 2^31 on x86: 32x32
 * frames scored NaN. Their scale-0 contrast-masking kernels also read one
 * column and one row past the band at the right and bottom edges, which only
 * fall inside the evaluated region for bands of 14 samples or less. The GPU
 * extractors also accepted frames below the 17x17 minimum the CPU extractor
 * enforces.
 *
 * Full-range content (T-SYCL-ADM-INT16-SEMANTICS-2026-09-18): the CPU stores
 * the scale-0 bands as int16_t, so large values wrap. Independent 8-bit noise
 * in the reference and the distorted picture reaches the wrap in the
 * contrast-masking threshold. The SYCL twin computed in 32 and 64 bits and
 * never wrapped, and integer_adm_scale0 came out 2.1e-4 off at 576x324.
 * Smooth content such as the tiny-frame ramp never gets there.
 *
 * The rejection test calls init() directly and needs no device: the size
 * check runs before any device resource is touched. The parity tests score
 * each geometry on the GPU twin and on the scalar CPU path, and skip when the
 * backend has no device.
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
#include "libvmaf/picture.h"

#if defined(HAVE_CUDA)
#include "libvmaf/libvmaf_cuda.h"
#define GPU_FEATURE "adm_cuda"
#elif defined(HAVE_HIP)
#include "libvmaf/libvmaf_hip.h"
#define GPU_FEATURE "adm_hip"
#elif defined(HAVE_SYCL)
#include "libvmaf/libvmaf_sycl.h"
#define GPU_FEATURE "adm_sycl"
#endif

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe, and this file mirrors
 * the C spelling of the surface it exercises. ADR-1138. */

/* ADR-0214 cross-backend gate (places=4). */
#define PARITY_TOL 1e-4

typedef struct {
    unsigned w;
    unsigned h;
} Geometry;

/* One dimension from 17 to 32 in every row, so the scale-0 band is 9 to 16
 * samples wide or high; 32x32 is the size that scored NaN. */
static const Geometry ACCEPTED[] = {
    {17u, 17u}, {18u, 18u}, {24u, 24u}, {32u, 32u}, {20u, 64u}, {64u, 20u}, {31u, 48u},
};
#define NUM_ACCEPTED (sizeof(ACCEPTED) / sizeof(ACCEPTED[0]))

/* Full-range noise: a small frame and the Netflix fixture size. */
static const Geometry NOISE[] = {
    {96u, 64u},
    {576u, 324u},
};
#define NUM_NOISE (sizeof(NOISE) / sizeof(NOISE[0]))

static const Geometry REJECTED[] = {
    {8u, 8u},
    {16u, 16u},
    {16u, 17u},
    {17u, 16u},
};
#define NUM_REJECTED (sizeof(REJECTED) / sizeof(REJECTED[0]))

static const char *const SCORE_KEYS[] = {
    "integer_adm_scale0",
    "integer_adm_scale1",
    "integer_adm_scale2",
    "integer_adm_scale3",
    "VMAF_integer_feature_adm2_score",
};
#define NUM_KEYS (sizeof(SCORE_KEYS) / sizeof(SCORE_KEYS[0]))

/* Luma sample at (row, col) of the reference (distorted == 0) or the
 * distorted picture. */
typedef uint8_t (*SampleFn)(unsigned row, unsigned col, int distorted);

/* The same ramp and periodic error as test_integer_adm_tiny_frames.c. */
static uint8_t ramp_sample(unsigned row, unsigned col, int distorted)
{
    unsigned v = (row * 5u + col * 3u) & 0xFFu;
    if (distorted) {
        v = (v + ((row * 7u + col) % 17u)) & 0xFFu;
    }
    return (uint8_t)v;
}

/* Full-range 8-bit noise, independent between the two pictures: the top byte
 * of a 32-bit integer hash (lowbias32) of the position, seeded per picture.
 * Stateless, so every backend sees the same frames. */
static uint8_t noise_sample(unsigned row, unsigned col, int distorted)
{
    uint32_t x = ((uint32_t)row << 16) ^ (uint32_t)col ^ (distorted ? 0x9E3779B9u : 0x85EBCA6Bu);
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return (uint8_t)(x >> 24);
}

static int fill_picture(VmafPicture *pic, Geometry g, SampleFn sample, int distorted)
{
    const int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, 8u, g.w, g.h);
    if (err) {
        return err;
    }
    uint8_t *luma = (uint8_t *)pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            luma[(row * pic->stride[0]) + col] = sample(row, col, distorted);
        }
    }
    for (unsigned p = 1; p < 3; p++) {
        uint8_t *plane = (uint8_t *)pic->data[p];
        for (unsigned row = 0; row < pic->h[p]; row++) {
            (void)memset(plane + (row * pic->stride[p]), 128, pic->w[p]);
        }
    }
    return 0;
}

/* Feed one frame of `g` and read every score. `*skipped` is set when the
 * backend reports its kernels were not built (-ENOSYS). */
static char *score_frame(VmafContext *vmaf, Geometry g, SampleFn sample, double out[NUM_KEYS],
                         int *skipped)
{
    VmafPicture ref;
    VmafPicture dist;
    mu_assert("reference picture allocation failed", !fill_picture(&ref, g, sample, 0));
    if (fill_picture(&dist, g, sample, 1)) {
        (void)vmaf_picture_unref(&ref);
        return "distorted picture allocation failed";
    }
    const int err = vmaf_read_pictures(vmaf, &ref, &dist, 0u);
    if (err == -ENOSYS) {
        *skipped = 1;
        return NULL;
    }
    mu_assert("vmaf_read_pictures failed", !err);
    mu_assert("vmaf_read_pictures(EOS) failed", !vmaf_read_pictures(vmaf, NULL, NULL, 0));
    for (size_t k = 0; k < NUM_KEYS; k++) {
        mu_assert("ADM score missing",
                  !vmaf_feature_score_at_index(vmaf, SCORE_KEYS[k], &out[k], 0u));
    }
    return NULL;
}

static char *score_cpu_scalar(Geometry g, SampleFn sample, double out[NUM_KEYS])
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE, .cpumask = ~(uint64_t)0};
    VmafContext *vmaf = NULL;
    mu_assert("CPU: vmaf_init failed", !vmaf_init(&vmaf, cfg));
    mu_assert("CPU: vmaf_use_feature(adm) failed", !vmaf_use_feature(vmaf, "adm", NULL));
    int skipped = 0;
    char *msg = score_frame(vmaf, g, sample, out, &skipped);
    (void)vmaf_close(vmaf);
    return msg;
}

#if defined(HAVE_CUDA)
typedef VmafCudaState GpuState;

static int gpu_open(GpuState **state)
{
    VmafCudaConfiguration cfg = {0};
    return vmaf_cuda_state_init(state, cfg);
}

static int gpu_import(VmafContext *vmaf, GpuState *state)
{
    return vmaf_cuda_import_state(vmaf, state);
}

static void gpu_free(GpuState **state)
{
    (void)vmaf_cuda_state_free(*state);
    *state = NULL;
}
#elif defined(HAVE_HIP)
typedef VmafHipState GpuState;

static int gpu_open(GpuState **state)
{
    VmafHipConfiguration cfg = {0};
    return vmaf_hip_state_init(state, cfg);
}

static int gpu_import(VmafContext *vmaf, GpuState *state)
{
    return vmaf_hip_import_state(vmaf, state);
}

static void gpu_free(GpuState **state)
{
    vmaf_hip_state_free(state);
}
#elif defined(HAVE_SYCL)
typedef VmafSyclState GpuState;

static int gpu_open(GpuState **state)
{
    VmafSyclConfiguration cfg = {.device_index = -1};
    return vmaf_sycl_state_init(state, cfg);
}

static int gpu_import(VmafContext *vmaf, GpuState *state)
{
    return vmaf_sycl_import_state(vmaf, state);
}

static void gpu_free(GpuState **state)
{
    vmaf_sycl_state_free(state);
}
#endif

/* Score `g` on the GPU twin. `*skipped` is set when there is no device or the
 * kernels were not built. */
static char *score_gpu(Geometry g, SampleFn sample, double out[NUM_KEYS], int *skipped)
{
    GpuState *state = NULL;
    if (gpu_open(&state) != 0 || state == NULL) {
        *skipped = 1;
        return NULL;
    }
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    char *msg = NULL;
    if (vmaf_init(&vmaf, cfg)) {
        msg = "GPU: vmaf_init failed";
    } else if (gpu_import(vmaf, state)) {
        msg = "GPU: importing the device state failed";
    } else if (vmaf_use_feature(vmaf, GPU_FEATURE, NULL)) {
        msg = "GPU: vmaf_use_feature failed";
    } else {
        msg = score_frame(vmaf, g, sample, out, skipped);
    }
    if (vmaf) {
        (void)vmaf_close(vmaf);
    }
    gpu_free(&state);
    return msg;
}

/* One content family of the parity tests. */
typedef struct {
    SampleFn sample;
    char *mismatch; /* failure message */
} Content;

static const Content TINY_RAMP = {
    ramp_sample,
    "GPU integer ADM differs from scalar CPU by more than 1e-4 on a tiny frame",
};

static const Content FULL_RANGE_NOISE = {
    noise_sample,
    "GPU integer ADM differs from scalar CPU by more than 1e-4 on full-range noise",
};

static char *check_parity(Geometry g, const Content *c, int *skipped)
{
    double cpu[NUM_KEYS];
    double gpu[NUM_KEYS];
    char *msg = score_cpu_scalar(g, c->sample, cpu);
    if (msg) {
        return msg;
    }
    msg = score_gpu(g, c->sample, gpu, skipped);
    if (msg || *skipped) {
        return msg;
    }
    for (size_t k = 0; k < NUM_KEYS; k++) {
        const double delta = fabs(cpu[k] - gpu[k]);
        if (!(delta <= PARITY_TOL)) {
            (void)fprintf(stderr, "\n  %ux%u %s: cpu=%.8f gpu=%.8f delta=%.2e\n", g.w, g.h,
                          SCORE_KEYS[k], cpu[k], gpu[k], delta);
            return c->mismatch;
        }
    }
    return NULL;
}

/* Parity on every geometry of `list`; a missing device skips the test. */
static char *check_parity_list(const Geometry *list, size_t count, const Content *c)
{
    for (size_t i = 0; i < count; i++) {
        int skipped = 0;
        char *msg = check_parity(list[i], c, &skipped);
        if (msg) {
            return msg;
        }
        if (skipped) {
            (void)fprintf(stderr, "[skip: no %s device or kernels] ", GPU_FEATURE);
            mu_skipped = 1;
            return NULL;
        }
    }
    return NULL;
}

static char *test_gpu_adm_tiny_frame_parity(void)
{
    return check_parity_list(ACCEPTED, NUM_ACCEPTED, &TINY_RAMP);
}

static char *test_gpu_adm_full_range_noise_parity(void)
{
    return check_parity_list(NOISE, NUM_NOISE, &FULL_RANGE_NOISE);
}

/* init() with a zeroed private state. Only rejected sizes come through here:
 * they return before init() touches the device state or the options, so
 * nothing needs closing. */
static int init_rejects(VmafFeatureExtractor *fex, Geometry g)
{
    void *priv = calloc(1, fex->priv_size);
    if (!priv) {
        return 0;
    }
    fex->priv = priv;
    const int rc = fex->init(fex, VMAF_PIX_FMT_YUV420P, 8u, g.w, g.h);
    free(priv);
    fex->priv = NULL;
    return rc == -EINVAL;
}

static char *test_gpu_adm_rejects_below_min_dim(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name(GPU_FEATURE);
    mu_assert("GPU integer ADM extractor is not registered", fex != NULL);
    for (size_t i = 0; i < NUM_REJECTED; i++) {
        if (!init_rejects(fex, REJECTED[i])) {
            (void)fprintf(stderr, "\n  %ux%u accepted\n", REJECTED[i].w, REJECTED[i].h);
            return "GPU integer ADM must reject frames below 17x17 with -EINVAL";
        }
    }
    return NULL;
}

char *run_tests(void)
{
    static const MuTest tests[] = {
        MU_TEST(test_gpu_adm_rejects_below_min_dim),
        MU_TEST(test_gpu_adm_tiny_frame_parity),
        MU_TEST(test_gpu_adm_full_range_noise_parity),
    };
    return mu_run_table(tests, MU_TABLE_LEN(tests));
}

/* NOLINTEND(modernize-use-nullptr) */
