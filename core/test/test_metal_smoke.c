/**
 *  Copyright 2026 Lusoris and Claude (Anthropic)
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 *
 *  Build + init smoke test for the Metal backend runtime (T8-1b /
 *  ADR-0420). Replaces the T8-1 scaffold-only test (which pinned
 *  every entry point at -ENOSYS) with a runtime test that exercises
 *  the real Apple Metal API path on Apple-Family-7+ hosts and
 *  surfaces a clean -ENODEV on every other host (Intel Macs, iOS
 *  simulators if anyone is daring enough, etc.). The first feature
 *  kernel arrives in T8-1c; this test is the gate that the runtime
 *  surface itself works.
 *
 *  Mirrors libvmaf/test/test_hip_smoke.c.
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "test.h"

#include "feature/feature_extractor.h"
#include "libvmaf/libvmaf_metal.h"
#include "metal/common.h"
#include "metal/dispatch_strategy.h"
#include "metal/kernel_template.h"

/*
 * Helper: try to grab a real context. Sets `*ctx_out` to the context
 * (on Apple-Family-7+) or NULL (on Intel Macs / no GPU). Returns the
 * raw rc so callers can `mu_assert(... rc == 0 || rc == -ENODEV ...)`
 * before dispatching the device-dependent assertions.
 */
static int try_get_ctx(VmafMetalContext **ctx_out)
{
    *ctx_out = NULL;
    return vmaf_metal_context_new(ctx_out, -1);
}

/* ---- Internal context (libvmaf/src/metal/common.h) ---- */

static char *test_context_new_rejects_null_out(void)
{
    const int rc = vmaf_metal_context_new(NULL, 0);
    mu_assert("NULL out -> -EINVAL", rc == -EINVAL);
    return NULL;
}

static char *test_context_destroy_null_is_noop(void)
{
    /* No assertion needed — must not crash. */
    vmaf_metal_context_destroy(NULL);
    return NULL;
}

static char *test_context_default_device_or_skip(void)
{
    /* -1 selects the system default device. On Apple Silicon CI
     * runners this succeeds; on Intel Mac CI lanes (if ever added)
     * this returns -ENODEV. */
    VmafMetalContext *ctx = NULL;
    const int rc = try_get_ctx(&ctx);
    mu_assert("context_new returns 0 or -ENODEV", rc == 0 || rc == -ENODEV);
    if (rc != 0) {
        return NULL;
    }
    /* Bridge accessors must return non-NULL when context_new succeeded. */
    mu_assert("context exposes device handle", vmaf_metal_context_device_handle(ctx) != NULL);
    mu_assert("context exposes queue handle", vmaf_metal_context_queue_handle(ctx) != NULL);
    vmaf_metal_context_destroy(ctx);
    return NULL;
}

static char *test_device_count_nonnegative(void)
{
    /* Runtime returns the number of Apple-Family-7+ devices visible
     * to the process; 0 on Intel Macs and non-Apple hosts. */
    const int n = vmaf_metal_device_count();
    mu_assert("device_count is non-negative", n >= 0);
    return NULL;
}

/* ---- Public C-API (libvmaf/include/libvmaf/libvmaf_metal.h) ---- */

static char *test_available_reports_built(void)
{
    /* Built-with-Metal: vmaf_metal_available() returns 1. The test
     * binary only links when -Denable_metal=enabled, so we expect 1. */
    const int avail = vmaf_metal_available();
    mu_assert("vmaf_metal_available returns 1 when built with Metal", avail == 1);
    return NULL;
}

static char *test_state_init_succeeds_or_enodev(void)
{
    VmafMetalConfiguration cfg = {.device_index = -1, .flags = 0};
    VmafMetalState *state = NULL;
    const int rc = vmaf_metal_state_init(&state, cfg);
    if (rc == 0) {
        mu_assert("on success the state pointer must be populated", state != NULL);
        vmaf_metal_state_free(&state);
        mu_assert("state_free clears the slot", state == NULL);
    } else {
        mu_assert("state_init failure must be -ENODEV (non-Apple-7+ host)", rc == -ENODEV);
        mu_assert("on failure the state pointer must stay NULL", state == NULL);
    }
    return NULL;
}

