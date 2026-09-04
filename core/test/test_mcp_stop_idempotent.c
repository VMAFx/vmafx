/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Regression test for the `vmaf_mcp_stop()` double-join SIGSEGV
 *  flagged in PR #460 (audit follow-up #5).
 *
 *  Background:
 *      Prior to the fix in this PR, `vmaf_mcp_stop()` used an
 *      unconditional `atomic_exchange(running, 2)` on each of the
 *      three transport state atomics (stdio / uds / sse). The
 *      branch that joins the worker thread fired whenever the
 *      observed previous value was 1 OR 2 — but the unconditional
 *      exchange also mutated a never-started transport from
 *      0 -> 2 on the first call. The second call then observed
 *      prev == 2 and re-entered the join branch, calling
 *      pthread_join() on a default-initialised pthread_t (UB),
 *      or — once a thread had actually been started and joined —
 *      pthread_join() on an already-joined thread handle (also UB,
 *      observed as SIGSEGV on glibc).
 *
 *  Repro (without the fix): three consecutive `vmaf_mcp_stop()`
 *  invocations on a server with any transport started crashes on
 *  the third call. With the fix (compare-exchange that only
 *  transitions 1 -> 2), every invocation past the first is a no-op
 *  and the test exits cleanly.
 *
 *  References:
 *      - PR #460 audit `Known follow-ups` item 5
 *      - core/src/mcp/mcp.c::vmaf_mcp_stop
 *      - ADR-0209 (embedded MCP scaffold) — public lifecycle contract
 */

#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "test.h"

#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_mcp.h"

/* Three consecutive stop() invocations on a server with the stdio
 * transport started must not SIGSEGV. The first joins the worker;
 * the next two are no-ops. */
static char *test_stop_thrice_with_stdio(void)
{
    VmafContext *ctx = NULL;
    VmafConfiguration vcfg = {0};
    vcfg.log_level = VMAF_LOG_LEVEL_NONE;
    vcfg.n_threads = 1u;
    mu_assert("vmaf_init", vmaf_init(&ctx, vcfg) == 0);

    VmafMcpServer *server = NULL;
    mu_assert("mcp init", vmaf_mcp_init(&server, ctx, NULL) == 0);

    /* A pipe pair feeds the stdio worker. Closing the read end's
     * write side after start_stdio drives the worker to EOF so the
     * subsequent join in stop() returns promptly. */
    int req_pipe[2] = {-1, -1};
    int resp_pipe[2] = {-1, -1};
    mu_assert("req pipe", pipe(req_pipe) == 0);
    mu_assert("resp pipe", pipe(resp_pipe) == 0);

    VmafMcpStdioConfig scfg = {.fd_in = req_pipe[0], .fd_out = resp_pipe[1]};
    mu_assert("start stdio", vmaf_mcp_start_stdio(server, &scfg) == 0);

    /* Close the request-write fd so the worker sees EOF and exits
     * the read loop; otherwise the first stop() would block in
     * pthread_join() forever. */
    (void)close(req_pipe[1]);
    req_pipe[1] = -1;

    /* The bug: prior code would SIGSEGV on the third invocation
     * because the second exchange mutated 0 -> 2 and the third
     * re-entered the join branch. */
    int rc1 = vmaf_mcp_stop(server);
    mu_assert("stop #1 returns 0", rc1 == 0);
    int rc2 = vmaf_mcp_stop(server);
    mu_assert("stop #2 returns 0", rc2 == 0);
    int rc3 = vmaf_mcp_stop(server);
    mu_assert("stop #3 returns 0", rc3 == 0);

    /* close() invokes stop() internally — must also be safe after
     * three prior explicit stops. */
    vmaf_mcp_close(&server);
    mu_assert("close NULLs handle", server == NULL);

    if (req_pipe[0] >= 0)
        (void)close(req_pipe[0]);
    if (resp_pipe[0] >= 0)
        (void)close(resp_pipe[0]);
    if (resp_pipe[1] >= 0)
        (void)close(resp_pipe[1]);
    (void)vmaf_close(ctx);
    return NULL;
}

/* Same shape but on a freshly-initialised server with NO transport
 * started. Every stop() call observes prev==0 on each atomic and
 * must therefore skip every join branch. Without the fix, the first
 * stop() flips 0 -> 2 unconditionally and the second crashes on
 * pthread_join() of a default-initialised pthread_t. */
static char *test_stop_thrice_without_start(void)
{
    VmafContext *ctx = NULL;
    VmafConfiguration vcfg = {0};
    vcfg.log_level = VMAF_LOG_LEVEL_NONE;
    vcfg.n_threads = 1u;
    mu_assert("vmaf_init", vmaf_init(&ctx, vcfg) == 0);

    VmafMcpServer *server = NULL;
    mu_assert("mcp init", vmaf_mcp_init(&server, ctx, NULL) == 0);

    int rc1 = vmaf_mcp_stop(server);
    mu_assert("stop #1 (no start) returns 0", rc1 == 0);
    int rc2 = vmaf_mcp_stop(server);
    mu_assert("stop #2 (no start) returns 0", rc2 == 0);
    int rc3 = vmaf_mcp_stop(server);
    mu_assert("stop #3 (no start) returns 0", rc3 == 0);

    vmaf_mcp_close(&server);
    mu_assert("close NULLs handle", server == NULL);

    (void)vmaf_close(ctx);
    return NULL;
}

static char *test_stop_thrice_with_uds(void)
{
    if (!vmaf_mcp_transport_available(VMAF_MCP_TRANSPORT_UDS))
        return NULL;

    VmafContext *ctx = NULL;
    VmafConfiguration vcfg = {0};
    vcfg.log_level = VMAF_LOG_LEVEL_NONE;
    vcfg.n_threads = 1u;
    mu_assert("vmaf_init", vmaf_init(&ctx, vcfg) == 0);

    VmafMcpServer *server = NULL;
    mu_assert("mcp init", vmaf_mcp_init(&server, ctx, NULL) == 0);

    char path[80];
    int n = snprintf(path, sizeof(path), "/tmp/vmaf-mcp-stop-uds-%d.sock", (int)getpid());
    mu_assert("path snprintf", n > 0 && (size_t)n < sizeof(path));

    VmafMcpUdsConfig ucfg = {.path = path};
    mu_assert("start uds", vmaf_mcp_start_uds(server, &ucfg) == 0);

    int rc1 = vmaf_mcp_stop(server);
    mu_assert("stop #1 returns 0", rc1 == 0);
    int rc2 = vmaf_mcp_stop(server);
    mu_assert("stop #2 returns 0", rc2 == 0);
    int rc3 = vmaf_mcp_stop(server);
    mu_assert("stop #3 returns 0", rc3 == 0);

    vmaf_mcp_close(&server);
    mu_assert("close NULLs handle", server == NULL);

    (void)vmaf_close(ctx);
    return NULL;
}

typedef char *(*test_fn)(void);

static const test_fn k_test_table[] = {
    test_stop_thrice_without_start,
    test_stop_thrice_with_stdio,
    test_stop_thrice_with_uds,
};

static const size_t k_test_table_len = sizeof(k_test_table) / sizeof(k_test_table[0]);

char *run_tests(void)
{
    for (size_t i = 0u; i < k_test_table_len; ++i) {
        mu_run_test(k_test_table[i]);
    }
    return NULL;
}
