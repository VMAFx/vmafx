/**
 *  Copyright (c) the JPEG XL Project Authors.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent AND BSD-3-Clause
 *
 *  Shared between the host twin (ssimulacra2_hip.c) and its device module
 *  (ssimulacra2/ssimulacra2_device.hip): the launch geometry constants and the
 *  kernel-argument structs. Both sides pass the structs by value through
 *  hipModuleLaunchKernel(), so their layout must stay plain C (fixed-width
 *  scalars and pointers only, no C++ members). They are declared as struct
 *  tags, not typedefs, so the one declaration is idiomatic in both languages.
 */

#ifndef FEATURE_SSIMULACRA2_HIP_H_
#define FEATURE_SSIMULACRA2_HIP_H_

#include <stddef.h>
#include <stdint.h>

#define SS2H_NUM_SCALES 6
#define SS2H_CHANNELS 3
#define SS2H_IMAGES 2 /* reference, distorted */
/* Per channel: SSIM L1, SSIM L4, artifact L1, artifact L4, detail L1,
 * detail L4 (the four edge sums in edge_diff_map order). */
#define SS2H_SUMS 6
#define SS2H_PAIR 2 /* hi, lo */
#define SS2H_LANE_FLOATS ((size_t)SS2H_SUMS * SS2H_PAIR)
#define SS2H_TOTAL_FLOATS ((size_t)SS2H_NUM_SCALES * SS2H_CHANNELS * SS2H_LANE_FLOATS)
#define SS2H_BLUR_BLOCK 64
/* Row pass: one wavefront of SS2H_ROW_TILE lanes per SS2H_ROW_TILE rows, the
 * rows staged through LDS SS2H_ROW_TILE columns at a time. The walk reads its
 * left input from the previous tile, so the IIR radius must stay at most
 * SS2H_ROW_TILE / 2 (sigma 1.5 gives 5). */
#define SS2H_ROW_TILE 32
#define SS2H_ROW_COLS 16
#define SS2H_PIX_BX 16
#define SS2H_PIX_BY 8
#define SS2H_ELEM_BLOCK 256
/* The reduction tree is a function of the plane size only (ADR-1363): the
 * same group count, work-group width and per-lane stride as the SYCL twin, so
 * the two backends add the per-pixel terms in the same order. */
#define SS2H_REDUCE_WG 256
#define SS2H_MAX_GROUPS 256
#define SS2H_PIXELS_PER_ITEM 16

/* YUV -> RGB constants, evaluated on the host with the float expressions of
 * ssimulacra2.c::picture_to_linear_rgb. */
struct Ss2hYuvCoefficients {
    float inv_peak;
    float y_off;
    float y_scale;
    float c_off;
    float c_scale;
    float cr_r;
    float cb_g;
    float cr_g;
    float cb_b;
};

/* Recursive Gaussian of ssimulacra2.c::create_recursive_gaussian. */
struct Ss2hIir {
    float n2[3];
    float d1[3];
    int radius;
};

/* ssimulacra2_yuv_to_linear: both images of one frame, packed raw planes. */
struct Ss2hYuvArgs {
    const void *plane[SS2H_IMAGES][SS2H_CHANNELS];
    float *out[SS2H_IMAGES];
    unsigned plane_w[SS2H_CHANNELS];
    unsigned plane_h[SS2H_CHANNELS];
    unsigned width;
    unsigned height;
    unsigned wide; /* 16-bit samples */
    struct Ss2hYuvCoefficients k;
};

/* ssimulacra2_xyb and ssimulacra2_downsample: both images, three planes. */
struct Ss2hPlanesArgs {
    const float *in[SS2H_IMAGES];
    float *out[SS2H_IMAGES];
    unsigned width; /* input scale */
    unsigned height;
    unsigned out_width; /* downsample only */
    unsigned out_height;
};

/* ssimulacra2_blur_rows / ssimulacra2_blur_cols: three planes of one scale.
 * The row pass blurs in * in2 when `in2` is set (the products ref^2, dis^2
 * and ref*dis of ssimulacra2.c::multiply_3plane, formed while loading) and
 * `in` alone otherwise; the column pass reads `in` only. */
struct Ss2hBlurArgs {
    const float *in;
    const float *in2;
    float *out;
    unsigned width;
    unsigned height;
    struct Ss2hIir iir;
};

/* ssimulacra2_combine_partials: the per-pixel terms of one scale. */
struct Ss2hCombineArgs {
    const float *mu1;
    const float *mu2;
    const float *s11;
    const float *s22;
    const float *s12;
    const float *img1;
    const float *img2;
    float *partials; /* [channel][group][sum][pair] */
    size_t plane;
    unsigned groups;
};

/* ssimulacra2_combine_final: one scale's group partials -> its six sums. */
struct Ss2hFinalArgs {
    const float *partials;
    float *totals; /* this scale's [channel][sum][pair] */
    unsigned groups;
};

extern const unsigned char ssimulacra2_device_hsaco[];

#endif /* FEATURE_SSIMULACRA2_HIP_H_ */
