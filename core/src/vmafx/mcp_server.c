/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * The embedded MCP server of the VMAFx API (vmafx/mcp.h, RC4 WP6): the
 * engine's server (core/src/mcp/, ADR-0209) under the new names. A
 * VmafxMcpServer is the engine's server object. The functions exist in every
 * build; without the server (`-Denable_mcp=false`) the constructors and
 * transports are VMAFX_E_NOTSUP and the queries report 0.
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include "error_internal.h"
#include "internal.h"
#include "status_gen.h"
#include "vmafx/vmafx.h"

#ifdef HAVE_MCP
#include "libvmaf/libvmaf_mcp.h"
#endif

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

#ifdef HAVE_MCP

static VmafMcpServer *engine_server(VmafxMcpServer *server)
{
    return (VmafMcpServer *)server;
}

static VmafxStatus mcp_failure(const VmafxReport *report, int err, const char *subject,
                               const char *what)
{
    return VMAFX_FAIL(report, vmafx_status_from_errno(err), err, VMAFX_SUBJECT_PARAMETER, subject,
                      "%s failed (%d)", what, err);
}

uint32_t vmafx_mcp_available(void)
{
    return vmaf_engine_mcp_available() ? 1u : 0u;
}

uint32_t vmafx_mcp_transport_available(uint32_t transport)
{
    return vmaf_engine_mcp_transport_available((VmafMcpTransport)transport) ? 1u : 0u;
}

VmafxStatus vmafx_mcp_server_create(VmafxContext *context, const VmafxMcpConfig *config,
                                    VmafxMcpServer **out, VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(context, error);
    VmafMcpConfig cfg = {0};
    if (config) {
        VmafxMcpConfig c = VMAFX_MCP_CONFIG_INIT;
        const VmafxStatus status = vmafx_read_sized(&report, &c, (uint32_t)sizeof(c), config,
                                                    VMAFX_MIN_MCP_CONFIG, "config");
        if (status != VMAFX_OK) {
            return status;
        }
        cfg.queue_depth = c.queue_depth;
        cfg.max_drain_per_frame = c.max_drain_per_frame;
        cfg.user_agent = c.user_agent;
    }
    VmafContext *const engine = context ? vmafx_context_engine(context) : NULL;
    const int err = vmaf_engine_mcp_init((VmafMcpServer **)out, engine, config ? &cfg : NULL);
    if (err) {
        return mcp_failure(&report, err, "config", "creating the MCP server");
    }
    return VMAFX_OK;
}

VmafxStatus vmafx_mcp_start_sse(VmafxMcpServer *server, const VmafxMcpSseConfig *config,
                                uint32_t *port, VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(NULL, error);
    VmafMcpSseConfig cfg = {0};
    VmafMcpSseConfig *engine_cfg = NULL;
    if (config) {
        VmafxMcpSseConfig c = VMAFX_MCP_SSE_CONFIG_INIT;
        const VmafxStatus status = vmafx_read_sized(&report, &c, (uint32_t)sizeof(c), config,
                                                    VMAFX_MIN_MCP_SSE_CONFIG, "config");
        if (status != VMAFX_OK) {
            return status;
        }
        if (c.port > UINT16_MAX) {
            return VMAFX_FAIL(&report, VMAFX_E_RANGE, 0, VMAFX_SUBJECT_PARAMETER, "config.port",
                              "port %u is not a TCP port", (unsigned)c.port);
        }
        cfg.port = (uint16_t)c.port;
        cfg.path = c.path;
        engine_cfg = &cfg;
    }
    const int err = vmaf_engine_mcp_start_sse(engine_server(server), engine_cfg);
    if (err) {
        return mcp_failure(&report, err, "config", "starting the SSE transport");
    }
    if (port) {
        *port = cfg.port;
    }
    return VMAFX_OK;
}

