/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Internal MCP runtime types — not part of the public ABI. The
 *  public header (`libvmaf/include/libvmaf/libvmaf_mcp.h`) only
 *  forward-declares `VmafMcpServer`; this header carries the
 *  storage class for the live stdio + UDS + SSE runtime.
 *
 *  Current scope (T5-2b through v3, ADR-0209):
 *      - All three transports live: stdio (line-delimited JSON-RPC),
 *        UDS (`AF_UNIX`, mode 0700, single client at a time), SSE
 *        (`AF_INET` loopback HTTP/1.1, fork-owned plain POSIX sockets
 *        — no mongoose, BSD-3 license compat).
 *      - line-delimited JSON-RPC framing on stdio/UDS;
 *        LSP `Content-Length:` framing remains a follow-up.
 *      - dispatcher routes `tools/list`, `tools/call`,
 *        `resources/list`. Tools: `list_features`, `compute_vmaf`
 *        (real per-call ephemeral `VmafContext` scoring).
 */

#ifndef LIBVMAF_SRC_MCP_INTERNAL_H_
#define LIBVMAF_SRC_MCP_INTERNAL_H_

#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_mcp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Hard caps — Power-of-10 rule 2 (bounded loops) + rule 3
 * (no dynamic alloc on the hot path). */
#define VMAF_MCP_MAX_LINE_BYTES ((size_t)64u * 1024u) /* 64 KiB request line. */
#define VMAF_MCP_MAX_FEATURES 256u                    /* `list_features` cap. */

/* Listener backlog — generous for the embedded single-server use case;
 * the kernel silently clamps to SOMAXCONN. The MCP server is not
 * expected to handle bursty accept storms, so 16 is comfortable. */
#define VMAF_MCP_LISTEN_BACKLOG 16

/* Upper bound on the `VmafMcpTransport` enum range expressed as a
 * bitmask test. The bitmask is built from per-transport
 * `1u << transport` shifts; restrict to the low 32 bits so the shift
 * is always defined behaviour. CERT INT34-C: undefined when
 * shift count >= bit width. */
#define VMAF_MCP_TRANSPORT_BITMASK_MAX 31u

/* Per-frame drain cap (paired-event bursts produced by the
 * `vmaf_metadata_publish` callback). Cap matches the largest known
 * publisher (DNN multi-output) plus 2x headroom. */
#define VMAF_MCP_MAX_DRAIN_PER_FRAME 64u

/* SSE URL-path string cap (e.g. "/events"). The path is purely a
 * routing key; 256 bytes is well above any sensible HTTP route. */
#define VMAF_MCP_SSE_PATH_MAX 256u

/* AF_UNIX `struct sockaddr_un.sun_path` is 108 bytes on Linux,
 * 104 on BSD. Cap at 100 to leave headroom across POSIX hosts and
 * keep the rejection cross-platform identical. */
#define VMAF_MCP_UDS_PATH_MAX 100u

/* Forward decl for the stdio worker — defined in transport_stdio.c,
 * dispatched from mcp.c. Declared here (rather than file-local in
 * mcp.c) to satisfy clang-tidy's misc-use-internal-linkage check. */
void *vmaf_mcp_stdio_thread_main(void *arg);

/* Forward decl for the UDS worker — defined in transport_uds.c. */
void *vmaf_mcp_uds_thread_main(void *arg);

/* Forward decl for the SSE worker — defined in transport_sse.c (v3). */
void *vmaf_mcp_sse_thread_main(void *arg);

/* Forward decl for the v2 compute_vmaf tool — defined in
 * compute_vmaf.c. The cJSON pointers are typed as `void *` here so
 * the cJSON header is not required by every TU that pulls in this
 * file; the implementation casts back. Returns 0 on success and
 * sets *result_out to a freshly-allocated cJSON object the caller
 * owns; on failure returns negative errno and (optionally) sets
 * *err_owned to a heap-allocated, NUL-terminated message the
 * caller must free(). */
int vmaf_mcp_compute_vmaf(const void *arguments_cjson, void **result_out_cjson, char **err_owned);

struct VmafMcpServer {
    VmafContext *ctx;       /* Borrowed; not owned. */
    VmafMcpConfig cfg;      /* Copied at init; user_agent dup'd. */
    char *user_agent_owned; /* NULL when defaulted. */

    /* stdio transport state. */
    int stdio_fd_in;
    int stdio_fd_out;
    pthread_t stdio_thread;
    atomic_int stdio_running;  /* 0 = not started, 1 = running, 2 = stop-requested. */
    pthread_mutex_t write_mtx; /* Serialises responses across future transports. */

    /* UDS transport state (v2). */
    int uds_listen_fd;      /* AF_UNIX listener; -1 when not started. */
    char *uds_path_owned;   /* Bound socket path; freed at close. */
    pthread_t uds_thread;   /* Listener-accept thread. */
    atomic_int uds_running; /* 0 / 1 / 2 same semantics as stdio. */

    /* SSE transport state (v3). Bound to 127.0.0.1 only. */
    int sse_listen_fd;      /* AF_INET listener on loopback; -1 when not started. */
    uint16_t sse_port;      /* Bound TCP port (resolved when caller passed 0). */
    char *sse_path_owned;   /* URL path served as the event stream. NULL → default. */
    pthread_t sse_thread;   /* Listener-accept thread. */
    atomic_int sse_running; /* 0 / 1 / 2 same semantics as stdio. */
};

/**
 * Dispatch a single JSON-RPC request string and produce a response
 * string. Caller frees `*response_out` with free().
 *
 * @param server       Server instance (borrowed).
 * @param request_buf  NUL-terminated JSON-RPC request.
 * @param response_out Receives heap-allocated NUL-terminated response,
 *                     or NULL for a notification (id absent). Caller frees.
 * @return 0 on success (response produced or notification swallowed).
 *         Negative errno on internal failure (parser OOM etc.) — the
 *         caller should still attempt to surface a JSON-RPC error
 *         response when the input was non-empty.
 */
int vmaf_mcp_dispatch(struct VmafMcpServer *server, const char *request_buf, char **response_out);

#ifdef __cplusplus
}
#endif

#endif /* LIBVMAF_SRC_MCP_INTERNAL_H_ */
