/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Device-resident SpEED pipeline shared by speed_chroma_sycl and
 *  speed_temporal_sycl (ADR-1358).
 *
 *  One upload of the raw input planes per frame, then every per-frame stage
 *  of the CPU reference (speed.c) runs on the device in one in-order chain:
 *  picture conversion (and temporal difference), optional prescale, the
 *  anti-alias filter at the 16x-decimated sample points, local mean
 *  subtraction, the 25 submatrix means, the 25x25 covariance matrix, its
 *  eigenvalues, the regularity decision, the Householder QR factorisation,
 *  the Q^T B multiply with back substitution, the entropies and variances,
 *  and the frame score. The host reads back one FrameResult at collect time.
 *
 *  Numerical contract: every stage mirrors the CPU reference operation for
 *  operation in fp32 (no fp64 anywhere, ADR-0220). The pipeline TU is built
 *  with contraction off and correctly rounded fp32 division and square root
 *  (core/src/meson.build, `sycl_exact_fp_args`), without which no
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

inline constexpr uint32_t kBlock = 5;
inline constexpr uint32_t kElements = kBlock * kBlock;
inline constexpr uint32_t kMaxChannels = 4;
inline constexpr uint32_t kMaxPairs = 2;
inline constexpr uint32_t kMaxRawPlanes = 4;
inline constexpr uint32_t kMaxTaps = 128;

/* Plane geometry, identical for every channel of one pipeline. Mirrors
 * SpeedInternalDimensions (speed_internal.h) plus the raw sample format. */
struct Geometry {
    uint32_t src_w;    /* original_width: raw plane width in samples */
    uint32_t src_h;    /* original_height */
    uint32_t scaled_w; /* after prescale */
    uint32_t scaled_h;
    uint32_t down_w; /* scaled >> 4 */
    uint32_t down_h;
    uint32_t trunc_w; /* multiple of kBlock */
    uint32_t trunc_h;
    uint32_t blocks_h;         /* blocks per row */
    uint32_t blocks;           /* total blocks */
    uint32_t sub_w;            /* submatrix width */
    uint32_t sub_h;            /* submatrix height */
    uint32_t bytes_per_sample; /* 2 for the picture_copy() high-bit-depth path, else 1 */
    float sample_scale;        /* picture_copy() divisor on the 2-byte path */
    int32_t prescale;          /* non-zero: resample the frame before filtering */
    int32_t scale_method;      /* enum vif_scaling_method */
};

/* Filter taps computed once on the host by vif_tools.c. */
struct Filters {
    float antialias[kMaxTaps];
    float lowpass[kMaxTaps];
    uint32_t antialias_width;
    uint32_t lowpass_width;
};

/* Scoring constants. The two log2f() constants are evaluated once on the host
 * with the same libm the CPU extractor uses. */
struct Scoring {
    float sigma_nn;
    float entropy_constant; /* log2f(2 * pi * e) */
    float base_entropy;     /* get_speed_score() entropy floor */
    int32_t weight_mode;    /* speed_weight_var_mode, 0..6 */
};

/* Raw planes one channel reads: `minuend - subtrahend` (temporal difference),
 * or `minuend` alone when `subtrahend` is negative. */
struct ChannelBinding {
    int32_t minuend;
    int32_t subtrahend;
};

} // namespace speed_sycl

namespace speed_sycl
{

/* Per-frame device result, read back once at collect time. Channel 2p is the
 * reference and 2p + 1 the distorted side of score pair p. */
struct FrameResult {
    float score[kMaxPairs];
    int32_t singular[kMaxChannels];      /* covariance could not be inverted */
    int32_t iteration_cap[kMaxChannels]; /* eigenvalue QR iteration hit its cap */
};

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

/* Enqueue the whole per-frame chain and the FrameResult readback. No host
 * wait; `bindings` holds config.channels entries. */
int pipeline_submit(Pipeline *pipeline, const ChannelBinding *bindings);

/* Wait for the queue and copy the frame's result. */
int pipeline_collect(Pipeline *pipeline, FrameResult *out);

/* Wait for the queue without reading a result (upload-only frames). */
int pipeline_wait(Pipeline *pipeline);

/* ---- Host setup shared by both extractors (speed_sycl_host.cpp) ---- */

/* Fill geometry, filters and scoring from the SpEED dimensions and options,
 * validating kernelscale and prescale method exactly as speed_init() does.
 * The caller sets queue, channels, raw_planes and staged. */
int configure(const SpeedInternalDimensions &dim, const SpeedInternalOptions &opt, unsigned bpc,
              PipelineConfig &config);

/* Copy plane `plane` of `pic` (src_w x src_h samples) into staging plane
 * `index`, packed. */
int stage_plane(Pipeline *pipeline, uint32_t index, const VmafPicture *pic, unsigned plane);

} // namespace speed_sycl

#endif /* VMAF_FEATURE_SYCL_SPEED_SYCL_PIPELINE_H_ */
