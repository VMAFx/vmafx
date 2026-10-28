<!-- markdownlint-disable MD060 -->
# Embedded MCP server: `libvmaf_mcp.h`

!!! warning "Deprecated: `libvmaf.h` compatibility API"
    This page documents the compatibility library `libvmaf.so.3`. New code
    uses the [VMAFx API](vmafx/index.md); the [migration table](vmafx/compat.md)
    names the successor of every function.

Use this API to run an MCP (Model Context Protocol) server inside your
process, bound to a `VmafContext`. An embedding host, such as an editor
plugin or a measurement-orchestration daemon, can then drive it over
loopback HTTP, a Unix-domain socket or a caller-owned stdio fd pair. The C
surface is
[`core/include/libvmaf/libvmaf_mcp.h`](../../core/include/libvmaf/libvmaf_mcp.h).

This page covers the embedded C API. The standalone Python MCP server in
[`mcp-server/vmaf-mcp/`](../../mcp-server/vmaf-mcp/) is a separate surface;
see [`docs/mcp/`](../mcp/).

## Build it

The umbrella option compiles the API in; each transport option adds one wire
driver, so a minimal build ships only the transports it needs.

```bash
meson setup build core -Denable_mcp=true -Denable_mcp_sse=enabled \
    -Denable_mcp_uds=true -Denable_mcp_stdio=true
```

| Option | Type | Default | Adds |
| --- | --- | --- | --- |
| `enable_mcp` | boolean | `false` | The API surface (required by the rest). |
| `enable_mcp_sse` | feature | `auto` | Loopback HTTP with an SSE endpoint and JSON-RPC `POST`. |
| `enable_mcp_uds` | boolean | `false` | AF_UNIX socket, newline-delimited JSON-RPC. |
| `enable_mcp_stdio` | boolean | `false` | Newline-delimited JSON-RPC on a caller-supplied fd pair. |

The header is installed only when `enable_mcp` is true. When libvmaf is
built without it, `vmaf_mcp_available()` returns `0` and the init and start
entry points return `-ENOSYS`.

## Public surface

| Symbol | Returns | Purpose |
| --- | --- | --- |
| `vmaf_mcp_available()` | `int` (0/1) | Built with `-Denable_mcp=true`? |
| `vmaf_mcp_transport_available(t)` | `int` (0/1) | Built with that transport option? `0` for an unknown id. |
| `vmaf_mcp_init(out, ctx, cfg)` | `0 / -errno` | Allocate a server handle bound to a `VmafContext`. |
| `vmaf_mcp_start_sse(s, cfg)` | `0 / -errno` | Bind a loopback HTTP listener and spawn the SSE thread. |
| `vmaf_mcp_start_uds(s, cfg)` | `0 / -errno` | Bind an AF_UNIX listener at the configured path (mode 0700). |
| `vmaf_mcp_start_stdio(s, cfg)` | `0 / -errno` | Spawn the stdio thread on a caller-supplied fd pair. |
| `vmaf_mcp_stop(s)` | `0 / -errno` | Join every running transport thread. Idempotent. `-EINVAL` for `NULL`. |
| `vmaf_mcp_close(&s)` | `void` | Release the handle (stops transports first) and set `*s` to `NULL`. `NULL` is a no-op. |

Transport ids: `VMAF_MCP_TRANSPORT_SSE` (0), `_UDS` (1), `_STDIO` (2).

Configuration structs (all safe to zero-initialise):

| Struct and field | Meaning |
| --- | --- |
| `VmafMcpConfig.queue_depth` | Ring slot count; `0` means 64. Must be a power of two, else `-EINVAL`. |
| `VmafMcpConfig.max_drain_per_frame` | Envelopes drained per frame; `0` means 4, capped at 64. |
| `VmafMcpConfig.user_agent` | Tag returned in MCP `serverInfo`; `NULL` for the libvmaf default. Copied. |
| `VmafMcpSseConfig.port` | Loopback TCP port 1 to 65535; `0` picks an ephemeral port and writes it back into the field. |
| `VmafMcpSseConfig.path` | SSE URL path; `NULL` means `/mcp/sse`. Copied. |
| `VmafMcpUdsConfig.path` | Socket path (required). Copied. |
| `VmafMcpStdioConfig.fd_in`, `fd_out` | Descriptors to read and write; both must be `>= 0`. The caller keeps ownership and libvmaf does not close them. |

