/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

#ifndef ARM64_NEON_SSIMULACRA2_HOST_H_
#define ARM64_NEON_SSIMULACRA2_HOST_H_

#include <stddef.h>

/*
 * aarch64 NEON variants of the ssimulacra2 Vulkan-host kernels (ADR-0242).
 * 4-wide float lanes. Same plane_stride convention as the AVX2 sibling:
 * channel p starts at `base + p * plane_stride`, where plane_stride >= w*h.
 *
 * Bit-exact contract: ADR-0161 / ADR-0242 — per-lane scalar cbrtf,
 * `#pragma STDC FP_CONTRACT OFF`, `-ffp-contract=off` compile flag.
 */

void ssimulacra2_host_linear_rgb_to_xyb_neon(const float *lin, float *xyb, unsigned w, unsigned h,
                                             size_t plane_stride);

void ssimulacra2_host_downsample_2x2_neon(const float *in, unsigned iw, unsigned ih, float *out,
                                          unsigned ow, unsigned oh, size_t plane_stride);

#endif /* ARM64_NEON_SSIMULACRA2_HOST_H_ */
