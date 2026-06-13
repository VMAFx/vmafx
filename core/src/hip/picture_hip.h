/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 */

#ifndef LIBVMAF_HIP_PICTURE_HIP_H_
#define LIBVMAF_HIP_PICTURE_HIP_H_

#include <stddef.h>

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

int vmaf_hip_picture_alloc(VmafHipContext *ctx, void **out, size_t size);
void vmaf_hip_picture_free(VmafHipContext *ctx, void *buf);

#ifdef __cplusplus
}
#endif

#endif /* LIBVMAF_HIP_PICTURE_HIP_H_ */
