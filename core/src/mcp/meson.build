# Included from libvmaf/src/meson.build via `subdir('mcp')`. Paths
# below are relative to libvmaf/src/mcp/.
#
# T5-2 scaffold (ADR-0209) shipped mcp.c only with `-ENOSYS`.
# T5-2b runtime (PR #490) added dispatcher.c + transport_stdio.c +
# a vendored cJSON (3rdparty/cJSON/, MIT) + a placeholder
# `compute_vmaf` returning {"status":"deferred_to_v2"}.
# T5-2c (PR #533 — MCP runtime v2) added transport_uds.c (Unix
# domain socket) and compute_vmaf.c (real libvmaf scoring binding).
# T5-2d (this PR — MCP runtime v3) adds transport_sse.c (loopback
# HTTP + Server-Sent Events) — implemented in plain POSIX sockets,
# NOT via the originally-planned mongoose vendor. Mongoose is
# GPL-2.0-only-OR-commercial; the fork ships under
# BSD-3-Clause-Plus-Patent, so a vendored copy would be a license
# blocker. See ADR-0332 § "Status update 2026-05-09 (v3 SSE)" for
# the decision matrix.

mcp_sources = files(
    'mcp.c',
    'dispatcher.c',
    'compute_vmaf.c',
    'transport_stdio.c',
    'transport_uds.c',
    'transport_sse.c',
    '3rdparty/cJSON/cJSON.c',
)

mcp_defines = ['-DHAVE_MCP=1']

# enable_mcp_sse moved from `boolean` to `feature` in v3 to match
# the project-wide convention for transport-style sub-flags
# (`enable_vulkan`). 'auto' compiles the transport in (no third-
# party prereqs after the mongoose pivot — only POSIX sockets),
# 'disabled' opts out, 'enabled' is identical to 'auto' but errors
# if the umbrella enable_mcp is off.
mcp_sse_opt = get_option('enable_mcp_sse')
if not mcp_sse_opt.disabled()
    mcp_defines += '-DHAVE_MCP_SSE=1'
endif
if get_option('enable_mcp_uds')
    mcp_defines += '-DHAVE_MCP_UDS=1'
endif
if get_option('enable_mcp_stdio')
    mcp_defines += '-DHAVE_MCP_STDIO=1'
endif
