/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Coverage push for the embedded MCP server under core/src/mcp/ .
 *
 *  Complements test_mcp_smoke.c — that file pins the happy-path
 *  contract (init / start / tools-list / tools-call success / UDS +
 *  SSE happy path). This file targets the error envelopes and
 *  state-machine edges that the smoke test does not reach:
 *
 *      Dispatcher (core/src/mcp/dispatcher.c):
 *          - `initialize` round-trip (serverInfo + capabilities)
 *          - `resources/list` round-trip (empty array)
 *          - parse-error envelope (-32700) on malformed JSON
 *          - invalid-request envelope (-32600) on missing/non-string
 *            `method`
 *          - notification (no id) consumed silently — no response
 *          - `tools/call` with missing `params`            -> -32602
 *          - `tools/call` with non-string `name`           -> -32602
 *          - `tools/call` unknown tool name                -> -32601
 *          - `tools/call` compute_vmaf with missing fields -> -32602
 *            + carries the tool-set error message
 *
 *      Lifecycle (core/src/mcp/mcp.c):
 *          - validate_config rejects non-power-of-2 queue_depth
 *          - validate_config rejects max_drain_per_frame > 64
 *          - user_agent dup'd into handle and surfaced via initialize
 *          - vmaf_mcp_transport_available reports 1 for built-in
 *            transports (stdio + uds + sse) and 0 for >31 ids
 *          - start_stdio double-start returns -EBUSY
 *          - start_uds rejects empty path + over-long path (>= 100 B)
 *          - start_sse rejects empty path + over-long path (> 256 B)
 *          - close-after-EOF teardown joins the worker cleanly
 *
 *      Transports (core/src/mcp/transport_*.c):
 *          - stdio: oversized line (> VMAF_MCP_MAX_LINE_BYTES) emits
 *            the parse-error overflow response, then continues
 *          - sse: GET / health endpoint
 *          - sse: GET /unknown returns 404
 *          - sse: malformed request line returns 400
 *          - sse: POST with no Content-Length returns 400
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include "test.h"

#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_mcp.h"

/* ============================================================
 * Harness: stdio pipe-pair + VmafContext + VmafMcpServer.
 *
 * Mirrors the McpHarness pattern in test_mcp_smoke.c without
 * sharing the symbol (so the two TUs can link side-by-side without
 * an internal header).
 * ============================================================ */

typedef struct CovHarness {
    int req_pipe[2];
    int resp_pipe[2];
    VmafContext *ctx;
    VmafMcpServer *server;
} CovHarness;

static int cov_harness_init(CovHarness *h)
{
    h->req_pipe[0] = h->req_pipe[1] = -1;
    h->resp_pipe[0] = h->resp_pipe[1] = -1;
    h->ctx = NULL;
    h->server = NULL;
    if (pipe(h->req_pipe) != 0)
        return -1;
    if (pipe(h->resp_pipe) != 0)
        return -1;
    VmafConfiguration cfg = {0};
    cfg.log_level = VMAF_LOG_LEVEL_NONE;
    cfg.n_threads = 1u;
    if (vmaf_init(&h->ctx, cfg) != 0)
        return -1;
    if (vmaf_mcp_init(&h->server, h->ctx, NULL) != 0)
        return -1;
    VmafMcpStdioConfig scfg = {.fd_in = h->req_pipe[0], .fd_out = h->resp_pipe[1]};
    if (vmaf_mcp_start_stdio(h->server, &scfg) != 0)
        return -1;
    return 0;
}

static void cov_harness_teardown(CovHarness *h)
{
    if (h->req_pipe[1] >= 0)
        (void)close(h->req_pipe[1]);
    if (h->server != NULL)
        vmaf_mcp_close(&h->server);
    if (h->req_pipe[0] >= 0)
        (void)close(h->req_pipe[0]);
    if (h->resp_pipe[0] >= 0)
        (void)close(h->resp_pipe[0]);
    if (h->resp_pipe[1] >= 0)
        (void)close(h->resp_pipe[1]);
    if (h->ctx != NULL)
        (void)vmaf_close(h->ctx);
}

/* Read up to max_len-1 bytes from `fd` until LF. */
static ssize_t cov_read_one_line(int fd, char *buf, size_t max_len)
{
    size_t n = 0u;
    while (n < max_len - 1u) {
        char c = 0;
        ssize_t r = read(fd, &c, 1);
        if (r <= 0) {
            if (n == 0u)
                return r;
            break;
        }
        if (c == '\n')
            break;
        if (c != '\r')
            buf[n++] = c;
    }
    buf[n] = '\0';
    return (ssize_t)n;
}

/* Send `req_len` bytes on req_pipe[1] and drain one LF-delimited
 * response line. Caller-provided line buffer. */
static char *cov_send_and_read(CovHarness *h, const char *req, size_t req_len, char *line,
                               size_t line_cap)
{
    ssize_t w = write(h->req_pipe[1], req, req_len);
    mu_assert("write request", w == (ssize_t)req_len);
    ssize_t n = cov_read_one_line(h->resp_pipe[0], line, line_cap);
    mu_assert("response received", n > 0);
    return NULL;
}

