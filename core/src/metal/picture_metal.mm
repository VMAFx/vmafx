/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
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

#include <cerrno>
#include <cstddef>

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

extern "C" {
#include "common.h"
#include "picture_metal.h"
}

int vmaf_metal_picture_alloc(VmafMetalContext *ctx, void **out, size_t size)
{
    if (ctx == nullptr || out == nullptr) {
        return -EINVAL;
    }
    if (size == 0) {
        return -EINVAL;
    }

    void *const device_handle = vmaf_metal_context_device_handle(ctx);
    if (device_handle == nullptr) {
        return -ENODEV;
    }
    id<MTLDevice> const device = (__bridge id<MTLDevice>)device_handle;

    id<MTLBuffer> const buf = [device newBufferWithLength:size
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
    if (buf == nullptr) {
        return;
    }
    /* Bridge-transfer back to ARC; the temporary id<MTLBuffer> goes
     * out of scope and ARC releases the +1 retain. */
    id<MTLBuffer> const b __attribute__((unused)) =
        (__bridge_transfer id<MTLBuffer>)buf;
}
