# MCP Server Release Channel

This page explains how each MCP server flavour is released and what to check
before tagging. Governing decisions:
[ADR-0166](../adr/0166-mcp-server-release-channel.md)
(the Python release channel) and [ADR-1229](../adr/1229-mcp-go-runtime.md)
(the Go binary is the MCP server; the Python package is deprecated).

## Flavours at a glance

| Flavour | Install | Versioning | Tools |
| --- | --- | --- | --- |
| Standalone Python server, `mcp-server/vmaf-mcp/` ([tools](tools.md)) | `pip install vmaf-mcp` | Coordinated VMAFx `vX.Y.Z` line; published to PyPI and signed with keyless Sigstore/OIDC | 19 |
| Standalone Go binary `vmafx-mcp`, `cmd/vmafx-mcp/` ([overview](index.md#go-implementation-vmafx-mcp)) | Installed at `/usr/local/bin/vmafx-mcp` in the container images, or `go build -o vmafx-mcp ./cmd/vmafx-mcp` | Built from the repository; not published as a separate artefact | 24 (the 19 Python tools plus 5 control-plane tools) |
| Embedded server inside `libvmaf`, `libvmaf_mcp.h` ([embedded](embedded.md)) | Build libvmaf with `-Denable_mcp=true` | Rides with libvmaf; compatibility follows the libvmaf SOVERSION | 2 |

## Python package

The `vmaf-mcp` distribution is published to PyPI from the same release flow
as the libvmaf fork. For an agent that needs a child-process tool surface:

```bash
pip install vmaf-mcp
```

For local development from a checkout:

```bash
cd mcp-server/vmaf-mcp
pip install -e .
```

Requirements and settings:

- MCP SDK 2.3.0 or newer and Pydantic 2.13.5 or newer;
  `mcp-server/vmaf-mcp/pyproject.toml`
  is the authority for the current floors.
- The SDK integration uses the 2.x low-level `on_list_tools` and
  `on_call_tool` handlers. This is an implementation compatibility boundary;
  the advertised MCP tools, their JSON schemas and the JSON-RPC transport
  stay unchanged for clients.
- Set `VMAF_BIN=/abs/path/to/vmaf` when the built CLI is not in one of the
  default locations, and set `VMAF_MCP_ALLOW` to any additional corpus roots
  the server may read.

!!! warning "Console script name"
    `vmafx-mcp` is the name of the Go server. The wheel installs `vmaf-mcp`
    for the Python server, plus a deprecated `vmafx-mcp` alias kept for one
    release (`mcp-server/vmaf-mcp/pyproject.toml`). The alias prints a
    deprecation notice on stderr and hands over to the Go binary when one is
    on `PATH`; with none, it runs the Python server. Use `vmaf-mcp` for the
    Python server and drop `vmafx-mcp` from client configs that meant it.

## Embedded server

Embedded-MCP users do not install `vmaf-mcp`. They build libvmaf with
`-Denable_mcp=true` and the needed transport flags, then call the
`libvmaf_mcp.h` C API from the host process.

The embedded server is not a separate package. Its public symbols live in
`libvmaf_mcp.h`, the implementation is compiled by `-Denable_mcp=true`, and
compatibility follows the libvmaf SOVERSION. A libvmaf build advertises the
embedded transports it compiled via `vmaf_mcp_transport_available()`, while
the Python package advertises the standalone CLI-wrapping tool surface.

## Release Checklist

For a libvmaf release:

1. Build with the intended MCP flags and run `test_mcp_smoke`.
2. Confirm `vmaf_mcp_available()` and `vmaf_mcp_transport_available()` match
   the release configuration.
3. Keep embedded MCP behavior documented in [`embedded.md`](embedded.md), not
   in the Python package README.

For a `vmaf-mcp` Python package release:

1. Build from `mcp-server/vmaf-mcp/`.
2. Keep the tool schemas in [`tools.md`](tools.md) aligned with
   `mcp-server/vmaf-mcp/src/vmaf_mcp/server.py`.
3. Publish and sign through the same release workflow used for the rest of
   the fork.

For the `vmafx-mcp` Go binary release:

1. Build from `cmd/vmafx-mcp/` with `go build -o vmafx-mcp ./cmd/vmafx-mcp`.
2. Run `go test ./cmd/vmafx-mcp/`. `TestToolListMatchesPython` and
   `TestToolSchemasMatchPython` compare the served tools with the Python
   server's tool list in `mcp-server/vmaf-mcp/tool-contract.json` (names,
   property types, required arguments) and refuse any tool that is neither
   in it nor declared Go-only; `TestVmafScoreTool` and
   `TestGoVsPythonOutputParity` additionally need the Netflix golden YUVs and
   the `vmaf` binary.
3. The tool count follows from step 2: the Python tools of the contract
   (19 today) plus the 5 control-plane tools.

!!! note
    The repository has no goreleaser configuration or workflow step that
    publishes a standalone `vmafx-mcp` binary. Per ADR-1229 the container
    images carry it at `/usr/local/bin/vmafx-mcp`; the shared release flow is
    in [`docs/development/release.md`](../development/release.md).

## PyPI Trusted Publisher

The Trusted Publisher binding of the `vmaf-mcp` project uses these exact
current repository identities:

| PyPI field   | Value              |
| ------------ | ------------------ |
| Project name | `vmaf-mcp`         |
| GitHub owner | `vidiko`           |
| Repository   | `vmafx`            |
| Workflow     | `supply-chain.yml` |
| Environment  | `pypi-publish`     |

Before publishing the GitHub draft, confirm the PyPI binding still matches
this table. Do not reuse the historical `lusoris/vmaf` identity from
ADR-0166; the repository was transferred and renamed after that accepted
decision.

### History

The Pending Trusted Publisher for the first PyPI publication was configured
on 2026-08-31. As of 2026-10-03 PyPI lists `vmaf-mcp` releases `1.0.0rc1`
and `1.0.0rc2`.

## See also

- [`docs/mcp/tools.md`](tools.md) — the MCP tool surface served by
  both flavours.
- [`docs/mcp/embedded.md`](embedded.md) — the embedded-server build
  flag and transport matrix.
- [`docs/development/release.md`](../development/release.md) — the
  shared release / signing pipeline.
- [ADR-0166](../adr/0166-mcp-server-release-channel.md) — design
  decision.
