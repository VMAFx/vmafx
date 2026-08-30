/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 */

#ifndef LIBVMAF_METAL_DISPATCH_STRATEGY_H_
#define LIBVMAF_METAL_DISPATCH_STRATEGY_H_

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Returns 1 if the Metal backend can dispatch the named feature on
 * the given context, 0 otherwise. Accepts both Metal extractor names
 * (for example `float_psnr_metal`) and the provided feature keys
 * emitted by those extractors (for example `psnr_y`). Same shape as
 * the Vulkan / SYCL / CUDA / HIP dispatch predicates.
 */
int vmaf_metal_dispatch_supports(const VmafMetalContext *ctx, const char *feature);

#ifdef __cplusplus
}
#endif

#endif /* LIBVMAF_METAL_DISPATCH_STRATEGY_H_ */
