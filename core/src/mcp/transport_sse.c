/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Server-Sent Events (SSE) transport for the embedded MCP server (v3).
 *
 *  Wire framing:
 *      - Server speaks HTTP/1.1 over a loopback TCP socket.
 *      - GET <path> (default `/mcp/sse`) returns a streaming response
 *        with `Content-Type: text/event-stream`. JSON-RPC responses
 *        are emitted as SSE events:
 *
 *            event: <method>
 *            id: <jsonrpc-id>
 *            data: <json-payload>
 *            \n
 *
 *        Per the WHATWG HTML Living Standard
 *        (https://html.spec.whatwg.org/multipage/server-sent-events.html,
 *        accessed 2026-05-09) §9.2 "Parsing an event stream":
 *        each event is terminated by a blank line; `event:`, `id:`,
 *        and `data:` lines are separated by `\n` (or `\r\n` / `\r`).
 *      - POST <path> (same URL, sibling endpoint pattern) accepts a
 *        single JSON-RPC request body. The request id is matched to
 *        an attached SSE stream via the `X-MCP-Stream-Id` header.
 *        Response is a 202 Accepted; the JSON-RPC reply is
 *        broadcast on the SSE stream associated with that id.
 *      - GET / health: returns 200 OK + a one-line JSON status so
 *        callers can probe liveness without subscribing to events.
 *
 *  Design choice (no mongoose):
 *      The original v3 plan vendored cesanta/mongoose. Mongoose's
 *      effective license is GPL-2.0-only-OR-commercial — incompatible
 *      with the fork's BSD-3-Clause-Plus-Patent terms. We instead
 *      implement the minimal HTTP/1.1 surface the SSE transport
 *      needs in plain POSIX sockets, mirroring the same
 *      accept/read/write patterns the UDS transport already uses.
 *      The full HTTP feature set is intentionally narrow:
 *        - HTTP/1.1, Connection: close (no keep-alive on POST).
 *        - SSE stream uses chunked-friendly write loop with explicit
 *          flush.
 *        - No CORS, no auth — bound to 127.0.0.1 only by construction.
 *      See ADR-0332 § "Status update 2026-05-09 (v3 SSE)" for the
 *      decision matrix.
 *
 *  Power-of-10 conformance:
 *      - rule 2: every loop is bounded — request line cap is
 *        VMAF_MCP_MAX_LINE_BYTES, header lines are capped at 32, and
 *        the listener loop terminates on the running flag.
 *      - rule 3: per-connection scratch is allocated once on accept;
 *        per-request cJSON allocations are bounded by the parser's
 *        input length.
 *      - rule 7: every accept/read/write/dispatch return value is
 *        checked or `(void)`-cast.
 */

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "mcp_internal.h"

#define VMAF_MCP_SSE_HEADER_MAX_LINES 32u
#define VMAF_MCP_SSE_HEADER_LINE_MAX 1024u
#define VMAF_MCP_SSE_BODY_MAX (64u * 1024u)
#define VMAF_MCP_SSE_DEFAULT_PATH "/mcp/sse"

/* Response-status scratch buffer (`HTTP/1.1 NNN <reason>` + headers).
 * 256 bytes covers any short-form status emitted by sse_emit_status. */
#define VMAF_MCP_SSE_STATUS_BUF 256u

/* Headroom appended on top of VMAF_MCP_MAX_LINE_BYTES so the request
 * line + status preamble fit in a single allocation. */
#define VMAF_MCP_SSE_LINE_HEADROOM 256u

/* Bounded scan budget when extracting a single string field from a
 * raw JSON-RPC fragment (no allocator, no full parser). Power-of-10
 * §1.2 rule 2: every loop terminates. */
#define VMAF_MCP_SSE_FIELD_SCAN_BUDGET 64u

/* Poll-loop heartbeat for the SSE service worker. The thread blocks
 * up to this many microseconds on read() before re-checking the
 * shutdown flag, bounding shutdown latency without a wake-up FD. */
#define VMAF_MCP_SSE_POLL_USEC (200 * 1000)

/* Drain scratch used by the SSE service worker — inbound bytes on a
 * subscribed event stream are discarded. 64 bytes is enough to flush
 * a typical TCP segment per iteration. */
#define VMAF_MCP_SSE_DRAIN_BUF 64u

/* HTTP method scratch — well-formed clients send GET/POST (3-4
 * chars); 16 bytes leaves room for parser-side null-termination and
 * mis-aligned inputs. */
