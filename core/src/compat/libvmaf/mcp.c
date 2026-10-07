/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * libvmaf embedded MCP server functions on the VMAFx API (vmafx/mcp.h,
 * ADR-1852 design section 2.11). They exist in builds with the server
 * (-Denable_mcp=true), as in libvmaf. A VmafMcpServer is a VmafxMcpServer
 * under its old name; the library validates the arguments and its errno is
 * libvmaf's return value. core/src/meson.build compiles this file only in
 * builds with the server.
 */

#include <stddef.h>
#include <stdint.h>

#include "compat_errno.h"
#include "libvmaf/libvmaf_mcp.h"
#include "vmafx/libvmaf_bridge.h"
#include "vmafx/vmafx.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

static VmafxMcpServer *server_of(VmafMcpServer *server)
{
    return (VmafxMcpServer *)server;
}

int vmaf_mcp_init(VmafMcpServer **out, VmafContext *ctx, const VmafMcpConfig *cfg)
{
    VmafxMcpConfig config = VMAFX_MCP_CONFIG_INIT;
    if (cfg) {
        config.queue_depth = cfg->queue_depth;
        config.max_drain_per_frame = cfg->max_drain_per_frame;
        config.user_agent = cfg->user_agent;
    }
    VmafxError *error = NULL;
    const VmafxStatus status = vmafx_mcp_server_create(
        vmafx_context_from_libvmaf(ctx), cfg ? &config : NULL, (VmafxMcpServer **)out, &error);
    return status == VMAFX_OK ? 0 : compat_errno(status, error);
}

/* libvmaf writes the bound port back to cfg->port (port 0: ephemeral). */
int vmaf_mcp_start_sse(VmafMcpServer *server, VmafMcpSseConfig *cfg)
{
    VmafxMcpSseConfig config = VMAFX_MCP_SSE_CONFIG_INIT;
    if (cfg) {
        config.port = cfg->port;
        config.path = cfg->path;
    }
    uint32_t port = 0;
    VmafxError *error = NULL;
    const VmafxStatus status =
        vmafx_mcp_start_sse(server_of(server), cfg ? &config : NULL, &port, &error);
    if (status != VMAFX_OK) {
        return compat_errno(status, error);
    }
    /* A NULL cfg never succeeds (-EINVAL above); the guard keeps the write
     * provably inside the caller's struct. */
    if (cfg) {
        cfg->port = (uint16_t)port;
    }
    return 0;
}

int vmaf_mcp_start_uds(VmafMcpServer *server, const VmafMcpUdsConfig *cfg)
{
    VmafxMcpUdsConfig config = VMAFX_MCP_UDS_CONFIG_INIT;
    if (cfg) {
        config.path = cfg->path;
    }
    VmafxError *error = NULL;
    const VmafxStatus status = vmafx_mcp_start_uds(server_of(server), cfg ? &config : NULL, &error);
    return status == VMAFX_OK ? 0 : compat_errno(status, error);
}

int vmaf_mcp_start_stdio(VmafMcpServer *server, const VmafMcpStdioConfig *cfg)
{
    VmafxMcpStdioConfig config = VMAFX_MCP_STDIO_CONFIG_INIT;
    if (cfg) {
        config.fd_in = cfg->fd_in;
        config.fd_out = cfg->fd_out;
    }
    VmafxError *error = NULL;
    const VmafxStatus status =
        vmafx_mcp_start_stdio(server_of(server), cfg ? &config : NULL, &error);
    return status == VMAFX_OK ? 0 : compat_errno(status, error);
}

int vmaf_mcp_stop(VmafMcpServer *server)
{
    VmafxError *error = NULL;
    const VmafxStatus status = vmafx_mcp_stop(server_of(server), &error);
    return status == VMAFX_OK ? 0 : compat_errno(status, error);
}

void vmaf_mcp_close(VmafMcpServer **server)
{
    if (!server) {
        return;
    }
    vmafx_mcp_server_destroy(server_of(*server));
    *server = NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
