/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Attaching a device to a VMAFx context (RC4 WP3 common lane, ADR-1852
 * design section 2.3): the context holds a reference to its device until it
 * is destroyed, and picks each feature's twin on the device's backend, so
 * the device comes before any feature, model or frame. The CPU device needs
 * no engine state; the backend lanes import their device into the engine
 * here (the successor of vmaf_cuda_import_state() and its siblings).
 */

#include <assert.h>
#include <stdint.h>

#include "engine.h"
#include "error_internal.h"
#include "internal.h"
#include "vmafx/vmafx.h"
#include "config.h"
#ifdef HAVE_CUDA
#include "cuda/vmafx_cuda.h"
#endif
#ifdef HAVE_SYCL
#include "sycl/vmafx_sycl.h"
#endif
#ifdef HAVE_HIP
#include "hip/vmafx_hip.h"
#endif

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

/* Import the device into the context's engine: nothing for the CPU, the
 * backend lane's engine state otherwise. */
static VmafxStatus attach_lane(const VmafxReport *report, VmafxContext *context,
                               VmafxDevice *device)
{
    if (device->backend == VMAFX_BACKEND_CPU) {
        return VMAFX_OK;
    }
#ifdef HAVE_CUDA
    if (device->backend == VMAFX_BACKEND_CUDA) {
        return vmafx_cuda_context_attach(report, context, device);
    }
#endif
#ifdef HAVE_SYCL
    if (device->backend == VMAFX_BACKEND_SYCL) {
        return vmafx_sycl_context_attach(report, context, device);
    }
#endif
#ifdef HAVE_HIP
    if (device->backend == VMAFX_BACKEND_HIP) {
        return vmafx_hip_context_attach(report, context, device);
    }
#endif
    (void)context;
    return VMAFX_FAIL(report, VMAFX_E_NOTSUP, 0, VMAFX_SUBJECT_BACKEND, "device",
                      "backend %s: this build scores on the devices of the backends it was "
                      "built with",
                      vmafx_backend_name(device->backend));
}

VmafxStatus vmafx_context_use_device(VmafxContext *context, VmafxDevice *device, VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(context, error);
    if (!context || !device) {
        return VMAFX_FAIL(&report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER,
                          !context ? "context" : "device", "NULL argument");
    }
    if (context->device) {
        return VMAFX_FAIL(&report, VMAFX_E_BUSY, 0, VMAFX_SUBJECT_DEVICE, "device",
                          "the context scores on a device already; a context has one device");
    }
    if (vmaf_engine_extractor_count(context->engine) != 0u || context->have_frame ||
        context->models.count != 0u || context->model_sets.count != 0u) {
        return VMAFX_FAIL(&report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_CONTEXT, "context",
                          "attach the device before any feature, model or frame: the context "
                          "picks each feature's twin on the device's backend when it is "
                          "registered");
    }
    const VmafxStatus status = attach_lane(&report, context, device);
    if (status != VMAFX_OK) {
        return status;
    }
    assert(context->device == NULL);
    context->device = vmafx_device_ref(device);
    return VMAFX_OK;
}

void vmafx_context_release_device(VmafxContext *context)
{
#ifdef HAVE_CUDA
    if (context->device && context->device->backend == VMAFX_BACKEND_CUDA) {
        vmafx_cuda_context_detach(context);
    }
#endif
#ifdef HAVE_SYCL
    if (context->device && context->device->backend == VMAFX_BACKEND_SYCL) {
        vmafx_sycl_context_detach(context);
    }
#endif
#ifdef HAVE_HIP
    if (context->device && context->device->backend == VMAFX_BACKEND_HIP) {
        vmafx_hip_context_detach(context);
    }
#endif
    vmafx_device_unref(context->device);
    context->device = NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
