/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Device-resident SpEED pipeline shared by speed_chroma_sycl and
 *  speed_temporal_sycl (ADR-1358).
 *
 *  One upload of the raw input planes per frame, then every stage of the CPU
 *  reference (speed.c) up to the solved linear system runs on the device in
 *  one in-order chain: picture conversion (and temporal difference),
 *  optional prescale, the anti-alias filter at the 16x-decimated sample
 *  points, local mean subtraction, the 25 submatrix means, the 25x25
 *  covariance matrix, its eigenvalues, the regularity decision, the
 *  Householder QR factorisation, the Q^T B multiply with back substitution
 *  and the variances. The host reads one block back at collect time (status
 *  words, eigenvalues, variances) and forms the entropies and the frame score
 *  from it with speed.c's statements and the host's log2()
 *  (speed_internal_gpu_tail_scores(), ADR-1477).
 *
 *  Numerical contract: every device stage mirrors the CPU reference
 *  operation for operation in fp32 (no fp64 anywhere, ADR-0220). The pipeline TU is built
 *  with contraction off and correctly rounded fp32 division and square root
 *  (core/src/meson.build, `sycl_strict_fp_args`, ADR-1367), without which no
 *  device kernel can reproduce the host arithmetic.
 */

#ifndef VMAF_FEATURE_SYCL_SPEED_SYCL_PIPELINE_H_
#define VMAF_FEATURE_SYCL_SPEED_SYCL_PIPELINE_H_

#include <cstddef>
#include <cstdint>

#include "libvmaf/picture.h"

#include "feature/speed_internal.h"

namespace speed_sycl
{

inline constexpr uint32_t kBlock = SPEED_GPU_BLOCK;
inline constexpr uint32_t kElements = SPEED_GPU_ELEMENTS;
inline constexpr uint32_t kMaxChannels = SPEED_GPU_MAX_CHANNELS;
inline constexpr uint32_t kMaxPairs = SPEED_GPU_MAX_PAIRS;
inline constexpr uint32_t kMaxRawPlanes = SPEED_GPU_MAX_RAW_PLANES;
inline constexpr uint32_t kMaxTaps = SPEED_GPU_MAX_TAPS;

/* The per-run contract every device-resident SpEED twin shares
 * (feature/speed_gpu_common.h, filled by speed_internal_gpu_configure()):
 * plane geometry, filter taps, scoring constants, the raw planes one channel
 * reads (`minuend - subtrahend`, or `minuend` alone when `subtrahend` is
 * negative) and the per-frame result. */
using Geometry = SpeedGpuGeometry;
using Filters = SpeedGpuFilters;
using Scoring = SpeedGpuScoring;
using ChannelBinding = SpeedGpuChannelBinding;

} // namespace speed_sycl

namespace speed_sycl
{

/* Per-frame result: the host tail's scores and the device's status words.
 * Channel 2p is the reference and 2p + 1 the distorted side of score pair p. */
using FrameResult = SpeedGpuFrameResult;

struct Pipeline;

struct PipelineConfig {
    void *queue; /* sycl::queue *, in-order */
    Geometry geometry;
    Filters filters;
    Scoring scoring;
    uint32_t channels;   /* 2 (one score pair) or 4 (two pairs) */
    uint32_t raw_planes; /* device raw-plane slots */
    uint32_t staged;     /* host staging planes uploaded per call */
};

/* Allocate every device and pinned-host buffer. Returns 0 or -ENOMEM/-EINVAL. */
int pipeline_create(Pipeline **out, const PipelineConfig &config);
void pipeline_destroy(Pipeline **pipeline);

/* Pinned host staging plane `index` (< config.staged), packed src_w x src_h. */
void *pipeline_staging(Pipeline *pipeline, uint32_t index);

/* Enqueue one host-to-device copy of staging planes [0, count) into raw
 * planes [first, first + count). */
int pipeline_upload(Pipeline *pipeline, uint32_t first, uint32_t count);

/* Enqueue the whole device chain and the readback of its tail block. No host
 * wait; `bindings` holds config.channels entries. */
int pipeline_submit(Pipeline *pipeline, const ChannelBinding *bindings);

/* Wait for the queue, then form the frame's result on the host. */
int pipeline_collect(Pipeline *pipeline, FrameResult *out);

/* Wait for the queue without reading a result (upload-only frames). */
int pipeline_wait(Pipeline *pipeline);

/* ---- Host setup shared by both extractors (speed_sycl_host.cpp) ---- */

/* Fill geometry, filters and scoring from the SpEED dimensions and options
 * through speed_internal_gpu_configure(), which validates kernelscale and
 * prescale method exactly as speed_init() does. The caller sets queue,
 * channels, raw_planes and staged. */
int configure(const SpeedInternalDimensions &dim, const SpeedInternalOptions &opt, unsigned bpc,
              PipelineConfig &config);

/* Copy plane `plane` of `pic` (src_w x src_h samples) into staging plane
 * `index`, packed. */
int stage_plane(Pipeline *pipeline, uint32_t index, const VmafPicture *pic, unsigned plane);

/* Copy plane `plane` of a frame of the VMAFx API on this device (src_w x
 * src_h samples) into raw plane `index` (< config.raw_planes), packed, on the
 * device: such a frame's planes are never read on the host (ADR-2091). */
int read_device_plane(Pipeline *pipeline, uint32_t index, const VmafPicture *pic, unsigned plane);

} // namespace speed_sycl

#endif /* VMAF_FEATURE_SYCL_SPEED_SYCL_PIPELINE_H_ */
