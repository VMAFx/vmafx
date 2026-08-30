/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Licensed under the BSD+Patent License (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      https://opensource.org/licenses/BSDplusPatent
 */

/**
 * @file libvmaf_mcp.h
 * @brief Embedded MCP (Model Context Protocol) server public API.
 *
 * Scaffolded by ADR-0209 / T5-2 (audit-first). Runtime (cJSON +
 * mongoose vendoring, dedicated MCP pthread, SPSC ring buffer,
 * SSE / UDS / stdio transport bodies) lands via T5-2b in a
 * follow-up PR. The header surface here is stable for downstream
 * consumers: every entry point returns -ENOSYS while the runtime
 * is unwired, so a caller compiled against this header sees a
 * predictable error and can fall back to the external Python MCP
 * server under mcp-server/vmaf-mcp/.
 *
 * When libvmaf was built without `-Denable_mcp=true`, every entry
 * point still returns -ENOSYS unconditionally; the linker resolves
 * the symbols against the same stub TU at core/src/mcp/mcp.c.
 *
 * Threading model (per ADR-0128 + Research-0005):
 *   - The host calls `vmaf_mcp_init` after `vmaf_init` and before
 *     the first `vmaf_read_pictures`. Init pre-allocates the SPSC
 *     ring buffer; no further allocation crosses the measurement-
 *     thread boundary (NASA Power-of-10 rule 3).
 *   - One transport-start call (`_start_sse`, `_start_uds`,
 *     `_start_stdio`) per active transport — they may be combined.
 *     Each spawns a dedicated MCP pthread; the measurement thread
 *     drains at most N command envelopes per frame from the SPSC
 *     ring (bounded loop, NASA Power-of-10 rule 2).
 *   - `vmaf_mcp_stop` joins the MCP threads; `vmaf_mcp_close`
 *     releases the ring buffer + handle. Closing a NULL handle is
 *     a no-op.
 *
 * Auth surface (per ADR-0128 § "Operational guardrails"):
 *   - SSE binds to 127.0.0.1 only.
 *   - UDS uses filesystem mode 0700 on the socket file.
 *   - stdio is trusted by construction (host owns the fds).
 *
 * Error contract (negative errno):
 *   -ENOSYS — feature not built (or scaffold not yet wired).
 *   -ENODEV — transport-specific runtime unavailable
 *             (e.g. UDS on a non-POSIX host).
 *   -EINVAL — bad argument (NULL where required, malformed config).
 *   -ENOMEM — ring/buffer allocation failed at init.
 *   -EBUSY  — measurement already in flight (start refused).
 */

#ifndef LIBVMAF_MCP_H_
#define LIBVMAF_MCP_H_

#include <stddef.h>
#include <stdint.h>

#include <libvmaf/libvmaf.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Returns 1 if libvmaf was built with the embedded MCP server
 * (-Denable_mcp=true), 0 otherwise. Cheap to call; no MCP runtime
 * is touched until @ref vmaf_mcp_init().
 *
 * The umbrella flag is independent of the per-transport sub-flags
 * (`enable_mcp_sse`, `enable_mcp_uds`, `enable_mcp_stdio`); use
 * @ref vmaf_mcp_transport_available to query a specific transport.
 *
 * @return 1 if the MCP feature was compiled in, 0 otherwise.
 */
VMAF_EXPORT int vmaf_mcp_available(void);

/**
 * Transport identifiers — used by @ref vmaf_mcp_transport_available
 * and the per-transport `_start_*` entry points.
 */
typedef enum VmafMcpTransport {
    VMAF_MCP_TRANSPORT_SSE = 0,   /**< Server-Sent Events over loopback HTTP. */
    VMAF_MCP_TRANSPORT_UDS = 1,   /**< Unix domain socket, newline-delimited JSON-RPC. */
    VMAF_MCP_TRANSPORT_STDIO = 2, /**< LSP-framed JSON-RPC on caller-supplied fd pair. */
} VmafMcpTransport;

/**
 * Returns 1 if the per-transport sub-flag was enabled at build
 * time (e.g. `-Denable_mcp_sse=true`), 0 otherwise. Returns 0 for
 * unknown transport ids.
 *
 * @param transport Transport identifier to query.
 *
 * @return 1 if the transport was compiled in, 0 otherwise (also 0
 *         for unknown transport ids).
 */
VMAF_EXPORT int vmaf_mcp_transport_available(VmafMcpTransport transport);

/**
 * Opaque handle to an embedded MCP server. One handle pins one
 * SPSC ring buffer + zero-or-more transport threads. The handle is
 * created by @ref vmaf_mcp_init and released by
 * @ref vmaf_mcp_close.
 */
typedef struct VmafMcpServer VmafMcpServer;

/**
 * MCP server configuration — populated by the host before
 * @ref vmaf_mcp_init. POD struct; safe to zero-initialise.
 */
typedef struct VmafMcpConfig {
    /** SPSC ring slot count. 0 → default 64. Must be a power of
     *  two; rejected otherwise with -EINVAL. Slots are fixed-size
     *  (no heap-owned data crosses the boundary), pre-allocated at
     *  @ref vmaf_mcp_init time. */
    uint32_t queue_depth;
    /** Upper bound on command envelopes the measurement thread
     *  drains per frame (NASA Power-of-10 rule 2). 0 → default 4.
     *  Cap 64. */
    uint32_t max_drain_per_frame;
    /** Optional NUL-terminated tag returned in MCP `serverInfo`.
     *  NULL → libvmaf default. Caller retains ownership; the string
     *  is copied into the handle. */
    const char *user_agent;
} VmafMcpConfig;

/**
 * Initialise an embedded MCP server bound to a VmafContext. Must
 * be called after `vmaf_init` and before the first
 * `vmaf_read_pictures`. Pre-allocates the SPSC ring buffer; no
 * further allocation occurs on the measurement-thread hot path.
 *
 * @param out  Receives the new server handle. Caller owns it; pair
 *             with @ref vmaf_mcp_close.
 * @param ctx  VmafContext the server introspects + steers. The
 *             handle borrows the pointer for its lifetime; the
 *             caller must call @ref vmaf_mcp_close before
 *             vmaf_close().
 * @param cfg  Configuration. NULL → all-defaults.
 *
 * @return 0 on success, -ENOSYS when built without MCP (or while
 *         the runtime is still unwired in the scaffold), -EINVAL
 *         on bad arguments, -ENOMEM on ring allocation failure,
 *         -EBUSY if measurement is already in flight.
 */
VMAF_EXPORT int vmaf_mcp_init(VmafMcpServer **out, VmafContext *ctx, const VmafMcpConfig *cfg);

/**
 * SSE transport configuration. Populated by the host before
 * @ref vmaf_mcp_start_sse.
 */
typedef struct VmafMcpSseConfig {
    /** Loopback TCP port, in [1, 65535]. 0 → kernel-picked
     *  ephemeral port; on success the chosen port is written back
     *  into this field for the host to read. */
    uint16_t port;
    /** Optional URL path the SSE stream binds to. NULL → libvmaf
     *  default ("/mcp/sse"). Caller retains ownership; the string
     *  is copied. */
    const char *path;
} VmafMcpSseConfig;

/**
 * Start the SSE (Server-Sent Events) transport on a loopback
 * socket. Spawns one dedicated MCP pthread that owns the listener.
 * The transport refuses to bind to a non-loopback address.
 *
 * @param server  Server handle previously created via
 *                @ref vmaf_mcp_init.
 * @param cfg     Transport configuration. Required.
 *
 * @return 0 on success, -ENOSYS when built without
 *         `-Denable_mcp_sse=true`, -EINVAL on bad arguments,
 *         -EADDRINUSE if the requested port is busy, -EBUSY if
 *         the transport is already running on this server.
 */
VMAF_EXPORT int vmaf_mcp_start_sse(VmafMcpServer *server, VmafMcpSseConfig *cfg);

/**
 * UDS (Unix domain socket) transport configuration.
 */
typedef struct VmafMcpUdsConfig {
    /** Filesystem path the listener binds to. The file is created
     *  mode 0700 (per ADR-0128 § auth). Required. Caller retains
     *  ownership of the string; it is copied. */
    const char *path;
} VmafMcpUdsConfig;

/**
 * Start the Unix-domain-socket transport. Spawns one dedicated
 * MCP pthread that owns the listener. Wire framing is
 * newline-delimited JSON-RPC.
 *
 * @param server  Server handle previously created via
 *                @ref vmaf_mcp_init.
 * @param cfg     Transport configuration. Required.
 *
 * @return 0 on success, -ENOSYS when built without
 *         `-Denable_mcp_uds=true`, -ENODEV on non-POSIX hosts
 *         that don't expose AF_UNIX, -EINVAL on bad arguments,
 *         -EADDRINUSE if the path is already bound, -EBUSY if the
 *         transport is already running.
 */
VMAF_EXPORT int vmaf_mcp_start_uds(VmafMcpServer *server, const VmafMcpUdsConfig *cfg);

/**
 * stdio transport configuration. Per ADR-0128 + Research-0005, the
 * embedded server does NOT claim the host's own stdin/stdout — the
 * host hands over a dedicated fd pair (typically fd 3 / fd 4 from
 * a parent-spawned wrapper).
 */
typedef struct VmafMcpStdioConfig {
    /** File descriptor the server reads JSON-RPC from. Must be
     *  >= 0. Caller retains ownership; libvmaf does not close it. */
    int fd_in;
    /** File descriptor the server writes JSON-RPC to. Must be
     *  >= 0. Caller retains ownership. */
    int fd_out;
} VmafMcpStdioConfig;

/**
 * Start the stdio transport. Spawns one dedicated MCP pthread that
 * reads LSP-framed JSON-RPC on `fd_in` and writes responses on
 * `fd_out`.
 *
 * @param server  Server handle previously created via
 *                @ref vmaf_mcp_init.
 * @param cfg     Transport configuration. Required.
 *
 * @return 0 on success, -ENOSYS when built without
 *         `-Denable_mcp_stdio=true`, -EINVAL on bad arguments
 *         (negative fds), -EBUSY if the transport is already
 *         running.
 */
VMAF_EXPORT int vmaf_mcp_start_stdio(VmafMcpServer *server, const VmafMcpStdioConfig *cfg);

/**
 * Stop all running transports on @p server, joining their
 * threads. Idempotent — calling on a server with no running
 * transport is a no-op and returns 0. Does NOT release the server
 * handle itself; pair with @ref vmaf_mcp_close.
 *
 * @param server  Server handle to stop.
 *
 * @return 0 on success, -EINVAL on NULL @p server.
 */
VMAF_EXPORT int vmaf_mcp_stop(VmafMcpServer *server);

/**
 * Release a server handle previously created via
 * @ref vmaf_mcp_init. Passing NULL is a no-op. After this call the
 * pointer is invalidated; the caller should set its copy to NULL.
 *
 * Implicitly calls @ref vmaf_mcp_stop if any transport is still
 * running.
 *
 * @param server  Pointer to the server handle to release.
 */
VMAF_EXPORT void vmaf_mcp_close(VmafMcpServer **server);

#ifdef __cplusplus
}
#endif

#endif /* LIBVMAF_MCP_H_ */