static char *test_state_init_rejects_bad_flags(void)
{
    VmafMetalConfiguration cfg = {.device_index = -1, .flags = 0xDEAD};
    VmafMetalState *state = NULL;
    const int rc = vmaf_metal_state_init(&state, cfg);
    mu_assert("non-zero flags -> -EINVAL", rc == -EINVAL);
    mu_assert("bad-flags leaves out-pointer at NULL", state == NULL);
    return NULL;
}

static char *test_state_free_null_is_noop(void)
{
    /* Must not crash on NULL pointer-to-pointer. */
    vmaf_metal_state_free(NULL);

    /* Must not crash and must clear the slot on a NULL value. */
    VmafMetalState *state = NULL;
    vmaf_metal_state_free(&state);
    mu_assert("state_free leaves slot at NULL", state == NULL);
    return NULL;
}

static char *test_list_devices_nonnegative(void)
{
    /* Returns the printed device count (one line per Apple-Family-7+
     * device). 0 on non-Apple-7+ hosts is fine. */
    const int rc = vmaf_metal_list_devices();
    mu_assert("list_devices returns a non-negative count", rc >= 0);
    return NULL;
}

/* ---- Kernel-template helpers (T8-1b runtime / ADR-0420) ---- */

static char *test_kernel_lifecycle_init_runs_or_skips(void)
{
    /* Init requires a valid context. On hosts without Apple-Family-7+
     * the test short-circuits when try_get_ctx returns -ENODEV; the
     * input-validation tests below still run unconditionally. */
    VmafMetalContext *ctx = NULL;
    const int ctx_rc = try_get_ctx(&ctx);
    mu_assert("context_new returns 0 or -ENODEV", ctx_rc == 0 || ctx_rc == -ENODEV);
    if (ctx_rc != 0) {
        return NULL;
    }
    VmafMetalKernelLifecycle lc = {0};
    int rc = vmaf_metal_kernel_lifecycle_init(&lc, ctx);
    mu_assert("kernel_lifecycle_init succeeds on real context", rc == 0);
    mu_assert("cmd_queue handle non-zero after init", lc.cmd_queue != 0);
    mu_assert("submit event non-zero after init", lc.submit != 0);
    mu_assert("finished event non-zero after init", lc.finished != 0);
    rc = vmaf_metal_kernel_lifecycle_close(&lc, ctx);
    mu_assert("kernel_lifecycle_close succeeds", rc == 0);
    vmaf_metal_context_destroy(ctx);
    return NULL;
}

static char *test_kernel_lifecycle_init_rejects_null_lc(void)
{
    const int rc = vmaf_metal_kernel_lifecycle_init(NULL, NULL);
    mu_assert("NULL lc -> -EINVAL", rc == -EINVAL);
    return NULL;
}

static char *test_kernel_buffer_alloc_runs_or_skips(void)
{
    VmafMetalContext *ctx = NULL;
    const int ctx_rc = try_get_ctx(&ctx);
    mu_assert("context_new returns 0 or -ENODEV", ctx_rc == 0 || ctx_rc == -ENODEV);
    if (ctx_rc != 0) {
        return NULL;
    }
    VmafMetalKernelBuffer buf = {0};
    const int rc = vmaf_metal_kernel_buffer_alloc(&buf, ctx, 4096);
    mu_assert("kernel_buffer_alloc succeeds on real context", rc == 0);
    mu_assert("MTLBuffer slot is non-zero after alloc", buf.buffer != 0);
    mu_assert("host_view is non-NULL (Shared storage)", buf.host_view != NULL);
    mu_assert("byte count is recorded", buf.bytes == 4096);
    const int free_rc = vmaf_metal_kernel_buffer_free(&buf, ctx);
    mu_assert("kernel_buffer_free succeeds", free_rc == 0);
    mu_assert("buffer slot cleared after free", buf.buffer == 0);
    vmaf_metal_context_destroy(ctx);
    return NULL;
}

