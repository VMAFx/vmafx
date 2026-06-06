/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 *
 *  GPU dispatch-runtime coverage test (host-only).
 *
 *  Targets the *shared* GPU-runtime surface used by every backend's
 *  dispatch_strategy module (CUDA, SYCL, HIP, Metal). Coverage gap
 *  identified by the runtime-files audit (2026-05-31): the three
 *  per-backend dispatch_strategy TUs and the shared
 *  `gpu_dispatch_env` / `gpu_dispatch_parse` helpers had no direct
 *  unit coverage prior to this file — only an integration-level
 *  exercise via the per-feature smoke tests.
 *
 *  Host-only by design: every assertion runs without a GPU driver,
 *  without a CUDA/HIP/SYCL SDK, and without any libvmaf state. The
 *  per-backend dispatch_strategy TUs are linked directly into this
 *  executable so the test exercises the *real* shipped logic rather
 *  than a re-implementation. See CLAUDE.md §15 — falls back to the
 *  host CPU-only audit posture when the dev-mcp container isn't
 *  available (no GPU sniffing needed for these tests).
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* setenv / unsetenv are POSIX and not available on Windows (MinGW64 or
 * MSVC). Provide portable wrappers that delegate to _putenv_s / _putenv
 * on Windows and to the POSIX originals everywhere else. */
#ifdef _WIN32
#include <stdio.h> /* snprintf */
static int test_setenv(const char *name, const char *value, int overwrite)
{
    (void)overwrite; /* _putenv_s always overwrites */
    return _putenv_s(name, value);
}
static int test_unsetenv(const char *name)
{
    /* Setting the variable to the empty string is the _putenv_s idiom
     * for removal on Windows; _putenv("NAME=") would also work. */
    return _putenv_s(name, "");
}
#define setenv(name, value, overwrite) test_setenv(name, value, overwrite)
#define unsetenv(name) test_unsetenv(name)
#endif /* _WIN32 */

#include "test.h"

#include "gpu_dispatch_env.h"
#include "gpu_dispatch_parse.h"

#include "cuda/dispatch_strategy.h"
#include "hip/dispatch_strategy.h"

/* ---- gpu_dispatch_parse.h — header-only static-inline parser ---- */

static char *test_parse_null_inputs_decline(void)
{
    int idx = 99;
    const char *const names[] = {"direct", "graph", NULL};
    mu_assert("NULL env declines", vmaf_gpu_dispatch_parse_env(NULL, "vif", names, &idx) == 0);
    mu_assert("NULL feature declines",
              vmaf_gpu_dispatch_parse_env("vif:graph", NULL, names, &idx) == 0);
    mu_assert("NULL names declines",
              vmaf_gpu_dispatch_parse_env("vif:graph", "vif", NULL, &idx) == 0);
    mu_assert("NULL out declines",
              vmaf_gpu_dispatch_parse_env("vif:graph", "vif", names, NULL) == 0);
    mu_assert("decline leaves out untouched", idx == 99);
    return NULL;
}

static char *test_parse_matches_strategy_by_name(void)
{
    int idx = -1;
    const char *const names[] = {"direct", "graph", NULL};
    mu_assert("vif:graph matches",
              vmaf_gpu_dispatch_parse_env("vif:graph", "vif", names, &idx) == 1);
    mu_assert("graph maps to index 1", idx == 1);

    idx = -1;
    mu_assert("vif:direct matches",
              vmaf_gpu_dispatch_parse_env("vif:direct", "vif", names, &idx) == 1);
    mu_assert("direct maps to index 0", idx == 0);
    return NULL;
}

static char *test_parse_multi_token_and_whitespace(void)
{
    int idx = -1;
    const char *const names[] = {"direct", "graph", NULL};
    /* Whitespace between tokens, second token matches. */
    mu_assert("multi-token second matches",
              vmaf_gpu_dispatch_parse_env(" adm:direct , vif:graph ", "vif", names, &idx) == 1);
    mu_assert("second token decoded", idx == 1);

    /* No match — feature not in the list. */
    mu_assert("absent feature declines",
              vmaf_gpu_dispatch_parse_env("adm:direct,vif:graph", "psnr", names, &idx) == 0);
    return NULL;
}

