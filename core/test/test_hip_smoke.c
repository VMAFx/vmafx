/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  HIP backend smoke test (ADR-0212 / T7-10b runtime).
 *
 *  Runtime PR (T7-10b) flips the contract from "every public API
 *  returns -ENOSYS" (the audit-first scaffold posture pinned by
 *  ADR-0212) to:
 *
 *    - `vmaf_hip_device_count()` returns >= 0 (real HIP probe).
 *    - Kernel-template lifecycle helpers succeed end-to-end when an
 *      AMD GPU is visible, including stream + event create/destroy
 *      and a pinned-host / device round-trip via `hipMemcpy`.
 *    - Public C-API `vmaf_hip_state_init` succeeds on a host with
 *      >= 1 visible device, returns -ENODEV otherwise.
 *
 *  Tests that need a live device skip themselves cleanly when the
 *  runtime reports zero devices, mirroring the
 *  Vulkan-on-lavapipe-less-CI pattern documented in
 *  ADR-0212 §"What lands next" point 1.
 *
 *  Mirrors libvmaf/test/test_vulkan_smoke.c.
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define __HIP_PLATFORM_AMD__ 1
#include <hip/hip_runtime_api.h>

#include "test.h"

#include "feature/feature_extractor.h"
#include "hip/common.h"
#include "hip/kernel_template.h"
#include "libvmaf/libvmaf_hip.h"

/*
 * Pinned-host -> device -> pinned-host memcpy round-trip helper.
 * Returns 0 on success or a negative POSIX errno on a HIP failure.
 * The helper exists so the kernel-template readback test can pin
 * the load-bearing "pinned-host buffer survives a device round-
 * trip" contract without hauling in a kernel launch. Mirrors the
 * SYCL T5-1b smoke's `q.copy()`-based round-trip.
 */
int test_hip_memcpy_round_trip(void *device, void *host_pinned, size_t bytes);
int test_hip_memcpy_round_trip(void *device, void *host_pinned, size_t bytes)
{
    if (device == NULL || host_pinned == NULL || bytes == 0) {
        return -EINVAL;
    }
    hipError_t rc = hipMemcpy(device, host_pinned, bytes, hipMemcpyHostToDevice);
    if (rc != hipSuccess) {
        return -EIO;
    }
    /* Stomp the pinned host buffer so the readback below cannot
     * succeed on the original write alone. */
    (void)memset(host_pinned, 0, bytes);
    rc = hipMemcpy(host_pinned, device, bytes, hipMemcpyDeviceToHost);
    if (rc != hipSuccess) {
        return -EIO;
    }
    return 0;
}

/* ---- Internal context (libvmaf/src/hip/common.h) ---- */

static char *test_context_new_returns_zeroed_struct(void)
{
    /* The scaffold's calloc + struct initialisation succeeds even
     * before a real device is selected. The opaque pointer is
     * non-NULL on success. */
    VmafHipContext *ctx = NULL;
    int rc = vmaf_hip_context_new(&ctx, 0);
    mu_assert("scaffold context_new must succeed", rc == 0);
    mu_assert("scaffold context must be populated", ctx != NULL);
    vmaf_hip_context_destroy(ctx);
    return NULL;
}

static char *test_context_new_rejects_null_out(void)
{
    int rc = vmaf_hip_context_new(NULL, 0);
    mu_assert("NULL out -> -EINVAL", rc == -EINVAL);
    return NULL;
}

static char *test_context_destroy_null_is_noop(void)
{
    /* No assertion needed — must not crash. */
    vmaf_hip_context_destroy(NULL);
    return NULL;
}

static char *test_device_count_runtime_returns_nonneg(void)
{
    /* T7-10b runtime: `vmaf_hip_device_count` now wraps
     * `hipGetDeviceCount`. Returns 0 cleanly on hosts without an AMD
     * GPU (or without a working HIP runtime), >= 1 on a working
     * ROCm install. The test is contract-only; the device-resident
     * checks below decide whether to exercise more. */
    const int n = vmaf_hip_device_count();
    mu_assert("device_count returns a non-negative count", n >= 0);
    return NULL;
}