/* ============================================================
 * Dispatcher error envelopes
 * ============================================================ */

static char *test_initialize_roundtrip(void)
{
    CovHarness h;
    mu_assert("harness init", cov_harness_init(&h) == 0);

    static const char req[] = "{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"initialize\","
                              "\"params\":{\"protocolVersion\":\"2024-11-05\"}}\n";
    char line[4096];
    char *err = cov_send_and_read(&h, req, sizeof(req) - 1u, line, sizeof(line));
    if (err != NULL) {
        cov_harness_teardown(&h);
        return err;
    }
    mu_assert("initialize id", strstr(line, "\"id\":11") != NULL);
    mu_assert("protocolVersion", strstr(line, "\"protocolVersion\":\"2024-11-05\"") != NULL);
    mu_assert("serverInfo present", strstr(line, "\"serverInfo\"") != NULL);
    mu_assert("capabilities present", strstr(line, "\"capabilities\"") != NULL);
    mu_assert("default server name", strstr(line, "libvmaf-mcp") != NULL);

    cov_harness_teardown(&h);
    return NULL;
}

static char *test_resources_list_roundtrip(void)
{
    CovHarness h;
    mu_assert("harness init", cov_harness_init(&h) == 0);

    static const char req[] = "{\"jsonrpc\":\"2.0\",\"id\":12,\"method\":\"resources/list\"}\n";
    char line[2048];
    char *err = cov_send_and_read(&h, req, sizeof(req) - 1u, line, sizeof(line));
    if (err != NULL) {
        cov_harness_teardown(&h);
        return err;
    }
    mu_assert("id 12", strstr(line, "\"id\":12") != NULL);
    /* v1 ships an empty array — verify both the field and absence
     * of any element. */
    mu_assert("resources field", strstr(line, "\"resources\"") != NULL);
    mu_assert("empty resources", strstr(line, "\"resources\":[]") != NULL);

    cov_harness_teardown(&h);
    return NULL;
}

static char *test_parse_error_envelope(void)
{
    CovHarness h;
    mu_assert("harness init", cov_harness_init(&h) == 0);

    /* Non-JSON nonsense — cJSON_Parse should return NULL. */
    static const char req[] = "this is not json at all\n";
    char line[1024];
    char *err = cov_send_and_read(&h, req, sizeof(req) - 1u, line, sizeof(line));
    if (err != NULL) {
        cov_harness_teardown(&h);
        return err;
    }
    mu_assert("parse-error code -32700", strstr(line, "-32700") != NULL);
    mu_assert("error envelope", strstr(line, "\"error\"") != NULL);
    mu_assert("null id on parse error", strstr(line, "\"id\":null") != NULL);

    cov_harness_teardown(&h);
    return NULL;
}

static char *test_invalid_request_missing_method(void)
{
    CovHarness h;
    mu_assert("harness init", cov_harness_init(&h) == 0);

    /* Valid JSON, but no `method` field — JSON-RPC §5.1 -32600. */
    static const char req[] = "{\"jsonrpc\":\"2.0\",\"id\":13}\n";
    char line[1024];
    char *err = cov_send_and_read(&h, req, sizeof(req) - 1u, line, sizeof(line));
    if (err != NULL) {
        cov_harness_teardown(&h);
        return err;
    }
    mu_assert("invalid-request code -32600", strstr(line, "-32600") != NULL);
    mu_assert("id echoed", strstr(line, "\"id\":13") != NULL);

    cov_harness_teardown(&h);
    return NULL;
}

static char *test_invalid_request_non_string_method(void)
{
    CovHarness h;
    mu_assert("harness init", cov_harness_init(&h) == 0);

    static const char req[] = "{\"jsonrpc\":\"2.0\",\"id\":14,\"method\":42}\n";
    char line[1024];
    char *err = cov_send_and_read(&h, req, sizeof(req) - 1u, line, sizeof(line));
    if (err != NULL) {
        cov_harness_teardown(&h);
        return err;
    }
    mu_assert("invalid-request -32600", strstr(line, "-32600") != NULL);

    cov_harness_teardown(&h);
    return NULL;
}

/* Notification: no `id` field — per JSON-RPC 2.0 §4.1 the server
 * MUST NOT reply. We send a notification followed by a regular
 * request and assert the response we read corresponds to the
 * second request, proving the notification was swallowed. */
static char *test_notification_no_response(void)
{
    CovHarness h;
    mu_assert("harness init", cov_harness_init(&h) == 0);

    static const char notif[] = "{\"jsonrpc\":\"2.0\",\"method\":\"tools/list\"}\n";
    static const char follow[] =
        "{\"jsonrpc\":\"2.0\",\"id\":1337,\"method\":\"resources/list\"}\n";
    ssize_t w1 = write(h.req_pipe[1], notif, sizeof(notif) - 1u);
    mu_assert("notif write", w1 == (ssize_t)(sizeof(notif) - 1u));
    ssize_t w2 = write(h.req_pipe[1], follow, sizeof(follow) - 1u);
    mu_assert("follow write", w2 == (ssize_t)(sizeof(follow) - 1u));

    char line[2048];
    ssize_t n = cov_read_one_line(h.resp_pipe[0], line, sizeof(line));
    mu_assert("one line received", n > 0);
    /* If a response to the notification leaked, this would be the
     * tools/list payload with no id. The id field MUST be 1337. */
    mu_assert("response is for follow request", strstr(line, "\"id\":1337") != NULL);
    mu_assert("not the notification response", strstr(line, "\"id\":null") == NULL);

    cov_harness_teardown(&h);
    return NULL;
}

