/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Test-only switches and counters of the VMAFx device-frame path (RC4 WP3,
 * ADR-1852 design section 2.7). Nothing here is exported from the shared
 * library (hidden visibility); the white-box tests link the static library.
 *
 * Counters: every place that copies imported pixel data through host memory
 * calls vmafx_count_host_copy(), every device-side planarisation (NV12 / P010
 * / P016) vmafx_count_conversion(). The backend lanes assert that the host
 * copy count stays 0 on their import paths (no silent host copy, D8).
 *
 * Switches plant the defects the fence and copy tests must catch: with one
 * set, a test that passes is a test that cannot see the defect.
 */

#ifndef VMAFX_FRAME_IMPORT_HOOKS_H
#define VMAFX_FRAME_IMPORT_HOOKS_H

#include <stdbool.h>
#include <stdint.h>

#include "vmafx/vmafx.h"

/* vmafx_frame_import() treats an unsignalled acquire fence as signalled. */
#define VMAFX_TEST_SKIP_ACQUIRE_WAIT (1u << 0)
/* vmafx_submit() signals the frames' release fences when it returns,
 * before the engine has dropped its references. */
#define VMAFX_TEST_EARLY_RELEASE (1u << 1)
/* Host-memory imports copy planar planes instead of binding them. */
#define VMAFX_TEST_FORCE_HOST_COPY (1u << 2)

/* Sleep between two looks at an unsignalled host fence (fence.c), and the
 * step of the virtual clock. */
#define VMAFX_FENCE_POLL_NS 50000u

/* The test clock: while it is virtual, a host fence wait reads this clock
 * and each poll interval advances it instead of sleeping, so a test sees a
 * 10 s wait bound hold without waiting 10 s. */
void vmafx_test_set_virtual_clock(bool on);
bool vmafx_test_clock_is_virtual(void);
uint64_t vmafx_test_clock_now_ns(void);
void vmafx_test_clock_advance(uint64_t ns);

/* The host wait the import rule allowed itself before its retry, last time
 * it retried (0 before the first). */
void vmafx_test_note_retry_wait(uint64_t ns);
uint64_t vmafx_test_last_retry_wait_ns(void);

/* No residency override (vmafx_test_set_import_residency()). */
#define VMAFX_TEST_RESIDENCY_OFF UINT32_MAX

void vmafx_test_set_switches(uint32_t mask);
bool vmafx_test_switch(uint32_t bit);

void vmafx_count_host_copy(uint64_t bytes);
void vmafx_count_conversion(void);
void vmafx_count_import_attempt(void);

uint64_t vmafx_test_host_copies(void);
uint64_t vmafx_test_host_copy_bytes(void);
uint64_t vmafx_test_conversions(void);
uint64_t vmafx_test_import_attempts(void);
void vmafx_test_reset_counters(void);

/* The next `count` imports fail with `status` before they look at their
 * descriptor (a transient failure for the D8 retry tests). */
void vmafx_test_fail_imports(VmafxStatus status, uint32_t count);
/* The status the current import fails with (one of the planted ones), or
 * VMAFX_OK. */
VmafxStatus vmafx_test_take_import_failure(void);

/* Imported frames claim `backend` as their residency (VMAFX_TEST_RESIDENCY_OFF:
 * their device's), so admission of device-resident frames runs on the CPU. */
void vmafx_test_set_import_residency(uint32_t backend);
uint32_t vmafx_test_import_residency(void);

#endif /* VMAFX_FRAME_IMPORT_HOOKS_H */
