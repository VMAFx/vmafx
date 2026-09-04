/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  HIP per-feature kernel scaffolding template — first-consumer
 *  mirror of `libvmaf/src/cuda/kernel_template.h` (ADR-0221).
 *
 *  T7-10 first-consumer PR (ADR-0241). The CUDA template captured
 *  the lifecycle every fork-added CUDA feature kernel converged on
 *  (private non-blocking stream + 2-event submit/finished pair +
 *  device accumulator + pinned host readback slot). HIP exposes the
 *  same async-stream + event surface as CUDA (`hipStream_t`,
 *  `hipEvent_t`, `hipMemcpyAsync` ...), so the template's shape ports
 *  one-to-one.
 *
 *  Scaffold posture: this header lands ahead of the HIP runtime
 *  (T7-10b). The struct definitions are stable C surface so consumer
 *  TUs can compile without `<hip/hip_runtime.h>`. The lifecycle
 *  helpers `vmaf_hip_kernel_lifecycle_init/_close`,
 *  `vmaf_hip_kernel_readback_alloc/_free`,
 *  `vmaf_hip_kernel_submit_pre_launch`,
 *  `vmaf_hip_kernel_collect_wait` are declared here and stubbed in
 *  `libvmaf/src/hip/kernel_template.c`. Every helper currently returns
 *  -ENOSYS until the runtime PR replaces the bodies with real HIP
 *  calls. The consumer (`integer_psnr_hip.c`) calls them through the
 *  same call-graph the CUDA reference uses, so the runtime PR can
 *  swap bodies without touching consumers.
 *
 *  Why a paired .c instead of inline `static inline` helpers like the
 *  CUDA template? Two reasons:
 *
 *    1. The CUDA helpers are inline because they unwrap straight onto
 *       `cu_state->f->cuStreamCreateWithPriority` etc. — every call
 *       resolves at compile time against the `CudaFunctions` driver
 *       table. HIP has no equivalent driver-loader scaffolded yet
 *       (the runtime PR is still pending), so the helper bodies must
 *       carry a -ENOSYS check that callers cannot inline away. A real
 *       (non-inline) function gives one place to flip from -ENOSYS to
 *       a real `hipStreamCreate` call when the runtime arrives.
 *    2. Splitting declaration from definition lets the runtime PR
 *       replace the bodies without recompiling every consumer TU —
 *       the kernel-template ABI stays stable.
 *
 *  When the runtime PR (T7-10b) lands, the bodies in
 *  `kernel_template.c` flip from -ENOSYS to real HIP calls and the
 *  documentation here drops the "scaffold" caveats. The struct shape
 *  and helper signatures stay verbatim — that's the load-bearing
 *  contract this PR pins.
 *
 *  Reference implementation (CUDA): see
 *  `libvmaf/src/cuda/kernel_template.h` and
 *  `libvmaf/src/feature/cuda/integer_psnr_cuda.c`.
 *  First HIP consumer:
 *  `libvmaf/src/feature/hip/integer_psnr_hip.c`.
 *
 *  Migration guide: `docs/backends/kernel-scaffolding.md`.
 */

#ifndef LIBVMAF_HIP_KERNEL_TEMPLATE_H_
#define LIBVMAF_HIP_KERNEL_TEMPLATE_H_

#include <stddef.h>
#include <stdint.h>

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Per-frame async lifecycle the HIP feature kernels share.
 *
 * Mirrors `VmafCudaKernelLifecycle` field-for-field. Handles cross
 * the fork's HIP scaffold ABI as `uintptr_t` so this header stays
 * free of `<hip/hip_runtime.h>` (matches the public `libvmaf_hip.h`
 * convention; see ADR-0212). The runtime PR will store
 * `(hipStream_t)(uintptr_t)` casts under the covers.
 *
 * `str`       — private non-blocking stream for the readback path.
 * `submit`    — event recorded on the picture's stream right after
 *               the kernel launch.
 * `finished`  — event recorded on `str` after the readback completes.
 */
typedef struct VmafHipKernelLifecycle {
    uintptr_t str;
    uintptr_t submit;
    uintptr_t finished;
} VmafHipKernelLifecycle;

