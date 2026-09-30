/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Device-resident SpEED pipeline shared by speed_chroma_cuda and
 *  speed_temporal_cuda (ADR-1380, the CUDA port of ADR-1358).
 *
 *  Per frame the extractor stages the raw input planes it needs into the
 *  pipeline's raw-plane slots with device-to-device copies of the picture
 *  planes the engine already uploaded, then speed_cuda_pipeline_submit()
 *  enqueues every per-frame stage of speed.c (speed/speed_score.cu) on the
 *  picture's stream and one readback of the SpeedGpuFrameResult on the
 *  pipeline's private stream. No call here waits on the device except
 *  speed_cuda_pipeline_collect(), once per frame.
 */

#ifndef VMAF_SRC_FEATURE_CUDA_SPEED_CUDA_PIPELINE_H_
#define VMAF_SRC_FEATURE_CUDA_SPEED_CUDA_PIPELINE_H_

#include <stdint.h>

#include "cuda/common.h"
#include "feature/speed_gpu_common.h"
#include "feature/speed_internal.h"
#include "libvmaf/picture.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SpeedCudaPipeline SpeedCudaPipeline;

/* Derive the SpEED dimensions of a width x height plane, configure the
 * pipeline with speed_internal_gpu_configure() and allocate its device
 * state: `channels` (2 or 4) channels read from `raw_planes` raw-plane slots.
 * Loads the kernels and uploads the filter taps. Returns 0 or a negative
 * errno. *out is set before the first device allocation and keeps the
 * partial pipeline on failure: the extractor's close, which the engine owes a
 * CUDA extractor even after a failed init (ADR-1336), releases it with
 * speed_cuda_pipeline_close(). */
int speed_cuda_pipeline_open(SpeedCudaPipeline **out, VmafCudaState *cu_state,
                             const SpeedInternalOptions *opt, unsigned width, unsigned height,
                             unsigned bpc, uint32_t channels, uint32_t raw_planes);

/* Drain the private stream and release everything. Safe on NULL. Returns the
 * first teardown error. */
int speed_cuda_pipeline_close(SpeedCudaPipeline **pipeline);

/* Enqueue on `stream` a device-to-device copy of plane `plane` of the device
 * picture `pic` (src_w x src_h samples, picture_copy()'s sample width) into
 * raw-plane slot `slot`. The caller holds the pipeline's CUDA context. */
int speed_cuda_pipeline_stage(SpeedCudaPipeline *pipeline, uint32_t slot, VmafPicture *pic,
                              unsigned plane, CUstream stream);

/* Enqueue the whole per-frame chain for `bindings` (`channels` entries) on
 * `stream`, then fence it and read the result back on the private stream.
 * Never waits. The caller holds the pipeline's CUDA context. */
int speed_cuda_pipeline_submit(SpeedCudaPipeline *pipeline, const SpeedGpuChannelBinding *bindings,
                               CUstream stream);

/* Fence the staging copies enqueued on `stream` without running the chain
 * (speed_temporal's first frame). Never waits. */
int speed_cuda_pipeline_fence(SpeedCudaPipeline *pipeline, CUstream stream);

/* The frame's one wait: drain the private stream and copy the result read
 * back by speed_cuda_pipeline_submit(). */
int speed_cuda_pipeline_collect(SpeedCudaPipeline *pipeline, SpeedGpuFrameResult *out);

/* The same wait without a result (after speed_cuda_pipeline_fence()). */
int speed_cuda_pipeline_wait(SpeedCudaPipeline *pipeline);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* VMAF_SRC_FEATURE_CUDA_SPEED_CUDA_PIPELINE_H_ */
