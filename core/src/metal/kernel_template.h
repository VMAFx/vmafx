/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Metal per-feature kernel scaffolding template — first-consumer
 *  mirror of `libvmaf/src/hip/kernel_template.h` (ADR-0241) which in
 *  turn mirrors `libvmaf/src/cuda/kernel_template.h` (ADR-0246).
 *
 *  T8-1 scaffold PR (ADR-0361). The CUDA / HIP templates capture the
 *  lifecycle every fork-added GPU feature kernel converged on (private
 *  command queue + 2-event submit/finished pair + device accumulator
 *  + host readback slot). Metal exposes the same async surface
 *  (`MTLCommandQueue`, `MTLCommandBuffer`, `MTLEvent`,
 *  `MTLBuffer`); the template's shape ports one-to-one with one
 *  simplification: on Apple Silicon's unified memory the
 *  `device` / `host_pinned` split collapses to a single `MTLBuffer`
 *  with `MTLResourceStorageModeShared` whose `[contents]` pointer
 *  the host reads directly. The runtime PR will track that as a
 *  single `uintptr_t` buffer slot rather than the (device, pinned-
 *  host) pair the CUDA / HIP twins carry.
 *
 *  Scaffold posture: this header lands ahead of the Metal runtime
 *  (T8-1b). The struct definitions are stable C surface so consumer
 *  TUs can compile without `<Metal/Metal.h>` / `<Metal/Metal.hpp>`.
 *  The lifecycle helpers `vmaf_metal_kernel_lifecycle_init/_close`,
 *  `vmaf_metal_kernel_buffer_alloc/_free`,
 *  `vmaf_metal_kernel_submit_pre_launch`,
 *  `vmaf_metal_kernel_collect_wait` are declared here and stubbed in
 *  `libvmaf/src/metal/kernel_template.c`. Every helper currently
 *  returns -ENOSYS until the runtime PR replaces the bodies with real
 *  Metal calls. The first consumer
 *  (`feature/metal/integer_motion_v2_metal.c`) calls them through the
 *  same call-graph the HIP / CUDA references use, so the runtime PR
 *  can swap bodies without touching consumers.
 *
 *  Why a paired .c instead of inline `static inline` helpers? Same
 *  rationale as the HIP twin (see `hip/kernel_template.h`): there is
 *  no driver-loader scaffolded yet, so the helper bodies must carry
 *  a -ENOSYS check that callers cannot inline away. Splitting
 *  declaration from definition lets the runtime PR replace the
 *  bodies without recompiling every consumer TU.
 *
 *  When the runtime PR (T8-1b) lands, the bodies in
 *  `kernel_template.c` flip from -ENOSYS to real Metal calls and the
 *  documentation here drops the "scaffold" caveats. The struct shape
 *  and helper signatures stay verbatim — that's the load-bearing
 *  contract this PR pins.
 *
 *  Reference implementations:
 *    - HIP: `libvmaf/src/hip/kernel_template.h`
 *    - CUDA: `libvmaf/src/cuda/kernel_template.h`
 *  First Metal consumer:
 *    `libvmaf/src/feature/metal/integer_motion_v2_metal.c`.
 */

#ifndef LIBVMAF_METAL_KERNEL_TEMPLATE_H_
#define LIBVMAF_METAL_KERNEL_TEMPLATE_H_

#include <stddef.h>
#include <stdint.h>

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Per-frame async lifecycle the Metal feature kernels share.
 *
 * Mirrors `VmafHipKernelLifecycle` field-for-field. Handles cross
 * the fork's Metal scaffold ABI as `uintptr_t` so this header stays
 * free of `<Metal/Metal.h>` (matches the public `libvmaf_metal.h`
 * convention; see ADR-0361). The runtime PR will store
 * `(MTL::CommandQueue *)(uintptr_t)` casts under the covers via the
 * MetalCpp wrapper.
 *
 * `cmd_queue` — private command queue for the readback path.
 * `submit`    — MTLEvent / MTLSharedEvent recorded after the kernel
 *               dispatch on the picture's command buffer.
 * `finished`  — MTLEvent recorded after the host-visible readback
 *               completes. On unified-memory Apple Silicon this is
 *               just a fence — the readback itself is a host load,
 *               not a copy.
 */
typedef struct VmafMetalKernelLifecycle {
    uintptr_t cmd_queue;
    uintptr_t submit;
    uintptr_t finished;
} VmafMetalKernelLifecycle;

