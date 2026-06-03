/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 *
 *  T7-29 part 4 (ADR-0251) — contract-level smoke for the v2
 *  async pending-fence ring on top of vmaf_vulkan_import_image /
 *  _wait_compute / _state_build_pictures.
 *
 *  The end-to-end "submit N > ring_size frames and verify
 *  scores match" test requires a valid VkImage handle and a
 *  decoder-side VkSemaphore — those live downstream in the
 *  FFmpeg `libvmaf_vulkan` filter (T7-29 part 3) and the
 *  cross-backend parity gate. This file pins the **contract**
 *  the ring exposes:
 *    - import_image with vk_image == 0 still rejects with
 *      -EINVAL after the v1 → v2 swap.
 *    - wait_compute on a state that never imported anything
 *      stays a 0-returning no-op.
 *    - build_pictures rejects "index never imported" with
 *      -EINVAL regardless of slot index parity.
 *    - state_free on a ring with zero outstanding fences must
 *      not crash, must not deadlock, and must not leak.
 *
 *  Once a real VkImage path is wired (FFmpeg filter or a synthetic
 *  vkCreateImage in the test), this file extends with the ring-
 *  wrap correctness check (submit ring_size + 1 frames, verify
 *  the slot[0] back-pressure stalls without corruption).
 */

#include <errno.h>
#include <stddef.h>

#include "test.h"

#include "libvmaf/libvmaf_vulkan.h"

/* Same VkImage = 0 rejection contract as v1 — the ring must not
 * have changed the public-API decision tree. */
static char *test_zero_image_still_rejects(void)
{
    if (vmaf_vulkan_list_devices() <= 0)
        return NULL;
    VmafVulkanConfiguration cfg = {.device_index = -1, .enable_validation = 0};
    VmafVulkanState *state = NULL;
    mu_assert("state_init", vmaf_vulkan_state_init(&state, cfg) == 0);

    /* index = 0 — would land on slot 0 if the handle were valid. */
    int rc = vmaf_vulkan_import_image(state, /*vk_image=*/0u, 0, 0, 0, 0, /*w=*/16, /*h=*/16,
                                      /*bpc=*/8, /*is_ref=*/1, /*index=*/0);
    mu_assert("v2 still rejects vk_image == 0 → -EINVAL", rc == -EINVAL);

    /* index large enough to wrap past ring_size = 4. The pre-image
     * checks must still fire before the ring math runs — otherwise
     * an attacker could provoke an out-of-range slot read by passing
     * a giant index with vk_image = 0. */
    rc = vmaf_vulkan_import_image(state, 0u, 0, 0, 0, 0, 16, 16, 8, 1, /*index=*/9999);
    mu_assert("v2 still rejects vk_image == 0 at high index → -EINVAL", rc == -EINVAL);

    vmaf_vulkan_state_free(&state);
    return NULL;
}

/* wait_compute on a state that never imported anything has to
 * be a no-op even though the ring now exists conceptually.
 * The lazy-alloc contract from ADR-0186 is preserved — no
 * import means no ring materialised means nothing to drain. */
static char *test_wait_compute_idle_after_v2(void)
{
    if (vmaf_vulkan_list_devices() <= 0)
        return NULL;
    VmafVulkanConfiguration cfg = {.device_index = -1, .enable_validation = 0};
    VmafVulkanState *state = NULL;
    mu_assert("state_init", vmaf_vulkan_state_init(&state, cfg) == 0);

    mu_assert("idle wait_compute returns 0 (v2 ring not materialised)",
              vmaf_vulkan_wait_compute(state) == 0);
    /* Calling twice in a row is also idempotent. */
    mu_assert("idle wait_compute is idempotent", vmaf_vulkan_wait_compute(state) == 0);

    vmaf_vulkan_state_free(&state);
    return NULL;
}

/* state_free on a freshly-init'd state with zero imports must
 * walk the ring teardown path without touching uninitialised
 * fences. The state struct's import-slots ring_size is 0 here,
 * so the loop bound is 0 and no slot_release runs — but we
 * still verify the path doesn't deadlock or crash. */
static char *test_state_free_no_imports_v2(void)
{
    if (vmaf_vulkan_list_devices() <= 0)
        return NULL;
    VmafVulkanConfiguration cfg = {.device_index = -1, .enable_validation = 0};
    VmafVulkanState *state = NULL;
    mu_assert("state_init", vmaf_vulkan_state_init(&state, cfg) == 0);
    /* No imports — ring is dormant. */
    vmaf_vulkan_state_free(&state);
    mu_assert("state_free clears the handle", state == NULL);
    return NULL;
}

