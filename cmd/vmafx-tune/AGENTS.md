# AGENTS.md — cmd/vmafx-tune

Go port of the vmaf-tune rate-quality tuning CLI. Installed as `vmafx-tune-go`
during the migration; see Stage roadmap in ADR-0705 (Stage 1) and ADR-0730
(Stage 2).

## Rebase-sensitive invariants

1. **JSON schema compatibility** (`pkg/report/report.go`): the JSON output of
   `EmitJSON` must remain schema-compatible with the Python `compare.py` v1/v2
   payloads. The Python `report.py` renderer ingests this JSON directly. Any
   field rename or removal requires a coordinated Python-side change. Add new
   optional fields only; never remove existing ones without a schema-version bump.

2. **NaN coercion** (`pkg/report/report.go` `nanToNull`): `float64` fields that
   can be NaN (failed-row bitrate, VMAF, encode time) MUST be serialized as JSON
   `null`, not as bare `NaN` tokens. RFC 8259 strict parsers reject bare `NaN`.
   Mirror the Python `_nan_to_none` discipline.

3. **Bisect midpoint bias** (`pkg/bisect/bisect.go`): the midpoint rounds toward
   the *higher* CRF end `(lo + hi + 1) / 2` so the best-so-far record is never
   populated with an unvalidated CRF. Changing the rounding direction breaks the
   monotonicity invariant.

4. **ScoreFunc seam** (`pkg/bisect/bisect.go`): `ScoreFunc` is the subprocess
   boundary. Tests inject mock score functions. Never merge the score function
   inline into `Run`; the seam is load-bearing for unit testability.

5. **Stage-1 scope** (`pkg/encoder/encoder.go`): `encoder.New` accepts only
   `libx264` and `libx265`. Hardware encoders (NVENC, QSV, AMF) and SVT-AV1 are
   available via `encoder.NewExtended` (Stage 2). Do not add new encoder types
   without a new ADR and the associated hw-init flag plumbing.

6. **Binary name** (`cmd/vmafx-tune/main.go`): the binary installs as
   `vmafx-tune-go`, not `vmaf-tune`, during Stages 1–2 to avoid collisions with
   the Python binary. Stage 4 (swap) will rename. Never install it as
   `vmaf-tune` in a PR that does not also remove the Python entry point.

7. **Ladder SamplerFn seam** (`pkg/ladder/ladder.go`): `SamplerFn` is the
   subprocess boundary for the ladder subcommand, analogous to `ScoreFunc` in
   bisect. Tests inject stub samplers. Never make `Build` call `bisect.Run`
   directly; the seam is load-bearing for unit testability without ffmpeg/vmaf
   on PATH.

8. **Ladder JSON schema forward-compatibility** (`pkg/ladder/ladder.go`): the
   `ladderWirePayload` schema (`schema_version: 1`) must remain a superset of
   the Python `ladder.py` output. New optional fields may be added; existing
   field names must not change without a schema-version bump. The
   `cloud[].target_vmaf` and `cloud[].ok` fields are Go-additive and present
   as optional (`omitempty` on zero values where appropriate).

9. **Stage-2 resolution note**: Stage 2 bisects at native source resolution and
   tags points with the requested rendition `width`/`height` for hull tracking.
   Resolution-aware downscale (inject ffmpeg `scale=` filter before each encode)
   is Stage-3 scope. Do not add downscale logic in Stage 2 without a new ADR.

10. **Stage-3 contract**: Stage 3 should add `tune-per-shot`, concurrent grid
    sampling (`--workers` semaphore, mirroring Python `concurrent.futures`
    pool), and resolution-aware scaling. The `SamplerFn` seam already supports
    resolution context via `(width, height int)` parameters — Stage 3 only
    needs to inject the scale filter, not change the interface.