/* ---- Public C-API (libvmaf/include/libvmaf/libvmaf_hip.h) ---- */

static char *test_available_reports_build_flag(void)
{
    /* When this TU compiles under -Denable_hip=true the meson glue
     * defines HAVE_HIP=1 for the test executable, so the function
     * reports 1; on the default no-HIP build it reports 0. The smoke
     * test is wired in libvmaf/test/meson.build only under
     * `if get_option('enable_hip') == true`, so this branch matches
     * the build that exercises the test. */
    const int avail = vmaf_hip_available();
    mu_assert("vmaf_hip_available returns a boolean", avail == 0 || avail == 1);
    return NULL;
}

static char *test_state_init_runtime_contract(void)
{
    /* T7-10b runtime: `vmaf_hip_state_init` succeeds when at least
     * one HIP device is visible to the runtime, otherwise returns
     * -ENODEV. Skip the success branch on hosts without a GPU; the
     * `>= 0` device-count contract is pinned separately. */
    VmafHipConfiguration cfg = {.device_index = -1, .flags = 0};
    VmafHipState *state = NULL;
    int rc = vmaf_hip_state_init(&state, cfg);
    if (vmaf_hip_device_count() <= 0) {
        mu_assert("state_init reports -ENODEV when no device is visible", rc == -ENODEV);
        mu_assert("state_init leaves out-pointer NULL on -ENODEV", state == NULL);
        return NULL;
    }
    mu_assert("state_init returns 0 with a real HIP device", rc == 0);
    mu_assert("state_init populates out-pointer on success", state != NULL);
    vmaf_hip_state_free(&state);
    mu_assert("state_free clears the slot", state == NULL);
    return NULL;
}

static char *test_import_state_validates_arguments(void)
{
    /* ADR-0519: the library-side state-binding stub was promoted from
     * -ENOSYS to a real implementation in libvmaf/src/libvmaf.c. The
     * function now stashes the caller-imported VmafHipState on the
     * VmafContext (same lifetime model as SYCL / Vulkan / Metal).
     * NULL arguments return -EINVAL; the device-bound success path is
     * covered by test_import_state_succeeds_with_real_state below. */
    int rc = vmaf_hip_import_state(NULL, NULL);
    mu_assert("import_state with NULL args returns -EINVAL", rc == -EINVAL);
    return NULL;
}

static char *test_import_state_succeeds_with_real_state(void)
{
    /* ADR-0519: end-to-end happy-path on a host with a visible HIP
     * device. Skip cleanly when no device is visible (CI without an
     * AMD GPU). The success branch is the load-bearing assertion the
     * `vmaf --backend hip` CLI exit-status gate depends on. */
    if (vmaf_hip_device_count() <= 0) {
        return NULL;
    }
    VmafHipConfiguration cfg = {.device_index = -1, .flags = 0};
    VmafHipState *state = NULL;
    int rc = vmaf_hip_state_init(&state, cfg);
    mu_assert("state_init returns 0 with a real HIP device", rc == 0);
    mu_assert("state_init populates out-pointer on success", state != NULL);

    VmafConfiguration vcfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    int err = vmaf_init(&vmaf, vcfg);
    mu_assert("vmaf_init succeeds", err == 0);
    mu_assert("vmaf_init populates context", vmaf != NULL);

    err = vmaf_hip_import_state(vmaf, state);
    mu_assert("import_state stashes the state and returns 0", err == 0);

    /* Re-import is idempotent and overwrites the slot. */
    err = vmaf_hip_import_state(vmaf, state);
    mu_assert("re-import_state is idempotent", err == 0);

    /* NULL state on a non-NULL ctx still rejected. */
    err = vmaf_hip_import_state(vmaf, NULL);
    mu_assert("import_state(NULL state) returns -EINVAL", err == -EINVAL);

    err = vmaf_close(vmaf);
    mu_assert("vmaf_close returns 0", err == 0);

    /* Caller still owns the state — must free after vmaf_close. */
    vmaf_hip_state_free(&state);
    mu_assert("state_free clears the slot", state == NULL);
    return NULL;
}

