/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * ADR-1399 — the float_ssim_cuda decimation kernel against iqa_decimate().
 *
 * CPU float_ssim (ssim.c) low-passes both planes with a scale x scale box of
 * weight 1.0f / (scale * scale) and decimates them in place with
 * iqa_decimate() before SSIM. `calculate_ssim_decimate_{8,16}bpc` in
 * core/src/feature/cuda/integer_ssim/ssim_score.cu does the same on the
 * device: the fp32 product per tap, an exact int64 window sum in units of
 * 2^-52 and one round-to-nearest conversion.
 *
 * This test launches the kernel itself on synthetic luma planes, reads the
 * two decimated planes back and compares them byte for byte with
 * iqa_decimate() run the way ssim.c runs it. test_cuda_float_ssim_parity
 * covers the scores; this covers the planes, where a one-ulp slip would
 * mostly hide behind the SSIM windows.
 *
 * Cases: every scale from 2 to 10 at 8 bits, 10 / 12 / 16-bit samples, odd
 * widths and heights, a width whose last window centre lies outside the plane
 * (855 at scale 5, 172 * 5 > 855), and scales 16 and 128 (the exactness
 * bound, above the option range).
 *
 * Skip behaviour: emits "[skip: no CUDA device]" and passes when
 * vmaf_cuda_state_init fails.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"

#include "cuda/common.h"
#include "feature/iqa/convolve.h"
#include "feature/iqa/decimate.h"
#include "libvmaf/libvmaf_cuda.h"
#include "libvmaf/picture.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

extern const unsigned char ssim_score_ptx[];

#define BLOCK_X 16u
#define BLOCK_Y 8u
#define MAX_SCALE 128

typedef struct DecimateCase {
    unsigned w;
    unsigned h;
    unsigned bpc;
    int scale;
} DecimateCase;

/* Everything one case owns; released by release_planes(). */
typedef struct CasePlanes {
    uint16_t *samples[2]; /* host luma values, ref and cmp */
    void *raw[2];         /* the same luma as the picture stores it */
    float *cpu[2];        /* iqa_decimate() result, in place */
    float *gpu[2];        /* kernel result */
    VmafCudaBuffer *d_raw[2];
    VmafCudaBuffer *d_out[2];
    unsigned out_w;
    unsigned out_h;
} CasePlanes;

static unsigned sample_bytes(unsigned bpc)
{
    return bpc > 8u ? 2u : 1u;
}

/* picture_copy()'s divisor. */
static float sample_divisor(unsigned bpc)
{
    if (bpc == 10u)
        return 4.0f;
    if (bpc == 12u)
        return 16.0f;
    return bpc == 16u ? 256.0f : 1.0f;
}

/* Hashed samples over the whole range, so every low bit varies. */
static unsigned sample_at(unsigned row, unsigned col, unsigned bpc, unsigned salt)
{
    const unsigned hash = (row * 2654435761u) ^ (col * 40503u) ^ ((salt + 1u) * 2246822519u);
    return (hash >> 9) & ((1u << bpc) - 1u);
}

static void fill_samples(uint16_t *samples, const DecimateCase *dc, unsigned salt)
{
    for (unsigned row = 0; row < dc->h; row++) {
        for (unsigned col = 0; col < dc->w; col++) {
            samples[(size_t)row * dc->w + col] = (uint16_t)sample_at(row, col, dc->bpc, salt);
        }
    }
}

/* The luma plane as a picture of this depth stores it: one byte per sample
 * at 8 bits, native uint16 above. */
static void *pack_raw(const uint16_t *samples, const DecimateCase *dc)
{
    const size_t n = (size_t)dc->w * dc->h;
    if (dc->bpc > 8u) {
        uint16_t *raw = malloc(n * sizeof(*raw));
        if (raw)
            memcpy(raw, samples, n * sizeof(*raw));
        return raw;
    }
    uint8_t *raw = malloc(n);
    for (size_t i = 0; raw && i < n; i++)
        raw[i] = (uint8_t)samples[i];
    return raw;
}

/* The plane ssim.c hands iqa_decimate(): picture_copy()'s floats. */
static void to_float(float *dst, const uint16_t *samples, const DecimateCase *dc)
{
    const float divisor = sample_divisor(dc->bpc);
    const size_t n = (size_t)dc->w * dc->h;
    for (size_t i = 0; i < n; i++)
        dst[i] = (float)samples[i] / divisor;
}

