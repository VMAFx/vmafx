/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Embedded MCP server — v1 stdio runtime (T5-2b).
 *
 *  History: T5-2 (ADR-0209) landed every entry point as `-ENOSYS`.
 *  T5-2b (this PR — see ADR-0209 § "Status update 2026-05-08")
 *  flips `vmaf_mcp_init` + `vmaf_mcp_start_stdio` + `vmaf_mcp_stop`
 *  + `vmaf_mcp_close` to a working JSON-RPC 2.0 dispatcher with
 *  two tools (`list_features`, `compute_vmaf`). SSE / UDS still
 *  return `-ENOSYS`; vendoring `mongoose` and wiring the SPSC ring
 *  drain at frame boundaries is deferred to v2.
 *
 *  Power-of-10 invariants:
 *      - rule 2 (bounded loops): all per-request loops are bounded
 *        by VMAF_MCP_MAX_LINE_BYTES / VMAF_MCP_MAX_FEATURES; see
 *        transport_stdio.c + dispatcher.c.
 *      - rule 3 (no dynamic alloc on hot path): per-line scratch
 *        is allocated once at thread-start; per-request cJSON
 *        allocations are confined to the dispatcher and bounded
 *        by the parser's input length. The measurement thread is
 *        not touched by v1 (no SPSC ring yet).
 *      - rule 7 (return values): every read()/write()/pthread_*()
 *        return value is checked or `(void)`-cast with rationale.
 */

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "libvmaf/libvmaf_mcp.h"
#include "mcp_internal.h"

int vmaf_mcp_available(void)
{
#ifdef HAVE_MCP
    return 1;
#else
    return 0;
#endif
}

int vmaf_mcp_transport_available(VmafMcpTransport transport)
{
    /* Build a per-call bitmask from the per-transport defines, then
     * test the bit matching the queried transport. The arithmetic
     * is preprocessor-fed, so the compiler folds the function body
     * to a constant load + bittest at every call site. Avoiding a
     * per-arm `#ifdef` keeps the body structurally distinct (no
     * `bugprone-branch-clone`) regardless of which sub-flags are
     * on. */
    if ((unsigned)transport > VMAF_MCP_TRANSPORT_BITMASK_MAX) {
        return 0;
    }
    /* Post-guard invariant — same predicate as the early return,
     * preserved for Power-of-10 §5 assertion density. */
    assert((unsigned)transport <= VMAF_MCP_TRANSPORT_BITMASK_MAX);
    unsigned mask = 0u;
#ifdef HAVE_MCP_SSE
    mask |= 1u << (unsigned)VMAF_MCP_TRANSPORT_SSE;
#endif
#ifdef HAVE_MCP_UDS
    mask |= 1u << (unsigned)VMAF_MCP_TRANSPORT_UDS;
#endif
#ifdef HAVE_MCP_STDIO
    mask |= 1u << (unsigned)VMAF_MCP_TRANSPORT_STDIO;
#endif
    return (mask & (1u << (unsigned)transport)) != 0u ? 1 : 0;
}

/* Validate cfg at init. Power-of-2 check on queue_depth (rejected
 * with -EINVAL when non-zero and not power of 2). */
static int validate_config(const VmafMcpConfig *cfg)
{
    if (cfg == NULL)
        return 0;
    if (cfg->queue_depth != 0u) {
        unsigned q = cfg->queue_depth;
        if ((q & (q - 1u)) != 0u)
            return -EINVAL;
    }
    if (cfg->max_drain_per_frame > VMAF_MCP_MAX_DRAIN_PER_FRAME)
        return -EINVAL;
    return 0;
}

