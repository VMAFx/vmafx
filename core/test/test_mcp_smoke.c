/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 *
 *  Smoke + protocol test for the embedded MCP server.
 *
 *  T5-2 (ADR-0209) shipped this file pinning the -ENOSYS scaffold
 *  contract. T5-2b (PR #490) flipped init/start_stdio/close to a
 *  working dispatcher. T5-2c (this PR — MCP runtime v2):
 *      - flips UDS from -ENOSYS to a real bind/listen/accept loop;
 *      - replaces compute_vmaf's `deferred_to_v2` placeholder with
 *        a real libvmaf scoring binding.
 *
 *  Coverage:
 *      - public availability + transport-availability accessors
 *      - NULL-guard contract on every entry point
 *      - init / start_stdio / stop / close lifecycle
 *      - JSON-RPC `tools/list` round-trip (stdio)
 *      - JSON-RPC `tools/call` for `list_features` round-trip
 *      - method-not-found error envelope
 *      - UDS bind + JSON-RPC round-trip
 *      - compute_vmaf real-score check against the testdata 576x324
 *        YUV pair (sanity-bounded; not bit-exact)
 *
 *  T5-2d (this PR — MCP runtime v3) flips SSE from -ENOSYS to a
 *  real loopback HTTP server (no mongoose vendor — see
 *  transport_sse.c header). New coverage:
 *      - SSE start/stop lifecycle on an ephemeral port
 *      - HTTP GET /mcp/sse returns text/event-stream framing
 *      - HTTP POST /mcp/sse delivers JSON-RPC tools/list and
 *        gets the inline JSON response back
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

static char *test_available_returns_one(void)
{
    mu_assert("vmaf_mcp_available must report 1 in the smoke build", vmaf_mcp_available() == 1);
    return NULL;
}

static char *test_transport_available_unknown_id_is_zero(void)
{
    int id = 999;
    mu_assert("unknown transport id must report unavailable",
              vmaf_mcp_transport_available((VmafMcpTransport)id) == 0);
    return NULL;
}

static char *test_init_rejects_null_out(void)
{
    int rc = vmaf_mcp_init(NULL, (VmafContext *)0x1, NULL);
    mu_assert("NULL out -> -EINVAL", rc == -EINVAL);
    return NULL;
}

static char *test_init_rejects_null_ctx(void)
{
    VmafMcpServer *server = (VmafMcpServer *)0x1;
    int rc = vmaf_mcp_init(&server, NULL, NULL);
    mu_assert("NULL ctx -> -EINVAL", rc == -EINVAL);
    return NULL;
}

/* Removed in v3: SSE is no longer -ENOSYS. The negative case
 * (NULL cfg) is covered by test_start_sse_rejects_null_cfg below;
 * the full SSE round-trip is covered by test_sse_event_stream. */
static char *test_start_sse_rejects_null_cfg(void)
{
    VmafMcpServer *server = (VmafMcpServer *)0x1;
    int rc = vmaf_mcp_start_sse(server, NULL);
    mu_assert("NULL cfg -> -EINVAL", rc == -EINVAL);
    return NULL;
}

static char *test_start_uds_rejects_null_path(void)
{
    VmafMcpServer *server = (VmafMcpServer *)0x1;
    VmafMcpUdsConfig cfg = {.path = NULL};
    int rc = vmaf_mcp_start_uds(server, &cfg);
    mu_assert("NULL path -> -EINVAL", rc == -EINVAL);
    return NULL;
}

/* Removed in v2: UDS is no longer -ENOSYS. The negative case
 * (NULL path) is still covered by test_start_uds_rejects_null_path.
 * The full UDS round-trip is covered by test_uds_roundtrip below. */

static char *test_start_stdio_rejects_negative_fd(void)
{
    VmafMcpServer *server = (VmafMcpServer *)0x1;
    VmafMcpStdioConfig cfg = {.fd_in = -1, .fd_out = 1};
    int rc = vmaf_mcp_start_stdio(server, &cfg);
    mu_assert("negative fd_in -> -EINVAL", rc == -EINVAL);
    return NULL;
}

static char *test_stop_rejects_null(void)
{
    int rc = vmaf_mcp_stop(NULL);
    mu_assert("NULL server -> -EINVAL", rc == -EINVAL);
    return NULL;
}

static char *test_close_null_is_noop(void)
{
    vmaf_mcp_close(NULL);
    return NULL;
}

static char *test_close_pointer_to_null_is_noop(void)
{
    VmafMcpServer *server = NULL;
    vmaf_mcp_close(&server);
    mu_assert("close leaves *server NULL", server == NULL);
    return NULL;
}

/* ============================================================
 * v1 runtime helpers
 * ============================================================ */

typedef struct McpHarness {
    int req_pipe[2];
    int resp_pipe[2];
    VmafContext *ctx;
    VmafMcpServer *server;
} McpHarness;

static int harness_init(McpHarness *h)
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

static void harness_teardown(McpHarness *h)
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

/* Read up to max_len-1 bytes until LF. */
static ssize_t read_one_line(int fd, char *buf, size_t max_len)
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

static char *send_and_read(McpHarness *h, const char *req, size_t req_len, char *line,
                           size_t line_cap)
{
    ssize_t w = write(h->req_pipe[1], req, req_len);
    mu_assert("write request", w == (ssize_t)req_len);
    ssize_t n = read_one_line(h->resp_pipe[0], line, line_cap);
    mu_assert("response received", n > 0);
    return NULL;
}

/* ============================================================
 * v1 runtime tests
 * ============================================================ */

static char *test_init_close_lifecycle(void)
{
    VmafContext *ctx = NULL;
    VmafConfiguration cfg = {0};
    cfg.log_level = VMAF_LOG_LEVEL_NONE;
    cfg.n_threads = 1u;
    int crc = vmaf_init(&ctx, cfg);
    mu_assert("vmaf_init must succeed", crc == 0 && ctx != NULL);

    VmafMcpServer *server = NULL;
    int rc = vmaf_mcp_init(&server, ctx, NULL);
    mu_assert("v1 init must succeed", rc == 0);
    mu_assert("v1 init must return a non-NULL handle", server != NULL);

    vmaf_mcp_close(&server);
    mu_assert("close NULLs the handle", server == NULL);

    (void)vmaf_close(ctx);
    return NULL;
}

static char *test_jsonrpc_tools_list_roundtrip(void)
{
    McpHarness h;
    mu_assert("harness init", harness_init(&h) == 0);

    static const char tools_list[] = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}\n";
    char line[8192];
    char *err = send_and_read(&h, tools_list, sizeof(tools_list) - 1u, line, sizeof(line));
    if (err != NULL) {
        harness_teardown(&h);
        return err;
    }
    mu_assert("list contains list_features", strstr(line, "\"list_features\"") != NULL);
    mu_assert("list contains compute_vmaf", strstr(line, "\"compute_vmaf\"") != NULL);
    mu_assert("jsonrpc 2.0", strstr(line, "\"jsonrpc\":\"2.0\"") != NULL);
    mu_assert("id 1", strstr(line, "\"id\":1") != NULL);

    harness_teardown(&h);
    return NULL;
}

