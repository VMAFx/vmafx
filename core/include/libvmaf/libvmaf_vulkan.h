/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 *
 *  Licensed under the BSD+Patent License (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      https://opensource.org/licenses/BSDplusPatent
 */

/**
 * @file libvmaf_vulkan.h
 * @brief Vulkan backend public API.
 *
 * Scaffolded by ADR-0175 / T5-1; full runtime landed via ADR-0178
 * (T5-1b). Kernel coverage matrix is complete for the default model
 * (VIF, ADM, motion, motion_v2, ssimulacra2, plus the GPU long-tail
 * batches per ADR-0193); see docs/backends/vulkan/overview.md.
 *
 * When libvmaf was built without `-Denable_vulkan=enabled`, every entry
 * point returns -ENOSYS unconditionally and the runtime treats vulkan as
 * disabled.
 */

#ifndef LIBVMAF_VULKAN_H_
#define LIBVMAF_VULKAN_H_

#include <stddef.h>
#include <stdint.h>

#include "libvmaf.h"
#include "libvmaf/macros.h"
#include "picture.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Returns 1 if libvmaf was built with Vulkan support
 * (-Denable_vulkan=enabled), 0 otherwise. Cheap to call; no Vulkan
 * runtime is touched until @ref vmaf_vulkan_state_init().
 */
VMAF_EXPORT int vmaf_vulkan_available(void);

/**
 * Opaque handle to a Vulkan-backed scoring state. One state pins one
 * VkInstance + VkDevice + compute queue; callers that want multi-GPU
 * fan-out create one state per device.
 */
typedef struct VmafVulkanState VmafVulkanState;

typedef struct VmafVulkanConfiguration {
    int device_index;      /**< -1 = first device with compute queue */
    int enable_validation; /**< non-zero: load VK_LAYER_KHRONOS_validation */

    /**
     * v2 async pending-fence ring depth (max frames in flight). 0 selects
     * the canonical default (4); values are clamped to [1, 8] internally.
     * The ring is materialised lazily on the first @ref
     * vmaf_vulkan_import_image, so this field must be set before that
     * call to take effect. See ADR-0251.
     */
    unsigned max_outstanding_frames;

    /**
     * Bit-exact-strict opt-in (ADR-0509, supersedes ADR-0492's hard
     * refusal). When non-zero, the backend init refuses to attach to
     * devices that do not expose `VkPhysicalDeviceFeatures::shaderFloat64`
     * and returns `-ENOTSUP`. Default (0) auto-falls-back to the fp32
     * VIF shader variant on such devices; the per-frame VMAF delta vs
     * the fp64 path is bounded by ~1e-4 on the Netflix golden 576x324
     * corpus — well within the cross-backend tolerance. Used by parity
     * test harnesses that need to assert the fp64 path is taken.
     */
    int require_fp64;
} VmafVulkanConfiguration;

/**
 * Allocate a VmafVulkanState. Picks the device by index; -1 selects the
 * first device that exposes a compute queue family.
 *
 * @return 0 on success, -ENOSYS when built without Vulkan, -ENODEV when
 *         no compatible device is found, -EINVAL on bad arguments.
 */
VMAF_EXPORT int vmaf_vulkan_state_init(VmafVulkanState **out, VmafVulkanConfiguration cfg);

/**
 * Pre-existing Vulkan handles supplied by the caller, used by
 * @ref vmaf_vulkan_state_init_external so libvmaf compute runs on
 * the *same* VkDevice as the source VkImages. The handles cross
 * the ABI as `uintptr_t` to keep the header free of
 * `<vulkan/vulkan.h>` (same pattern as the import_image surface
 * — see ADR-0184). Cast from VkInstance / VkPhysicalDevice /
 * VkDevice / VkQueue on the caller side.
 *
 * Lifetime: libvmaf does NOT take ownership of the supplied
 * handles. The caller (typically FFmpeg's `AVHWDeviceContext` —
 * `AVVulkanDeviceContext`) keeps them alive at least until
 * @ref vmaf_vulkan_state_free returns.
 */
typedef struct VmafVulkanExternalHandles {
    uintptr_t instance;        /**< VkInstance */
    uintptr_t physical_device; /**< VkPhysicalDevice */
    uintptr_t device;          /**< VkDevice */
    uintptr_t queue;           /**< VkQueue (compute-capable) */
    uint32_t queue_family_index;
    uint32_t api_version; /**< e.g. VK_API_VERSION_1_3 */
} VmafVulkanExternalHandles;

/**
 * Allocate a VmafVulkanState that adopts caller-supplied Vulkan
 * handles instead of creating its own VkInstance / VkDevice.
 * Required when the caller will pass external VkImage handles
 * via @ref vmaf_vulkan_import_image — those handles are only
 * valid on the device that created them, so libvmaf compute must
 * run on the same device.
 *
 * Mutually exclusive with @ref vmaf_vulkan_state_init in a
 * single process: the volk function-pointer table is global and
 * gets re-bound to the supplied instance/device. Use one or the
 * other.
 *
 * @return 0 on success, -ENOSYS when built without Vulkan,
 *         -EINVAL on bad arguments, -ENOMEM on allocation
 *         failure.
 */
VMAF_EXPORT int vmaf_vulkan_state_init_external(VmafVulkanState **out,
                                                VmafVulkanExternalHandles handles);

/**
 * Inspect the v2 async pending-fence ring depth a state was
 * initialised with. Returns the clamped value of @ref
 * VmafVulkanConfiguration::max_outstanding_frames (always in
 * [1, 8]); 0 when @p state is NULL or libvmaf was built without
 * Vulkan support. ADR-0251 follow-up #3.
 */
VMAF_EXPORT unsigned vmaf_vulkan_state_max_outstanding_frames(const VmafVulkanState *state);

/**
 * Hand the Vulkan state to a VmafContext. The context borrows the
 * state pointer for the duration of its lifetime; the caller still
 * owns the state and must free it with @ref vmaf_vulkan_state_free
 * after vmaf_close(). Same lifetime model as the SYCL backend.
 */
VMAF_EXPORT int vmaf_vulkan_import_state(VmafContext *ctx, VmafVulkanState *state);

/**
 * Per-picture-pool preallocation strategy. Mirrors the CUDA / SYCL
 * surfaces (`VmafCudaPicturePreallocationMethod`,
 * `VmafSyclPicturePreallocationMethod`).
 *
 * - `NONE`: no preallocation; the caller drives `vmaf_picture_alloc`
 *           or feeds frames via the import-image zero-copy path.
 * - `HOST`: pictures live in regular host memory (`vmaf_picture_alloc`).
 *           Useful when the score consumer is CPU-side but the
 *           upload path runs on the Vulkan compute queue.
 * - `DEVICE`: pictures are backed by host-visible Vulkan buffers
 *           (VMA `AUTO_PREFER_HOST`) — the same memory the kernel
 *           descriptor sets bind to. Lets the caller write directly
 *           into the buffer the shaders read, removing the host →
 *           device staging copy.
 *
 * ADR-0238 closes the API parity gap with CUDA / SYCL.
 */
enum VmafVulkanPicturePreallocationMethod {
    VMAF_VULKAN_PICTURE_PREALLOCATION_METHOD_NONE = 0,
    VMAF_VULKAN_PICTURE_PREALLOCATION_METHOD_HOST,
    VMAF_VULKAN_PICTURE_PREALLOCATION_METHOD_DEVICE,
};

typedef struct VmafVulkanPictureConfiguration {
    struct {
        unsigned w, h;
        unsigned bpc;
        enum VmafPixelFormat pix_fmt;
    } pic_params;
    enum VmafVulkanPicturePreallocationMethod pic_prealloc_method;
} VmafVulkanPictureConfiguration;

/**
 * Configure and preallocate VmafPictures for use with the Vulkan
 * compute backend. The pool depth is fixed at the canonical
 * `frames-in-flight = 2` (matches the SYCL preallocation contract);
 * pictures are dispensed round-robin via @ref vmaf_vulkan_picture_fetch.
 *
 * @return 0 on success;
 *         -EINVAL on bad arguments or unknown method;
 *         -ENOMEM on allocation failure;
 *         -EBUSY if a pool already exists on @p vmaf;
 *         -ENOSYS when libvmaf was built without Vulkan support.
 */
VMAF_EXPORT int vmaf_vulkan_preallocate_pictures(VmafContext *vmaf,
                                                 VmafVulkanPictureConfiguration cfg);

/**
 * Fetch one preallocated VmafPicture. With
 * `VMAF_VULKAN_PICTURE_PREALLOCATION_METHOD_NONE` (or no prior
 * `preallocate_pictures` call), this falls back to a regular
 * host-backed `vmaf_picture_alloc` so callers can ignore the
 * preallocation surface entirely if they want.
 *
 * @return 0 on success, or a negative errno on failure.
 */
VMAF_EXPORT int vmaf_vulkan_picture_fetch(VmafContext *vmaf, VmafPicture *pic);

/**
 * Release a state previously allocated via @ref vmaf_vulkan_state_init.
 * Safe to pass `NULL` or a state that was never imported. After import
 * the caller is still responsible for freeing — call this after
 * vmaf_close() to avoid using a state the context still references.
 */
VMAF_EXPORT void vmaf_vulkan_state_free(VmafVulkanState **state);

/**
 * Enumerate compute-capable Vulkan devices visible to the runtime.
 * Prints one line per device with its ordinal, name, and API version.
 * Returns the device count or -ENOSYS when built without Vulkan.
 */
VMAF_EXPORT int vmaf_vulkan_list_devices(void);

/**
 * Zero-copy frame import — T7-29 (ADR-0184).
 *
 * The next three entry points mirror the SYCL backend's
 * VAAPI/dmabuf import surface (see libvmaf_sycl.h). They let
 * an FFmpeg-side filter — or any direct C-API caller — hand
 * an externally-decoded VkImage straight to the libvmaf
 * Vulkan compute queue without a CPU readback round-trip.
 *
 * Implementation landed via ADR-0186 (T7-29 parts 2 + 3):
 * `vkCmdCopyImageToBuffer` + timeline-semaphore wait, plus the
 * `libvmaf_vulkan` FFmpeg filter wired by
 * `ffmpeg-patches/0006-libvmaf-add-libvmaf-vulkan-filter.patch`.
 *
 * Header purity: Vulkan handles cross the ABI as `uintptr_t`
 * to keep this header usable from translation units that
 * don't have <vulkan/vulkan.h> in scope. Cast from VkImage /
 * VkSemaphore on the caller side.
 */

/**
 * Import an external VkImage into the libvmaf Vulkan compute
 * pipeline. Caller retains ownership of the underlying VkImage
 * and VkSemaphore.
 *
 * @param state              Vulkan state handle.
 * @param vk_image           VkImage handle (cast to uintptr_t).
 * @param vk_format          VkFormat enum value.
 * @param vk_layout          Current VkImageLayout enum value.
 * @param vk_semaphore       VkSemaphore handle (cast to uintptr_t).
 * @param vk_semaphore_value Wait value — libvmaf will wait until
 *                           the semaphore reaches this value
 *                           before reading the image.
 * @param w                  Frame width.
 * @param h                  Frame height.
 * @param bpc                Bits per component (8 / 10 / 12 / 16).
 * @param is_ref             1 = reference frame, 0 = distorted.
 * @param index              Frame index (matches the index
 *                           passed to vmaf_vulkan_read_imported_pictures).
 *
 * @return 0 on success, -EINVAL on bad args.
 */
VMAF_EXPORT int vmaf_vulkan_import_image(VmafVulkanState *state, uintptr_t vk_image,
                                         uint32_t vk_format, uint32_t vk_layout,
                                         uintptr_t vk_semaphore, uint64_t vk_semaphore_value,
                                         unsigned w, unsigned h, unsigned bpc, int is_ref,
                                         unsigned index);

/**
 * Block until all previously-submitted compute work on `state`
 * has finished. Used by FFmpeg-side filters before reusing
 * imported images in the next frame.
 *
 * @return 0 on success, -EINVAL on bad state.
 */
VMAF_EXPORT int vmaf_vulkan_wait_compute(VmafVulkanState *state);

/**
 * Trigger a libvmaf score read for the imported reference +
 * distorted images at `index`. Mirrors vmaf_read_pictures_sycl
 * but for Vulkan-imported frames.
 *
 * @return 0 on success, -EINVAL on bad state.
 */
VMAF_EXPORT int vmaf_vulkan_read_imported_pictures(VmafContext *ctx, unsigned index);

#ifdef __cplusplus
}
#endif

#endif /* LIBVMAF_VULKAN_H_ */
