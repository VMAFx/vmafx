/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Host orchestration of the device-resident CUDA SpEED pipeline (ADR-1380);
 *  see speed_cuda_pipeline.h for the per-frame protocol and
 *  speed/speed_score.cu for the kernels and their numerical contract.
 *
 *  Every per-frame call here only enqueues work: device-to-device staging
 *  copies, the kernel chain, an event, and one device-to-host copy of the
 *  40-byte SpeedGpuFrameResult into pinned memory. The only host wait is
 *  speed_cuda_pipeline_collect() / _wait(), once per frame. No host code
 *  touches pixel, covariance or score data.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cuda/common.h"
#include "cuda/kernel_template.h"
#include "cuda_helper.cuh"
#include "feature/cuda/speed/speed_cuda_params.h"
#include "feature/cuda/speed_cuda_pipeline.h"
#include "feature/speed_internal.h"
#include "log.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

/* PTX/fatbin of speed/speed_score.cu, embedded by bin2c at build time. */
extern const unsigned char speed_score_ptx[];

#define SPEED_CUDA_N 25u         /* elements_in_block */
#define SPEED_CUDA_MATRIX 625u   /* 25 x 25 */
#define SPEED_CUDA_TRIANGLE 325u /* lower triangle incl. diagonal */

typedef enum {
    SPEED_FN_SCALE,
    SPEED_FN_DECIMATE_RAW,
    SPEED_FN_DECIMATE_SCALED,
    SPEED_FN_CENTRE,
    SPEED_FN_MEANS,
    SPEED_FN_COVARIANCE,
    SPEED_FN_LINALG,
    SPEED_FN_SOLVE,
    SPEED_FN_SCORE,
    SPEED_FN_COUNT,
} SpeedCudaKernel;

static const char *const speed_kernel_names[SPEED_FN_COUNT] = {
    "speed_scale_kernel",  "speed_decimate_raw_kernel", "speed_decimate_scaled_kernel",
    "speed_centre_kernel", "speed_means_kernel",        "speed_covariance_kernel",
    "speed_linalg_kernel", "speed_solve_kernel",        "speed_score_kernel",
};

/* Device buffers, one table so allocation and release walk the same list. */
typedef enum {
    SPEED_BUF_RAW,
    SPEED_BUF_TAPS,
    SPEED_BUF_SCALED,
    SPEED_BUF_DOWN,
    SPEED_BUF_CENTERED,
    SPEED_BUF_INDTERM,
    SPEED_BUF_MEANS,
    SPEED_BUF_COV,
    SPEED_BUF_EIG,
    SPEED_BUF_QMAT,
    SPEED_BUF_RMAT,
    SPEED_BUF_STATUS,
    SPEED_BUF_VAR,
    SPEED_BUF_ENT,
    SPEED_BUF_CONTRIB,
    SPEED_BUF_RESULT,
    SPEED_BUF_COUNT,
} SpeedCudaBufferId;

struct SpeedCudaPipeline {
    VmafCudaState *cu_state;
    VmafCudaKernelLifecycle lc; /* private readback stream + fences */
    CUmodule module;
    CUfunction fn[SPEED_FN_COUNT];
    VmafCudaBuffer *buf[SPEED_BUF_COUNT];
    SpeedGpuFrameResult *h_result; /* pinned */
    SpeedGpuConfig config;
    uint32_t channels;
    uint32_t raw_planes;
    uint32_t cov_threads;
    size_t plane_bytes;
};

/* ------------------------------------------------------------------ */
/* Configuration and sizes                                             */
/* ------------------------------------------------------------------ */

static bool speed_in_range(uint32_t value, uint32_t low, uint32_t high)
{
    return value >= low && value <= high;
}

/* The shapes the kernels index with; speed_internal_init_dimensions()
 * produces nothing else, this only guards the contract. */
static bool speed_config_valid(const SpeedGpuConfig *c, uint32_t channels, uint32_t raw_planes)
{
    const SpeedGpuGeometry *g = &c->geometry;
    const bool dims = g->blocks > 0u && g->sub_w > 0u && g->sub_h > 0u;
    const bool nested = g->down_w >= g->trunc_w && g->down_h >= g->trunc_h;
    const bool sample = g->bytes_per_sample == 1u || g->bytes_per_sample == 2u;
    const bool taps = speed_in_range(c->filters.antialias_width, 1u, SPEED_GPU_MAX_TAPS) &&
                      speed_in_range(c->filters.lowpass_width, 1u, SPEED_GPU_MAX_TAPS);
    const bool mode = c->scoring.weight_mode >= 0 && c->scoring.weight_mode <= 6;
    const bool lanes = (channels == 2u || channels == 4u) &&
                       speed_in_range(raw_planes, 1u, SPEED_GPU_MAX_RAW_PLANES);
    return dims && nested && sample && taps && mode && lanes;
}

