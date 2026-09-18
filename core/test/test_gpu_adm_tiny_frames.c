/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Integer-ADM GPU twins on tiny frames (T-GPU-ADM-TINY-FRAME-SHIFT-2026-09-18).
 *
 * Frames 17 to 32 pixels wide give scale-0 bands of 9 to 16 samples, where the
 * horizontal and vertical cube shift is exactly 0. The CUDA and HIP host code
 * computed that shift's rounding constant as 1 << (shift - 1), which is 2^31
 * on x86: 32x32 frames scored NaN. Their scale-0 contrast-masking kernels also
 * read one column and one row past the band at the right and bottom edges,
 * which only fall inside the evaluated region for bands of 14 samples or less.
 * The GPU extractors also accepted frames below the 17x17 minimum the CPU
 * extractor enforces.
 *
 * The rejection test calls init() directly and needs no device: the size
 * check runs before any device resource is touched. The parity test scores
 * each geometry on the GPU twin and on the scalar CPU path, and skips when the
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

/* The same ramp and periodic error as test_integer_adm_tiny_frames.c. */
static uint8_t sample(unsigned row, unsigned col, int distorted)
{
    unsigned v = (row * 5u + col * 3u) & 0xFFu;
    if (distorted) {
        v = (v + ((row * 7u + col) % 17u)) & 0xFFu;
    }
    return (uint8_t)v;
}

static int fill_picture(VmafPicture *pic, Geometry g, int distorted)
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
static char *score_frame(VmafContext *vmaf, Geometry g, double out[NUM_KEYS], int *skipped)
{
    VmafPicture ref;
    VmafPicture dist;
    mu_assert("reference picture allocation failed", !fill_picture(&ref, g, 0));
    if (fill_picture(&dist, g, 1)) {
        (void)vmaf_picture_unref(&ref);
        return "distorted picture allocation failed";
    }
    const int err = vmaf_read_pictures(vmaf, &ref, &dist, 0u);
    if (err == -ENOSYS) {
        *skipped = 1;
        return NULL;
    }
    mu_assert("vmaf_read_pictures failed on a tiny frame", !err);
    mu_assert("vmaf_read_pictures(EOS) failed", !vmaf_read_pictures(vmaf, NULL, NULL, 0));
    for (size_t k = 0; k < NUM_KEYS; k++) {
        mu_assert("ADM score missing",
                  !vmaf_feature_score_at_index(vmaf, SCORE_KEYS[k], &out[k], 0u));
    }
    return NULL;
}

static char *score_cpu_scalar(Geometry g, double out[NUM_KEYS])
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE, .cpumask = ~(uint64_t)0};
    VmafContext *vmaf = NULL;
    mu_assert("CPU: vmaf_init failed", !vmaf_init(&vmaf, cfg));
    mu_assert("CPU: vmaf_use_feature(adm) failed", !vmaf_use_feature(vmaf, "adm", NULL));
    int skipped = 0;
    char *msg = score_frame(vmaf, g, out, &skipped);
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
static char *score_gpu(Geometry g, double out[NUM_KEYS], int *skipped)
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
        msg = score_frame(vmaf, g, out, skipped);
    }
    if (vmaf) {
        (void)vmaf_close(vmaf);
    }
    gpu_free(&state);
    return msg;
}

static char *check_parity(Geometry g, int *skipped)
{
    double cpu[NUM_KEYS];
    double gpu[NUM_KEYS];
    char *msg = score_cpu_scalar(g, cpu);
    if (msg) {
        return msg;
    }
    msg = score_gpu(g, gpu, skipped);
    if (msg || *skipped) {
        return msg;
    }
    for (size_t k = 0; k < NUM_KEYS; k++) {
        const double delta = fabs(cpu[k] - gpu[k]);
        if (!(delta <= PARITY_TOL)) {
            (void)fprintf(stderr, "\n  %ux%u %s: cpu=%.8f gpu=%.8f delta=%.2e\n", g.w, g.h,
                          SCORE_KEYS[k], cpu[k], gpu[k], delta);
            return "GPU integer ADM differs from scalar CPU by more than 1e-4 on a tiny frame";
        }
    }
    return NULL;
}

static char *test_gpu_adm_tiny_frame_parity(void)
{
    for (size_t i = 0; i < NUM_ACCEPTED; i++) {
        int skipped = 0;
        char *msg = check_parity(ACCEPTED[i], &skipped);
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
    };
    return mu_run_table(tests, MU_TABLE_LEN(tests));
}

/* NOLINTEND(modernize-use-nullptr) */
