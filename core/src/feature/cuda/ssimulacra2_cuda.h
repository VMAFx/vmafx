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
/* Reduction: 256 threads per block, at most 256 blocks per channel, about 16
 * pixels per thread. The block count depends on the plane size only, so the
 * summation tree is the same on every device. */
#define SS2C_REDUCE_BLOCK 256
#define SS2C_MAX_GROUPS 256
#define SS2C_PIXELS_PER_ITEM 16

/* The five blurred quantities of ssimulacra2.c::extract, one blur job each:
 * blur(ref), blur(dis), blur(ref * ref), blur(dis * dis), blur(ref * dis). */
enum ss2c_blur_job { SS2C_MU1 = 0, SS2C_MU2, SS2C_S11, SS2C_S22, SS2C_S12, SS2C_BLUR_JOBS };
/* Horizontal pass: one warp per block, one row per lane, 32-column tiles
 * staged through shared memory. Vertical pass: one column per thread. */
#define SS2C_BLUR_TILE 32
#define SS2C_BLUR_V_BLOCK 64
/* The horizontal pass keeps two 32-column tiles; the IIR reads at most
 * 2 * radius columns back, so the radius may not exceed 16. */
#define SS2C_BLUR_MAX_RADIUS 16

/* YUV -> linear RGB constants, evaluated on the host with the float
 * expressions of ssimulacra2.c::picture_to_linear_rgb. */
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

/* One scale's SSIM / edge-difference combine. Three compact planes per
 * buffer; `pixels` = width x height of the scale. */
typedef struct Ss2cCombineArgs {
    const float *mu1;
    const float *mu2;
    const float *s11;
    const float *s22;
    const float *s12;
    const float *img1;
    const float *img2;
    double *partials; /* [channel][group][sum] */
    size_t pixels;
    unsigned groups;
    unsigned pad_;
} Ss2cCombineArgs;

extern const unsigned char ssimulacra2_blur_ptx[];
extern const unsigned char ssimulacra2_device_ptx[];

#endif /* FEATURE_SSIMULACRA2_CUDA_H_ */
