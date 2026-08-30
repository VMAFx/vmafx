<!-- markdownlint-disable MD013 MD060 -->
# ADR-0705: vmafx-tune Go port — Stage 1 (compare subcommand)

- **Status**: Accepted
- **Date**: 2026-05-28
- **Deciders**: lusoris
- **Tags**: `go`, `vmafx-tune`, `language-modernization`, `cli`, `phase4`, `fork-local`

## Context

The Phase 4 language-modernization initiative (ADR-0702) establishes Go as the
target language for VMAFX CLI tooling. `tools/vmaf-tune/` is a 4,000+ line Python
CLI with multiple subcommands (`compare`, `tune-per-shot`, `ladder`, `fast`,
`corpus`, `report`, …). A full port in one PR would be unreviably large and would
block the Python binary during migration.

The user directed a staged approach: Stage 1 ports the single most commonly invoked
subcommand (`compare`) to a Go binary installed as `vmafx-tune-go` alongside the
existing Python binary, with all other subcommands as stubs that redirect to
`vmaf-tune`. Stage 2+ will port the remaining subcommands one milestone at a time.

The `compare` subcommand runs a rate-quality sweep: for each `(codec, VMAF target)`
pair it bisects the CRF axis to find the highest CRF whose measured VMAF still meets
the target, then reports bitrate and score. This is the most commonly invoked
subcommand and the most self-contained (no per-shot state, no ladder state machine).

## Decision

We ship `cmd/vmafx-tune/` as the Stage-1 Go binary under the name `vmafx-tune-go`.
The binary is wired via the existing `go.mod` at `github.com/VMAFx/vmafx` (Phase 4
foundation). The three new Go packages are:

- `pkg/encoder/` — `Encoder` interface + `LibX264Encoder` / `LibX265Encoder`
  (subprocess ffmpeg, no libavcodec cgo). Hardware encoders are Stage-2.
- `pkg/bisect/` — stateless `Run(src, enc, scoreFunc, params)` VMAF-target bisect
  mirroring the Python Phase B algorithm in `bisect.py`.
- `pkg/report/` — `EmitJSON` / `EmitMarkdown` renderers whose JSON schema matches
  the Python `compare.py` schema-v1 / schema-v2 so the existing `report.py`
  renderer can ingest Go output without modification.

The Python `tools/vmaf-tune/` is unchanged. The Go binary installs as
`vmafx-tune-go` (not `vmaf-tune`) to avoid a naming collision during the migration.
Stage 3 (swap) will rename when feature parity is reached.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|--------|------|------|----------------|
| Full port in one PR | Single migration event | 40,000+ LOC diff, unreviable, blocks Python users if bugs slip through | Too risky; staged approach is required by project policy (ADR-0108) |
| Port `tune-per-shot` first | Complex subcommand tests completeness | Harder to test in isolation; depends on per-shot state machine | `compare` is simpler and most commonly invoked |
| Use cgo for libavcodec | No ffmpeg subprocess overhead | Adds C build complexity, breaks static binary goal | Phase 1 goal is correctness and portability; subprocess is fine for rate-quality work |
| Ship under `vmaf-tune` name (replace) | Simpler UX | Any missing feature breaks existing users immediately | Migration strategy requires parallel coexistence until parity |

## Consequences

- `vmafx-tune-go compare` is immediately usable for libx264/libx265 rate-quality
  sweeps. Hardware encoders require Stage-2.
- The JSON output schema is compatible with the Python `report.py` renderer and
  all existing tooling that reads `vmaf-tune` JSON.
- `go.mod` adds `github.com/spf13/cobra` and `github.com/spf13/pflag` as
  dependencies.
- The bisect in `pkg/bisect/` is the test seam: unit tests inject a mock encoder
  and mock score function so tests run without ffmpeg or vmaf on PATH.

## References

- req: "Begin the Go port of vmaf-tune — Stage 1: minimum viable Go CLI" (user directive 2026-05-28)
- ADR-0702: vmafx Phase 4 language-modernization umbrella
- `tools/vmaf-tune/src/vmaftune/bisect.py` Phase B algorithm
- `tools/vmaf-tune/src/vmaftune/compare.py` schema-v1/v2 JSON shape