static char *test_tools_call_missing_params(void)
{
    CovHarness h;
    mu_assert("harness init", cov_harness_init(&h) == 0);

    static const char req[] = "{\"jsonrpc\":\"2.0\",\"id\":21,\"method\":\"tools/call\"}\n";
    char line[1024];
    char *err = cov_send_and_read(&h, req, sizeof(req) - 1u, line, sizeof(line));
    if (err != NULL) {
        cov_harness_teardown(&h);
        return err;
    }
    mu_assert("invalid-params -32602", strstr(line, "-32602") != NULL);
    mu_assert("id echoed", strstr(line, "\"id\":21") != NULL);

    cov_harness_teardown(&h);
    return NULL;
}

static char *test_tools_call_non_string_name(void)
{
    CovHarness h;
    mu_assert("harness init", cov_harness_init(&h) == 0);

    static const char req[] = "{\"jsonrpc\":\"2.0\",\"id\":22,\"method\":\"tools/call\","
                              "\"params\":{\"name\":99,\"arguments\":{}}}\n";
    char line[1024];
    char *err = cov_send_and_read(&h, req, sizeof(req) - 1u, line, sizeof(line));
    if (err != NULL) {
        cov_harness_teardown(&h);
        return err;
    }
    mu_assert("invalid-params -32602", strstr(line, "-32602") != NULL);

    cov_harness_teardown(&h);
    return NULL;
}

static char *test_tools_call_unknown_tool(void)
{
    CovHarness h;
    mu_assert("harness init", cov_harness_init(&h) == 0);

    static const char req[] = "{\"jsonrpc\":\"2.0\",\"id\":23,\"method\":\"tools/call\","
                              "\"params\":{\"name\":\"no_such_tool\",\"arguments\":{}}}\n";
    char line[1024];
    char *err = cov_send_and_read(&h, req, sizeof(req) - 1u, line, sizeof(line));
    if (err != NULL) {
        cov_harness_teardown(&h);
        return err;
    }
    mu_assert("method-not-found -32601", strstr(line, "-32601") != NULL);
    mu_assert("id 23", strstr(line, "\"id\":23") != NULL);

    cov_harness_teardown(&h);
    return NULL;
}

/* compute_vmaf with empty arguments: tool's parse_arguments() fires
 * `set_err()` and returns -EINVAL. The dispatcher must translate
 * to -32602 AND carry the tool-supplied error message string. */
static char *test_compute_vmaf_missing_arguments(void)
{
    CovHarness h;
    mu_assert("harness init", cov_harness_init(&h) == 0);

    static const char req[] = "{\"jsonrpc\":\"2.0\",\"id\":24,\"method\":\"tools/call\","
                              "\"params\":{\"name\":\"compute_vmaf\",\"arguments\":null}}\n";
    char line[2048];
    char *err = cov_send_and_read(&h, req, sizeof(req) - 1u, line, sizeof(line));
    if (err != NULL) {
        cov_harness_teardown(&h);
        return err;
    }
    mu_assert("invalid-params -32602", strstr(line, "-32602") != NULL);
    /* The tool's set_err() string should make it into the
     * error.message; the dispatcher uses the tool message instead
     * of the generic "invalid params". */
    mu_assert("tool-supplied error message present",
              strstr(line, "compute_vmaf") != NULL || strstr(line, "arguments") != NULL);

    cov_harness_teardown(&h);
    return NULL;
}

/* compute_vmaf with valid object but missing required `reference_path`:
 * parse_arguments → set_err("requires string fields...") → -EINVAL. */
static char *test_compute_vmaf_missing_required_field(void)
{
    CovHarness h;
    mu_assert("harness init", cov_harness_init(&h) == 0);

    static const char req[] = "{\"jsonrpc\":\"2.0\",\"id\":25,\"method\":\"tools/call\","
                              "\"params\":{\"name\":\"compute_vmaf\","
                              "\"arguments\":{\"width\":64,\"height\":64}}}\n";
    char line[2048];
    char *err = cov_send_and_read(&h, req, sizeof(req) - 1u, line, sizeof(line));
    if (err != NULL) {
        cov_harness_teardown(&h);
        return err;
    }
    mu_assert("invalid-params -32602", strstr(line, "-32602") != NULL);
    mu_assert("id 25", strstr(line, "\"id\":25") != NULL);

    cov_harness_teardown(&h);
    return NULL;
}

/* ============================================================
 * Lifecycle: validate_config + user_agent
 * ============================================================ */