static char *test_parse_unknown_strategy_declines(void)
{
    int idx = 42;
    const char *const names[] = {"direct", "graph", NULL};
    /* Feature matches but strategy string is unknown — must decline
     * (not silently pick index 0). */
    mu_assert("unknown strategy declines",
              vmaf_gpu_dispatch_parse_env("vif:rocket", "vif", names, &idx) == 0);
    mu_assert("decline leaves out untouched", idx == 42);
    return NULL;
}

static char *test_parse_malformed_no_colon_declines(void)
{
    int idx = 7;
    const char *const names[] = {"direct", "graph", NULL};
    /* Missing colon — entire env malformed, parse declines without crash. */
    mu_assert("missing colon declines",
              vmaf_gpu_dispatch_parse_env("vif graph", "vif", names, &idx) == 0);
    mu_assert("empty string declines", vmaf_gpu_dispatch_parse_env("", "vif", names, &idx) == 0);
    mu_assert("decline leaves out untouched", idx == 7);
    return NULL;
}

/* ---- gpu_dispatch_env.c — thread-safe once-snapshot ---- */

static char *test_env_get_null_var_returns_null(void)
{
    /* NULL var_name → NULL return, no crash. Pinned by the contract
     * in gpu_dispatch_env.h. */
    mu_assert("NULL var_name → NULL", vmaf_gpu_dispatch_env_get(NULL) == NULL);
    return NULL;
}

static char *test_env_get_snapshots_first_call(void)
{
    /* Use a private, namespaced var-name so we never collide with the
     * real VMAF_*_DISPATCH variables (some of which may be present in
     * the test process env). The snapshot is a process-wide singleton,
     * so the FIRST call wins permanently — pre-set the env before the
     * first call. */
    const char *const var = "VMAFX_TEST_DISPATCH_RUNTIME_A";
    /* NOLINTNEXTLINE(concurrency-mt-unsafe) — single-thread test setup. */
    (void)setenv(var, "vif:graph,adm:direct", 1);

    const char *snap = vmaf_gpu_dispatch_env_get(var);
    mu_assert("snapshot is non-NULL when env is set", snap != NULL);
    mu_assert("snapshot value matches env", strcmp(snap, "vif:graph,adm:direct") == 0);

    /* Now mutate the env — the snapshot is immutable per contract. */
    /* NOLINTNEXTLINE(concurrency-mt-unsafe) — single-thread test setup. */
    (void)setenv(var, "psnr:direct", 1);
    const char *snap2 = vmaf_gpu_dispatch_env_get(var);
    mu_assert("snapshot is identity-stable on repeat get", snap2 == snap);
    mu_assert("snapshot value did not observe later setenv",
              strcmp(snap2, "vif:graph,adm:direct") == 0);
    return NULL;
}

static char *test_env_get_distinct_keys_independent(void)
{
    const char *const var_b = "VMAFX_TEST_DISPATCH_RUNTIME_B";
    const char *const var_c = "VMAFX_TEST_DISPATCH_RUNTIME_C";
    /* NOLINTNEXTLINE(concurrency-mt-unsafe) — single-thread test setup. */
    (void)setenv(var_b, "alpha", 1);
    /* NOLINTNEXTLINE(concurrency-mt-unsafe) — single-thread test setup. */
    (void)unsetenv(var_c);

    const char *snap_b = vmaf_gpu_dispatch_env_get(var_b);
    const char *snap_c = vmaf_gpu_dispatch_env_get(var_c);

    mu_assert("set var snapshots to its value", snap_b != NULL);
    mu_assert("set var value preserved", strcmp(snap_b, "alpha") == 0);
    mu_assert("unset var snapshots to NULL", snap_c == NULL);
    return NULL;
}

/* ---- cuda/dispatch_strategy.c — host-only pure-function selector ---- */
/*
 * CUDA dispatch_strategy.c is built with pthread + stdlib only (no
 * CUDA SDK), so it links cleanly into a host test. The selector is
 * the *only* entry point a feature extractor sees; pinning its
 * defaults and env override matters because a regression silently
 * routes every feature to the wrong submission path.
 *
 * Caveat: vmaf_cuda_select_strategy snapshots VMAF_CUDA_DISPATCH on
 * the first call via pthread_once. Setting the env in this test
 * does NOT affect already-snapshotted state from a sibling test
 * binary, but within this binary the first call wins. We pre-set
 * the env BEFORE invoking the selector for the first time.
 */

