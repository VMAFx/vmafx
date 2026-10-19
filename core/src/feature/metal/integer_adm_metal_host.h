/**
 *  Copyright 2016-2020 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2 AND BSD-2-Clause-Patent
 *
 *  The host side of integer_adm_metal that does not touch the Metal API:
 *  geometry, buffer sizes, the kernels each scale runs, the uniforms and the
 *  conclusion of the reductions into the extractor's scores. Plain C, so the
 *  Objective-C++ dispatch (integer_adm_metal.mm) and the device-free host
 *  replay of the kernels (core/test/test_metal_integer_adm_host_replay.c,
 *  ADR-1806) run the same code.
 *
 *  The uniforms carry the CPU's shifts and rounding terms and each scale is
 *  concluded by the CPU's own adm_cm_result() / adm_csf_den_result() and their
 *  i4_ forms, on the contexts integer_adm.c builds (as integer_adm_cuda.c does,
 *  ADR-1416), so no fixed-point table or score formula has a second copy here
 *  (T-METAL-INTEGER-ADM-TWIN-DEFECTS-2026-10-05).
 */

#ifndef VMAF_FEATURE_METAL_INTEGER_ADM_METAL_HOST_H_
#define VMAF_FEATURE_METAL_INTEGER_ADM_METAL_HOST_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "metal_integer_adm_uniforms.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IADM_METAL_NUM_SCALES 4
#define IADM_METAL_MAX_STAGES 5u

/* NOLINTBEGIN(modernize-use-using,performance-enum-size): C header included by C
 * and C++ translation units; C cannot spell `using` or fixed underlying enum types
 * across required toolchains. ADR-0141. */

/* The options of integer_adm.c that the twin reads. */
typedef struct IadmMetalOptions {
    double adm_enhn_gain_limit;
    double adm_norm_view_dist;
    double adm_csf_scale;
    double adm_csf_diag_scale;
    double adm_noise_weight;
    double adm_min_val;
    double adm_dlm_weight;
    double adm_p_norm;
    int adm_ref_display_height;
    int adm_csf_mode;
    bool adm_skip_aim;
    bool adm_skip_scale0;
} IadmMetalOptions;

/* Frame geometry: the input planes and the DWT bands of each scale. */
typedef struct IadmMetalGeometry {
    unsigned w;
    unsigned h;
    unsigned bpc;
    unsigned buf_stride;
    unsigned scale_w[IADM_METAL_NUM_SCALES];
    unsigned scale_h[IADM_METAL_NUM_SCALES];
    unsigned half_w[IADM_METAL_NUM_SCALES];
    unsigned half_h[IADM_METAL_NUM_SCALES];
    /* Reduction threadgroups: three bands times the rows of adm_border(). */
    unsigned wg_count[IADM_METAL_NUM_SCALES];
} IadmMetalGeometry;

/* The kernel functions of integer_adm.metal. */
typedef enum IadmMetalKernel {
    IADM_METAL_DWT_VERT_8BPC = 0,
    IADM_METAL_DWT_VERT_16BPC,
    IADM_METAL_DWT_VERT_S1,
    IADM_METAL_DWT_VERT_S123,
    IADM_METAL_DWT_HORI_S0,
    IADM_METAL_DWT_HORI_S123,
    IADM_METAL_DECOUPLE_CSF_S0,
    IADM_METAL_DECOUPLE_CSF_S123,
    IADM_METAL_CSF_CM_S0,
    IADM_METAL_CSF_CM_S123,
    IADM_METAL_AIM_CM_S0,
    IADM_METAL_AIM_CM_S123,
    IADM_METAL_KERNEL_COUNT
} IadmMetalKernel;

/* The device buffers. */
typedef enum IadmMetalBuffer {
    IADM_METAL_BUF_SOURCE = 0, /* one input plane, u8 or u16 */
    IADM_METAL_BUF_DWT_TMP,    /* vertical DWT rows of one plane, int32 */
    IADM_METAL_BUF_BAND,       /* a, h, v, d of one plane at one scale */
    IADM_METAL_BUF_CSF,        /* three CSF bands, int32 at the scale-0 size */
    IADM_METAL_BUF_ACCUM       /* the reduction slots of one scale */
} IadmMetalBuffer;

/* One dispatch: a kernel over groups[] threadgroups of threads[] threads. */
typedef struct IadmMetalStage {
    IadmMetalKernel entry; /* not `kernel`: an MSL keyword, a macro in the host shim */
    uint32_t groups[3];
    uint32_t threads[3];
} IadmMetalStage;

/* Everything integer_adm.c emits for a frame. */
typedef struct IadmMetalScores {
    double scores[2 * IADM_METAL_NUM_SCALES]; /* per scale: numerator, denominator */
    double scale_scores[IADM_METAL_NUM_SCALES];
    double score;
    double score_aim;
    double score_adm3;
    double score_num;
    double score_den;
} IadmMetalScores;

/* NOLINTEND(modernize-use-using,performance-enum-size) */

/* The function name of `entry` in integer_adm.metal, NULL out of range. */
const char *iadm_metal_kernel_name(IadmMetalKernel entry);

void iadm_metal_geometry(IadmMetalGeometry *g, unsigned w, unsigned h, unsigned bpc);

/* Size of buffer `buffer` (`scale` for the band and accumulator buffers). */
size_t iadm_metal_buffer_bytes(const IadmMetalGeometry *g, IadmMetalBuffer buffer, int scale);

/* 0 when every scale's CSF weights convert to fixed point, else -EINVAL. */
int iadm_metal_check_options(const IadmMetalOptions *o);

/* The uniforms of `scale`. */
void iadm_metal_uniforms(const IadmMetalOptions *o, const IadmMetalGeometry *g, int scale,
                         IadmDims *d, IadmCsf *c);

/* The dispatches of `scale`, in order; returns their count. */
unsigned iadm_metal_stages(const IadmMetalOptions *o, const IadmMetalGeometry *g, int scale,
                           IadmMetalStage stages[IADM_METAL_MAX_STAGES]);

/* The dispatches of `scale` at viewing distance `view` (ADR-2795), in order;
 * returns their count. View 0 runs every stage of iadm_metal_stages(); a
 * later view only the stages after the DWT, which read the bands view 0's
 * DWT left, with `o` at that view's distance and its own reduction buffers. */
unsigned iadm_metal_view_stages(const IadmMetalOptions *o, const IadmMetalGeometry *g, int scale,
                                unsigned view, IadmMetalStage stages[IADM_METAL_MAX_STAGES]);

/* The scores of frame `index` from the four scales' reduction buffers, as
 * integer_adm.c's integer_compute_adm() and extract() form them. Returns the
 * first error of the CPU's checks (-EINVAL for a non-finite or undefined
 * aggregate). */
int iadm_metal_scores(const IadmMetalOptions *o, const IadmMetalGeometry *g,
                      const uint32_t *const accum[IADM_METAL_NUM_SCALES], unsigned index,
                      IadmMetalScores *out);

#ifdef __cplusplus
}
#endif

#endif /* VMAF_FEATURE_METAL_INTEGER_ADM_METAL_HOST_H_ */
