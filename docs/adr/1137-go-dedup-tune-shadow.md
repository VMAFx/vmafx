<!-- markdownlint-disable MD013 MD041 -->

# ADR-1137: One implementation per shared Go package — folding the vmafx-tune shadow packages

- **Status**: Accepted
- **Date**: 2026-09-02
- **Deciders**: Lusoris
- **Tags**: `go`, `vmafx-tune`, `python-sunset`, `refactor`, `fork-local`

## Context

[ADR-1125](1125-vmafx-tune-go-port-integration.md) merged seven parallel
`vmafx-tune` Go ports onto one branch. It reconciled the packages whose import
paths collided in git, and explicitly left the rest as follow-up: four
CPython-JSON encoders at paths that never collided, and a `pkg/tune/` subtree
whose packages shadow shared ones. Measured on the merge-base of this change:

| Duplicate | Copies | Where | Non-test LOC |
| --- | --- | --- | --- |
| CPython `json.dumps` encoder | 4 | `pkg/pyjson`, `internal/pyjson`, `internal/pyjsonstrict`, `pkg/tune/pyjson` | 405 + 338 + 325 + 338 = 1,406 |
| `BuildFFmpegCommand` + `ParseVersions` + the `ffmpeg -version` probe table | 4 | `pkg/ffencode`, `pkg/corpus/encode.go`, `pkg/encodeprofile/encode.go`, `pkg/tune/executor` | ~130 each |
| codec-adapter registry | 2 | `pkg/codecadapter`, `pkg/tune/codec` | 629 / 458 |
| analytical predictor + `PickCRF` | 2 | `pkg/predictor`, `pkg/tune/predictor` | 1,182 / 269 |
| HDR detection + codec-flag dispatch | 2 | `pkg/tune/hdr`, `pkg/corpus/hdr.go` | 445 / 472 |
| `pymath` (correctly-rounded `Exp2` / `Log10`) | 1, misplaced | `pkg/tune/pymath`, consumed by `pkg/predictor` outside `pkg/tune` | 279 |
| ORT-session wiring behind `--model` | 2 | `cmd/vmafx-tune/cmd/ortsession.go` (`predict`), the registry-based ONNX path inside `pkg/tune/predictor` (`auto`, `sidecar`) | ~60 / ~50 |

Nine files carried their own CPython float spelling on top of the four
encoders (`pkg/ffencode`'s `formatFloat`, the tree-based helpers in
`pkg/pershot`, `pkg/fast`, `pkg/recommend`, `pkg/prefilter`).

The duplicates were not merely redundant; they had already diverged in ways
the per-package test suites could not see:

