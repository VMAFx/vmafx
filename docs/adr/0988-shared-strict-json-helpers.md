# ADR-0988: Route strict-JSON helpers through `vmaftune.jsonio` across vmaf-tune

- **Status**: Accepted
- **Date**: 2026-06-03
- **Deciders**: Lusoris
- **Tags**: `refactor`, `json`, `vmaf-tune`, `mcp`

## Context

`vmaftune.jsonio` (`tools/vmaf-tune/src/vmaftune/jsonio.py`) was introduced to
provide a single implementation of the RFC 8259 strict-JSON pattern used
throughout vmaf-tune: coerce non-finite floats to `null` then emit with
`allow_nan=False`.  Despite its existence, two modules within the same package
carried private copies of the same three-line helper:

- `vmaftune.compare` — private `_nan_to_none` + inline in `_emit_json` and
  `emit_sweep_json`.
- `vmaftune.report` — private `_nan_to_none` + `_portable_json_dump`.
- `vmaftune.benchmark` — `render_json` called `json.dumps` without NaN
  protection.

Additionally, the MCP server (`vmaf_mcp.server`) serialized every tool result
via a bare `json.dumps(result, indent=2)` without NaN protection.  Any VMAF
score payload that contained a `NaN` metric (possible with partial-decode or
failed-row paths) would emit non-RFC-8259 JSON that Go / Rust / `jq` strict
parsers reject.

## Decision

Remove all private copies of `_nan_to_none` and the associated inline
`json.dumps(..., allow_nan=False)` calls from `compare`, `report`, and
`benchmark`, replacing them with imports from `vmaftune.jsonio.dumps_strict`.

For `vmaf_mcp.server`, which does not depend on `vmaf-tune`, introduce a
small inlined `_nan_to_none` / `_dumps_strict` pair with a doc-comment
pointing to `vmaftune.jsonio` as the canonical source.  A cross-package
dependency is not warranted for three lines of helper code.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Extract a shared `vmaf-common` package | Single canonical source for both packages | New package, new wheel, new version lifecycle | Over-engineered for three lines; cross-package dep costs more than it saves |
| Leave private copies in place | Zero change risk | Divergence risk (one copy acquires a fix the other misses); contradicts DRY | Duplication already present; ADR-0678 mandates helper routing |
| Only fix compare/report, skip MCP and benchmark | Smaller PR scope | MCP final serialization still unprotected | All RFC 8259 emit surfaces should be consistent |

## Consequences

- **Positive**: the three in-package duplicates are gone; future fixes to
  `nan_to_none` (e.g. `decimal.Decimal` support) propagate automatically to
  `compare`, `report`, and `benchmark`.  MCP tool results are now RFC 8259
  clean even when a backend emits `NaN` metrics.
- **Negative**: `vmaf_mcp.server` retains a local copy; it must be kept in
  sync with `vmaftune.jsonio` manually.  The `# ADR-0988` comment in the
  inlined copy is the synchronisation anchor.
- **Neutral / follow-ups**: `cli.py`, `auto.py`, `ladder.py`, and
  `conformal.py` each still use bare `json.dumps`; those are out of scope for
  this PR.  A follow-up sweep can use `jsonio.dumps_strict` there once the
  immediate duplicates are cleared.

## References

- ADR-0678 — helper-routing policy (shared helpers over private copies).
- `tools/vmaf-tune/src/vmaftune/jsonio.py` — canonical implementation.
