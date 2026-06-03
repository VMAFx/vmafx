/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 *
 *  Picture allocation / lifecycle for the Metal backend — runtime
 *  implementation (T8-1b / ADR-0420). Replaces the C scaffold's
 *  -ENOSYS stubs with `[id<MTLDevice> newBufferWithLength:options:]`
 *  using `MTLResourceStorageModeShared` (zero-copy unified memory on
 *  Apple Silicon).
 *
 *  Unified-memory posture: there is no H2D / D2H staging. Host writes
 *  to `[buffer contents]` are immediately visible to the GPU when the
 *  command buffer is committed (same coherence guarantee Apple
 *  documents for Shared storage on Apple-Family-7+). The runtime stores
 *  the MTLBuffer handle as a bridge-retained `void *` so consumer TUs
 *  can stay pure-C.
 */

#include <errno.h>
#include <stddef.h>

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

extern "C" {
#include "common.h"
#include "picture_metal.h"
}

int vmaf_metal_picture_alloc(VmafMetalContext *ctx, void **out, size_t size)
{
    if (ctx == NULL || out == NULL) {
        return -EINVAL;
    }
    if (size == 0) {
        return -EINVAL;
    }

    void *device_handle = vmaf_metal_context_device_handle(ctx);
    if (device_handle == NULL) {
        return -ENODEV;
    }
    id<MTLDevice> device = (__bridge id<MTLDevice>)device_handle;

    id<MTLBuffer> buf = [device newBufferWithLength:size
                                            options:MTLResourceStorageModeShared];
    if (buf == nil) {
        return -ENOMEM;
    }

    /* Bridge-retain into the C-side `void *` so the buffer survives
     * past this function. Consumer releases via vmaf_metal_picture_free. */
    *out = (__bridge_retained void *)buf;
    return 0;
}

void vmaf_metal_picture_free(VmafMetalContext *ctx, void *buf)
{
    (void)ctx;
    if (buf == NULL) {
        return;
    }
    /* Bridge-transfer back to ARC; the temporary id<MTLBuffer> goes
     * out of scope and ARC releases the +1 retain. */
    id<MTLBuffer> b __attribute__((unused)) =
        (__bridge_transfer id<MTLBuffer>)buf;
}