/*
 * One device-side accumulator + one pinned host readback slot.
 *
 * Mirrors `VmafCudaKernelReadback`. The runtime PR fills `device`
 * via `hipMallocAsync` and `host_pinned` via `hipHostMalloc`; the
 * scaffold leaves both NULL.
 */
typedef struct VmafHipKernelReadback {
    void *device;
    void *host_pinned;
    size_t bytes;
} VmafHipKernelReadback;

/*
 * Init: stream + event-pair create. Scaffold returns -ENOSYS; the
 * runtime PR swaps in `hipStreamCreateWithFlags` +
 * `hipEventCreateWithFlags` ×2.
 *
 * Caller pre-conditions: `lc` zero-initialised; `ctx` is the opaque
 * HIP context (from `vmaf_hip_context_new`). The scaffold accepts
 * NULL for `ctx` since no real HIP state exists yet — the runtime
 * PR will tighten the contract.
 *
 * Returns 0 on success or a negative errno. The scaffold returns
 * -ENOSYS unconditionally.
 */
int vmaf_hip_kernel_lifecycle_init(VmafHipKernelLifecycle *lc, VmafHipContext *ctx);

/*
 * Allocate a (device, pinned-host) readback pair of `bytes` size.
 *
 * Returns 0 on success or a negative errno. The scaffold returns
 * -ENOSYS; on failure `rb` is left zero-initialised.
 */
int vmaf_hip_kernel_readback_alloc(VmafHipKernelReadback *rb, VmafHipContext *ctx, size_t bytes);

/*
 * Per-frame submit-side helper.
 *
 *   1. Zero the device accumulator on `lc->str`.
 *   2. Wait for the dist-side ready event on `picture_stream`.
 *
 * Mirrors `vmaf_cuda_kernel_submit_pre_launch`. The runtime PR does
 * the `hipMemsetAsync` + `hipStreamWaitEvent`; the scaffold returns
 * -ENOSYS so the consumer's submit() short-circuits.
 *
 * Stream + event handles cross as `uintptr_t` for the same
 * header-purity reason as the lifecycle struct.
 */
int vmaf_hip_kernel_submit_pre_launch(VmafHipKernelLifecycle *lc, VmafHipContext *ctx,
                                      VmafHipKernelReadback *rb, uintptr_t picture_stream,
                                      uintptr_t dist_ready_event);

/*
 * collect()-side wait point: drains the private stream so the host
 * pinned buffer is safe to read.
 *
 * Mirrors `vmaf_cuda_kernel_collect_wait`. Scaffold returns -ENOSYS.
 */
int vmaf_hip_kernel_collect_wait(VmafHipKernelLifecycle *lc, VmafHipContext *ctx);

/*
 * close()-side teardown: drain + destroy stream, destroy events.
 *
 * Mirrors `vmaf_cuda_kernel_lifecycle_close`. The scaffold body is
 * a no-op (every handle is zero); the runtime PR will sequence
 * `hipStreamSynchronize` → `hipStreamDestroy` → `hipEventDestroy` ×2
 * and aggregate the first error. Safe to call on a partially-
 * initialised lifecycle.
 */
int vmaf_hip_kernel_lifecycle_close(VmafHipKernelLifecycle *lc, VmafHipContext *ctx);

/*
 * Free the readback pair. Scaffold body is a no-op; runtime PR
 * issues `hipFreeAsync` + `hipHostFree`. Safe to call on a partially-
 * allocated readback.
 */
int vmaf_hip_kernel_readback_free(VmafHipKernelReadback *rb, VmafHipContext *ctx);

/*
 * Post-launch submit-side helper: records the `finished` event on
 * the private readback stream (`lc->str`) after the DtoH copy is
 * enqueued. `collect_wait` synchronises on `finished` to confirm the
 * readback is complete before the host reads the pinned buffer.
 *
 * Mirrors `vmaf_cuda_kernel_submit_post_record` from
 * `libvmaf/src/cuda/kernel_template.h`. PR #612 adds this helper for
 * `float_psnr_hip`; batch-1 (ADR-0372) also requires it for
 * `integer_psnr_hip`. On merge conflict with
 * PR #612 at merge time, keep one copy and discard the duplicate.
 */
int vmaf_hip_kernel_submit_post_record(VmafHipKernelLifecycle *lc, VmafHipContext *ctx);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LIBVMAF_HIP_KERNEL_TEMPLATE_H_ */