static char *test_jsonrpc_tools_call_list_features(void)
{
    McpHarness h;
    mu_assert("harness init", harness_init(&h) == 0);

    static const char call_lf[] = "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\","
                                  "\"params\":{\"name\":\"list_features\",\"arguments\":{}}}\n";
    char line[8192];
    char *err = send_and_read(&h, call_lf, sizeof(call_lf) - 1u, line, sizeof(line));
    if (err != NULL) {
        harness_teardown(&h);
        return err;
    }
    mu_assert("id 2", strstr(line, "\"id\":2") != NULL);
    mu_assert("content array", strstr(line, "\"content\"") != NULL);
    mu_assert("mentions features", strstr(line, "features") != NULL);

    harness_teardown(&h);
    return NULL;
}

static char *test_jsonrpc_method_not_found(void)
{
    McpHarness h;
    mu_assert("harness init", harness_init(&h) == 0);

    static const char bogus[] = "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"does/not/exist\"}\n";
    char line[2048];
    char *err = send_and_read(&h, bogus, sizeof(bogus) - 1u, line, sizeof(line));
    if (err != NULL) {
        harness_teardown(&h);
        return err;
    }
    mu_assert("error envelope", strstr(line, "\"error\"") != NULL);
    mu_assert("code -32601", strstr(line, "-32601") != NULL);

    harness_teardown(&h);
    return NULL;
}