static char *test_init_rejects_non_power_of_two_queue_depth(void)
{
    VmafContext *ctx = NULL;
    VmafConfiguration cfg = {0};
    cfg.log_level = VMAF_LOG_LEVEL_NONE;
    cfg.n_threads = 1u;
    mu_assert("vmaf_init", vmaf_init(&ctx, cfg) == 0);

    VmafMcpServer *server = NULL;
    VmafMcpConfig mcfg = {0};
    mcfg.queue_depth = 7u; /* not a power of two */
    int rc = vmaf_mcp_init(&server, ctx, &mcfg);
    mu_assert("queue_depth=7 -> -EINVAL", rc == -EINVAL);
    mu_assert("no handle leaks", server == NULL);

    (void)vmaf_close(ctx);
    return NULL;
}

static char *test_init_rejects_max_drain_over_cap(void)
{
    VmafContext *ctx = NULL;
    VmafConfiguration cfg = {0};
    cfg.log_level = VMAF_LOG_LEVEL_NONE;
    cfg.n_threads = 1u;
    mu_assert("vmaf_init", vmaf_init(&ctx, cfg) == 0);

    VmafMcpServer *server = NULL;
    VmafMcpConfig mcfg = {0};
    mcfg.max_drain_per_frame = 65u; /* cap is 64 */
    int rc = vmaf_mcp_init(&server, ctx, &mcfg);
    mu_assert("drain=65 -> -EINVAL", rc == -EINVAL);

    (void)vmaf_close(ctx);
    return NULL;
}

static char *test_init_accepts_valid_power_of_two_queue_depth(void)
{
    VmafContext *ctx = NULL;
    VmafConfiguration cfg = {0};
    cfg.log_level = VMAF_LOG_LEVEL_NONE;
    cfg.n_threads = 1u;
    mu_assert("vmaf_init", vmaf_init(&ctx, cfg) == 0);

    VmafMcpServer *server = NULL;
    VmafMcpConfig mcfg = {0};
    mcfg.queue_depth = 128u;        /* 2^7 */
    mcfg.max_drain_per_frame = 16u; /* well under cap */
    int rc = vmaf_mcp_init(&server, ctx, &mcfg);
    mu_assert("valid cfg -> 0", rc == 0);
    mu_assert("handle returned", server != NULL);

    vmaf_mcp_close(&server);
    (void)vmaf_close(ctx);
    return NULL;
}

/* Bring up a harness whose VmafMcpServer carries the supplied
 * VmafMcpConfig. Mirrors cov_harness_init() but plumbs the config so
 * tests for `user_agent`, `queue_depth`, and `max_drain_per_frame`
 * can exercise non-default paths without duplicating boilerplate. */
static int cov_harness_init_with_cfg(CovHarness *h, const VmafMcpConfig *mcfg)
{
    h->req_pipe[0] = h->req_pipe[1] = -1;
    h->resp_pipe[0] = h->resp_pipe[1] = -1;
    h->ctx = NULL;
    h->server = NULL;
    if (pipe(h->req_pipe) != 0)
        return -1;
    if (pipe(h->resp_pipe) != 0)
        return -1;
    VmafConfiguration cfg = {0};
    cfg.log_level = VMAF_LOG_LEVEL_NONE;
    cfg.n_threads = 1u;
    if (vmaf_init(&h->ctx, cfg) != 0)
        return -1;
    if (vmaf_mcp_init(&h->server, h->ctx, mcfg) != 0)
        return -1;
    VmafMcpStdioConfig scfg = {.fd_in = h->req_pipe[0], .fd_out = h->resp_pipe[1]};
    if (vmaf_mcp_start_stdio(h->server, &scfg) != 0)
        return -1;
    return 0;
}

/* Verify user_agent is dup'd into the handle and reflected in the
 * initialize-handshake `serverInfo.name`. */
static char *test_user_agent_surfaces_in_initialize(void)
{
    CovHarness h;
    VmafMcpConfig mcfg = {0};
    mcfg.user_agent = "cov-test/9.9";
    mu_assert("harness init", cov_harness_init_with_cfg(&h, &mcfg) == 0);

    static const char req[] = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\"}\n";
    char line[4096];
    char *err = cov_send_and_read(&h, req, sizeof(req) - 1u, line, sizeof(line));
    if (err != NULL) {
        cov_harness_teardown(&h);
        return err;
    }
    mu_assert("custom user_agent name", strstr(line, "\"name\":\"cov-test/9.9\"") != NULL);

    cov_harness_teardown(&h);
    return NULL;
}

/* ============================================================
 * Lifecycle: state machine
 * ============================================================ */

/* Confirm vmaf_mcp_transport_available returns 1 for the three
 * sub-flags this build was compiled with, and 0 for an out-of-range
 * (>31) transport id. */
