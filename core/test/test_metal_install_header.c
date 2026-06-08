/**
 * Copyright 2026 Lusoris
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 * Licensed under the BSD+Patent License (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://opensource.org/licenses/BSDplusPatent
 */

/**
 * @file test_metal_install_header.c
 * @brief Compile-time install-gate for libvmaf_metal.h — ADR-0437.
 *
 * Verifies that:
 *   1. `libvmaf/libvmaf_metal.h` is reachable via the build-tree include
 *      path (the same path `meson install` places the header under
 *      `<prefix>/include/libvmaf/`).
 *   2. The public symbols `vmaf_metal_import_state`, `vmaf_metal_state_init`,
 *      `vmaf_metal_state_free`, `vmaf_metal_available`, and the IOSurface
 *      import sub-API are declared with the expected prototypes — verified by
 *      taking a function-pointer-typed address of each symbol; a prototype
 *      mismatch is a compile error.
 *
 * This test is a compile-time + link-time gate only.  No Metal runtime
 * entry points are called, so it runs safely on the CI macOS runner even
 * when no Apple-Family-7 device is present.
 *
 * Meson guard in `libvmaf/test/meson.build`:
 *   `if host_machine.system() == 'darwin'` — not built on Linux/Windows.
 */

#include "libvmaf/libvmaf_metal.h"

#include <stddef.h>

#include "test.h"

/* Verify the function pointer types are correct by taking their address.
 * Each assignment must match the declared prototype exactly; a type mismatch
 * is a compile error rather than a runtime assertion. */
static char *test_symbol_prototypes(void)
{
    /* vmaf_metal_available: void -> int */
    int (*fp_avail)(void) = vmaf_metal_available;
    /* vmaf_metal_state_init: (VmafMetalState**, VmafMetalConfiguration) -> int */
    int (*fp_init)(VmafMetalState **, VmafMetalConfiguration) = vmaf_metal_state_init;
    /* vmaf_metal_import_state: (VmafContext*, VmafMetalState*) -> int */
    int (*fp_import)(VmafContext *, VmafMetalState *) = vmaf_metal_import_state;
    /* vmaf_metal_state_free: (VmafMetalState**) -> void */
    void (*fp_free)(VmafMetalState **) = vmaf_metal_state_free;
    /* vmaf_metal_list_devices: void -> int */
    int (*fp_list)(void) = vmaf_metal_list_devices;

    /* Suppress unused-variable warnings; the assignments above are the
     * real assertions (compile-time prototype check). */
    (void)fp_avail;
    (void)fp_init;
    (void)fp_free;
    (void)fp_list;

    mu_assert("vmaf_metal_import_state symbol is reachable", fp_import != NULL);
    return NULL;
}

/* Verify IOSurface import sub-API symbols are reachable (ADR-0423). */
static char *test_iosurface_symbol_prototypes(void)
{
    int (*fp_init_ext)(VmafMetalState **, VmafMetalExternalHandles) =
        vmaf_metal_state_init_external;
    int (*fp_pic_import)(VmafMetalState *, uintptr_t, unsigned, unsigned, unsigned, unsigned, int,
                         unsigned) = vmaf_metal_picture_import;
    int (*fp_wait)(VmafMetalState *) = vmaf_metal_wait_compute;
    int (*fp_read)(VmafContext *, unsigned) = vmaf_metal_read_imported_pictures;

    (void)fp_init_ext;
    (void)fp_wait;
    (void)fp_read;

    mu_assert("vmaf_metal_picture_import symbol is reachable", fp_pic_import != NULL);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_symbol_prototypes);
    mu_run_test(test_iosurface_symbol_prototypes);
    return NULL;
}