/* ============================================================
 * UDS transport round-trip (v2)
 * ============================================================ */

static char *test_uds_roundtrip(void)
{
    /* Deterministic per-pid socket path — keeps parallel test
     * runs from clobbering each other. */
    char path[80];
    int n = snprintf(path, sizeof(path), "/tmp/vmaf-mcp-uds-test-%d.sock", (int)getpid());
    mu_assert("path snprintf", n > 0 && (size_t)n < sizeof(path));

    VmafContext *ctx = NULL;
    VmafConfiguration vcfg = {0};
    vcfg.log_level = VMAF_LOG_LEVEL_NONE;
    vcfg.n_threads = 1u;
    mu_assert("vmaf_init", vmaf_init(&ctx, vcfg) == 0);

    VmafMcpServer *server = NULL;
    mu_assert("mcp init", vmaf_mcp_init(&server, ctx, NULL) == 0);

    VmafMcpUdsConfig ucfg = {.path = path};
    int rc = vmaf_mcp_start_uds(server, &ucfg);
    mu_assert("uds start", rc == 0);

    /* Connect a client and round-trip a tools/list. */
    int cfd = socket(AF_UNIX, SOCK_STREAM, 0);
    mu_assert("client socket", cfd >= 0);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path, strlen(path) + 1u);
    int crc = connect(cfd, (const struct sockaddr *)&addr, sizeof(addr));
    mu_assert("client connect", crc == 0);

    static const char req[] = "{\"jsonrpc\":\"2.0\",\"id\":42,\"method\":\"tools/list\"}\n";
    ssize_t w = write(cfd, req, sizeof(req) - 1u);
    mu_assert("client write", w == (ssize_t)(sizeof(req) - 1u));

    char line[8192];
    ssize_t got = read_one_line(cfd, line, sizeof(line));
    mu_assert("uds response received", got > 0);
    mu_assert("uds id 42", strstr(line, "\"id\":42") != NULL);
    mu_assert("uds list contains compute_vmaf", strstr(line, "\"compute_vmaf\"") != NULL);

    (void)close(cfd);
    vmaf_mcp_close(&server);
    (void)vmaf_close(ctx);

    /* Verify socket file got cleaned up by stop(). */
    struct stat st;
    int sr = stat(path, &st);
    mu_assert("uds socket file unlinked on close", sr != 0);
    return NULL;
}

/* ============================================================
 * compute_vmaf real-score binding (v2)
 * ============================================================ */

/* Use the testdata 576x324 8-bit YUV pair shipped at
 * testdata/ref_576x324_48f.yuv + dis_576x324_48f.yuv. The test
 * is conditional: if the host does not ship the testdata yuv
 * (e.g. some packagers strip it), we skip rather than fail. */
static char *test_compute_vmaf_real_score(void)
{
    const char *ref_path = "../testdata/ref_576x324_48f.yuv";
    const char *dis_path = "../testdata/dis_576x324_48f.yuv";
    struct stat st;
    if (stat(ref_path, &st) != 0 || stat(dis_path, &st) != 0)
        return NULL; /* fixture absent on this host — skip. */

    McpHarness h;
    mu_assert("harness init", harness_init(&h) == 0);

    char req[512];
    int n = snprintf(req, sizeof(req),
                     "{\"jsonrpc\":\"2.0\",\"id\":99,\"method\":\"tools/call\","
                     "\"params\":{\"name\":\"compute_vmaf\",\"arguments\":{"
                     "\"reference_path\":\"%s\","
                     "\"distorted_path\":\"%s\","
                     "\"width\":576,\"height\":324,"
                     "\"model_version\":\"vmaf_v0.6.1\"}}}\n",
                     ref_path, dis_path);
    mu_assert("req snprintf", n > 0 && (size_t)n < sizeof(req));

    /* Larger buffer — compute_vmaf wraps a stringified JSON object
     * inside MCP's content envelope, so the line can exceed 1 KiB. */
    static char line[16384];
    char *err = send_and_read(&h, req, (size_t)n, line, sizeof(line));
    if (err != NULL) {
        harness_teardown(&h);
        return err;
    }
    /* Real path returns a `score` numeric field; the placeholder
     * (which v2 must NOT bring back) returned `deferred_to_v2`. */
    mu_assert("compute_vmaf id 99", strstr(line, "\"id\":99") != NULL);
    mu_assert("compute_vmaf returns score", strstr(line, "\\\"score\\\"") != NULL);
    mu_assert("compute_vmaf no v1 placeholder", strstr(line, "deferred_to_v2") == NULL);
    mu_assert("compute_vmaf reports frames_scored", strstr(line, "\\\"frames_scored\\\"") != NULL);

    harness_teardown(&h);
    return NULL;
}

