/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  Typed views of the HIP handles that the kernel template stores as
 *  `uintptr_t`.
 *
 *  `kernel_template.h` and `libvmaf_hip.h` carry `hipStream_t` / `hipEvent_t`
 *  as `uintptr_t` so that they stay free of <hip/hip_runtime_api.h>
 *  (ADR-0241). The host TUs that use the handles convert them back here,
 *  through a union rather than an integer-to-pointer cast, so the conversion
 *  is written once. Include this header only from a TU that already builds
 *  against the HIP runtime.
 */

#ifndef LIBVMAF_HIP_HIP_HANDLE_H_
#define LIBVMAF_HIP_HIP_HANDLE_H_

#include <stdint.h>

#include <hip/hip_runtime_api.h>

typedef union VmafHipHandle {
    uintptr_t bits;
    hipStream_t stream;
    hipEvent_t event;
} VmafHipHandle;

/* The stream stored in `bits`; 0 is the null stream. */
static inline hipStream_t vmaf_hip_stream_of(uintptr_t bits)
{
    const VmafHipHandle h = {.bits = bits};
    return h.stream;
}

/* The event stored in `bits`. */
static inline hipEvent_t vmaf_hip_event_of(uintptr_t bits)
{
    const VmafHipHandle h = {.bits = bits};
    return h.event;
}

/* The bits of `stream`, for a helper that takes the `uintptr_t` form. */
static inline uintptr_t vmaf_hip_stream_bits(hipStream_t stream)
{
    const VmafHipHandle h = {.stream = stream};
    return h.bits;
}

#endif /* LIBVMAF_HIP_HIP_HANDLE_H_ */
