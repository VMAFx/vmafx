/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 */

#ifndef LIBVMAF_HIP_COMMON_H_
#define LIBVMAF_HIP_COMMON_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Created by ADR-0212 / T7-10; common.c, picture_hip.c, the import_*.c
 * family, shared_frame.c and feature/hip/<feature>_hip.c are the live HIP
 * runtime. Without enable_hip, stubs.c supplies the public entry points as
 * -ENOSYS.
 */

typedef struct VmafHipContext VmafHipContext;

/* HIP error type. Prefer the real runtime header if present (resolves
 * hipError_t to its native enum); otherwise fall back to a scaffold int
 * typedef so non-HIP TUs still compile. We probe via __has_include so the
 * decision is local to this TU and doesn't depend on macros set by the build
 * (HAVE_HIPCC is build-system level and can be inconsistent across targets,
 * e.g. test executables that include hip/hip_runtime_api.h directly). */
#if defined(__has_include)
#if __has_include(<hip/hip_runtime_api.h>)
#include <hip/hip_runtime_api.h>
#define VMAF_HIP_HAS_RUNTIME_HEADER 1
#endif
#endif
#ifndef VMAF_HIP_HAS_RUNTIME_HEADER
typedef int hipError_t;
#endif

struct VmafHipState;

/*
 * Allocate a context on HIP device `device_index` and make that device the
 * calling thread's, so the allocations and module loads that follow land on
 * it. A HIP twin passes `fex->hip_device_index`, the device of the
 * context's imported VmafHipState. Returns 0, -EINVAL for a NULL `ctx` or an
 * index outside [0, vmaf_hip_device_count()), -ENODEV when the runtime sees
 * no device, -ENOMEM, or the runtime's error as a negative errno. `*ctx` is
 * NULL on every failure.
 */
int vmaf_hip_context_new(VmafHipContext **ctx, int device_index);
void vmaf_hip_context_destroy(VmafHipContext *ctx);

/*
 * The number of HIP devices the runtime sees: >= 0 when the runtime answers
 * (0 when it reports hipErrorNoDevice), a negative errno when it cannot be
 * queried. A failure is never reported as a count.
 */
int vmaf_hip_device_count(void);

/* The device index `state` was created on (vmaf_hip_state_init() resolves
 * -1 to 0); -EINVAL for NULL. */
int vmaf_hip_state_device_index(const struct VmafHipState *state);

/* Make the device of `state` the calling thread's HIP device. libvmaf calls
 * it before a frame's HIP twins run and before the flush, so a caller that
 * moves vmaf_read_pictures() to another thread keeps the device it chose.
 * Returns 0, -EINVAL for NULL, or the runtime's error as a negative errno. */
int vmaf_hip_state_bind(const struct VmafHipState *state);

/*
 * Translate a HIP error code into a negative POSIX errno. Consolidates
 * 8 identical per-feature private helpers into a single shared implementation.
 * Feature extractors use this instead of defining their own static versions.
 */
int vmaf_hip_rc_to_errno(hipError_t rc);

#ifdef __cplusplus
}
#endif

#endif /* LIBVMAF_HIP_COMMON_H_ */