/* Enough threads per covariance entry to cover the submatrix, no more
 * (speed_sycl_pipeline.cpp covariance_group_size()). */
static uint32_t speed_covariance_threads(uint32_t terms)
{
    uint32_t threads = SPEED_CUDA_COV_MIN_THREADS;
    for (int step = 0; step < 3 && threads < SPEED_CUDA_COV_MAX_THREADS && threads < terms / 8u;
         step++)
        threads *= 2u;
    return threads;
}

static size_t speed_buffer_bytes(const SpeedCudaPipeline *p, SpeedCudaBufferId id)
{
    const SpeedGpuGeometry *g = &p->config.geometry;
    const size_t ch = p->channels;
    const size_t f = sizeof(float);
    switch (id) {
    case SPEED_BUF_RAW:
        return p->plane_bytes * p->raw_planes;
    case SPEED_BUF_TAPS:
        return (size_t)2u * SPEED_GPU_MAX_TAPS * f;
    case SPEED_BUF_SCALED:
        return g->prescale ? ch * g->scaled_w * g->scaled_h * f : 0u;
    case SPEED_BUF_DOWN:
        return ch * g->down_w * g->down_h * f;
    case SPEED_BUF_CENTERED:
        return ch * g->trunc_w * g->trunc_h * f;
    case SPEED_BUF_INDTERM:
        return ch * SPEED_CUDA_N * g->blocks * f;
    case SPEED_BUF_MEANS:
    case SPEED_BUF_EIG:
        return ch * SPEED_CUDA_N * f;
    case SPEED_BUF_COV:
    case SPEED_BUF_QMAT:
    case SPEED_BUF_RMAT:
        return ch * SPEED_CUDA_MATRIX * f;
    case SPEED_BUF_STATUS:
        return ch * 2u * sizeof(int32_t);
    case SPEED_BUF_VAR:
    case SPEED_BUF_ENT:
        return ch * g->blocks * f;
    case SPEED_BUF_CONTRIB:
        return (ch / 2u) * g->blocks * f;
    case SPEED_BUF_RESULT:
        return sizeof(SpeedGpuFrameResult);
    default:
        return 0u;
    }
}

/* ------------------------------------------------------------------ */
/* Setup and teardown                                                  */
/* ------------------------------------------------------------------ */

static int speed_preserve(int rc, int err)
{
    return rc ? rc : err;
}

int speed_cuda_pipeline_close(SpeedCudaPipeline **pipeline)
{
    if (!pipeline || !*pipeline)
        return 0;
    SpeedCudaPipeline *p = *pipeline;
    /* Quiesce the private stream before anything it may reference goes. */
    int rc = vmaf_cuda_kernel_lifecycle_close(&p->lc, p->cu_state);
    if (rc)
        return rc;
    for (int id = 0; id < SPEED_BUF_COUNT; id++)
        rc = speed_preserve(rc, vmaf_cuda_buffer_free_owned(p->cu_state, &p->buf[id]));
    rc = speed_preserve(rc, vmaf_cuda_buffer_host_free_owned(p->cu_state, (void **)&p->h_result));
    rc = speed_preserve(rc, vmaf_cuda_module_unload(p->cu_state, &p->module));
    if (rc)
        return rc; /* keep what could not be released for a retry */
    free(p);
    *pipeline = NULL;
    return 0;
}

/* Run `body` with the pipeline's context current and pop it on every path;
 * the body's error wins over a failed pop. */
static int speed_in_context(SpeedCudaPipeline *p, int (*body)(SpeedCudaPipeline *))
{
    CudaFunctions *cu_f = p->cu_state->f;
    CHECK_CUDA_RETURN(cu_f, cuCtxPushCurrent(p->cu_state->ctx));
    const int err = body(p);
    const CUresult pop = cu_f->cuCtxPopCurrent(NULL);
    return err ? err : vmaf_cuda_result_to_errno((int)pop);
}

