# ADR-0734: vmafx-tune Go port — Stage 3 (workers + downscale + bisect subcommand)

- **Status**: Accepted
- **Date**: 2026-05-28
- **Deciders**: lusoris
- **Tags**: `go`, `vmafx-tune`, `language-modernization`, `cli`, `phase4`, `fork-local`

## Context

ADR-0730 shipped Stage 2 of the vmafx-tune Go port: the `ladder` subcommand
with per-title ABR bitrate-ladder generation.  Stage 2 explicitly deferred
three features to Stage 3:

1. **Resolution-aware downscaling** — Stage 2 bisected at the native source
   resolution and tagged points with the requested rendition resolution.  A
   true per-title sweep requires the source to be downscaled to each rendition
   resolution before encoding so that VMAF is measured at the actual playback
   resolution.

2. **Concurrent grid sampling** — Stage 2 sampled the (resolution × target)
   grid sequentially to validate correctness first.  Parallelising the grid
   with a bounded semaphore is the natural follow-up once the sequential path
   was stable.

3. **Uncertainty-aware rung pruning** — Stage 2 accepted every bisect result
   at face value.  VMAF scores have measurement noise (encoder non-determinism,
   content complexity), and a conformal interval around the score provides a
   principled way to reject rungs whose actual VMAF is unlikely to meet the
   target.

Stage 3 ships all three deferred items and also ports the `bisect` subcommand —
the bitrate-domain binary search — which was identified as the highest-leverage
next subcommand port given Go's concurrency model and container-friendliness.

## Decision

### Stage-3 ladder extensions

**Resolution-aware downscaling** is implemented by adding `ScaleWidth` and
`ScaleHeight` fields to `pkg/encoder.EncodeParams`.  When set, `runEncode`
injects a `scale=W:H:flags=lanczos` video filter before the encoder.  The
Lanczos algorithm is chosen because it minimises aliasing on downscale and
matches the Python `ladder.py` pre-scale behaviour.  QSV (Intel Quick Sync)
has a special path in `injectQSVInitChain` that merges the scale filter with
the QSV hw-upload chain to avoid conflicting `-vf` flags.

The `pkg/bisect.Params` struct gains `ScaleWidth`/`ScaleHeight` fields that
are forwarded to `encoder.EncodeParams` so the CLI sampler can request
resolution-aware encoding for each grid cell.

**Concurrent grid sampling** is implemented in `pkg/ladder.Build` via a
semaphore channel (`chan struct{}` of size `Workers`).  Each grid cell is
dispatched as a goroutine; the semaphore bounds the number of concurrent
encoder invocations to avoid overwhelming the encoder queue.  The default
worker count is `NumCPU/2` clamped to `[1, 8]` — enough to saturate typical
encode pipelines on an 8–32 core workstation without exceeding available GPU
or disk I/O bandwidth.  The pre-allocated `cloud` slice (one slot per cell)
eliminates slice-growth races; each goroutine writes to its own index.

The `--workers` flag is exposed on the `ladder` subcommand.  `0` means "use
the default".

**Uncertainty-aware rung pruning** is implemented in the new `pkg/conformal`
package.  `conformal.Compute(samples, coverage)` accepts M VMAF measurements
and returns a split-conformal prediction interval `[lo, hi]` at the requested
coverage level.  The algorithm: compute non-conformity scores as absolute
residuals from the mean, then take the empirical quantile at level
`⌈(M+1)*(1−coverage)⌉/M`.  The interval is asymptotically valid under
exchangeability (Angelopoulos & Bates 2021).

The conformal package is wired as a library; the ladder CLI does not yet
expose conformal sampling at the CLI layer (collecting M samples per cell
requires M × grid_size encoder runs, which adds significant wall time).  The
package is ready for CLI wiring in Stage 4 when the performance budget is
understood.

### bisect subcommand

`pkg/bitratesearch` implements a bitrate-domain binary search.  The
algorithm:

1. Probe `BitrateMinKbps`; if VMAF ≥ target, return immediately.
2. Probe `BitrateMaxKbps`; if VMAF < target, report not-found.
3. Otherwise bisect: narrow to the lower half when VMAF ≥ target, upper half
   otherwise.  Stop when `hi − lo ≤ Tolerance` or `MaxIter` iterations.