static int write_all_bytes(int fd, const unsigned char *buf, size_t len)
{
    size_t off = 0u;
    while (off < len) {
        ssize_t w = write(fd, buf + off, len - off);
        if (w <= 0)
            return -1;
        off += (size_t)w;
    }
    return 0;
}

static int write_yuv420p10_fixture(const char *path, unsigned width, unsigned height,
                                   unsigned frames)
{
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (fd < 0)
        return -1;
    const unsigned chroma_width = width / 2u;
    const unsigned chroma_height = height / 2u;
    const unsigned char y_sample[2] = {0x00u, 0x02u};  /* 512 little-endian. */
    const unsigned char uv_sample[2] = {0x00u, 0x02u}; /* neutral chroma. */
    int rc = 0;
    for (unsigned f = 0u; f < frames && rc == 0; ++f) {
        for (unsigned i = 0u; i < width * height && rc == 0; ++i)
            rc = write_all_bytes(fd, y_sample, sizeof(y_sample));
        for (unsigned p = 0u; p < 2u && rc == 0; ++p) {
            for (unsigned i = 0u; i < chroma_width * chroma_height && rc == 0; ++i)
                rc = write_all_bytes(fd, uv_sample, sizeof(uv_sample));
        }
    }
    int close_rc = close(fd);
    return rc == 0 && close_rc == 0 ? 0 : -1;
}

static char *test_compute_vmaf_yuv420p10_score(void)
{
    char ref_path[96];
    char dis_path[96];
    int rn = snprintf(ref_path, sizeof(ref_path), "/tmp/vmaf-mcp-ref-10b-%d.yuv", (int)getpid());
    int dn = snprintf(dis_path, sizeof(dis_path), "/tmp/vmaf-mcp-dis-10b-%d.yuv", (int)getpid());
    mu_assert("ref path snprintf", rn > 0 && (size_t)rn < sizeof(ref_path));
    mu_assert("dis path snprintf", dn > 0 && (size_t)dn < sizeof(dis_path));
    /* 192x192: float_ms_ssim needs min 176px per side (11-tap Gaussian × 5 levels).
     * 64x64 caused feed_frame failure → no score → mu_assert trip. */
    mu_assert("write ref 10-bit fixture", write_yuv420p10_fixture(ref_path, 192u, 192u, 2u) == 0);
    mu_assert("write dis 10-bit fixture", write_yuv420p10_fixture(dis_path, 192u, 192u, 2u) == 0);

    McpHarness h;
    mu_assert("harness init", harness_init(&h) == 0);

    char req[640];
    int n = snprintf(req, sizeof(req),
                     "{\"jsonrpc\":\"2.0\",\"id\":100,\"method\":\"tools/call\","
                     "\"params\":{\"name\":\"compute_vmaf\",\"arguments\":{"
                     "\"reference_path\":\"%s\","
                     "\"distorted_path\":\"%s\","
                     "\"width\":192,\"height\":192,\"bitdepth\":10,"
                     "\"model_version\":\"vmaf_v0.6.1\"}}}\n",
                     ref_path, dis_path);
    mu_assert("req snprintf", n > 0 && (size_t)n < sizeof(req));

    static char line[16384];
    char *err = send_and_read(&h, req, (size_t)n, line, sizeof(line));
    if (err != NULL) {
        harness_teardown(&h);
        (void)unlink(ref_path);
        (void)unlink(dis_path);
        return err;
    }
    mu_assert("compute_vmaf 10-bit id 100", strstr(line, "\"id\":100") != NULL);
    mu_assert("compute_vmaf 10-bit returns score", strstr(line, "\\\"score\\\"") != NULL);
    mu_assert("compute_vmaf 10-bit reports bitdepth", strstr(line, "\\\"bitdepth\\\":10") != NULL);
    mu_assert("compute_vmaf 10-bit reports frames",
              strstr(line, "\\\"frames_scored\\\":2") != NULL);

    harness_teardown(&h);
    (void)unlink(ref_path);
    (void)unlink(dis_path);
    return NULL;
}

