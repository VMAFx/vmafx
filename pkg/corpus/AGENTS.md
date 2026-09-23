# AGENTS.md — pkg/corpus

Go port of Phase A corpus orchestrator (`vmaftune.corpus` and its encode /
score / HDR / shot / stats dependencies). Backs `vmafx-tune-go corpus`.

Python implementation under `tools/vmaf-tune/src/vmaftune/` still
shipped; both write same JSONL until ADR-0703 §Decision / ADR-0704
§Consequences retire Python. **Every invariant below exists because the
two implementations must produce same bytes for same inputs.**

## Rebase-sensitive invariants

1. **Corpus JSONL is cross-implementation contract**
   (`jsonl.go`, `schema.go`). `RowKeys` mirrors `vmaftune.CORPUS_ROW_KEYS`
   in order: downstream trainers index positionally into canonical-6
   columns, order matters, not set membership alone. Adding column:
   coordinated change with Phase B/C consumers + `SchemaVersion` bump.
   `schema_test.go` pins full list against Python tuple.

2. **Row rendering goes through `pkg/pyjson`, never `encoding/json`**
   (`jsonl.go`). Corpus row carries `NaN` in every canonical-6 column
   libvmaf did not populate (ADR-0366). `encoding/json` refuses to
   marshal that at all. CPython renders floats with `repr()` (mandatory
   trailing `.0`, different fixed/exponential threshold than Go's `%g`).
   `WriteRowLine` is only sanctioned writer; `corpus_test.go` asserts
   `json.Marshal` *fails* on real row so constraint cannot silently
   regress.

3. **Float aggregates use CPython algorithms, not Go-idiomatic loops**
   (`pysum.go`, consumed by `encoderstats.go` and `shots.go`). CPython
   3.12+ `sum()` applies Neumaier compensation to float iterables;
   `statistics.pstdev()` computes variance exactly over `Fraction`s
   before correctly-rounded square root. Plain `for` loop lands one or
   two ULP away — *visible byte difference* in emitted JSONL. Never
   "simplify" `pySum` / `pyPopulationStdev` into naive accumulator —
   `pysum_test.go` pins cases where two disagree.

4. **`BuildFFmpegCommand` and `BuildVMAFCommand` are pure and
   byte-pinned** (`encode.go`, `score.go`). They decide what sweep
   encodes and scores. Since ADR-1137 `EncodeRequest`,
   `BuildFFmpegCommand` and `ParseVersions` in `encode.go` are type
   alias and one-line wrappers over `pkg/ffencode` (one port of
   `vmaftune.encode`), and `HdrInfo`, `DetectHDR`,
   `ClassifyFFprobePayload` and `HDRCodecArgs` in `hdr.go` wrap
   `pkg/hdr`; fix belongs in shared package, never in re-grown local
   copy, while tables here keep pinning contract under corpus names.
   Argv tables in `encode_test.go` / `score_test.go` were read off
   `vmaftune.encode.build_ffmpeg_command` and
   `vmaftune.score.build_vmaf_command`; changing flag's position or
   spelling changes produced bitstream. In particular:
   - `-ss` / `-t` are **input-side** (before `-i`) so ffmpeg fast-seeks
     raw YUV; output-side seeking would still decode whole source.
   - Clip precedence is sample-clip first, then bound `DurationS`, then
     nothing (ADR-0506 Bug #V6-1, ADR-0508 Bug #V8-A).
   - Floats in argv render through `pyjson.FloatRepr`, so `24.0` stays
     `"24.0"` rather than Go's `"24"`.

5. **`.y4m` is not raw-YUV suffix** (`score.go`, `vmafRawSuffixes`).
   libvmaf CLI's `raw_input_open` path is active whenever `--width` /
   `--height` / `--pixel_format` / `--bitdepth` are passed, which this
   package always does, and it trips file-size guard on Y4M container
   (ADR-0499 Bug #V3-B). Re-adding `.y4m` here silently reproduces
   "file size mismatch" class of failure. Empty-suffix entry is
   deliberate — fixture trees name raw YUV without extension.

6. **Failed distorted decode passes request through unchanged**
   (`corpus.go`). `corpus._maybe_decode_distorted` returns only
   request, not status, so vmaf binary is invoked on undecodable
   container and row records `exit_status != 0`. Short-circuiting
   instead would change recorded `vmaf_binary_version` and
   `stderr_tail` for that cell.

7. **Backend selection never silently downgrades**
   (`pkg/scorebackend`; `backend.go` is compatibility-only).
   `pkg/scorebackend` is the sole backend vocabulary, probe and selector;
   `vmafx-tune-go corpus` imports it directly. `auto` walks its fallback
   chain; any explicit `--score-backend` name that host cannot provide
   returns `*scorebackend.UnavailableError` (aliased as the legacy
   `corpus.BackendUnavailableError`) per ADR-0299 / ADR-0314. Falling back
   would mask hardware/build mismatch and lie about wall-clock expectations.
   Probes are intentionally *not* `pkg/gpu.Detect()` — that package answers
   "which vendor's device is present" (via `clinfo` for Intel), while this
   path needs "does local vmaf binary advertise `--backend NAME` and does
   matching runtime probe succeed" (`sycl-ls`, `rocminfo`). Do not regrow
   probe or selection logic under `pkg/corpus` during a rebase; update
   `pkg/scorebackend` and keep the wrappers thin.

   The typed failure is owned by `pkg/scorebackend.UnavailableError`;
   `BackendUnavailableError` is an alias kept for corpus API compatibility.
   Do not restore a second `Error()` implementation.

   `BuildVMAFCommand` formats model selectors through
   `pkg/model.CLIArgument`. Do not restore a local `modelArg`; unlike the
   defaulting callers, this boundary intentionally preserves empty as
   `version=`.

8. **Resolution-aware model selection overrides configured model**
   (`corpus.go`, `resolveHDRScoreModel` call site). When
   `Options.ResolutionAware` is set — default — `SelectVMAFModelVersion`
   replaces `Options.VMAFModel`, so `--neg` choice silently dropped.
   This reproduces Python pipeline exactly; `corpus_test.go` pins it as
   parity note. Do not "fix" it here without fixing
   `vmaftune.corpus.iter_rows` in same change, or two implementations
   diverge.

## Known gaps versus the Python implementation

- **Content-addressed encode cache** (`vmaftune.cache`, ADR-0298)
  is not ported. Unreachable from CLI: `cli._build_opts` never sets
  `cache_enabled` / `cache_dir`, and `corpus` subparser exposes no
  `--cache-dir`. Port it together with CLI flag, not before.
- **Saliency ROI helpers** on codec adapters (`zones_from_saliency`,
  `qpfile_from_saliency`, `roi_from_saliency`) are absent: they belong
  to `tune-per-shot` / saliency surface, not corpus sweep.
