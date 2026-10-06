/**
 *  Copyright 2026 Lusoris
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * The SYCL lane of the VMAFx device-frame API (RC4 WP3, ADR-1852 design
 * section 2.7, ADR-1929, ADR-2091): SYCL devices, imports of USM pointers,
 * Linux dma-bufs (de-tiled on the device) and OpenGL textures (exported as
 * dma-bufs through EGL), SYCL event, sync_file and GL sync fences. The
 * modules of core/src/vmafx/ call these for a device whose backend is
 * VMAFX_BACKEND_SYCL; nothing here is exported.
 *
 * Ordering (ADR-2091): a frame's conversions run on the device's library
 * queue behind its acquire fence; every engine read of its planes is a
 * device copy that waits on the frame's ready event and is recorded on the
 * frame; the release is one barrier over those reads on the library queue,
 * after the last reader of every context.
 */

#ifndef VMAF_SRC_SYCL_VMAFX_SYCL_H_
#define VMAF_SRC_SYCL_VMAFX_SYCL_H_

#include <stdint.h>

#include "vmafx/internal.h"
#include "vmafx/vmafx.h"

/* ---- Devices (import_device.c) ------------------------------------------------ */

/* Level Zero GPUs this process sees (0 is not a failure). */
VmafxStatus vmafx_sycl_device_count(const VmafxReport *report, uint32_t *count);
/* What SYCL device `index` is, without creating a VmafxDevice. */
VmafxStatus vmafx_sycl_device_info(const VmafxReport *report, int32_t index, VmafxDeviceInfo *info);
/* Open the device the descriptor names (by index, or the context and device
 * of the caller's queue in `external[0]`) into `device->lane`. */
VmafxStatus vmafx_sycl_device_open(const VmafxReport *report, const VmafxDeviceDesc *desc,
                                   VmafxDevice *device);
/* Drain the device's work and free `device->lane`. */
void vmafx_sycl_device_close(VmafxDevice *device);
/* What an open device is. */
void vmafx_sycl_device_describe(const VmafxDevice *device, VmafxDeviceInfo *info);

/* Make an engine state on the device's context and import it into the
 * context's engine (the successor of vmaf_sycl_state_init() +
 * vmaf_sycl_import_state()); kept in `context->lane_state`. */
VmafxStatus vmafx_sycl_context_attach(const VmafxReport *report, VmafxContext *context,
                                      VmafxDevice *device);
/* Free the engine state after the engine was closed. */
void vmafx_sycl_context_detach(VmafxContext *context);

/* ---- Frames (import_frame.c, import_dmabuf.c, import_gl.c, import_pool.c) ---- */

/* Check and bind a geometry-checked import descriptor on a SYCL device. */
VmafxStatus vmafx_sycl_frame_import(const VmafxReport *report, VmafxDevice *device,
                                    const VmafxFrameImport *desc, const VmafxImportLayout *layout,
                                    VmafxFrame **out);
/* Why a registered extractor named `extractor` cannot read a frame in SYCL
 * device memory, or NULL when it reads it. */
const char *vmafx_sycl_refusal(const char *extractor);

/* The device planes of one pool frame of geometry `d` (data, stride; no
 * private part or count yet): 0, or a negative errno. */
int vmafx_sycl_pool_frame_init(VmafxDevice *device, const VmafxFrameDesc *d, VmafxFrame *frame);
/* Arm a pool frame for one acquire: its picture a SYCL device picture, its
 * release the lane's; waits (on the host) until the readers of its previous
 * use ran. 0 or a negative errno. */
int vmafx_sycl_pool_frame_arm(VmafxFrame *frame);
/* Free a pool frame's device planes once the device read them. */
void vmafx_sycl_pool_frame_free(VmafxFrame *frame);

/* ---- Fences (import_fence.c) --------------------------------------------------- */

/* A release fence of `kind` (SYCL_EVENT) for a frame of a SYCL device. */
VmafxStatus vmafx_sycl_release_fence(const VmafxReport *report, VmafxFrame *frame, uint32_t kind,
                                     VmafxFence *out);
/* The planted early-release defect: open the frame's release fences now. */
void vmafx_sycl_release_early(VmafxFrame *frame);
/* A new fence of `kind` on a SYCL device (SYCL_EVENT: an event object the
 * caller assigns a command's event to). */
VmafxStatus vmafx_sycl_fence_create(const VmafxReport *report, VmafxDevice *device, uint32_t kind,
                                    VmafxFence *out);
/* Host wait on a SYCL_EVENT fence (VMAFX_PENDING for a poll). */
VmafxStatus vmafx_sycl_fence_wait(const VmafxReport *report, const VmafxFence *fence,
                                  uint64_t timeout_ns);
/* Destroy a SYCL_EVENT fence the library returned. */
VmafxStatus vmafx_sycl_fence_destroy(const VmafxReport *report, const VmafxFence *fence);

#endif /* VMAF_SRC_SYCL_VMAFX_SYCL_H_ */
