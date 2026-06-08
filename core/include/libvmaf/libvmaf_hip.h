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
 * @file libvmaf_hip.h
 * @brief HIP (AMD ROCm) backend public API — scaffolded by ADR-0212 / T7-10.
 *
 * **Status: scaffold only.** Every entry point currently returns -ENOSYS
 * pending a real implementation. The header lands so downstream consumers
 * can compile against the API surface; the kernels (ADM, VIF, motion)
 * arrive in follow-up PRs. Mirrors the Vulkan scaffold (ADR-0175) — see
 * ADR-0212 for the audit-first decision and rollout sequence.
 *
 * When libvmaf was built without `-Denable_hip=true`, every entry point
 * returns -ENOSYS unconditionally and the runtime treats HIP as
 * disabled.
 *
 * Header purity: the HIP runtime types (`hipDevice_t`, `hipStream_t`)
 * cross the ABI as `uintptr_t` to keep this header free of
 * `<hip/hip_runtime.h>`. Cast on the caller side.
 */

#ifndef LIBVMAF_HIP_H_
#define LIBVMAF_HIP_H_

#include <stddef.h>
#include <stdint.h>

#include "libvmaf.h"
#include "picture.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Returns 1 if libvmaf was built with HIP support
 * (-Denable_hip=true), 0 otherwise. Cheap to call; no HIP runtime
 * is touched until @ref vmaf_hip_state_init().
 *
 * @return 1 if HIP support was compiled in, 0 otherwise.
 */
VMAF_EXPORT int vmaf_hip_available(void);

/**
 * Opaque handle to a HIP-backed scoring state. One state pins one
 * HIP device + compute stream; callers that want multi-GPU fan-out
 * create one state per device. Same lifetime model as
 * `VmafCudaState` / `VmafVulkanState`.
 */
typedef struct VmafHipState VmafHipState;

/**
 * Configuration passed to @ref vmaf_hip_state_init. POD struct;
 * safe to zero-initialise (yields device_index = 0, flags = 0).
 */
typedef struct VmafHipConfiguration {
    int device_index; /**< -1 = first HIP device with compute capability */
    int flags;        /**< reserved for future use; pass 0 */
} VmafHipConfiguration;

/**
 * Allocate a VmafHipState. Picks the device by index; -1 selects the
 * first compute-capable HIP device.
 *
 * @param out  receives the new state handle on success. Pair with
 *             @ref vmaf_hip_state_free.
 * @param cfg  device selection.
 *
 * @return 0 on success, -ENOSYS when built without HIP, -ENODEV when
 *         no compatible device is found, -EINVAL on bad arguments.
 */
VMAF_EXPORT int vmaf_hip_state_init(VmafHipState **out, VmafHipConfiguration cfg);

/**
 * Hand the HIP state to a VmafContext. After import, the context
 * borrows the state pointer for the duration of its lifetime; the
 * caller still owns the state and must free it with
 * @ref vmaf_hip_state_free after vmaf_close(). Same lifetime model as
 * the SYCL + Vulkan backends.
 *
 * @param ctx    live VmafContext (from `vmaf_init()`).
 * @param state  state handle previously allocated via
 *               @ref vmaf_hip_state_init.
 *
 * @return 0 on success, -EINVAL when @p ctx or @p state is NULL,
 *         -ENOSYS when built without HIP.
 */
VMAF_EXPORT int vmaf_hip_import_state(VmafContext *ctx, VmafHipState *state);

/**
 * Release a state previously allocated via @ref vmaf_hip_state_init.
 * Safe to pass `NULL` or a state that was never imported. After import
 * the caller is still responsible for freeing — call this after
 * vmaf_close() to avoid using a state the context still references.
 *
 * @param state  pointer to the state handle to release; set to NULL on
 *               return.
 */
VMAF_EXPORT void vmaf_hip_state_free(VmafHipState **state);

/**
 * Enumerate compute-capable HIP devices visible to the runtime.
 * Prints one line per device with its ordinal, name, and compute
 * capability.
 *
 * @return Device count, or -ENOSYS when built without HIP.
 */
VMAF_EXPORT int vmaf_hip_list_devices(void);

#ifdef __cplusplus
}
#endif

#endif /* LIBVMAF_HIP_H_ */
