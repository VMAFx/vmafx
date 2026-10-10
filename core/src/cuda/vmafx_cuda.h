/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * The CUDA lane of the VMAFx device-frame API (RC4 WP3, ADR-1852 design
 * section 2.7, ADR-1929, ADR-2023): CUDA devices, imports of CUDA device
 * pointers, CUDA arrays and OpenGL textures, CUDA event and GL sync fences,
 * and the frame pools of a CUDA device. The modules of core/src/vmafx/ call
 * these for a device whose backend is VMAFX_BACKEND_CUDA; nothing here is
 * exported.
 *
 * Ordering (ADR-2023): every frame of a CUDA device is read on the device's
 * library stream. An acquire fence becomes a wait on that stream, the frame's
 * ready event is recorded behind it, and a release fence is signalled by work
 * the library stream reaches after the frame's last reader; the per-frame
 * context barrier of ADR-1199 stays for frames that carry no fence.
 */

#ifndef VMAF_SRC_CUDA_VMAFX_CUDA_H_
#define VMAF_SRC_CUDA_VMAFX_CUDA_H_

#include <stdint.h>

#include "vmafx/internal.h"
#include "vmafx/vmafx.h"

/* ---- Devices (import_device.c) ------------------------------------------ */

/* CUDA devices this process sees: VMAFX_E_NOTSUP naming the backend when the
 * driver library cannot be loaded. */
VmafxStatus vmafx_cuda_device_count(const VmafxReport *report, uint32_t *count);
/* What CUDA device `index` is, without creating a VmafxDevice. */
VmafxStatus vmafx_cuda_device_info(const VmafxReport *report, int32_t index, VmafxDeviceInfo *info);
/* Open the device the descriptor names (by index, or the caller's context and
 * stream in `external`) into `device->lane`. */
VmafxStatus vmafx_cuda_device_open(const VmafxReport *report, const VmafxDeviceDesc *desc,
                                   VmafxDevice *device);
/* Drain the device's work and free `device->lane`. */
void vmafx_cuda_device_close(VmafxDevice *device);
/* What an open device is. */
void vmafx_cuda_device_describe(const VmafxDevice *device, VmafxDeviceInfo *info);

/* Import the device into the context's engine (the successor of
 * vmaf_cuda_import_state()); the context keeps the engine state in
 * `context->lane_state` until vmafx_cuda_context_detach(). */
VmafxStatus vmafx_cuda_context_attach(const VmafxReport *report, VmafxContext *context,
                                      VmafxDevice *device);
/* Free the engine state after the engine was closed. */
void vmafx_cuda_context_detach(VmafxContext *context);

/* ---- Frames (import_frame.c, import_pool.c) ------------------------------ */

/* Check and bind a geometry-checked import descriptor on a CUDA device. */
VmafxStatus vmafx_cuda_frame_import(const VmafxReport *report, VmafxDevice *device,
                                    const VmafxFrameImport *desc, const VmafxImportLayout *layout,
                                    VmafxFrame **out);
/* Why a registered extractor named `extractor` on the CUDA backend cannot
 * read a frame in CUDA device memory, or NULL when it reads it. */
const char *vmafx_cuda_refusal(const char *extractor);

/* The device planes of one pool frame of geometry `d` in `frame->pic`
 * (data, stride, size; no private part or count yet): 0, or a negative
 * errno. */
int vmafx_cuda_pool_frame_init(VmafxDevice *device, const VmafxFrameDesc *d, VmafxFrame *frame);
/* Arm a pool frame for one acquire: its picture's private part is a CUDA
 * picture read on the library stream (with no fence: the ADR-1199 barrier
 * orders the caller's writes), its release the lane's. 0 or a negative
 * errno. */
int vmafx_cuda_pool_frame_arm(VmafxFrame *frame);
/* Free a pool frame's device planes once the device read them (the pool is
 * gone). */
void vmafx_cuda_pool_frame_free(VmafxFrame *frame);

/* ---- Fences (import_fence.c, import_gl.c) -------------------------------- */

/* A release fence of `kind` (CUDA_EVENT) for a frame of a CUDA device. */
VmafxStatus vmafx_cuda_release_fence(const VmafxReport *report, VmafxFrame *frame, uint32_t kind,
                                     VmafxFence *out);
/* Signal a producer's VULKAN_SEMAPHORE fence behind the frame's last reader
 * (vmafx_frame_signal_on_release(); import_vulkan.c). */
VmafxStatus vmafx_cuda_signal_on_release(const VmafxReport *report, VmafxFrame *frame,
                                         const VmafxFence *signal);
/* The planted early-release defect: open the frame's release fences now. */
void vmafx_cuda_release_early(VmafxFrame *frame);
/* A new fence of `kind` on a CUDA device (CUDA_EVENT: an unrecorded event). */
VmafxStatus vmafx_cuda_fence_create(const VmafxReport *report, VmafxDevice *device, uint32_t kind,
                                    VmafxFence *out);
/* Host wait on a CUDA_EVENT fence (VMAFX_PENDING for a poll). GL_SYNC
 * fences are waited on by fence.c (sync_object.c), without a device. */
VmafxStatus vmafx_cuda_fence_wait(const VmafxReport *report, const VmafxFence *fence,
                                  uint64_t timeout_ns);
/* Destroy a CUDA_EVENT fence the library returned. */
VmafxStatus vmafx_cuda_fence_destroy(const VmafxReport *report, const VmafxFence *fence);

#endif /* VMAF_SRC_CUDA_VMAFX_CUDA_H_ */