- `pkg/predictor` once computed its curve with `math.Log10` while
  `pkg/tune/predictor` used the `pymath` port, so `predict` and `auto`
  disagreed on the same input on ~27% of realistic probe bitrates (fixed in
  ADR-1125's integration, but only because someone noticed).
- `pkg/tune/executor` reproduced the AMF adapters' inert duplicate
  `-quality/-rc/-qp_i/-qp_p` tail; `pkg/ffencode`, `pkg/corpus` and
  `pkg/encodeprofile` — via `pkg/codecadapter` — deliberately did not
  (ADR-1125 §Consequences). Two encode drivers, two argv shapes for the same
  cell.
- `pkg/corpus/hdr.go` rounded the content-light `max_content` / `max_average`
  values; `pkg/tune/hdr` truncated them the way Python's `int()` does.
- `pkg/tune/predictor` invented a per-codec fallback for a caller-supplied
  coefficient table; `pkg/predictor` and the Python fall back to the libx264
  curve.
- `pkg/ffencode`'s `formatFloat` used Go's shortest `%g`, which switches to
  exponent form at 1e6 where CPython's `repr()` keeps fixed notation up to
  1e16 (measured: `strconv.FormatFloat(1e6, 'g', -1, 64)` is `1e+06`,
  `repr(1e6)` is `1000000.0`).
- The four encoders disagreed on nil containers: the three tree-based ones
  rendered a nil `[]any` as `[]`, the reflect-based `internal/pyjson` as
  `null`.

ADR-1125 also recorded that `internal/pyjson` and `internal/pyjsonstrict`
were "deliberately two packages" because they mirror two Python entry points
(`json.dumps` versus `jsonio.dumps_strict`). That reasoning does not survive
inspection: the two entry points differ in exactly one bit — how a non-finite
float is spelled — and the encoders were measured redundant on everything else
(200,000 random `float64` bit patterns and 10,000 rendered payloads, zero
disagreements). One `Options` field expresses the difference.

## Decision

We keep **one implementation of each shared layer, at a shared path outside
`pkg/tune/`**, and delete the shadows:

| Layer | Kept | Deleted / moved |
| --- | --- | --- |
| CPython JSON | `pkg/pyjson` (rewritten: reflect-based, `Options{SortKeys, Indent, NonFinite}`, `FloatRepr` / `FormatFloat` / `EncodeString`, the `MarshalSorted` / `MarshalIndentSorted` / `MarshalStrict` conveniences, the sentinel reader) | `internal/pyjson`, `internal/pyjsonstrict` deleted; `pkg/tune/pyjson`'s implementation deleted (the path stays as a transitional alias, see below); `pkg/ffencode.formatFloat` deleted |
| ffmpeg encode argv + version parsing | `pkg/ffencode` (`BuildFFmpegCommand`, `InputArgs`, `ParseVersions`, `ProbePattern`) | `pkg/corpus`, `pkg/encodeprofile`, `pkg/tune/executor` keep their names as a type alias (`EncodeRequest = ffencode.Request`) and one-line wrappers; their regex tables and probe maps are deleted |
| codec registry | `pkg/codecadapter` | `pkg/tune/codec`'s implementation deleted (the path stays as a transitional alias, see below); its Python-metadata fixture moves to `pkg/codecadapter/testdata/` |
| predictor | `pkg/predictor`, which gains `ORTSession` / `NewORTSession` / `NewWithModel` — the one ORT-session adapter, moved in from `cmd/vmafx-tune/cmd/ortsession.go` | `pkg/tune/predictor`'s implementation deleted (the path stays as a transitional alias, see below); its ~1,700-vector Python fixture moves to `pkg/predictor/testdata/`; `cmd/vmafx-tune/cmd/ortsession.go` deleted |
| HDR | `pkg/hdr` (moved from `pkg/tune/hdr`) | `pkg/corpus/hdr.go` keeps `HdrInfo` / `DetectHDR` / `ClassifyFFprobePayload` / `HDRCodecArgs` as an alias and wrappers over `pkg/hdr`; the model resolver stays |
| libm parity | `pkg/pymath` (moved from `pkg/tune/pymath`) | — |

Every parity fixture travels with the winner, and each winner gains the
fixture tests the loser carried (`python_adapters.json`,
`python_predictor.json`, `float_repr.txt`), so no Python-derived evidence is
lost. `pkg/predictor` additionally pins nine CPython-computed curve values as
raw bits so a stdlib `Log10` can never creep back in silently.

**The sidecar boundary.** `pkg/tune/sidecar/` and
`cmd/vmafx-tune/cmd/sidecar.go` (with their tests) are not touched by this
change: they belong to the in-flight sidecar Python-parity fix (#1187), which
rewrites `cmd/vmafx-tune/cmd/sidecar.go` around the very imports this change
would repoint. Those four files still import `pkg/tune/{codec,predictor,
pyjson}`, so those three paths survive as **transitional thin alias
packages** — one file each, type aliases and one-line wrappers over the
survivor, no logic and no tests of their own — and the sidecar compiles
whichever of the two PRs merges first. The ORT-session adapter the sidecar's
`predictor.New(modelPath, log)` needs is not duplicated into the alias: it
moves from `cmd/vmafx-tune/cmd/ortsession.go` into `pkg/predictor`
(`ORTSession`, `NewORTSession`, `NewWithModel`), where `predict`, `auto` and
the alias all reach it. The alias-then-delete option below is therefore used
narrowly and with a named trigger: once #1187 lands, a follow-up repoints the
four sidecar files and deletes the three alias packages.

Where the survivors disagreed, the Python is the tiebreaker: `int()`
truncation for content-light values, the libx264 fallback for a partial
coefficient table, `repr()` thresholds for argv floats, and the empty
container for a nil Go slice or map (a Python list is never `None` by being
empty). The one documented exception stands: the AMF argv is emitted once
(pkg/codecadapter `AGENTS.md` invariant 3), so `pkg/tune/executor` loses the
inert duplicate it used to reproduce.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Keep both (status quo, document the duplication) | Zero risk to any consumer today | The divergences above already exist and grow with every fix that lands on one copy; ADR-1125's "deliberately two" note was itself wrong within a day | Rejected — duplication with a deadline, same reasoning as ADR-1125 |
| Alias-then-delete (leave every old import path as a re-export package for one release, delete later) | Consumers migrate at their own pace | Go has no package aliasing: each shim is a hand-maintained file of wrapper functions, and the "later" never arrives; every shim is a place for the next divergence | Rejected as a blanket policy. Used narrowly, with a named trigger, for the three packages the sidecar files import (`pkg/tune/{codec,predictor,pyjson}`): those files belong to the in-flight #1187, so the aliases hold until it lands and are then deleted together with the import repoint |
| Delete now, repoint every consumer (chosen for the pure duplicates) | One implementation, one import path, the compiler finds every caller | A wide diff (57 files) in one PR | Chosen — the diff is mechanical and `go build` gates it |
| Delete the implementation, keep the name (chosen for `EncodeRequest` / `BuildFFmpegCommand` / `ParseVersions` in three packages and the HDR names in `pkg/corpus`) | The per-package argv and HDR test tables keep pinning the contract under the name their consumers use; each wrapper is one line and cannot diverge | Two names for one function | Chosen for those surfaces only; the wrappers carry a doc comment naming the implementation |
| Keep `internal/pyjson` and `internal/pyjsonstrict` separate, delete only the `pkg/` copies | Honours ADR-1125's note | The note was measured wrong; two packages for a one-field difference | Rejected |

## Consequences

- **Positive**: 988 non-test lines added against 3,104 removed (net −2,116);
  test code +1,071 / −1,302 (net −231) while every Python-derived fixture
  survives. One encoder, one argv builder, one registry, one predictor with
  one ORT-session adapter, one HDR port, one libm parity layer. `pkg/tune/`
  now holds what is tune-specific (`auto`, `sidecar`, `executor`) plus the
  three transitional aliases, each a single file with no logic.
- **Negative / behavioural deltas** (all deliberate, all pinned by tests):
  - `vmafx-tune auto --execute` no longer emits the AMF duplicate tail — the
    argv for `h264_amf` / `hevc_amf` / `av1_amf` cells matches the other
    three encode drivers and the ADR-1125 decision.
  - `pkg/corpus` content-light SEI values are truncated, not rounded, matching
    Python's `int()`. Only fractional `max_content` / `max_average` values in
    ffprobe side data are affected.
  - A caller-supplied predictor coefficient table that omits a codec now falls
    back to the libx264 curve (Python's behaviour), not to that codec's shipped
    default.
  - `pkg/tune/executor`'s lenient argv path for an out-of-vocabulary preset
    now passes the mnemonic through verbatim (the `pkg/codecadapter` rule)
    instead of substituting the adapter's default preset. Neither behaviour
    was the Python's (which raises `KeyError`), and the planner only ever
    emits `medium`, so no plan reaches it.
  - `pkg/pyjson.Marshal` renders a nil slice or map as `[]` / `{}`. No
    consumer of the former `internal/pyjson` relied on `null` — the one place
    that could have (`pkg/corpusrow`'s `extra_params`) already defended
    against it.
  - `auto --model` and `sidecar --model` now report a degraded run the way
    `predict --model` already did: when `vmafx-ort-runner` is absent from
    `PATH` the shared predictor logs one warning and falls back to the
    analytical curve. The deleted `pkg/tune/predictor` path swallowed that
    one error silently (Python's silent `ImportError` fallback);
    `pkg/predictor` deliberately reports it (`session_fallback_test.go`),
    and one predictor means one posture. stdout, the plan JSON and the
    sidecar state are unchanged.
- **Neutral / follow-ups**: the CLI-level `--model` construction for `auto`
  and `sidecar` now goes through `predictor.NewWithModel`, attaching the same
  ORT session `predict` uses; a missing model path still fails the command,
  matching the Python `FileNotFoundError`. Once #1187 lands, a follow-up
  repoints the four sidecar files onto `pkg/{codecadapter,predictor,pyjson}`,
  deletes the three alias packages, and reconciles the
  `docs/usage/vmafx-tune-go.md` paragraphs #1187 rewrites around the old
  paths. The remaining per-file float helpers in
  `pkg/pershot`, `pkg/fast`, `pkg/recommend` and `pkg/prefilter` are
  `encoding/json`-embedded (`MarshalJSON` implementations and argv tokens) and
  are left for a follow-up that migrates those emitters onto `pkg/pyjson`
  wholesale.

## References

- `req` — task brief (2026-09-02): consolidate the duplicated Go packages left
  over from integrating the seven parallel ports; one implementation of each,
  everything else deleted or a thin alias, byte-identical outputs, keep the
  `pymath` semantics and pin known values.
- [ADR-1125](1125-vmafx-tune-go-port-integration.md) — the integration that
  left these duplicates, and the AMF de-duplication decision this ADR keeps.
- [ADR-0705](0705-vmafx-tune-go-stage1.md) — the schema-forward byte-parity
  invariant every consolidated surface must still meet.
- [ADR-0366](0366-corpus-nan-features.md) — why the corpus JSONL carries bare
  `NaN` tokens, i.e. why `encoding/json` cannot be the writer.
- [#1187](https://github.com/VMAFx/vmafx/pull/1187) — the in-flight sidecar
  Python-parity fix that owns `pkg/tune/sidecar/` and
  `cmd/vmafx-tune/cmd/sidecar.go`; the reason the three alias packages
  exist and the trigger for deleting them.
