/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
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

/* Read up to `max_len - 1` bytes from `fd` into `buf` until LF or EOF.
 * NUL-terminates. Returns:
 *   > 0  : number of bytes consumed (excluding terminating LF/NUL).
 *   0    : EOF before any byte read.
 *   -1   : error (errno set).
 *   -2   : line too long (> max_len - 1 bytes before LF).
 */
static ssize_t read_line(int fd, char *buf, size_t max_len)
{
    if (max_len < 2u)
        return -1;
    size_t n = 0u;
    for (;;) {
        if (n >= max_len - 1u)
            return -2;
        char c = 0;
        /* False positive — the analyzer's inter-procedural lock
         * tracking gets confused when the worker loop calls
         * write_all_with_newline() (which acquires + releases
         * server->write_mtx and returns) on a previous iteration
         * before re-entering read_line() on the next iteration.
         * No mutex is held across this read. */
        /* NOLINTNEXTLINE(clang-analyzer-unix.BlockInCriticalSection) — ADR-0141 / ADR-0209 / ADR-0278 */
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
            continue; /* Tolerate CRLF. */
        buf[n++] = c;
    }
}

/* Write the entire buffer + a single trailing LF. Robust against
 * partial writes; respects EINTR. Returns 0 on success, -errno on
 * failure. */
static int write_all_with_newline(int fd, pthread_mutex_t *mtx, const char *buf, size_t len)
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

void *vmaf_mcp_stdio_thread_main(void *arg)
{
    struct VmafMcpServer *server = (struct VmafMcpServer *)arg;
    if (server == NULL)
        return NULL;
    /* Power-of-10 §5: post-guard — `server` is non-NULL hereafter
     * and the per-call invariants below (fd_in valid, running ==
     * 1) hold by construction at thread spawn-time. */
    assert(server != NULL);
    assert(server->stdio_fd_in >= 0);

    char *line = (char *)malloc(VMAF_MCP_MAX_LINE_BYTES);
    if (line == NULL) {
        atomic_store(&server->stdio_running, 0);
        return NULL;
    }
    /* Power-of-10 §5: scratch is bounded — VMAF_MCP_MAX_LINE_BYTES
     * is a compile-time constant per mcp_internal.h. */
    assert(line != NULL);

    while (atomic_load(&server->stdio_running) == 1) {
        ssize_t n = read_line(server->stdio_fd_in, line, VMAF_MCP_MAX_LINE_BYTES);
        if (n == 0)
            break; /* EOF — clean shutdown. */
        if (n == -1)
            break; /* Read error — exit thread. */
        if (n == -2) {
            /* Line too long — emit a parse-error response and skip. */
            const char overflow[] = "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32700,"
                                    "\"message\":\"request exceeds 64 KiB line limit\"}}";
            (void)write_all_with_newline(server->stdio_fd_out, &server->write_mtx, overflow,
                                         sizeof(overflow) - 1u);
            /* Drain bytes to next LF before resuming. The analyzer's
             * inter-procedural lock tracking flags the read below
             * as "in critical section" because it sees the
             * write_all_with_newline() above and assumes the lock
             * is still held; in fact the lock is released before
             * that function returns. */
            for (;;) {
                char c = 0;
                /* NOLINTNEXTLINE(clang-analyzer-unix.BlockInCriticalSection) — ADR-0141 / ADR-0209 / ADR-0278 */
                ssize_t r = read(server->stdio_fd_in, &c, 1);
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
            continue; /* Empty line — ignore. */

        char *response = NULL;
        int rc = vmaf_mcp_dispatch(server, line, &response);
        if (rc != 0 && response == NULL) {
            /* Dispatcher couldn't even build an error envelope — log
             * via stderr is unavailable here; just skip. The remote
             * will time-out and reconnect. */
            continue;
        }
        if (response != NULL) {
            (void)write_all_with_newline(server->stdio_fd_out, &server->write_mtx, response,
                                         strlen(response));
            free(response);
        }
    }

    free(line);
    atomic_store(&server->stdio_running, 0);
    return NULL;
}