static char *test_state_free_null_is_noop(void)
{
    /* Must not crash on NULL pointer-to-pointer. */
    vmaf_hip_state_free(NULL);

    /* Must not crash and must clear the slot on a NULL value. */
    VmafHipState *state = NULL;
    vmaf_hip_state_free(&state);
    mu_assert("state_free leaves slot at NULL", state == NULL);
    return NULL;
}

static char *test_list_devices_returns_count(void)
{
    /* T7-10b runtime: `vmaf_hip_list_devices` returns the device
     * count (or a negative errno on a runtime-load failure). On a
     * host with no AMD GPU the count is 0; the test only asserts
     * the >= 0 contract so it stays portable across CI runners. */
    int rc = vmaf_hip_list_devices();
    mu_assert("list_devices returns a non-negative count", rc >= 0);
    return NULL;
}

/* ---- Kernel-template helpers (T7-10b runtime / ADR-0212) ---- */
/*
 * Runtime PR (T7-10b) flips every helper from -ENOSYS to a real
 * HIP runtime call. Tests that need a live device skip themselves
 * cleanly when no AMD GPU is visible; the partial-init / null-
 * argument paths stay verifiable on every host.
 */

static char *test_kernel_lifecycle_init_runtime(void)
{
    /* Init creates a stream + 2 events. On hosts without HIP it
     * returns a negative errno (-ENODEV / -EINVAL / -EIO depending
     * on the runtime's failure mode). On a host with >=1 HIP
     * device, init succeeds and populates non-zero handles. */
    VmafHipKernelLifecycle lc = {0};
    const int rc = vmaf_hip_kernel_lifecycle_init(&lc, NULL);
    if (vmaf_hip_device_count() <= 0) {
        mu_assert("kernel_lifecycle_init returns negative errno when no device", rc < 0);
        mu_assert("kernel_lifecycle_init leaves stream handle at 0", lc.str == 0);
        mu_assert("kernel_lifecycle_init leaves submit event at 0", lc.submit == 0);
        mu_assert("kernel_lifecycle_init leaves finished event at 0", lc.finished == 0);
        return NULL;
    }
    mu_assert("kernel_lifecycle_init returns 0 on a real device", rc == 0);
    mu_assert("kernel_lifecycle_init populates stream handle", lc.str != 0);
    mu_assert("kernel_lifecycle_init populates submit event", lc.submit != 0);
    mu_assert("kernel_lifecycle_init populates finished event", lc.finished != 0);
    const int close_rc = vmaf_hip_kernel_lifecycle_close(&lc, NULL);
    mu_assert("kernel_lifecycle_close clean tear-down", close_rc == 0);
    mu_assert("kernel_lifecycle_close clears stream handle", lc.str == 0);
    mu_assert("kernel_lifecycle_close clears submit event", lc.submit == 0);
    mu_assert("kernel_lifecycle_close clears finished event", lc.finished == 0);
    return NULL;
}

