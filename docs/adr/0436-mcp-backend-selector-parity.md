<!-- markdownlint-disable MD060 -->
# ADR-0436: MCP server backend-selector parity

- **Status**: Accepted
- **Date**: 2026-05-15
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: mcp, agents, api, dispatch, fork-local

## Context

The Python MCP server at `mcp-server/vmaf-mcp/src/vmaf_mcp/server.py`
exposes two tools that take a `backend` selector (`vmaf_score`,
`describe_worst_frames`) and a third tool that enumerates available
backends (`list_backends`). The 2026-05-15 deep audit (slice D §D.1.1)
flagged that the JSON-schema `enum` only listed `auto / cpu / cuda /
sycl` — `vulkan`, `hip`, and `metal` were silently dropped.

Concretely:

- The tool's `inputSchema.properties.backend.enum` rejected requests
  with `backend="vulkan"` / `"hip"` / `"metal"` at the JSON-schema
  layer, so MCP clients had no way to ask for those backends.
- Even if the enum had been bypassed, `_run_vmaf_score()` only
  emitted `--no_*` toggles for cpu / cuda / sycl — `vulkan`, `hip`,
  `metal` selections fell through to `auto`, picking whichever the
  host probe happened to surface.
- `_list_backends()` returned a 4-key dict (`cpu`, `cuda`, `sycl`,
  `hip`) that omitted `vulkan` and `metal` entirely.

The libvmaf CLI itself (`core/tools/cli_parse.c`) has shipped
`--no_vulkan`, `--no_hip`, and `--no_metal` for months; the MCP
surface had simply not caught up.

## Decision

Bring the MCP server's backend-selector surface to full parity with
libvmaf's CLI by:

1. Extending `ScoreRequest.backend` and the `inputSchema.enum` for
   both `vmaf_score` and `describe_worst_frames` to accept
   `auto / cpu / cuda / sycl / vulkan / hip / metal`.
2. Adding the corresponding `--no_<sibling>` flag emission in
   `_run_vmaf_score()` so each explicit selection disables every
   sibling backend (auto stays bare).
3. Returning all six keys from `_list_backends()` with a host-probe
   that scans `vmaf --version` for `vulkan` and `metal` substrings
   in addition to `cuda` / `sycl` / `hip`.
4. Updating the `list_backends` tool description and the module
   docstring header to enumerate all six backends.
5. Adding `tests/test_backend_dispatch.py` with one parametrised
   case per backend that asserts the exact `--no_*` flag set
   reaching argv (mocking `asyncio.create_subprocess_exec`).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Extend enum + dispatch (chosen)** | Minimal, mechanical, mirrors what libvmaf already exposes; tests prove correctness without GPU | None — this is the obvious gap closure | Chosen |
| **Reject unknown backends with clear error** | Forces clients to update | Breaks MCP clients that already pass `vulkan`/etc. expecting it to "work" via auto | Worse UX; the silent fall-through is a real bug, not a feature |
| **Drop the explicit `backend` parameter entirely** | Smallest schema | Removes a load-bearing dispatch knob; cross-backend probing from the MCP layer requires it | Workflow regression |
| **Add a generic `extra_argv` escape hatch** | Maximum flexibility | Foot-gun: lets MCP clients pass arbitrary CLI flags | Out of scope; security review needed |

## Consequences

### Positive

- MCP clients can now pin a specific backend for any of the six
  backends libvmaf supports.
- `_list_backends()` becomes a faithful per-backend availability
  report, suitable for client-side dispatch decisions.
- Cross-backend ULP probing (per `cross-backend-diff` skill) becomes
  fully driveable from the MCP layer.

### Negative

- Slightly larger argv when a single backend is selected (5 sibling
  flags instead of 1–2). Not measurable in subprocess setup time.
- The `vulkan` / `hip` / `metal` substring probes in
  `_list_backends()` are heuristic — if libvmaf's `--version`
  output ever stops mentioning the backend by name, the probe
  silently regresses. Mitigated by the probe defaulting to `False`,
  matching the pre-PR behaviour for those backends.

### Neutral

- ADR-0451 (dev-MCP container) ships a build with all six backends
  enabled, which lets the smoke-probe loop exercise this surface
  end-to-end as soon as both PRs merge.

## References

- Audit slice D §D.1.1 (`.workingdir/audit-2026-05-15/D-mcp-and-backends.md`).
- `core/tools/cli_parse.c` — source of the `--no_<backend>` flags.
- ADR-0451 — local dev-MCP container that exercises this surface.
- Test fixture: `mcp-server/vmaf-mcp/tests/test_backend_dispatch.py`.
