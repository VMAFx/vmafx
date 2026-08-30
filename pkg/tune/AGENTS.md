# AGENTS.md — pkg/tune

Parent: [../../AGENTS.md](../../AGENTS.md). CLI wiring lives in
[cmd/vmafx-tune/AGENTS.md](../../cmd/vmafx-tune/AGENTS.md).

Go port of the `vmaf-tune auto` and `vmaf-tune sidecar` surfaces, plus the
shared machinery both need. The Python originals stay in
`tools/vmaf-tune/src/vmaftune/` until Go reaches parity (ADR-0703 §Decision,
ADR-0704 §Consequences), so **every package here has a live Python
counterpart that it must not drift from.**

| Package | Python counterpart |
|---------|--------------------|
| `auto/` | `vmaftune/auto.py` |
| `sidecar/` | `vmaftune/sidecar.py` |
| `predictor/` | `vmaftune/predictor.py` |
| `codec/` | `vmaftune/codec_adapters/` |
| `hdr/` | `vmaftune/hdr.py` |
| `executor/` | `vmaftune/executor.py` (`run_plan`), `encode.py`, `score.py` |
| `pyjson/` | CPython `json.dumps`, `vmaftune/jsonio.py` |
| `pymath/` | CPython's `2.0 ** x` and `math.log10` on the platform libm |

## Rebase-sensitive invariants

1. **The plan JSON is byte-compatible with the Python emitter, NaN token
   included** (`auto/auto.go` `EmitPlanJSON`, `pyjson/`). `vmaftune.auto`
   serialises with plain `json.dumps(payload, indent=2, sort_keys=True)`, whose
   default `allow_nan=True` writes the bare token `NaN` for an uncalibrated
   conformal `interval_width` — which every non-smoke run without a
   `CellIntervals` seam produces. The Go side therefore **cannot** use
   `encoding/json`, and `pkg/tune/pyjson` exists to reproduce CPython's
   spelling exactly: the `NaN` / `Infinity` tokens, `repr()`'s mandatory `.0`
   on integral floats, its fixed/exponential switch at
   `decpt <= -4 || decpt > 16`, and `ensure_ascii=True` escaping.
   `TestEmitPlanJSONMatchesPython` diffs whole plans against fixtures generated
   from the Python module. Do not "fix" the NaN by switching to
   `MarshalStrict` — that would silently break every downstream consumer
   comparing the two emitters. The `--execute` JSONL rows *are* strict
   (`dumps_strict` on both sides); that asymmetry is intentional.

2. **`pymath` is not a micro-optimisation; it is the parity layer**
   (`pymath/exp2.go`, `pymath/log10.go`). Go's `math.Pow` and `math.Log10` land
   one ULP away from the platform libm CPython calls, and both results reach
   user-visible JSON fields — `estimated_bitrate_kbps` via `2**((probe_quality −
   crf)/6)`, and `estimated_vmaf` via the predictor curve's
   `+ d·log10(bitrate)`. Swapping either back to the stdlib moves the last
   mantissa digit and fails the parity fixtures. `Exp2` matches CPython exactly
   across the whole `n/6` family the planner produces (12,606 vectors). `Log10`
   is correctly rounded, which agrees with glibc on ~99.3% of random inputs
   versus the stdlib's ~72%; the residual is glibc's own rounding error and is
   documented in the package, not a defect to chase.

3. **Short-circuit order is the output contract** (`auto/auto.go`
   `ShortCircuitPredicates`). `plan.metadata.short_circuits` records firing
   order, and post-hoc speedup analysis reads it. Append new predicates; never
   reorder or renumber the existing ten. Each predicate stays a pure function
   of `(SourceMeta, *PlanState)` so it is unit-testable in isolation.

4. **The recipe fires before the ladder stage** (`auto/auto.go`, Stage 0 vs
   Stage 1). A content recipe can set `force_single_rung`, and the ladder stage
   has to see it. Moving the recipe application after the rung selection would
   silently drop that override on 4K sources. Only the four documented keys
   (`tight_interval_max_width`, `force_single_rung`, `saliency_intensity`,
   `target_vmaf_offset`) survive into `metadata.recipe_overrides`; the
   calibrator's `_provenance` blocks are stripped.

