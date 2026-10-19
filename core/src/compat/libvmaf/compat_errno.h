/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * The libvmaf return value of a failed VMAFx call (ADR-1852 design section
 * 2.11), shared by the generated shims (libvmaf_gen.c) and the hand-written
 * compat functions of this directory. Compat code calls exported vmafx_
 * functions only: libvmaf.so.3 links against libvmafx.so.1 with no other
 * symbol to reach.
 */

#ifndef VMAF_COMPAT_ERRNO_H
#define VMAF_COMPAT_ERRNO_H

#include <stdint.h>

#include "vmafx/vmafx.h"

/* Negative errno libvmaf returns for a status without an engine errno
 * (status_errno_gen.c; generated from the status table). */
int vmaf_compat_status_errno(VmafxStatus status);

/* The negative errno libvmaf returned for this failure: the engine's own code
 * when the failure came from the engine, else the status's errno. Releases
 * the error. */
static inline int compat_errno(VmafxStatus status, VmafxError *error)
{
    const int32_t engine_errno = vmafx_error_errno(error);
    vmafx_error_free(error);
    if (engine_errno != 0) {
        return engine_errno;
    }
    return vmaf_compat_status_errno(status);
}

#endif /* VMAF_COMPAT_ERRNO_H */