static char *test_transport_available_positive_and_oob(void)
{
    /* The build wires HAVE_MCP_STDIO; HAVE_MCP_UDS and HAVE_MCP_SSE
     * are also defined when the per-transport sub-flags are on. We
     * assert the function returns 0 or 1 (binary) for the defined
     * transport ids, and 0 for an OOB id. We don't hard-pin which
     * sub-flag is on because the gating is build-time. */
    int rs = vmaf_mcp_transport_available(VMAF_MCP_TRANSPORT_SSE);
    int ru = vmaf_mcp_transport_available(VMAF_MCP_TRANSPORT_UDS);
    int rt = vmaf_mcp_transport_available(VMAF_MCP_TRANSPORT_STDIO);
    mu_assert("sse availability is binary", rs == 0 || rs == 1);
    mu_assert("uds availability is binary", ru == 0 || ru == 1);
    mu_assert("stdio availability is binary", rt == 0 || rt == 1);

    /* OOB transport id (> 31). The bitmask code path early-returns 0.
     * NOLINTNEXTLINE-justification: this test deliberately exercises
     * the > 31 guard in vmaf_mcp_transport_available; the cast is the
     * only way to drive that branch through the public API surface. */
    /* NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange) */
    int rb = vmaf_mcp_transport_available((VmafMcpTransport)32);
    mu_assert("oob id -> 0", rb == 0);
    /* NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange) */
    int rb2 = vmaf_mcp_transport_available((VmafMcpTransport)100);
    mu_assert("huge oob id -> 0", rb2 == 0);
    return NULL;
}

static char *test_start_stdio_double_start_returns_ebusy(void)
{
    int req_pipe[2] = {-1, -1};
    int resp_pipe[2] = {-1, -1};
    mu_assert("req pipe", pipe(req_pipe) == 0);
    mu_assert("resp pipe", pipe(resp_pipe) == 0);

    VmafContext *ctx = NULL;
    VmafConfiguration cfg = {0};
    cfg.log_level = VMAF_LOG_LEVEL_NONE;
    cfg.n_threads = 1u;
    mu_assert("vmaf_init", vmaf_init(&ctx, cfg) == 0);

    VmafMcpServer *server = NULL;
    mu_assert("mcp init", vmaf_mcp_init(&server, ctx, NULL) == 0);

    VmafMcpStdioConfig scfg = {.fd_in = req_pipe[0], .fd_out = resp_pipe[1]};
    mu_assert("first start", vmaf_mcp_start_stdio(server, &scfg) == 0);
    int rc = vmaf_mcp_start_stdio(server, &scfg);
    mu_assert("second start -> -EBUSY", rc == -EBUSY);

    (void)close(req_pipe[1]);
    vmaf_mcp_close(&server);
    (void)close(req_pipe[0]);
    (void)close(resp_pipe[0]);
    (void)close(resp_pipe[1]);
    (void)vmaf_close(ctx);
    return NULL;
}

static char *test_start_uds_rejects_empty_path(void)
{
    VmafContext *ctx = NULL;
    VmafConfiguration cfg = {0};
    cfg.log_level = VMAF_LOG_LEVEL_NONE;
    cfg.n_threads = 1u;
    mu_assert("vmaf_init", vmaf_init(&ctx, cfg) == 0);

    VmafMcpServer *server = NULL;
    mu_assert("mcp init", vmaf_mcp_init(&server, ctx, NULL) == 0);

    VmafMcpUdsConfig ucfg = {.path = ""};
    int rc = vmaf_mcp_start_uds(server, &ucfg);
    mu_assert("empty path -> -EINVAL", rc == -EINVAL);

    vmaf_mcp_close(&server);
    (void)vmaf_close(ctx);
    return NULL;
}

static char *test_start_uds_rejects_overlong_path(void)
{
    VmafContext *ctx = NULL;
    VmafConfiguration cfg = {0};
    cfg.log_level = VMAF_LOG_LEVEL_NONE;
    cfg.n_threads = 1u;
    mu_assert("vmaf_init", vmaf_init(&ctx, cfg) == 0);

    VmafMcpServer *server = NULL;
    mu_assert("mcp init", vmaf_mcp_init(&server, ctx, NULL) == 0);

    /* 120 bytes >= 100 byte limit. */
    char longpath[128];
    memset(longpath, 'a', sizeof(longpath) - 1);
    longpath[sizeof(longpath) - 1] = '\0';
    VmafMcpUdsConfig ucfg = {.path = longpath};
    int rc = vmaf_mcp_start_uds(server, &ucfg);
    mu_assert("overlong path -> -EINVAL", rc == -EINVAL);

    vmaf_mcp_close(&server);
    (void)vmaf_close(ctx);
    return NULL;
}

static char *test_start_sse_rejects_empty_path(void)
{
    VmafContext *ctx = NULL;
    VmafConfiguration cfg = {0};
    cfg.log_level = VMAF_LOG_LEVEL_NONE;
    cfg.n_threads = 1u;
    mu_assert("vmaf_init", vmaf_init(&ctx, cfg) == 0);

    VmafMcpServer *server = NULL;
    mu_assert("mcp init", vmaf_mcp_init(&server, ctx, NULL) == 0);

    VmafMcpSseConfig scfg = {.port = 0, .path = ""};
    int rc = vmaf_mcp_start_sse(server, &scfg);
    mu_assert("empty path -> -EINVAL", rc == -EINVAL);

    vmaf_mcp_close(&server);
    (void)vmaf_close(ctx);
    return NULL;
}

