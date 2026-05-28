# AGENTS.md — cmd/vmafx-tune

Go port of the vmaf-tune rate-quality tuning CLI. Installed as `vmafx-tune-go`
during the migration; see Stage roadmap in ADR-0705 (Stage 1), ADR-0730
(Stage 2), and ADR-0734 (Stage 3).

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

9. **Resolution-aware downscaling** (`pkg/encoder/encoder.go`, Stage-3):
   `EncodeParams.ScaleWidth`/`ScaleHeight` inject a `scale=W:H:flags=lanczos`
   filter. When BOTH values are zero, no filter is injected. Partial zero (one
   set, one not) is undefined — always set both or neither. QSV's
   `injectQSVInitChain` merges the scale filter with the hw-upload chain and
   clears the fields to prevent `runEncode` from injecting a second `-vf`.

10. **Workers semaphore contract** (`pkg/ladder/ladder.go`, Stage-3):
    `Build` pre-allocates `cloud` with `totalCells` slots and dispatches one
    goroutine per cell. Each goroutine writes only to its own index
    (`ri*nTgt + ti`). The semaphore (`chan struct{}` of size `Workers`) limits
    concurrency. Do not append to `cloud` from goroutines — always write to
    the pre-allocated slot. Changing the indexing formula requires updating
    both the allocation and all goroutine writes in lockstep.

11. **Conformal interval contract** (`pkg/conformal/conformal.go`, Stage-3):
    `conformal.Compute` requires `len(samples) ≥ 2` and `coverage ∈ (0, 1)`.
    The half-width is the empirical quantile at index
    `⌈(n+1)*(1-coverage)⌉ − 1` (0-indexed, clamped to `[0, n-1]`).
    Do not change the quantile formula without re-validating the coverage
    guarantee. `MeetsTarget` and `RejectableWithHighConfidence` are
    convenience methods — they do not mutate the interval.

12. **Bitrate bisect convergence contract** (`pkg/bitratesearch/bitratesearch.go`,
    Stage-3): the binary search maintains the invariant that `bestBitrateKbps`
    is always the *lowest* bitrate seen so far that meets the target. The
    bisect narrows `hi = mid` (not `hi = mid-1`) when the midpoint meets the
    target, so `mid` is itself included in the remaining window. Do not change
    this to integer-kbps bisect without updating `encodeAtBitrate` to round
    the bitrate string consistently.

13. **Bisect JSON schema forward-compatibility** (`cmd/vmafx-tune/cmd/bisect.go`):
    `bisectWirePayload` schema (`schema_version: 1`) must remain a superset of
    the Python `vmaf-tune bisect` JSON output. Field names must not change.
    `best_bitrate_kbps = -1` is the sentinel for "no bitrate meets target" —
    do not change to `null` without a schema-version bump and Python-side
    coordination.

14. **Stage-4 contract**: Stage 4 should add conformal interval CLI wiring to
    the `ladder` sampler (collecting M samples per grid cell), `tune-per-shot`,
    and `report`. The `pkg/conformal` package is ready; the CLI seam is the
    `SamplerFn` — Stage 4 wraps it with a multi-sample conformal sampler.
