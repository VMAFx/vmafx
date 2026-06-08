/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Internal bridge between libvmaf.c (HAVE_METAL block) and the
 *  Objective-C++ IOSurface import TU (picture_import.mm). The C-API
 *  surface lives in libvmaf/include/libvmaf/libvmaf_metal.h; this
 *  header carries only the helpers libvmaf.c needs to translate
 *  vmaf_metal_read_imported_pictures() into a vmaf_read_pictures()
 *  call — mirrors libvmaf/src/vulkan/import_picture.h.
 */

#ifndef LIBVMAF_METAL_IMPORT_H_
#define LIBVMAF_METAL_IMPORT_H_

#include "libvmaf/libvmaf_metal.h"
#include "libvmaf/picture.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Pull the pre-built ref/dis VmafPicture pair for `index` out of the
 * import ring. Caller is `vmaf_metal_read_imported_pictures` in
 * libvmaf.c, which hands the pictures straight to
 * `vmaf_read_pictures` (which takes ownership). The slot is cleared
 * before this returns so the next frame at the same ring position
 * starts from a clean state.
 *
 * @return 0 on success, -EINVAL on NULL / unbuilt state / index
 *         mismatch / not-all-planes-filled.
 */
int vmaf_metal_state_build_pictures(VmafMetalState *state, unsigned index, VmafPicture *out_ref,
                                    VmafPicture *out_dis);

/**
 * Release the import ring + any partially-filled pictures still
 * sitting in it. Called from `vmaf_metal_state_free` so a state
 * teardown after a half-imported frame doesn't leak the VmafPicture
 * buffers. Safe to call on a state that never had import_ring set.
 */
void vmaf_metal_state_import_ring_free(VmafMetalState *state);

#ifdef __cplusplus
}
#endif

#endif /* LIBVMAF_METAL_IMPORT_H_ */