static char *test_kernel_buffer_alloc_rejects_null(void)
{
    const int rc = vmaf_metal_kernel_buffer_alloc(NULL, NULL, 1);
    mu_assert("NULL buf -> -EINVAL", rc == -EINVAL);
    return NULL;
}

static char *test_kernel_lifecycle_close_zero_handles_is_noop(void)
{
    /* Close on an all-zero lifecycle must succeed (mirrors the HIP
     * twin's "safe to call on a partially-initialised lifecycle"
     * contract). */
    VmafMetalKernelLifecycle lc = {0};
    const int rc = vmaf_metal_kernel_lifecycle_close(&lc, NULL);
    mu_assert("kernel_lifecycle_close returns 0 on zero handles", rc == 0);
    return NULL;
}

static char *test_kernel_buffer_free_zero_handles_is_noop(void)
{
    /* Same partially-allocated contract as the lifecycle close. */
    VmafMetalKernelBuffer buf = {0};
    const int rc = vmaf_metal_kernel_buffer_free(&buf, NULL);
    mu_assert("kernel_buffer_free returns 0 on zero handles", rc == 0);
    return NULL;
}

/* ---- First consumer extractor registration (T8-1 / ADR-0361) ---- */

static char *test_motion_v2_metal_extractor_registered(void)
{
    /* The first-consumer scaffold registers `motion_v2_metal` so
     * callers asking by name get a clean "found but kernel not ready"
     * surface rather than "no such extractor". The kernel itself
     * arrives in T8-1c; this test stays "extractor is registered" and
     * does NOT yet tighten to "init returns 0 with a real device". */
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("motion_v2_metal");
    mu_assert("motion_v2_metal extractor must be registered", fex != NULL);
    mu_assert("motion_v2_metal extractor name matches", strcmp(fex->name, "motion_v2_metal") == 0);
    mu_assert("motion_v2_metal extractor must carry the TEMPORAL flag",
              (fex->flags & VMAF_FEATURE_EXTRACTOR_TEMPORAL) != 0);
    return NULL;
}

static char *test_dispatch_strategy_rejects_null_and_unknown(void)
{
    mu_assert("NULL context is unsupported",
              vmaf_metal_dispatch_supports(NULL, "float_psnr_metal") == 0);

    VmafMetalContext *ctx = NULL;
    const int ctx_rc = try_get_ctx(&ctx);
    mu_assert("context_new returns 0 or -ENODEV", ctx_rc == 0 || ctx_rc == -ENODEV);
    if (ctx_rc != 0) {
        return NULL;
    }
    mu_assert("NULL feature is unsupported", vmaf_metal_dispatch_supports(ctx, NULL) == 0);
    mu_assert("unknown feature is unsupported",
              vmaf_metal_dispatch_supports(ctx, "definitely_not_metal") == 0);
    vmaf_metal_context_destroy(ctx);
    return NULL;
}

static char *test_dispatch_strategy_supports_landed_kernels_or_skips(void)
{
    VmafMetalContext *ctx = NULL;
    const int ctx_rc = try_get_ctx(&ctx);
    mu_assert("context_new returns 0 or -ENODEV", ctx_rc == 0 || ctx_rc == -ENODEV);
    if (ctx_rc != 0) {
        return NULL;
    }
    mu_assert("extractor name is supported",
              vmaf_metal_dispatch_supports(ctx, "float_psnr_metal") == 1);
    mu_assert("provided feature key is supported",
              vmaf_metal_dispatch_supports(ctx, "psnr_y") == 1);
    mu_assert("motion_v2 extractor is supported",
              vmaf_metal_dispatch_supports(ctx, "motion_v2_metal") == 1);
    mu_assert("ms-ssim provided feature key is supported",
              vmaf_metal_dispatch_supports(ctx, "float_ms_ssim") == 1);
    vmaf_metal_context_destroy(ctx);
    return NULL;
}

/* ---- T8-IOS impl contract (ADR-0423) ---- */
/* IOSurface-import entry points were scaffolded as -ENOSYS in the
 * initial PR and flipped to real semantics in T8-IOS-b (same PR,
 * "no scaffold-only" decision per ADR-0423 §"Implementation folded
 * in"). Tests assert real input-validation + the
 * device-handle gate; no live IOSurface is exercised here (that
 * requires a CVPixelBufferRef from VideoToolbox — covered by the
 * ffmpeg-side integration tests under tools/test/). */

