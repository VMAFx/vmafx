/**
 *  Copyright 2026 luoye-cpu
 *  SPDX-License-Identifier: EUPL-1.2
 */

/**
 * @file libvmaf_vulkan.h
 * @brief Vulkan backend public API — ADR-3402 (integer path).
 *
 * The Vulkan backend is the fp64-free integer compute path of ADR-3402: it
 * runs the integer ADM shader (the fp64-free adm_angle_flag_i64()
 * reformulation of ADR-1194's predicate) on any Vulkan 1.0+ device that
 * reports shaderInt64 — including Intel iGPUs whose drivers report
 * shaderFloat64 = 0, where the SYCL float_adm twin cannot run.
 *
 * The runtime loads `vulkan-1` / `libvulkan.so.1` dynamically (VK_NO_PROTOTYPES,
 * no SDK linkage, no volk, no VMA); the SPIR-V for the shader is compiled at
 * build time and embedded into the library. The draft scope covers integer ADM
 * only; further Vulkan twins arrive with their own PRs.
 *
 * When libvmaf was built without `-Denable_vulkan=true`, every public entry
 * point in this header returns `-ENOSYS` unconditionally (stubs compiled
 * into libvmaf by `core/src/vulkan/stubs.c` on the engine side and
 * `core/src/compat/libvmaf/backend_absent_vulkan.c` on the compat side)
 * and the runtime treats Vulkan as disabled.
 *
 * Header purity: `VmafVulkanState` crosses the ABI as an opaque handle
 * (the concrete definition lives in `core/src/vulkan/common.h`, behind
 * `<vulkan/vulkan.h>`). Cast on the caller side; callers never touch
 * the inside.
 */

#ifndef LIBVMAF_VULKAN_H_
#define LIBVMAF_VULKAN_H_

/* NOLINTBEGIN(modernize-use-using, modernize-deprecated-headers, performance-enum-size):
 * public C API header. clang-tidy reads it as C++ and proposes `using`,
 * `<cstdint>` and a narrower enum base type; all three are wrong here. This
 * header has to compile as C for every consumer of the shipped library, where
 * `using` does not exist and the C spellings of the standard headers are the
 * only ones available, and the enum's base type is part of the ABI the fork
 * publishes. CLAUDE.md rule 12 reserves suppressions for exactly this: a rule
 * that cannot be followed without breaking a load-bearing invariant
 * (ADR-0141). */

#include <stddef.h>
#include <stdint.h>

#include <libvmaf/libvmaf.h>
#include <libvmaf/picture.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Returns 1 if libvmaf was built with Vulkan support
 * (-Denable_vulkan=true), 0 otherwise. Cheap to call; no Vulkan
 * loader is touched until @ref vmaf_vulkan_state_init().
 *
 * @return 1 if Vulkan support was compiled in, 0 otherwise.
 */
VMAF_EXPORT int vmaf_vulkan_available(void);

/**
 * Opaque handle to a Vulkan-backed scoring state. One state pins one
 * Vulkan physical device + compute queue; callers that want multi-GPU
 * fan-out create one state per device. Same lifetime model as
 * `VmafCudaState` / `VmafSyclState` / `VmafHipState`.
 */
typedef struct VmafVulkanState VmafVulkanState;

/**
 * Configuration passed to @ref vmaf_vulkan_state_init. POD struct;
 * zero-initialising it selects the first enumerated device (index 0).
 */
typedef struct VmafVulkanConfiguration {
    int device_index; /**< -1 = auto (discrete before integrated); 0+ = the
                           enumerated physical device of that index */
    int flags;        /**< reserved for future use; pass 0 */
} VmafVulkanConfiguration;

/**
 * Allocate a VmafVulkanState. Loads the Vulkan loader, enumerates the
 * physical devices and brings up the selected one; -1 selects the first
 * compute-capable discrete device, falling back to an integrated one.
 * The device must report shaderInt64 (the ADR-3402 shaders are fp64-free
 * but 64-bit integer arithmetic is load-bearing).
 *
 * @param out  receives the new state handle on success. Pair with
 *             @ref vmaf_vulkan_state_free.
 * @param cfg  device selection.
 *
 * @return 0 on success, -ENOSYS when built without Vulkan, -ENOENT when
 *         no Vulkan loader is found, -ENODEV when no compatible device is
 *         found (none enumerated, shaderInt64 missing, or no compute
 *         queue), -EINVAL on bad arguments, or another negative errno
 *         when the Vulkan runtime fails.
 */
VMAF_EXPORT int vmaf_vulkan_state_init(VmafVulkanState **out, VmafVulkanConfiguration cfg);

/**
 * Hand the Vulkan state to a VmafContext. After import, the context
 * borrows the state pointer for the duration of its lifetime; the
 * caller still owns the state and must free it with
 * @ref vmaf_vulkan_state_free only after vmaf_close() returns exactly 0.
 * A nonzero close retains the state dependency for retry. Same lifetime
 * model as the SYCL / HIP backends.
 *
 * After import, Vulkan-flagged feature extractors (the integer ADM twin
 * `adm_vulkan` of ADR-3402) are selected for their features.
 *
 * @param ctx    live VmafContext (from `vmaf_init()`).
 * @param state  state handle previously allocated via
 *               @ref vmaf_vulkan_state_init.
 *
 * @return 0 on success, -EINVAL when @p ctx or @p state is NULL,
 *         -ENOSYS when built without Vulkan.
 */
VMAF_EXPORT int vmaf_vulkan_import_state(VmafContext *ctx, VmafVulkanState *state);

/**
 * Release a state previously allocated via @ref vmaf_vulkan_state_init.
 * Safe to pass `NULL` or a state that was never imported. After import
 * the caller is still responsible for freeing — call this only after
 * vmaf_close() returns 0. Keep the state alive across any nonzero close
 * result because the retained context still references it.
 *
 * @param state  pointer to the state handle to release; set to NULL on
 *               return.
 */
VMAF_EXPORT void vmaf_vulkan_state_free(VmafVulkanState **state);

#ifdef __cplusplus
}
#endif

/* NOLINTEND(modernize-use-using, modernize-deprecated-headers, performance-enum-size) */

#endif /* LIBVMAF_VULKAN_H_ */
