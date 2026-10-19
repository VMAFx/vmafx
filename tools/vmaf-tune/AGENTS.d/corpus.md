---
paths:
  - tools/vmaf-tune/src/vmaftune/corpus.py
  - tools/vmaf-tune/tests/test_corpus*.py
invariant: Phase A JSONL corpus row schema is API contract; canonical-6 unconditionally populated; vmaf_model per-row.
---
<!-- markdownlint-disable MD024 -->
# Grid sweep corpus generation

- **Phase A JSONL corpus row schema is API contract for Phase B /
  C.** Phase B (target-VMAF bisect) and Phase C (per-title CRF
  predictor) read corpora produced by this tool. Adding optional
  keys with default is fine; renaming or removing keys, or changing
  their type/semantics, requires bumping `vmaftune.SCHEMA_VERSION`
  and updating every downstream consumer in same PR. Canonical key
  list lives in `src/vmaftune/__init__.py` (`CORPUS_ROW_KEYS`) and
  is asserted on every emitted row by `corpus._row_for`. Schema v3
  ([ADR-0366](../../../docs/adr/0366-corpus-schema-v3.md)) added 12
  canonical-6 per-feature aggregate columns (`adm2_mean`,
  `vif_scale[0..3]_mean`, `motion2_mean` plus matching `_std`); they
  are sourced from libvmaf's `pooled_metrics.<feature>` block and
  **must surface as `NaN` — never `0.0` — when libvmaf does not
  expose feature** so trainers can drop row instead of fitting on
  synthetic zeros. Reader (`corpus.read_jsonl`) back-fills missing
  v3 columns on legacy v2 rows with `NaN`; on-disk `schema_version`
  preserved so consumers can filter on `>= 3` when they need real
  per-feature data.
- **Canonical-6 columns are unconditionally populated across all
  default and custom models.** Since ADR-1168/1169 moved default
  model to `vmaf_v1.0.16_3d0h`, libvmaf does not evaluate VIF unless
  explicitly requested via `--feature vif`.
  `vmaftune.score.build_vmaf_command` and every Go libvmaf argv
  builder (`pkg/corpus.BuildVMAFCommand`, `pkg/fast.BuildVMAFCommand`,
  `pkg/scorecli.BuildCommand`, `pkg/tune/executor.BuildVMAFCommand`)
  append `--feature vif` for models lacking VIF natively. Go side
  decides via single `pkg/model.RequestsVIF` helper, so new argv
  builder must call it. Both also parse options-suffixed keys via
  prefix matching, guaranteeing all canonical-6 columns (`adm2`,
  `vif_scale0..3`, `motion2`) populate real float means rather than
  emitting `NaN`.
- **`vmaf_model` JSONL field is now per-row, not per-job.** Since
  ADR-0289 (resolution-aware model selection), `corpus._row_for`
  populates `vmaf_model` from sweep's score model
  (`_sweep_score_model`, resolved once per job): height rule
  (`resolution.select_vmaf_model_version`) when
  `CorpusOptions.resolution_aware` is True, else `vmaf_model`; then
  NEG variant when `neg` is set; then HDR model resolution. Encoded
  cells and reference-decode-failed cells name same model. CLI
  sets `resolution_aware=False` exactly when `--vmaf-model` is given.
  Mixed-ladder corpora legitimately contain multiple distinct
  `vmaf_model` values across rows. Downstream consumers (Phase B/C/D)
  must group/filter by `vmaf_model` rather than assuming constant.
