/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Licensed under the BSD+Patent License (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      https://opensource.org/licenses/BSDplusPatent
 *
 *  -ENOSYS fallbacks for the public `libvmaf/libvmaf_hip.h` surface
 *  when libvmaf is built without `-Denable_hip=true`. Mirrors the
 *  `core/src/dnn/dnn_api.c` pattern that already does the same for
 *  `VMAF_HAVE_DNN`.
 *
 *  Without these stubs the entries declared in libvmaf_hip.h are not
 *  emitted into libvmaf.so at all (the real implementations live in
 *  `hip/common.c` + `libvmaf.c` under `#ifdef HAVE_HIP`), so any
 *  downstream consumer that links against libvmaf and references the
 *  HIP entry points fails at link time. The header documents the
 *  contract — "every entry point returns -ENOSYS unconditionally" —
 *  and these stubs honour it.
 *
 *  Meson wires this TU into `libvmaf_sources` only when
 *  `is_hip_enabled == false`. When HIP is enabled the real
 *  implementations in `hip/common.c` and `libvmaf.c` take over.
 */

#include "config.h"

#ifndef HAVE_HIP

#include <errno.h>
#include <stddef.h>

#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_hip.h"

int vmaf_hip_available(void)
{
    return 0;
}

int vmaf_hip_state_init(VmafHipState **out, VmafHipConfiguration cfg)
{
    (void)cfg;
    if (out != NULL) {
        *out = NULL;
    }
    return -ENOSYS;
}

int vmaf_hip_import_state(VmafContext *ctx, VmafHipState *state)
{
    (void)ctx;
    (void)state;
    return -ENOSYS;
}

void vmaf_hip_state_free(VmafHipState **state)
{
    if (state != NULL) {
        *state = NULL;
    }
}

int vmaf_hip_list_devices(void)
{
    return -ENOSYS;
}

#endif /* !HAVE_HIP */