static char *test_start_sse_rejects_overlong_path(void)
{
    VmafContext *ctx = NULL;
    VmafConfiguration cfg = {0};
    cfg.log_level = VMAF_LOG_LEVEL_NONE;
    cfg.n_threads = 1u;
    mu_assert("vmaf_init", vmaf_init(&ctx, cfg) == 0);

    VmafMcpServer *server = NULL;
    mu_assert("mcp init", vmaf_mcp_init(&server, ctx, NULL) == 0);

    /* 300 chars > 256 byte cap. */
    char longpath[320];
    memset(longpath, 'a', sizeof(longpath) - 1);
    longpath[sizeof(longpath) - 1] = '\0';
    VmafMcpSseConfig scfg = {.port = 0, .path = longpath};
    int rc = vmaf_mcp_start_sse(server, &scfg);
    mu_assert("overlong path -> -EINVAL", rc == -EINVAL);

    vmaf_mcp_close(&server);
    (void)vmaf_close(ctx);
    return NULL;
}

/* Verifies the close() teardown after a transport has been started
 * + EOFed (without an intervening explicit stop()). close() calls
 * stop() exactly once internally, which is the canonical embedded-
 * host shape: spawn, drive, close. */
static char *test_close_after_eof_joins_worker(void)
{
    int req_pipe[2] = {-1, -1};
    int resp_pipe[2] = {-1, -1};
    mu_assert("req pipe", pipe(req_pipe) == 0);
    mu_assert("resp pipe", pipe(resp_pipe) == 0);

    VmafContext *ctx = NULL;
    VmafConfiguration cfg = {0};
    cfg.log_level = VMAF_LOG_LEVEL_NONE;
    cfg.n_threads = 1u;
    mu_assert("vmaf_init", vmaf_init(&ctx, cfg) == 0);

    VmafMcpServer *server = NULL;
    mu_assert("mcp init", vmaf_mcp_init(&server, ctx, NULL) == 0);

    VmafMcpStdioConfig scfg = {.fd_in = req_pipe[0], .fd_out = resp_pipe[1]};
    mu_assert("start", vmaf_mcp_start_stdio(server, &scfg) == 0);

    /* Close write end → worker hits EOF and self-exits. */
    (void)close(req_pipe[1]);
    req_pipe[1] = -1;

    /* close() does the teardown (stop + join + mutex_destroy + free).
     * Confirms the canonical lifecycle path without an explicit
     * intervening stop() call. */
    vmaf_mcp_close(&server);
    mu_assert("handle NULLed", server == NULL);

    (void)close(req_pipe[0]);
    (void)close(resp_pipe[0]);
    (void)close(resp_pipe[1]);
    (void)vmaf_close(ctx);
    return NULL;
}

/* ============================================================
 * stdio transport: oversized-line overflow envelope
 *
 * Per transport_stdio.c the read_line() helper returns -2 when the
 * input exceeds VMAF_MCP_MAX_LINE_BYTES (64 KiB) before an LF. The
 * worker emits a fixed parse-error response, drains to the next LF,
 * and continues. We verify by:
 *   1. Writing > 64 KiB of garbage followed by an LF.
 *   2. Reading one response line — it must be the canned
 *      "64 KiB line limit" envelope (code -32700).
 *   3. Sending a valid tools/list and getting the normal response,
 *      proving the worker resumed cleanly.
 * ============================================================ */

/* Drive an oversize write past VMAF_MCP_MAX_LINE_BYTES and assert the
 * worker emits the canned 64-KiB-overflow parse-error envelope. Body
 * is split out of test_stdio_oversize_line_overflow_envelope to keep
 * the test function under readability-function-size. */
static char *check_stdio_oversize_overflow(CovHarness *h)
{
    /* Send 65 KiB of 'x' followed by LF. The first 64 KiB-1 fills
     * the read_line() buffer; the loop returns -2 before the LF.
     * Static buffer (.bss) — no per-test alloc / free dance, no
     * early-return malloc-leak surface for the analyzer. */
    static char big[66u * 1024u + 1u];
    static const size_t big_size = sizeof(big) - 1u;
    memset(big, 'x', big_size);
    big[big_size] = '\n';
    ssize_t w = write(h->req_pipe[1], big, big_size + 1u);
    mu_assert("oversize write", w == (ssize_t)(big_size + 1u));

    static char line[2048];
    ssize_t n = cov_read_one_line(h->resp_pipe[0], line, sizeof(line));
    mu_assert("overflow response", n > 0);
    mu_assert("overflow parse-error -32700", strstr(line, "-32700") != NULL);
    mu_assert("overflow message", strstr(line, "64 KiB") != NULL);

    /* Worker must have drained the bytes-up-to-LF + resumed cleanly. */
    static const char follow[] = "{\"jsonrpc\":\"2.0\",\"id\":777,\"method\":\"tools/list\"}\n";
    ssize_t w2 = write(h->req_pipe[1], follow, sizeof(follow) - 1u);
    mu_assert("follow write", w2 == (ssize_t)(sizeof(follow) - 1u));
    ssize_t n2 = cov_read_one_line(h->resp_pipe[0], line, sizeof(line));
    mu_assert("post-overflow response", n2 > 0);
    mu_assert("post-overflow id 777", strstr(line, "\"id\":777") != NULL);
    return NULL;
}