- **coarse-to-fine window comes from adapter.**
  `corpus.coarse_search_window()` intersects `COARSE_WINDOW` (10..50)
  with adapter's `quality_range`, and `coarse_to_fine_search` checks
  window against `adapter.validate()` when it is called, before it
  returns row iterator, so refused window raises `ValueError`
  before any encode (CLI turns it into exit 2). Keep eager check:
  generator body would raise mid-write. fine-pass centre follows
  `invert_quality` (`higher_is_better` for VideoToolbox's `-q:v`).
- **`CorpusOptions.decode_semaphore` gates once-per-sweep reference
  decode** (ADR-0577); ladder sampler passes
  `--max-concurrent-decodes` through it and puts each rung's scratch
  directory under `--workdir` (`ladder.SamplerResources`).
- **encode-cache key covers every input of cell (cache key
  version 2).** `_cell_cache_key` passes adapter's
  `adapter_version`, ffmpeg version probed once per sweep through
  `probe_runner`, pass count, sample-clip window and
  `settings` map (geometry, source geometry, pixel format, frame rate,
  duration, extra encoder argv, score model, score backend). new
  input that changes encode or score goes into `settings` in
  same PR. hit replays stored miss row (`CachedResult.row`)
  with fresh `run_id` / `timestamp` / `encode_path`; entry without
  row is miss. No ffmpeg version, no cache for run (logged).
- **Sample-clip windows are mirrored on both sides**
  ([ADR-0301](../../../docs/adr/0301-vmaf-tune-sample-clip.md)).
  Encode side uses FFmpeg input-side `-ss <start> -t <N>`
  (rawvideo demuxer fast-seek); score side uses libvmaf's
  `--frame_skip_ref` / `--frame_cnt`. They MUST stay in sync —
  centre-anchored window is computed once in
  `_resolve_sample_clip` for corpus rows or `_sample_clip_window`
  for Phase-B bisect and threaded through both `EncodeRequest` and
  `ScoreRequest`. Do not slice reference YUV on disk into temp
  file (zero-I/O frame-skip path is design); do not use
  output-side `-ss` (it decodes full source first, defeating
  speedup).
- **Coarse-to-fine search is layered on `iter_rows`, not
  duplicated (ADR-0306).** `corpus.coarse_to_fine_search()` builds
  two `dataclasses.replace(job, cells=...)` jobs (coarse + fine)
  and delegates to `iter_rows` for each. Do **not** factor out
  parallel encoder dispatch path inside search loop — JSONL row
  schema, encode-failure handling, and `keep_encodes` cleanup all
  live in `iter_rows`, and forking search loop loses them. New
  search strategies (binary, Bayesian) should follow same pattern:
  build list of `(preset, crf)` cells, call `iter_rows`,
  post-process emitted rows.

## Phase scope

Phase A (this scaffold): grid sweep + JSONL emit, x264 only.
Phase A.5 (this PR): opt-in `fast` subcommand scaffold (proxy +
Bayesian + GPU-verify, smoke-mode validated; production loop
deferred to follow-up). Phases B–F per ADR-0237 are explicitly out
of scope here; do not add bisect / predictor / ladder / MCP code
into this tree without ADR-0237 follow-up promoting corresponding
phase.
Phase A (this scaffold): grid sweep + JSONL emit. Wired codecs:
`libx264` (initial scaffold) and `libx265` (ADR-0288). Further
codecs (`libsvtav1`, `libvpx-vp9`, `libvvenc`, `libaom`,
neural-codec extras) are one-file adapter additions under
`codec_adapters/` per ADR-0237. Phases B–F per ADR-0237 are
explicitly out of scope here; do not add bisect / predictor /
ladder / MCP code into this tree without ADR-0237 follow-up
promoting corresponding phase.
  wired `libx264`; ADR-0281 widened registry with three Intel QSV
  adapters (`h264_qsv`, `hevc_qsv`, `av1_qsv`). Search loop must
  use registry uniformly. Do not branch on codec name in
  `corpus.py` / `encode.py` / `score.py`; route via adapter. New
  codecs are one-file additions under `codec_adapters/`.

- **`corpus.py` uses `aiutils` helpers for file hashing and
  timestamps.** `_sha256_file` (imported as
  `aiutils.file_utils.sha256`) and `_utc_now_iso` (imported as
  `aiutils.time_utils.now_iso_8601`) replace formerly inline
  `_sha256_of` and `_utc_now_iso` functions. Module adds `ai/src`
  to `sys.path` at import time so callers on plain dev clone
  (without `aiutils` installed as editable package) still resolve
  import. Do not reintroduce inline duplicates of either helper —
  canonical implementations live in `ai/src/aiutils/`.
- **`CorpusJob.{src_width, src_height}` are source-side overrides,
  not rung targets (ADR-0498, Bug #v2-B).** When set distinct from
  `width / height`, `iter_rows` tells ffmpeg actual source
  geometry on `-s W:H` and appends `-vf scale=W:H` to encode argv
  so encoder sees downscaled rendition. Both `None` keeps legacy
  single-resolution path where rung target serves as both source
  and encode dims. Ladder default sampler populates these from
  `make_default_sampler(src_width=, src_height=)` which CLI binds
  to `--src-width / --src-height` (defaulting to largest entry in
  `--resolutions`).
- **`corpus.iter_rows` marks container sources for ffmpeg
  auto-detect (ADR-0505, Bug #V5-2).**
  `EncodeRequest.source_is_container` is derived from
  `source.suffix.lower() not in _VMAF_RAW_SUFFIXES`. Container
  sources additionally always get `-vf scale=W:H` filter against
  rung target so ffmpeg renders encoded output at rendition
  geometry regardless of source resolution. Regression that flips
  `source_is_container=False` for container inputs re-introduces
  "VMAF 4-9 at 50 Mbps" bug — encode driver then emits
  `-f rawvideo -pix_fmt yuv420p -s WxH -i src.mp4` and
  re-interprets compressed bytes as planar YUV pixels.

## HISS-04 helper-ordering invariants

Split of `iter_rows` / `_row_for` / `_maybe_decode_reference` /
`coarse_to_fine_search` moved ordering contracts from one function
body into call sites. Each breaks silently; no test name points at it.

- **Once-per-sweep work stays in `_prepare_sweep`, in order**: adapter
  lookup, source hash, encode-dir mkdir, cache open, sample-clip
  window, shot detection, HDR, reference decode. `iter_rows` calls it
  first inside generator body (lazy: nothing runs before first
  `next()`). Reference decode runs once, before first cell; no cell
  helper may call `_decode_job_reference` / `_maybe_decode_reference`.
- **`_cell_row` order**: `adapter.validate` -> cache lookup
  (`_cached_row`) -> reference-decode-failed short-circuit -> encode
  -> score model (HDR warning) -> score -> `_row_for` -> `_cache_put`
  -> `_cleanup_encode` -> return. `_cache_put` before
  `_cleanup_encode`: put copies encode file. Every cell side effect
  ends before row returns, so `iter_rows` yields after cleanup.
- **`tune_cache.flush()` follows loop in `iter_rows` body**: runs only
  when generator is exhausted. Not `finally`, not per cell.
- **Reference-decode-failed rows score against `opts.vmaf_model`**
  (HDR-resolved, no resolution-aware pick).
  `_ref_decode_failed_row` must not call `_cell_score_model`.
- **`_row_for` column helpers apply in schema order**:
  `_row_source_columns`, `_row_result_columns`, `_row_context_columns`,
  `_row_canonical6_columns`, then `aggregate_stats`. Dict insertion
  order = row column order; reordering changes emitted rows.
- **`subprocess.run` resolved at call time** in `_run_ffmpeg`,
  `_decode_job_reference`, `_score_cell`: tests monkeypatch
  `subprocess.run`. Never bind it at import or store it in `_Sweep`.