5. **The recipe never widens a production gate** (`auto/auto.go`
   `applyRecipeThresholds`). `target_vmaf_offset` shifts what the *predictor*
   aims for. It must never shift the gate that decides whether a model ships,
   and `wide_interval_min_width` is preserved verbatim — a recipe asking for a
   tight gate wider than `wide` is capped, never allowed to invert the
   `tight <= wide` invariant.

6. **The sidecar feature-vector layout pins every persisted weight**
   (`sidecar/sidecar.go` `FeatureVector`, `FeatureDim = 14`). The column order
   is the index of every ridge weight on disk. Changing it — including adding a
   column — requires bumping `SchemaVersion`, or an older `state.json` will
   load with its columns silently mis-assigned. Note the vector stops at
   `Width`: `Height` is deliberately absent, matching the Python builder.

7. **A predictor-version mismatch must discard the fit** (`sidecar/sidecar.go`
   `ModelFromMap`, `Load`). This is what makes a shipped-model upgrade safe: a
   correction trained against the old predictor can never be replayed against a
   refreshed one. `Load` returns cold start rather than an error for a version,
   schema, or shape mismatch, and for corrupt JSON — and it leaves the corrupt
   file in place so an operator can inspect it.

8. **The host UUID is random, never machine-derived** (`sidecar/sidecar.go`
   `GetOrCreateHostUUID`). 128 bits from `crypto/rand`, persisted at the
   cache-dir root so it survives predictor upgrades. Never derive it from a MAC
   address, hostname, `/etc/machine-id`, CPUID, or any other identifying
   signal — that property is the precondition for any future opt-in upload.

9. **Cold start must return exactly `0.0`** (`sidecar/sidecar.go`
   `PredictCorrection`). With zero weights the dot product is exactly zero, so
   `sidecar.Predictor` degenerates to the bare predictor until the first
   capture. A "small epsilon" initialisation would silently perturb every
   untrained host's scores.

10. **The subprocess seam is load-bearing for testability** (`hdr.Runner`,
    `executor.Runner`). Every ffprobe / ffmpeg / vmaf invocation goes through an
    injectable runner, and the whole test suite runs without those binaries
    installed. Never inline `exec.Command` into a driver. Note the convention:
    a non-zero exit is reported through the result, and the `error` return is
    reserved for spawn failures, so callers can tell "the tool said no" from
    "the tool is not installed".

11. **Probe failure degrades, it does not abort** (`auto/auto.go`
    `ProbeSourceMeta`, `hdr/hdr.go` `Detect`). A missing ffprobe, a non-zero
    exit, or unparseable output all land on the documented defaults
    (1920x1080, duration 0, SDR). HDR detection is deliberately permissive in
    one direction only: misclassifying SDR as HDR would inject PQ signalling
    into a gamma-2.4 encode, so a PQ transfer without BT.2020 primaries is
    treated as SDR.

12. **`codec.FFmpegCodecArgs` is lenient; `codec.Validate` is strict**
    (`codec/codec.go`). The argv builder tolerates an out-of-vocabulary preset
    and an out-of-window quality, falling back to the adapter's documented
    default, exactly as the Python adapters do. `Validate` is the gate that
    rejects them. Do not collapse the two. The AMF family's argv repeats its
    `-quality / -rc / -qp_i / -qp_p` tail because the Python adapter emits it
    from both `ffmpeg_codec_args` and `extra_params`; ffmpeg takes the last
    occurrence so the duplication is inert, and it is reproduced deliberately
    to keep a Go-vs-Python argv diff empty.

13. **Plan cells carry no `cell_index` or `preset`** (`executor/executor.go`
    `cellToEncodeRequest`, `makeRow`). The planner does not emit those keys, so
    the executor falls back to index 0 and preset `medium`, and the JSONL row
    records both as `null`. That is the Python behaviour, and the row schema is
    read by downstream tooling. If the planner ever starts emitting them, the
    row shape changes and needs a coordinated note in `docs/`.

## Regenerating the parity fixtures

Every `testdata/python_*.json` file (and the `pymath` reference vectors) was
dumped from the in-tree Python implementation. Regenerate them **only**
alongside a deliberate, coordinated change on both sides — a silent
regeneration turns the parity gate into a tautology. Each fixture's loader
documents the shape it expects.