static char *test_kernel_readback_alloc_runtime(void)
{
    /* Alloc requests one device buffer + one pinned host buffer.
     * Skip on hosts without HIP. With a device, exercise a
     * round-trip: write a sentinel into the pinned buffer, copy
     * host -> device -> back, and verify the byte arrived intact.
     * This is the load-bearing "pinned host alloc actually
     * round-trips through the device" check the runtime PR has to
     * pin. */
    VmafHipKernelReadback rb = {0};
    const int rc = vmaf_hip_kernel_readback_alloc(&rb, NULL, sizeof(uint64_t));
    if (vmaf_hip_device_count() <= 0) {
        mu_assert("kernel_readback_alloc returns negative errno when no device", rc < 0);
        mu_assert("kernel_readback_alloc leaves device pointer at NULL", rb.device == NULL);
        mu_assert("kernel_readback_alloc leaves host_pinned pointer at NULL",
                  rb.host_pinned == NULL);
        return NULL;
    }
    mu_assert("kernel_readback_alloc succeeds on a real device", rc == 0);
    mu_assert("kernel_readback_alloc populates device pointer", rb.device != NULL);
    mu_assert("kernel_readback_alloc populates host_pinned pointer", rb.host_pinned != NULL);
    mu_assert("kernel_readback_alloc records requested byte count", rb.bytes == sizeof(uint64_t));
    /* Round-trip: pinned host -> device -> back. */
    uint64_t sentinel = 0xCAFEBABE12345678ULL;
    /* Use volatile so the optimiser cannot fold the read after the
     * second hipMemcpy into the original `sentinel` constant. */
    volatile uint64_t *host = (volatile uint64_t *)rb.host_pinned;
    *host = sentinel;
    extern int test_hip_memcpy_round_trip(void *device, void *host_pinned, size_t bytes);
    const int trip_rc = test_hip_memcpy_round_trip(rb.device, rb.host_pinned, rb.bytes);
    mu_assert("hipMemcpy round-trip succeeds", trip_rc == 0);
    mu_assert("round-trip preserves the sentinel byte pattern", *host == sentinel);
    const int free_rc = vmaf_hip_kernel_readback_free(&rb, NULL);
    mu_assert("kernel_readback_free clean release", free_rc == 0);
    mu_assert("kernel_readback_free clears device pointer", rb.device == NULL);
    mu_assert("kernel_readback_free clears host_pinned pointer", rb.host_pinned == NULL);
    return NULL;
}

static char *test_kernel_lifecycle_close_zero_is_noop(void)
{
    /* Close on an all-zero lifecycle must succeed (mirrors the CUDA
     * twin's "safe to call on a partially-initialised lifecycle"
     * contract). The runtime body short-circuits when every handle
     * is zero. */
    VmafHipKernelLifecycle lc = {0};
    const int rc = vmaf_hip_kernel_lifecycle_close(&lc, NULL);
    mu_assert("kernel_lifecycle_close returns 0 on zero handles", rc == 0);
    return NULL;
}

static char *test_kernel_readback_free_zero_is_noop(void)
{
    /* Same partially-allocated contract as the lifecycle close. */
    VmafHipKernelReadback rb = {0};
    const int rc = vmaf_hip_kernel_readback_free(&rb, NULL);
    mu_assert("kernel_readback_free returns 0 on zero handles", rc == 0);
    return NULL;
}

/* ---- First consumer extractor registration (T7-10 / ADR-0241) ---- */

static char *test_psnr_hip_extractor_registered(void)
{
    /* The first-consumer PR (T7-10 / ADR-0241) flips the registration
     * posture: previously the docs noted "kernel stubs intentionally do
     * not register with the feature registry"; for `psnr_hip` we now
     * register the extractor so callers asking by name get a clean
     * "found but runtime not ready" surface. The runtime PR (T7-10b)
     * keeps this assertion green and tightens it to "init returns 0
     * with a real device". */
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("psnr_hip");
    mu_assert("psnr_hip extractor must be registered", fex != NULL);
    mu_assert("psnr_hip extractor name matches", strcmp(fex->name, "psnr_hip") == 0);
    return NULL;
}

static char *test_ciede_hip_extractor_registered(void)
{
    /* Third-consumer PR (ADR-0257) extends the same registration
     * contract to `ciede_hip`: extractor is found by name, with the
     * matching `.name` string. `init()` is not invoked here — the
     * scaffold returns -ENOSYS at that layer; the registration smoke
     * test only pins the lookup contract. The runtime PR (T7-10b)
     * keeps this assertion green and tightens it to "init returns 0
     * with a real device". */
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("ciede_hip");
    mu_assert("ciede_hip extractor must be registered", fex != NULL);
    mu_assert("ciede_hip extractor name matches", strcmp(fex->name, "ciede_hip") == 0);
    return NULL;
}