static int speed_load_kernels(SpeedCudaPipeline *p)
{
    CudaFunctions *cu_f = p->cu_state->f;
    CHECK_CUDA_RETURN(cu_f, cuModuleLoadData(&p->module, speed_score_ptx));
    for (int k = 0; k < SPEED_FN_COUNT; k++)
        CHECK_CUDA_RETURN(cu_f, cuModuleGetFunction(&p->fn[k], p->module, speed_kernel_names[k]));
    return 0;
}

/* The filter taps: antialias in [0, 128), lowpass in [128, 256). Init only,
 * so the copy may be synchronous. */
static int speed_upload_taps(SpeedCudaPipeline *p)
{
    float host[2u * SPEED_GPU_MAX_TAPS];
    memcpy(host, p->config.filters.antialias, sizeof(float) * SPEED_GPU_MAX_TAPS);
    memcpy(host + SPEED_GPU_MAX_TAPS, p->config.filters.lowpass,
           sizeof(float) * SPEED_GPU_MAX_TAPS);
    CHECK_CUDA_RETURN(p->cu_state->f,
                      cuMemcpyHtoD(p->buf[SPEED_BUF_TAPS]->data, host, sizeof(host)));
    return 0;
}

static int speed_allocate(SpeedCudaPipeline *p)
{
    for (int id = 0; id < SPEED_BUF_COUNT; id++) {
        const size_t bytes = speed_buffer_bytes(p, (SpeedCudaBufferId)id);
        if (bytes == 0u)
            continue; /* SCALED without prescale */
        const int err = vmaf_cuda_buffer_alloc(p->cu_state, &p->buf[id], bytes);
        if (err)
            return err;
    }
    return vmaf_cuda_buffer_host_alloc(p->cu_state, (void **)&p->h_result,
                                       sizeof(SpeedGpuFrameResult));
}

static int speed_configure(SpeedCudaPipeline *p, const SpeedInternalOptions *opt, unsigned width,
                           unsigned height, unsigned bpc)
{
    SpeedInternalDimensions dim;
    memset(&dim, 0, sizeof(dim));
    int err = speed_internal_init_dimensions(&dim, (int)width, (int)height, opt->speed_prescale);
    if (!err)
        err = speed_internal_gpu_configure(&dim, opt, bpc, &p->config);
    if (!err && !speed_config_valid(&p->config, p->channels, p->raw_planes))
        err = -EINVAL;
    if (err)
        return err;
    const SpeedGpuGeometry *g = &p->config.geometry;
    p->plane_bytes = (size_t)g->src_w * g->src_h * g->bytes_per_sample;
    p->cov_threads = speed_covariance_threads(g->sub_w * g->sub_h);
    return 0;
}

int speed_cuda_pipeline_open(SpeedCudaPipeline **out, VmafCudaState *cu_state,
                             const SpeedInternalOptions *opt, unsigned width, unsigned height,
                             unsigned bpc, uint32_t channels, uint32_t raw_planes)
{
    if (!out || !cu_state || !cu_state->f || !opt)
        return -EINVAL;
    SpeedCudaPipeline *p = calloc(1, sizeof(*p));
    *out = p;
    if (!p)
        return -ENOMEM;
    p->cu_state = cu_state;
    p->channels = channels;
    p->raw_planes = raw_planes;
    /* Published before the first device allocation: on failure the partial
     * pipeline stays in *out for the extractor's close (ADR-1336), which
     * releases whatever was acquired and keeps what it could not. */
    int err = speed_configure(p, opt, width, height, bpc);
    if (!err)
        err = vmaf_cuda_kernel_lifecycle_init(&p->lc, cu_state);
    if (!err)
        err = speed_in_context(p, speed_load_kernels);
    if (!err)
        err = speed_allocate(p);
    if (!err)
        err = speed_in_context(p, speed_upload_taps);
    return err;
}

/* ------------------------------------------------------------------ */
/* Per frame: stage, chain, fence, collect                             */
/* ------------------------------------------------------------------ */

int speed_cuda_pipeline_stage(SpeedCudaPipeline *p, uint32_t slot, VmafPicture *pic, unsigned plane,
                              CUstream stream)
{
    if (!p || !pic || slot >= p->raw_planes || plane > 2u || !pic->data[plane])
        return -EINVAL;
    const SpeedGpuGeometry *g = &p->config.geometry;
    if (pic->w[plane] < g->src_w || pic->h[plane] < g->src_h)
        return -EINVAL;
    const size_t row_bytes = (size_t)g->src_w * g->bytes_per_sample;
    CUDA_MEMCPY2D copy;
    memset(&copy, 0, sizeof(copy));
    copy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
    copy.srcDevice = (CUdeviceptr)(uintptr_t)pic->data[plane];
    copy.srcPitch = (size_t)pic->stride[plane];
    copy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
    copy.dstDevice = p->buf[SPEED_BUF_RAW]->data + (CUdeviceptr)(p->plane_bytes * slot);
    copy.dstPitch = row_bytes;
    copy.WidthInBytes = row_bytes;
    copy.Height = g->src_h;
    CHECK_CUDA_RETURN(p->cu_state->f, cuMemcpy2DAsync(&copy, stream));
    return 0;
}

