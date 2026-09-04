<!-- markdownlint-disable MD024 -->
# `tools/vmaf-tune/` — agent notes

Parent: [../../AGENTS.md](../../AGENTS.md).

Quality-aware encode automation harness. See
[`docs/adr/0237-quality-aware-encode-automation.md`](../../docs/adr/0237-quality-aware-encode-automation.md)
for the umbrella spec and
[`docs/research/0044-quality-aware-encode-automation.md`](../../docs/research/0044-quality-aware-encode-automation.md)
for the option-space digest.

## Rebase-sensitive invariants

- **The 10 Pelorus deband knobs in `filter_adapters/pelorus_deband.py`
  are a frozen two-repo contract, not a free parameter
  ([ADR-1116](../../docs/adr/1116-autotune-prefilter-control-plane.md);
  Pelorus ADR-0110).** `PELORUS_DEBAND_KNOBS` (name / type / `lo` / `hi`
  / `default`) mirrors the Pelorus control-plane table verbatim. Do
  **not** widen, narrow, rename, retype, or reorder a knob, and do
  **not** add `sample` / `blur` / `planes` / `meta` (deliberately
  out-of-contract). A change here is only valid as half of a
  coordinated Pelorus + vmafx PR pair. The conformance test
  `tests/test_filter_adapter_pelorus_deband.py` re-transcribes the
  contract independently and fails on any drift — if it goes red after a
  rebase, the contract moved, not the test. The `filter_adapters/`
  family is the sibling of `codec_adapters/`: a *pre-filter* is not a
  codec (no preset/CRF/two-pass surface), so the two registries stay
  separate. The `prefilter` joint TPE search (`prefilter.py`) builds its
  search space straight from this table + a synthetic `crf` axis, and
  reuses the `fast.py` `TPESampler` study — keep the search-engine reuse
  rather than forking a second sampler.

- **The `compare` JSON has two schemas in tree (v1 + v2)** — pick the
  ingester by discriminator, never by row count. v1 (single-target
  legacy) has no `schema_version` key and no `target_vmafs` list and
  carries one row per codec at the single `target_vmaf`. v2
  ([ADR-0516](../../docs/adr/0516-vmaf-tune-compare-rate-quality-sweep.md))
  stamps `"schema_version": 2` and `"target_vmafs": [...]` and emits
  one row per `(codec, target_vmaf)` pair. The discriminator is
  `schema_version >= 2 OR "target_vmafs" in payload`; the helper
  `vmaftune.compare.detect_schema_version()` is the single source of
  truth and **must not be inlined** into renderers. Both shapes share
  the per-row key set (`COMPARE_ROW_KEYS`) — adding columns to one
  schema means adding to both. When the renderer encounters a v2
  payload it draws the rate-quality curve + pareto-frontier overlay;
  v1 keeps the legacy bar+dot chart. Operators that consume the JSON
  programmatically can `if payload.get("schema_version", 1) >= 2:`
  branch on the contract.

- **`compare_codecs_sweep` builds one bisect predicate per target
  VMAF**, memoised in a per-target cache, then flat-dispatches the
  cross-product `(codec, target_vmaf)` to the thread pool. Do not
  collapse this into a single per-codec predicate: the bisect closure
  binds `target_vmaf` at construction time (the per-iteration
  candidate-CRF probe wants the right rung), so re-using one closure
  across multiple targets re-runs the same target every time.

- **The v2 schema's `bisect_samples` row field is optional and
  additive (ADR-0530).** Every successful encode+score round-trip
  the underlying bisect computes is appended to
  `BisectResult.samples` and projected through
  `RecommendResult.bisect_samples` (a tuple of dicts with `crf`,
  `bitrate_kbps`, `vmaf_score`, `encode_time_ms`). `to_row` emits
  the field only when populated so the absence of the key still
  identifies "old v2 dump (pre-ADR-0530)" — the renderer falls back
  to the legacy connect-the-dots chart with a caveat note in that
  case. The chart deduplicates samples per codec by CRF, sorts by
  bitrate, and draws a monotonic-friendly per-codec curve with the
  picked-CRF rows highlighted as larger circled markers. **Do not
  collapse `bisect_samples` into a "winner only" field** — that
  defeats the whole purpose of the additive plumb. The CSV emitter
  intentionally drops the structured column via
  `extrasaction="ignore"`; preserving the flat row contract is
  load-bearing for downstream `csv` consumers (e.g. spreadsheet
  ingestion).