static char *test_float_moment_hip_extractor_registered(void)
{
    /* Fourth-consumer PR (ADR-0258) extends the same registration
     * contract to `float_moment_hip`: extractor is found by name with
     * the matching `.name` string. Pins the registration shape — the
     * runtime PR (T7-10b) keeps the assertion green. */
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("float_moment_hip");
    mu_assert("float_moment_hip extractor must be registered", fex != NULL);
    mu_assert("float_moment_hip extractor name matches",
              strcmp(fex->name, "float_moment_hip") == 0);
    return NULL;
}

/* ---- Sixth consumer extractor registration (T7-10b / ADR-0267) ---- */

static char *test_motion_v2_hip_extractor_registered(void)
{
    /* Sixth-consumer PR (ADR-0267) extends the same registration
     * contract to `motion_v2_hip`. The CUDA twin's name is
     * `motion_v2_cuda` (without the `integer_` prefix on the
     * extractor name even though the source file is
     * `integer_motion_v2_cuda.c`); the HIP twin keeps the same
     * naming choice. Smoke test only pins the lookup contract;
     * the runtime PR (T7-10b) wires the kernel and tightens this
     * assertion to "init returns 0 with a real device". */
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("motion_v2_hip");
    mu_assert("motion_v2_hip extractor must be registered", fex != NULL);
    mu_assert("motion_v2_hip extractor name matches", strcmp(fex->name, "motion_v2_hip") == 0);
    return NULL;
}

static char *test_float_motion_hip_extractor_registered(void)
{
    /* Seventh-consumer PR (ADR-0273) extends the same registration
     * contract to `float_motion_hip`: extractor is found by name with
     * the matching `.name` string, and carries the TEMPORAL flag bit
     * (load-bearing for the feature engine's collect-before-next-submit
     * scheduling that motion-class metrics rely on). The runtime PR
     * (T7-10b) keeps these assertions green. */
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("float_motion_hip");
    mu_assert("float_motion_hip extractor must be registered", fex != NULL);
    mu_assert("float_motion_hip extractor name matches",
              strcmp(fex->name, "float_motion_hip") == 0);
    mu_assert("float_motion_hip extractor must carry the TEMPORAL flag",
              (fex->flags & VMAF_FEATURE_EXTRACTOR_TEMPORAL) != 0);
    return NULL;
}

static char *test_float_ssim_hip_extractor_registered(void)
{
    /* Eighth-consumer PR (ADR-0274) extends the same registration
     * contract to `float_ssim_hip`: extractor is found by name with
     * the matching `.name` string. Pins the two-dispatch shape via
     * `chars.n_dispatches_per_frame == 2` so the runtime PR's
     * dispatch-counter accounting (and the `places=4` cross-backend
     * gate's per-frame-cost model) inherits a consumer that exercises
     * the multi-dispatch path. */
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("float_ssim_hip");
    mu_assert("float_ssim_hip extractor must be registered", fex != NULL);
    mu_assert("float_ssim_hip extractor name matches", strcmp(fex->name, "float_ssim_hip") == 0);
    mu_assert("float_ssim_hip extractor reports two dispatches per frame",
              fex->chars.n_dispatches_per_frame == 2);
    return NULL;
}

/* ---- Second consumer: float_psnr_hip (T7-10b / ADR-0254) ---- */