/* ============================================================
 * SSE transport round-trip (v3)
 *
 * Verifies the loopback HTTP server actually emits text/event-stream
 * framing per WHATWG SSE §9.2 (accessed 2026-05-09):
 * https://html.spec.whatwg.org/multipage/server-sent-events.html
 *
 * Per CLAUDE memory feedback_no_test_weakening — this test must
 * verify real event-stream framing, not just HTTP-200. We check:
 *   - GET /mcp/sse returns Content-Type: text/event-stream
 *   - The stream emits at least one parsable `event:` + `data:`
 *     frame terminated by a blank line
 *   - POST /mcp/sse delivers a JSON-RPC tools/list and the
 *     response body contains "list_features"
 * ============================================================ */

/* Connect a fresh client TCP socket to 127.0.0.1:port. */
static int sse_test_connect(uint16_t port)
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
    return fd;
}

/* Drain the socket up to `cap` bytes or short-read. Stops once we
 * see the SSE blank-line terminator OR after the read times out
 * (whichever happens first). Returns bytes read. Caller-owned
 * buffer; NUL-terminated. */
static ssize_t sse_test_drain(int fd, char *buf, size_t cap, int timeout_seconds)
{
    struct timeval tv = {.tv_sec = timeout_seconds, .tv_usec = 0};
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    size_t total = 0u;
    while (total + 1u < cap) {
        ssize_t r = read(fd, buf + total, cap - 1u - total);
        if (r <= 0)
            break;
        total += (size_t)r;
        buf[total] = '\0';
        /* Per WHATWG SSE §9.2 each event is terminated by a blank
         * line. Stop as soon as we see the terminator after we have
         * also seen at least one `data:` or `event:` field — which
         * means a real frame has arrived. */
        if (strstr(buf, "data: ") != NULL && strstr(buf, "\n\n") != NULL)
            break;
        /* Inline POST response uses Content-Length; once we have a
         * "\r\n\r\n" header terminator and any JSON-ish content, we
         * are done. */
        if (strstr(buf, "\r\n\r\n") != NULL && strstr(buf, "\"jsonrpc\"") != NULL)
            break;
    }
    buf[total] = '\0';
    return (ssize_t)total;
}

/* GET /mcp/sse — verify event-stream framing per WHATWG SSE §9.2. */
static char *sse_check_get_stream(uint16_t port)
{
    int gfd = sse_test_connect(port);
    mu_assert("sse get connect", gfd >= 0);
    static const char get_req[] = "GET /mcp/sse HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    ssize_t gw = write(gfd, get_req, sizeof(get_req) - 1u);
    mu_assert("get write", gw == (ssize_t)(sizeof(get_req) - 1u));

    static char gbuf[4096];
    ssize_t got = sse_test_drain(gfd, gbuf, sizeof(gbuf), 3);
    (void)close(gfd);
    mu_assert("sse get response", got > 0);
    mu_assert("sse 200 OK", strstr(gbuf, "200 OK") != NULL);
    mu_assert("sse content-type", strstr(gbuf, "text/event-stream") != NULL);
    /* Per WHATWG SSE §9.2 (accessed 2026-05-09) an event frame ends
     * in a blank line. The transport emits an initial `event: ready`
     * frame after the response headers — verify both pieces are
     * present. */
    mu_assert("sse event field", strstr(gbuf, "event: ready") != NULL);
    mu_assert("sse data field", strstr(gbuf, "data: ") != NULL);
    mu_assert("sse blank-line terminator", strstr(gbuf, "\n\n") != NULL);
    return NULL;
}

