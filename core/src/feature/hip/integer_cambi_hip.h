/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  HIP host glue header for the CAMBI banding-detection feature extractor,
 *  device-resident since ADR-1378 (the HIP port of the SYCL design of
 *  ADR-1357).
 *
 *  The HSACO fat binary is compiled from `integer_cambi/cambi_score.hip`
 *  by hipcc and embedded as a C byte array when `enable_hipcc=true`.
 *  The host code loads it via `hipModuleLoadData` + `hipModuleLaunchKernel`.
 *
 *  When `enable_hipcc=false` (CPU-only build or HIP without a ROCm
 *  toolchain), init() returns -ENOSYS — identical to the scaffold posture
 *  used by all other HIP kernel consumers (ADR-0254 et al.).
 *
 *  cambi_hip_plan() and its companions below lay out the parameter block the
 *  kernels read. They are host-only and built in every HIP build, so the
 *  device-free replay (core/test/test_hip_cambi_device_math.c) runs with the
 *  exact block the extractor uploads.
 */
#ifndef FEATURE_INTEGER_CAMBI_HIP_H_
#define FEATURE_INTEGER_CAMBI_HIP_H_

#include <stddef.h>
#include <stdint.h>

#include "integer_cambi/cambi_hip_device.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef HAVE_HIPCC
/* HSACO fat binary embedded by xxd -i during the meson hipcc pipeline.
 * Defined in the auto-generated `cambi_score_hsaco.c` custom_target output. */
extern const unsigned char cambi_score_hsaco[];
extern const unsigned int cambi_score_hsaco_len;
#endif /* HAVE_HIPCC */

/* The resolved configuration of one extractor instance (cambi.c init). */
typedef struct CambiHipPlanInput {
    unsigned src_width;
    unsigned src_height;
    unsigned src_bpc;
    unsigned proc_width; /* the encode resolution */
    unsigned proc_height;
    unsigned enc_bpc;
    unsigned speedup; /* 1 when the high-res speed-up is in effect */
    unsigned adjusted_window;
    unsigned num_diffs;
    unsigned levels; /* v_band_size */
    unsigned vlt_luma;
    unsigned v_band_base;
    double topk;
    unsigned compute_units; /* sizes the c-values row chunks */
} CambiHipPlanInput;

/* Every scalar and per-scale dimension of the parameter block: the scale
 * walk of cambi_score(), the c-values chunking, spatial_pooling()'s top-K
 * counts and the pooling groups. Buffer pointers are left to the caller. */
void cambi_hip_plan(CambiHipParams *params, const CambiHipPlanInput *in);

/* Point each scale at its image buffers: scale s works in `preproc` or `alt`
 * so that a decimation never reads the buffer it writes. */
void cambi_hip_plan_bind_scales(CambiHipParams *params, uint16_t *preproc, uint16_t *alt,
                                uint16_t *filtered_h, unsigned speedup);

/* Cells of the per-chunk column histograms the largest scale needs. */
size_t cambi_hip_plan_hist_cells(const CambiHipParams *params);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* FEATURE_INTEGER_CAMBI_HIP_H_ */