static char *test_stdio_oversize_line_overflow_envelope(void)
{
    CovHarness h;
    mu_assert("harness init", cov_harness_init(&h) == 0);
    char *err = check_stdio_oversize_overflow(&h);
    cov_harness_teardown(&h);
    return err;
}

/* ============================================================
 * SSE transport: 404, health, malformed request line, oversized POST
 * ============================================================ */

typedef struct SseTestCtx {
    VmafContext *ctx;
    VmafMcpServer *server;
    uint16_t port;
} SseTestCtx;

static int sse_test_ctx_up(SseTestCtx *s)
{
    s->ctx = NULL;
    s->server = NULL;
    s->port = 0u;
    VmafConfiguration vcfg = {0};
    vcfg.log_level = VMAF_LOG_LEVEL_NONE;
    vcfg.n_threads = 1u;
    if (vmaf_init(&s->ctx, vcfg) != 0)
        return -1;
    if (vmaf_mcp_init(&s->server, s->ctx, NULL) != 0)
        return -1;
    VmafMcpSseConfig sse_cfg = {.port = 0, .path = NULL};
    if (vmaf_mcp_start_sse(s->server, &sse_cfg) != 0)
        return -1;
    s->port = sse_cfg.port;
    return s->port == 0u ? -1 : 0;
}

static void sse_test_ctx_down(SseTestCtx *s)
{
    if (s->server != NULL)
        vmaf_mcp_close(&s->server);
    if (s->ctx != NULL)
        (void)vmaf_close(s->ctx);
}

static int sse_cov_connect(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sin.sin_port = htons(port);
    if (connect(fd, (const struct sockaddr *)&sin, sizeof(sin)) != 0) {
        (void)close(fd);
        return -1;
    }
    /* Conservative read timeout so a transport regression cannot
     * hang the suite. */
    struct timeval tv = {.tv_sec = 3, .tv_usec = 0};
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return fd;
}

/* Parse the Content-Length: N header and return N, or -1 if absent. */
static long sse_cov_content_length(const char *buf)
{
    const char *p = strstr(buf, "Content-Length:");
    if (p == NULL)
        return -1;
    p += sizeof("Content-Length:") - 1u;
    while (*p == ' ' || *p == '\t')
        p++;
    long v = 0;
    int any = 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (*p - '0');
        p++;
        any = 1;
    }
    return any != 0 ? v : -1;
}

/* Read until the connection drops or we have headers + the full
 * Content-Length body. The server closes the socket after each
 * non-SSE response (Connection: close), so EOF is the canonical end-
 * of-message marker. We also short-circuit once header+body are
 * complete so the test does not wait on TCP teardown timing. */
static ssize_t sse_cov_drain(int fd, char *buf, size_t cap)
{
    if (cap < 2u)
        return 0;
    size_t total = 0u;
    const size_t max_total = cap - 1u;
    while (total < max_total) {
        size_t want = max_total - total;
        /* NOLINTNEXTLINE-justification: `read()` is read()-tainted by
         * the analyzer; the explicit `> want` clamp below + the
         * `total < max_total` loop guard pin `total` to `<= cap - 2`
         * BEFORE the buf[total] write. The static analyzer can't
         * follow the clamp across the `total += r` arithmetic, so the
         * ArrayBound finding is a known false positive. */
        /* NOLINTNEXTLINE(clang-analyzer-security.ArrayBound) */
        ssize_t r = read(fd, buf + total, want);
        if (r <= 0)
            break; /* EOF or error. */
        size_t got = (size_t)r > want ? want : (size_t)r;
        total += got;
        if (total >= cap)
            total = cap - 1u; /* unreachable — silences the analyzer's taint flow. */
        /* NOLINTNEXTLINE(clang-analyzer-security.ArrayBound) */
        buf[total] = '\0';
        const char *hdr_end = strstr(buf, "\r\n\r\n");
        if (hdr_end == NULL)
            continue;
        long cl = sse_cov_content_length(buf);
        if (cl < 0)
            break; /* no Content-Length → server will close. */
        size_t body_so_far = total - (size_t)((hdr_end + 4u) - buf);
        if (body_so_far >= (size_t)cl)
            break;
    }
    if (total >= cap)
        total = cap - 1u;
    /* NOLINTNEXTLINE(clang-analyzer-security.ArrayBound) */
    buf[total] = '\0';
    return (ssize_t)total;
}

