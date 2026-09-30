/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  Kernel argument block of the device-resident CUDA SpEED chain (ADR-1380).
 *
 *  Every kernel in speed_score.cu takes exactly one parameter, this struct by
 *  value, and speed_cuda_pipeline.c passes `void *params[] = {&args}` to every
 *  cuLaunchKernel. One layout shared by the host C code and the device code
 *  means a parameter cannot be dropped, reordered or miscounted between the
 *  two sides; the driver silently ignores a surplus argument (ADR-1215).
 *  Device pointers travel as uint64_t (CUdeviceptr on the host); the kernels
 *  cast them back to their element type.
 */

#ifndef VMAF_SRC_FEATURE_CUDA_SPEED_SPEED_CUDA_PARAMS_H_
#define VMAF_SRC_FEATURE_CUDA_SPEED_SPEED_CUDA_PARAMS_H_

#include <stdint.h>

#include "feature/speed_gpu_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Threads per block of each kernel; the kernels declare the same values as
 * __launch_bounds__. */
#define SPEED_CUDA_PIXEL_THREADS 256u   /* scale, decimate, centre */
#define SPEED_CUDA_MEANS_THREADS 128u   /* 25 x channels threads */
#define SPEED_CUDA_COV_MIN_THREADS 32u  /* covariance block, power of two */
#define SPEED_CUDA_COV_MAX_THREADS 256u /* covariance block, power of two */
#define SPEED_CUDA_LINALG_THREADS 64u   /* one block per channel */
#define SPEED_CUDA_SOLVE_THREADS 128u   /* one thread per (channel, block) */
#define SPEED_CUDA_SCORE_THREADS 256u   /* one block per score pair */

/* Raw planes the kernels read: `minuend - subtrahend` per channel (see
 * SpeedGpuChannelBinding). */
typedef struct SpeedCudaBindings {
    SpeedGpuChannelBinding channel[SPEED_GPU_MAX_CHANNELS];
} SpeedCudaBindings;

typedef struct SpeedCudaFrameArgs {
    uint64_t raw;         /* uint8 raw planes, raw_planes x plane_bytes */
    uint64_t plane_bytes; /* src_w * src_h * bytes_per_sample */
    uint64_t taps;        /* float antialias[128], then lowpass[128] */
    uint64_t scaled;      /* float channels x scaled_h x scaled_w (prescale only) */
    uint64_t down;        /* float channels x down_h x down_w */
    uint64_t centered;    /* float channels x trunc_h x trunc_w */
    uint64_t indterm;     /* float channels x 25 x blocks */
    uint64_t means;       /* float channels x 25 */
    uint64_t cov;         /* float channels x 625 */
    uint64_t eig;         /* float channels x 25 */
    uint64_t qmat;        /* float channels x 625, accumulated reflector product */
    uint64_t rmat;        /* float channels x 625 */
    uint64_t status;      /* int32 channels x 2: singular, iteration cap */
    uint64_t var;         /* float channels x blocks */
    uint64_t ent;         /* float channels x blocks */
    uint64_t contrib;     /* float pairs x blocks */
    uint64_t result;      /* SpeedGpuFrameResult */
    SpeedGpuGeometry geometry;
    SpeedGpuScoring scoring;
    SpeedCudaBindings bindings;
    uint32_t channels;        /* 2 (one score pair) or 4 (two pairs) */
    uint32_t antialias_width; /* taps */
    uint32_t lowpass_width;   /* taps */
    uint32_t cov_threads;     /* covariance block size, a power of two */
} SpeedCudaFrameArgs;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* VMAF_SRC_FEATURE_CUDA_SPEED_SPEED_CUDA_PARAMS_H_ */