static uint32_t float_bits(float value)
{
    uint32_t bits = 0;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

/* ssim.c::ssim_decimate_pair for one plane: the box kernel of
 * ssim_low_pass_alloc() and an in-place iqa_decimate(). */
static int cpu_decimate(float *plane, const DecimateCase *dc, int *rw, int *rh)
{
    static float taps[MAX_SCALE * MAX_SCALE];
    const float inv2 = 1.0f / (float)(dc->scale * dc->scale);
    for (int i = 0; i < dc->scale * dc->scale; i++)
        taps[i] = inv2;
    const struct iqa_kernel low_pass = {
        .kernel = taps,
        .w = dc->scale,
        .h = dc->scale,
        .normalized = 0,
        .bnd_opt = KBND_SYMMETRIC,
    };
    return iqa_decimate(plane, (int)dc->w, (int)dc->h, dc->scale, &low_pass, 0, rw, rh);
}

static int release_planes(VmafCudaState *cu_state, CasePlanes *p)
{
    int rc = 0;
    for (unsigned i = 0; i < 2u; i++) {
        free(p->samples[i]);
        free(p->raw[i]);
        free(p->cpu[i]);
        free(p->gpu[i]);
        int e = vmaf_cuda_buffer_free_owned(cu_state, &p->d_raw[i]);
        if (e && !rc)
            rc = e;
        e = vmaf_cuda_buffer_free_owned(cu_state, &p->d_out[i]);
        if (e && !rc)
            rc = e;
    }
    return rc;
}

/* Host planes and the CPU reference for both sides of one case. */
static int prepare_host(CasePlanes *p, const DecimateCase *dc)
{
    const size_t n = (size_t)dc->w * dc->h;
    for (unsigned i = 0; i < 2u; i++) {
        p->samples[i] = malloc(n * sizeof(uint16_t));
        p->cpu[i] = malloc(n * sizeof(float));
        if (!p->samples[i] || !p->cpu[i])
            return -ENOMEM;
        fill_samples(p->samples[i], dc, i);
        p->raw[i] = pack_raw(p->samples[i], dc);
        if (!p->raw[i])
            return -ENOMEM;
        to_float(p->cpu[i], p->samples[i], dc);
        int rw = 0;
        int rh = 0;
        if (cpu_decimate(p->cpu[i], dc, &rw, &rh))
            return -EINVAL;
        p->out_w = (unsigned)rw;
        p->out_h = (unsigned)rh;
    }
    return 0;
}

/* Device planes: the raw luma uploaded, the outputs allocated. */
static int prepare_device(VmafCudaState *cu_state, CasePlanes *p, const DecimateCase *dc)
{
    const size_t raw_bytes = (size_t)dc->w * dc->h * sample_bytes(dc->bpc);
    const size_t out_bytes = (size_t)p->out_w * p->out_h * sizeof(float);
    for (unsigned i = 0; i < 2u; i++) {
        p->gpu[i] = malloc(out_bytes);
        if (!p->gpu[i])
            return -ENOMEM;
        int err = vmaf_cuda_buffer_alloc(cu_state, &p->d_raw[i], raw_bytes);
        if (!err)
            err = vmaf_cuda_buffer_alloc(cu_state, &p->d_out[i], out_bytes);
        if (!err)
            err = vmaf_cuda_buffer_upload_async(cu_state, p->d_raw[i], p->raw[i], 0);
        if (err)
            return err;
    }
    return 0;
}

/* Launches the decimation kernel with the argument list of
 * integer_ssim_cuda.c::float_ssim_launch_decimate. The kernel reads data[0]
 * and stride[0] of each picture, nothing else. */
static int launch_decimate(VmafCudaState *cu_state, CUfunction func, CasePlanes *p,
                           const DecimateCase *dc)
{
    CudaFunctions *cu_f = cu_state->f;
    VmafPicture pics[2];
    memset(pics, 0, sizeof(pics));
    for (unsigned i = 0; i < 2u; i++) {
        // NOLINTNEXTLINE(performance-no-int-to-ptr): Driver API device address the kernel dereferences (ADR-0747)
        pics[i].data[0] = (void *)p->d_raw[i]->data;
        pics[i].stride[0] = (ptrdiff_t)dc->w * sample_bytes(dc->bpc);
    }
    int width = (int)dc->w;
    int height = (int)dc->h;
    int scale = dc->scale;
    float sample_scale = 1.0f / sample_divisor(dc->bpc);
    float tap_weight = 1.0f / (float)(dc->scale * dc->scale);
    void *params[] = {
        &pics[0],  &pics[1],  p->d_out[0], p->d_out[1],   &width,      &height,
        &p->out_w, &p->out_h, &scale,      &sample_scale, &tap_weight,
    };
    const unsigned grid_x = (p->out_w + BLOCK_X - 1u) / BLOCK_X;
    const unsigned grid_y = (p->out_h + BLOCK_Y - 1u) / BLOCK_Y;
    CHECK_CUDA_RETURN(cu_f, cuCtxPushCurrent(cu_state->ctx));
    const CUresult launch = cu_f->cuLaunchKernel(func, grid_x, grid_y, 1, BLOCK_X, BLOCK_Y, 1, 0,
                                                 cu_state->str, params, NULL);
    CHECK_CUDA_RETURN(cu_f, cuCtxPopCurrent(NULL));
    return launch == CUDA_SUCCESS ? 0 : -EIO;
}

static int read_back(VmafCudaState *cu_state, CasePlanes *p)
{
    for (unsigned i = 0; i < 2u; i++) {
        const int err = vmaf_cuda_buffer_download_async(cu_state, p->d_out[i], p->gpu[i], 0);
        if (err)
            return err;
    }
    return vmaf_cuda_sync(cu_state);
}

/* Number of samples that differ between the CPU and device planes, or a
 * negative errno. */
static long run_case(VmafCudaState *cu_state, CUfunction func, const DecimateCase *dc)
{
    CasePlanes p;
    memset(&p, 0, sizeof(p));
    int err = prepare_host(&p, dc);
    if (!err)
        err = prepare_device(cu_state, &p, dc);
    if (!err)
        err = launch_decimate(cu_state, func, &p, dc);
    if (!err)
        err = read_back(cu_state, &p);
    long mismatches = 0;
    const size_t n = (size_t)p.out_w * p.out_h;
    for (unsigned i = 0; i < 2u && !err; i++) {
        for (size_t k = 0; k < n; k++)
            mismatches += float_bits(p.cpu[i][k]) != float_bits(p.gpu[i][k]);
    }
    const int release_err = release_planes(cu_state, &p);
    if (err || release_err)
        return err ? err : release_err;
    return mismatches;
}

static int load_kernels(VmafCudaState *cu_state, CUmodule *module, CUfunction *func_8,
                        CUfunction *func_16)
{
    CudaFunctions *cu_f = cu_state->f;
    CHECK_CUDA_RETURN(cu_f, cuCtxPushCurrent(cu_state->ctx));
    CUresult res = cu_f->cuModuleLoadData(module, ssim_score_ptx);
    if (res == CUDA_SUCCESS)
        res = cu_f->cuModuleGetFunction(func_8, *module, "calculate_ssim_decimate_8bpc");
    if (res == CUDA_SUCCESS)
        res = cu_f->cuModuleGetFunction(func_16, *module, "calculate_ssim_decimate_16bpc");
    CHECK_CUDA_RETURN(cu_f, cuCtxPopCurrent(NULL));
    return res == CUDA_SUCCESS ? 0 : -EIO;
}

static const DecimateCase decimate_cases[] = {
    {320u, 180u, 8u, 2},  {320u, 180u, 8u, 3},      {320u, 180u, 8u, 4},   {321u, 181u, 8u, 5},
    {320u, 180u, 8u, 6},  {323u, 177u, 8u, 7},      {320u, 180u, 8u, 8},   {321u, 180u, 8u, 9},
    {320u, 181u, 8u, 10}, {855u, 481u, 8u, 5},      {400u, 224u, 10u, 3},  {401u, 225u, 10u, 6},
    {400u, 224u, 12u, 5}, {400u, 224u, 16u, 7},     {401u, 223u, 16u, 10}, {640u, 360u, 16u, 16},
    {640u, 360u, 8u, 16}, {1408u, 1408u, 16u, 128},
};

static char *check_cases(VmafCudaState *cu_state, CUfunction func_8, CUfunction func_16)
{
    for (size_t i = 0; i < sizeof(decimate_cases) / sizeof(decimate_cases[0]); i++) {
        const DecimateCase *dc = &decimate_cases[i];
        const long mismatches = run_case(cu_state, dc->bpc > 8u ? func_16 : func_8, dc);
        if (mismatches) {
            (void)fprintf(stderr, "\n%ux%u %u-bit scale=%d: %ld (mismatching samples or -errno)\n",
                          dc->w, dc->h, dc->bpc, dc->scale, mismatches);
        }
        mu_assert("the decimated planes must equal iqa_decimate() byte for byte", !mismatches);
    }
    return NULL;
}

static char *test_decimated_planes_equal_iqa_decimate(void)
{
    VmafCudaState *cu_state = NULL;
    VmafCudaConfiguration cuda_cfg = {0};
    if (vmaf_cuda_state_init(&cu_state, cuda_cfg) != 0 || !cu_state) {
        (void)fprintf(stderr, "[skip: no CUDA device] ");
        return NULL;
    }
    CUmodule module = NULL;
    CUfunction func_8 = NULL;
    CUfunction func_16 = NULL;
    const int load_err = load_kernels(cu_state, &module, &func_8, &func_16);
    char *msg =
        load_err ? "loading the float_ssim kernels failed" : check_cases(cu_state, func_8, func_16);
    const int unload_err = vmaf_cuda_module_unload(cu_state, &module);
    const int release_err = vmaf_cuda_state_free(cu_state);
    if (msg)
        return msg;
    mu_assert("unloading the module failed", !unload_err);
    mu_assert("releasing the CUDA state failed", !release_err);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_decimated_planes_equal_iqa_decimate);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
