/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  ADR-0536: weak stubs for HIP HSACO blob symbols whose .hip kernel
 *  sources don't yet build standalone via `hipcc --genco`.
 *
 *  Background: ADR-0533 (PR #1292) wired several HIP feature extractors
 *  into the dispatch table whose `_hsaco` kernel sources were not yet
 *  portable.  Weak fallback symbols here keep the host link succeeding;
 *  the `hipModuleLoadData` call against an empty blob fails and the
 *  init path returns -ENOSYS, falling through to the CPU implementation.
 *
 *  When a kernel is ported and registered in the `hip_kernel_sources`
 *  meson dict, the xxd-embedded `_hsaco` strong symbol overrides the
 *  weak fallback here and the weak slot can be removed.
 *
 *  ADR-0539 (this commit): adm_dwt2/adm_csf/adm_csf_den/adm_cm ported
 *  and registered — their weak stubs have been removed below.
 */

#include <stddef.h>

#define VMAF_HSACO_WEAK_STUB(name)                                                                 \
    __attribute__((weak)) const unsigned char name[1] = {0};                                       \
    __attribute__((weak)) const unsigned int name##_len = 0u;

/* The integer ADM HIP kernels now build standalone (ADR-0539); the four
 * `adm_*_hsaco` weak stubs that previously lived here have been removed.
 * Their strong symbols come from the xxd-embedded targets registered in
 * `libvmaf/src/meson.build::hip_kernel_sources`.
 *
 * No other HIP extractor currently requires a weak HSACO stub. The macro
 * above is retained as the recommended pattern for future port-in-progress
 * extractors so the host link stays green during incremental porting. */
