/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 */

#ifndef LIBVMAF_METAL_PICTURE_METAL_H_
#define LIBVMAF_METAL_PICTURE_METAL_H_

#include <stddef.h>

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

int vmaf_metal_picture_alloc(VmafMetalContext *ctx, void **out, size_t size);
void vmaf_metal_picture_free(VmafMetalContext *ctx, void *buf);

#ifdef __cplusplus
}
#endif

#endif /* LIBVMAF_METAL_PICTURE_METAL_H_ */
