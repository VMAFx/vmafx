# AGENTS.md — pkg/tune

Parent: [../../AGENTS.md](../../AGENTS.md). CLI wiring lives in
[cmd/vmafx-tune/AGENTS.md](../../cmd/vmafx-tune/AGENTS.md).

Go port of `vmaf-tune auto` and `vmaf-tune sidecar` surfaces. Python
originals stay in `tools/vmaf-tune/src/vmaftune/` until Go reaches
parity (ADR-0703 §Decision, ADR-0704 §Consequences), so **every
package here has live Python counterpart it must not drift from.**

| Package | Python counterpart |
| --------- | -------------------- |
| `auto/` | `vmaftune/auto.py` |
| `sidecar/` | `vmaftune/sidecar.py` |
| `executor/` | `vmaftune/executor.py` (`run_plan`), `encode.py`, `score.py` |

Shared layers these consume live **outside** `pkg/tune/`, one
implementation each (ADR-1137): `pkg/predictor` (`vmaftune/predictor.py`),
`pkg/codecadapter` (`vmaftune/codec_adapters/`), `pkg/hdr`
(`vmaftune/hdr.py`), `pkg/ffencode` (`vmaftune/encode.py`), `pkg/pyjson`
(CPython `json.dumps` and `vmaftune/jsonio.py`), `pkg/pymath` (CPython's
`2.0 ** x` and `math.log10` on platform libm). `pkg/tune/{hdr,pymath}`
shadows the parallel port produced are gone (moved to `pkg/hdr` /
`pkg/pymath`). `pkg/tune/{predictor,codec,pyjson}` are **transitional
thin aliases** — one file each, type aliases and one-line wrappers over
survivor, no tests of their own — kept only because `sidecar/` and
`cmd/vmafx-tune/cmd/sidecar.go` still import them; those files owned by
in-flight sidecar parity fix (#1187). Once #1187 lands, repoint sidecar
imports, delete three alias packages. Do not add to alias. Do not
re-grow shadow on rebase — repoint import.

## Rebase-sensitive invariants

1. **Plan JSON is byte-compatible with Python emitter, NaN token
   included** (`auto/auto.go` `EmitPlanJSON`, `pkg/pyjson`).
   `vmaftune.auto` serialises with plain
   `json.dumps(payload, indent=2, sort_keys=True)`; default
   `allow_nan=True` writes bare token `NaN`
   for uncalibrated conformal `interval_width` — every non-smoke run
   without `CellIntervals` seam produces it. Go side therefore
   **cannot** use `encoding/json`; plan goes through
   `pyjson.MarshalIndentSorted`, which reproduces CPython's spelling
   exactly: `NaN` / `Infinity` tokens, `repr()`'s mandatory `.0` on
   integral floats, fixed/exponential switch at
   `decpt <= -4 || decpt > 16`, `ensure_ascii=True` escaping.
   `TestEmitPlanJSONMatchesPython` diffs whole plans against fixtures
   generated from Python module. Do not "fix" NaN by switching to
   `pyjson.MarshalStrict` — would silently break every downstream
   consumer comparing two emitters. `--execute` JSONL rows and sidecar
   state file *are* strict (`dumps_strict` on both sides, i.e.
   `MarshalStrict`); asymmetry intentional.

2. **`pkg/pymath` is not micro-optimisation; it is parity layer**
   (`pkg/pymath/exp2.go`, `pkg/pymath/log10.go`). Go's `math.Pow` and
   `math.Log10` land one ULP away from platform libm CPython calls;
   both results reach user-visible JSON fields —
   `estimated_bitrate_kbps` via `2**((probe_quality − crf)/6)` in
   `auto/auto.go`, `estimated_vmaf` via `pkg/predictor` curve's
   `+ d·log10(bitrate)`. Swapping either back to stdlib moves last
   mantissa digit, fails parity fixtures. `Exp2` matches CPython
   exactly across whole `n/6` family planner produces (12,606
   vectors). `Log10` is correctly rounded, agrees with glibc on
   ~99.3% of random inputs versus stdlib's ~72%; residual is glibc's
   own rounding error, documented in package, not defect to chase.

3. **Short-circuit order is output contract** (`auto/auto.go`
   `ShortCircuitPredicates`). `plan.metadata.short_circuits` records
   firing order; post-hoc speedup analysis reads it. Append new
   predicates; never reorder or renumber existing ten. Each predicate
   stays pure function of `(SourceMeta, *PlanState)`, unit-testable in
   isolation.

4. **Recipe fires before ladder stage** (`auto/auto.go`, Stage 0 vs
   Stage 1). Content recipe can set `force_single_rung`; ladder stage
   has to see it. Moving recipe application after rung selection would
   silently drop that override on 4K sources. Only four documented
   keys (`tight_interval_max_width`, `force_single_rung`,
   `saliency_intensity`, `target_vmaf_offset`) survive into
   `metadata.recipe_overrides`; calibrator's `_provenance` blocks
   stripped.

5. **Recipe never widens production gate** (`auto/auto.go`
   `applyRecipeThresholds`). `target_vmaf_offset` shifts what
   *predictor* aims for. Must never shift gate deciding whether model
   ships. `wide_interval_min_width` preserved verbatim — recipe asking
   for tight gate wider than `wide` is capped, never allowed to invert
   `tight <= wide` invariant.

6. **Sidecar feature-vector layout pins every persisted weight**
   (`sidecar/sidecar.go` `FeatureVector`, `FeatureDim = 14`). Column
   order is index of every ridge weight on disk. Changing it —
   including adding column — requires bumping `SchemaVersion`, or
   older `state.json` will load with columns silently mis-assigned.
   Vector stops at `Width`: `Height` deliberately absent, matching
   Python builder.

7. **Predictor-version mismatch must discard fit**
   (`sidecar/sidecar.go` `ModelFromMap`, `Load`). Makes shipped-model
   upgrade safe: correction trained against old predictor can never be
   replayed against refreshed one. `Load` returns cold start rather
   than error for version, schema, or shape mismatch, and for corrupt
   JSON — leaves corrupt file in place so operator can inspect it.
   `stateDoc` decodes `weights` and `a_inv` through `*float64` so JSON
   `null` — what `Save` writes for NaN weight — is load failure (cold
   start) exactly as CPython's `float(None)` is, never silent zero.

8. **Host UUID is random, never machine-derived**
   (`sidecar/sidecar.go` `GetOrCreateHostUUID`). 128 bits from
   `crypto/rand`, persisted at cache-dir root, survives predictor
   upgrades. Never derive it from MAC address, hostname,
   `/etc/machine-id`, CPUID, or any other identifying signal — that
   property is precondition for any future opt-in upload.

9. **Cold start must return exactly `0.0`** (`sidecar/sidecar.go`
   `PredictCorrection`). With zero weights, dot product is exactly
   zero, so `sidecar.Predictor` degenerates to bare predictor until
   first capture. "Small epsilon" initialisation would silently
   perturb every untrained host's scores.

10. **Subprocess seam is load-bearing for testability**
    (`pkg/hdr.Runner`, `executor.Runner`). Every ffprobe / ffmpeg /
    vmaf invocation goes through injectable runner; whole test suite
    runs without those binaries installed. Never inline
    `exec.Command` into driver. Convention: non-zero exit reported
    through result; `error` return reserved for spawn failures, so
    callers can tell "the tool said no" from "the tool is not
    installed".

11. **Probe failure degrades, does not abort** (`auto/auto.go`
    `ProbeSourceMeta`, `pkg/hdr/hdr.go` `Detect`). Missing ffprobe,
    non-zero exit, or unparseable output all land on documented
    defaults (1920x1080, duration 0, SDR). HDR detection deliberately
    permissive in one direction only: misclassifying SDR as HDR would
    inject PQ signalling into gamma-2.4 encode, so PQ transfer without
    BT.2020 primaries treated as SDR.

12. **Executor's argv is `pkg/ffencode`'s; AMF tail emitted once**
    (`executor/executor.go` `BuildFFmpegCommand`, one-line wrapper).
    `EncodeRequest` is type alias of `ffencode.Request`; input-side
    `-ss` / `-t` placement, `DurationS` fallback and codec-adapter
    slice pinned by `executor_test.go` through wrapper. Argv builder
    is lenient — `(*codecadapter.Adapter).ResolveCodecArgs` passes
    out-of-vocabulary preset through verbatim — while package-level
    `codecadapter.ResolveCodecArgs` is strict gate; plan driver only
    ever emits `medium`, so lenient path unreachable from real plan.
    AMF family's inert duplicate `-quality / -rc / -qp_i / -qp_p` tail
    is **not** reproduced (ADR-1125, pkg/codecadapter `AGENTS.md`
    invariant 3); pre-ADR-1137 executor was only driver that still
    emitted it.

13. **Plan cells carry no `cell_index` or `preset`**
    (`executor/executor.go` `cellToEncodeRequest`, `makeRow`). Planner
    does not emit those keys, so executor falls back to index 0 and
    preset `medium`; JSONL row records both as `null`. That is Python
    behaviour; row schema read by downstream tooling. If planner ever
    starts emitting them, row shape changes, needs coordinated note in
    `docs/`.

## Regenerating the parity fixtures

Every `testdata/python_*.json` file here, plus fixtures that moved
with shared layers in ADR-1137
(`pkg/predictor/testdata/python_predictor.json`,
`pkg/codecadapter/testdata/python_adapters.json`,
`pkg/hdr/testdata/python_hdr.json`,
`pkg/pyjson/testdata/float_repr.txt`, `pkg/pymath` reference vectors),
dumped from in-tree Python implementation. Regenerate them **only**
alongside deliberate, coordinated change on both sides — silent
regeneration turns parity gate into tautology. Each fixture's loader
documents shape it expects.