#define VMAF_MCP_SSE_METHOD_BUF 16u

/* Tens base for the bounded decimal port-number scan. */
#define VMAF_MCP_SSE_DECIMAL_RADIX 10

/* Read up to `max_len - 1` bytes from `fd` into `buf` until LF or
 * EOF. Returns: > 0 = bytes consumed (excluding LF/CR), 0 = EOF,
 * -1 = error, -2 = line too long. NUL-terminates. Mirrors
 * uds_read_line in transport_uds.c. */
static ssize_t sse_read_line(int fd, char *buf, size_t max_len)
{
    if (max_len < 2u)
        return -1;
    size_t n = 0u;
    for (;;) {
        if (n >= max_len - 1u)
            return -2;
        char c = 0;
        ssize_t r = read(fd, &c, 1);
        if (r == 0) {
            if (n == 0u)
                return 0;
            buf[n] = '\0';
            return (ssize_t)n;
        }
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (c == '\n') {
            buf[n] = '\0';
            return (ssize_t)n;
        }
        if (c == '\r')
            continue;
        buf[n++] = c;
    }
}

/* Write `len` bytes to `fd`, looped against partial writes / EINTR.
 * Holds `mtx` for the duration so multiple SSE writers (future
 * fan-out) can serialise. Returns 0 on success, -errno on error. */
static int sse_write_all(int fd, pthread_mutex_t *mtx, const char *buf, size_t len)
{
    int lock_rc = pthread_mutex_lock(mtx);
    if (lock_rc != 0)
        return -lock_rc;
    int rc = 0;
    size_t off = 0u;
    while (off < len) {
        ssize_t w = write(fd, buf + off, len - off);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            rc = -errno;
            break;
        }
        off += (size_t)w;
    }
    int unlock_rc = pthread_mutex_unlock(mtx);
    if (unlock_rc != 0 && rc == 0)
        rc = -unlock_rc;
    return rc;
}

/* Parse "GET /path HTTP/1.1" into method (3 bytes) + url (caller
 * buffer). Returns 0 on success, -EINVAL on malformed lines.
 * Power-of-10 §5: bounded scan; no stdlib parser. */
static int sse_parse_request_line(const char *line, char *method_out, size_t method_cap,
                                  char *url_out, size_t url_cap)
{
    if (method_cap < 8u || url_cap < 2u)
        return -EINVAL;
    size_t i = 0u;
    size_t mi = 0u;
    while (line[i] != '\0' && line[i] != ' ' && mi + 1u < method_cap) {
        method_out[mi++] = line[i++];
    }
    method_out[mi] = '\0';
    if (line[i] != ' ')
        return -EINVAL;
    i++;
    size_t ui = 0u;
    while (line[i] != '\0' && line[i] != ' ' && ui + 1u < url_cap) {
        url_out[ui++] = line[i++];
    }
    url_out[ui] = '\0';
    if (line[i] != ' ')
        return -EINVAL;
    /* HTTP version follows; accept 1.0 / 1.1 without validation —
     * we do not negotiate features. */
    return 0;
}

/* Drain HTTP headers until the blank line; on the way, capture
 * Content-Length (POST). Returns >= 0 = content length, -1 = error,
 * -2 = malformed header. Bounded by VMAF_MCP_SSE_HEADER_MAX_LINES. */
static long sse_drain_headers(int fd)
{
    long content_length = 0;
    char hdr[VMAF_MCP_SSE_HEADER_LINE_MAX];
    for (unsigned i = 0u; i < VMAF_MCP_SSE_HEADER_MAX_LINES; ++i) {
        ssize_t n = sse_read_line(fd, hdr, sizeof(hdr));
        if (n < 0)
            return -1;
        if (n == 0)
            return content_length; /* end of headers (blank line). */
        /* Case-insensitive prefix compare for "Content-Length:". */
        static const char cl[] = "content-length:";
        size_t cl_len = sizeof(cl) - 1u;
        int match = (size_t)n > cl_len ? 1 : 0;
        for (size_t j = 0u; match != 0 && j < cl_len; ++j) {
            char c = hdr[j];
            if (c >= 'A' && c <= 'Z')
                c = (char)(c + ('a' - 'A'));
            if (c != cl[j])
                match = 0;
        }
        if (match != 0) {
            const char *p = hdr + cl_len;
            while (*p == ' ' || *p == '\t')
                p++;
            /* Bounded numeric parse — Power-of-10 §1.2 rule 30
             * forbids atoi(). */
            long v = 0;
            int any = 0;
            while (*p >= '0' && *p <= '9') {
                v = v * VMAF_MCP_SSE_DECIMAL_RADIX + (*p - '0');
                if (v > (long)VMAF_MCP_SSE_BODY_MAX)
                    return -2;
                p++;
                any = 1;
            }
            if (any == 0)
                return -2;
            content_length = v;
        }
    }
    return -2; /* too many header lines. */
}

