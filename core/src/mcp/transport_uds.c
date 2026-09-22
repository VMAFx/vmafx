/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  Unix-domain-socket transport for the embedded MCP server (v2).
 *
 *  Wire framing: newline-delimited JSON-RPC, identical to the
 *  stdio transport. One client at a time — the listener accept()s
 *  the next connection only after the current one closes. This is
 *  intentional for the embedded use case (single host driver per
 *  measurement run); future v3 may add per-client threads.
 *
 *  Auth surface (per ADR-0128 § "Operational guardrails"):
 *      - Socket file is mode 0700 (set in mcp.c after bind()).
 *      - No additional auth on top: filesystem permissions are
 *        the only access control. Hosts that share the same uid
 *        are inside the trust boundary.
 *
 *  Power-of-10 conformance:
 *      - rule 2: every loop is bounded — the per-client read loop
 *        caps lines at VMAF_MCP_MAX_LINE_BYTES; the listener loop
 *        is bounded by the running flag (transport_stdio.c-style).
 *      - rule 3: per-line scratch is allocated once per accepted
 *        connection; per-request cJSON allocations are bounded by
 *        the parser's input length.
 *      - rule 7: every accept()/read()/write()/dispatch return
 *        value is checked or `(void)`-cast.
 */

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "mcp_internal.h"

#define VMAF_MCP_UDS_IO_RETRY_LIMIT 64u

/* Read up to `max_len - 1` bytes from `fd` into `buf` until LF or
 * EOF. Returns: > 0 = bytes consumed, 0 = EOF, -1 = error,
 * -2 = line too long. NUL-terminates. */
static ssize_t uds_read_line(int fd, char *buf, size_t max_len)
{
    if (max_len < 2u) {
        return -1;
    }
    size_t n = 0u;
    size_t consumed = 0u;
    size_t interruptions = 0u;
    while (consumed < max_len - 1u) {
        char c = 0;
        const ssize_t r = read(fd, &c, 1);
        if (r == 0) {
            if (n == 0u) {
                return 0;
            }
            buf[n] = '\0';
            return (ssize_t)n;
        }
        if (r < 0) {
            if (errno == EINTR && interruptions < VMAF_MCP_UDS_IO_RETRY_LIMIT) {
                interruptions++;
                continue;
            }
            return -1;
        }
        interruptions = 0u;
        consumed++;
        if (c == '\n') {
            buf[n] = '\0';
            return (ssize_t)n;
        }
        if (c == '\r') {
            continue;
        }
        buf[n++] = c;
    }
    return -2;
}

static int uds_write_all(int fd, const char *buf, size_t len)
{
    size_t off = 0u;
    size_t interruptions = 0u;
    while (off < len) {
        const ssize_t w = write(fd, buf + off, len - off);
        if (w < 0) {
            if (errno == EINTR && interruptions < VMAF_MCP_UDS_IO_RETRY_LIMIT) {
                interruptions++;
                continue;
            }
            return -errno;
        }
        if (w == 0) {
            return -EIO;
        }
        interruptions = 0u;
        off += (size_t)w;
    }
    return 0;
}

/* Write `len` bytes + a trailing LF, looped against partial
 * writes / EINTR. Holds the server's write-mutex for the duration
 * (mirrors transport_stdio.c so a future multi-transport host
 * can interleave responses without corruption). */
static int uds_write_all_with_newline(int fd, pthread_mutex_t *mtx, const char *buf, size_t len)
{
    const int lock_rc = pthread_mutex_lock(mtx);
    if (lock_rc != 0) {
        return -lock_rc;
    }
    int rc = uds_write_all(fd, buf, len);
    const char nl = '\n';
    if (rc == 0) {
        rc = uds_write_all(fd, &nl, 1u);
    }
    const int unlock_rc = pthread_mutex_unlock(mtx);
    if (unlock_rc != 0 && rc == 0) {
        rc = -unlock_rc;
    }
    return rc;
}

static int uds_drain_overlong_line(int fd)
{
    size_t consumed = 0u;
    size_t interruptions = 0u;
    while (consumed < VMAF_MCP_MAX_LINE_BYTES) {
        char c = 0;
        const ssize_t r = read(fd, &c, 1);
        if (r == 0 || (r == 1 && c == '\n')) {
            return 0;
        }
        if (r < 0) {
            if (errno == EINTR && interruptions < VMAF_MCP_UDS_IO_RETRY_LIMIT) {
                interruptions++;
                continue;
            }
            return -errno;
        }
        interruptions = 0u;
        consumed++;
    }
    return -E2BIG;
}

static int uds_report_and_drain_overlong_line(struct VmafMcpServer *server, int client_fd)
{
    static const char overflow[] = "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32700,"
                                   "\"message\":\"request exceeds 64 KiB line limit\"}}";
    const int write_rc =
        uds_write_all_with_newline(client_fd, &server->write_mtx, overflow, sizeof(overflow) - 1u);
    if (write_rc != 0) {
        return write_rc;
    }
    return uds_drain_overlong_line(client_fd);
}

static int uds_dispatch_line(struct VmafMcpServer *server, int client_fd, const char *line)
{
    char *response = nullptr;
    const int dispatch_rc = vmaf_mcp_dispatch(server, line, &response);
    if (dispatch_rc != 0 && response == nullptr) {
        return 0;
    }
    int write_rc = 0;
    if (response != nullptr) {
        write_rc =
            uds_write_all_with_newline(client_fd, &server->write_mtx, response, strlen(response));
        free(response);
    }
    return write_rc;
}

/* Service one accepted client end-to-end. Returns when EOF or
 * error closes the connection. */
static void serve_client(struct VmafMcpServer *server, int client_fd)
{
    char *line = (char *)malloc(VMAF_MCP_MAX_LINE_BYTES);
    if (line == nullptr) {
        return;
    }

    while (atomic_load(&server->uds_running) == 1) {
        const ssize_t n = uds_read_line(client_fd, line, VMAF_MCP_MAX_LINE_BYTES);
        if (n == 0) {
            break; /* EOF. */
        }
        if (n == -1) {
            break; /* read() error. */
        }
        if (n == -2) {
            if (uds_report_and_drain_overlong_line(server, client_fd) != 0) {
                break;
            }
            continue;
        }
        if (uds_dispatch_line(server, client_fd, line) != 0) {
            break;
        }
    }

    free(line);
}

void *vmaf_mcp_uds_thread_main(void *arg)
{
    assert(arg != nullptr);
    struct VmafMcpServer *server = (struct VmafMcpServer *)arg;
    if (server == nullptr) {
        return nullptr;
    }
    if (atomic_load(&server->uds_running) != 1) {
        return nullptr;
    }
    const int listen_fd = server->uds_listen_fd;
    if (listen_fd < 0) {
        return nullptr;
    }

    while (atomic_load(&server->uds_running) == 1) {
        const int client_fd = accept(listen_fd, nullptr, nullptr);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            /* Listener was closed during stop(); exit cleanly. */
            break;
        }
        assert(client_fd >= 0);
        serve_client(server, client_fd);
        (void)close(client_fd);
    }

    atomic_store(&server->uds_running, 0);
    return nullptr;
}