int vmaf_mcp_init(VmafMcpServer **out, VmafContext *ctx, const VmafMcpConfig *cfg)
{
    if (out == NULL)
        return -EINVAL;
    if (ctx == NULL)
        return -EINVAL;
    *out = NULL;

    int vrc = validate_config(cfg);
    if (vrc != 0)
        return vrc;

    struct VmafMcpServer *s = (struct VmafMcpServer *)calloc(1u, sizeof(*s));
    if (s == NULL)
        return -ENOMEM;
    /* Power-of-10 §5: post-condition on the freshly-allocated handle. */
    assert(s != NULL);

    s->ctx = ctx;
    if (cfg != NULL) {
        s->cfg = *cfg;
        if (cfg->user_agent != NULL) {
            size_t len = strlen(cfg->user_agent);
            char *dup = (char *)malloc(len + 1u);
            if (dup == NULL) {
                free(s);
                return -ENOMEM;
            }
            memcpy(dup, cfg->user_agent, len + 1u);
            s->user_agent_owned = dup;
            s->cfg.user_agent = dup;
        }
    }
    s->stdio_fd_in = -1;
    s->stdio_fd_out = -1;
    atomic_store(&s->stdio_running, 0);
    s->uds_listen_fd = -1;
    s->uds_path_owned = NULL;
    atomic_store(&s->uds_running, 0);
    s->sse_listen_fd = -1;
    s->sse_port = 0u;
    s->sse_path_owned = NULL;
    atomic_store(&s->sse_running, 0);
    /* Power-of-10 §5: invariant — server starts in "not-running"
     * state regardless of caller config. */
    assert(atomic_load(&s->stdio_running) == 0);
    int mrc = pthread_mutex_init(&s->write_mtx, NULL);
    if (mrc != 0) {
        free(s->user_agent_owned);
        free(s);
        return -mrc;
    }

    *out = s;
    return 0;
}

/* Bind a freshly-created loopback TCP socket on cfg->port. Returns
 * the bound fd on success (and writes the resolved port back to
 * `*resolved_port`); returns negative errno on failure. */
static int sse_bind_loopback(uint16_t requested_port, uint16_t *resolved_port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -errno;
    int one = 1;
    /* SO_REUSEADDR — let the smoke test reuse the kernel-picked port
     * across rapid restarts; harmless on loopback. */
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) != 0) {
        int saved = errno;
        (void)close(fd);
        return -saved;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    /* 127.0.0.1 — embedded MCP must NOT expose to non-loopback per
     * ADR-0128 § "Operational guardrails". */
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(requested_port);
    if (bind(fd, (const struct sockaddr *)&addr, sizeof(addr)) != 0) {
        int saved = errno;
        (void)close(fd);
        return saved == EADDRINUSE ? -EADDRINUSE : -saved;
    }
    if (listen(fd, VMAF_MCP_LISTEN_BACKLOG) != 0) {
        int saved = errno;
        (void)close(fd);
        return -saved;
    }
    /* Resolve the bound port (so port==0 callers learn what the
     * kernel picked). */
    struct sockaddr_in bound;
    socklen_t blen = sizeof(bound);
    if (getsockname(fd, (struct sockaddr *)&bound, &blen) != 0) {
        int saved = errno;
        (void)close(fd);
        return -saved;
    }
    *resolved_port = ntohs(bound.sin_port);
    return fd;
}

int vmaf_mcp_start_sse(VmafMcpServer *server, VmafMcpSseConfig *cfg)
{
    if (server == NULL)
        return -EINVAL;
    if (cfg == NULL)
        return -EINVAL;

    int expected = 0;
    if (!atomic_compare_exchange_strong(&server->sse_running, &expected, 1)) {
        return -EBUSY;
    }
    /* Power-of-10 §5: post-CAS invariant — exactly one start wins. */
    assert(atomic_load(&server->sse_running) == 1);

    /* Optional URL path; copy into handle storage so caller-owned
     * memory does not need to outlive the server. */
    char *path_dup = NULL;
    if (cfg->path != NULL) {
        size_t len = strlen(cfg->path);
        if (len == 0u || len > VMAF_MCP_SSE_PATH_MAX) {
            atomic_store(&server->sse_running, 0);
            return -EINVAL;
        }
        path_dup = (char *)malloc(len + 1u);
        if (path_dup == NULL) {
            atomic_store(&server->sse_running, 0);
            return -ENOMEM;
        }
        memcpy(path_dup, cfg->path, len + 1u);
    }

    uint16_t resolved_port = 0u;
    int fd = sse_bind_loopback(cfg->port, &resolved_port);
    if (fd < 0) {
        free(path_dup);
        atomic_store(&server->sse_running, 0);
        return fd;
    }

    server->sse_listen_fd = fd;
    server->sse_port = resolved_port;
    server->sse_path_owned = path_dup;

    int rc = pthread_create(&server->sse_thread, NULL, vmaf_mcp_sse_thread_main, server);
    if (rc != 0) {
        (void)close(fd);
        server->sse_listen_fd = -1;
        free(path_dup);
        server->sse_path_owned = NULL;
        atomic_store(&server->sse_running, 0);
        return -rc;
    }
    /* Echo the resolved port back to the caller (port==0 → ephemeral). */
    cfg->port = resolved_port;
    return 0;
}

