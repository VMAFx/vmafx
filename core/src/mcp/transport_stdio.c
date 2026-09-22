/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  stdio transport for the embedded MCP server. Reads
 *  newline-delimited JSON-RPC requests from `cfg->fd_in`, writes
 *  newline-delimited JSON-RPC responses to `cfg->fd_out`.
 *
 *  Power-of-10 conformance:
 *      - Bounded read loop: every accepted line is capped at
 *        VMAF_MCP_MAX_LINE_BYTES (rule 2).
 *      - One per-line scratch buffer is malloc'd up-front; per-
 *        request cJSON allocations are inside the dispatcher and
 *        bounded by the parser's input length (rule 3).
 *      - All return values from read()/write()/dispatcher checked
 *        (rule 7).
 */

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mcp_internal.h"

#define VMAF_MCP_IO_RETRY_LIMIT 64u

/* Read up to `max_len - 1` bytes from `fd` into `buf` until LF or EOF.
 * NUL-terminates. Returns:
 *   > 0  : number of bytes consumed (excluding terminating LF/NUL).
 *   0    : EOF before any byte read.
 *   -1   : error (errno set).
 *   -2   : line too long (> max_len - 1 bytes before LF).
 */
static ssize_t read_line(int fd, char *buf, size_t max_len)
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
            if (errno == EINTR && interruptions < VMAF_MCP_IO_RETRY_LIMIT) {
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
            continue; /* Tolerate CRLF. */
        }
        buf[n++] = c;
    }
    return -2;
}

static int write_all(int fd, const char *buf, size_t len)
{
    size_t off = 0u;
    size_t interruptions = 0u;
    while (off < len) {
        const ssize_t w = write(fd, buf + off, len - off);
        if (w < 0) {
            if (errno == EINTR && interruptions < VMAF_MCP_IO_RETRY_LIMIT) {
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

/* Write the entire buffer + a single trailing LF. Robust against
 * partial writes; respects EINTR. Returns 0 on success, -errno on
 * failure. */
static int write_all_with_newline(int fd, pthread_mutex_t *mtx, const char *buf, size_t len)
{
    const int lock_rc = pthread_mutex_lock(mtx);
    if (lock_rc != 0) {
        return -lock_rc;
    }
    int rc = write_all(fd, buf, len);
    const char nl = '\n';
    if (rc == 0) {
        rc = write_all(fd, &nl, 1u);
    }
    const int unlock_rc = pthread_mutex_unlock(mtx);
    if (unlock_rc != 0 && rc == 0) {
        rc = -unlock_rc;
    }
    return rc;
}

static int drain_overlong_line(int fd)
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
            if (errno == EINTR && interruptions < VMAF_MCP_IO_RETRY_LIMIT) {
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

static int report_and_drain_overlong_line(struct VmafMcpServer *server)
{
    static const char overflow[] = "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32700,"
                                   "\"message\":\"request exceeds 64 KiB line limit\"}}";
    const int write_rc = write_all_with_newline(server->stdio_fd_out, &server->write_mtx, overflow,
                                                sizeof(overflow) - 1u);
    if (write_rc != 0) {
        return write_rc;
    }
    return drain_overlong_line(server->stdio_fd_in);
}

static int dispatch_line(struct VmafMcpServer *server, const char *line)
{
    char *response = nullptr;
    const int dispatch_rc = vmaf_mcp_dispatch(server, line, &response);
    if (dispatch_rc != 0 && response == nullptr) {
        return 0;
    }
    int write_rc = 0;
    if (response != nullptr) {
        write_rc = write_all_with_newline(server->stdio_fd_out, &server->write_mtx, response,
                                          strlen(response));
        free(response);
    }
    return write_rc;
}

void *vmaf_mcp_stdio_thread_main(void *arg)
{
    struct VmafMcpServer *server = (struct VmafMcpServer *)arg;
    if (server == nullptr)
        return nullptr;
    /* Power-of-10 §5: post-guard — `server` is non-null hereafter
     * and the per-call invariants below (fd_in valid, running ==
     * 1) hold by construction at thread spawn-time. */
    assert(server != nullptr);
    assert(server->stdio_fd_in >= 0);

    char *line = (char *)malloc(VMAF_MCP_MAX_LINE_BYTES);
    if (line == nullptr) {
        atomic_store(&server->stdio_running, 0);
        return nullptr;
    }
    /* Power-of-10 §5: scratch is bounded — VMAF_MCP_MAX_LINE_BYTES
     * is a compile-time constant per mcp_internal.h. */
    assert(line != nullptr);

    while (atomic_load(&server->stdio_running) == 1) {
        const ssize_t n = read_line(server->stdio_fd_in, line, VMAF_MCP_MAX_LINE_BYTES);
        if (n == 0) {
            break; /* EOF — clean shutdown. */
        }
        if (n == -1) {
            break; /* Read error — exit thread. */
        }
        if (n == -2) {
            if (report_and_drain_overlong_line(server) != 0) {
                break;
            }
            continue;
        }
        if (dispatch_line(server, line) != 0) {
            break;
        }
    }

    free(line);
    atomic_store(&server->stdio_running, 0);
    return nullptr;
}