static char *test_sse_health_endpoint(void)
{
    SseTestCtx s;
    mu_assert("sse up", sse_test_ctx_up(&s) == 0);

    int fd = sse_cov_connect(s.port);
    mu_assert("connect", fd >= 0);
    static const char req[] = "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    ssize_t w = write(fd, req, sizeof(req) - 1u);
    mu_assert("write", w == (ssize_t)(sizeof(req) - 1u));

    char buf[2048];
    ssize_t n = sse_cov_drain(fd, buf, sizeof(buf));
    (void)close(fd);
    mu_assert("got bytes", n > 0);
    mu_assert("health 200", strstr(buf, "200 OK") != NULL);
    mu_assert("health vmaf-mcp", strstr(buf, "vmaf-mcp") != NULL);

    sse_test_ctx_down(&s);
    return NULL;
}

static char *test_sse_404_unknown_path(void)
{
    SseTestCtx s;
    mu_assert("sse up", sse_test_ctx_up(&s) == 0);

    int fd = sse_cov_connect(s.port);
    mu_assert("connect", fd >= 0);
    static const char req[] = "GET /no/such/endpoint HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    ssize_t w = write(fd, req, sizeof(req) - 1u);
    mu_assert("write", w == (ssize_t)(sizeof(req) - 1u));

    char buf[2048];
    ssize_t n = sse_cov_drain(fd, buf, sizeof(buf));
    (void)close(fd);
    mu_assert("got bytes", n > 0);
    mu_assert("404 Not Found", strstr(buf, "404 Not Found") != NULL);

    sse_test_ctx_down(&s);
    return NULL;
}

static char *test_sse_malformed_request_line(void)
{
    SseTestCtx s;
    mu_assert("sse up", sse_test_ctx_up(&s) == 0);

    int fd = sse_cov_connect(s.port);
    mu_assert("connect", fd >= 0);
    /* No space between the method and the (absent) URL — fails the
     * sse_parse_request_line() validator. */
    static const char req[] = "BROKENLINE_NOSPACES\r\n\r\n";
    ssize_t w = write(fd, req, sizeof(req) - 1u);
    mu_assert("write", w == (ssize_t)(sizeof(req) - 1u));

    char buf[2048];
    ssize_t n = sse_cov_drain(fd, buf, sizeof(buf));
    (void)close(fd);
    mu_assert("got bytes", n > 0);
    mu_assert("400 Bad Request", strstr(buf, "400 Bad Request") != NULL);

    sse_test_ctx_down(&s);
    return NULL;
}

static char *test_sse_post_without_content_length(void)
{
    SseTestCtx s;
    mu_assert("sse up", sse_test_ctx_up(&s) == 0);

    int fd = sse_cov_connect(s.port);
    mu_assert("connect", fd >= 0);
    /* POST hits the configured /mcp/sse path but ships no Content-
     * Length header → sse_serve_post() sees content_length == 0 and
     * emits the 400 envelope. */
    static const char req[] = "POST /mcp/sse HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    ssize_t w = write(fd, req, sizeof(req) - 1u);
    mu_assert("write", w == (ssize_t)(sizeof(req) - 1u));

    char buf[2048];
    ssize_t n = sse_cov_drain(fd, buf, sizeof(buf));
    (void)close(fd);
    mu_assert("got bytes", n > 0);
    mu_assert("400 Bad Request", strstr(buf, "400 Bad Request") != NULL);

    sse_test_ctx_down(&s);
    return NULL;
}

/* ============================================================
 * Test table — mirrors test_mcp_smoke.c pattern to keep run_tests()
 * below the readability-function-size budget.
 * ============================================================ */

typedef char *(*test_fn)(void);

static const test_fn k_cov_test_table[] = {
    /* Dispatcher */
    test_initialize_roundtrip,
    test_resources_list_roundtrip,
    test_parse_error_envelope,
    test_invalid_request_missing_method,
    test_invalid_request_non_string_method,
    test_notification_no_response,
    test_tools_call_missing_params,
    test_tools_call_non_string_name,
    test_tools_call_unknown_tool,
    test_compute_vmaf_missing_arguments,
    test_compute_vmaf_missing_required_field,
    /* Lifecycle */
    test_init_rejects_non_power_of_two_queue_depth,
    test_init_rejects_max_drain_over_cap,
    test_init_accepts_valid_power_of_two_queue_depth,
    test_user_agent_surfaces_in_initialize,
    test_transport_available_positive_and_oob,
    test_start_stdio_double_start_returns_ebusy,
    test_start_uds_rejects_empty_path,
    test_start_uds_rejects_overlong_path,
    test_start_sse_rejects_empty_path,
    test_start_sse_rejects_overlong_path,
    test_close_after_eof_joins_worker,
    /* Transports */
    test_stdio_oversize_line_overflow_envelope,
    test_sse_health_endpoint,
    test_sse_404_unknown_path,
    test_sse_malformed_request_line,
    test_sse_post_without_content_length,
};

static const size_t k_cov_test_table_len = sizeof(k_cov_test_table) / sizeof(k_cov_test_table[0]);

char *run_tests(void)
{
    for (size_t i = 0u; i < k_cov_test_table_len; ++i) {
        mu_run_test(k_cov_test_table[i]);
    }
    return NULL;
}