static CUdeviceptr speed_dptr(const SpeedCudaPipeline *p, SpeedCudaBufferId id)
{
    return p->buf[id] ? p->buf[id]->data : (CUdeviceptr)0;
}

static void speed_frame_args(const SpeedCudaPipeline *p, const SpeedGpuChannelBinding *bindings,
                             SpeedCudaFrameArgs *a)
{
    memset(a, 0, sizeof(*a));
    a->raw = speed_dptr(p, SPEED_BUF_RAW);
    a->plane_bytes = p->plane_bytes;
    a->taps = speed_dptr(p, SPEED_BUF_TAPS);
    a->scaled = speed_dptr(p, SPEED_BUF_SCALED);
    a->down = speed_dptr(p, SPEED_BUF_DOWN);
    a->centered = speed_dptr(p, SPEED_BUF_CENTERED);
    a->indterm = speed_dptr(p, SPEED_BUF_INDTERM);
    a->means = speed_dptr(p, SPEED_BUF_MEANS);
    a->cov = speed_dptr(p, SPEED_BUF_COV);
    a->eig = speed_dptr(p, SPEED_BUF_EIG);
    a->qmat = speed_dptr(p, SPEED_BUF_QMAT);
    a->rmat = speed_dptr(p, SPEED_BUF_RMAT);
    a->status = speed_dptr(p, SPEED_BUF_STATUS);
    a->var = speed_dptr(p, SPEED_BUF_VAR);
    a->ent = speed_dptr(p, SPEED_BUF_ENT);
    a->contrib = speed_dptr(p, SPEED_BUF_CONTRIB);
    a->result = speed_dptr(p, SPEED_BUF_RESULT);
    a->geometry = p->config.geometry;
    a->scoring = p->config.scoring;
    for (uint32_t ch = 0; ch < p->channels; ch++)
        a->bindings.channel[ch] = bindings[ch];
    a->channels = p->channels;
    a->antialias_width = p->config.filters.antialias_width;
    a->lowpass_width = p->config.filters.lowpass_width;
    a->cov_threads = p->cov_threads;
}

static unsigned speed_grid(size_t items, unsigned threads)
{
    return (unsigned)((items + threads - 1u) / threads);
}

/* Every kernel takes the one SpeedCudaFrameArgs block by value. */
static int speed_launch(const SpeedCudaPipeline *p, SpeedCudaKernel k, unsigned grid,
                        unsigned threads, CUstream stream, SpeedCudaFrameArgs *args)
{
    void *params[] = {args};
    CHECK_CUDA_RETURN(p->cu_state->f, cuLaunchKernel(p->fn[k], grid, 1u, 1u, threads, 1u, 1u, 0u,
                                                     stream, params, NULL));
    return 0;
}

/* Prescale (when configured), then the anti-alias filter at the decimated
 * sample points. */
static int speed_enqueue_filter(const SpeedCudaPipeline *p, CUstream stream, SpeedCudaFrameArgs *a)
{
    const SpeedGpuGeometry *g = &p->config.geometry;
    const size_t ch = p->channels;
    const unsigned down = speed_grid(ch * g->down_w * g->down_h, SPEED_CUDA_PIXEL_THREADS);
    if (!g->prescale)
        return speed_launch(p, SPEED_FN_DECIMATE_RAW, down, SPEED_CUDA_PIXEL_THREADS, stream, a);
    const unsigned scaled = speed_grid(ch * g->scaled_w * g->scaled_h, SPEED_CUDA_PIXEL_THREADS);
    const int err = speed_launch(p, SPEED_FN_SCALE, scaled, SPEED_CUDA_PIXEL_THREADS, stream, a);
    if (err)
        return err;
    return speed_launch(p, SPEED_FN_DECIMATE_SCALED, down, SPEED_CUDA_PIXEL_THREADS, stream, a);
}

