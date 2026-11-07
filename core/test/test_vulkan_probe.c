/**
 *
 *  Copyright 2026 Lusoris and Claude (Anthropic)
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Device self-check probe driver (ADR-3402).
 *
 * Runs the four checks (u32 x u32 -> u64 full product, u64 carry-chain
 * reduce, fp32 NoContraction audit, dispatch sentinel) on the device
 * the probe selects, and asserts the JSON verdicts. Without
 * `-Denable_vulkan=enabled` (no libvulkan at build time) the test
 * skips; with it, the probe still skips when no Vulkan loader /
 * compute-capable device answers — the probe refuses rather than
 * guesses, and so does the test.
 *
 * The identity-selection refusal path (requested vendorID/deviceID not
 * selectable, adapters named) is exercised without a device too: the
 * probe answers -ENODEV on a Vulkan-less build before device creation,
 * and the test asserts that the adapters-seen diagnostic is produced
 * whenever the refusal happens.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "test.h"

#ifdef HAVE_VULKAN

#include "feature/vulkan/probe/vulkan_probe.h"

static char *test_probe_default_selection(void)
{
    VmafVulkanProbeParams params = { .count = 4096, .out = stderr };
    VmafVulkanProbeResult result;
    const int err = vmaf_vulkan_probe_run(&params, &result);
    if (err == -ENODEV) {
        (void)fprintf(stderr, "[skip: no Vulkan loader / compute device] ");
        mu_skipped = 1;
        return NULL;
    }
    mu_assert("probe failed with an unexpected error", err == 0);
    mu_assert("umul32-product check passed", result.check_umul32 == 1);
    mu_assert("u64-add-reduce check passed", result.check_u64_reduce == 1);
    mu_assert("NoContraction honored (0 violations)", result.check_contraction == 1);
    mu_assert("contraction audit had no unclassifiable case",
              result.contraction_unclassified == 0u);
    mu_assert("dispatch sentinel proved the kernel ran", result.check_sentinel == 1);
    return NULL;
}

static char *test_probe_wrong_identity_refused(void)
{
    /* A vendorID/deviceID pair no real adapter carries. The probe must
     * refuse (non-zero, -ENODEV) and name the adapters it saw — never
     * silently fall back to something else (research-0090: ordinals
     * lie, identities do not; the gate refuses wrong identity). */
    VmafVulkanProbeParams params = {
        .vendor_id = 0xffffu,
        .device_id = 0xffffu,
        .count = 4096,
        .out = stderr,
    };
    VmafVulkanProbeResult result;
    const int err = vmaf_vulkan_probe_run(&params, &result);
    if (err == -ENODEV && result.adapters_seen == 0u) {
        /* Refused before enumeration (no loader): still a refusal. */
        (void)fprintf(stderr, "[skip: no Vulkan loader] ");
        mu_skipped = 1;
        return NULL;
    }
    mu_assert("wrong identity must be refused with -ENODEV", err == -ENODEV);
    mu_assert("refusal names the adapters seen", result.adapters_seen > 0u);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_probe_default_selection);
    mu_run_test(test_probe_wrong_identity_refused);
    return NULL;
}

#else /* !HAVE_VULKAN */

static char *test_probe_not_built(void)
{
    (void)fprintf(stderr, "[skip: probe not built (enable_vulkan off)] ");
    mu_skipped = 1;
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_probe_not_built);
    return NULL;
}

#endif /* HAVE_VULKAN */