static char *test_iosurface_state_init_external_default_device_or_enodev(void)
{
    /* device == 0 falls back to MTLCreateSystemDefaultDevice (the
     * FFmpeg n8.1.1 path until AVMetalDeviceContext lands). On
     * Apple-Family-7+ hosts that succeeds; everywhere else it
     * surfaces as -ENODEV. */
    VmafMetalExternalHandles h = {.device = 0, .command_queue = 0};
    VmafMetalState *state = NULL;
    const int rc = vmaf_metal_state_init_external(&state, h);
    mu_assert("default-device init returns 0 or -ENODEV", rc == 0 || rc == -ENODEV);
    if (rc == 0) {
        mu_assert("state populated on success", state != NULL);
        vmaf_metal_state_free(&state);
        mu_assert("state_free clears the slot", state == NULL);
    } else {
        mu_assert("state stays NULL on -ENODEV", state == NULL);
    }
    return NULL;
}

static char *test_iosurface_state_init_external_rejects_null_out(void)
{
    VmafMetalExternalHandles h = {.device = 0, .command_queue = 0};
    const int rc = vmaf_metal_state_init_external(NULL, h);
    mu_assert("NULL out -> -EINVAL", rc == -EINVAL);
    return NULL;
}

static char *test_iosurface_picture_import_rejects_null_state(void)
{
    /* NULL state is a caller bug; -EINVAL before we touch any IO. */
    const int rc = vmaf_metal_picture_import(NULL, 0, 0, 1920, 1080, 8, 1, 0);
    mu_assert("NULL state -> -EINVAL", rc == -EINVAL);
    return NULL;
}

static char *test_iosurface_wait_compute_rejects_null_state(void)
{
    const int rc = vmaf_metal_wait_compute(NULL);
    mu_assert("NULL state -> -EINVAL", rc == -EINVAL);
    return NULL;
}

static char *test_iosurface_read_imported_pictures_rejects_null_ctx(void)
{
    const int rc = vmaf_metal_read_imported_pictures(NULL, 0);
    mu_assert("NULL ctx -> -EINVAL", rc == -EINVAL);
    return NULL;
}

typedef char *(*test_fn)(void);

static const test_fn test_table[] = {
    test_context_new_rejects_null_out,
    test_context_destroy_null_is_noop,
    test_context_default_device_or_skip,
    test_device_count_nonnegative,
    test_available_reports_built,
    test_state_init_succeeds_or_enodev,
    test_state_init_rejects_bad_flags,
    test_state_free_null_is_noop,
    test_list_devices_nonnegative,
    /* T8-1b kernel-template runtime (ADR-0420) */
    test_kernel_lifecycle_init_runs_or_skips,
    test_kernel_lifecycle_init_rejects_null_lc,
    test_kernel_buffer_alloc_runs_or_skips,
    test_kernel_buffer_alloc_rejects_null,
    test_kernel_lifecycle_close_zero_handles_is_noop,
    test_kernel_buffer_free_zero_handles_is_noop,
    /* T8-1 first-consumer registration — kernel arrives in T8-1c (ADR-0361). */
    test_motion_v2_metal_extractor_registered,
    test_dispatch_strategy_rejects_null_and_unknown,
    test_dispatch_strategy_supports_landed_kernels_or_skips,
    /* T8-IOS impl contract — input-validation (ADR-0423). */
    test_iosurface_state_init_external_default_device_or_enodev,
    test_iosurface_state_init_external_rejects_null_out,
    test_iosurface_picture_import_rejects_null_state,
    test_iosurface_wait_compute_rejects_null_state,
    test_iosurface_read_imported_pictures_rejects_null_ctx,
};

static const size_t test_table_len = sizeof(test_table) / sizeof(test_table[0]);

char *run_tests(void)
{
    for (size_t i = 0; i < test_table_len; ++i) {
        mu_run_test(test_table[i]);
    }
    return NULL;
}
