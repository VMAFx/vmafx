/**
 *  Copyright (c) the JPEG XL Project Authors.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent AND BSD-3-Clause
 *
 *  Host / device contract of the device-resident ssimulacra2 CUDA twin
 *  (ADR-1391). Shared by ssimulacra2_cuda.c and
 *  ssimulacra2/ssimulacra2_device.cu, so every struct passed to a kernel by
 *  value has one definition.
 */

#ifndef FEATURE_SSIMULACRA2_CUDA_H_
#define FEATURE_SSIMULACRA2_CUDA_H_

#include <stddef.h>
#include <stdint.h>

#include "common.h"

#define SS2C_IMAGES 2   /* reference, distorted */
#define SS2C_CHANNELS 3 /* X, Y, B (and R, G, B before the XYB step) */
/* Per channel: SSIM L1, SSIM L4, artifact L1, artifact L4, detail L1,
 * detail L4 (ssimulacra2.c::ssim_map and ::edge_diff_map, in that order). */
#define SS2C_SUMS 6
#define SS2C_NUM_SCALES 6
/* Per-pixel kernels: 16 x 8 threads per block. */
#define SS2C_PIX_BX 16
#define SS2C_PIX_BY 8
/* Sums (ADR-1433): the CPU adds each of a channel's six terms pixel after
 * pixel into one double, and the twin returns those bits
 * (feature/ordered_sum.h). The plane is cut into chunks in raster order; a
 * chunk is one block of 256 lanes, each lane taking 4 consecutive pixels. */
#define SS2C_REDUCE_BLOCK 256
#define SS2C_CHUNK_RUN 4
#define SS2C_CHUNK_PIXELS (SS2C_REDUCE_BLOCK * SS2C_CHUNK_RUN)
/* The two kernels that follow the chunks in order stage them through shared
 * memory, SS2C_BATCH chunks at a time (4 per lane), so that the one lane
 * that walks them does not read device memory chunk by chunk. */
#define SS2C_BATCH_RUN 4
#define SS2C_BATCH (SS2C_REDUCE_BLOCK * SS2C_BATCH_RUN)

/* The five blurred quantities of ssimulacra2.c::extract, one blur job each:
 * blur(ref), blur(dis), blur(ref * ref), blur(dis * dis), blur(ref * dis). */
/* NOLINTBEGIN(performance-enum-size): C header included by C and C++ translation units; C has no fixed enum underlying type across the required toolchains (ADR-1470). ADR-1138. */
enum ss2c_blur_job { SS2C_MU1 = 0, SS2C_MU2, SS2C_S11, SS2C_S22, SS2C_S12, SS2C_BLUR_JOBS };
/* NOLINTEND(performance-enum-size) */
/* Horizontal pass: one warp per block, one row per lane, 32-column tiles
 * staged through shared memory. Vertical pass: one column per thread. */
#define SS2C_BLUR_TILE 32
#define SS2C_BLUR_V_BLOCK 64
/* The horizontal pass keeps two 32-column tiles; the IIR reads at most
 * 2 * radius columns back, so the radius may not exceed 16. */
#define SS2C_BLUR_MAX_RADIUS 16

/* YUV -> linear RGB constants, evaluated on the host with the float
 * expressions of ssimulacra2.c::picture_to_linear_rgb. */
/* NOLINTBEGIN(modernize-use-using): C header included by C and C++ translation units; C has no `using`. ADR-1138. */
typedef struct Ss2cYuvCoefficients {
    float inv_peak;
    float y_off;
    float y_scale;
    float c_off;
    float c_scale;
    float cr_r;
    float cb_g;
    float cr_g;
    float cb_b;
} Ss2cYuvCoefficients;

/* Both pictures of a frame: raw device planes in, planar linear RGB out
 * (three compact width x height planes per image). */
typedef struct Ss2cYuvArgs {
    const void *plane[SS2C_IMAGES][SS2C_CHANNELS];
    size_t pitch[SS2C_IMAGES][SS2C_CHANNELS]; /* bytes */
    float *out[SS2C_IMAGES];
    unsigned plane_w[SS2C_CHANNELS];
    unsigned plane_h[SS2C_CHANNELS];
    unsigned width;
    unsigned height;
    unsigned wide; /* 16-bit samples */
    Ss2cYuvCoefficients k;
} Ss2cYuvArgs;

/* One scale's five blurs (ssimulacra2.c::blur_3plane for each job): XYB in,
 * horizontal-pass output in `pass`, the blurred planes in `out`. Three compact
 * width x height planes per buffer. */
typedef struct Ss2cBlurArgs {
    const float *ref;
    const float *dis;
    float *pass[SS2C_BLUR_JOBS];
    float *out[SS2C_BLUR_JOBS];
    unsigned width;
    unsigned height;
    float n2[3];
    float d1[3];
    int radius;
    unsigned pad_;
} Ss2cBlurArgs;

/* One scale's SSIM / edge-difference sums. Three compact planes per input
 * buffer; `pixels` = width x height of the scale, `chunks` the number of
 * SS2C_CHUNK_PIXELS-pixel chunks of one plane (the last may be partial). */
typedef struct Ss2cCombineArgs {
    const float *mu1;
    const float *mu2;
    const float *s11;
    const float *s22;
    const float *s12;
    const float *img1;
    const float *img2;
    double *chunk_sums; /* [channel][chunk][sum]: each chunk's terms, added in a tree */
    int16_t *plan;      /* [channel][sum][chunk]: vmaf_ordsum_plan() */
    int64_t *units;     /* [channel][sum][chunk][2]: VmafOrdsumUnits even, odd */
    double *totals;     /* [channel][sum] of this scale: the CPU's sums */
    size_t pixels;
    unsigned chunks;
    unsigned pad_;
} Ss2cCombineArgs;
/* NOLINTEND(modernize-use-using) */

extern const unsigned char ssimulacra2_blur_ptx[];
extern const unsigned char ssimulacra2_device_ptx[];

#endif /* FEATURE_SSIMULACRA2_CUDA_H_ */
