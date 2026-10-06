/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 */

/**
 * @file libvmaf_mcp.h
 * @brief Embedded MCP (Model Context Protocol) server public API.
 *
 * Designed by ADR-0128, scaffolded by ADR-0209, runtime v3 (T5-2b/c/d):
 * `vmaf_mcp_init`, `vmaf_mcp_start_{stdio,uds,sse}`, `vmaf_mcp_stop` and
 * `vmaf_mcp_close` are wired and serve `list_features` and `compute_vmaf`
 * (see docs/mcp/embedded.md). The SPSC command ring that would let a
 * transport steer a running measurement is v4 work: until then
 * `VmafMcpConfig.queue_depth` and `max_drain_per_frame` are validated and
 * stored but allocate and drain nothing.
 *
 * When libvmaf was built without `-Denable_mcp=true`, every entry
 * point returns -ENOSYS unconditionally, so a caller compiled against
 * this header sees a predictable error and can fall back to the external
 * MCP server (Go `cmd/vmafx-mcp` or Python `mcp-server/vmaf-mcp/`).
 *
 * Threading model (per ADR-0128 + Research-0005):
 *   - The host calls `vmaf_mcp_init` after `vmaf_init` and before
 *     the first `vmaf_read_pictures`.
 *   - One transport-start call (`_start_sse`, `_start_uds`,
 *     `_start_stdio`) per active transport; they may be combined.
 *     Each spawns a dedicated MCP pthread. The measurement thread is
 *     not touched in v3 (no SPSC ring yet).
 *   - `vmaf_mcp_stop` joins the MCP threads; `vmaf_mcp_close`
 *     releases the handle. Closing a NULL handle is a no-op.
 *
 * Auth surface (per ADR-0128 § "Operational guardrails"):
 *   - SSE binds to 127.0.0.1 only.
 *   - UDS uses filesystem mode 0700 on the socket file.
 *   - stdio is trusted by construction (host owns the fds).
 *
 * Error contract (negative errno):
 *   -ENOSYS — feature (or the transport) not built.
 *   -ENODEV — transport-specific runtime unavailable
 *             (e.g. UDS on a non-POSIX host).
 *   -EINVAL — bad argument (NULL where required, malformed config).
 *   -ENOMEM — ring/buffer allocation failed at init.
 *   -EBUSY  — measurement already in flight (start refused).
 */

#ifndef LIBVMAF_MCP_H_
#define LIBVMAF_MCP_H_

/* NOLINTBEGIN(modernize-use-using, modernize-deprecated-headers, performance-enum-size):
 * public C API header. clang-tidy reads it as C++ and proposes `using`,
 * `<cstdint>` and a narrower enum base type; all three are wrong here. This
 * header has to compile as C for every consumer of the shipped library, where
 * `using` does not exist and the C spellings of the standard headers are the
 * only ones available, and the enum's base type is part of the ABI the fork
 * publishes. CLAUDE.md rule 12 reserves suppressions for exactly this: a rule
 * that cannot be followed without breaking a load-bearing invariant
 * (ADR-0141). */

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
VMAF_DEPRECATED("use vmafx_mcp_available")
VMAF_EXPORT int vmaf_mcp_available(void);

/**
 * Transport identifiers — used by @ref vmaf_mcp_transport_available
 * and the per-transport `_start_*` entry points.
 */
typedef enum VmafMcpTransport {
    VMAF_MCP_TRANSPORT_SSE = 0,   /**< Server-Sent Events over loopback HTTP. */
    VMAF_MCP_TRANSPORT_UDS = 1,   /**< Unix domain socket, newline-delimited JSON-RPC. */
    VMAF_MCP_TRANSPORT_STDIO = 2, /**< newline-delimited JSON-RPC on a caller-supplied fd pair. */
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
VMAF_DEPRECATED("use vmafx_mcp_transport_available")
VMAF_EXPORT int vmaf_mcp_transport_available(VmafMcpTransport transport);

/**
 * Opaque handle to an embedded MCP server. One handle pins one
 * server context + zero-or-more transport threads. The handle is
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
     *  two; rejected otherwise with -EINVAL. Reserved for the v4
     *  SPSC bridge: v3 validates and stores it but allocates no
     *  ring. */
    uint32_t queue_depth;
    /** Upper bound on command envelopes the measurement thread
     *  drains per frame (NASA Power-of-10 rule 2). 0 → default 4.
     *  Cap 64. Reserved for the v4 SPSC bridge; v3 drains nothing. */
    uint32_t max_drain_per_frame;
    /** Optional NUL-terminated tag returned in MCP `serverInfo`.
     *  NULL → libvmaf default. Caller retains ownership; the string
     *  is copied into the handle. */
    const char *user_agent;
} VmafMcpConfig;

/**
 * Initialise an embedded MCP server bound to a VmafContext. Must
 * be called after `vmaf_init` and before the first
 * `vmaf_read_pictures`. Allocates the server handle; the measurement
 * thread is not touched.
 *
 * @param out  Receives the new server handle. Caller owns it; pair
 *             with @ref vmaf_mcp_close.
 * @param ctx  VmafContext the server introspects + steers. The
 *             handle borrows the pointer for its lifetime; the
 *             caller must call @ref vmaf_mcp_close before
 *             vmaf_close().
 * @param cfg  Configuration. NULL → all-defaults.
 *
 * @return 0 on success, -ENOSYS when built without MCP, -EINVAL
 *         on bad arguments, -ENOMEM on ring allocation failure,
 *         -EBUSY if measurement is already in flight.
 */
VMAF_DEPRECATED("use vmafx_mcp_server_create")
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
VMAF_DEPRECATED("use vmafx_mcp_start_sse")
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
VMAF_DEPRECATED("use vmafx_mcp_start_uds")
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
 * reads newline-delimited JSON-RPC on `fd_in` (one request per line; no
 * LSP `Content-Length:` framing) and writes one response line on
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
VMAF_DEPRECATED("use vmafx_mcp_start_stdio")
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
VMAF_DEPRECATED("use vmafx_mcp_stop")
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
VMAF_DEPRECATED("use vmafx_mcp_server_destroy")
VMAF_EXPORT void vmaf_mcp_close(VmafMcpServer **server);

#ifdef __cplusplus
}
#endif

/* NOLINTEND(modernize-use-using, modernize-deprecated-headers, performance-enum-size) */

#endif /* LIBVMAF_MCP_H_ */
