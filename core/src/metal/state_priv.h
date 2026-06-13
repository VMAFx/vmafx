/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Private state-struct definitions shared between common.mm and the
 *  IOSurface import TU. Kept out of common.h because consumer TUs
 *  outside libvmaf/src/metal/ (e.g. feature/metal/*.mm) only need the
 *  opaque-pointer accessors — they must not touch struct layout.
 */

#ifndef LIBVMAF_METAL_STATE_PRIV_H_
#define LIBVMAF_METAL_STATE_PRIV_H_

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Mirrors the layout in common.mm; the inner `void *` slots carry
 * __bridge_retained id<MTLDevice> / id<MTLCommandQueue> references
 * — release path is `vmaf_metal_state_free` (common.mm). */
struct VmafMetalContext {
    int device_index;
    void *device;
    void *command_queue;
};

/* The public opaque type. `import_ring` is owned by the IOSurface
 * import TU (picture_import.mm) and freed via
 * `vmaf_metal_state_import_ring_free` on state teardown. NULL when
 * no IOSurface import has been started yet. */
struct VmafMetalState {
    struct VmafMetalContext ctx;
    void *import_ring;
};

#ifdef __cplusplus
}
#endif

#endif /* LIBVMAF_METAL_STATE_PRIV_H_ */