VmafxStatus vmafx_mcp_start_uds(VmafxMcpServer *server, const VmafxMcpUdsConfig *config,
                                VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(NULL, error);
    VmafMcpUdsConfig cfg = {0};
    if (config) {
        VmafxMcpUdsConfig c = VMAFX_MCP_UDS_CONFIG_INIT;
        const VmafxStatus status = vmafx_read_sized(&report, &c, (uint32_t)sizeof(c), config,
                                                    VMAFX_MIN_MCP_UDS_CONFIG, "config");
        if (status != VMAFX_OK) {
            return status;
        }
        cfg.path = c.path;
    }
    const int err = vmaf_engine_mcp_start_uds(engine_server(server), config ? &cfg : NULL);
    if (err) {
        return mcp_failure(&report, err, "config", "starting the UDS transport");
    }
    return VMAFX_OK;
}

VmafxStatus vmafx_mcp_start_stdio(VmafxMcpServer *server, const VmafxMcpStdioConfig *config,
                                  VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(NULL, error);
    VmafMcpStdioConfig cfg = {0};
    if (config) {
        VmafxMcpStdioConfig c = VMAFX_MCP_STDIO_CONFIG_INIT;
        const VmafxStatus status = vmafx_read_sized(&report, &c, (uint32_t)sizeof(c), config,
                                                    VMAFX_MIN_MCP_STDIO_CONFIG, "config");
        if (status != VMAFX_OK) {
            return status;
        }
        cfg.fd_in = c.fd_in;
        cfg.fd_out = c.fd_out;
    }
    const int err = vmaf_engine_mcp_start_stdio(engine_server(server), config ? &cfg : NULL);
    if (err) {
        return mcp_failure(&report, err, "config", "starting the stdio transport");
    }
    return VMAFX_OK;
}

VmafxStatus vmafx_mcp_stop(VmafxMcpServer *server, VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(NULL, error);
    const int err = vmaf_engine_mcp_stop(engine_server(server));
    if (err) {
        return mcp_failure(&report, err, "server", "stopping the MCP server");
    }
    return VMAFX_OK;
}

void vmafx_mcp_server_destroy(VmafxMcpServer *server)
{
    VmafMcpServer *held = engine_server(server);
    vmaf_engine_mcp_close(&held);
}

#else /* !HAVE_MCP */

static VmafxStatus mcp_absent(VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(NULL, error);
    return VMAFX_FAIL(&report, VMAFX_E_NOTSUP, 0, VMAFX_SUBJECT_BACKEND, "mcp",
                      "this build has no embedded MCP server (-Denable_mcp=true)");
}

uint32_t vmafx_mcp_available(void)
{
    return 0u;
}

uint32_t vmafx_mcp_transport_available(uint32_t transport)
{
    (void)transport;
    return 0u;
}

VmafxStatus vmafx_mcp_server_create(VmafxContext *context, const VmafxMcpConfig *config,
                                    VmafxMcpServer **out, VmafxError **error)
{
    (void)context;
    (void)config;
    if (out) {
        *out = NULL;
    }
    return mcp_absent(error);
}

VmafxStatus vmafx_mcp_start_sse(VmafxMcpServer *server, const VmafxMcpSseConfig *config,
                                uint32_t *port, VmafxError **error)
{
    (void)server;
    (void)config;
    if (port) {
        *port = 0u; /* no transport, so no bound port: the out-parameter is defined */
    }
    return mcp_absent(error);
}

VmafxStatus vmafx_mcp_start_uds(VmafxMcpServer *server, const VmafxMcpUdsConfig *config,
                                VmafxError **error)
{
    (void)server;
    (void)config;
    return mcp_absent(error);
}

VmafxStatus vmafx_mcp_start_stdio(VmafxMcpServer *server, const VmafxMcpStdioConfig *config,
                                  VmafxError **error)
{
    (void)server;
    (void)config;
    return mcp_absent(error);
}

VmafxStatus vmafx_mcp_stop(VmafxMcpServer *server, VmafxError **error)
{
    (void)server;
    return mcp_absent(error);
}

void vmafx_mcp_server_destroy(VmafxMcpServer *server)
{
    (void)server;
}

#endif /* HAVE_MCP */

/* NOLINTEND(modernize-use-nullptr) */