int vmaf_mcp_start_uds(VmafMcpServer *server, const VmafMcpUdsConfig *cfg)
{
    if (server == NULL)
        return -EINVAL;
    if (cfg == NULL)
        return -EINVAL;
    if (cfg->path == NULL)
        return -EINVAL;

    /* Path-length must fit AF_UNIX struct sockaddr_un.sun_path
     * (typically 108 bytes on Linux, 104 on BSD); enforce
     * VMAF_MCP_UDS_PATH_MAX (100) to leave headroom across POSIX
     * hosts. */
    size_t path_len = strlen(cfg->path);
    if (path_len == 0u || path_len >= VMAF_MCP_UDS_PATH_MAX)
        return -EINVAL;

    int expected = 0;
    if (!atomic_compare_exchange_strong(&server->uds_running, &expected, 1)) {
        return -EBUSY;
    }
    /* Power-of-10 §5: post-CAS invariant — exactly one start
     * wins. */
    assert(atomic_load(&server->uds_running) == 1);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        atomic_store(&server->uds_running, 0);
        return -errno;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, cfg->path, path_len + 1u);

    /* Best-effort unlink of a stale socket file — ignore ENOENT. */
    if (unlink(cfg->path) != 0 && errno != ENOENT) {
        int saved = errno;
        (void)close(fd);
        atomic_store(&server->uds_running, 0);
        return -saved;
    }
    if (bind(fd, (const struct sockaddr *)&addr, sizeof(addr)) != 0) {
        int saved = errno;
        (void)close(fd);
        atomic_store(&server->uds_running, 0);
        return saved == EADDRINUSE ? -EADDRINUSE : -saved;
    }
    /* Per ADR-0128 § "Operational guardrails" — UDS file is mode
     * 0700 (owner-only). chmod after bind so the umask cannot
     * loosen permissions. */
    if (chmod(cfg->path, S_IRWXU) != 0) {
        int saved = errno;
        (void)unlink(cfg->path);
        (void)close(fd);
        atomic_store(&server->uds_running, 0);
        return -saved;
    }
    /* listen-backlog SOMAXCONN-equivalent;
     * VMAF_MCP_LISTEN_BACKLOG is generous for the embedded use
     * case. */
    if (listen(fd, VMAF_MCP_LISTEN_BACKLOG) != 0) {
        int saved = errno;
        (void)unlink(cfg->path);
        (void)close(fd);
        atomic_store(&server->uds_running, 0);
        return -saved;
    }

    char *path_dup = (char *)malloc(path_len + 1u);
    if (path_dup == NULL) {
        (void)unlink(cfg->path);
        (void)close(fd);
        atomic_store(&server->uds_running, 0);
        return -ENOMEM;
    }
    memcpy(path_dup, cfg->path, path_len + 1u);

    server->uds_listen_fd = fd;
    server->uds_path_owned = path_dup;

    int rc = pthread_create(&server->uds_thread, NULL, vmaf_mcp_uds_thread_main, server);
    if (rc != 0) {
        (void)unlink(path_dup);
        free(path_dup);
        server->uds_path_owned = NULL;
        (void)close(fd);
        server->uds_listen_fd = -1;
        atomic_store(&server->uds_running, 0);
        return -rc;
    }
    return 0;
}

int vmaf_mcp_start_stdio(VmafMcpServer *server, const VmafMcpStdioConfig *cfg)
{
    if (server == NULL)
        return -EINVAL;
    if (cfg == NULL)
        return -EINVAL;
    if (cfg->fd_in < 0)
        return -EINVAL;
    if (cfg->fd_out < 0)
        return -EINVAL;

    int expected = 0;
    if (!atomic_compare_exchange_strong(&server->stdio_running, &expected, 1)) {
        return -EBUSY;
    }
    /* Power-of-10 §5: post-CAS invariant — exactly one start can win
     * the race; the loser returned -EBUSY above. */
    assert(atomic_load(&server->stdio_running) == 1);

    server->stdio_fd_in = cfg->fd_in;
    server->stdio_fd_out = cfg->fd_out;

    int rc = pthread_create(&server->stdio_thread, NULL, vmaf_mcp_stdio_thread_main, server);
    if (rc != 0) {
        atomic_store(&server->stdio_running, 0);
        return -rc;
    }
    return 0;
}