- **`--target-vmafs` default sweep is `94,96,97,98` (ADR-0538,
  supersedes ADR-0534's `75,80,85,90,93`).** Premium-archival
  operating points; the fork's primary user encodes archival
  masters at VMAF >= 95 exclusively. Back-compat for legacy
  scripts that pin a single VMAF via `--target-vmaf NN` is
  preserved by the `_TrackedDefaultAction` sentinel: when
  `--target-vmaf` is explicit and `--target-vmafs` is at its
  default, the v1 single-target schema is honoured. The sentinel
  detection happens in `_run_compare` — if you bypass `main()` and
  invoke `_run_compare` directly, stamp `args._target_vmafs_was_default`
  and `args._target_vmaf_was_default` first (call
  `_stamp_tracked_default_sentinels(args)`).

- **The bisect default search window is the encoder's absolute CRF
  range (ADR-0538), not the adapter's `quality_range`.** When
  `bisect_target_vmaf` is called with `crf_range=None` it consults
  `_ABSOLUTE_CRF_RANGE_BY_NAME` in `bisect.py` to pick the
  encoder's accepted bounds: `libx264 / libx265 -> (0, 51)`,
  `libvpx-vp9 / libaom-av1 / libsvtav1 -> (0, 63)`. This is wider
  than the perceptually-informative `adapter.quality_range` (e.g.
  libx265's `(15, 40)`) so high-VMAF targets are reachable. The
  bisect also bypasses `adapter.validate`'s CRF gate and
  re-implements just the preset + absolute-range checks in
  `_encode_and_score`; if you change `adapter.validate` semantics
  also audit `bisect._encode_and_score` to keep them in sync. The
  corpus-generator path in `corpus.py` still calls
  `adapter.validate` unchanged — only the bisect search loop is
  widened. Adding a new codec to the absolute-range table is a
  single dict entry; codecs not in the table fall back to
  `adapter.crf_min/crf_max` then `quality_range`.

- **`DEFAULT_SAMPLER_CRF_SWEEP` must stay inside every shipped
  adapter's `quality_range`.** The canonical 5-point sweep
  `(20, 25, 30, 35, 40)` is used by `_default_sampler` (called when
  `build_ladder(sampler=None)`); `corpus.iter_rows` runs
  `adapter.validate(preset, crf)` on every cell before encoding
  starts, so a sweep point below any adapter's lower bound (e.g.
  `SvtAv1Adapter.quality_range = (20, 50)`) raises `ValueError`
  pre-encode and the ladder exits 2. The lower bound 20 is the
  *maximum* of every shipped adapter's lower bound — bumping it
  further is fine; lowering it requires either widening every
  adapter's `quality_range` or per-adapter default sweeps. Bug N-2
  regression is covered by `tests/test_ladder_svtav1_default_crf.py`.

- **Hardware-encoder availability probing is opt-in by codec, not by
  flag.** `probe_encoder_available()` only runs the 1-frame lavfi
  dummy encode when the codec is in `HARDWARE_ENCODERS`. Adding a new
  hardware encoder family (e.g. VAAPI) means appending its names to
  that tuple; the encoder will then automatically pay the dummy-encode
  cost on every `compare` invocation. CPU encoders short-circuit
  after the `ffmpeg -encoders` listing grep.
- **Score backend auto-selection is native-first (ADR-0667 / ADR-0726).**
  `vmaftune.score_backend.DEFAULT_FALLBACKS` must stay
  `cuda -> sycl -> hip -> cpu`. ADR-0726 (2026-05-28) removed Vulkan
  from the chain. Adding another explicit backend to `ALL_BACKENDS`
  requires a same-PR probe, docs update, and strict-mode unit tests.
- **Compare runtime variants are labels, not adapters
  ([ADR-0644](../../docs/adr/0644-vmaf-tune-codec-runtime-variants.md)).**
  `ADAPTER@VARIANT` tokens in `vmaf-tune compare` must parse through
  `encoder_runtime.resolve_encoder_runtime_specs()`. The base
  `ADAPTER` routes through `codec_adapters.get_adapter()` and
  `probe_encoder_available()`; the full token is only the display
  label and the key for `--encoder-ffmpeg-bin TOKEN=PATH`. Do not add
  fake adapters such as `libsvtav1-hdr` when FFmpeg still exposes
  `-c:v libsvtav1`. Compare JSON/CSV rows must keep `codec` (display
  token), `adapter`, `runtime_variant`, and `ffmpeg_bin` together so
  encoder-profile consumers can audit which runtime produced each
  row.

- **`_QSV_ENCODERS` and `BaseQsvAdapter.qsv_hw_init_args()` must stay
  in sync (ADR-0601).** `compare._hw_init_args_for_encoder()` injects
  the QSV VA-API device-init chain (`-init_hw_device vaapi=va:…
  -init_hw_device qsv=qsv_dev@va -filter_hw_device va`) only for
  encoders listed in `_QSV_ENCODERS`. If a new QSV adapter is added,
  its encoder string must be added to `_QSV_ENCODERS` in the same
  commit — omitting it silently skips the init chain and produces
  `-22 Invalid argument` at runtime. The static helper
  `BaseQsvAdapter.qsv_hw_init_args(vaapi_device)` must produce the
  same flag sequence as `_hw_init_args_for_encoder` for QSV encoders.
  `test_bbb_e2e_v14_bug_cluster.py::test_qsv_adapter_static_helper_returns_init_args`
  asserts this invariant.

- **Probe dummy-encode resolution floor is 320×240 (ADR-0601).**
  `probe_encoder_available()` uses `nullsrc=size=320x240:rate=24:
  duration=0.5` for the 1-frame dummy encode. Do not lower this
  resolution: NVENC requires at least ~145×49 and QSV requires
  ~128×96; 64×64 (the pre-fix value) was below both minima and caused
  every hardware encoder to fail the probe with EINVAL on otherwise
  fully-working GPU hosts.

- **The Phase A JSONL corpus row schema is the API contract for Phase
  B / C.** Phase B (target-VMAF bisect) and Phase C (per-title CRF
  predictor) read corpora produced by this tool. Adding optional keys
  with a default is fine; renaming or removing keys, or changing their
  type/semantics, requires bumping `vmaftune.SCHEMA_VERSION` and
  updating every downstream consumer in the same PR. The canonical
  key list lives in `src/vmaftune/__init__.py` (`CORPUS_ROW_KEYS`)
  and is asserted on every emitted row by `corpus._row_for`. Schema
  v3 ([ADR-0331](../../docs/adr/0331-corpus-schema-v3.md)) added 12
  canonical-6 per-feature aggregate columns (`adm2_mean`,
  `vif_scale[0..3]_mean`, `motion2_mean` plus matching `_std`);
  they are sourced from libvmaf's `pooled_metrics.<feature>` block
  and **must surface as `NaN` — never `0.0` — when libvmaf does not
  expose the feature** so trainers can drop the row instead of
  fitting on synthetic zeros. The reader (`corpus.read_jsonl`)
  back-fills missing v3 columns on legacy v2 rows with `NaN`; the
  on-disk `schema_version` is preserved so consumers can filter on
  `>= 3` when they need real per-feature data.
- **The `vmaf_model` JSONL field is now per-row, not per-job.** Since
  ADR-0289 (resolution-aware model selection), `corpus._row_for`
  populates `vmaf_model` from `score_res.request.model`, which in
  turn comes from `resolution.select_vmaf_model_version(width, height)`
  when `CorpusOptions.resolution_aware` is True. Mixed-ladder corpora
  legitimately contain multiple distinct `vmaf_model` values across
  rows. Downstream consumers (Phase B/C/D) must group/filter by
  `vmaf_model` rather than assuming a constant.
- **`resolution.py` decision rule is height-only.** `height >= 2160`
  picks `vmaf_4k_v0.6.1`; everything else picks `vmaf_v0.6.1`. Width
  is accepted in the API for symmetry but ignored in the body. Do not
  add per-codec / per-pixel-count branches without an ADR-0289
  follow-up — the rule mirrors Netflix's published guidance and is
  the only defensible default until the fork ships its own
  intermediate models.
- **The codec-adapter contract is multi-codec from day one.** Phase A
  wires `libx264` end-to-end; `libaom-av1`
  ([ADR-0279](../../docs/adr/0279-vmaf-tune-codec-adapter-libaom.md))
  joins as a metadata-and-argv-helper adapter (its argv shape uses
  `-cpu-used`, not `-preset`, so the encode driver gains a second
  argv path when the codec-pluggable encode wiring lands).
  `codec_adapters/__init__.py` exposes a registry the search loop
  must use uniformly. Do not branch on codec name in `corpus.py` /
  `encode.py` / `score.py`; route via the adapter. New codecs are
  one-file additions under `codec_adapters/`.
- **Adapter preset vocabulary is the cross-codec sweep axis.** The
  ten-name preset tuple (`placebo, slowest, slower, slow, medium,
  fast, faster, veryfast, superfast, ultrafast`) is shared across
  AV1-family adapters so a single `--preset` axis covers x264 / x265
  / svtav1 / libaom-av1 / libvpx-vp9 in one sweep. Each adapter maps the name
  onto its codec-specific knob (cpu-used, preset enum, ...). Do not
  introduce per-adapter preset names; if the codec needs a knob the
  shared vocabulary cannot express, route it through `extra_params`
  rather than splitting the preset axis.
- **`libvpx-vp9` two-pass is FFmpeg-generic, encoder-stats is not.**
  The adapter may set `supports_two_pass = True` because FFmpeg's
  libvpx wrapper honours `-pass` / `-passlogfile`, but
  `supports_encoder_stats` stays `False`: VP9 first-pass stats are a
  binary libvpx packet stream, not the x264/x265 text stats schema
  consumed by `encoder_stats.py`.
- **The codec-adapter contract is multi-codec from day one.**
  `codec_adapters/__init__.py` exposes a registry the search loop must
  use uniformly. Do not branch on codec name in `corpus.py` /
  `encode.py` / `score.py`; route via the adapter. New codecs are
  one-file additions under `codec_adapters/`. Wired today: `libx264`
  (Phase A scaffold) and `libx265` (ADR-0288). One narrow exception
  lives in `encode.parse_versions(stderr, encoder=…)` — the per-codec
  banner regex (x264's `x264 - core <N>` vs x265's
  `x265 [info]: HEVC encoder version <V>`) cannot be expressed as a
  single pattern, so the function dispatches on the encoder name. This
  branch is allowed; the corpus emitter and the search loop must still
  go through the registry.
  wires `libx264` plus the NVENC family (`h264_nvenc`,
  `hevc_nvenc`, `av1_nvenc` — see
  [ADR-0290](../../docs/adr/0290-vmaf-tune-nvenc-adapters.md)).
  `codec_adapters/__init__.py` exposes a registry the search loop
  must use uniformly. Do not branch on codec name in `corpus.py` /
  `encode.py` / `score.py`; route via the adapter. New codecs are
  one-file additions under `codec_adapters/`. Hardware-encoder
  families share private helpers (e.g. `_nvenc_common.py`) — keep
  the mnemonic preset map and CQ window in one place per family so
  the per-codec files stay thin.
  wires `libx264` and `libsvtav1` (ADR-0294); `codec_adapters/__init__.py`
  exposes a registry the search loop must use uniformly. Do not branch
  on codec name in `corpus.py` / `encode.py` / `score.py`; route via
  the adapter. New codecs are one-file additions under
  `codec_adapters/`.
- **`PRESET_NAME_TO_INT` in `codec_adapters/svtav1.py` is closed and
  order-stable** (ADR-0278). The mapping (`placebo`→`0`, `slowest`→`1`,
  `slower`→`3`, `slow`→`5`, `medium`→`7`, `fast`→`9`, `faster`→`11`,
  `veryfast`→`13`) is exercised by every corpus row that records
  `encoder == "libsvtav1"`. Adding a name is a schema bump for any
  fr_regressor_v2 corpus that pinned the previous mapping; reordering
  silently changes the integer SVT-AV1 receives. Editing this table
  requires a same-PR doc + ADR update.
- **The `ffmpeg_preset_token()` adapter hook is optional** —
  `corpus.iter_rows` falls back to forwarding the preset name verbatim
  when an adapter does not implement it (the libx264 path). Adapters
  that need a non-string preset translation (libsvtav1 today,
  libsvthevc / future codecs tomorrow) implement the hook and return
  a string for argv. Do not promote it to a required protocol method
  without a same-PR pass over every existing adapter.
- **Subprocess boundary is the test seam.** `encode.run_encode` and
  `score.run_score` accept a `runner` argument that defaults to
  `subprocess.run`. Tests inject a fake; production callers leave it
  default. Do not reach for `os.system` / `popen` shortcuts —
  `tests/test_corpus.py` will silently stop covering the path.
- **Fast-path is opt-in; the grid stays canonical
  ([ADR-0276](../../docs/adr/0276-vmaf-tune-fast-path.md)).** The
  `fast` subcommand under `src/vmaftune/fast.py` accelerates the
  *recommendation* use case via proxy + Bayesian + GPU-verify, but
  must never automatically replace the Phase A grid path. The grid
  is the ground-truth corpus generator that Phase B/C/D consume;
  removing or re-routing it breaks the Phase A.5 → Phase A
  fallback contract for proxy-OOD sources. The `fast` subcommand
  surfaces its smoke vs production mode in the CLI output's
  `notes` field — keep that visibility when extending the loop.
- **Fast-path time budgets are enforced by Optuna, not just reported.**
  `fast.fast_recommend(time_budget_s=...)` passes the value to
  `study.optimize(timeout=...)`, and the emitted `n_trials` field is
  the number of completed trials, not the requested cap. Preserve that
  distinction so wrappers can tell when the budget cut a search short.
- **`vmaf-tune fast` CLI exit-code contract is the fall-back
  signal** (HP-3, ADR-0276 § Status update 2026-05-08). `_run_fast`
  in `cli.py` exits `0` for an in-tolerance recommendation, `2`
  for argument errors, and **`3`** for the OOD case where the
  proxy/verify gap exceeds `--proxy-tolerance`. The `||
  vmaf-tune recommend ...` fall-back idiom in
  `docs/usage/vmaf-tune.md` depends on the non-zero exit when
  the gap exceeds tolerance — do not silently downgrade to `0`
  or print a warning instead. The CLI is the **only** seam that
  injects `sample_extractor` (canonical-6 from probe encode +
  libvmaf JSON parse) and `encode_runner` (verify pass) into
  `fast.fast_recommend`; downstream callers that need to re-use
  the wiring import `_build_fast_sample_extractor` /
  `_build_fast_encode_runner` rather than re-implementing them.
  Output schema is the same JSON shape `recommend` and `predict`
  emit (single source of truth) plus the fast-path-specific
  `verify_vmaf` / `proxy_verify_gap` / `score_backend` fields.
- **Optuna is an optional runtime dep.** Importing it at module
  scope outside `src/vmaftune/fast.py` (or its tests) is a bug —
  the core install path stays zero-dep so corpus generation works
  on hosts that never run the fast path. The lazy-import guard in
  `fast.py` is the only correct entry point; tests that exercise
  `fast.py` use `pytest.importorskip("optuna")`.
- **Usage docs describe shipped implementation status.** The
  dedicated `docs/usage/vmaf-tune-*.md` pages and the umbrella
  `docs/usage/vmaf-tune.md` page are user-discoverable contracts,
  not backlog scratch space. When a tune surface leaves scaffold
  state, update both the standalone page and the umbrella page in
  the same PR; do not leave `(stub)`, `scaffold-only`, or stale CLI
  names on paths backed by implementation and tests.
- **Local sidecar CLI mirrors the programmatic sidecar contract
  (ADR-0394).** `vmaf-tune sidecar` is the operator surface for
  `vmaftune.sidecar.SidecarPredictor`: it must keep the same
  cache layout (`<cache>/<predictor-version>/<codec>/state.json`),
  same random host UUID posture, and same `ShotFeatures` column
  semantics as the Python API. Do not add upload, hostname-derived
  identifiers, or predictor mutation to this CLI; community pooling
  and non-linear sidecars require a separate ADR / PR.
- **Local sidecar `state.json` is strict JSON.** Persistence routes
  through `vmaftune.jsonio.write_json_strict()` so non-finite residuals,
  weights, or inverse-Gram cells become `null`, never JavaScript
  `NaN`/`Infinity` tokens. Loading a state with those nulls is treated
  as invalid and cold-starts; do not make the loader silently coerce
  them back to zero because that would hide a corrupt correction.
- **Phase-F executor result JSONL is strict JSON.** `run_plan`,
  `run_plan_per_shot`, and `run_plan_saliency` write
  `tune_results*.jsonl` through the shared `vmaftune.jsonio`
  serialization path. Failed scores and all-failed per-shot weighted
  means stay `NaN` in memory for caller-side math, but serialize as
  `null` so strict JSONL consumers, report renderers, and FFmpeg
  profile readers never ingest JavaScript-only tokens.
- **All compare / report / benchmark JSON output routes through `vmaftune.jsonio.dumps_strict`
  (ADR-0988).** Do not add bare `json.dumps` calls without NaN protection in
  `compare.py`, `report.py`, or `benchmark.py` — import `dumps_strict` instead.
  Private `_nan_to_none` helpers in those modules were removed in ADR-0988; any
  reintroduction is a rebase regression.
- **Ladder uncertainty is post-hull / pre-knee.** `vmaf-tune ladder
  --with-uncertainty` must run the ADR-0279 prune/insert recipe only
  after `convex_hull()` and before `select_knees()`. Preserve corpus
  row `vmaf_interval` payloads when present; when rows are point-only,
  use the active `wide_interval_min_width` as the conservative centred
  fallback interval so point-only corpora still participate in midpoint
  insertion.
- **Saliency inference consumes RGB, not luma-replicated input
  (ADR-0430).** `saliency.compute_saliency_map()` reads yuv420p Y/U/V,
  nearest-neighbour upsamples chroma, converts BT.709 limited-range
  YUV to RGB, and only then applies ImageNet normalisation for
  `saliency_student_v1`. Do not reintroduce the old luma-only tensor
  path unless the model card and operator docs explicitly change.
- **Predictor saliency uses the raw-YUV saliency helper
  (ADR-0654).** `predictor_features._compute_saliency()` must decode the
  requested shot range to temporary `yuv420p` before calling
  `saliency.compute_saliency_map(raw_path, width, height, ...)`; the
  public `predict --source` accepts containers, but the saliency helper
  intentionally remains raw-YUV-only. `predictor_train.project_row()`
  must preserve row-provided saliency / signalstats values in the
  existing 14-column predictor layout and only zero-fill missing legacy
  rows.
- **Saliency temporal aggregation is a CLI-visible contract
  (ADR-0396 Phase 1).** `recommend-saliency --saliency-aggregator`
  exposes `mean`, `ema`, `max`, and `motion-weighted`. `mean` is the
  compatibility default; changing that default or removing a reducer
  changes user-visible encode behaviour and needs a same-PR usage-doc
  update plus an ADR-0396 follow-up.
- **`auto` non-smoke source probing is a real planning path.**
  `run_auto(smoke=False, meta_override=None)` must route source
  metadata through `_probe_source_meta`: ffprobe geometry, ffprobe
  duration, and `hdr.detect_hdr` share the same subprocess runner seam.
  Keep failures conservative (1920x1080 SDR, `duration_s=0.0`) so the
  planner can still emit an auditable JSON plan instead of depending on
  host ffprobe quirks or reintroducing `NotImplementedError`.
- **`auto` emits one selected winner.** `run_auto` must keep
  `metadata.winner` aligned with the single `cells[].selected == true`
  row whenever the winner status has a `cell_index`; evidence-failure
  plans may report `no_eligible_cells` with no selected row. The
  selector is quality/budget ordered per ADR-0428: first
  in-budget target passes, then target passes with the smallest budget
  overage, then the closest quality miss. Do not make callers infer the
  winner from cell order.
- **Fast-path proxy invariant
  ([ADR-0304](../../docs/adr/0304-vmaf-tune-fast-path-prod-wiring.md)).**
  The production proxy is **always** `fr_regressor_v2` (no smoke
  models in the production path; ADR-0291 flipped v2 to
  production). Every consumer goes through
  `vmaftune.proxy.run_proxy(...)` — a single seam over
  onnxruntime + the 14-D codec block (12-way ENCODER_VOCAB v2
  one-hot + preset_norm + crf_norm). Do not call onnxruntime
  directly from `fast.py` / `recommend.py` / `per_shot.py`; future
  probabilistic-head / ensemble migrations (ADR-0279 follow-up)
  must land in `proxy.py` so callers see no diff. Onnxruntime and
  numpy stay lazy-imported inside `proxy.py` so the corpus path
  on hosts without those deps stays zero-dep. A **single** GPU
  verify pass at `fast_recommend` end is mandatory — proxy alone
  never wins, regardless of how confident the proxy looks.
  Verification uses the existing `score_backend.select_backend`
  selector (ADR-0299); `verify_vmaf` and `proxy_verify_gap` ride
  on the result dict. When the gap exceeds the configured
  tolerance the result is flagged OOD; the operator falls back to
  the slow Phase A grid (ADR-0276 fallback contract).
  `ENCODER_VOCAB_V2` ordering is frozen by ADR-0291; reordering
  silently invalidates every shipped v2 inference.
- **Fast-path probe feature extraction and normalisation contract
  (T-VMAFTUNE-FAST-PY-PROBE-BROKEN-2026-08-30).** The Python fast path
  (`vmaftune.cli._build_fast_sample_extractor`, `vmaftune.fast._build_production_sample_extractor`,
  `vmaftune.proxy.normalise_features`) and Go twin (`pkg/fast`) share a strict
  numerical and operational contract:
  1. Probe encodes output container bitstreams (e.g. `.mp4`); distorted inputs
     must be decoded to temporary raw YUV via `maybe_decode_distorted` prior
     to libvmaf execution and cleaned up in a `finally` block.
  2. Feature extraction parses libvmaf pooled metric keys (`integer_adm2`,
     `integer_vif_scale0..3`, `integer_motion2`), falling back to bare metric keys
     and per-frame averages.
  3. Raw canonical-6 features must be normalised with `(x - mean) / std` using
     `feature_mean` and `feature_std` from `model/tiny/fr_regressor_v2.json` before
     evaluating the ONNX proxy regressor alongside the 14-D codec block.
  4. Any non-zero exit from probe encoding or libvmaf feature extraction must raise
     `RuntimeError` immediately — never zero-fill (`[0.0] * 6`).
  Cross-language parity is pinned by `tests/test_fast_parity.py` — its
  `test_e2e_probe_extraction_parity` runs the Python extractor and
  `go test ./pkg/fast -run TestProbePipelineExtractsRealFeatures` on the same
  fixture and asserts identical raw pooled means and normalised features
  within 1e-6 (skips when ffmpeg, the vmaf CLI or the Go toolchain is
  absent) — and by the seam tests in `tests/test_cli_fast.py`. Changing the
  probe argv, the pooled-key lookup, the scaler, or the vocabulary on one
  side without the other breaks that test.
- **`recommend` is a pure consumer of the corpus schema.** The
  `recommend` subcommand reads `vmaf_score`, `bitrate_kbps`, `crf`,
  `preset`, `encoder`, `exit_status` directly from rows produced by
  `corpus.py` (or loaded via `--from-corpus` from a previous run).
  No new schema, no parallel data path. If `SCHEMA_VERSION` bumps,
  `recommend.py`'s row-reader is one of the downstream consumers
  that must be updated in the same PR — the contract is checked by
  `test_recommend.py` against `CORPUS_ROW_KEYS`.
- **Predicate semantics are part of the user-visible contract.**
  `--target-vmaf T` returns the *smallest CRF* whose `vmaf_score >=
  T` (falling back to closest-miss when nothing clears, marked
  `(UNMET)`). `--target-bitrate KBPS` returns the row with minimum
  `|bitrate_kbps - KBPS|`, ties broken by smaller CRF. The two
  flags are mutually exclusive at the argparse layer (exit code 2
  when both are passed). Changing any of these defaults is a
  user-visible behaviour change requiring an ADR.
- **Phase F 2-pass goes through the adapter, not the driver (ADR-0333).**
  Codecs opting into 2-pass encoding declare `supports_two_pass = True`
  and override `two_pass_args(pass_number, stats_path) -> tuple[str, ...]`
  on their adapter (today: `X264Adapter`, returning
  `('-pass', str(N), '-passlogfile', str(path))`, and `X265Adapter`,
  returning `('-x265-params', f'pass={N}:stats={path}')`). The encode driver
  (`encode.py`) calls the adapter via `getattr(adapter, "supports_two_pass", False)`
  `adapter.two_pass_args(...)` — it never branches on codec name.
  `EncodeRequest` carries `pass_number: int = 0` (0 = single-pass /
  default; 1 / 2 = pass index) and `stats_path: Path | None = None`.
  `build_ffmpeg_command` redirects pass-1 output to `-f null -` so
  the throwaway encoded bitstream isn't written. The 2-pass loop
  itself lives in `run_two_pass_encode` in `encode.py`; it
  materialises the stats file in a `tempfile.mkdtemp` (or a
  caller-supplied `scratch_dir`) and removes it (plus known encoder
  sidecars such as libx265's `.cutree`) on exit. When
  `supports_two_pass = False`, the
  driver falls back to single-pass with a stderr warning by default
  (`on_unsupported="fallback"`), or raises with
  `on_unsupported="raise"` — matches the saliency.py
  "unsupported ROI encoder, fallback to plain encode" precedent.
  Sibling codec adapters (libsvtav1, libvvenc, libaom-av1) inherit
  this seam without touching the driver — their PRs only need to override
  `supports_two_pass` + `two_pass_args` on the adapter file. NVENC's
  `-multipass` is **not** this seam (single-invocation lookahead, not
  a stats-file two-call sequence); a separate adapter contract is the
  follow-up if demand surfaces.
- **`two_pass_args` is implemented on every adapter (ADR-0546).** No
  adapter inherits the protocol-default `NotImplementedError` body.
  `libaom-av1` + `libvvenc` are now `supports_two_pass=True` (FFmpeg
  generic `-pass N -passlogfile <prefix>`). `libsvtav1` returns the
  same VBR-mode argv but stays `supports_two_pass=False` because SVT-AV1
  enforces "CRF does not support multi-pass" at runtime — the harness
  default mode is CRF, so the driver falls back to single-pass. NVENC
  / QSV / AMF return their single-invocation in-encoder analysis flags
  (`-multipass fullres` / `-extbrc 1 -look_ahead_depth 40` /
  `-preanalysis true`) for pass 1 and `()` for pass 2; callers compose
  the pass-1 argv into `EncodeRequest.extra_params` for a
  quality-boosted single-pass encode. All four VideoToolbox adapters
  raise the typed `VideoToolboxTwoPassUnsupportedError` from
  `_videotoolbox_common` documenting that `VTCompressionSession` has
  no multi-pass C API. Do not regress these adapters back to bare
  `NotImplementedError` — the search loop assumes the contract is
  uniformly implemented.
- **AMF preset compression is fixed (ADR-0282).** The 7-into-3 preset
  table in `codec_adapters/_amf_common.py` (`_PRESET_TO_AMF`) is the
  cross-codec axis Phase B / C consumers depend on. Do not extend
  `presets` beyond the canonical 7 names without amending ADR-0282 —
  the registry uniformity that lets the search loop ignore codec
  identity rests on every codec accepting the same preset vocabulary.
  AV1 (`av1_amf`) is RDNA3+ only; `ensure_amf_available` is the
  runtime gate.

- **Phase E ladder math is two-pass and order-sensitive.** `convex_hull`
  in `ladder.py` runs (1) Pareto filter sorted by bitrate ascending,
  vmaf descending tie-break; (2) upper-convex envelope with `cross >= 0`
  pop predicate (drops accelerating-returns interior points so the
  hull is concave / diminishing-returns end-to-end). Re-deriving the
  hull from a different starting condition is easy to get subtly
  wrong — the algorithm is pinned by `test_ladder.py` invariants
  (monotonic both axes, no domination). Don't refactor without
  re-running that suite.
- **Phase E spacing names are part of the CLI contract.** `--spacing
  log_bitrate` is the default, `--spacing vmaf` is the documented
  perceptual-spacing mode, and `uniform` is a legacy alias for `vmaf`.
  Keep the CLI choices and `ladder.select_knees()` aliases in lockstep
  so argparse cannot accept a value the library rejects.
- **Phase E sampler is pluggable; default is a 5-point CRF sweep
  (ADR-0307).** `ladder.build_ladder` accepts an explicit `sampler=`
  callback; when omitted, `_default_sampler` composes
  `corpus.iter_rows` (Phase A encode+score) with
  `recommend.pick_target_vmaf` (smallest CRF clearing the target VMAF)
  over the canonical sweep
  `DEFAULT_SAMPLER_CRF_SWEEP = (18, 23, 28, 33, 38)` at the codec
  adapter's mid-range preset (`"medium"` for libx264 / libx265 /
  libsvtav1). The 5-point sweep is the load-bearing default; do not
  widen it without an ADR-0307 follow-up — Phase E callers downstream
  size their wall-time budget against five encodes per
  (resolution, target_vmaf) cell. Callers needing a finer grid, a
  Bayesian bisect, or a precomputed corpus stream pass an explicit
  `sampler=` — that seam stays open. Tests stub `iter_rows` via
  `monkeypatch.setattr(corpus_module, "iter_rows", ...)`; the lazy
  `from .corpus import iter_rows` inside `_default_sampler` resolves
  through the patched module attribute on every call.
- **Saliency signal blend matches `vmaf-roi` (ADR-0293).**
  `saliency.py` deliberately mirrors `vmaf-roi`'s ADR-0247 signal
  blend (`offset = (2*sal − 1) * foreground_offset`, clamped to
  ±12). If `vmaf-roi`'s C-side blend changes, `saliency.py` follows
  in the same PR — the bit-for-bit equivalence is pinned by
  `tests/test_saliency.py` and is the contract that lets us swap
  the Python implementation for a `vmaf-roi` shell-out later
  without behaviour drift. The ONNX session is the second test
  seam (`session_factory` parameter) — production callers leave it
  default; tests inject a fake. Do not import `onnxruntime` at
  module top-level; lazy-load via `_import_onnxruntime` so the
  corpus subcommand and unit tests work without it installed.
- **Compare predicate is the recommend seam.** `compare.compare_codecs`
  takes a `predicate(codec, src, target_vmaf) -> RecommendResult`
  callable. The programmatic default predicate returns `ok=False`
  pointing callers at `bisect.make_bisect_predicate(target_vmaf, *,
  width=..., height=..., framerate=..., duration_s=...)` because the
  bare predicate signature does not carry source geometry. The
  `vmaf-tune compare` CLI binds that Phase B
  ([ADR-0326](../../docs/adr/0326-vmaf-tune-phase-b-bisect.md))
  predicate from its explicit geometry flags by default; the
  `--predicate-module MODULE:CALLABLE` hook is the only supported
  way to bypass real bisect. `tests/test_compare.py` injects fake
  predicates so ranking is exercised without `ffmpeg` / `vmaf`
  binaries. Do not branch on codec name inside `compare.py` — route
  every per-codec call through the predicate / adapter registry.
- **Phase G benchmark is read-only corpus analysis (ADR-0424).**
  `vmaf-tune benchmark` consumes existing Phase-A JSONL rows and must
  not call `ffmpeg`, `vmaf`, `compare.compare_codecs`, or Phase-B
  bisect. Its contract is one summary row per encoder: lowest-bitrate
  corpus point clearing `--target-vmaf`, with closest misses preserved
  as `status="unmet"`. Live encode comparisons stay in `compare`;
  offline corpus reports stay in `benchmark`.
- **Phase B bisect assumes monotone-decreasing VMAF in CRF
  ([ADR-0326](../../docs/adr/0326-vmaf-tune-phase-b-bisect.md)).**
  `vmaftune.bisect.bisect_target_vmaf` aborts with a clear error when
  two non-adjacent samples violate this contract by more than 0.5
  VMAF (looser than measurement noise). Never weaken to a fall-back
  search strategy on monotonicity violation — the contract is part
  of the public surface, and surfacing the violation is more useful
  than papering over it. Real-world content + modern codecs satisfy
  the contract; pathological exceptions are encoder bugs we want to
  see, not absorb. Subprocess seam mirrors `encode.run_encode` /
  `score.run_score`: tests inject `encode_runner` / `score_runner`
  stubs; production callers leave them `None`.
- **`COMPARE_ROW_KEYS` is the JSON / CSV output contract** for
  `vmaf-tune compare`. Same maintenance discipline as
  `CORPUS_ROW_KEYS`: adding optional keys with a default is fine,
  renaming or removing keys requires bumping the schema and updating
  every downstream consumer in the same PR.
- **`bisect_target_vmaf` public kwarg `workdir`** — added by ADR-0549.
  Resolution order: `workdir=` kwarg (explicit Path) > `VMAFTUNE_WORKDIR`
  env var > OS default (`/tmp`). The private helpers `_workdir_parent`,
  `_estimate_yuv_bytes`, and `_check_disk_space` are **not** in
  `__all__` — `test_module_exports_match_public_surface` in
  `tests/test_bisect.py` pins the exact set `{BisectResult,
  BisectSample, bisect_target_vmaf, make_bisect_predicate}`. Adding
  private helpers to `__all__` will trip that test; import them
  directly in tests if needed. The `make_bisect_predicate` forwarding
  call in `_run_compare` (cli.py) includes `workdir=args.workdir`; any
  new compare-path caller must carry this kwarg through or the
  `test_cli_compare_binds_real_bisect_predicate` assertion will catch
  the omission. ([ADR-0549](../../docs/adr/0549-vmaftune-workdir-relocation.md))
- **Score backend selection is strict-by-default
  ([ADR-0299](../../docs/adr/0299-vmaf-tune-gpu-score.md)).**
  `score_backend.select_backend(prefer)` honours `cuda` / `sycl` /
  `hip` / `cpu` exactly — if the requested backend is not available,
  it raises `BackendUnavailableError` rather than silently falling back
  to CPU. Only `prefer="auto"` walks the fallback chain. Do not "fix"
  a strict-mode test that fails on a CI runner without GPU by adding
  silent fallback to `select_backend`; the strict guarantee is
  load-bearing for operator wall-clock expectations. Mock the
  `available` argument or `runner` instead.
- **`--score-backend` argparse choices are kept in sync with
  `score_backend.ALL_BACKENDS` and libvmaf's `--backend NAME`
  vocabulary ([ADR-0314](../../docs/adr/0314-vmaf-tune-score-backend-vulkan.md) /
  [ADR-0726](../../docs/adr/0726-drop-vulkan-backend.md)).**
  Do NOT add a new value (e.g. `metal`) to the argparse `choices`
  tuple in `cli.py` without the corresponding libvmaf-side wiring
  landing in the same release. The four current values (`cpu`,
  `cuda`, `sycl`, `hip`) are the exact set the libvmaf CLI accepts
  post-ADR-0726 (Vulkan dropped 2026-05-28); widening the harness
  without widening the binary produces silent strict-mode failures
  on hosts that probe positively for the new value. Cross-reference:
  `core/tools/cli_parse.c` `--backend` alternation.
- **HDR detection is fail-safe to SDR (ADR-0295).** `hdr.detect_hdr`
  returns `None` on any classification ambiguity (missing file,
  ffprobe failure, malformed JSON, mismatched primaries vs.
  PQ/HLG transfer). Misclassifying SDR as HDR is the dangerous
  failure mode (would inject mismatched signaling into a Rec.709
  encode); misclassifying HDR as SDR is recoverable. Do not relax
  the BT.2020 primaries gate in `_classify_payload` without an ADR
  superseding 0261.
- **The HDR codec dispatch table is the contract for codec adapters.**
  `hdr.hdr_codec_args` dispatches per `encoder` name. When a new
  codec adapter (libx265, libsvtav1, ...) lands under
  `codec_adapters/`, it inherits the dispatch row that already
  exists; adapters do not roll their own HDR flag set.
- **`auto` records HDR args through the same dispatch table.**
  `run_auto` must call `hdr_codec_args(codec, info)` per cell when
  `meta.is_hdr` is true. A generic tuple such as
  `("-color_primaries", "bt2020", "-color_trc", "smpte2084")`
  is insufficient because x265, SVT-AV1, HEVC hardware encoders,
  AV1 hardware encoders, and VVenC use different ffmpeg flag
  families. Hardware HEVC rows force `p010le` + `main10`; hardware
  AV1 rows force `p010le`; codec-private SEI flags stay limited to
  families with stable FFmpeg knobs. Tests in
  `tests/test_auto_short_circuits.py` lock this per-codec shape.
- **`select_hdr_vmaf_model` falls back silently.** When
  `model/vmaf_hdr_*.json` is absent (current state — fork hasn't
  ported Netflix's HDR model yet), `_resolve_vmaf_model` logs a
  warning and returns the SDR model. Do not change this to raise —
  HDR encode-side correctness ships independently of HDR scoring.
- **`model/vmaf_hdr_model_card.md` is documentation, not weights**
  ([research-0089](../../docs/research/0089-hdr-vmaf-model-search.md);
  ADR-0300 status update 2026-05-09). The file is a `.md`, not a
  `.json`, so `select_hdr_vmaf_model`'s `vmaf_hdr_*.json` glob does
  **not** match it and continues to return `None`. Do not rename
  the card to `.json`, do not relax the resolver glob to also match
  `.md`, and do not synthesise placeholder weights — the SDR-fallback
  path with a one-shot warning is the deliberate Path C outcome
  until either Netflix open-sources `vmaf_hdr_v0.6.1.json` upstream
  or the fork acquires a permissively-licensed HDR-MOS-labelled
  training corpus.
- **HDR is resolved once per source in `corpus.iter_rows`** (HP-2,
  ADR-0300 status update 2026-05-08). `_resolve_hdr` returns
  `(HdrInfo | None, forced: bool)`; `hdr_codec_args` runs once and
  the resulting argv tail rides on every cell's
  `EncodeRequest.extra_params`. Do **not** re-probe ffprobe per
  cell (would burn an ffprobe per encode for a constant signal),
  and do not move the HDR-mode resolution into `_row_for` (the
  decision drives the encode argv, so it must precede the encode).
  The one-shot HDR-VMAF-model warning fires once per `iter_rows`
  invocation via the `score_model_warned` mutable flag — keep
  that semantics or operators get N spurious warnings on a
  single corpus run.
- **Cache key fields are load-bearing
  ([ADR-0298](../../docs/adr/0298-vmaf-tune-cache.md)).** The
  `cache_key()` function in `cache.py` digests six fields:
  `src_sha256`, `encoder`, `preset`, `crf`, `adapter_version`,
  `ffmpeg_version`. Dropping any one of them is a silent
  correctness bug — stale entries shadow real results when the
  adapter or ffmpeg is upgraded. The contract is asserted by
  `test_cache_key_diffs_on_each_field`. When adding a new codec
  adapter, set `adapter_version: str` on the dataclass; the
  registry `Protocol` already requires it. Bump the string when
  the adapter's argv shape, preset list, or quality range changes.
- **Cache content stays opaque.** The cache value is the parsed
  `(bitrate, vmaf, encode_time, score_time)` tuple plus an opaque
  `<key>.bin` blob. Do not bake cache contents into the JSONL row —
  the row is the canonical record, the cache is a sidecar. A cache
  hit must produce a row that is bit-identical to a cache miss
  (modulo `encode_path`, which stays empty unless `--keep-encodes`).
- **Sample-clip windows are mirrored on both sides** ([ADR-0301](../../docs/adr/0301-vmaf-tune-sample-clip.md)).
  The encode side uses FFmpeg input-side `-ss <start> -t <N>`
  (rawvideo demuxer fast-seek); the score side uses libvmaf's
  `--frame_skip_ref` / `--frame_cnt`. They MUST stay in sync — the
  centre-anchored window is computed once in `_resolve_sample_clip`
  for corpus rows or `_sample_clip_window` for Phase-B bisect and
  threaded through both `EncodeRequest` and `ScoreRequest`. Do not
  slice the reference YUV on disk into a temp file (the zero-I/O
  frame-skip path is the design); do not use output-side `-ss` (it
  decodes the full source first, defeating the speedup).
- **Coarse-to-fine search is layered on `iter_rows`, not duplicated
  (ADR-0296).** `corpus.coarse_to_fine_search()` builds two
  `dataclasses.replace(job, cells=...)` jobs (coarse + fine) and
  delegates to `iter_rows` for each. Do **not** factor out a parallel
  encoder dispatch path inside the search loop — the JSONL row schema,
  encode-failure handling, and `keep_encodes` cleanup all live in
  `iter_rows`, and forking the search loop loses them. New search
  strategies (binary, Bayesian) should follow the same pattern: build
  a list of `(preset, crf)` cells, call `iter_rows`, post-process the
  emitted rows.
- **Adapter `quality_range` is the search-space boundary, not a
  user-input gate (ADR-0296).** Widening libx264's range from `(15,
  40)` to `(0, 51)` was deliberate: the recommend / coarse-to-fine
  flow must be allowed to probe boundary CRFs to bracket the answer.
  If a future codec adapter wants to restrict the *user-visible* range
  on `--crf NNN`, do that at the CLI layer, not in `adapter.validate`.

## Phase scope

Phase A (this scaffold): grid sweep + JSONL emit, x264 only.
Phase A.5 (this PR): opt-in `fast` subcommand scaffold (proxy +
Bayesian + GPU-verify, smoke-mode validated; production loop
deferred to follow-up). Phases B–F per ADR-0237 are explicitly out
of scope here; do not add bisect / predictor / ladder / MCP code
into this tree without an ADR-0237 follow-up promoting the
corresponding phase.
Phase A (this scaffold): grid sweep + JSONL emit. Wired codecs:
`libx264` (initial scaffold) and `libx265` (ADR-0288). Further codecs
(`libsvtav1`, `libvpx-vp9`, `libvvenc`, `libaom`, neural-codec extras)
are one-file adapter additions under `codec_adapters/` per ADR-0237.
Phases B–F per ADR-0237 are explicitly out of scope here; do not add
bisect / predictor / ladder / MCP code into this tree without an
ADR-0237 follow-up promoting the corresponding phase.
  wired `libx264`; ADR-0281 widened the registry with the three
  Intel QSV adapters (`h264_qsv`, `hevc_qsv`, `av1_qsv`). The
  search loop must use the registry uniformly. Do not branch on
  codec name in `corpus.py` / `encode.py` / `score.py`; route via
  the adapter. New codecs are one-file additions under
  `codec_adapters/`.

- **The QSV adapters share `_qsv_common.py`.** Three encoders with
  identical parameter shape (preset vocabulary, ICQ
  `global_quality` window) is a deliberate exception to the
  "one file per codec, nothing shared" Phase A convention. Per
  ADR-0281, future codec families that share parameter shape
  (NVENC's three encoders, AMF's three encoders, VideoToolbox's
  two H.264 + HEVC encoders) follow the same pattern: one
  `_<family>_common.py` private module, thin dataclass adapters.
  Single-codec families stay flat.
- **Apple VideoToolbox adapters share `_videotoolbox_common.py`
  (ADR-0283 + ADR-0283 *Status update 2026-05-09*).** Three
  encoders (`h264_videotoolbox`, `hevc_videotoolbox`,
  `prores_videotoolbox`) reuse the nine-name preset → `-realtime`
  boolean mapping. H.264 and HEVC share a single `-q:v` 0..100
  quality knob (higher = better; `invert_quality=False`). ProRes
  uses `-profile:v` instead — it is a fixed-rate intermediate
  codec, so the harness's `crf` slot carries the integer tier id
  (0=`proxy` → 5=`xq`); the adapter has its own validator
  `validate_prores_videotoolbox()` and an integer-id-to-FFmpeg-alias
  helper `prores_profile_name()`. Per the codec-adapter contract,
  the search loop never branches on adapter identity — it consumes
  `quality_range` + `ffmpeg_codec_args(...)` uniformly. AV1
  hardware encoding is intentionally absent — Apple Silicon has
  no AV1 hardware encoder block as of 2026 and FFmpeg exposes no
  `av1_videotoolbox`. Tests mock `subprocess.run`; the suite runs
  on Linux CI without macOS. End-to-end VT exercise is left to
  contributors with macOS + VideoToolbox available locally
  (ProRes additionally requires M1 Pro / Max / Ultra or later —
  Intel Macs with T2 do not have the ProRes hardware block).
- **The encode pipeline (`encode.py`) is still x264-CRF-tied.**
  ADR-0281 added the QSV adapter classes but did not widen
  `build_ffmpeg_command` to dispatch on `adapter.quality_knob`.
  Until that follow-up lands, the QSV adapters validate
  `(preset, global_quality)` correctly but the harness will not
  yet successfully drive a QSV encode end-to-end.
- **Subprocess boundary is the test seam.** `encode.run_encode`,
  `score.run_score`, and the QSV `ffmpeg_supports_encoder` probe
  accept a `runner` argument that defaults to `subprocess.run`.
  Tests inject a fake; production callers leave it default. Do
  not reach for `os.system` / `popen` shortcuts —
  `tests/test_corpus.py` and `tests/test_codec_adapter_qsv.py`
  will silently stop covering the path.

## Phase scope (codec registry)

Phase A (the original scaffold): grid sweep + JSONL emit, x264
only. ADR-0281 added the three QSV codec adapters as a one-file
extension off the registry; the encode-pipeline widening that
makes them functional is itself a separate Phase A follow-up.
Phases B–F per ADR-0237 (bisect / predictor / ladder / MCP) remain
explicitly out of scope here; do not add that code into this tree
without an ADR-0237 follow-up promoting the corresponding phase.
Phase A (corpus generation): grid sweep + JSONL emit, x264 only.
Phase D (per-shot CRF tuning, ADR-0276): orchestrates shot detection
(via the C-side `vmaf-perShot` binary, ADR-0222), extracts each shot
to raw YUV, and binds the pluggable per-shot CRF predicate to Phase B's
real bisect backend by default. The CLI deliberately stops before
running the final segment encodes — it emits an FFmpeg encoding plan as
JSON plus an optional shell script. `--predicate-module` remains the
advanced custom/test escape hatch; it is no longer the production path.

Phases B (target-VMAF bisect), C (per-title CRF predictor), E
(Pareto ABR ladder) and F (MCP tools) per ADR-0237 are explicitly
out of scope here; do not add bisect / predictor / ladder / MCP code
into this tree without an ADR-0237 follow-up promoting the
corresponding phase.

## Phase D rebase-sensitive invariants

- **Predicate signature is the Phase B contract.** The
  ``PredicateFn`` type alias in ``per_shot.py`` is
  ``(Shot, target_vmaf: float, encoder: str) -> (crf: int,
  measured_or_predicted_vmaf: float)``. The CLI adapter around
  Phase-B bisect must conform to this signature; widening the return
  tuple is a coordinated change that bumps the public-API surface
  across both modules in the same PR.
- **CLI default is real per-shot bisect.** `vmaf-tune tune-per-shot`
  must call the Phase-B bisect backend unless
  `--predicate-module MODULE:CALLABLE` is explicitly supplied. Do not
  reintroduce the adapter-default CRF as CLI behaviour; that fallback
  exists only for library dry runs that call `tune_per_shot()` without
  a predicate.
- **Bisect inputs are temporary raw YUV shots.** `bisect_target_vmaf`
  expects raw YUV geometry, so the CLI extracts each detected
  half-open shot range to a temporary raw-YUV file before calling it.
  Raw `.yuv` / `.raw` sources are opened with explicit rawvideo
  demuxer flags (`--width`, `--height`, `--pix-fmt`, `--framerate`);
  container and Y4M sources are left to FFmpeg's demuxer.
- **Shot ranges are half-open inside Python.** The C-side
  ``vmaf-perShot`` JSON/CSV sidecar uses inclusive ``end_frame``;
  ``per_shot.py`` normalises into ``[start_frame, end_frame)`` at
  the parse boundary. ``Shot.length`` and the
  ``-frames:v`` arg in ``_segment_command`` both depend on the
  half-open form. Do not "round-trip back to inclusive" — every
  downstream consumer assumes the half-open form.
- **The ``vmaf-perShot`` binary surface is the canonical detector.**
  Do not add a parallel ONNX-Runtime-from-Python detector path.
  When TransNet V2 is hot-pathed (e.g. Phase E ladder generation
  re-running detection), extend ``detect_shots`` to call
  ``vmaf-perShot`` once and cache, not to bypass the binary.
- **Scene-threshold + uniform-window splitter (ADR-0512).**
  ``detect_shots`` accepts ``diff_threshold`` (forwarded to the C
  binary as ``--diff-threshold``) and ``max_shot_duration_sec``
  (post-processing splitter, requires ``framerate``). The CLI
  exposes these as ``--scene-threshold`` and ``--max-shot-duration``
  (default ``2.0 s``, ``0`` disables). The splitter is intentionally
  default-on so 5 s clips always produce ``>= 2`` shots even when
  the luma-delta heuristic under-cuts; lowering the default is a
  behavioural change that must come with a fresh empirical
  calibration against the BBB e2e fixtures. The ``split_long_shots``
  helper preserves contiguity (``out[i].end_frame ==
  out[i+1].start_frame``) and distributes the remainder so partition
  lengths differ by at most one frame — both invariants are covered
  by ``test_per_shot.py`` and downstream merge / concat-listing code
  depends on the contiguity property.
- **Segment-dir priority order is load-bearing (ADR-0532).** The CLI
- **Segment-dir priority order is load-bearing (ADR-0530).** The CLI
  resolves the concat-listing directory in this exact order: (1)
  ``--segment-dir`` when set; (2) ``plan_out.parent / "segments"`` when
  ``--plan-out`` is set; (3) ``output.parent / "segments"`` otherwise.
  Order (2) ensures the concat listing lands alongside the plan JSON on
  a writable path — the plan write already succeeded at that point, so
  the parent is guaranteed writable.  The ``write_concat_listing`` call
  is wrapped in an ``OSError`` catch; failure emits a ``WARN`` to stderr
  and the command exits 0 (plan JSON is the authoritative deliverable).
  Do not collapse orders (2) and (3) without updating this invariant and
  ``test_per_shot.py::test_cli_tune_per_shot_readonly_cwd_returns_zero``.
- **Shot detection runs once per source, never per cell.** The
  corpus driver (``corpus._resolve_shot_metadata``) calls
  ``_detect_shots_with_status`` at the top of ``iter_rows`` and
  passes the resulting ``ShotMetadata`` down to every
  ``(preset, crf)`` row via ``_row_for``. Moving the call inside
  the cell loop roughly doubles corpus wall time on TransNet-V2.
  ``_detect_shots_with_status`` is the only API that returns the
  ``(shots, ok)`` tuple needed to distinguish a real single-shot
  source from a "binary failed" fallback — the public
  ``detect_shots`` shape cannot carry that flag.
- **HDR VMAF model resolution goes through
  ``hdr.select_hdr_vmaf_model``.** The canonical filename is
  ``vmaf_hdr_v0.6.1.json`` (Netflix's research-artefact name).
  Route lookups through ``hdr_model_name_for(transfer)`` so a
  future Dolby-Vision-specific model entry is one
  dispatch-table row away. The "HDR model not shipped" warning
  is single-shot per process; clear it from tests via
  ``hdr.reset_hdr_model_warning()``.
Phase A (this scaffold): grid sweep + JSONL emit. Codecs wired so
far: `libx264` (ADR-0237) and `libsvtav1` (ADR-0294). Phases B–F per
ADR-0237 are explicitly out of scope here; do not add bisect /
predictor / ladder / MCP code into this tree without an ADR-0237
follow-up promoting the corresponding phase.
Phase A (the corpus scaffold): grid sweep + JSONL emit, x264 only.
Phase E (this scaffold): per-title bitrate-ladder generator (Pareto
hull + manifest emit), sampler-pluggable, smoke-only until Phase B
merges. Phases B / C / D / F per ADR-0237 are explicitly out of scope
here; do not add bisect / predictor / per-shot / MCP code into this
tree without an ADR-0237 follow-up promoting the corresponding phase.

- **The seven F.2 short-circuit predicates in ``auto.py`` are an
  ordered tuple, not a set.** ``SHORT_CIRCUIT_PREDICATES`` declares
  ``ShortCircuit.LADDER_SINGLE_RUNG`` first and
  ``ShortCircuit.SKIP_PER_SHOT`` last; the order is part of the
  public contract because tests assert determinism across
  `evaluate_short_circuits` invocations and the JSON schema records
  the canonical-order list under ``plan.metadata.short_circuits``.
  Adding an eighth short-circuit (F.3+ follow-ups) appends to the
  tuple; never insert in the middle. The Phase D thresholds
  (`PHASE_D_DURATION_GATE_S = 300.0` and
  `PHASE_D_SHOT_VARIANCE_GATE = 0.15`) are placeholders pending F.3
  empirical fit — change them via an ADR-0325 follow-up, not a
  drive-by tweak. See [ADR-0325](../../docs/adr/0325-vmaf-tune-phase-f-auto.md).

- **F.3 confidence-aware thresholds are corpus-derived; do not
  hand-pick.** `DEFAULT_TIGHT_INTERVAL_MAX_WIDTH = 2.0` and
  `DEFAULT_WIDE_INTERVAL_MIN_WIDTH = 5.0` in `auto.py` are an
  emergency floor (Research-0067), not a target. The production
  thresholds load from a calibration JSON sidecar emitted by the
  conformal-VQA pipeline (ADR-0279) — keys
  `tight_interval_max_width` and `wide_interval_min_width`.
  `load_confidence_thresholds` falls back to the defaults with a
  one-line WARNING when no sidecar is found; do not silence that
  warning, and do not "tune" the defaults to make a failing
  integration test pass. The fix for surprising cell escalations on
  real data is a recalibration PR, not a threshold loosening here
  (CLAUDE.md `feedback_no_test_weakening`). The decision helper
  `_confidence_aware_escalation` is a pure function of
  `(verdict, interval_width, thresholds)` so it stays trivially
  unit-testable; keep it pure when extending the decision table.
  `run_auto` must pass the recipe-adjusted `effective_thresholds`
  from `_apply_recipe_override` into every F.3 decision and into
  `plan.metadata.confidence_thresholds`; computing the adjusted
  value and then falling back to `ConfidenceThresholds()` is a
  user-visible planning bug.

- **Fast-NR calibration sidecars are write-gated before tune consumes
  them.** `NRProxyBackend` intentionally trusts `calibration_slope`,
  `calibration_intercept`, and `calibration_threshold` once they are in
  `nr_metric_v1.json`; the safety boundary is
  `ai/scripts/calibrate_nr_threshold.py` (ADR-0665), which refuses weak
  sample-count or PLCC fits by default. If a fresh real-corpus run is
  rejected, fix the NR model/features or training corpus; do not loosen
  the vmaf-tune early-elimination logic to make a bad sidecar useful.

- **Profile-card reports start with run-specific takeaways.**
  `report.py::_quick_takeaways` is the single source for the Markdown and
  HTML "Quick takeaways" section (ADR-0666). It must stay derived from
  `ReportData`, not from rendered text or browser-side JavaScript, so the
  Markdown, HTML, tests, and future PDF/export paths agree on the same
  recommendation summary.

- **F.4 recipe overrides are read-only factories, not literal
  dicts.** `_CONTENT_RECIPE_TABLE` in `auto.py` stores **callables**
  (`_animation_recipe`, `_screen_content_recipe`,
  `_live_action_hdr_recipe`, `_ugc_recipe`, `_empty_recipe`); every
  call returns a fresh dict so a caller mutating the return value
  cannot leak the mutation into the next `run_auto` invocation. Tests
  in `tests/test_auto_recipe_overrides.py` assert this invariant
  explicitly. Adding a new content class means adding a factory
  function and a `RECIPE_CLASS_<NAME>` constant; never inline a
  literal dict into the table or mutate one in place. The four
  override keys (`tight_interval_max_width`, `force_single_rung`,
  `saliency_intensity`, `target_vmaf_offset`) are the only keys the
  driver honours — `get_recipe_for_class` filters by the
  `_RECIPE_KEYS` allowlist as defence-in-depth. Every threshold value
  shipped at F.4 is `[provisional, calibrate against real corpus in
  F.5]`; do not promote a placeholder to "calibrated" in a drive-by
  edit. Per memory `feedback_no_test_weakening`,
  `target_vmaf_offset` shifts only the predictor's effective target;
  the input `--target-vmaf` (the gate that ships models) is
  preserved verbatim in `plan.metadata.target_vmaf`. See
  [ADR-0325](../../docs/adr/0325-vmaf-tune-phase-f-auto.md) §F.4.

## ADR-0332 invariants (encoder-internal stats capture)

- The corpus row schema is at v3; new columns added to
  ``CORPUS_ROW_KEYS`` and ``SCHEMA_VERSION`` must keep the v3 ten
  ``enc_internal_*`` columns positionally stable so v2 readers see a
  zero rather than a missing key. Coordinates with ADR-0302.
- Every adapter in ``codec_adapters/`` must declare
  ``supports_encoder_stats: bool`` (no Protocol default). x264 / x265
  set True; everything else False until a codec-specific parser
  lands. x265's ``q-aq`` and ``icu`` / ``pcu`` / ``scu`` pass-1 aliases
  are intentionally normalised in ``encoder_stats.py`` so corpus rows
  keep the same ten ``enc_internal_*`` columns as x264.
- ``run_encode_with_stats`` doubles per-encode wall-clock on opt-in
  adapters by design. Do not collapse the pass-1 + pass-2 calls into
  one — the encoder won't emit a parseable stats file outside
  ``-pass 1`` mode.

## Sidecar (ADR-0325) rebase-sensitive invariants

- **`FEATURE_DIM = 14` and the column order in
  `sidecar._feature_vector` are the load-bearing pin** for the
  online-ridge state. Adding or reordering features without
  bumping `SIDECAR_SCHEMA_VERSION` will silently align saved
  weights to the wrong column on load. The leading `1.0` bias /
  intercept term must stay at column 0; it is what lets the
  ridge fit absorb a constant offset between predicted and
  observed VMAF.
- **`SidecarConfig.predictor_version` is the contract that
  invalidates stale corrections** when the shipped predictor
  upgrades. Tag mismatch on `SidecarModel.from_dict` raises and
  the caller (`SidecarModel.load`) falls back to a cold-start
  model. Do not catch the mismatch and "rescale" — a stale
  correction trained against the previous predictor's residuals
  is worse than no correction.
- **The host UUID is anonymous by construction.** It is generated
  by `secrets.token_hex(16)` on first install and persisted at
  `<cache_dir>/host-uuid`. **Never** swap it for `uuid.getnode()`
  / `socket.gethostname()` / `/etc/machine-id` / CPUID — that
  would re-identify the operator and break the privacy
  precondition for the future opt-in upload PR (ADR-0325
  §Future work).
- **Sidecar state is local-only by default.** The harness has no
  upload code path. Adding one requires the dedicated opt-in
  upload ADR + signing chain spelled out in ADR-0325 §Future
  work — do not slip a network call into `SidecarPredictor` or
  any of its callers without that ADR landing first.

## Predictor stub-models policy (ADR-0325)

The fork ships one `model/predictor_<codec>.onnx` per codec adapter.
As of 2026-05-14 the NVENC / QSV predictors (`h264_nvenc`,
`hevc_nvenc`, `av1_nvenc`, `h264_qsv`, `hevc_qsv`, `av1_qsv`) are
real-corpus retrains from `runs/phase_a/full_grid/comprehensive.jsonl`
and their cards carry `corpus.kind: real-N=<rows>`. Software and AMF
predictors remain synthetic stubs until matching real corpora exist.
The trainer
(`tools/vmaf-tune/src/vmaftune/predictor_train.py`) sources its
`CODECS` tuple from `predictor._DEFAULT_COEFFS` so the two stay
single-source. When a new codec adapter is added (e.g. a future
`vp9_qsv` row in `_DEFAULT_COEFFS`), the same PR must:

1. Re-run `python3 -m vmaftune.predictor_train --output-dir model`
   to produce the matching `predictor_<codec>.onnx` + card.
2. Commit the new ONNX bytes — the shipped-model smoke test
   parameterises over `CODECS` and fails if a coefficient row has
   no shipped artefact.
3. Refresh the model card's `corpus.kind` line on every retrain
   (the trainer does this automatically; review the diff).

Stub models are explicitly **not** for production CRF picks. The
synthetic target *is* the analytical fallback, so PLCC / SROCC
numbers in stub cards are artificially high. Real-corpus retrains
follow the same trainer entry point with `--corpus path/to/file.jsonl`
or `--corpus path/to/corpus-dir/` and produce honest metrics. Directory
corpus inputs are recursive and sorted so `.workingdir2/corpus_run/`
trains deterministically without a manual concatenation step. Keep that
directory handling reachable from both `train_all_codecs()` and the CLI;
file-only `is_file()` guards above `load_corpus()` silently turn real
corpus directories back into synthetic stubs. The loader accepts both
canonical `encoder` / `crf` / `vmaf_score` /
`bitrate_kbps` rows and historical hardware-sweep `codec` / `q` /
`vmaf` / `actual_kbps` aliases; do not reintroduce external conversion
scripts for those local corpora.

- **`corpus.py` uses `aiutils` helpers for file hashing and timestamps.**
  `_sha256_file` (imported as `aiutils.file_utils.sha256`) and `_utc_now_iso`
  (imported as `aiutils.time_utils.now_iso_8601`) replace the formerly
  inline `_sha256_of` and `_utc_now_iso` functions. The module adds
  `ai/src` to `sys.path` at import time so callers on a plain dev clone
  (without `aiutils` installed as an editable package) still resolve the
  import. Do not reintroduce inline duplicates of either helper — the
  canonical implementations live in `ai/src/aiutils/`.
- **`ScoreRequest.duration_s` is a decode-clamp, not a score window
  (ADR-0498, Bug #v2-A).** The new optional field threads down through
  `maybe_decode_distorted` / `_decode_to_raw_yuv` into an ffmpeg `-t`
  output clamp so a 10 s probe against a 634 s source doesn't
  materialise tens of gigabytes of raw YUV. The score window is still
  driven by `frame_skip_ref` / `frame_cnt`; `duration_s` is purely the
  disk-budget gate for the container -> raw YUV decode step. Default
  `0.0` preserves the legacy full-source decode.
- **`CorpusJob.{src_width, src_height}` are source-side overrides,
  not rung targets (ADR-0498, Bug #v2-B).** When set distinct from
  `width / height`, `iter_rows` tells ffmpeg the actual source
  geometry on `-s W:H` and appends `-vf scale=W:H` to the encode
  argv so the encoder sees the downscaled rendition. Both `None`
  keeps the legacy single-resolution path where the rung target
  serves as both source and encode dims. The ladder default sampler
  populates these from `make_default_sampler(src_width=, src_height=)`
  which the CLI binds to `--src-width / --src-height` (defaulting
  to the largest entry in `--resolutions`).
- **Encoder-version probe is a process-cached fallback (ADR-0498,
  follow-up #7).** `encode._probe_encoder_version_from_ffmpeg`
  runs at most once per `(ffmpeg_bin, encoder)` pair via
  `_PROBE_CACHE` (module-scope dict). Tests that exercise the
  fallback must clear `_PROBE_CACHE` explicitly. The probe parses
  `ffmpeg -version`'s configuration line and returns
  `"<encoder>-enabled"` when the encoder is compiled in; an empty
  string lets the caller keep its `"unknown"` placeholder so
  existing tests that pin that exact value still pass.
  `_VERSION_PROBE_PATTERNS` now covers `libx264`, `libsvtav1`,
  `libx265`, `libvpx-vp9`, `libaom-av1`, and `libvvenc` (ADR-1077);
  tests for any of these codecs that use a fake runner and don't
  return `--enable-*` text in stdout must capture only the first
  subprocess call (the encode argv), not the last, since the probe
  fires a second `ffmpeg -version` call when the encoder banner is
  absent from the encode stderr.
  `encode.probe_encoder_info(ffmpeg_bin, encoder)` returns
  `EncoderInfo(encoder, codec_detected, version_label)` — callers
  should use this rather than re-parsing the version string.
- **`fast._build_production_sample_extractor` accepts a `backend` kwarg
  (ADR-0498 follow-up #7).** Pass `backend=select_backend(...)` so
  TPE proxy trials score on GPU rather than always defaulting to CPU.
  `_build_prod_predictor` and `fast_recommend` forward the selected
  backend automatically; test seams that inject a custom
  `sample_extractor` callable are unaffected.
- **`codec_adapters.parse_available_codecs(stdout, *, restrict_to_known)`
  (ADR-0498 follow-up #7).** Parses `ffmpeg -hide_banner -encoders`
  output into a frozenset of codec names. Set `restrict_to_known=False`
  to get the full ffmpeg encoder list; the default restricts to the
  adapter registry so callers can intersect with `known_codecs()`.
- **`_maybe_decode_reference` scales the reference YUV to the rung
  target on cross-resolution sweeps (ADR-0501, Bug #V4-B).** When
  `CorpusJob.src_width / src_height` differs from `width / height`,
  `iter_rows` passes the rung target to `_maybe_decode_reference`
  which appends `-vf scale=W:H` to the ffmpeg decode argv and
  embeds the dims in the sidecar filename
  (`<src>.ref.decoded.<W>x<H>.yuv`) so multi-rung sweeps don't
  collide on a stale path. Single-resolution rungs (src dims == rung
  dims, or both `None`) keep the legacy native-geometry decode.
  Without this scale the libvmaf CLI silently mis-parses the planar
  bytes (1080p reference handed to a 720p-reading CLI = ~21 VMAF
  instead of ~93) and collapses the ladder grid.
- **`vmaf-tune-ladder/v1` JSON always emits a `samples[]` array
  (ADR-0501, Bug #V4-B).** `emit_manifest(format="json", samples=…)`
  threads the pre-hull sampler cloud through `build_and_emit`. Empty
  array when no cloud is wired — never a missing key — so consumers
  can read `payload["samples"]` unconditionally. HLS / DASH emitters
  silently ignore the `samples=` kwarg.
- **`_run_report` separates infrastructure-gap rows from real
  failures (ADR-0501, Bug #V4-C).** A row whose `error` starts with
  `"encoder unavailable"` (the bisect discriminator prefix added in
  ADR-0498 follow-up #6) raises a new top-level `degraded=true`
  flag without flipping `ok=false`. `ok=true` requires
  `at_least_one_row_succeeded AND no_real_failure`; `ok=false`
  whenever any non-unavailable row fails. New counter
  `codec_rows_unavailable` exposes the gap count for dashboards.
  Changing the prefix in `bisect._predicate_for_codec` must update
  this aggregator too.
- **Report JSON carries an encoder-profile contract
  (ADR-0643).** `ReportData.to_dict()` embeds
  `encoder_profile.schema == "vmaftune.encoder_profile.v1"` and
  `vmaf-tune encode-profile` reads that payload from JSON, HTML, or
  Markdown reports. Do not drop successful rows, failed rows, codec
  metadata, source geometry, `pix_fmt`, binary provenance, or
  `selected_pareto`; the profile reader uses them to choose exactly
  one recommendation and construct the FFmpeg argv. HTML escaping of
  the raw JSON `<pre>` is part of the contract because the reader
  unescapes it before `json.loads`.
- **`corpus.iter_rows` marks container sources for ffmpeg
  auto-detect (ADR-0505, Bug #V5-2).** `EncodeRequest.source_is_container`
  is derived from `source.suffix.lower() not in _VMAF_RAW_SUFFIXES`.
  Container sources additionally always get a `-vf scale=W:H`
  filter against the rung target so ffmpeg renders the encoded
  output at the rendition geometry regardless of source resolution.
  A regression that flips `source_is_container=False` for container
  inputs re-introduces the "VMAF 4-9 at 50 Mbps" bug — the encode
  driver then emits `-f rawvideo -pix_fmt yuv420p -s WxH -i src.mp4`
  and re-interprets compressed bytes as planar YUV pixels.
- **Sample cloud is full per-CRF sweep, de-duplicated by
  `(width, height, crf)` (ADR-0505, Bug #V5-2 + #V5-3).** The
  CLI's `_run_ladder` constructs a local `cloud_sink: list[LadderPoint]`,
  passes it to `make_default_sampler(cloud_sink=…)`, and threads
  it into `build_and_emit(extra_samples=…)`. The sampler appends
  every successfully-scored CRF row from `iter_rows` into the sink
  before `pick_target_vmaf` collapses the cell, so the emitted
  `samples[]` array carries every encoded CRF row per resolution
  instead of one-row-per-target-cell (V4 emit shape). The
  emit-side `_dedup_samples` pass keys on `(width, height, crf)`
  so two targets converging on the same CRF emit one sample row,
  not two — `_run_ladder` test stubs that fabricate `LadderPoint`
  instances with the same `(w, h, crf)` triple across different
  cells will collapse in the JSON descriptor as designed.
- **`ladder --duration N` bounds the encode pipe AND the
  reference decode (ADR-0506, Bug #V6-1).** `EncodeRequest.duration_s`
  is plumbed by `iter_rows` from `CorpusJob.duration_s`;
  `build_ffmpeg_command` appends `-t duration_s` as an input-side
  flag iff `sample_clip_seconds == 0.0 AND duration_s > 0`. A
  regression that drops the `duration_s` field or skips the
  fallback branch re-introduces the "ladder smoke run takes 10
  minutes per cell" bug — the encoder will process the full
  source while only `duration_s` seconds of reference is decoded
  for scoring. Sample-clip mode (ADR-0297) keeps precedence
  because it carries a centred start offset.
- **Raw-YUV reference decode emits demuxer-side flags before
  `-i` (ADR-0506, Bug #V6-2).** `_decode_source_to_yuv` requires
  `source_width` / `source_height` when `source_is_raw=True`
  (raises `ValueError` otherwise) and prepends `-f rawvideo
  -pix_fmt <pf> -s SRCWxSRCH -r FR` before `-i`. The container
  path (`source_is_raw=False`, the default) keeps the auto-
  detect argv unchanged so every v3/v4/v5 container test still
  passes. `_maybe_decode_reference` derives `source_is_raw` from
  the source suffix and forwards `iter_rows`'s
  `job.src_width / src_height / framerate` (or, when those are
  `None`, the rung dims as the legacy single-res case). A
  regression that drops the demuxer-side block when raw is the
  shape re-introduces "default sampler produced no scorable
  encodes" on every cross-res rung against a raw source.
- **`_run_ladder` returns RC=2 on operational failure
  (ADR-0506, Bug #V6-3).** `build_and_emit` can legitimately
  raise `RuntimeError` (no scorable encodes) or `ValueError`
  (bad input). The wrapper prints the exception message to
  stderr and returns 2; do not widen the exception list to
  bare `Exception` (that would swallow programmer errors and
  `KeyboardInterrupt`).
- **`_run_compare` auto-probes container-source framerate /
  duration (ADR-0509, Bug #V7-1).** When `--src` is a container
  (suffix outside `score.VMAF_RAW_SUFFIXES`) and the user did
  NOT pass `--framerate` / `--duration` explicitly,
  `_resolve_compare_source_geometry` substitutes the probed
  values from `vmaftune.report.probe_source` before building
  the bisect predicate. The "user passed explicitly" signal
  rides on `_TrackedDefaultAction` + `_stamp_tracked_default_sentinels`
  which set `args._<dest>_was_default = False` for any flag
  the user explicitly named. The sentinel is intentionally
  inverted (default `True`, opted-out to `False`) because
  argparse never invokes `Action.__call__` on omitted flags.
  Explicit user overrides win; mismatches emit a one-line
  stderr warning. A regression that bypasses the helper (i.e.
  feeds `args.framerate` straight into `make_bisect_predicate`)
  re-opens the v7 bug class: the encoder pulls frames at the
  container's native rate but `frame_skip_ref` / `frame_cnt`
  walk the reference YUV at a different rate, collapsing VMAF
  to the 4-90 band regardless of CRF. Tests pinning the
  invariant live under `tests/test_compare.py` (the
  `test_resolve_compare_source_geometry_*` + `test_cli_compare_passes_probed_framerate_for_container_src`
  family); when adding a new `_TrackedDefaultAction` flag,
  extend the hardcoded tuple in `_stamp_tracked_default_sentinels`.
- **`vmaf-tune ladder --score-backend` resolves up-front;
  `tune-per-shot --score-backend` defers to libvmaf (ADR-0511).**
  `_run_ladder` calls `score_backend.select_backend(prefer=raw_backend,
  vmaf_bin=args.vmaf_bin)` BEFORE any encode starts so an unavailable
  backend errors out with RC=2 and a clear message instead of failing
  mid-sweep with cryptic libvmaf output. The resolved value threads
  through `make_default_sampler(score_backend=...)` →
  `CorpusOptions.score_backend` → `vmaf --backend $name`. When `auto`
  resolves to `cpu`, the value passed downstream is `None` so the
  corpus step omits the explicit `--backend` flag and lets libvmaf
  use its own default (preserves the legacy zero-flag invocation
  pattern). `_run_tune_per_shot` DELIBERATELY DOES NOT pre-resolve —
  `_build_per_shot_bisect_predicate` keeps the historical
  `None if args.score_backend == "auto" else args.score_backend`
  conversion so `bisect_target_vmaf` receives `None` for auto and lets
  libvmaf pick the live runtime at scoring time. The asymmetry is
  documented inline at the top of `_run_tune_per_shot`; do not "fix"
  it without updating that contract and the predicate tests.
- **`_build_per_shot_bisect_predicate` returns a 2-tuple (ADR-0531).**
  The function now returns `(predicate_fn, bitrate_sidecar)` where
  `bitrate_sidecar` is a `dict[tuple[int, int], float]` keyed by
  `(start_frame, end_frame)`. The predicate closure populates the dict
  as each shot's bisect completes; `_run_tune_per_shot` annotates each
  `ShotRecommendation` via `dataclasses.replace(r, bitrate_kbps=...)`.
  Custom `--predicate-module` callers are unaffected (the sidecar is
  empty for that path). Do not change the function signature back to a
  bare `PerShotPredicateFn` return without also wiring an alternative
  bitrate capture path — the plan JSON schema now requires `bitrate_kbps`
  per shot, and its absence causes the report renderer to show "—".
- **`ShotRecommendation.bitrate_kbps` defaults to NaN (ADR-0531).**
  The field carries the measured segment bitrate from the bisect
  predicate. NaN is correct for dry-run / synthetic predicates that
  never encode a real segment. The plan-JSON emitter serialises NaN as
  `null` (RFC-8259-portable); the report ingester treats `null` and
  absent as NaN and renders "—". Tests that construct `ShotRecommendation`
  directly need not set `bitrate_kbps` unless testing the bitrate column.
- **`tune-per-shot` geometry auto-probe: patch `args` in-place before
  any downstream call (ADR-0542).** `_run_tune_per_shot` writes the
  ffprobe-derived width, height, framerate, and total-frames back onto
  the `args` namespace at the top of the function so that
  `_build_per_shot_bisect_predicate`, `merge_shots`, plan serialisation,
  and the `detect_shots` call all receive consistent geometry without
  signature changes. The branch condition is
  `not _source_needs_rawvideo_demux(args.src)` — raw YUV (`.yuv` /
  `.raw`) sources still require explicit `--width` and `--height` and
  exit 2 if omitted. Do not add a new geometry-consuming helper inside
  `_run_tune_per_shot` without reading `args.width` / `args.height`
  AFTER the probe block, not before. Tests: `tests/test_tune_per_shot_container_src.py`.
- **`compare --no-bisect` skips bisect; `_run_compare_crf_sweep` owns
  schema-v3 output (ADR-0542).** When `args.no_bisect` is truthy,
  `_run_compare()` delegates immediately to `_run_compare_crf_sweep(args,
  encoders)` — the normal bisect path is not entered. The sweep function
  calls `bisect._encode_and_score` directly (no iterative search), builds
  a `{"schema_version": 3, "mode": "crf_sweep", "rows": [...]}` payload,
  and writes / prints JSON only (CSV / Markdown rendering is a
  follow-up). `--target-vmaf` / `--target-vmafs` are parsed but act
  as label-only annotation; they do not influence the encode loop. Do
  not short-circuit `_run_compare` before the format-validation block
  (the format guard still applies). Tests: `tests/test_compare_no_bisect.py`.
- **Pathlib-only filesystem ops (ADR-0936).** Fork-owned modules under
  `tools/vmaf-tune/src/` and `tools/vmaf-tune/tests/` use `pathlib.Path`
  exclusively for filesystem operations — no `os.path.*`, `os.replace`,
  `os.symlink`, `os.path.getsize`, `os.path.splitext`, builtin `open()`
  on a path argument, or `glob.glob`. The ruff `PTH` ruleset
  (flake8-use-pathlib) is enabled in `pyproject.toml` and will fail
  lint on regression. When refactoring atomic-rename plumbing (e.g.
  `EncodeCache.put` blob commit), prefer `tmp_path.replace(dst)` over
  `os.replace(tmp_path, dst)` — they are semantically identical, just
  the pathlib-method form.
- **`CodecAdapter` Protocol must declare every field every concrete
  adapter relies on (ADR-0888).** `codec_adapters/__init__.py` declares
  the `CodecAdapter` `typing.Protocol`; every concrete adapter
  (`X264Adapter`, `LibaomAdapter`, NVENC / AMF / QSV / VideoToolbox /
  VVenC / SvtAv1 / libvpx) implements the contract. When adding a
  new field to *every* concrete adapter — most recently the
  `presets: tuple[str, ...]` field consumed by
  `ladder._default_sampler_preset` — promote it to the Protocol in the
  same change. A concrete-only field that callers reach via
  `getattr(adapter, "presets")` silently drops type safety and pyright
  flags every cross-adapter call site. The Protocol is also the spec
  for the `_REGISTRY: dict[str, CodecAdapter]` table; pyright variance
  rules require the Protocol fields to be `Final` / read-only iff every
  adapter uses a frozen dataclass — track that audit separately if a
  new mutable-field adapter ever lands.
- **`_ladder_point_from_row` returns a union the annotation hides
  (ADR-0888).** `tools/vmaf-tune/src/vmaftune/ladder.py` documents that
  `UncertaintyLadderPoint` is deliberately NOT a subclass of
  `LadderPoint` (the "subclassing would require runtime isinstance
  gymnastics" comment). The function returns either type at runtime
  based on whether the row carries a `vmaf_interval`; downstream tests
  (`test_ladder.py::test_build_ladder_default_sampler_preserves_vmaf_interval`)
  assert the runtime variant. The return annotation is kept as the
  narrower `LadderPoint` with an explicit `cast` because widening the
  whole `Ladder.points` chain cascades through the public ladder API
  (`build_ladder`, `make_default_sampler`, `select_knees`,
  `convex_hull`). If a future change does promote the union into the
  public surface, lift the `cast` and audit every `Ladder.points`
  consumer for `isinstance` discriminators.

## _stamp_tracked_default_sentinels tuple invariant (ADR-1048)

The hard-coded tuple in `_stamp_tracked_default_sentinels` (cli.py ~line 147) must
contain the `dest=` value of every `_TrackedDefaultAction` flag, not the flag name.
`ladder --duration` uses `dest="duration_s"`, so both `"duration"` (corpus) and
`"duration_s"` (ladder) must be in the tuple. When adding a new
`_TrackedDefaultAction` flag with a non-standard `dest=` argument, add the dest
to the tuple at the same time or the sentinel will never be set.