/*
 * One MTLBuffer (storage-mode shared on Apple Silicon → host can read
 * directly via `[buffer contents]`; no separate pinned-host slot
 * needed thanks to unified memory).
 *
 * Field shape diverges from the HIP / CUDA twins by one slot —
 * `host_pinned` is implicit in `[buffer contents]`. The runtime PR
 * will cache the contents pointer in `host_view` to avoid the
 * Objective-C++ message-send on every collect.
 */
typedef struct VmafMetalKernelBuffer {
    uintptr_t buffer; /* MTLBuffer handle (uintptr_t cast) */
    void *host_view;  /* cached `[buffer contents]` for host loads */
    size_t bytes;
} VmafMetalKernelBuffer;

/*
 * Init: command queue + event-pair create. Scaffold returns -ENOSYS;
 * the runtime PR swaps in `[id<MTLDevice> newCommandQueue]` +
 * `[id<MTLDevice> newSharedEvent]` ×2.
 *
 * Caller pre-conditions: `lc` zero-initialised; `ctx` is the opaque
 * Metal context (from `vmaf_metal_context_new`). The scaffold
 * accepts NULL for `ctx` since no real Metal state exists yet — the
 * runtime PR will tighten the contract.
 *
 * Returns 0 on success or a negative errno. The scaffold returns
 * -ENOSYS unconditionally.
 */
int vmaf_metal_kernel_lifecycle_init(VmafMetalKernelLifecycle *lc, VmafMetalContext *ctx);

/*
 * Allocate an MTLBuffer of `bytes` size. On Apple Silicon the
 * runtime PR will pass `MTLResourceStorageModeShared` so host and
 * device see the same memory; on hypothetical Intel-Mac discrete-GPU
 * paths (rejected per ADR-0361, but documented for completeness)
 * this would be `MTLResourceStorageModeManaged`.
 *
 * Returns 0 on success or a negative errno. The scaffold returns
 * -ENOSYS; on failure `buf` is left zero-initialised.
 */
int vmaf_metal_kernel_buffer_alloc(VmafMetalKernelBuffer *buf, VmafMetalContext *ctx, size_t bytes);

/*
 * Per-frame submit-side helper.
 *
 *   1. Zero the buffer on `lc->cmd_queue` (compute-encoder fill_buffer).
 *   2. Wait for the dist-side ready event on `picture_command_buffer`.
 *
 * Mirrors `vmaf_hip_kernel_submit_pre_launch`. The runtime PR does
 * the `[id<MTLBlitCommandEncoder> fillBuffer:range:value:]` +
 * `[id<MTLCommandBuffer> encodeWaitForEvent:value:]`; the scaffold
 * returns -ENOSYS so the consumer's submit() short-circuits.
 *
 * Command-buffer + event handles cross as `uintptr_t` for the same
 * header-purity reason as the lifecycle struct.
 */
int vmaf_metal_kernel_submit_pre_launch(VmafMetalKernelLifecycle *lc, VmafMetalContext *ctx,
                                        VmafMetalKernelBuffer *buf,
                                        uintptr_t picture_command_buffer,
                                        uintptr_t dist_ready_event);

/*
 * collect()-side wait point: drains the private command queue so
 * `[buffer contents]` is safe to read.
 *
 * Mirrors `vmaf_hip_kernel_collect_wait`. Scaffold returns -ENOSYS.
 */
int vmaf_metal_kernel_collect_wait(VmafMetalKernelLifecycle *lc, VmafMetalContext *ctx);

/*
 * close()-side teardown: drain + release command queue, release
 * events.
 *
 * Mirrors `vmaf_hip_kernel_lifecycle_close`. The scaffold body is a
 * no-op (every handle is zero); the runtime PR will sequence
 * `[cmd_queue waitUntilCompleted]` → release queue → release events
 * and aggregate the first error. Safe to call on a partially-
 * initialised lifecycle.
 */
int vmaf_metal_kernel_lifecycle_close(VmafMetalKernelLifecycle *lc, VmafMetalContext *ctx);

/*
 * Free the buffer. Scaffold body is a no-op; runtime PR releases
 * the MTLBuffer via MetalCpp's NS::SharedPtr destructor. Safe to
 * call on a partially-allocated buffer.
 */
int vmaf_metal_kernel_buffer_free(VmafMetalKernelBuffer *buf, VmafMetalContext *ctx);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LIBVMAF_METAL_KERNEL_TEMPLATE_H_ */
