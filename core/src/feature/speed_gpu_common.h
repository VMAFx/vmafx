/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Host/device contract of the device-resident SpEED pipelines (ADR-1358,
 *  ADR-1380, ADR-1384): the per-run constants a GPU twin needs to run every per-frame
 *  stage of speed.c on the device, and the one result block it reads back
 *  per frame.
 *
 *  Plain C with fixed-width fields only, so the same layout is shared by the
 *  C host code, the SYCL pipeline (speed_sycl_pipeline.h aliases these types),
 *  the CUDA kernels (cuda/speed/speed_score.cu takes them by value) and the
 *  HIP pipeline (hip/speed/speed_hip_device.h embeds them).
 *  speed_internal_gpu_configure() (speed_internal.c) fills them at init from
 *  the helpers the CPU extractor uses, so every backend receives the host's
 *  values bit for bit. Nothing here runs per frame.
 *
 *  Until ADR-1380 / ADR-1384 this header held the ADR-0567 split's parameter blocks,
 *  which no translation unit included.
 */

#ifndef VMAF_SRC_FEATURE_SPEED_GPU_COMMON_H_
#define VMAF_SRC_FEATURE_SPEED_GPU_COMMON_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* NOLINTBEGIN(modernize-use-using):
 * This is a C header. speed_internal.c and the CUDA host files
 * (cuda/speed_cuda_pipeline.c, cuda/speed_chroma_cuda.c,
 * cuda/speed_temporal_cuda.c) include it as C, and clang-tidy also parses it
 * as C++: under core/tools/vmaf.cpp, through feature_dimensions.h and
 * speed_internal.h, and under the SYCL SpEED translation units.
 * `modernize-use-using` asks for a `using` alias in place of each
 * `typedef struct`, which C cannot spell. File-scoped, the shape ADR-1138
 * prescribes for this C-parsed-as-C++ artefact, applied under ADR-0141. */

#define SPEED_GPU_BLOCK 5u                                     /* block_size */
#define SPEED_GPU_ELEMENTS (SPEED_GPU_BLOCK * SPEED_GPU_BLOCK) /* elements_in_block */
#define SPEED_GPU_MAX_CHANNELS 4u   /* two (reference, distorted) pairs */
#define SPEED_GPU_MAX_PAIRS 2u      /* scores per frame */
#define SPEED_GPU_MAX_RAW_PLANES 4u /* raw input planes a pipeline keeps */
#define SPEED_GPU_MAX_TAPS 128u     /* filter taps per filter */

/* Plane geometry, identical for every channel of one pipeline. Mirrors
 * SpeedInternalDimensions (speed_internal.h) plus the raw sample format. */
typedef struct SpeedGpuGeometry {
    uint32_t src_w;            /* original_width: raw plane width in samples */
    uint32_t src_h;            /* original_height */
    uint32_t scaled_w;         /* after prescale */
    uint32_t scaled_h;         /* after prescale */
    uint32_t down_w;           /* scaled_w >> NUM_SCALES */
    uint32_t down_h;           /* scaled_h >> NUM_SCALES */
    uint32_t trunc_w;          /* multiple of SPEED_GPU_BLOCK */
    uint32_t trunc_h;          /* multiple of SPEED_GPU_BLOCK */
    uint32_t blocks_h;         /* blocks per row */
    uint32_t blocks;           /* total blocks */
    uint32_t sub_w;            /* submatrix width */
    uint32_t sub_h;            /* submatrix height */
    uint32_t bytes_per_sample; /* 2 on picture_copy()'s 16-bit path, else 1 */
    float sample_scale;        /* picture_copy() divisor on the 2-byte path */
    int32_t prescale;          /* non-zero: resample the frame before filtering */
    int32_t scale_method;      /* enum vif_scaling_method */
} SpeedGpuGeometry;

/* Filter taps, computed once on the host by vif_tools.c. */
typedef struct SpeedGpuFilters {
    float antialias[SPEED_GPU_MAX_TAPS];
    float lowpass[SPEED_GPU_MAX_TAPS];
    uint32_t antialias_width;
    uint32_t lowpass_width;
} SpeedGpuFilters;

/* Scoring constants. The two log2f() constants are evaluated once on the host
 * with the libm the CPU extractor uses. */
typedef struct SpeedGpuScoring {
    float sigma_nn;
    float entropy_constant; /* log2f(2 * pi * e) */
    float base_entropy;     /* get_speed_score() entropy floor */
    int32_t weight_mode;    /* speed_weight_var_mode, 0..6 */
} SpeedGpuScoring;

/* Raw planes one channel reads: `minuend - subtrahend` (the temporal
 * difference of speed.c's subtract_image()), or `minuend` alone when
 * `subtrahend` is negative. Channel 2p is the reference and 2p + 1 the
 * distorted side of score pair p. */
typedef struct SpeedGpuChannelBinding {
    int32_t minuend;
    int32_t subtrahend;
} SpeedGpuChannelBinding;

/* The per-frame device result, read back once at collect time. */
typedef struct SpeedGpuFrameResult {
    float score[SPEED_GPU_MAX_PAIRS];
    int32_t singular[SPEED_GPU_MAX_CHANNELS];      /* covariance could not be inverted */
    int32_t iteration_cap[SPEED_GPU_MAX_CHANNELS]; /* eigenvalue QR iteration hit its cap */
} SpeedGpuFrameResult;

/* Everything speed_internal_gpu_configure() derives from the options. */
typedef struct SpeedGpuConfig {
    SpeedGpuGeometry geometry;
    SpeedGpuFilters filters;
    SpeedGpuScoring scoring;
} SpeedGpuConfig;

/* NOLINTEND(modernize-use-using) */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* VMAF_SRC_FEATURE_SPEED_GPU_COMMON_H_ */
