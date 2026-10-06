/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Test-only switches and counters of the VMAFx device-frame path (RC4 WP3);
 * see frame_import_hooks.h. Process-wide atomics: a test that sets a switch
 * clears it before it returns.
 */

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "frame_import_hooks.h"
#include "vmafx/vmafx.h"

/* Attempts of the compare-exchange in vmafx_test_take_import_failure(). */
#define VMAFX_TEST_CAS_TRIES 1024u

static atomic_uint test_switches;
static atomic_uint_fast64_t host_copies;
static atomic_uint_fast64_t host_copy_bytes;
static atomic_uint_fast64_t conversions;
static atomic_uint_fast64_t import_attempts;
static atomic_int planted_status;
static atomic_uint planted_count;
static atomic_uint residency_override = VMAFX_TEST_RESIDENCY_OFF;

void vmafx_test_set_switches(uint32_t mask)
{
    atomic_store(&test_switches, mask);
}

bool vmafx_test_switch(uint32_t bit)
{
    return (atomic_load_explicit(&test_switches, memory_order_relaxed) & bit) != 0u;
}

void vmafx_count_host_copy(uint64_t bytes)
{
    atomic_fetch_add(&host_copies, 1u);
    atomic_fetch_add(&host_copy_bytes, bytes);
}

void vmafx_count_conversion(void)
{
    atomic_fetch_add(&conversions, 1u);
}

void vmafx_count_import_attempt(void)
{
    atomic_fetch_add(&import_attempts, 1u);
}

uint64_t vmafx_test_host_copies(void)
{
    return atomic_load(&host_copies);
}

uint64_t vmafx_test_host_copy_bytes(void)
{
    return atomic_load(&host_copy_bytes);
}

uint64_t vmafx_test_conversions(void)
{
    return atomic_load(&conversions);
}

uint64_t vmafx_test_import_attempts(void)
{
    return atomic_load(&import_attempts);
}

void vmafx_test_reset_counters(void)
{
    atomic_store(&host_copies, 0u);
    atomic_store(&host_copy_bytes, 0u);
    atomic_store(&conversions, 0u);
    atomic_store(&import_attempts, 0u);
}

void vmafx_test_fail_imports(VmafxStatus status, uint32_t count)
{
    atomic_store(&planted_status, status);
    atomic_store(&planted_count, count);
}

VmafxStatus vmafx_test_take_import_failure(void)
{
    unsigned left = atomic_load(&planted_count);
    /* One planted failure per import, even with imports on several threads;
     * a failed exchange reloads `left`. Bounded (HISS-02): the tests import
     * from at most a few threads. */
    for (unsigned tries = 0u; tries < VMAFX_TEST_CAS_TRIES && left > 0u; tries++) {
        if (atomic_compare_exchange_weak(&planted_count, &left, left - 1u)) {
            return (VmafxStatus)atomic_load(&planted_status);
        }
    }
    return VMAFX_OK;
}

void vmafx_test_set_import_residency(uint32_t backend)
{
    atomic_store(&residency_override, backend);
}

uint32_t vmafx_test_import_residency(void)
{
    return atomic_load(&residency_override);
}