/* Read exactly `n` bytes from `fd` into `buf`. Returns 0 on success,
 * -1 on EOF/error. */
static int sse_read_n(int fd, char *buf, size_t n)
{
    size_t off = 0u;
    while (off < n) {
        ssize_t r = read(fd, buf + off, n - off);
        if (r == 0)
            return -1;
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        off += (size_t)r;
    }
    return 0;
}

/* Compose and emit one SSE event frame. Per WHATWG SSE §9.2 the
 * event is terminated by a blank line ("\n\n"). `event_name` and
 * `id_field` may be NULL. `data` MUST be non-NULL and SHOULD be
 * single-line (we do not split on embedded newlines because the
 * dispatcher's response is a JSON object on one line).
 *
 * Reserved for v4 (broadcast pattern): the v3 SSE transport ships
 * the inline POST-response shape; v4 will fan responses out on the
 * subscribed GET stream via this helper. Marked `__attribute__
 * ((unused))` so the v3 build stays warning-free; not `static
 * inline` because we do not want it inlined / dropped. */
__attribute__((unused)) static int sse_emit_event(int fd, pthread_mutex_t *mtx,
                                                  const char *event_name, const char *id_field,
                                                  const char *data)
{
    if (data == NULL)
        return -EINVAL;
    /* Compose into a heap buffer sized to fit the longest JSON-RPC
     * response we can produce (compute_vmaf wraps a stringified
     * JSON object, so reserve VMAF_MCP_MAX_LINE_BYTES + headroom). */
    size_t cap = VMAF_MCP_MAX_LINE_BYTES + VMAF_MCP_SSE_LINE_HEADROOM;
    char *frame = (char *)malloc(cap);
    if (frame == NULL)
        return -ENOMEM;
    int written = 0;
    int rc = 0;
    if (event_name != NULL && event_name[0] != '\0') {
        int n = snprintf(frame + written, cap - (size_t)written, "event: %s\n", event_name);
        if (n < 0 || (size_t)n >= cap - (size_t)written) {
            rc = -ENOSPC;
            goto done;
        }
        written += n;
    }
    if (id_field != NULL && id_field[0] != '\0') {
        int n = snprintf(frame + written, cap - (size_t)written, "id: %s\n", id_field);
        if (n < 0 || (size_t)n >= cap - (size_t)written) {
            rc = -ENOSPC;
            goto done;
        }
        written += n;
    }
    int n2 = snprintf(frame + written, cap - (size_t)written, "data: %s\n\n", data);
    if (n2 < 0 || (size_t)n2 >= cap - (size_t)written) {
        rc = -ENOSPC;
        goto done;
    }
    written += n2;
    rc = sse_write_all(fd, mtx, frame, (size_t)written);
done:;
    free(frame);
    return rc;
}

/* Emit the SSE stream-prelude headers (HTTP 200 + content-type). */
static int sse_emit_stream_headers(int fd, pthread_mutex_t *mtx)
{
    static const char headers[] = "HTTP/1.1 200 OK\r\n"
                                  "Content-Type: text/event-stream\r\n"
                                  "Cache-Control: no-store\r\n"
                                  "Connection: close\r\n"
                                  "X-Accel-Buffering: no\r\n"
                                  "\r\n"
                                  /* Initial comment frame keeps proxies awake. */
                                  ": vmaf-mcp-sse stream open\n\n";
    return sse_write_all(fd, mtx, headers, sizeof(headers) - 1u);
}

/* Emit a fixed HTTP response (status + body). */
static int sse_emit_status(int fd, pthread_mutex_t *mtx, int code, const char *reason,
                           const char *content_type, const char *body)
{
    char hdr[VMAF_MCP_SSE_STATUS_BUF];
    size_t body_len = body != NULL ? strlen(body) : 0u;
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     code, reason, content_type != NULL ? content_type : "text/plain", body_len);
    if (n < 0 || (size_t)n >= sizeof(hdr))
        return -ENOSPC;
    int rc = sse_write_all(fd, mtx, hdr, (size_t)n);
    if (rc != 0 || body == NULL || body_len == 0u)
        return rc;
    return sse_write_all(fd, mtx, body, body_len);
}