static char *test_float_psnr_hip_extractor_registered(void)
{
    /* T7-10b first real kernel (ADR-0254): `float_psnr_hip` is
     * registered and discoverable by name. Pins the registration shape
     * (name, is_reduction_only, n_dispatches_per_frame) so the runtime
     * PR's dispatch-accounting inherits a proven contract.
     *
     * With `enable_hipcc=true` on a ROCm host this assertion stays green
     * AND init() succeeds. Without hipcc the HSACO symbol is absent and
     * init() returns -ENOSYS — the lookup still succeeds. */
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("float_psnr_hip");
    mu_assert("float_psnr_hip extractor must be registered", fex != NULL);
    mu_assert("float_psnr_hip extractor name matches", strcmp(fex->name, "float_psnr_hip") == 0);
    mu_assert("float_psnr_hip extractor is reduction-only", fex->chars.is_reduction_only);
    mu_assert("float_psnr_hip extractor has one dispatch per frame",
              fex->chars.n_dispatches_per_frame == 1);
    return NULL;
}

/* ---- ADR-0530: integer_motion_hip flag-promotion + selectable
 * dispatch. Registration + HIP-flag presence are the load-bearing
 * assertions; the runtime kernel launch is validated by the CLI
 * reproducer in the PR description (AMD_LOG_LEVEL=3 trace shows
 * `calculate_motion_score_kernel_8bpc` dispatched once per frame). */

static char *test_integer_motion_hip_extractor_registered(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("motion_hip");
    mu_assert("motion_hip extractor must be registered (ADR-0530)", fex != NULL);
    mu_assert("motion_hip extractor name matches", strcmp(fex->name, "motion_hip") == 0);
    mu_assert("motion_hip carries the TEMPORAL flag",
              (fex->flags & VMAF_FEATURE_EXTRACTOR_TEMPORAL) != 0);
    mu_assert("motion_hip carries the HIP flag (ADR-0530 promotion)",
              (fex->flags & VMAF_FEATURE_EXTRACTOR_HIP) != 0);
    return NULL;
}

static char *test_integer_motion_hip_dispatch_picks_hip(void)
{
    /* The model-driven dispatch resolves `VMAF_integer_feature_motion2_score`
     * to the HIP-flagged extractor when the HIP flag is part of the
     * fex_flags mask (i.e. when a HIP state has been imported via
     * vmaf_hip_import_state). With flags=0 the CPU twin still wins by
     * registry order. The fallback path (ADR-0530) returns the CPU
     * twin when an unflagged feature has no HIP-flagged provider. */
    VmafFeatureExtractor *cpu_fex =
        vmaf_get_feature_extractor_by_feature_name("VMAF_integer_feature_motion2_score", 0);
    mu_assert("CPU motion2 extractor resolvable with flags=0", cpu_fex != NULL);
    /* CPU integer_motion extractor's registry name is `motion` (source
     * file is `integer_motion.c` but the registry name drops the
     * `integer_` prefix to match upstream Netflix/vmaf history). */
    mu_assert("flags=0 picks the CPU twin (name=motion)", strcmp(cpu_fex->name, "motion") == 0);

    VmafFeatureExtractor *hip_fex = vmaf_get_feature_extractor_by_feature_name(
        "VMAF_integer_feature_motion2_score", VMAF_FEATURE_EXTRACTOR_HIP);
    mu_assert("HIP motion2 extractor resolvable with HIP flag", hip_fex != NULL);
    mu_assert("HIP flag picks the HIP twin (name=motion_hip)",
              strcmp(hip_fex->name, "motion_hip") == 0);
    return NULL;
}

/* ---- ADR-0533: full HIP-extractor registration sweep ---- */
/*
 * One assertion per newly-registered HIP feature extractor. Pins the
 * load-bearing contract that ADR-0523 first established: every TU under
 * `libvmaf/src/feature/hip/` that defines a `VmafFeatureExtractor
 * vmaf_fex_*_hip` symbol must be discoverable via
 * `vmaf_get_feature_extractor_by_name(<name>)` once compiled into the
 * HIP runtime archive. Without these assertions a future drop-out from
 * `hip_sources` (or from the `extern` block in feature_extractor.c)
 * fails silently — the registry returns NULL and the CLI's
 * `--feature <name>` plumbing dies at the lookup step. Scaffold posture
 * is unaffected: the test only pins lookup, not `init()` — `init()`
 * still returns -ENOSYS on builds without `enable_hipcc=true` for the
 * scaffold TUs.
 */

