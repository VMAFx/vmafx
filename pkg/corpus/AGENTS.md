# AGENTS.md — pkg/corpus

Go port of the Phase A corpus orchestrator (`vmaftune.corpus` and its encode /
score / HDR / shot / stats dependencies). It backs `vmafx-tune-go corpus`.

The Python implementation under `tools/vmaf-tune/src/vmaftune/` is still the
shipped one; both write the same JSONL until ADR-0703 §Decision / ADR-0704
§Consequences retire the Python. **Every invariant below exists because the two
implementations must produce the same bytes for the same inputs.**

## Rebase-sensitive invariants

1. **The corpus JSONL is a cross-implementation contract**
   (`jsonl.go`, `schema.go`). `RowKeys` mirrors `vmaftune.CORPUS_ROW_KEYS` in
   order, not just as a set — downstream trainers index positionally into the
   canonical-6 columns. Adding a column is a coordinated change with the Phase
   B/C consumers and a `SchemaVersion` bump. `schema_test.go` pins the full list
   against the Python tuple.

2. **Row rendering goes through `pkg/pyjson`, never `encoding/json`**
   (`jsonl.go`). A corpus row carries `NaN` in every canonical-6 column libvmaf
   did not populate (ADR-0366), which `encoding/json` refuses to marshal at all,
   and CPython renders floats with `repr()` (mandatory trailing `.0`, a
   different fixed/exponential threshold than Go's `%g`). `WriteRowLine` is the
   only sanctioned writer; `corpus_test.go` asserts that `json.Marshal` *fails*
   on a real row so the constraint cannot silently regress.

3. **Float aggregates use the CPython algorithms, not Go-idiomatic loops**
   (`pysum.go`, consumed by `encoderstats.go` and `shots.go`). CPython 3.12+
   `sum()` applies Neumaier compensation to float iterables, and
   `statistics.pstdev()` computes its variance exactly over `Fraction`s before a
   correctly-rounded square root. A plain `for` loop lands one or two ULP away,
   which is a *visible byte difference* in the emitted JSONL. Never "simplify"
   `pySum` / `pyPopulationStdev` into a naive accumulator —
   `pysum_test.go` pins the cases where the two disagree.

4. **`BuildFFmpegCommand` and `BuildVMAFCommand` are pure and byte-pinned**
   (`encode.go`, `score.go`). They decide what the sweep actually encodes and
   scores. The argv tables in `encode_test.go` / `score_test.go` were read off
   `vmaftune.encode.build_ffmpeg_command` and `vmaftune.score.build_vmaf_command`;
   changing a flag's position or spelling changes the produced bitstream. In
   particular:
   - `-ss` / `-t` are **input-side** (before `-i`) so ffmpeg fast-seeks raw YUV;
     output-side seeking would still decode the whole source.
   - Clip precedence is sample-clip first, then a bound `DurationS`, then
     nothing (ADR-0506 Bug #V6-1, ADR-0508 Bug #V8-A).
   - Floats in argv render through `pyjson.FloatRepr`, so `24.0` stays `"24.0"`
     rather than Go's `"24"`.

5. **`.y4m` is not a raw-YUV suffix** (`score.go`, `vmafRawSuffixes`). The
   libvmaf CLI's `raw_input_open` path is active whenever `--width` /
   `--height` / `--pixel_format` / `--bitdepth` are passed, which this package
   always does, and it trips the file-size guard on a Y4M container
   (ADR-0499 Bug #V3-B). Re-adding `.y4m` here silently reproduces the
   "file size mismatch" class of failure. The empty-suffix entry is
   deliberate — fixture trees name raw YUV without an extension.

6. **A failed distorted decode passes the request through unchanged**
   (`corpus.go`). `corpus._maybe_decode_distorted` returns only a request, not a
   status, so the vmaf binary is invoked on the undecodable container and the
   row records `exit_status != 0`. Short-circuiting instead would change the
   recorded `vmaf_binary_version` and `stderr_tail` for that cell.

7. **Backend selection never silently downgrades** (`backend.go`). `auto` walks
   the fallback chain; any explicit `--score-backend` name that the host cannot
   provide returns a `*BackendUnavailableError` (ADR-0299 / ADR-0314). Falling
   back would mask a hardware/build mismatch and lie about wall-clock
   expectations. The probes are intentionally *not* `pkg/gpu.Detect()` — that
   package answers "which vendor's device is present" (via `clinfo` for Intel),
   while this one needs "does the local vmaf binary advertise `--backend NAME`
   and does the matching runtime probe succeed" (`sycl-ls`, `rocminfo`).
   Swapping in `pkg/gpu` would change which backend a sweep picks relative to
   Python.

8. **Resolution-aware model selection overrides the configured model**
   (`corpus.go`, `resolveHDRScoreModel` call site). When
   `Options.ResolutionAware` is set — the default — `SelectVMAFModelVersion`
   replaces `Options.VMAFModel`, so a `--neg` choice is silently dropped. This
   reproduces the Python pipeline exactly; `corpus_test.go` pins it as a parity
   note. Do not "fix" it here without fixing `vmaftune.corpus.iter_rows` in the
   same change, or the two implementations diverge.

## Known gaps versus the Python implementation

- **The content-addressed encode cache** (`vmaftune.cache`, ADR-0298) is not
  ported. It is unreachable from the CLI: `cli._build_opts` never sets
  `cache_enabled` / `cache_dir`, and the `corpus` subparser exposes no
  `--cache-dir`. Port it together with the CLI flag, not before.
- **Saliency ROI helpers** on the codec adapters (`zones_from_saliency`,
  `qpfile_from_saliency`, `roi_from_saliency`) are absent: they belong to the
  `tune-per-shot` / saliency surface, not the corpus sweep.