/* read_imported_pictures still has to gate on a NULL VmafContext
 * regardless of which slot the requested index would map to. */
static char *test_read_imported_null_ctx_after_v2(void)
{
    int rc = vmaf_vulkan_read_imported_pictures(NULL, /*index=*/0);
    mu_assert("NULL ctx → -EINVAL (slot 0)", rc == -EINVAL);
    rc = vmaf_vulkan_read_imported_pictures(NULL, /*index=*/3);
    mu_assert("NULL ctx → -EINVAL (slot ring_size-1)", rc == -EINVAL);
    rc = vmaf_vulkan_read_imported_pictures(NULL, /*index=*/4);
    mu_assert("NULL ctx → -EINVAL (slot 0 after wrap)", rc == -EINVAL);
    rc = vmaf_vulkan_read_imported_pictures(NULL, /*index=*/9999);
    mu_assert("NULL ctx → -EINVAL (high index)", rc == -EINVAL);
    return NULL;
}

/* ADR-0251 follow-up #3: max_outstanding_frames is now a public
 * VmafVulkanConfiguration field. Verify the clamp + readback contract
 * end-to-end via the public API (no internal-header peeking). */
static char *test_ring_size_default_when_zero(void)
{
    if (vmaf_vulkan_list_devices() <= 0)
        return NULL;
    VmafVulkanConfiguration cfg = {.device_index = -1, .enable_validation = 0};
    /* cfg.max_outstanding_frames left at its zero-initialised value. */
    VmafVulkanState *state = NULL;
    mu_assert("state_init", vmaf_vulkan_state_init(&state, cfg) == 0);
    mu_assert("0 maps to VMAF_VULKAN_RING_DEFAULT (4)",
              vmaf_vulkan_state_max_outstanding_frames(state) == 4u);
    vmaf_vulkan_state_free(&state);
    return NULL;
}

static char *test_ring_size_passthrough_in_range(void)
{
    if (vmaf_vulkan_list_devices() <= 0)
        return NULL;
    for (unsigned want = 1u; want <= 8u; want++) {
        VmafVulkanConfiguration cfg = {
            .device_index = -1, .enable_validation = 0, .max_outstanding_frames = want};
        VmafVulkanState *state = NULL;
        mu_assert("state_init", vmaf_vulkan_state_init(&state, cfg) == 0);
        mu_assert("in-range value passes through unchanged",
                  vmaf_vulkan_state_max_outstanding_frames(state) == want);
        vmaf_vulkan_state_free(&state);
    }
    return NULL;
}

static char *test_ring_size_clamps_to_max(void)
{
    if (vmaf_vulkan_list_devices() <= 0)
        return NULL;
    const unsigned over[] = {9u, 16u, 64u, 1024u, 0xFFFFFFFFu};
    for (size_t i = 0; i < sizeof(over) / sizeof(over[0]); i++) {
        VmafVulkanConfiguration cfg = {
            .device_index = -1, .enable_validation = 0, .max_outstanding_frames = over[i]};
        VmafVulkanState *state = NULL;
        mu_assert("state_init", vmaf_vulkan_state_init(&state, cfg) == 0);
        mu_assert("over-MAX clamps to VMAF_VULKAN_RING_MAX (8)",
                  vmaf_vulkan_state_max_outstanding_frames(state) == 8u);
        vmaf_vulkan_state_free(&state);
    }
    return NULL;
}

static char *test_ring_size_getter_null_safe(void)
{
    /* No device probe — the getter is meant to be safe to call on a
     * NULL state regardless of build/runtime Vulkan availability. */
    mu_assert("NULL state returns 0 (not undefined behaviour)",
              vmaf_vulkan_state_max_outstanding_frames(NULL) == 0u);
    return NULL;
}

static char *run_v2_contract_tests(void)
{
    mu_run_test(test_zero_image_still_rejects);
    mu_run_test(test_wait_compute_idle_after_v2);
    mu_run_test(test_state_free_no_imports_v2);
    mu_run_test(test_read_imported_null_ctx_after_v2);
    return NULL;
}

static char *run_ring_tunable_tests(void)
{
    mu_run_test(test_ring_size_default_when_zero);
    mu_run_test(test_ring_size_passthrough_in_range);
    mu_run_test(test_ring_size_clamps_to_max);
    mu_run_test(test_ring_size_getter_null_safe);
    return NULL;
}

char *run_tests(void)
{
    char *r = run_v2_contract_tests();
    if (r)
        return r;
    return run_ring_tunable_tests();
}