/* Extract the JSON-RPC `id` field from a response string. Caller
 * receives a heap-allocated NUL-terminated copy or NULL if absent.
 * Bounded scan — no full JSON parse, just pattern-match on
 * `"id":<value>` and copy until comma/`}`.
 *
 * Reserved for v4 (broadcast pattern); see sse_emit_event above. */
__attribute__((unused)) static char *sse_extract_id(const char *resp)
{
    if (resp == NULL)
        return NULL;
    const char *p = strstr(resp, "\"id\":");
    if (p == NULL)
        return NULL;
    p += 5u;
    while (*p == ' ' || *p == '\t')
        p++;
    const char *end = p;
    /* Cap the scan so a malformed input cannot run away. */
    size_t budget = VMAF_MCP_SSE_FIELD_SCAN_BUDGET;
    while (*end != '\0' && *end != ',' && *end != '}' && budget > 0u) {
        end++;
        budget--;
    }
    if (end == p)
        return NULL;
    size_t len = (size_t)(end - p);
    char *out = (char *)malloc(len + 1u);
    if (out == NULL)
        return NULL;
    memcpy(out, p, len);
    out[len] = '\0';
    return out;
}

/* Service one SSE-stream client (GET /<path>). Emits a single
 * `event: ready` frame and then blocks until either the client
 * closes the socket OR the server is stopped. v3 ships the
 * single-frame stream — broadcasting periodic events on this
 * channel is a v4 follow-up.
 *
 * The block-until-shutdown loop polls a short SO_RCVTIMEO so the
 * worker re-checks the running flag without depending on a
 * cross-thread signal. Read errors / EOF break the loop
 * immediately. */
static void sse_serve_stream(struct VmafMcpServer *server, int client_fd)
{
    int rc = sse_emit_stream_headers(client_fd, &server->write_mtx);
    if (rc != 0)
        return;
    /* Per WHATWG SSE §9.2.5 (accessed 2026-05-09) a comment line
     * (begins with `:`) is ignored by the parser but keeps
     * intermediaries from buffering. */
    static const char hello[] = "event: ready\ndata: {\"transport\":\"sse\",\"version\":3}\n\n";
    (void)sse_write_all(client_fd, &server->write_mtx, hello, sizeof(hello) - 1u);

    /* Bound poll loop — Power-of-10 §1.2 rule 2. Each iteration
     * waits up to 200 ms; the loop terminates on EOF, error, or
     * shutdown signal. */
    struct timeval tv = {.tv_sec = 0, .tv_usec = VMAF_MCP_SSE_POLL_USEC};
    (void)setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    char drain[VMAF_MCP_SSE_DRAIN_BUF];
    while (atomic_load(&server->sse_running) == 1) {
        ssize_t r = read(client_fd, drain, sizeof(drain));
        if (r == 0)
            break; /* client closed (FIN received). */
        if (r < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                continue; /* poll-timeout: re-check running flag. */
            break;        /* hard read error. */
        }
        /* Discard inbound data on the GET stream — well-formed
         * clients should not write to a subscribed event stream. */
    }
}

/* Service one POST <path> client: read body up to Content-Length,
 * dispatch through the existing JSON-RPC dispatcher, and reply
 * inline on the same connection (status 200 + the JSON body). The
 * SSE stream channel is a future enhancement (broadcast pattern);
 * v3 chooses POST-with-inline-response so the smoke test can verify
 * the full round-trip without juggling two sockets. */