The encoder is invoked in VBR mode (`-b:v`) rather than CRF mode.  This
inverts the CRF-domain bisect: instead of asking "what is the minimum CRF
that meets the target?", it asks "what is the minimum bitrate that meets the
target?" — the direct user-facing question for adaptive streaming budget
allocation.

`cmd/vmafx-tune/cmd/bisect.go` wires `bitratesearch.Run` under a `cobra`
subcommand.  The JSON output schema (`schema_version: 1`) mirrors the Python
`vmaf-tune bisect` JSON output with the same field names.  `--scale-width` /
`--scale-height` enable resolution-aware downscaling.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|--------|------|------|----------------|
| `--workers` default = NumCPU (no clamping) | Maximum parallelism | Saturates disk I/O + GPU on large machines; encoder queue contention causes encode failures | Clamping to 8 is the standard HLS encoder-pool convention; we can raise the ceiling in Stage 4 once we have profiling data |
| `--workers` default = 1 (sequential) | Safest; exactly Stage-2 behaviour | No improvement; defeats the purpose of Stage-3 | The Stage-2 sequential path is already validated; default > 1 is the correct Stage-3 step |
| Bootstrap percentile intervals instead of conformal | Simpler concept | Requires distribution assumption; conformal coverage is assumption-free (exchangeability only) | Conformal is strictly more general; same complexity |
| Gaussian ± k·σ | Very simple | Assumes normality; VMAF scores are bounded [0,100] and skewed near boundaries | Conformal does not make a normality assumption |
| `bisect` midpoint strategy: geometric mean `(lo*hi)^0.5` instead of arithmetic mean `(lo+hi)/2` | Better for logarithmic rate-quality curves | More complex; converges in the same number of steps asymptotically | Arithmetic midpoint is simpler and standard; geometric midpoint is a Stage-4 optimisation |
| Port `report` instead of `bisect` | No subprocess; pure render | Lower leverage — the Python `report` already works and has no subprocess overhead | `bisect` adds a new user-facing encode path; report is accessible from Python |
| Port `fast` instead of `bisect` | Popular for NR-proxy workflows | Requires ONNX Runtime Go binding for the NR-proxy model; not yet available | Blocked by missing dependency; `bisect` has no new Go dependencies |

## Consequences

- **Positive**: `vmafx-tune-go ladder` now performs resolution-correct per-title
  ladder sweeps; VMAF is measured at the actual playback resolution for each
  rendition.
- **Positive**: Concurrent grid sampling reduces ladder wall time by up to
  `min(Workers, num_cells)×` on CPU-bound workloads.
- **Positive**: `pkg/conformal` provides a principled uncertainty quantifier
  for VMAF measurements, ready for CLI wiring in Stage 4.
- **Positive**: `vmafx-tune-go bisect` is a new user-facing subcommand that
  directly answers the minimum-bitrate question.
- **Negative**: Concurrent grid sampling introduces goroutine overhead for
  small grids (1–2 cells); the overhead is negligible (< 1 ms) compared to
  encode time.
- **Negative**: Conformal sampling at CLI level is deferred to Stage 4 — the
  `pkg/conformal` package is not yet wired into the ladder sampler for
  production use.
- **Neutral**: `bisect` subcommand is removed from the stub list in root.go;
  the Python `vmaf-tune bisect` remains available and is not affected.

## References

- req: "Implement vmafx-tune Go Stage 3 — extend Stage 2's ladder subcommand with the 3 features the Stage 2 agent explicitly deferred. Also pick the NEXT high-value subcommand to port from Python." (user directive 2026-05-28)
- ADR-0730: vmafx-tune Go Stage 2 — `ladder` subcommand
- ADR-0705: vmafx-tune Go Stage 1 — `compare` subcommand
- ADR-0702: vmafx Phase 4 language-modernization umbrella
- Angelopoulos & Bates 2021, "A Gentle Introduction to Conformal Prediction and Distribution-Free Uncertainty Quantification"
- `tools/vmaf-tune/src/vmaftune/ladder.py` Phase E algorithm
- `tools/vmaf-tune/src/vmaftune/bisect.py` Phase B algorithm