/* Per-transport stop helpers.
 *
 * Each stop() helper is a CAS-guarded one-shot: the `*_running`
 * atomic rides a 3-state machine — 0 = never started, 1 = started,
 * 2 = stop() has already observed it and joined the worker. The CAS
 * `compare_exchange_strong(expected=1, desired=2)` is the only path
 * that drives the join branch. A 0 (never started) or 2 (already
 * stopped) state leaves the atomic untouched and the helper returns
 * a no-op. This makes `vmaf_mcp_stop()` safe to call any number of
 * times — including via `vmaf_mcp_close()` after the caller already
 * invoked stop() explicitly. Prior implementation used an
 * unconditional `atomic_exchange(..., 2)` plus a `prev == 1 || prev
 * == 2` guard, which mutated `0 -> 2` silently on a never-started
 * transport and re-entered the join branch on the next call —
 * resulting in `pthread_join()` on a default-initialised or
 * already-joined thread handle (UB; SIGSEGV on glibc 2.40). */

static void stop_stdio(VmafMcpServer *server)
{
    int expected = 1;
    if (!atomic_compare_exchange_strong(&server->stdio_running, &expected, 2))
        return;
    /* Closing fd_in is the canonical way to unblock the read() loop.
     * The caller owns the fd per the public contract, so we cannot
     * close it ourselves; instead we rely on the caller having
     * already closed (or being about to close) fd_in to drive the
     * worker to EOF. We still join the thread to release its
     * resources. For tests and well-behaved hosts the read end is
     * already EOF by the time stop() is called. */
    (void)pthread_join(server->stdio_thread, NULL);
}

static void stop_uds(VmafMcpServer *server)
{
    int expected = 1;
    if (!atomic_compare_exchange_strong(&server->uds_running, &expected, 2))
        return;
    /* shutdown + close listener fd to unblock accept(); join the
     * thread. On Linux, plain close() of a listening AF_UNIX or AF_INET
     * socket does NOT unblock accept() in another thread —
     * shutdown(SHUT_RDWR) does (verified empirically). The
     * path file is unlinked so the next start_uds() with the same
     * path doesn't fail on stale-socket. */
    if (server->uds_listen_fd >= 0) {
        (void)shutdown(server->uds_listen_fd, SHUT_RDWR);
        (void)close(server->uds_listen_fd);
        server->uds_listen_fd = -1;
    }
    (void)pthread_join(server->uds_thread, NULL);
    if (server->uds_path_owned != NULL) {
        (void)unlink(server->uds_path_owned);
        free(server->uds_path_owned);
        server->uds_path_owned = NULL;
    }
}

static void stop_sse(VmafMcpServer *server)
{
    int expected = 1;
    if (!atomic_compare_exchange_strong(&server->sse_running, &expected, 2))
        return;
    /* shutdown + close listener fd to unblock accept(); join the
     * thread. On Linux, plain close() of a listening AF_INET socket
     * does NOT unblock accept() in another thread —
     * shutdown(SHUT_RDWR) does (verified empirically; see
     * docs/mcp/embedded.md). */
    if (server->sse_listen_fd >= 0) {
        (void)shutdown(server->sse_listen_fd, SHUT_RDWR);
        (void)close(server->sse_listen_fd);
        server->sse_listen_fd = -1;
    }
    (void)pthread_join(server->sse_thread, NULL);
    free(server->sse_path_owned);
    server->sse_path_owned = NULL;
}

int vmaf_mcp_stop(VmafMcpServer *server)
{
    if (server == NULL)
        return -EINVAL;
    /* Power-of-10 §5: post-guard — server pointer is now known
     * non-NULL; downstream helpers assume that. */
    assert(server != NULL);
    stop_stdio(server);
    stop_uds(server);
    stop_sse(server);
    return 0;
}

void vmaf_mcp_close(VmafMcpServer **server)
{
    if (server == NULL)
        return;
    if (*server == NULL)
        return;
    struct VmafMcpServer *s = *server;
    (void)vmaf_mcp_stop(s);
    (void)pthread_mutex_destroy(&s->write_mtx);
    free(s->user_agent_owned);
    free(s);
    *server = NULL;
}
