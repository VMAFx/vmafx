/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Host side of the device-resident SpEED chain shared by speed_chroma_hip
 *  and speed_temporal_hip (ADR-1384, the HIP port of ADR-1358).
 *
 *  speed_internal_gpu_configure() (speed_internal.c) derives, once at init
 *  and with the CPU extractor's own helpers, the plane geometry, filter taps
 *  and scoring constants; the SYCL twins take theirs from the same routine.
 *  A pipeline then owns one device arena, one pinned staging block, one
 *  pinned result block and one stream. Per frame the extractor uploads its
 *  raw planes (vmaf_hip_picture_upload_staged(), no host wait) and submits
 *  the chain; nothing waits until speed_hip_pipeline_collect() /
 *  speed_hip_pipeline_wait(), the one wait of the frame. The kernels are in
 *  speed/speed_pipeline.hip.
 */

#ifndef VMAF_SRC_FEATURE_HIP_SPEED_HIP_PIPELINE_H_
#define VMAF_SRC_FEATURE_HIP_SPEED_HIP_PIPELINE_H_

#include <stddef.h>
#include <stdint.h>

#include "libvmaf/picture.h"

#include "feature/speed_gpu_common.h"
#include "feature/speed_internal.h"
#include "speed/speed_hip_device.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Everything a pipeline is created from. `shared` comes from
 * speed_internal_gpu_configure(); the caller sets the rest. */
typedef struct SpeedHipConfig {
    SpeedGpuConfig shared;
    uint32_t channels;   /* 2 (one score pair) or 4 (two pairs) */
    uint32_t raw_planes; /* device raw-plane slots */
    uint32_t staged;     /* most planes one upload carries */
} SpeedHipConfig;

/* The channel bindings of every binding set a frame can be submitted with
 * (chroma uses set 0; temporal alternates sets 0 and 1 with the slots). */
typedef struct SpeedHipBindingSets {
    SpeedGpuChannelBinding set[SPEED_HIP_BINDING_SETS][SPEED_HIP_MAX_CHANNELS];
} SpeedHipBindingSets;

/* One raw plane to upload: plane `plane` of `pic`. */
typedef struct SpeedHipPlane {
    const VmafPicture *pic;
    unsigned plane;
} SpeedHipPlane;

typedef struct SpeedHipPipeline SpeedHipPipeline;

/* Bytes of one packed raw plane. */
size_t speed_hip_plane_bytes(const SpeedGpuGeometry *geometry);

/* The scalar part of the parameter block and the tap table the kernels read
 * (antialias taps, then lowpass taps, SPEED_HIP_MAX_TAPS each), from the
 * configuration. speed_hip_pipeline_create() uploads exactly this; the
 * device-free replay (core/test/test_hip_speed_device_math.c) runs with it.
 * Buffer pointers are left to the caller. Host only, every build. */
void speed_hip_params_fill(SpeedHipParams *params, const SpeedHipConfig *config,
                           const SpeedHipBindingSets *bindings);
void speed_hip_taps_fill(float *taps, const SpeedGpuFilters *filters);

/* The binding sets of the two extractors. speed_chroma_hip: channel c reads
 * raw plane c (U ref, U dis, V ref, V dis). speed_temporal_hip: set s scores a
 * frame whose index is s modulo 2; the current frame sits in raw slot s
 * (planes 2s, 2s + 1), the previous one in the other slot, and each channel
 * reads subtract_image(previous, current) -- for the distorted side the
 * current reference with speed_use_ref_diff. Host only, every build. */
void speed_hip_bindings_chroma(SpeedHipBindingSets *bindings);
void speed_hip_bindings_temporal(int use_ref_diff, SpeedHipBindingSets *bindings);

/* Whether a configuration and its bindings describe a pipeline the kernels
 * can run: 1 or 0. Host only, every build. */
int speed_hip_config_valid(const SpeedHipConfig *config, const SpeedHipBindingSets *bindings);

/* Allocate the arena, the pinned blocks and the stream, load the kernels and
 * upload the parameter block; only the first config->channels entries of
 * each binding set are read. Returns 0 or a negative errno; -ENOSYS in a
 * build without HIP kernels. */
int speed_hip_pipeline_create(SpeedHipPipeline **out, const SpeedHipConfig *config,
                              const SpeedHipBindingSets *bindings);

/* Drain the stream and release everything. NULL-safe. */
void speed_hip_pipeline_destroy(SpeedHipPipeline **pipeline);

/* Copy `count` picture planes into pinned staging and enqueue their upload
 * into raw planes [first, first + count), packed, without waiting
 * (vmaf_hip_picture_upload_staged()). The pictures may be recycled as soon
 * as this returns. */
int speed_hip_pipeline_upload(SpeedHipPipeline *pipeline, uint32_t first,
                              const SpeedHipPlane *planes, uint32_t count);

/* Enqueue the whole chain for binding set `set` and the result readback.
 * No wait. */
int speed_hip_pipeline_submit(SpeedHipPipeline *pipeline, uint32_t set);

/* The frame's one wait, then its result. */
int speed_hip_pipeline_collect(SpeedHipPipeline *pipeline, SpeedGpuFrameResult *out);

/* The frame's one wait, for a frame that only uploaded. */
int speed_hip_pipeline_wait(SpeedHipPipeline *pipeline);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* VMAF_SRC_FEATURE_HIP_SPEED_HIP_PIPELINE_H_ */
