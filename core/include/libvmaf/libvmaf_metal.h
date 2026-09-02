/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Licensed under the BSD+Patent License (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      https://opensource.org/licenses/BSDplusPatent
 */

/**
 * @file libvmaf_metal.h
 * @brief Metal (Apple Silicon) backend public API — ADR-0361 / T8-1 through T8-1d.
 *
 * **Status: live.** The runtime (T8-1b, ADR-0420) and the first kernel set
 * (T8-1c/d, ADR-0421 — `integer_motion_v2.metal` + 7 additional
 * feature-extractor MSL shaders) are fully shipped. All 8 `.mm` dispatch
 * translation units and 8 `.metal` shaders are compiled, linked, and
 * registered. Entry points return 0 on Apple-Family-7+ (M1 and later) or
 * -ENODEV on Intel Macs and non-Apple hosts.
 *
 * Mirrors the HIP scaffold (ADR-0212) and the Vulkan scaffold (ADR-0175) —
 * see ADR-0361 for the audit-first decision and rollout sequence.
 *
 * When libvmaf was built without `-Denable_metal=enabled` (or built on
 * a non-macOS host where the Metal framework auto-probe failed), every
 * entry point returns -ENOSYS unconditionally and the runtime treats
 * Metal as disabled.
 *
 * Header purity: the Metal runtime types (`id<MTLDevice>`,
 * `id<MTLCommandQueue>`, `id<MTLBuffer>`) cross the ABI as
 * `uintptr_t` to keep this header free of `<Metal/Metal.h>` /
 * `<Metal/Metal.hpp>`. Cast on the caller side. Same convention the
 * HIP backend uses for `hipStream_t` / `hipEvent_t` (per ADR-0212)
 * and the Vulkan backend uses for `VkDevice` / `VkQueue` (per
 * ADR-0184).
 *
 * Apple-platform-only: device selection is gated on `MTLGPUFamily.Apple7`
 * (M1 and later). Intel Macs and non-Apple hosts surface as -ENODEV from
 * `vmaf_metal_state_init`. See ADR-0361 §"Apple Silicon-only" for reasoning.
 * CLI exposure: `--metal_device <N>` / `--no_metal` / `--backend metal`
 * (ADR-0422).
 */

#ifndef LIBVMAF_METAL_H_
#define LIBVMAF_METAL_H_

#include <stddef.h>
#include <stdint.h>

#include <libvmaf/libvmaf.h>
#include <libvmaf/picture.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Returns 1 if libvmaf was built with Metal support
 * (-Denable_metal=enabled or auto-probe succeeded on macOS), 0
 * otherwise. Cheap to call; no Metal runtime is touched until @ref
 * vmaf_metal_state_init().
 *
 * @return 1 if Metal support was compiled in, 0 otherwise.
 */
VMAF_EXPORT int vmaf_metal_available(void);

/**
 * Opaque handle to a Metal-backed scoring state. One state pins one
 * Metal device + command queue; callers that want multi-GPU fan-out
 * create one state per device. Same lifetime model as
 * `VmafCudaState` / `VmafVulkanState` / `VmafHipState`.
 */
typedef struct VmafMetalState VmafMetalState;

/**
 * Configuration passed to @ref vmaf_metal_state_init. POD struct; safe
 * to zero-initialise (yields device_index = 0).
 */
typedef struct VmafMetalConfiguration {
    int device_index; /**< -1 = system default Metal device (typical Apple Silicon path) */
    int flags;        /**< reserved for future use; pass 0 */
} VmafMetalConfiguration;

/**
 * Allocate a VmafMetalState. Picks the device by index; -1 selects the
 * system default Metal device (`MTLCreateSystemDefaultDevice` on
 * Apple Silicon).
 *
 * @param out  receives the new state handle on success. Pair with
 *             @ref vmaf_metal_state_free.
 * @param cfg  device selection.
 *
 * @return 0 on success, -ENOSYS when built without Metal, -ENODEV when
 *         no Apple-Family-7+ device is available (Intel Mac, non-macOS
 *         host, or M-series device unavailable), -EINVAL on bad
 *         arguments.
 */
VMAF_EXPORT int vmaf_metal_state_init(VmafMetalState **out, VmafMetalConfiguration cfg);

/**
 * Hand the Metal state to a VmafContext. After import, the context
 * borrows the state pointer for the duration of its lifetime; the
 * caller still owns the state and must free it with
 * @ref vmaf_metal_state_free after vmaf_close(). Same lifetime model as
 * the SYCL + Vulkan + HIP backends.
 *
 * @param ctx    live VmafContext (from vmaf_init()).
 * @param state  state handle previously allocated via
 *               @ref vmaf_metal_state_init.
 *
 * @return 0 on success, -EINVAL on bad arguments, -ENOSYS when built
 *         without Metal.
 */
VMAF_EXPORT int vmaf_metal_import_state(VmafContext *ctx, VmafMetalState *state);

/**
 * Release a state previously allocated via @ref vmaf_metal_state_init.
 * Safe to pass `NULL` or a state that was never imported. After import
 * the caller is still responsible for freeing — call this after
 * vmaf_close() to avoid using a state the context still references.
 *
 * @param state  pointer to the state handle to release; set to NULL on
 *               return.
 */
VMAF_EXPORT void vmaf_metal_state_free(VmafMetalState **state);

/**
 * Enumerate Apple-Family-7+ Metal devices visible to the runtime.
 * Prints one line per device with its ordinal, name, and GPU family.
 *
 * @return Device count, or -ENOSYS when built without Metal.
 */
VMAF_EXPORT int vmaf_metal_list_devices(void);

/* -----------------------------------------------------------------
 * IOSurface picture import — ADR-0423 scaffold (T8-IOS).
 *
 * Caller holds an external GPU-resident frame (e.g. from VideoToolbox
 * hardware decode where frames arrive as CVPixelBufferRef backed by
 * an IOSurface) and hands its opaque handle to libvmaf. FFmpeg surfaces
 * it as `AVFrame->data[3] -> CVPixelBufferRef` from
 * `AV_HWDEVICE_TYPE_VIDEOTOOLBOX`; the caller extracts the IOSurface
 * with `CVPixelBufferGetIOSurface` before passing it here.
 *
 * Same-device contract: source IOSurfaces are bound to whichever
 * MTLDevice rendered them. libvmaf compute must run on the same
 * device, hence @ref vmaf_metal_state_init_external. On a single-GPU
 * Apple Silicon Mac (the common case) there is only one
 * Apple-Family-7+ device and the constraint is trivially satisfied;
 * the external-init entry point exists so multi-GPU Mac Pro hosts get
 * a deterministic device match.
 *
 * Real behaviour / deferred true zero-copy:
 * The v1 implementation locks the source IOSurface via `IOSurfaceLock`
 * and performs a synchronous CPU memcpy into a shared-storage
 * `VmafPicture` buffer (shared CPU/GPU unified memory). True zero-copy
 * direct GPU texture/buffer binding without CPU memcpy
 * (`[MTLDevice newTextureWithDescriptor:iosurface:plane:]` or direct
 * buffer pointer mapping with GPU completion/fence tracking) is
 * deferred under GAP-METAL-IOSURFACE-NOT-TRUE-ZERO-COPY.
 * ----------------------------------------------------------------- */

/**
 * Pre-existing Metal handles supplied by the caller. Used by
 * @ref vmaf_metal_state_init_external so libvmaf compute runs on
 * the same MTLDevice as the source IOSurfaces (same constraint
 * the Vulkan import path enforces — see ADR-0184). Handles cross
 * the ABI as `uintptr_t` to keep this header free of
 * `<Metal/Metal.h>`; cast on the caller side.
 *
 * Lifetime: libvmaf does NOT take ownership. The caller (typically
 * FFmpeg's `AVHWDeviceContext` / `AVMetalDeviceContext` when the
 * MoltenVK or VideoToolbox bridge lands) keeps them alive at least
 * until @ref vmaf_metal_state_free returns.
 */
typedef struct VmafMetalExternalHandles {
    uintptr_t device;        /**< id<MTLDevice> (optional; 0 = use MTLCreateSystemDefaultDevice) */
    uintptr_t command_queue; /**< id<MTLCommandQueue> (optional; 0 = create internally) */
} VmafMetalExternalHandles;

/**
 * Allocate a VmafMetalState that adopts caller-supplied Metal
 * handles instead of creating its own MTLDevice / MTLCommandQueue.
 * Required when the caller will pass external IOSurface handles
 * via @ref vmaf_metal_picture_import — the IOSurface's backing
 * MTLTexture is only addressable on the device that mapped it.
 *
 * `handles.device == 0` falls back to `MTLCreateSystemDefaultDevice`
 * (FFmpeg n8.1.1 path, until upstream exposes an
 * `AVMetalDeviceContext`). `handles.command_queue == 0` creates a
 * fresh command queue from the resolved device. The
 * Apple-Family-7+ gate still applies in every case — passing an
 * Intel-Mac MTLDevice or running on a non-Apple host returns
 * -ENODEV.
 *
 * Mutually exclusive with @ref vmaf_metal_state_init in a single
 * process context: pick one.
 *
 * @param out      receives the new state handle on success. Pair with
 *                 @ref vmaf_metal_state_free.
 * @param handles  caller-owned MTLDevice / MTLCommandQueue handles.
 *
 * @return 0 on success, -EINVAL on bad arguments, -ENODEV on a
 *         non-Apple-Family-7 device, -ENOMEM on allocation failure.
 */
VMAF_EXPORT int vmaf_metal_state_init_external(VmafMetalState **out,
                                               VmafMetalExternalHandles handles);

/**
 * Import an external IOSurface (typically pulled from a
 * `CVPixelBufferRef` via `CVPixelBufferGetIOSurface`) into the
 * libvmaf Metal compute pipeline. Caller retains ownership of the
 * underlying IOSurface; libvmaf locks the surface read-only and
 * memcpys the requested plane into a shared-storage VmafPicture.
 *
 * @param state    Metal state handle.
 * @param iosurface IOSurfaceRef (cast to uintptr_t).
 * @param plane    Plane index (0 = Y, 1 = U, 2 = V — caller is
 *                 responsible for de-interleaving biplanar
 *                 VideoToolbox formats before calling).
 * @param w        Luma (frame) width.
 * @param h        Luma (frame) height.
 * @param bpc      Bits per component (8 / 10 / 12 / 16).
 * @param is_ref   1 = reference frame, 0 = distorted.
 * @param index    Frame index (matches the index passed to
 *                 @ref vmaf_metal_read_imported_pictures).
 *
 * @return 0 on success, -EINVAL on bad arguments, -EIO on
 *         IOSurface lock failure, -ENOMEM on allocation failure.
 */
VMAF_EXPORT int vmaf_metal_picture_import(VmafMetalState *state, uintptr_t iosurface,
                                          unsigned plane, unsigned w, unsigned h, unsigned bpc,
                                          int is_ref, unsigned index);

/**
 * Block until all previously-submitted Metal compute work on
 * `state` has finished. Mirrors `vmaf_vulkan_wait_compute`.
 * Used by FFmpeg-side filters before reusing imported IOSurfaces
 * in the next frame.
 *
 * Currently a no-op: the v1 import path is synchronous CPU memcpy
 * (data is host-visible by the time @ref vmaf_metal_picture_import
 * returns). A future async path replaces this with a per-frame
 * MTLSharedEvent drain.
 *
 * @param state  Metal state handle.
 *
 * @return 0 on success, -EINVAL on NULL state.
 */
VMAF_EXPORT int vmaf_metal_wait_compute(VmafMetalState *state);

/**
 * Trigger a libvmaf score read for the imported reference +
 * distorted IOSurfaces at `index`.
 *
 * Requires all 3 planes (Y/U/V) to have been imported for both
 * ref and dis at the matching index.
 *
 * @param ctx    live VmafContext (from vmaf_init()).
 * @param index  frame index matching the value passed to
 *               @ref vmaf_metal_picture_import.
 *
 * @return 0 on success, -EINVAL on missing imports or stale state.
 */
VMAF_EXPORT int vmaf_metal_read_imported_pictures(VmafContext *ctx, unsigned index);

#ifdef __cplusplus
}
#endif

#endif /* LIBVMAF_METAL_H_ */
