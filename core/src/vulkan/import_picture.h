/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 *
 *  Bridge between the per-state Vulkan import slots
 *  (libvmaf/src/vulkan/import.c) and libvmaf.c's read-pictures
 *  pipeline. T7-29 part 2 (ADR-0186).
 *
 *  The builder wraps the staging VkBuffers (host-visible
 *  HOST_VISIBLE | HOST_COHERENT) into VmafPicture handles whose
 *  release callback is a no-op — the buffers are owned by the
 *  state, not by the picture pool, so vmaf_picture_unref must
 *  not free them.
 *
 *  This header deliberately does NOT pull in <volk.h> so libvmaf.c
 *  can include it without inheriting Vulkan's symbol surface.
 */

#ifndef LIBVMAF_VULKAN_IMPORT_PICTURE_H_
#define LIBVMAF_VULKAN_IMPORT_PICTURE_H_

#include "libvmaf/libvmaf_vulkan.h"
#include "libvmaf/picture.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Build VmafPicture handles for the ref + dis staging buffers
 * pinned by previous vmaf_vulkan_import_image() calls. Both
 * pictures must have been imported for `index` first; the
 * function returns -EINVAL otherwise.
 *
 * The output VmafPictures are YUV400P (luma-only — chroma is
 * outside T7-29 part 2 scope), reference-counted via fresh
 * VmafRefs, and carry a no-op release callback so the
 * vmaf_picture_unref calls inside vmaf_read_pictures do not
 * touch the state-owned VkBuffer mappings.
 *
 * On success the function clears the per-buffer pending flags;
 * the staging buffers can be reused on the next frame.
 */
int vmaf_vulkan_state_build_pictures(VmafVulkanState *state, unsigned index, VmafPicture *out_ref,
                                     VmafPicture *out_dis);

#ifdef __cplusplus
}
#endif

#endif /* LIBVMAF_VULKAN_IMPORT_PICTURE_H_ */
