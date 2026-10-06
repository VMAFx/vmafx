/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * The HIP lane of the VMAFx device-frame API (RC4 WP3, ADR-1852 design
 * section 2.7, ADR-1929, ADR-2092): HIP devices, imports of HIP device
 * pointers, Linux dma-bufs (as HIP external memory), HIP arrays and OpenGL
 * textures, HIP event, GL sync and sync_file acquire fences, and HIP event
 * release fences. The modules of core/src/vmafx/ call these for a device
 * whose backend is VMAFX_BACKEND_HIP; nothing here is exported.
 *
 * Ordering (ADR-2092, the CUDA rules of ADR-2023 on HIP): every read of a
 * frame of a HIP device is enqueued on the device's library stream. An
 * acquire fence becomes a wait on that stream (HIP_EVENT) or a host check
 * before the import (GL_SYNC, SYNC_FILE); the twins copy or convert the
 * planes on the library stream and their own streams wait for it
 * (core/src/hip/picture_hip.h); a release fence is signalled by work the
 * library stream reaches after the frame's last reader.
 */

#ifndef VMAF_SRC_HIP_VMAFX_HIP_H_
#define VMAF_SRC_HIP_VMAFX_HIP_H_

#include <stdint.h>

#include "vmafx/internal.h"
#include "vmafx/vmafx.h"

/* ---- Devices (import_device.c) ------------------------------------------ */

/* HIP devices this process sees; 0 when the runtime reports none. */
VmafxStatus vmafx_hip_device_count(const VmafxReport *report, uint32_t *count);
/* What HIP device `index` is, without creating a VmafxDevice. */
VmafxStatus vmafx_hip_device_info(const VmafxReport *report, int32_t index, VmafxDeviceInfo *info);
/* Open the device the descriptor names (by index, or the device of the
 * caller's stream in `external[0]`) into `device->lane`. */
VmafxStatus vmafx_hip_device_open(const VmafxReport *report, const VmafxDeviceDesc *desc,
                                  VmafxDevice *device);
/* Drain the device's work and free `device->lane`. */
void vmafx_hip_device_close(VmafxDevice *device);
/* What an open device is. */
void vmafx_hip_device_describe(const VmafxDevice *device, VmafxDeviceInfo *info);

/* Import the device into the context's engine (the successor of
 * vmaf_hip_state_init() + vmaf_hip_import_state()); the context keeps the
 * engine state in `context->lane_state` until vmafx_hip_context_detach(). */
VmafxStatus vmafx_hip_context_attach(const VmafxReport *report, VmafxContext *context,
                                     VmafxDevice *device);
/* Free the engine state after the engine was closed. */
void vmafx_hip_context_detach(VmafxContext *context);

/* ---- Frames (import_frame.c) --------------------------------------------- */

/* Check and bind a geometry-checked import descriptor on a HIP device. */
VmafxStatus vmafx_hip_frame_import(const VmafxReport *report, VmafxDevice *device,
                                   const VmafxFrameImport *desc, const VmafxImportLayout *layout,
                                   VmafxFrame **out);
/* Why a registered extractor named `extractor` on the HIP backend cannot read
 * a frame in HIP device memory, or NULL when it reads it. */
const char *vmafx_hip_refusal(const char *extractor);

/* ---- Fences (import_fence.c) ---------------------------------------------- */

/* A release fence of `kind` (HIP_EVENT) for a frame of a HIP device. */
VmafxStatus vmafx_hip_release_fence(const VmafxReport *report, VmafxFrame *frame, uint32_t kind,
                                    VmafxFence *out);
/* The planted early-release defect: open the frame's release fences now. */
void vmafx_hip_release_early(VmafxFrame *frame);
/* A new fence of `kind` on a HIP device (HIP_EVENT: an unrecorded event). */
VmafxStatus vmafx_hip_fence_create(const VmafxReport *report, VmafxDevice *device, uint32_t kind,
                                   VmafxFence *out);
/* Host wait on a HIP_EVENT fence (VMAFX_PENDING for a poll). */
VmafxStatus vmafx_hip_fence_wait(const VmafxReport *report, const VmafxFence *fence,
                                 uint64_t timeout_ns);
/* Destroy a HIP_EVENT fence the library returned. */
VmafxStatus vmafx_hip_fence_destroy(const VmafxReport *report, const VmafxFence *fence);

#endif /* VMAF_SRC_HIP_VMAFX_HIP_H_ */
