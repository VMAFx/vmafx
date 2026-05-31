<!-- markdownlint-disable MD060 -->
# Embedded MCP server — `libvmaf_mcp.h`

The fork ships an in-process MCP (Model Context Protocol) server that an
embedding host (e.g. an editor plugin or a measurement-orchestration
daemon) can drive over loopback HTTP, a Unix-domain socket, or a
caller-owned stdio fd pair. The C surface is defined in
[`core/include/libvmaf/libvmaf_mcp.h`](../../core/include/libvmaf/libvmaf_mcp.h).

This page covers the **embedded C API**. The standalone Python MCP
server under [`mcp-server/vmaf-mcp/`](../../mcp-server/vmaf-mcp/) is a
separate surface — see [`docs/mcp/`](../mcp/) for that one.

## Status

The audit-first header scaffold from
[ADR-0209](../adr/0209-mcp-embedded-scaffold.md) has been promoted to a
working embedded runtime:

- `vmaf_mcp_init()` / `vmaf_mcp_stop()` / `vmaf_mcp_close()` manage a
  server handle bound to a host `VmafContext`.
- `vmaf_mcp_start_stdio()` serves line-delimited JSON-RPC on a
  caller-owned fd pair.
- `vmaf_mcp_start_uds()` serves line-delimited JSON-RPC over an
  AF_UNIX socket and creates the socket path mode 0700.
- `vmaf_mcp_start_sse()` serves loopback HTTP with an SSE endpoint and
  JSON-RPC `POST` requests.
- The embedded tool set currently includes read-only `list_features`
  and out-of-band `compute_vmaf` for YUV420p 8/10/12/16-bit pairs.

When libvmaf is built without `-Denable_mcp=true`, the public symbols
remain available but `vmaf_mcp_available()` returns `0` and start/init
entry points return `-ENOSYS`. Transport availability is controlled by
the per-transport build flags.

## Build flag

```bash
meson setup build -Denable_mcp=true \
                  -Denable_mcp_sse=enabled `# loopback HTTP / SSE`     \
                  -Denable_mcp_uds=true   `# AF_UNIX, JSON-RPC`       \
                  -Denable_mcp_stdio=true `# line JSON-RPC on host fds`
```

The umbrella flag (`enable_mcp`) compiles in the API surface; each
transport sub-flag adds the corresponding wire driver. Sub-flags
let a minimal MCP build ship with only the transports it needs. SSE is a
Meson feature option (`auto` by default); UDS and stdio are boolean
options that default to `false`.

## Public surface (one-liner reference)

| Symbol                            | Returns         | Purpose                                                         |
|-----------------------------------|-----------------|-----------------------------------------------------------------|
| `vmaf_mcp_available()`            | `int` (0/1)     | Built with `-Denable_mcp=true`?                              |
| `vmaf_mcp_transport_available(t)` | `int` (0/1)     | Built with the named transport sub-flag?                     |
| `vmaf_mcp_init(out, ctx, cfg)`    | `0 / -errno`    | Allocate a server handle bound to a `VmafContext`.           |
| `vmaf_mcp_start_sse(s, cfg)`      | `0 / -errno`    | Bind a loopback HTTP listener; spawn the SSE pthread.        |
| `vmaf_mcp_start_uds(s, cfg)`      | `0 / -errno`    | Bind an AF_UNIX listener at the configured path (mode 0700). |
| `vmaf_mcp_start_stdio(s, cfg)`    | `0 / -errno`    | Spawn the stdio pthread on a caller-supplied fd pair.        |
| `vmaf_mcp_stop(s)`                | `0 / -errno`    | Join every running transport thread (idempotent).            |
| `vmaf_mcp_close(out)`             | `void`          | Release the handle; sets `*out` to `NULL`.                   |

The embedded `compute_vmaf` tool accepts YUV420p inputs at 8, 10, 12,
or 16 bpc via its optional `bitdepth` JSON argument. The default is
8 bpc. YUV422P / YUV444P are intentionally outside the current schema.

## Threading and allocation

Per [ADR-0209](../adr/0209-mcp-embedded-scaffold.md) and
Research-0005:

- Each `_start_*` call spawns one dedicated MCP pthread. Multiple
  transports can co-exist on one server handle.
- JSON parsing, socket I/O, and per-request allocation stay on the
  transport thread. The host measurement thread is not mutated by the
  current tool set.
- `compute_vmaf` uses a short-lived private `VmafContext` for the
  requested YUV pair instead of borrowing the host's active scorer.
- `queue_depth` and `max_drain_per_frame` are validated API fields for
  the planned v4 SPSC bridge, but v3 does not yet drain envelopes on
  frame boundaries.

## Auth

- **SSE** binds 127.0.0.1 only — the listener refuses non-loopback
  addresses.
- **UDS** creates the socket mode 0700.
- **stdio** is trusted by construction (the host owns the fds and
  decides who else sees them).

## Example

```c
#include <libvmaf/libvmaf.h>
#include <libvmaf/libvmaf_mcp.h>

VmafContext *ctx = NULL;
VmafConfiguration cfg = { /* … */ };
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
    /* … vmaf_read_pictures + vmaf_score_pooled as usual … */
    vmaf_mcp_close(&mcp);
}

vmaf_close(ctx);
```

`-ENOSYS` from init/start calls means this libvmaf build omitted the
embedded MCP umbrella flag. `vmaf_mcp_transport_available()` lets hosts
check individual transport flags before calling `_start_*`.

## Error contract

All entry points return a negative `errno` on failure (`0` on success).
The most common codes:

- `-ENOSYS` — embedded MCP was not built into this libvmaf.
- `-ENODEV` — transport-specific runtime unavailable (e.g. UDS on a
  non-POSIX host).
- `-EINVAL` — bad argument (NULL where required, malformed config,
  non-power-of-two `queue_depth`).
- `-ENOMEM` — ring/buffer allocation failed at init.
- `-EBUSY` — measurement already in flight, or the named transport is
  already running on this handle.
- `-EADDRINUSE` — SSE port / UDS path already bound.

## Related

- [ADR-0209](../adr/0209-mcp-embedded-scaffold.md) — embedded-MCP
  scaffold and runtime status history.
- [`docs/mcp/embedded.md`](../mcp/embedded.md) — user-side overview of
  the embedded server.
- [`docs/mcp/`](../mcp/) — standalone Python MCP server surface.
