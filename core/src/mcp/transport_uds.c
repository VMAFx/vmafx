/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
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

/* Read up to `max_len - 1` bytes from `fd` into `buf` until LF or
 * EOF. Returns: > 0 = bytes consumed, 0 = EOF, -1 = error,
 * -2 = line too long. NUL-terminates. */
static ssize_t uds_read_line(int fd, char *buf, size_t max_len)
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

/* Write `len` bytes + a trailing LF, looped against partial
 * writes / EINTR. Holds the server's write-mutex for the duration
 * (mirrors transport_stdio.c so a future multi-transport host
 * can interleave responses without corruption). */
static int uds_write_all_with_newline(int fd, pthread_mutex_t *mtx, const char *buf, size_t len)
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
            goto unlock;
        }
        off += (size_t)w;
    }
    const char nl = '\n';
    while (1) {
        ssize_t w = write(fd, &nl, 1);
        if (w == 1)
            break;
        if (w < 0 && errno == EINTR)
            continue;
        rc = w < 0 ? -errno : -EIO;
        goto unlock;
    }
unlock:;
    int unlock_rc = pthread_mutex_unlock(mtx);
    if (unlock_rc != 0 && rc == 0)
        rc = -unlock_rc;
    return rc;
}

/* Service one accepted client end-to-end. Returns when EOF or
 * error closes the connection. */
static void serve_client(struct VmafMcpServer *server, int client_fd)
{
    char *line = (char *)malloc(VMAF_MCP_MAX_LINE_BYTES);
    if (line == NULL)
        return;

    while (atomic_load(&server->uds_running) == 1) {
        ssize_t n = uds_read_line(client_fd, line, VMAF_MCP_MAX_LINE_BYTES);
        if (n == 0)
            break; /* EOF. */
        if (n == -1)
            break; /* read() error. */
        if (n == -2) {
            const char overflow[] = "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32700,"
                                    "\"message\":\"request exceeds 64 KiB line limit\"}}";
            (void)uds_write_all_with_newline(client_fd, &server->write_mtx, overflow,
                                             sizeof(overflow) - 1u);
            for (;;) {
                char c = 0;
                ssize_t r = read(client_fd, &c, 1);
                if (r < 0) {
                    if (errno == EINTR)
                        continue;
                    break;
                }
                if (r == 0 || c == '\n')
                    break;
            }
            continue;
        }
        if (n == 0)
            continue;

        char *response = NULL;
        int rc = vmaf_mcp_dispatch(server, line, &response);
        if (rc != 0 && response == NULL)
            continue;
        if (response != NULL) {
            (void)uds_write_all_with_newline(client_fd, &server->write_mtx, response,
                                             strlen(response));
            free(response);
        }
    }

    free(line);
}

void *vmaf_mcp_uds_thread_main(void *arg)
{
    assert(arg != NULL);
    struct VmafMcpServer *server = (struct VmafMcpServer *)arg;
    if (server == NULL)
        return NULL;
    assert(server->uds_listen_fd >= 0);

    while (atomic_load(&server->uds_running) == 1) {
        int client_fd = accept(server->uds_listen_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR)
                continue;
            /* Listener was closed during stop(); exit cleanly. */
            break;
        }
        assert(client_fd >= 0);
        serve_client(server, client_fd);
        (void)close(client_fd);
    }

    atomic_store(&server->uds_running, 0);
    return NULL;
}