static char *test_float_vif_hip_extractor_registered(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("float_vif_hip");
    mu_assert("float_vif_hip extractor must be registered", fex != NULL);
    mu_assert("float_vif_hip extractor name matches", strcmp(fex->name, "float_vif_hip") == 0);
    return NULL;
}

static char *test_adm_hip_extractor_registered(void)
{
    /* `vmaf_fex_integer_adm_hip` registers under the name `"adm_hip"`
     * (no `integer_` prefix), mirroring the convention chosen at the
     * source-file level. The CUDA twin uses `integer_adm_cuda` →
     * `adm` / `adm_scale*`; the HIP twin keeps the `adm_hip` short
     * form so callers can opt in without re-typing the `integer_`. */
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("adm_hip");
    mu_assert("adm_hip extractor must be registered", fex != NULL);
    mu_assert("adm_hip extractor name matches", strcmp(fex->name, "adm_hip") == 0);
    return NULL;
}

static char *test_integer_ms_ssim_hip_extractor_registered(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("integer_ms_ssim_hip");
    mu_assert("integer_ms_ssim_hip extractor must be registered", fex != NULL);
    mu_assert("integer_ms_ssim_hip extractor name matches",
              strcmp(fex->name, "integer_ms_ssim_hip") == 0);
    return NULL;
}

static char *test_psnr_hvs_hip_extractor_registered(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("psnr_hvs_hip");
    mu_assert("psnr_hvs_hip extractor must be registered", fex != NULL);
    mu_assert("psnr_hvs_hip extractor name matches", strcmp(fex->name, "psnr_hvs_hip") == 0);
    return NULL;
}

static char *test_integer_ssim_hip_extractor_registered(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("integer_ssim_hip");
    mu_assert("integer_ssim_hip extractor must be registered", fex != NULL);
    mu_assert("integer_ssim_hip extractor name matches",
              strcmp(fex->name, "integer_ssim_hip") == 0);
    return NULL;
}

static char *test_ssimulacra2_hip_extractor_registered(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("ssimulacra2_hip");
    mu_assert("ssimulacra2_hip extractor must be registered", fex != NULL);
    mu_assert("ssimulacra2_hip extractor name matches", strcmp(fex->name, "ssimulacra2_hip") == 0);
    return NULL;
}

/* ---- ADR-0537: integer VIF HIP kernel-crash fix ---- */
/* Pins the contract that closes the ADR-0530 follow-up: the integer
 * VIF HIP extractor is registered under name `vif_hip`, carries the
 * HIP flag bit (re-enabled after the kernel-level fix), and the
 * model-driven dispatch routes `VMAF_integer_feature_vif_scale0_score`
 * to it when the HIP flag mask is active.  Lookup-only — runtime
 * dispatch is verified separately by the end-to-end repro in the
 * PR description. */

static char *test_integer_vif_hip_extractor_registered(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("vif_hip");
    mu_assert("vif_hip extractor must be registered (ADR-0537)", fex != NULL);
    mu_assert("vif_hip extractor name matches", strcmp(fex->name, "vif_hip") == 0);
    mu_assert("vif_hip carries the HIP flag (ADR-0537 re-enabled after kernel fix)",
              (fex->flags & VMAF_FEATURE_EXTRACTOR_HIP) != 0);
    return NULL;
}

