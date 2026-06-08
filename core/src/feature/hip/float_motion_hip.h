/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  HIP host glue for the float_motion feature extractor — seventh
 *  kernel-template consumer (T7-10b batch-2 / ADR-0373).
 *
 *  Mirrors libvmaf/src/feature/cuda/float_motion_cuda.h. The HSACO
 *  symbol declared here is produced by the meson `hip_hsaco_c_float_motion_score`
 *  custom_target pipeline: `xxd -i -n float_motion_score_hsaco
 *  float_motion_score.hsaco > float_motion_score_hsaco.c`
 *  (same pattern as ADR-0372 / ADR-0374).
 */
#ifndef FEATURE_FLOAT_MOTION_HIP_H_
#define FEATURE_FLOAT_MOTION_HIP_H_

/* HSACO blob embedded by xxd; consumed by `hipModuleLoadData` in
 * `float_motion_hip.c:fm_hip_module_load()`. */
extern const unsigned char float_motion_score_hsaco[];
extern const unsigned int float_motion_score_hsaco_len;

#endif /* FEATURE_FLOAT_MOTION_HIP_H_ */
