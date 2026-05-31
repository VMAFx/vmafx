<!-- markdownlint-disable MD013 MD060 -->
# ADR-0730: vmafx-tune Go port — Stage 2 (ladder subcommand)

- **Status**: Accepted
- **Date**: 2026-05-28
- **Deciders**: lusoris
- **Tags**: `go`, `vmafx-tune`, `language-modernization`, `cli`, `phase4`, `fork-local`

## Context

ADR-0705 shipped Stage 1 of the vmafx-tune Go port: the `compare` subcommand
as a standalone `vmafx-tune-go` binary, with all other subcommands as stubs
redirecting to the Python binary.

Stage 2 is the next porting milestone. The `ladder` subcommand is the most
impactful choice: it implements per-title ABR bitrate-ladder generation
(the Netflix per-title encoding paper's central idea), is the second most
commonly invoked subcommand after `compare`, and has the highest leverage ratio
— it composes the already-ported `compare` bisect engine (`pkg/bisect`) without
requiring new subprocess plumbing.

The Python `ladder.py` Phase E algorithm is well-specified:

1. Sample the `(resolution × VMAF-target)` plane via the Phase B bisect.
2. Compute the upper convex hull (Pareto frontier) of the `(bitrate, vmaf)` cloud.
3. Select a small set of "knee" renditions using curvature-based inflection
   detection (Kneedle-style perpendicular-distance recursion).
4. Emit a JSON payload compatible with the existing HLS/DASH manifest renderer.

## Decision

We ship `pkg/ladder/` as the Stage-2 core package and
`cmd/vmafx-tune/cmd/ladder.go` as the cobra subcommand.

New package:

- **`pkg/ladder/`** — `Build(src, encoder, Params)` function, convex hull
  (`upperConvexHull`), knee selection (`selectRenditions`), min-bitrate-gap
  filter (`applyBitrateGap`), and the `SamplerFn` seam.
  The `SamplerFn` mirrors the Python `ladder.py` type alias so tests can inject
  stub samplers without running ffmpeg or vmaf.

The ladder CLI subcommand wires `bisect.Run` as the production sampler,
composing the Stage-1 bisect engine for each `(resolution, target)` cell.
Hardware encoders from `pkg/encoder/hardware.go` (NVENC, QSV, AMF, SVT-AV1)
are supported via `encoder.NewExtended`.

Resolution-aware scaling (downscale the source before encoding each rendition)
is Stage-3 scope; Stage 2 bisects at the native source resolution and tags
points with the requested rendition resolution for hull and rendition tracking.
This matches the Python `ladder.py` behaviour when the source resolution equals
the target rendition resolution, which is the common case for single-clip
ladder sweeps.

The JSON output schema (`schema_version: 1`) is a superset of the Python
ladder output — it adds `target_vmaf` per cloud point as an optional field
but never removes existing fields (ADR-0705 schema-forward invariant).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|--------|------|------|----------------|
| Port `tune-per-shot` (Stage 2) | Exercises per-shot state machine early | Complex state machine, no shared infra with `compare`; slower to test | `ladder` composes the already-ported bisect engine directly |
| Port `report` (Stage 2) | No subprocess calls; pure render | Lower leverage — already usable via Python | Does not add a new user-facing encode path |
| Port `fast` (Stage 2) | Popular for NR-proxy accelerated workflows | Depends on NR-proxy model loading not yet in Go | Premature without an ONNX Runtime Go binding |
| Concurrent grid sampling | Lower wall time for large grids | Race-condition risk; Stage 1 lesson: correctness first | Stage 3 will add `--workers` once sequential path is validated |
| Full resolution-scaling in Stage 2 | True per-title ladder | Requires ffmpeg scale filter injection + ffprobe source dimension extraction; 2× scope | Deferred to Stage 3; Stage 2 validates hull + knee selection first |

## Consequences

- **Positive**: `vmafx-tune-go ladder` is immediately usable for ABR ladder
  generation at native source resolution, with libx264/libx265 and all
  Stage-2 hardware encoders.
- **Positive**: `pkg/ladder/` is fully unit-testable via `SamplerFn` injection;
  no ffmpeg or vmaf binary required for `go test ./pkg/ladder/`.
- **Positive**: JSON output is schema-compatible with the Python ladder renderer.
- **Negative**: Resolution-aware scaling deferred to Stage 3 — callers that need
  a true multi-resolution sweep must downscale sources externally or use the
  Python `vmaf-tune ladder`.
- **Neutral**: `cmd/vmafx-tune/cmd/root.go` updated to Stage-2 messaging;
  `ladder` removed from the stub list.

## References

- req: "Implement vmafx-tune Go Stage 2 (next subcommand after Stage 1's `compare`)" (user directive 2026-05-28)
- ADR-0705: vmafx-tune Go Stage 1 — `compare` subcommand
- ADR-0702: vmafx Phase 4 language-modernization umbrella
- `tools/vmaf-tune/src/vmaftune/ladder.py` Phase E algorithm
- `docs/adr/0295-vmaf-tune-phase-e-bitrate-ladder.md` (Python ladder design rationale)
- `docs/adr/0307-vmaf-tune-ladder-default-sampler.md` (default sampler canonical CRF sweep)
