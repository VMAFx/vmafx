<!-- markdownlint-disable MD013 MD060 -->
# ADR-0770: vmafx-tune Go port — Stage 4 (`report` subcommand)

- **Status**: Accepted
- **Date**: 2026-05-29
- **Deciders**: lusoris
- **Tags**: `go`, `vmafx-tune`, `language-modernization`, `cli`, `phase4`, `fork-local`

## Context

Stages 1–3 of the vmafx-tune Go port shipped:

- **Stage 1** (ADR-0705): `compare` subcommand, `pkg/encoder`, `pkg/bisect`, `pkg/report`
  (single-run Markdown/JSON rendering).
- **Stage 2** (ADR-0730): `ladder` subcommand, `pkg/ladder` (convex hull + knee selection).
- **Stage 3**: `pkg/conformal`, downscale plumbing, conformal interval support
  (CLI wiring deferred).

Stage 3 left `pkg/conformal` implemented but not wired to a subcommand.
Stage 4 candidates from the deferred list were:

| Candidate | Effort | User value | Blocker |
|-----------|--------|-----------|---------|
| `tune-per-shot` | High | High | Complex state machine, no shared infra |
| `report` | Low | High | None — pure rendering, no subprocess calls |
| `fast` | Medium | Medium | ONNX Go binding not yet available |
| Conformal CLI wiring | Low | Low | Narrow use case until conformal intervals are documented |

`report` is the optimal Stage-4 choice: it has no subprocess dependencies, is
immediately useful (any user who ran `compare` or `ladder` benefits), and
exercises the schema compatibility invariant end-to-end by unmarshalling its own
sibling subcommands' JSON output.

## Decision

Ship `cmd/vmafx-tune/cmd/report.go` as the Stage-4 cobra subcommand and extend
`pkg/report/` with a new file `multi.go` containing:

- **`CompareWirePayload` / `LadderWirePayload`** — JSON unmarshal targets for the
  schemas produced by `compare` and `ladder`.
- **`MultiReport`** — a normalised, renderer-neutral in-memory representation.
  Both wire schemas collapse into this shape so the Markdown and HTML renderers
  have a single code path.
- **`MultiReportFromCompare` / `MultiReportFromLadder`** — conversion constructors.
- **`RenderMarkdownMulti` / `RenderHTMLMulti`** — renderers.  HTML is
  self-contained (inlined CSS, no external CDN dependencies) so the output file
  works offline.

The subcommand accepts one or more JSON input files as positional arguments,
probes the top-level JSON keys (`renditions` vs `rows`) to auto-detect the
schema, and writes Markdown (default) or HTML to stdout or `--output`.

`root.go` is updated to register `report` and `ladder` as real subcommands
(removing their stubs) and update the help text to Stage-4.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|--------|------|------|----------------|
| Port `tune-per-shot` instead | Higher-leverage new capability | Complex per-shot state machine; no shared bisect infrastructure | Disproportionate scope for Stage 4 |
| Wire conformal CLI | Small scope | Low user visibility until conformal intervals are documented | Deferred to Stage 5 alongside docs |
| Use a template engine (html/template) | Structured HTML generation | Adds complexity without user-visible benefit at this scale | fmt.Fprintf is sufficient; no dynamic data |
| External CSS (CDN) | Smaller HTML file size | Output file requires network access to render | Inline CSS makes output portable |
| Merge rendering into existing report.go | Single file | Makes the Stage-1 wire schema entangled with Stage-4 rendering | Separate file keeps the Stage-1 schema invariant easy to audit |

## Consequences

- **Positive**: `vmafx-tune-go report results.json` is immediately usable
  after any `compare` or `ladder` run.
- **Positive**: `pkg/report/multi.go` is fully unit-testable without any
  subprocess calls; 10 table-driven tests cover both schemas, both renderers,
  sort order, XSS escaping, and empty/nil edge cases.
- **Positive**: HTML output is self-contained and renders offline.
- **Neutral**: `WireRow` uses `*float64` for nullable fields, deviating from
  the Stage-1 `Row` (which uses bare `float64` + a NaN convention).  The two
  types coexist; `WireRow` is the unmarshal-only shape, `Row` is the emit-only
  shape.
- **Negative**: `conformal` CLI wiring remains deferred; users who want
  conformal intervals must call `pkg/conformal` directly.

## References

- req: "Design + scaffold vmafx-tune Go Stage 4. Pick `report`." (user directive 2026-05-29)
- ADR-0705: vmafx-tune Go Stage 1 — `compare` subcommand
- ADR-0730: vmafx-tune Go Stage 2 — `ladder` subcommand
- ADR-0702: vmafx Phase 4 language-modernization umbrella
- `cmd/vmafx-tune/AGENTS.md` schema-forward invariant (JSON shape compatibility)