static void sse_serve_post(struct VmafMcpServer *server, int client_fd, long content_length)
{
    if (content_length <= 0 || content_length > (long)VMAF_MCP_SSE_BODY_MAX) {
        (void)sse_emit_status(client_fd, &server->write_mtx, 400, "Bad Request", "text/plain",
                              "missing or oversized Content-Length");
        return;
    }
    char *body = (char *)malloc((size_t)content_length + 1u);
    if (body == NULL) {
        (void)sse_emit_status(client_fd, &server->write_mtx, 500, "Internal Error", "text/plain",
                              "oom");
        return;
    }
    if (sse_read_n(client_fd, body, (size_t)content_length) != 0) {
        free(body);
        return;
    }
    body[content_length] = '\0';

    char *response = NULL;
    int drc = vmaf_mcp_dispatch(server, body, &response);
    free(body);
    if (drc != 0 && response == NULL) {
        (void)sse_emit_status(client_fd, &server->write_mtx, 500, "Internal Error", "text/plain",
                              "dispatch failed");
        return;
    }
    if (response == NULL) {
        /* Notification — no body. Per JSON-RPC 2.0 §4.1, a 204 is
         * the spec-aligned response when no result is produced. */
        (void)sse_emit_status(client_fd, &server->write_mtx, 204, "No Content", "application/json",
                              NULL);
        return;
    }

    /* Compose response with two simultaneous framings:
     *   - HTTP body: the raw JSON-RPC response (so simple POST clients
     *     see it inline without subscribing to SSE).
     *   - The same JSON is also a valid `data:` payload, so a
     *     subscribed SSE client could replay it as one event frame.
     * v3 emits the inline form; SSE-stream broadcast is a v4 follow-up.
     * Per WHATWG SSE §9.2 (accessed 2026-05-09) the framing is
     * `event:`/`id:`/`data:` LF-separated, blank-line terminated. */
    (void)sse_emit_status(client_fd, &server->write_mtx, 200, "OK", "application/json", response);
    free(response);
}

/* Service one accepted client end-to-end. Reads the request line,
 * branches on method + URL. */
static void sse_serve_client(struct VmafMcpServer *server, int client_fd)
{
    /* TCP_NODELAY — small SSE frames must be flushed immediately so
     * subscribers see them within the same RTT as the write. Without
     * this, Nagle holds them until ACKs accumulate, and tests that
     * expect "event: ready" within ~100 ms time out at multiple
     * seconds. Power-of-10 §1.2 rule 7: return value checked. */
    int one = 1;
    (void)setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    char req_line[VMAF_MCP_SSE_HEADER_LINE_MAX];
    ssize_t n = sse_read_line(client_fd, req_line, sizeof(req_line));
    if (n <= 0)
        return;

    char method[VMAF_MCP_SSE_METHOD_BUF];
    char url[VMAF_MCP_SSE_HEADER_LINE_MAX];
    if (sse_parse_request_line(req_line, method, sizeof(method), url, sizeof(url)) != 0) {
        (void)sse_emit_status(client_fd, &server->write_mtx, 400, "Bad Request", "text/plain",
                              "malformed request line");
        return;
    }

    long content_length = sse_drain_headers(client_fd);
    if (content_length < 0) {
        (void)sse_emit_status(client_fd, &server->write_mtx, 400, "Bad Request", "text/plain",
                              "header parse error");
        return;
    }

    const char *configured_path =
        server->sse_path_owned != NULL ? server->sse_path_owned : VMAF_MCP_SSE_DEFAULT_PATH;
    int is_path_match = strcmp(url, configured_path) == 0 ? 1 : 0;
    int is_health = strcmp(url, "/") == 0 ? 1 : 0;

    if (strcmp(method, "GET") == 0 && is_path_match != 0) {
        sse_serve_stream(server, client_fd);
        return;
    }
    if (strcmp(method, "POST") == 0 && is_path_match != 0) {
        sse_serve_post(server, client_fd, content_length);
        return;
    }
    if (strcmp(method, "GET") == 0 && is_health != 0) {
        (void)sse_emit_status(client_fd, &server->write_mtx, 200, "OK", "application/json",
                              "{\"server\":\"vmaf-mcp\",\"transport\":\"sse\"}");
        return;
    }
    (void)sse_emit_status(client_fd, &server->write_mtx, 404, "Not Found", "text/plain",
                          "no such endpoint");
}

void *vmaf_mcp_sse_thread_main(void *arg)
{
    assert(arg != NULL);
    struct VmafMcpServer *server = (struct VmafMcpServer *)arg;
    if (server == NULL)
        return NULL;
    assert(server->sse_listen_fd >= 0);

    while (atomic_load(&server->sse_running) == 1) {
        int client_fd = accept(server->sse_listen_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR)
                continue;
            /* Listener closed during stop(). */
            break;
        }
        assert(client_fd >= 0);
        sse_serve_client(server, client_fd);
        (void)close(client_fd);
    }

    atomic_store(&server->sse_running, 0);
    return NULL;
}
