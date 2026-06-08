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
 *  -ENOSYS fallbacks for the public `libvmaf/libvmaf_metal.h` surface
 *  when libvmaf is built without `-Denable_metal=enabled` (or the
 *  auto-probe failed on a non-macOS host). Mirrors the
 *  `core/src/dnn/dnn_api.c` pattern that already does the same for
 *  `VMAF_HAVE_DNN` and the sibling `core/src/hip/stubs.c` for HIP.
 *
 *  Without these stubs the entries declared in libvmaf_metal.h are
 *  not emitted into libvmaf.so at all (the real implementations live
 *  in `metal/common.mm` + `metal/picture_import.mm` + `libvmaf.c`
 *  under `#ifdef HAVE_METAL`), so any downstream consumer that links
 *  against libvmaf and references the Metal entry points fails at
 *  link time. The header documents the contract — "every entry point
 *  returns -ENOSYS unconditionally" — and these stubs honour it.
 *
 *  Meson wires this TU into `libvmaf_sources` only when
 *  `is_metal_enabled == false`. When Metal is enabled the real
 *  implementations take over.
 */

#include "config.h"

#ifndef HAVE_METAL

#include <errno.h>
#include <stddef.h>

#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_metal.h"

int vmaf_metal_available(void)
{
    return 0;
}

int vmaf_metal_state_init(VmafMetalState **out, VmafMetalConfiguration cfg)
{
    (void)cfg;
    if (out != NULL) {
        *out = NULL;
    }
    return -ENOSYS;
}

int vmaf_metal_import_state(VmafContext *ctx, VmafMetalState *state)
{
    (void)ctx;
    (void)state;
    return -ENOSYS;
}

void vmaf_metal_state_free(VmafMetalState **state)
{
    if (state != NULL) {
        *state = NULL;
    }
}

int vmaf_metal_list_devices(void)
{
    return -ENOSYS;
}

int vmaf_metal_state_init_external(VmafMetalState **out, VmafMetalExternalHandles handles)
{
    (void)handles;
    if (out != NULL) {
        *out = NULL;
    }
    return -ENOSYS;
}

int vmaf_metal_picture_import(VmafMetalState *state, uintptr_t iosurface, unsigned plane,
                              unsigned w, unsigned h, unsigned bpc, int is_ref, unsigned index)
{
    (void)state;
    (void)iosurface;
    (void)plane;
    (void)w;
    (void)h;
    (void)bpc;
    (void)is_ref;
    (void)index;
    return -ENOSYS;
}

int vmaf_metal_wait_compute(VmafMetalState *state)
{
    (void)state;
    return -ENOSYS;
}

int vmaf_metal_read_imported_pictures(VmafContext *ctx, unsigned index)
{
    (void)ctx;
    (void)index;
    return -ENOSYS;
}

#endif /* !HAVE_METAL */