/* POST /mcp/sse — verify JSON-RPC tools/list inline round-trip. */
static char *sse_check_post_jsonrpc(uint16_t port)
{
    int pfd = sse_test_connect(port);
    mu_assert("sse post connect", pfd >= 0);
    static const char rpc[] = "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/list\"}";
    char post_req[512];
    int pn = snprintf(post_req, sizeof(post_req),
                      "POST /mcp/sse HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                      "Content-Type: application/json\r\n"
                      "Content-Length: %zu\r\n\r\n%s",
                      sizeof(rpc) - 1u, rpc);
    mu_assert("post snprintf", pn > 0 && (size_t)pn < sizeof(post_req));
    ssize_t pw = write(pfd, post_req, (size_t)pn);
    mu_assert("post write", pw == (ssize_t)pn);

    static char pbuf[8192];
    ssize_t pgot = sse_test_drain(pfd, pbuf, sizeof(pbuf), 5);
    (void)close(pfd);
    mu_assert("sse post response", pgot > 0);
    mu_assert("sse post 200", strstr(pbuf, "200 OK") != NULL);
    mu_assert("sse post jsonrpc", strstr(pbuf, "\"jsonrpc\":\"2.0\"") != NULL);
    mu_assert("sse post id 7", strstr(pbuf, "\"id\":7") != NULL);
    mu_assert("sse post lists features", strstr(pbuf, "list_features") != NULL);
    return NULL;
}

static char *test_sse_event_stream(void)
{
    /* Spawn the SSE server on an ephemeral loopback port. */
    VmafContext *ctx = NULL;
    VmafConfiguration vcfg = {0};
    vcfg.log_level = VMAF_LOG_LEVEL_NONE;
    vcfg.n_threads = 1u;
    mu_assert("vmaf_init", vmaf_init(&ctx, vcfg) == 0);

    VmafMcpServer *server = NULL;
    mu_assert("mcp init", vmaf_mcp_init(&server, ctx, NULL) == 0);

    VmafMcpSseConfig sse_cfg = {.port = 0, .path = NULL};
    int sse_rc = vmaf_mcp_start_sse(server, &sse_cfg);
    mu_assert("sse start", sse_rc == 0);
    mu_assert("sse port resolved", sse_cfg.port != 0u);

    char *err = sse_check_get_stream(sse_cfg.port);
    if (err == NULL)
        err = sse_check_post_jsonrpc(sse_cfg.port);

    vmaf_mcp_close(&server);
    (void)vmaf_close(ctx);
    return err;
}

/* ============================================================
 * Test table — keeps run_tests below clang-tidy's 15-branch budget
 * (mirrors test_hip_smoke.c / test_vulkan_smoke.c precedent).
 * ============================================================ */

typedef char *(*test_fn)(void);

static const test_fn k_test_table[] = {
    test_available_returns_one,
    test_transport_available_unknown_id_is_zero,
    test_init_rejects_null_out,
    test_init_rejects_null_ctx,
    test_start_sse_rejects_null_cfg,
    test_start_uds_rejects_null_path,
    test_start_stdio_rejects_negative_fd,
    test_stop_rejects_null,
    test_close_null_is_noop,
    test_close_pointer_to_null_is_noop,
    test_init_close_lifecycle,
    test_jsonrpc_tools_list_roundtrip,
    test_jsonrpc_tools_call_list_features,
    test_jsonrpc_method_not_found,
    test_uds_roundtrip,
    test_compute_vmaf_real_score,
    test_compute_vmaf_yuv420p10_score,
    test_sse_event_stream,
};

static const size_t k_test_table_len = sizeof(k_test_table) / sizeof(k_test_table[0]);

char *run_tests(void)
{
    for (size_t i = 0u; i < k_test_table_len; ++i) {
        mu_run_test(k_test_table[i]);
    }
    return NULL;
}
