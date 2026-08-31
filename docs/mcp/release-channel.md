# MCP Server Release Channel

The fork ships **three** MCP server flavours:

1. The standalone Python server under `mcp-server/vmaf-mcp/`
   ([`docs/mcp/tools.md`](tools.md)).
2. The standalone Go binary `vmafx-mcp` under `cmd/vmafx-mcp/` —
   same 15 tools, byte-for-byte schema parity with the Python server.
   ([`docs/mcp/index.md#go-implementation-vmafx-mcp`](index.md#go-implementation-vmafx-mcp)).
3. The embedded server inside `libvmaf` itself, exposed via the
   `libvmaf_mcp.h` C surface
   ([`docs/mcp/embedded.md`](embedded.md)).

[ADR-0166](../adr/0166-mcp-server-release-channel.md) governs the
standalone Python server release channel. The `vmaf-mcp`
distribution is published to PyPI from the same release flow as the
libvmaf fork, uses the coordinated VMAFx `vX.Y.Z` version line, and
is signed through the same keyless Sigstore/OIDC pipeline.

Operators should install the standalone server from PyPI when an
agent needs a child-process tool surface:

```bash
pip install vmaf-mcp
```

For local development from a checkout:

```bash
cd mcp-server/vmaf-mcp
pip install -e .
```

Set `VMAF_BIN=/abs/path/to/vmaf` when the built CLI is not at the
repo-default `build/tools/vmaf`, and set `VMAF_MCP_ALLOW` to any
additional corpus roots the server is allowed to read.

Embedded-MCP users do not install `vmaf-mcp`; they build libvmaf
with `-Denable_mcp=true` and the needed transport flags, then call
the `libvmaf_mcp.h` C API from the host process. The embedded server
is not a separate package. Its ABI and version ride with libvmaf
itself: the public symbols live in `libvmaf_mcp.h`, the
implementation is compiled by `-Denable_mcp=true`, and compatibility
follows the libvmaf SOVERSION. That keeps client expectations simple:
a libvmaf build advertises the embedded transports it actually
compiled via `vmaf_mcp_transport_available()`, while the Python
package advertises the standalone CLI-wrapping tool surface.

## Release Checklist

For a libvmaf release:

- Build with the intended MCP flags and run `test_mcp_smoke`.
- Confirm `vmaf_mcp_available()` and
  `vmaf_mcp_transport_available()` match the release configuration.
- Keep embedded MCP behavior documented in
  [`embedded.md`](embedded.md), not in the Python package README.

For a `vmaf-mcp` Python package release:

- Build from `mcp-server/vmaf-mcp/`.
- Keep the tool schemas in [`tools.md`](tools.md) aligned with
  `mcp-server/vmaf-mcp/src/vmaf_mcp/server.py`.
- Publish and sign through the same release workflow used for the
  rest of the fork.

The Pending Trusted Publisher for the first PyPI publication was configured on
2026-08-31. Its binding uses these exact current repository identities:

| PyPI field   | Value              |
|--------------|--------------------|
| Project name | `vmaf-mcp`         |
| GitHub owner | `VMAFx`            |
| Repository   | `vmafx`            |
| Workflow     | `supply-chain.yml` |
| Environment  | `pypi-publish`     |

PyPI returns 404 for the project until that first trusted publication
completes. Before publishing the GitHub draft, confirm the pending row still
matches this table. Do not reuse the historical `lusoris/vmaf` identity from
ADR-0166; the repository was transferred and renamed after that accepted
decision.

For the `vmafx-mcp` Go binary release:

- Build from `cmd/vmafx-mcp/` with `go build -o vmafx-mcp ./cmd/vmafx-mcp`.
- Run `TestVmafScoreTool` and `TestGoVsPythonOutputParity` to confirm
  byte-for-byte schema parity with the Python server.
- Confirm tool count is 15 (matching the Python surface) before tagging.
- Binary artifact is published via the same goreleaser step as the rest
  of the Go toolchain; see
  [`docs/development/release.md`](../development/release.md).

## See also

- [`docs/mcp/tools.md`](tools.md) — the MCP tool surface served by
  both flavours.
- [`docs/mcp/embedded.md`](embedded.md) — the embedded-server build
  flag and transport matrix.
- [`docs/development/release.md`](../development/release.md) — the
  shared release / signing pipeline.
- [ADR-0166](../adr/0166-mcp-server-release-channel.md) — design
  decision.