static char *test_integer_vif_hip_dispatch_picks_hip(void)
{
    /* With flags=0 the CPU twin still wins by registry order; the
     * ADR-0530 fallback path falls back to CPU when no HIP-flagged
     * provider exists.  With the HIP flag mask active the model-driven
     * dispatch picks the HIP twin. */
    VmafFeatureExtractor *cpu_fex =
        vmaf_get_feature_extractor_by_feature_name("VMAF_integer_feature_vif_scale0_score", 0);
    mu_assert("CPU vif extractor resolvable with flags=0 (ADR-0537)", cpu_fex != NULL);
    /* The CPU `integer_vif` extractor's registry name is `vif` (matches
     * upstream Netflix/vmaf: file name drops the `integer_` prefix at
     * registration). */
    mu_assert("flags=0 picks the CPU vif twin", strcmp(cpu_fex->name, "vif") == 0);

    VmafFeatureExtractor *hip_fex = vmaf_get_feature_extractor_by_feature_name(
        "VMAF_integer_feature_vif_scale0_score", VMAF_FEATURE_EXTRACTOR_HIP);
    mu_assert("HIP vif extractor resolvable with HIP flag (ADR-0537)", hip_fex != NULL);
    mu_assert("HIP flag picks the HIP vif twin (name=vif_hip)",
              strcmp(hip_fex->name, "vif_hip") == 0);
    return NULL;
}

/* Function-pointer table keeps `run_tests` flat — without it,
 * `mu_run_test` macro-expands to a branching pair per test, blowing
 * past clang-tidy's `readability-function-size` 15-branch budget at
 * 9 sub-tests. The table-driven form is also what
 * libvmaf/test/test_vulkan_smoke.c grew to once its sub-test count
 * climbed past the threshold. */
typedef char *(*test_fn)(void);

static const test_fn test_table[] = {
    test_context_new_returns_zeroed_struct,
    test_context_new_rejects_null_out,
    test_context_destroy_null_is_noop,
    test_device_count_runtime_returns_nonneg,
    test_available_reports_build_flag,
    test_state_init_runtime_contract,
    test_import_state_validates_arguments,
    test_import_state_succeeds_with_real_state,
    test_state_free_null_is_noop,
    test_list_devices_returns_count,
    /* T7-10 first consumer (ADR-0241) */
    test_kernel_lifecycle_init_runtime,
    test_kernel_readback_alloc_runtime,
    test_kernel_lifecycle_close_zero_is_noop,
    test_kernel_readback_free_zero_is_noop,
    test_psnr_hip_extractor_registered,
    /* T7-10b third + fourth consumers (ADR-0257 / ADR-0258) */
    test_ciede_hip_extractor_registered,
    test_float_moment_hip_extractor_registered,
    /* T7-10b sixth consumer (ADR-0267) */
    test_motion_v2_hip_extractor_registered,
    /* T7-10b seventh + eighth consumers (ADR-0273 / ADR-0274) */
    test_float_motion_hip_extractor_registered,
    test_float_ssim_hip_extractor_registered,
    /* T7-10b first real kernel (ADR-0254): float_psnr_hip */
    test_float_psnr_hip_extractor_registered,
    /* ADR-0530: integer_motion_hip flag-promotion + selectable
     * dispatch. */
    test_integer_motion_hip_extractor_registered,
    test_integer_motion_hip_dispatch_picks_hip,
    /* ADR-0533: full HIP-extractor registration sweep — six TUs that
     * declared `vmaf_fex_*_hip` but were never wired into the registry
     * are now resolvable by name. One assertion per extractor pins the
     * lookup contract. */
    test_float_vif_hip_extractor_registered,
    test_adm_hip_extractor_registered,
    test_integer_ms_ssim_hip_extractor_registered,
    test_psnr_hvs_hip_extractor_registered,
    test_integer_ssim_hip_extractor_registered,
    test_ssimulacra2_hip_extractor_registered,
    /* ADR-0537: integer_vif_hip kernel-crash fix + flag re-enable. */
    test_integer_vif_hip_extractor_registered,
    test_integer_vif_hip_dispatch_picks_hip,
};

static const size_t test_table_len = sizeof(test_table) / sizeof(test_table[0]);

char *run_tests(void)
{
    for (size_t i = 0; i < test_table_len; ++i) {
        mu_run_test(test_table[i]);
    }
    return NULL;
}