`vmaf_mcp_init()` must run after `vmaf_init()` and before the first
`vmaf_read_pictures()`. The handle borrows the context pointer, so call
`vmaf_mcp_close()` before `vmaf_close()`. Passing `NULL` for the config
selects all defaults.

## Embedded tools

The embedded tool set is the read-only `list_features` and the out-of-band
`compute_vmaf`.

- `compute_vmaf` scores a YUV420p pair at 8, 10, 12 or 16 bits per
  component, chosen with its optional `bitdepth` JSON argument (default 8).
  YUV422P and YUV444P are outside the current schema.
- It uses a short-lived private `VmafContext` for the requested pair
  instead of borrowing the host's active scorer.

## Threading and allocation

Per [ADR-0209](../adr/0209-mcp-embedded-scaffold.md) and Research-0005:

- Each `_start_*` call spawns one dedicated MCP pthread. Several transports
  can run on one handle.
- JSON parsing, socket I/O and per-request allocation stay on the transport
  thread. The host measurement thread is not mutated by the current tool set.
- `queue_depth` and `max_drain_per_frame` are validated and stored, but the
  measurement thread does not yet drain command envelopes at frame
  boundaries (no such drain exists in `core/src/mcp/`).

## Authentication

| Transport | Rule |
| --- | --- |
| SSE | Binds `127.0.0.1` only; the listener refuses non-loopback addresses. |
| UDS | The socket file is created with mode 0700. |
| stdio | Trusted by construction: the host owns the fds and decides who else sees them. |

## Example

```c
#include <libvmaf/libvmaf.h>
#include <libvmaf/libvmaf_mcp.h>

VmafContext *ctx = NULL;
VmafConfiguration cfg = { .log_level = VMAF_LOG_LEVEL_WARNING };
vmaf_init(&ctx, cfg);

if (vmaf_mcp_available()) {
    VmafMcpServer *mcp = NULL;
    VmafMcpConfig mc = { .queue_depth = 64,
                         .max_drain_per_frame = 4,
                         .user_agent = "my-host/1.0" };
    int rc = vmaf_mcp_init(&mcp, ctx, &mc);
    if (rc == 0 && vmaf_mcp_transport_available(VMAF_MCP_TRANSPORT_UDS)) {
        VmafMcpUdsConfig uds = { .path = "/run/vmaf/mcp.sock" };
        rc = vmaf_mcp_start_uds(mcp, &uds);
    }
    /* ... vmaf_read_pictures and vmaf_score_pooled as usual ... */
    vmaf_mcp_close(&mcp);          /* before vmaf_close() */
}

int close_rc = vmaf_close(ctx);
if (close_rc != 0)
    close_rc = vmaf_close(ctx);    /* retained teardown-only context */
if (close_rc == 0)
    ctx = NULL;
/* A persistent error retains ctx and every borrowed dependency. */
```

Check `vmaf_mcp_transport_available()` before each `_start_*` call. Close
the MCP handle first; an exact-zero `vmaf_close()` invalidates the context,
and a nonzero result retains it for retry
([close and retry](lifecycle.md#close-and-retry)).

## Error contract

All entry points return `0` on success and a negative `errno` on failure.
`vmaf_mcp_available()`, `vmaf_mcp_transport_available()` and
`vmaf_mcp_close()` return no error code.

| Code | Meaning |
| --- | --- |
| `-ENOSYS` | Embedded MCP, or that transport, was not built in. |
| `-ENODEV` | Transport runtime unavailable, for example UDS on a non-POSIX host. |
| `-EINVAL` | Bad argument: `NULL` where required, malformed config, negative fd, non-power-of-two `queue_depth`. |
| `-ENOMEM` | Ring or buffer allocation failed at init. |
| `-EBUSY` | A measurement is already in flight, or the transport already runs on this handle. |
| `-EADDRINUSE` | SSE port or UDS path already bound. |

## History

- [ADR-0209](../adr/0209-mcp-embedded-scaffold.md) introduced an
  audit-first header scaffold whose entry points returned `-ENOSYS`. It has
  since been promoted to a working embedded runtime: `vmaf_mcp_init()`,
  `vmaf_mcp_stop()` and `vmaf_mcp_close()` manage the handle, and the three
  transports above are implemented.

## Related

- [ADR-0209](../adr/0209-mcp-embedded-scaffold.md): embedded-MCP scaffold
  and runtime status history.
- [`docs/mcp/embedded.md`](../mcp/embedded.md): user-side overview of the
  embedded server.
- [`docs/mcp/`](../mcp/): standalone Python MCP server surface.