/* Local mean subtraction, means, covariance, eigenvalues and QR. */
static int speed_enqueue_statistics(const SpeedCudaPipeline *p, CUstream stream,
                                    SpeedCudaFrameArgs *a)
{
    const SpeedGpuGeometry *g = &p->config.geometry;
    const size_t ch = p->channels;
    const unsigned centre = speed_grid(ch * g->trunc_w * g->trunc_h, SPEED_CUDA_PIXEL_THREADS);
    int err = speed_launch(p, SPEED_FN_CENTRE, centre, SPEED_CUDA_PIXEL_THREADS, stream, a);
    if (!err) {
        err =
            speed_launch(p, SPEED_FN_MEANS, speed_grid(ch * SPEED_CUDA_N, SPEED_CUDA_MEANS_THREADS),
                         SPEED_CUDA_MEANS_THREADS, stream, a);
    }
    if (!err) {
        err = speed_launch(p, SPEED_FN_COVARIANCE, (unsigned)(ch * SPEED_CUDA_TRIANGLE),
                           p->cov_threads, stream, a);
    }
    if (!err)
        err = speed_launch(p, SPEED_FN_LINALG, (unsigned)ch, SPEED_CUDA_LINALG_THREADS, stream, a);
    return err;
}

/* Solve, variances, entropies, and the per-pair frame score. */
static int speed_enqueue_scoring(const SpeedCudaPipeline *p, CUstream stream, SpeedCudaFrameArgs *a)
{
    const size_t ch = p->channels;
    const unsigned solve = speed_grid(ch * p->config.geometry.blocks, SPEED_CUDA_SOLVE_THREADS);
    const int err = speed_launch(p, SPEED_FN_SOLVE, solve, SPEED_CUDA_SOLVE_THREADS, stream, a);
    if (err)
        return err;
    return speed_launch(p, SPEED_FN_SCORE, (unsigned)(ch / 2u), SPEED_CUDA_SCORE_THREADS, stream,
                        a);
}

int speed_cuda_pipeline_fence(SpeedCudaPipeline *p, CUstream stream)
{
    if (!p)
        return -EINVAL;
    CudaFunctions *cu_f = p->cu_state->f;
    CHECK_CUDA_RETURN(cu_f, cuEventRecord(p->lc.submit, stream));
    CHECK_CUDA_RETURN(cu_f, cuStreamWaitEvent(p->lc.str, p->lc.submit, CU_EVENT_WAIT_DEFAULT));
    return vmaf_cuda_kernel_submit_post_record(&p->lc, p->cu_state);
}

int speed_cuda_pipeline_submit(SpeedCudaPipeline *p, const SpeedGpuChannelBinding *bindings,
                               CUstream stream)
{
    if (!p || !bindings)
        return -EINVAL;
    for (uint32_t ch = 0; ch < p->channels; ch++) {
        const int32_t planes = (int32_t)p->raw_planes;
        if (bindings[ch].minuend < 0 || bindings[ch].minuend >= planes ||
            bindings[ch].subtrahend >= planes)
            return -EINVAL;
    }
    SpeedCudaFrameArgs args;
    speed_frame_args(p, bindings, &args);
    int err = speed_enqueue_filter(p, stream, &args);
    if (!err)
        err = speed_enqueue_statistics(p, stream, &args);
    if (!err)
        err = speed_enqueue_scoring(p, stream, &args);
    if (err)
        return err;
    CudaFunctions *cu_f = p->cu_state->f;
    CHECK_CUDA_RETURN(cu_f, cuEventRecord(p->lc.submit, stream));
    CHECK_CUDA_RETURN(cu_f, cuStreamWaitEvent(p->lc.str, p->lc.submit, CU_EVENT_WAIT_DEFAULT));
    CHECK_CUDA_RETURN(cu_f, cuMemcpyDtoHAsync(p->h_result, speed_dptr(p, SPEED_BUF_RESULT),
                                              sizeof(SpeedGpuFrameResult), p->lc.str));
    return vmaf_cuda_kernel_submit_post_record(&p->lc, p->cu_state);
}

int speed_cuda_pipeline_wait(SpeedCudaPipeline *p)
{
    if (!p)
        return -EINVAL;
    return vmaf_cuda_kernel_collect_wait(&p->lc, p->cu_state);
}

int speed_cuda_pipeline_collect(SpeedCudaPipeline *p, SpeedGpuFrameResult *out)
{
    if (!out)
        return -EINVAL;
    const int err = speed_cuda_pipeline_wait(p);
    if (err)
        return err;
    *out = *p->h_result;
    return 0;
}

/* NOLINTEND(modernize-use-nullptr) */