static char *test_cuda_dispatch_default_is_direct(void)
{
    /* Pre-set BEFORE first call so the pthread_once snapshot picks
     * up our value. An empty value (or absent) yields DIRECT for
     * every feature per ADR-0181. */
    /* NOLINTNEXTLINE(concurrency-mt-unsafe) — single-thread test setup. */
    (void)setenv("VMAF_CUDA_DISPATCH", "vif:graph,adm:direct", 1);

    const VmafCudaDispatchStrategy s_vif = vmaf_cuda_select_strategy("vif", NULL, 1920, 1080);
    mu_assert("env override routes vif → GRAPH_CAPTURE", s_vif == VMAF_CUDA_DISPATCH_GRAPH_CAPTURE);

    const VmafCudaDispatchStrategy s_adm = vmaf_cuda_select_strategy("adm", NULL, 1920, 1080);
    mu_assert("env override routes adm → DIRECT", s_adm == VMAF_CUDA_DISPATCH_DIRECT);

    /* Feature not mentioned in the env falls back to DIRECT (the
     * library-wide default for CUDA — graph-capture is opt-in). */
    const VmafCudaDispatchStrategy s_psnr = vmaf_cuda_select_strategy("psnr", NULL, 1920, 1080);
    mu_assert("unmentioned feature defaults to DIRECT", s_psnr == VMAF_CUDA_DISPATCH_DIRECT);
    return NULL;
}

static char *test_cuda_dispatch_null_feature_defaults_direct(void)
{
    /* NULL feature_name short-circuits the env parser — DIRECT
     * fallback applies. Pins the input-validation contract.
     * (Snapshot has already occurred via the test above; calling
     * again is safe — the once-init is no-op after first call.) */
    const VmafCudaDispatchStrategy s = vmaf_cuda_select_strategy(NULL, NULL, 1920, 1080);
    mu_assert("NULL feature falls back to DIRECT", s == VMAF_CUDA_DISPATCH_DIRECT);
    return NULL;
}

/* ---- hip/dispatch_strategy.c — stub selector ---- */
/*
 * The HIP dispatch_strategy stub returns 0 unconditionally pending
 * the kernel-routing table (ADR-0212 §"What lands next"). This pin
 * makes any future "accidentally support a feature before the
 * routing table lands" regression caught at test time, not at the
 * vmaf --backend hip CLI exit path. Mirrors libvmaf/test/
 * test_metal_smoke.c's vmaf_metal_dispatch_supports pin.
 */

static char *test_hip_dispatch_supports_null_and_unknown(void)
{
    /* NULL ctx + NULL feature + unknown feature all return 0
     * (unsupported). The stub never crashes regardless of input. */
    mu_assert("NULL ctx + NULL feature → 0", vmaf_hip_dispatch_supports(NULL, NULL) == 0);
    mu_assert("NULL ctx + named feature → 0", vmaf_hip_dispatch_supports(NULL, "psnr_hip") == 0);
    mu_assert("NULL ctx + unknown feature → 0",
              vmaf_hip_dispatch_supports(NULL, "definitely_not_hip") == 0);
    return NULL;
}

/* Function-pointer table keeps run_tests below the
 * readability-function-size budget (mirrors the test_hip_smoke.c
 * pattern for the same reason). */
typedef char *(*test_fn)(void);

static const test_fn test_table[] = {
    /* gpu_dispatch_parse.h */
    test_parse_null_inputs_decline,
    test_parse_matches_strategy_by_name,
    test_parse_multi_token_and_whitespace,
    test_parse_unknown_strategy_declines,
    test_parse_malformed_no_colon_declines,
    /* gpu_dispatch_env.c */
    test_env_get_null_var_returns_null,
    test_env_get_snapshots_first_call,
    test_env_get_distinct_keys_independent,
    /* cuda/dispatch_strategy.c */
    test_cuda_dispatch_default_is_direct,
    test_cuda_dispatch_null_feature_defaults_direct,
    /* hip/dispatch_strategy.c */
    test_hip_dispatch_supports_null_and_unknown,
};

static const size_t test_table_len = sizeof(test_table) / sizeof(test_table[0]);

char *run_tests(void)
{
    for (size_t i = 0; i < test_table_len; ++i) {
        mu_run_test(test_table[i]);
    }
    return NULL;
}
