<!-- markdownlint-disable MD024 -->
# `tools/vmaf-tune/` — agent notes

Parent: [../../AGENTS.md](../../AGENTS.md).

Quality-aware encode automation harness. See
[`docs/adr/0237-quality-aware-encode-automation.md`](../../docs/adr/0237-quality-aware-encode-automation.md)
for umbrella spec and
[`docs/research/0044-quality-aware-encode-automation.md`](../../docs/research/0044-quality-aware-encode-automation.md)
for option-space digest.

## Rebase-sensitive invariants

- **10 Pelorus deband knobs in `filter_adapters/pelorus_deband.py`
  are frozen two-repo contract, not free parameter
  ([ADR-1116](../../docs/adr/1116-autotune-prefilter-control-plane.md);
  Pelorus ADR-0110).** `PELORUS_DEBAND_KNOBS` (name / type / `lo` /
  `hi` / `default`) mirrors Pelorus control-plane table verbatim. Do
  **not** widen, narrow, rename, retype, or reorder knob, and do
  **not** add `sample` / `blur` / `planes` / `meta` (deliberately
  out-of-contract). Change here only valid as half of coordinated
  Pelorus + vmafx PR pair. Conformance test
  `tests/test_filter_adapter_pelorus_deband.py` re-transcribes
  contract independently, fails on any drift — if it goes red after
  rebase, contract moved, not test. `filter_adapters/` family is
  sibling of `codec_adapters/`: *pre-filter* is not codec (no
  preset/CRF/two-pass surface), so two registries stay separate.
  `prefilter` joint TPE search (`prefilter.py`) builds search space
  straight from this table + synthetic `crf` axis, reuses `fast.py`
  `TPESampler` study — keep search-engine reuse rather than forking
  second sampler.

- **`compare` JSON has two schemas in tree (v1 + v2)** — pick
  ingester by discriminator, never by row count. v1 (single-target
  legacy) has no `schema_version` key and no `target_vmafs` list,
  carries one row per codec at single `target_vmaf`. v2
  ([ADR-0516](../../docs/adr/0516-vmaf-tune-compare-rate-quality-sweep.md))
  stamps `"schema_version": 2` and `"target_vmafs": [...]`, emits
  one row per `(codec, target_vmaf)` pair. Discriminator is
  `schema_version >= 2 OR "target_vmafs" in payload`; helper
  `vmaftune.compare.detect_schema_version()` is single source of
  truth and **must not be inlined** into renderers. Both shapes
  share per-row key set (`COMPARE_ROW_KEYS`) — adding columns to one
  schema means adding to both. When renderer encounters v2 payload
  it draws rate-quality curve + pareto-frontier overlay; v1 keeps
  legacy bar+dot chart. Operators that consume JSON programmatically
  can `if payload.get("schema_version", 1) >= 2:` branch on
  contract.

- **`compare_codecs_sweep` builds one bisect predicate per target
  VMAF**, memoised in per-target cache, then flat-dispatches
  cross-product `(codec, target_vmaf)` to thread pool. Do not
  collapse this into single per-codec predicate: bisect closure
  binds `target_vmaf` at construction time (per-iteration
  candidate-CRF probe wants right rung), so re-using one closure
  across multiple targets re-runs same target every time.

- **v2 schema's `bisect_samples` row field is optional and additive
  (ADR-0530).** Every successful encode+score round-trip underlying
  bisect computes is appended to `BisectResult.samples` and
  projected through `RecommendResult.bisect_samples` (tuple of
  dicts with `crf`, `bitrate_kbps`, `vmaf_score`, `encode_time_ms`).
  `to_row` emits field only when populated so absence of key still
  identifies "old v2 dump (pre-ADR-0530)" — renderer falls back to
  legacy connect-the-dots chart with caveat note in that case. Chart
  deduplicates samples per codec by CRF, sorts by bitrate, draws
  monotonic-friendly per-codec curve with picked-CRF rows
  highlighted as larger circled markers. **Do not collapse
  `bisect_samples` into "winner only" field** — that defeats whole
  purpose of additive plumb. CSV emitter intentionally drops
  structured column via `extrasaction="ignore"`; preserving flat row
  contract is load-bearing for downstream `csv` consumers (e.g.
  spreadsheet ingestion).

- **`--target-vmafs` default sweep is `94,96,97,98` (ADR-0538,
  supersedes ADR-0534's `75,80,85,90,93`).** Premium-archival
  operating points; fork's primary user encodes archival masters at
  VMAF >= 95 exclusively. Back-compat for legacy scripts that pin
  single VMAF via `--target-vmaf NN` preserved by
  `_TrackedDefaultAction` sentinel: when `--target-vmaf` explicit
  and `--target-vmafs` at default, v1 single-target schema honoured.
  Sentinel detection happens in `_run_compare` — if you bypass
  `main()` and invoke `_run_compare` directly, stamp
  `args._target_vmafs_was_default` and
  `args._target_vmaf_was_default` first (call
  `_stamp_tracked_default_sentinels(args)`).

- **Bisect default search window is encoder's absolute CRF range
  (ADR-0538), not adapter's `quality_range`.** When
  `bisect_target_vmaf` is called with `crf_range=None` it consults
  `_ABSOLUTE_CRF_RANGE_BY_NAME` in `bisect.py` to pick encoder's
  accepted bounds: `libx264 / libx265 -> (0, 51)`,
  `libvpx-vp9 / libaom-av1 / libsvtav1 -> (0, 63)`. This is wider
  than perceptually-informative `adapter.quality_range` (e.g.
  libx265's `(15, 40)`) so high-VMAF targets are reachable. Bisect
  also bypasses `adapter.validate`'s CRF gate, re-implements only
  preset + absolute-range checks in `_encode_and_score`; if you
  change `adapter.validate` semantics also audit
  `bisect._encode_and_score` to keep them in sync. Corpus-generator
  path in `corpus.py` still calls `adapter.validate` unchanged —
  only bisect search loop widened. Adding new codec to
  absolute-range table is single dict entry; codecs not in table
  fall back to `adapter.crf_min/crf_max` then `quality_range`.

- **`DEFAULT_SAMPLER_CRF_SWEEP` must stay inside every shipped
  adapter's `quality_range`.** Canonical 5-point sweep
  `(20, 25, 30, 35, 40)` is used by `_default_sampler` (called when
  `build_ladder(sampler=None)`); `corpus.iter_rows` runs
  `adapter.validate(preset, crf)` on every cell before encoding
  starts, so sweep point below any adapter's lower bound (e.g.
  `SvtAv1Adapter.quality_range = (20, 50)`) raises `ValueError`
  pre-encode and ladder exits 2. Lower bound 20 is *maximum* of
  every shipped adapter's lower bound — bumping it further is fine;
  lowering it requires either widening every adapter's
  `quality_range` or per-adapter default sweeps. Bug N-2 regression
  covered by `tests/test_ladder_svtav1_default_crf.py`.

- **Hardware-encoder availability probing is opt-in by codec, not
  by flag.** `probe_encoder_available()` only runs 1-frame lavfi
  dummy encode when codec is in `HARDWARE_ENCODERS`. Adding new
  hardware encoder family (e.g. VAAPI) means appending its names to
  that tuple; encoder will then automatically pay dummy-encode cost
  on every `compare` invocation. CPU encoders short-circuit after
  `ffmpeg -encoders` listing grep.
- **Score backend auto-selection is native-first (ADR-0667 /
  ADR-0726).** `vmaftune.score_backend.DEFAULT_FALLBACKS` must stay
  `cuda -> sycl -> hip -> cpu`. ADR-0726 (2026-05-28) removed Vulkan
  from chain. Adding another explicit backend to `ALL_BACKENDS`
  requires same-PR probe, docs update, and strict-mode unit tests.
- **Compare runtime variants are labels, not adapters
  ([ADR-0644](../../docs/adr/0644-vmaf-tune-codec-runtime-variants.md)).**
  `ADAPTER@VARIANT` tokens in `vmaf-tune compare` must parse through
  `encoder_runtime.resolve_encoder_runtime_specs()`. Base `ADAPTER`
  routes through `codec_adapters.get_adapter()` and
  `probe_encoder_available()`; full token is only display label and
  key for `--encoder-ffmpeg-bin TOKEN=PATH`. Do not add fake
  adapters such as `libsvtav1-hdr` when FFmpeg still exposes
  `-c:v libsvtav1`. Compare JSON/CSV rows must keep `codec` (display
  token), `adapter`, `runtime_variant`, and `ffmpeg_bin` together so
  encoder-profile consumers can audit which runtime produced each
  row.

- **`_QSV_ENCODERS` and `BaseQsvAdapter.qsv_hw_init_args()` must
  stay in sync (ADR-0601).** `compare._hw_init_args_for_encoder()`
  injects QSV VA-API device-init chain (`-init_hw_device vaapi=va:…
  -init_hw_device qsv=qsv_dev@va -filter_hw_device va`) only for
  encoders listed in `_QSV_ENCODERS`. If new QSV adapter added, its
  encoder string must be added to `_QSV_ENCODERS` in same commit —
  omitting it silently skips init chain, produces
  `-22 Invalid argument` at runtime. Static helper
  `BaseQsvAdapter.qsv_hw_init_args(vaapi_device)` must produce same
  flag sequence as `_hw_init_args_for_encoder` for QSV encoders.
  `test_bbb_e2e_v14_bug_cluster.py::test_qsv_adapter_static_helper_returns_init_args`
  asserts this invariant.

- **Probe dummy-encode resolution floor is 320×240 (ADR-0601).**
  `probe_encoder_available()` uses `nullsrc=size=320x240:rate=24:
  duration=0.5` for 1-frame dummy encode. Do not lower this
  resolution: NVENC requires at least ~145×49 and QSV requires
  ~128×96; 64×64 (pre-fix value) was below both minima and caused
  every hardware encoder to fail probe with EINVAL on otherwise
  fully-working GPU hosts.

- **Phase A JSONL corpus row schema is API contract for Phase B /
  C.** Phase B (target-VMAF bisect) and Phase C (per-title CRF
  predictor) read corpora produced by this tool. Adding optional
  keys with default is fine; renaming or removing keys, or changing
  their type/semantics, requires bumping `vmaftune.SCHEMA_VERSION`
  and updating every downstream consumer in same PR. Canonical key
  list lives in `src/vmaftune/__init__.py` (`CORPUS_ROW_KEYS`) and
  is asserted on every emitted row by `corpus._row_for`. Schema v3
  ([ADR-0331](../../docs/adr/0331-corpus-schema-v3.md)) added 12
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
  populates `vmaf_model` from `score_res.request.model`, which in
  turn comes from `resolution.select_vmaf_model_version(width, height)`
  when `CorpusOptions.resolution_aware` is True. Mixed-ladder corpora
  legitimately contain multiple distinct `vmaf_model` values across
  rows. Downstream consumers (Phase B/C/D) must group/filter by
  `vmaf_model` rather than assuming constant.
- **`resolution.py` decision rule is height-only.** `height >= 2160`
  picks `vmaf_4k_v0.6.1`; everything else picks `vmaf_v0.6.1`. Width
  is accepted in API for symmetry but ignored in body. Do not add
  per-codec / per-pixel-count branches without ADR-0289 follow-up —
  rule mirrors Netflix's published guidance and is only defensible
  default until fork ships its own intermediate models.
- **Codec-adapter contract is multi-codec from day one.** Phase A
  wires `libx264` end-to-end; `libaom-av1`
  ([ADR-0279](../../docs/adr/0279-vmaf-tune-codec-adapter-libaom.md))
  joins as metadata-and-argv-helper adapter (its argv shape uses
  `-cpu-used`, not `-preset`, so encode driver gains second argv
  path when codec-pluggable encode wiring lands).
  `codec_adapters/__init__.py` exposes registry search loop must
  use uniformly. Do not branch on codec name in `corpus.py` /
  `encode.py` / `score.py`; route via adapter. New codecs are
  one-file additions under `codec_adapters/`.
- **Adapter preset vocabulary is cross-codec sweep axis.** Ten-name
  preset tuple (`placebo, slowest, slower, slow, medium, fast,
  faster, veryfast, superfast, ultrafast`) is shared across
  AV1-family adapters. Single `--preset` axis covers x264 / x265
  / svtav1 / libaom-av1 / libvpx-vp9 in one sweep. Each adapter maps
  name onto its codec-specific knob (cpu-used, preset enum, ...).
  Do not introduce per-adapter preset names; if codec needs knob
  shared vocabulary cannot express, route it through `extra_params`
  rather than splitting preset axis.
- **`libvpx-vp9` two-pass is FFmpeg-generic, encoder-stats is not.**
  Adapter may set `supports_two_pass = True` because FFmpeg's
  libvpx wrapper honours `-pass` / `-passlogfile`, but
  `supports_encoder_stats` stays `False`: VP9 first-pass stats are
  binary libvpx packet stream, not x264/x265 text stats schema
  consumed by `encoder_stats.py`.
- **Codec-adapter contract is multi-codec from day one.**
  `codec_adapters/__init__.py` exposes registry search loop must
  use uniformly. Do not branch on codec name in `corpus.py` /
  `encode.py` / `score.py`; route via adapter. New codecs are
  one-file additions under `codec_adapters/`. Wired today: `libx264`
  (Phase A scaffold) and `libx265` (ADR-0288). One narrow exception
  lives in `encode.parse_versions(stderr, encoder=…)` — per-codec
  banner regex (x264's `x264 - core <N>` vs x265's
  `x265 [info]: HEVC encoder version <V>`) cannot be expressed as
  single pattern, so function dispatches on encoder name. This
  branch is allowed; corpus emitter and search loop must still go
  through registry.
  wires `libx264` plus NVENC family (`h264_nvenc`, `hevc_nvenc`,
  `av1_nvenc` — see
  [ADR-0290](../../docs/adr/0290-vmaf-tune-nvenc-adapters.md)).
  `codec_adapters/__init__.py` exposes registry search loop must use
  uniformly. Do not branch on codec name in `corpus.py` /
  `encode.py` / `score.py`; route via adapter. New codecs are
  one-file additions under `codec_adapters/`. Hardware-encoder
  families share private helpers (e.g. `_nvenc_common.py`) — keep
  mnemonic preset map and CQ window in one place per family so
  per-codec files stay thin.
  wires `libx264` and `libsvtav1` (ADR-0294); `codec_adapters/__init__.py`
  exposes registry search loop must use uniformly. Do not branch
  on codec name in `corpus.py` / `encode.py` / `score.py`; route via
  adapter. New codecs are one-file additions under
  `codec_adapters/`.
- **`PRESET_NAME_TO_INT` in `codec_adapters/svtav1.py` is closed and
  order-stable** (ADR-0278). Mapping (`placebo`→`0`, `slowest`→`1`,
  `slower`→`3`, `slow`→`5`, `medium`→`7`, `fast`→`9`, `faster`→`11`,
  `veryfast`→`13`) is exercised by every corpus row that records
  `encoder == "libsvtav1"`. Adding name is schema bump for any
  fr_regressor_v2 corpus that pinned previous mapping; reordering
  silently changes integer SVT-AV1 receives. Editing this table
  requires same-PR doc + ADR update.
- **`ffmpeg_preset_token()` adapter hook is optional** —
  `corpus.iter_rows` falls back to forwarding preset name verbatim
  when adapter does not implement it (libx264 path). Adapters that
  need non-string preset translation (libsvtav1 today, libsvthevc /
  future codecs tomorrow) implement hook and return string for
  argv. Do not promote it to required protocol method without
  same-PR pass over every existing adapter.
- **Subprocess boundary is test seam.** `encode.run_encode` and
  `score.run_score` accept `runner` argument that defaults to
  `subprocess.run`. Tests inject fake; production callers leave it
  default. Do not reach for `os.system` / `popen` shortcuts —
  `tests/test_corpus.py` will silently stop covering path.
- **Fast-path is opt-in; grid stays canonical
  ([ADR-0276](../../docs/adr/0276-vmaf-tune-fast-path.md)).**
  `fast` subcommand under `src/vmaftune/fast.py` accelerates
  *recommendation* use case via proxy + Bayesian + GPU-verify, but
  must never automatically replace Phase A grid path. Grid is
  ground-truth corpus generator that Phase B/C/D consume; removing
  or re-routing it breaks Phase A.5 → Phase A fallback contract for
  proxy-OOD sources. `fast` subcommand surfaces its smoke vs
  production mode in CLI output's `notes` field — keep that
  visibility when extending loop.
- **Fast-path time budgets are enforced by Optuna, not only
  reported.** `fast.fast_recommend(time_budget_s=...)` passes value
  to `study.optimize(timeout=...)`, and emitted `n_trials` field is
  number of completed trials, not requested cap. Preserve that
  distinction so wrappers can tell when budget cut search short.
- **`vmaf-tune fast` CLI exit-code contract is fall-back signal**
  (HP-3, ADR-0276 § Status update 2026-05-08). `_run_fast` in
  `cli.py` exits `0` for in-tolerance recommendation, `2` for
  argument errors, and **`3`** for OOD case where proxy/verify gap
  exceeds `--proxy-tolerance`. `|| vmaf-tune recommend ...`
  fall-back idiom in `docs/usage/vmaf-tune.md` depends on non-zero
  exit when gap exceeds tolerance — do not silently downgrade to
  `0` or print warning instead. CLI is **only** seam that injects
  `sample_extractor` (canonical-6 from probe encode + libvmaf JSON
  parse) and `encode_runner` (verify pass) into
  `fast.fast_recommend`; downstream callers that need to re-use
  wiring import `_build_fast_sample_extractor` /
  `_build_fast_encode_runner` rather than re-implementing them.
  Output schema is same JSON shape `recommend` and `predict` emit
  (single source of truth) plus fast-path-specific `verify_vmaf` /
  `proxy_verify_gap` / `score_backend` fields.
- **Optuna is optional runtime dep.** Importing it at module scope
  outside `src/vmaftune/fast.py` (or its tests) is bug. Core
  install path stays zero-dep so corpus generation works on hosts
  that never run fast path. Lazy-import guard in `fast.py` is only
  correct entry point; tests that exercise `fast.py` use
  `pytest.importorskip("optuna")`.
- **Usage docs describe shipped implementation status.**
  Dedicated `docs/usage/vmaf-tune-*.md` pages and umbrella
  `docs/usage/vmaf-tune.md` page are user-discoverable contracts,
  not backlog scratch space. When tune surface leaves scaffold
  state, update both standalone page and umbrella page in same PR;
  do not leave `(stub)`, `scaffold-only`, or stale CLI names on
  paths backed by implementation and tests.
- **Local sidecar CLI mirrors programmatic sidecar contract
  (ADR-0394).** `vmaf-tune sidecar` is operator surface for
  `vmaftune.sidecar.SidecarPredictor`: it must keep same cache
  layout (`<cache>/<predictor-version>/<codec>/state.json`), same
  random host UUID posture, and same `ShotFeatures` column
  semantics as Python API. Do not add upload, hostname-derived
  identifiers, or predictor mutation to this CLI; community pooling
  and non-linear sidecars require separate ADR / PR.
- **Local sidecar `state.json` is strict JSON.** Persistence routes
  through `vmaftune.jsonio.write_json_strict()` so non-finite
  residuals, weights, or inverse-Gram cells become `null`, never
  JavaScript `NaN`/`Infinity` tokens. Loading state with those
  nulls is treated as invalid and cold-starts; do not make loader
  silently coerce them back to zero because that would hide
  corrupt correction.
- **Phase-F executor result JSONL is strict JSON.** `run_plan`,
  `run_plan_per_shot`, and `run_plan_saliency` write
  `tune_results*.jsonl` through shared `vmaftune.jsonio`
  serialization path. Failed scores and all-failed per-shot
  weighted means stay `NaN` in memory for caller-side math, but
  serialize as `null` so strict JSONL consumers, report renderers,
  and FFmpeg profile readers never ingest JavaScript-only tokens.
- **All compare / report / benchmark JSON output routes through
  `vmaftune.jsonio.dumps_strict` (ADR-0988).** Do not add bare
  `json.dumps` calls without NaN protection in `compare.py`,
  `report.py`, or `benchmark.py` — import `dumps_strict` instead.
  Private `_nan_to_none` helpers in those modules were removed in
  ADR-0988; any reintroduction is rebase regression.
- **Ladder uncertainty is post-hull / pre-knee.** `vmaf-tune ladder
  --with-uncertainty` must run ADR-0279 prune/insert recipe only
  after `convex_hull()` and before `select_knees()`. Preserve
  corpus row `vmaf_interval` payloads when present; when rows are
  point-only, use active `wide_interval_min_width` as conservative
  centred fallback interval so point-only corpora still
  participate in midpoint insertion.
- **Saliency inference consumes RGB, not luma-replicated input
  (ADR-0430).** `saliency.compute_saliency_map()` reads yuv420p
  Y/U/V, nearest-neighbour upsamples chroma, converts BT.709
  limited-range YUV to RGB, and only then applies ImageNet
  normalisation for `saliency_student_v1`. Do not reintroduce old
  luma-only tensor path unless model card and operator docs
  explicitly change.
- **Predictor saliency uses raw-YUV saliency helper (ADR-0654).**
  `predictor_features._compute_saliency()` must decode requested
  shot range to temporary `yuv420p` before calling
  `saliency.compute_saliency_map(raw_path, width, height, ...)`;
  public `predict --source` accepts containers, but saliency
  helper intentionally remains raw-YUV-only.
  `predictor_train.project_row()` must preserve row-provided
  saliency / signalstats values in existing 14-column predictor
  layout and only zero-fill missing legacy rows.
- **Saliency temporal aggregation is CLI-visible contract
  (ADR-0396 Phase 1).** `recommend-saliency --saliency-aggregator`
  exposes `mean`, `ema`, `max`, and `motion-weighted`. `mean` is
  compatibility default; changing that default or removing reducer
  changes user-visible encode behaviour and needs same-PR
  usage-doc update plus ADR-0396 follow-up.
- **`auto` non-smoke source probing is real planning path.**
  `run_auto(smoke=False, meta_override=None)` must route source
  metadata through `_probe_source_meta`: ffprobe geometry, ffprobe
  duration, and `hdr.detect_hdr` share same subprocess runner seam.
  Keep failures conservative (1920x1080 SDR, `duration_s=0.0`) so
  planner can still emit auditable JSON plan instead of depending
  on host ffprobe quirks or reintroducing `NotImplementedError`.
- **`auto` emits one selected winner.** `run_auto` must keep
  `metadata.winner` aligned with single `cells[].selected == true`
  row whenever winner status has `cell_index`; evidence-failure
  plans may report `no_eligible_cells` with no selected row.
  Selector is quality/budget ordered per ADR-0428: first in-budget
  target passes, then target passes with smallest budget overage,
  then closest quality miss. Do not make callers infer winner from
  cell order.
- **Fast-path proxy invariant
  ([ADR-0304](../../docs/adr/0304-vmaf-tune-fast-path-prod-wiring.md)).**
  Production proxy is **always** `fr_regressor_v2` (no smoke
  models in production path; ADR-0291 flipped v2 to production).
  Every consumer goes through `vmaftune.proxy.run_proxy(...)` —
  single seam over onnxruntime + 14-D codec block (12-way
  ENCODER_VOCAB v2 one-hot + preset_norm + crf_norm). Do not call
  onnxruntime directly from `fast.py` / `recommend.py` /
  `per_shot.py`; future probabilistic-head / ensemble migrations
  (ADR-0279 follow-up) must land in `proxy.py` so callers see no
  diff. Onnxruntime and numpy stay lazy-imported inside `proxy.py`
  so corpus path on hosts without those deps stays zero-dep.
  **Single** GPU verify pass at `fast_recommend` end is mandatory —
  proxy alone never wins, regardless of how confident proxy looks.
  Verification uses existing `score_backend.select_backend`
  selector (ADR-0299); `verify_vmaf` and `proxy_verify_gap` ride on
  result dict. When gap exceeds configured tolerance, result
  flagged OOD; operator falls back to slow Phase A grid (ADR-0276
  fallback contract). `ENCODER_VOCAB_V2` ordering is frozen by
  ADR-0291; reordering silently invalidates every shipped v2
  inference.
- **Fast-path probe feature extraction and normalisation contract
  (T-VMAFTUNE-FAST-PY-PROBE-BROKEN-2026-08-30).** Python fast path
  (`vmaftune.cli._build_fast_sample_extractor`,
  `vmaftune.fast._build_production_sample_extractor`,
  `vmaftune.proxy.normalise_features`) and Go twin (`pkg/fast`)
  share strict numerical and operational contract:
  1. Probe encodes output container bitstreams (e.g. `.mp4`);
     distorted inputs must be decoded to temporary raw YUV via
     `maybe_decode_distorted` prior to libvmaf execution and
     cleaned up in `finally` block.
  2. Feature extraction parses libvmaf pooled metric keys
     (`integer_adm2`, `integer_vif_scale0..3`, `integer_motion2`),
     falling back to bare metric keys and per-frame averages.
  3. Raw canonical-6 features must be normalised with
     `(x - mean) / std` using `feature_mean` and `feature_std`
     from `model/tiny/fr_regressor_v2.json` before evaluating ONNX
     proxy regressor alongside 14-D codec block.
  4. Any non-zero exit from probe encoding or libvmaf feature
     extraction must raise `RuntimeError` immediately — never
     zero-fill (`[0.0] * 6`).
  Cross-language parity is pinned by `tests/test_fast_parity.py`.
  Its `test_e2e_probe_extraction_parity` runs Python extractor and
  `go test ./pkg/fast -run TestProbePipelineExtractsRealFeatures`
  on same fixture. Asserts identical raw pooled means and
  normalised features within 1e-6 (skips when ffmpeg, vmaf CLI or
  Go toolchain absent) — and by seam tests in
  `tests/test_cli_fast.py`. Changing probe argv, pooled-key lookup,
  scaler, or vocabulary on one side without other breaks that
  test.
- **`recommend` is pure consumer of corpus schema.** `recommend`
  subcommand reads `vmaf_score`, `bitrate_kbps`, `crf`, `preset`,
  `encoder`, `exit_status` directly from rows produced by
  `corpus.py` (or loaded via `--from-corpus` from previous run). No
  new schema, no parallel data path. If `SCHEMA_VERSION` bumps,
  `recommend.py`'s row-reader is one of downstream consumers that
  must be updated in same PR — contract is checked by
  `test_recommend.py` against `CORPUS_ROW_KEYS`.
- **Predicate semantics are part of user-visible contract.**
  `--target-vmaf T` returns *smallest CRF* whose `vmaf_score >= T`
  (falling back to closest-miss when nothing clears, marked
  `(UNMET)`). `--target-bitrate KBPS` returns row with minimum
  `|bitrate_kbps - KBPS|`, ties broken by smaller CRF. Two flags
  are mutually exclusive at argparse layer (exit code 2 when both
  passed). Changing any of these defaults is user-visible
  behaviour change requiring ADR.
- **Phase F 2-pass goes through adapter, not driver (ADR-0333).**
  Codecs opting into 2-pass encoding declare
  `supports_two_pass = True` and override
  `two_pass_args(pass_number, stats_path) -> tuple[str, ...]` on
  their adapter (today: `X264Adapter`, returning
  `('-pass', str(N), '-passlogfile', str(path))`, and
  `X265Adapter`, returning
  `('-x265-params', f'pass={N}:stats={path}')`). Encode driver
  (`encode.py`) calls adapter via
  `getattr(adapter, "supports_two_pass", False)`
  `adapter.two_pass_args(...)` — never branches on codec name.
  `EncodeRequest` carries `pass_number: int = 0` (0 = single-pass /
  default; 1 / 2 = pass index) and `stats_path: Path | None = None`.
  `build_ffmpeg_command` redirects pass-1 output to `-f null -` so
  throwaway encoded bitstream isn't written. 2-pass loop itself
  lives in `run_two_pass_encode` in `encode.py`; materialises
  stats file in `tempfile.mkdtemp` (or caller-supplied
  `scratch_dir`) and removes it (plus known encoder sidecars such
  as libx265's `.cutree`) on exit. When
  `supports_two_pass = False`, driver falls back to single-pass
  with stderr warning by default (`on_unsupported="fallback"`), or
  raises with `on_unsupported="raise"` — matches saliency.py
  "unsupported ROI encoder, fallback to plain encode" precedent.
  Sibling codec adapters (libsvtav1, libvvenc, libaom-av1) inherit
  this seam without touching driver — their PRs only need to
  override `supports_two_pass` + `two_pass_args` on adapter file.
  NVENC's `-multipass` is **not** this seam (single-invocation
  lookahead, not stats-file two-call sequence); separate adapter
  contract is follow-up if demand surfaces.
- **`two_pass_args` is implemented on every adapter (ADR-0546).**
  No adapter inherits protocol-default `NotImplementedError` body.
  `libaom-av1` + `libvvenc` are now `supports_two_pass=True`
  (FFmpeg generic `-pass N -passlogfile <prefix>`). `libsvtav1`
  returns same VBR-mode argv but stays `supports_two_pass=False`
  because SVT-AV1 enforces "CRF does not support multi-pass" at
  runtime — harness default mode is CRF, so driver falls back to
  single-pass. NVENC / QSV / AMF return their single-invocation
  in-encoder analysis flags (`-multipass fullres` /
  `-extbrc 1 -look_ahead_depth 40` / `-preanalysis true`) for pass
  1 and `()` for pass 2; callers compose pass-1 argv into
  `EncodeRequest.extra_params` for quality-boosted single-pass
  encode. All four VideoToolbox adapters raise typed
  `VideoToolboxTwoPassUnsupportedError` from `_videotoolbox_common`
  documenting that `VTCompressionSession` has no multi-pass C API.
  Do not regress these adapters back to bare
  `NotImplementedError` — search loop assumes contract is
  uniformly implemented.
- **AMF preset compression is fixed (ADR-0282).** 7-into-3 preset
  table in `codec_adapters/_amf_common.py` (`_PRESET_TO_AMF`) is
  cross-codec axis Phase B / C consumers depend on. Do not extend
  `presets` beyond canonical 7 names without amending ADR-0282 —
  registry uniformity that lets search loop ignore codec identity
  rests on every codec accepting same preset vocabulary. AV1
  (`av1_amf`) is RDNA3+ only; `ensure_amf_available` is runtime
  gate.

- **Phase E ladder math is two-pass and order-sensitive.**
  `convex_hull` in `ladder.py` runs (1) Pareto filter sorted by
  bitrate ascending, vmaf descending tie-break; (2) upper-convex
  envelope with `cross >= 0` pop predicate (drops
  accelerating-returns interior points so hull is concave /
  diminishing-returns end-to-end). Re-deriving hull from different
  starting condition is easy to get subtly wrong — algorithm is
  pinned by `test_ladder.py` invariants (monotonic both axes, no
  domination). Don't refactor without re-running that suite.
- **Phase E spacing names are part of CLI contract.** `--spacing
  log_bitrate` is the default, `--spacing vmaf` is the documented
  perceptual-spacing mode, and `uniform` is legacy alias for
  `vmaf`. Keep CLI choices and `ladder.select_knees()` aliases in
  lockstep so argparse cannot accept value library rejects.
- **Phase E sampler is pluggable; default is 5-point CRF sweep
  (ADR-0307).** `ladder.build_ladder` accepts explicit `sampler=`
  callback; when omitted, `_default_sampler` composes
  `corpus.iter_rows` (Phase A encode+score) with
  `recommend.pick_target_vmaf` (smallest CRF clearing target VMAF)
  over canonical sweep
  `DEFAULT_SAMPLER_CRF_SWEEP = (18, 23, 28, 33, 38)` at codec
  adapter's mid-range preset (`"medium"` for libx264 / libx265 /
  libsvtav1). 5-point sweep is load-bearing default; do not widen
  it without ADR-0307 follow-up — Phase E callers downstream size
  their wall-time budget against five encodes per
  (resolution, target_vmaf) cell. Callers needing finer grid,
  Bayesian bisect, or precomputed corpus stream pass explicit
  `sampler=` — that seam stays open. Tests stub `iter_rows` via
  `monkeypatch.setattr(corpus_module, "iter_rows", ...)`; lazy
  `from .corpus import iter_rows` inside `_default_sampler`
  resolves through patched module attribute on every call.
- **Saliency signal blend matches `vmaf-roi` (ADR-0293).**
  `saliency.py` deliberately mirrors `vmaf-roi`'s ADR-0247 signal
  blend (`offset = (2*sal − 1) * foreground_offset`, clamped to
  ±12). If `vmaf-roi`'s C-side blend changes, `saliency.py`
  follows in same PR — bit-for-bit equivalence is pinned by
  `tests/test_saliency.py` and is contract that lets us swap
  Python implementation for `vmaf-roi` shell-out later without
  behaviour drift. ONNX session is second test seam
  (`session_factory` parameter) — production callers leave it
  default; tests inject fake. Do not import `onnxruntime` at
  module top-level; lazy-load via `_import_onnxruntime` so corpus
  subcommand and unit tests work without it installed.
- **Compare predicate is recommend seam.**
  `compare.compare_codecs` takes
  `predicate(codec, src, target_vmaf) -> RecommendResult`
  callable. Programmatic default predicate returns `ok=False`
  pointing callers at
  `bisect.make_bisect_predicate(target_vmaf, *, width=...,
  height=..., framerate=..., duration_s=...)` because bare
  predicate signature does not carry source geometry.
  `vmaf-tune compare` CLI binds that Phase B
  ([ADR-0326](../../docs/adr/0326-vmaf-tune-phase-b-bisect.md))
  predicate from its explicit geometry flags by default;
  `--predicate-module MODULE:CALLABLE` hook is only supported way
  to bypass real bisect. `tests/test_compare.py` injects fake
  predicates so ranking is exercised without `ffmpeg` / `vmaf`
  binaries. Do not branch on codec name inside `compare.py` —
  route every per-codec call through predicate / adapter registry.
- **Phase G benchmark is read-only corpus analysis (ADR-0424).**
  `vmaf-tune benchmark` consumes existing Phase-A JSONL rows and
  must not call `ffmpeg`, `vmaf`, `compare.compare_codecs`, or
  Phase-B bisect. Its contract is one summary row per encoder:
  lowest-bitrate corpus point clearing `--target-vmaf`, with
  closest misses preserved as `status="unmet"`. Live encode
  comparisons stay in `compare`; offline corpus reports stay in
  `benchmark`.
- **Phase B bisect assumes monotone-decreasing VMAF in CRF
  ([ADR-0326](../../docs/adr/0326-vmaf-tune-phase-b-bisect.md)).**
  `vmaftune.bisect.bisect_target_vmaf` aborts with clear error when
  two non-adjacent samples violate this contract by more than 0.5
  VMAF (looser than measurement noise). Never weaken to fall-back
  search strategy on monotonicity violation — contract is part of
  public surface, and surfacing violation is more useful than
  papering over it. Real-world content + modern codecs satisfy
  contract; pathological exceptions are encoder bugs we want to
  see, not absorb. Subprocess seam mirrors `encode.run_encode` /
  `score.run_score`: tests inject `encode_runner` / `score_runner`
  stubs; production callers leave them `None`.
- **`COMPARE_ROW_KEYS` is JSON / CSV output contract** for
  `vmaf-tune compare`. Same maintenance discipline as
  `CORPUS_ROW_KEYS`: adding optional keys with default is fine,
  renaming or removing keys requires bumping schema and updating
  every downstream consumer in same PR.
- **`bisect_target_vmaf` public kwarg `workdir`** — added by
  ADR-0549. Resolution order: `workdir=` kwarg (explicit Path) >
  `VMAFTUNE_WORKDIR` env var > OS default (`/tmp`). Private
  helpers `_workdir_parent`, `_estimate_yuv_bytes`, and
  `_check_disk_space` are **not** in `__all__` —
  `test_module_exports_match_public_surface` in
  `tests/test_bisect.py` pins exact set `{BisectResult,
  BisectSample, bisect_target_vmaf, make_bisect_predicate}`. Adding
  private helpers to `__all__` will trip that test; import them
  directly in tests if needed. `make_bisect_predicate` forwarding
  call in `_run_compare` (cli.py) includes `workdir=args.workdir`;
  any new compare-path caller must carry this kwarg through or
  `test_cli_compare_binds_real_bisect_predicate` assertion will
  catch omission.
  ([ADR-0549](../../docs/adr/0549-vmaftune-workdir-relocation.md))
- **Score backend selection is strict-by-default
  ([ADR-0299](../../docs/adr/0299-vmaf-tune-gpu-score.md)).**
  `score_backend.select_backend(prefer)` honours `cuda` / `sycl` /
  `hip` / `cpu` exactly — if requested backend not available, it
  raises `BackendUnavailableError` rather than silently falling
  back to CPU. Only `prefer="auto"` walks fallback chain. Do not
  "fix" strict-mode test that fails on CI runner without GPU by
  adding silent fallback to `select_backend`; strict guarantee is
  load-bearing for operator wall-clock expectations. Mock
  `available` argument or `runner` instead.
- **`--score-backend` argparse choices are kept in sync with
  `score_backend.ALL_BACKENDS` and libvmaf's `--backend NAME`
  vocabulary ([ADR-0314](../../docs/adr/0314-vmaf-tune-score-backend-vulkan.md) /
  [ADR-0726](../../docs/adr/0726-drop-vulkan-backend.md)).**
  Do NOT add new value (e.g. `metal`) to argparse `choices` tuple
  in `cli.py` without corresponding libvmaf-side wiring landing in
  same release. Four current values (`cpu`, `cuda`, `sycl`, `hip`)
  are exact set libvmaf CLI accepts post-ADR-0726 (Vulkan dropped
  2026-05-28); widening harness without widening binary produces
  silent strict-mode failures on hosts that probe positively for
  new value. Cross-reference: `core/tools/cli_parse.c` `--backend`
  alternation.
- **HDR detection is fail-safe to SDR (ADR-0295).**
  `hdr.detect_hdr` returns `None` on any classification ambiguity
  (missing file, ffprobe failure, malformed JSON, mismatched
  primaries vs. PQ/HLG transfer). Misclassifying SDR as HDR is
  dangerous failure mode (would inject mismatched signaling into
  Rec.709 encode); misclassifying HDR as SDR is recoverable. Do not
  relax BT.2020 primaries gate in `_classify_payload` without
  ADR superseding 0261.
- **HDR codec dispatch table is contract for codec adapters.**
  `hdr.hdr_codec_args` dispatches per `encoder` name. When new
  codec adapter (libx265, libsvtav1, ...) lands under
  `codec_adapters/`, it inherits dispatch row that already exists;
  adapters do not roll their own HDR flag set.
- **`auto` records HDR args through same dispatch table.**
  `run_auto` must call `hdr_codec_args(codec, info)` per cell when
  `meta.is_hdr` is true. Generic tuple such as
  `("-color_primaries", "bt2020", "-color_trc", "smpte2084")`
  is insufficient because x265, SVT-AV1, HEVC hardware encoders,
  AV1 hardware encoders, and VVenC use different ffmpeg flag
  families. Hardware HEVC rows force `p010le` + `main10`; hardware
  AV1 rows force `p010le`; codec-private SEI flags stay limited to
  families with stable FFmpeg knobs. Tests in
  `tests/test_auto_short_circuits.py` lock this per-codec shape.
- **`select_hdr_vmaf_model` falls back silently.** When
  `model/vmaf_hdr_*.json` is absent (current state — fork hasn't
  ported Netflix's HDR model yet), `_resolve_vmaf_model` logs
  warning and returns SDR model. Do not change this to raise —
  HDR encode-side correctness ships independently of HDR scoring.
- **`model/vmaf_hdr_model_card.md` is documentation, not weights**
  ([research-0089](../../docs/research/0089-hdr-vmaf-model-search.md);
  ADR-0300 status update 2026-05-09). File is `.md`, not `.json`,
  so `select_hdr_vmaf_model`'s `vmaf_hdr_*.json` glob does **not**
  match it and continues to return `None`. Do not rename card to
  `.json`, do not relax resolver glob to also match `.md`, and do not
  synthesise placeholder weights. SDR-fallback path with one-shot
  warning is deliberate Path C outcome until either Netflix
  open-sources `vmaf_hdr_v0.6.1.json` upstream or fork acquires
  permissively-licensed HDR-MOS-labelled training corpus.
- **HDR is resolved once per source in `corpus.iter_rows`** (HP-2,
  ADR-0300 status update 2026-05-08). `_resolve_hdr` returns
  `(HdrInfo | None, forced: bool)`; `hdr_codec_args` runs once and
  resulting argv tail rides on every cell's
  `EncodeRequest.extra_params`. Do **not** re-probe ffprobe per
  cell (would burn ffprobe per encode for constant signal), and do not
  move HDR-mode resolution into `_row_for` (decision drives
  encode argv, so it must precede encode). One-shot
  HDR-VMAF-model warning fires once per `iter_rows` invocation via
  `score_model_warned` mutable flag — keep that semantics or
  operators get N spurious warnings on single corpus run.
- **Cache key fields are load-bearing
  ([ADR-0298](../../docs/adr/0298-vmaf-tune-cache.md)).**
  `cache_key()` function in `cache.py` digests six fields:
  `src_sha256`, `encoder`, `preset`, `crf`, `adapter_version`,
  `ffmpeg_version`. Dropping any one of them is silent correctness
  bug — stale entries shadow real results when adapter or ffmpeg
  is upgraded. Contract is asserted by
  `test_cache_key_diffs_on_each_field`. When adding new codec
  adapter, set `adapter_version: str` on dataclass; registry
  `Protocol` already requires it. Bump string when adapter's argv
  shape, preset list, or quality range changes.
- **Cache content stays opaque.** Cache value is parsed
  `(bitrate, vmaf, encode_time, score_time)` tuple plus opaque
  `<key>.bin` blob. Do not bake cache contents into JSONL row —
  row is canonical record, cache is sidecar. Cache hit must
  produce row that is bit-identical to cache miss (modulo
  `encode_path`, which stays empty unless `--keep-encodes`).
- **Sample-clip windows are mirrored on both sides**
  ([ADR-0301](../../docs/adr/0301-vmaf-tune-sample-clip.md)).
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
  duplicated (ADR-0296).** `corpus.coarse_to_fine_search()` builds
  two `dataclasses.replace(job, cells=...)` jobs (coarse + fine)
  and delegates to `iter_rows` for each. Do **not** factor out
  parallel encoder dispatch path inside search loop — JSONL row
  schema, encode-failure handling, and `keep_encodes` cleanup all
  live in `iter_rows`, and forking search loop loses them. New
  search strategies (binary, Bayesian) should follow same pattern:
  build list of `(preset, crf)` cells, call `iter_rows`,
  post-process emitted rows.
- **Adapter `quality_range` is search-space boundary, not
  user-input gate (ADR-0296).** Widening libx264's range from `(15,
  40)` to `(0, 51)` was deliberate: recommend / coarse-to-fine
  flow must be allowed to probe boundary CRFs to
  bracket answer. If future codec adapter wants to restrict
  *user-visible* range on `--crf NNN`, do that at CLI layer, not
  in `adapter.validate`.

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

- **QSV adapters share `_qsv_common.py`.** Three encoders with
  identical parameter shape (preset vocabulary, ICQ
  `global_quality` window) is deliberate exception to "one file
  per codec, nothing shared" Phase A convention. Per ADR-0281,
  future codec families that share parameter shape (NVENC's three
  encoders, AMF's three encoders, VideoToolbox's two H.264 + HEVC
  encoders) follow same pattern: one `_<family>_common.py` private
  module, thin dataclass adapters. Single-codec families stay
  flat.
- **Apple VideoToolbox adapters share `_videotoolbox_common.py`
  (ADR-0283 + ADR-0283 *Status update 2026-05-09*).** Three
  encoders (`h264_videotoolbox`, `hevc_videotoolbox`,
  `prores_videotoolbox`) reuse nine-name preset → `-realtime`
  boolean mapping. H.264 and HEVC share single `-q:v` 0..100
  quality knob (higher = better; `invert_quality=False`). ProRes
  uses `-profile:v` instead — it is fixed-rate intermediate codec,
  so harness's `crf` slot carries integer tier id (0=`proxy` →
  5=`xq`); adapter has its own validator
  `validate_prores_videotoolbox()` and integer-id-to-FFmpeg-alias
  helper `prores_profile_name()`. Per codec-adapter contract,
  search loop never branches on adapter identity — it consumes
  `quality_range` + `ffmpeg_codec_args(...)` uniformly. AV1
  hardware encoding is intentionally absent — Apple Silicon has
  no AV1 hardware encoder block as of 2026 and FFmpeg exposes no
  `av1_videotoolbox`. Tests mock `subprocess.run`; suite runs on
  Linux CI without macOS. End-to-end VT exercise left to
  contributors with macOS + VideoToolbox available locally
  (ProRes additionally requires M1 Pro / Max / Ultra or later —
  Intel Macs with T2 do not have ProRes hardware block).
- **Encode pipeline (`encode.py`) is still x264-CRF-tied.**
  ADR-0281 added QSV adapter classes but did not widen
  `build_ffmpeg_command` to dispatch on `adapter.quality_knob`.
  Until that follow-up lands, QSV adapters validate
  `(preset, global_quality)` correctly but harness will not yet
  successfully drive QSV encode end-to-end.
- **Subprocess boundary is test seam.** `encode.run_encode`,
  `score.run_score`, and QSV `ffmpeg_supports_encoder` probe
  accept `runner` argument that defaults to `subprocess.run`.
  Tests inject fake; production callers leave it default. Do not
  reach for `os.system` / `popen` shortcuts —
  `tests/test_corpus.py` and `tests/test_codec_adapter_qsv.py`
  will silently stop covering path.

## Phase scope (codec registry)

Phase A (original scaffold): grid sweep + JSONL emit, x264 only.
ADR-0281 added three QSV codec adapters as one-file extension off
registry; encode-pipeline widening that makes them functional is
itself separate Phase A follow-up. Phases B–F per ADR-0237
(bisect / predictor / ladder / MCP) remain explicitly out of
scope here; do not add that code into this tree without ADR-0237
follow-up promoting corresponding phase.
Phase A (corpus generation): grid sweep + JSONL emit, x264 only.
Phase D (per-shot CRF tuning, ADR-0276): orchestrates shot
detection (via C-side `vmaf-perShot` binary, ADR-0222), extracts
each shot to raw YUV, and binds pluggable per-shot CRF predicate
to Phase B's real bisect backend by default. CLI deliberately
stops before running final segment encodes — it emits FFmpeg
encoding plan as JSON plus optional shell script.
`--predicate-module` remains advanced custom/test escape hatch;
it is no longer production path.

Phases B (target-VMAF bisect), C (per-title CRF predictor), E
(Pareto ABR ladder) and F (MCP tools) per ADR-0237 are explicitly
out of scope here; do not add bisect / predictor / ladder / MCP
code into this tree without ADR-0237 follow-up promoting
corresponding phase.

## Phase D rebase-sensitive invariants

- **Predicate signature is Phase B contract.** ``PredicateFn``
  type alias in ``per_shot.py`` is ``(Shot, target_vmaf: float,
  encoder: str) -> (crf: int, measured_or_predicted_vmaf: float)``.
  CLI adapter around Phase-B bisect must conform to this
  signature; widening return tuple is coordinated change that
  bumps public-API surface across both modules in same PR.
- **CLI default is real per-shot bisect.**
  `vmaf-tune tune-per-shot` must call Phase-B bisect backend
  unless `--predicate-module MODULE:CALLABLE` is explicitly
  supplied. Do not reintroduce adapter-default CRF as CLI
  behaviour; that fallback exists only for library dry runs that
  call `tune_per_shot()` without predicate.
- **Bisect inputs are temporary raw YUV shots.**
  `bisect_target_vmaf` expects raw YUV geometry, so CLI extracts
  each detected half-open shot range to temporary raw-YUV file
  before calling it. Raw `.yuv` / `.raw` sources are opened with
  explicit rawvideo demuxer flags (`--width`, `--height`,
  `--pix-fmt`, `--framerate`); container and Y4M sources are left
  to FFmpeg's demuxer.
- **Shot ranges are half-open inside Python.** C-side
  ``vmaf-perShot`` JSON/CSV sidecar uses inclusive ``end_frame``;
  ``per_shot.py`` normalises into ``[start_frame, end_frame)`` at
  parse boundary. ``Shot.length`` and ``-frames:v`` arg in
  ``_segment_command`` both depend on half-open form. Do not
  "round-trip back to inclusive" — every downstream consumer
  assumes half-open form.
- **``vmaf-perShot`` binary surface is canonical detector.** Do not
  add parallel ONNX-Runtime-from-Python detector path. When
  TransNet V2 is hot-pathed (e.g. Phase E ladder generation
  re-running detection), extend ``detect_shots`` to call
  ``vmaf-perShot`` once and cache, not to bypass binary.
- **Scene-threshold + uniform-window splitter (ADR-0512).**
  ``detect_shots`` accepts ``diff_threshold`` (forwarded to C
  binary as ``--diff-threshold``) and ``max_shot_duration_sec``
  (post-processing splitter, requires ``framerate``). CLI exposes
  these as ``--scene-threshold`` and ``--max-shot-duration``
  (default ``2.0 s``, ``0`` disables). Splitter is intentionally
  default-on so 5 s clips always produce ``>= 2`` shots even when
  luma-delta heuristic under-cuts; lowering default is behavioural
  change that must come with fresh empirical calibration against
  BBB e2e fixtures. ``split_long_shots`` helper preserves
  contiguity (``out[i].end_frame == out[i+1].start_frame``) and
  distributes remainder so partition lengths differ by at most
  one frame — both invariants are covered by ``test_per_shot.py``
  and downstream merge / concat-listing code depends on
  contiguity property.
- **Segment-dir priority order is load-bearing (ADR-0532).** CLI
- **Segment-dir priority order is load-bearing (ADR-0530).** CLI
  resolves concat-listing directory in this exact order: (1)
  ``--segment-dir`` when set; (2) ``plan_out.parent / "segments"``
  when ``--plan-out`` is set; (3) ``output.parent / "segments"``
  otherwise. Order (2) ensures concat listing lands alongside
  plan JSON on writable path — plan write already succeeded at
  that point, so parent is guaranteed writable.
  ``write_concat_listing`` call is wrapped in ``OSError`` catch;
  failure emits ``WARN`` to stderr and command exits 0 (plan JSON
  is authoritative deliverable). Do not collapse orders (2) and
  (3) without updating this invariant and
  ``test_per_shot.py::test_cli_tune_per_shot_readonly_cwd_returns_zero``.
- **Shot detection runs once per source, never per cell.** Corpus
  driver (``corpus._resolve_shot_metadata``) calls
  ``_detect_shots_with_status`` at top of ``iter_rows`` and
  passes resulting ``ShotMetadata`` down to every
  ``(preset, crf)`` row via ``_row_for``. Moving call inside cell
  loop roughly doubles corpus wall time on TransNet-V2.
  ``_detect_shots_with_status`` is only API that returns
  ``(shots, ok)`` tuple needed to distinguish real single-shot
  source from "binary failed" fallback — public ``detect_shots``
  shape cannot carry that flag.
- **HDR VMAF model resolution goes through
  ``hdr.select_hdr_vmaf_model``.** Canonical filename is
  ``vmaf_hdr_v0.6.1.json`` (Netflix's research-artefact name).
  Route lookups through ``hdr_model_name_for(transfer)`` so
  future Dolby-Vision-specific model entry is one dispatch-table
  row away. "HDR model not shipped" warning is single-shot per
  process; clear it from tests via
  ``hdr.reset_hdr_model_warning()``.
Phase A (this scaffold): grid sweep + JSONL emit. Codecs wired so
far: `libx264` (ADR-0237) and `libsvtav1` (ADR-0294). Phases B–F
per ADR-0237 are explicitly out of scope here; do not add bisect /
predictor / ladder / MCP code into this tree without ADR-0237
follow-up promoting corresponding phase.
Phase A (corpus scaffold): grid sweep + JSONL emit, x264 only.
Phase E (this scaffold): per-title bitrate-ladder generator (Pareto
hull + manifest emit), sampler-pluggable, smoke-only until Phase B
merges. Phases B / C / D / F per ADR-0237 are explicitly out of
scope here; do not add bisect / predictor / per-shot / MCP code
into this tree without ADR-0237 follow-up promoting corresponding
phase.

- **Seven F.2 short-circuit predicates in ``auto.py`` are ordered
  tuple, not set.** ``SHORT_CIRCUIT_PREDICATES`` declares
  ``ShortCircuit.LADDER_SINGLE_RUNG`` first and
  ``ShortCircuit.SKIP_PER_SHOT`` last; order is part of public
  contract because tests assert determinism across
  `evaluate_short_circuits` invocations and JSON schema records
  canonical-order list under ``plan.metadata.short_circuits``.
  Adding eighth short-circuit (F.3+ follow-ups) appends to tuple;
  never insert in middle. Phase D thresholds
  (`PHASE_D_DURATION_GATE_S = 300.0` and
  `PHASE_D_SHOT_VARIANCE_GATE = 0.15`) are placeholders pending
  F.3 empirical fit — change them via ADR-0325 follow-up, not
  drive-by tweak. See
  [ADR-0325](../../docs/adr/0325-vmaf-tune-phase-f-auto.md).

- **F.3 confidence-aware thresholds are corpus-derived; do not
  hand-pick.** `DEFAULT_TIGHT_INTERVAL_MAX_WIDTH = 2.0` and
  `DEFAULT_WIDE_INTERVAL_MIN_WIDTH = 5.0` in `auto.py` are
  emergency floor (Research-0067), not target. Production
  thresholds load from calibration JSON sidecar emitted by
  conformal-VQA pipeline (ADR-0279) — keys
  `tight_interval_max_width` and `wide_interval_min_width`.
  `load_confidence_thresholds` falls back to defaults with
  one-line WARNING when no sidecar found; do not silence that
  warning, and do not "tune" defaults to make failing integration
  test pass. Fix for surprising cell escalations on real data is
  recalibration PR, not threshold loosening here (CLAUDE.md
  `feedback_no_test_weakening`). Decision helper
  `_confidence_aware_escalation` is pure function of
  `(verdict, interval_width, thresholds)` so it stays trivially
  unit-testable; keep it pure when extending decision table.
  `run_auto` must pass recipe-adjusted `effective_thresholds` from
  `_apply_recipe_override` into every F.3 decision and into
  `plan.metadata.confidence_thresholds`; computing adjusted value
  and then falling back to `ConfidenceThresholds()` is user-visible
  planning bug.

- **Fast-NR calibration sidecars are write-gated before tune
  consumes them.** `NRProxyBackend` intentionally trusts
  `calibration_slope`, `calibration_intercept`, and
  `calibration_threshold` once they are in `nr_metric_v1.json`;
  safety boundary is `ai/scripts/calibrate_nr_threshold.py`
  (ADR-0665), which refuses weak sample-count or PLCC fits by
  default. If fresh real-corpus run is rejected, fix NR
  model/features or training corpus; do not loosen vmaf-tune
  early-elimination logic to make bad sidecar useful.

- **Profile-card reports start with run-specific takeaways.**
  `report.py::_quick_takeaways` is single source for Markdown and
  HTML "Quick takeaways" section (ADR-0666). It must stay derived
  from `ReportData`, not from rendered text or browser-side
  JavaScript, so Markdown, HTML, tests, and future PDF/export
  paths agree on same recommendation summary.

- **F.4 recipe overrides are read-only factories, not literal
  dicts.** `_CONTENT_RECIPE_TABLE` in `auto.py` stores
  **callables** (`_animation_recipe`, `_screen_content_recipe`,
  `_live_action_hdr_recipe`, `_ugc_recipe`, `_empty_recipe`);
  every call returns fresh dict so caller mutating return value
  cannot leak mutation into next `run_auto` invocation. Tests in
  `tests/test_auto_recipe_overrides.py` assert this invariant
  explicitly. Adding new content class means adding factory
  function and `RECIPE_CLASS_<NAME>` constant; never inline
  literal dict into table or mutate one in place. Four override
  keys (`tight_interval_max_width`, `force_single_rung`,
  `saliency_intensity`, `target_vmaf_offset`) are only keys
  driver honours — `get_recipe_for_class` filters by
  `_RECIPE_KEYS` allowlist as defence-in-depth. Every threshold
  value shipped at F.4 is
  `[provisional, calibrate against real corpus in F.5]`; do not
  promote placeholder to "calibrated" in drive-by edit. Per
  memory `feedback_no_test_weakening`, `target_vmaf_offset` shifts
  only predictor's effective target; input `--target-vmaf` (gate
  that ships models) is preserved verbatim in
  `plan.metadata.target_vmaf`. See
  [ADR-0325](../../docs/adr/0325-vmaf-tune-phase-f-auto.md) §F.4.

## ADR-0332 invariants (encoder-internal stats capture)

- Corpus row schema is at v3; new columns added to
  ``CORPUS_ROW_KEYS`` and ``SCHEMA_VERSION`` must keep v3 ten
  ``enc_internal_*`` columns positionally stable so v2 readers see
  zero rather than missing key. Coordinates with ADR-0302.
- Every adapter in ``codec_adapters/`` must declare
  ``supports_encoder_stats: bool`` (no Protocol default). x264 /
  x265 set True; everything else False until codec-specific
  parser lands. x265's ``q-aq`` and ``icu`` / ``pcu`` / ``scu``
  pass-1 aliases are intentionally normalised in
  ``encoder_stats.py`` so corpus rows keep same ten
  ``enc_internal_*`` columns as x264.
- ``run_encode_with_stats`` doubles per-encode wall-clock on
  opt-in adapters by design. Do not collapse pass-1 + pass-2 calls
  into one — encoder won't emit parseable stats file outside
  ``-pass 1`` mode.

## Sidecar (ADR-0325) rebase-sensitive invariants

- **`FEATURE_DIM = 14` and column order in
  `sidecar._feature_vector` are load-bearing pin** for online-ridge
  state. Adding or reordering features without bumping
  `SIDECAR_SCHEMA_VERSION` will silently align saved weights to
  wrong column on load. Leading `1.0` bias / intercept term must
  stay at column 0; it is what lets ridge fit absorb constant
  offset between predicted and observed VMAF.
- **`SidecarConfig.predictor_version` is contract that
  invalidates stale corrections** when shipped predictor upgrades.
  Tag mismatch on `SidecarModel.from_dict` raises and caller
  (`SidecarModel.load`) falls back to cold-start model. Do not
  catch mismatch and "rescale" — stale correction trained against
  previous predictor's residuals is worse than no correction.
- **Host UUID is anonymous by construction.** It is generated by
  `secrets.token_hex(16)` on first install and persisted at
  `<cache_dir>/host-uuid`. **Never** swap it for `uuid.getnode()`
  / `socket.gethostname()` / `/etc/machine-id` / CPUID — that
  would re-identify operator and break privacy precondition for
  future opt-in upload PR (ADR-0325 §Future work).
- **Sidecar state is local-only by default.** Harness has no
  upload code path. Adding one requires dedicated opt-in upload
  ADR + signing chain spelled out in ADR-0325 §Future work. Do not
  slip network call into `SidecarPredictor` or any of its callers
  without that ADR landing first.

## Predictor stub-models policy (ADR-0325)

Fork ships one `model/predictor_<codec>.onnx` per codec adapter.
As of 2026-05-14 NVENC / QSV predictors (`h264_nvenc`,
`hevc_nvenc`, `av1_nvenc`, `h264_qsv`, `hevc_qsv`, `av1_qsv`) are
real-corpus retrains from `runs/phase_a/full_grid/comprehensive.jsonl`
and their cards carry `corpus.kind: real-N=<rows>`. Software and
AMF predictors remain synthetic stubs until matching real corpora
exist. Trainer
(`tools/vmaf-tune/src/vmaftune/predictor_train.py`) sources its
`CODECS` tuple from `predictor._DEFAULT_COEFFS` so two stay
single-source. When new codec adapter is added (e.g. future
`vp9_qsv` row in `_DEFAULT_COEFFS`), same PR must:

1. Re-run `python3 -m vmaftune.predictor_train --output-dir model`
   to produce matching `predictor_<codec>.onnx` + card.
2. Commit new ONNX bytes — shipped-model smoke test
   parameterises over `CODECS` and fails if coefficient row has
   no shipped artefact.
3. Refresh model card's `corpus.kind` line on every retrain
   (trainer does this automatically; review diff).

Stub models are explicitly **not** for production CRF picks.
Synthetic target *is* analytical fallback, so PLCC / SROCC numbers
in stub cards are artificially high. Real-corpus retrains follow
same trainer entry point with `--corpus path/to/file.jsonl`
or `--corpus path/to/corpus-dir/` and produce honest metrics.
Directory corpus inputs are recursive and sorted so
`.corpus/corpus_run/` trains deterministically without
manual concatenation step. Keep that directory handling reachable
from both `train_all_codecs()` and CLI; file-only `is_file()`
guards above `load_corpus()` silently turn real corpus
directories back into synthetic stubs. Loader accepts both
canonical `encoder` / `crf` / `vmaf_score` /
`bitrate_kbps` rows and historical hardware-sweep `codec` / `q` /
`vmaf` / `actual_kbps` aliases; do not reintroduce external
conversion scripts for those local corpora.

- **`corpus.py` uses `aiutils` helpers for file hashing and
  timestamps.** `_sha256_file` (imported as
  `aiutils.file_utils.sha256`) and `_utc_now_iso` (imported as
  `aiutils.time_utils.now_iso_8601`) replace formerly inline
  `_sha256_of` and `_utc_now_iso` functions. Module adds `ai/src`
  to `sys.path` at import time so callers on plain dev clone
  (without `aiutils` installed as editable package) still resolve
  import. Do not reintroduce inline duplicates of either helper —
  canonical implementations live in `ai/src/aiutils/`.
- **`ScoreRequest.duration_s` is decode-clamp, not score window
  (ADR-0498, Bug #v2-A).** New optional field threads down through
  `maybe_decode_distorted` / `_decode_to_raw_yuv` into ffmpeg `-t`
  output clamp. 10 s probe against 634 s source doesn't
  materialise tens of gigabytes of raw YUV. Score window is still
  driven by `frame_skip_ref` / `frame_cnt`; `duration_s` is purely
  disk-budget gate for container -> raw YUV decode step. Default
  `0.0` preserves legacy full-source decode.
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
- **Encoder-version probe is process-cached fallback (ADR-0498,
  follow-up #7).** `encode._probe_encoder_version_from_ffmpeg`
  runs at most once per `(ffmpeg_bin, encoder)` pair via
  `_PROBE_CACHE` (module-scope dict). Tests that exercise fallback
  must clear `_PROBE_CACHE` explicitly. Probe parses
  `ffmpeg -version`'s configuration line and returns
  `"<encoder>-enabled"` when encoder is compiled in; empty string
  lets caller keep its `"unknown"` placeholder so existing tests
  that pin that exact value still pass.
  `_VERSION_PROBE_PATTERNS` now covers `libx264`, `libsvtav1`,
  `libx265`, `libvpx-vp9`, `libaom-av1`, and `libvvenc` (ADR-1077).
  Tests for any of these codecs that use fake runner and don't
  return `--enable-*` text in stdout must capture only first
  subprocess call (encode argv), not last. Probe fires second
  `ffmpeg -version` call when encoder banner absent from encode
  stderr.
  `encode.probe_encoder_info(ffmpeg_bin, encoder)` returns
  `EncoderInfo(encoder, codec_detected, version_label)` — callers
  should use this rather than re-parsing version string.
- **`fast._build_production_sample_extractor` accepts `backend`
  kwarg (ADR-0498 follow-up #7).** Pass `backend=select_backend(...)`
  so TPE proxy trials score on GPU rather than always defaulting
  to CPU. `_build_prod_predictor` and `fast_recommend` forward
  selected backend automatically; test seams that inject custom
  `sample_extractor` callable are unaffected.
- **`codec_adapters.parse_available_codecs(stdout, *, restrict_to_known)`
  (ADR-0498 follow-up #7).** Parses `ffmpeg -hide_banner -encoders`
  output into frozenset of codec names. Set
  `restrict_to_known=False` to get full ffmpeg encoder list;
  default restricts to adapter registry so callers can intersect
  with `known_codecs()`.
- **`_maybe_decode_reference` scales reference YUV to rung target
  on cross-resolution sweeps (ADR-0501, Bug #V4-B).** When
  `CorpusJob.src_width / src_height` differs from `width / height`,
  `iter_rows` passes rung target to `_maybe_decode_reference`
  which appends `-vf scale=W:H` to ffmpeg decode argv. It embeds
  dims in sidecar filename (`<src>.ref.decoded.<W>x<H>.yuv`) so
  multi-rung sweeps don't collide on stale path.
  Single-resolution rungs (src dims == rung dims, or both `None`)
  keep legacy native-geometry decode. Without this scale, libvmaf
  CLI silently mis-parses planar bytes (1080p reference handed to
  720p-reading CLI = ~21 VMAF instead of ~93) and collapses ladder
  grid.
- **`vmaf-tune-ladder/v1` JSON always emits `samples[]` array
  (ADR-0501, Bug #V4-B).** `emit_manifest(format="json", samples=…)`
  threads pre-hull sampler cloud through `build_and_emit`. Empty
  array when no cloud wired — never missing key — so consumers can
  read `payload["samples"]` unconditionally. HLS / DASH emitters
  silently ignore `samples=` kwarg.
- **`_run_report` separates infrastructure-gap rows from real
  failures (ADR-0501, Bug #V4-C).** Row whose `error` starts with
  `"encoder unavailable"` (bisect discriminator prefix added in
  ADR-0498 follow-up #6) raises new top-level `degraded=true` flag
  without flipping `ok=false`. `ok=true` requires
  `at_least_one_row_succeeded AND no_real_failure`; `ok=false`
  whenever any non-unavailable row fails. New counter
  `codec_rows_unavailable` exposes gap count for dashboards.
  Changing prefix in `bisect._predicate_for_codec` must update
  this aggregator too.
- **Report JSON carries encoder-profile contract (ADR-0643).**
  `ReportData.to_dict()` embeds
  `encoder_profile.schema == "vmaftune.encoder_profile.v1"` and
  `vmaf-tune encode-profile` reads that payload from JSON, HTML,
  or Markdown reports. Do not drop successful rows, failed rows,
  codec metadata, source geometry, `pix_fmt`, binary provenance,
  or `selected_pareto`; profile reader uses them to choose exactly
  one recommendation and construct FFmpeg argv. HTML escaping of
  raw JSON `<pre>` is part of contract because reader unescapes it
  before `json.loads`.
- **Profile-card `--format both` means all three artifacts.** Both
  `compare` and `report` call
  `report.write_report_outputs`; do not restore CLI-local writer copies.
  The helper emits machine-readable `.json` before the `.html` and `.md`
  renders, and returns those paths in that order.
  `--json-sidecar` adds JSON to a single-format HTML or Markdown run;
  it is not required for `both`. Regression tests must call the
  production writer or CLI entry point rather than copy its dispatch
  logic into a test helper.
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
- **Sample cloud is full per-CRF sweep, de-duplicated by
  `(width, height, crf)` (ADR-0505, Bug #V5-2 + #V5-3).** CLI's
  `_run_ladder` constructs local `cloud_sink: list[LadderPoint]`,
  passes it to `make_default_sampler(cloud_sink=…)`, and threads
  it into `build_and_emit(extra_samples=…)`. Sampler appends every
  successfully-scored CRF row from `iter_rows` into sink before
  `pick_target_vmaf` collapses cell. Emitted `samples[]` array
  carries every encoded CRF row per resolution instead of
  one-row-per-target-cell (V4 emit shape). Emit-side
  `_dedup_samples` pass keys on `(width, height, crf)` so two
  targets converging on same CRF emit one sample row, not two.
  `_run_ladder` test stubs that fabricate `LadderPoint` instances
  with same `(w, h, crf)` triple across different cells will
  collapse in JSON descriptor as designed.
- **`ladder --duration N` bounds encode pipe AND reference decode
  (ADR-0506, Bug #V6-1).** `EncodeRequest.duration_s` is plumbed
  by `iter_rows` from `CorpusJob.duration_s`;
  `build_ffmpeg_command` appends `-t duration_s` as input-side
  flag iff `sample_clip_seconds == 0.0 AND duration_s > 0`.
  Regression that drops `duration_s` field or skips fallback
  branch re-introduces "ladder smoke run takes 10 minutes per
  cell" bug. Encoder will process full source while only
  `duration_s` seconds of reference is decoded for scoring.
  Sample-clip mode (ADR-0297) keeps precedence because it carries
  centred start offset.
- **Raw-YUV reference decode emits demuxer-side flags before
  `-i` (ADR-0506, Bug #V6-2).** `_decode_source_to_yuv` requires
  `source_width` / `source_height` when `source_is_raw=True`
  (raises `ValueError` otherwise) and prepends `-f rawvideo
  -pix_fmt <pf> -s SRCWxSRCH -r FR` before `-i`. Container path
  (`source_is_raw=False`, default) keeps auto-detect argv
  unchanged so every v3/v4/v5 container test still passes.
  `_maybe_decode_reference` derives `source_is_raw` from source
  suffix and forwards `iter_rows`'s
  `job.src_width / src_height / framerate` (or, when those are
  `None`, rung dims as legacy single-res case). Regression that
  drops demuxer-side block when raw is shape re-introduces
  "default sampler produced no scorable encodes" on every
  cross-res rung against raw source.
- **`_run_ladder` returns RC=2 on operational failure
  (ADR-0506, Bug #V6-3).** `build_and_emit` can legitimately
  raise `RuntimeError` (no scorable encodes) or `ValueError`
  (bad input). Wrapper prints exception message to stderr and
  returns 2; do not widen exception list to bare `Exception`
  (that would swallow programmer errors and `KeyboardInterrupt`).
- **`_run_compare` auto-probes container-source framerate /
  duration (ADR-0509, Bug #V7-1).** When `--src` is container
  (suffix outside `score.VMAF_RAW_SUFFIXES`) and user did NOT
  pass `--framerate` / `--duration` explicitly,
  `_resolve_compare_source_geometry` substitutes probed values
  from `vmaftune.report.probe_source` before building bisect
  predicate. "User passed explicitly" signal rides on
  `_TrackedDefaultAction` + `_stamp_tracked_default_sentinels`
  which set `args._<dest>_was_default = False` for any flag user
  explicitly named. Sentinel is intentionally inverted (default
  `True`, opted-out to `False`) because argparse never invokes
  `Action.__call__` on omitted flags. Explicit user overrides
  win; mismatches emit one-line stderr warning. Regression that
  bypasses helper (i.e. feeds `args.framerate` straight into
  `make_bisect_predicate`) re-opens v7 bug class. Encoder pulls
  frames at container's native rate but `frame_skip_ref` /
  `frame_cnt` walk reference YUV at different rate, collapsing
  VMAF to 4-90 band regardless of CRF. Tests pinning invariant
  live under `tests/test_compare.py` (the
  `test_resolve_compare_source_geometry_*` +
  `test_cli_compare_passes_probed_framerate_for_container_src`
  family); when adding new `_TrackedDefaultAction` flag, extend
  hardcoded tuple in `_stamp_tracked_default_sentinels`.
- **`vmaf-tune ladder --score-backend` resolves up-front;
  `tune-per-shot --score-backend` defers to libvmaf (ADR-0511).**
  `_run_ladder` calls
  `score_backend.select_backend(prefer=raw_backend, vmaf_bin=args.vmaf_bin)`
  BEFORE any encode starts so unavailable backend errors out
  with RC=2 and clear message instead of failing mid-sweep with
  cryptic libvmaf output. Resolved value threads through
  `make_default_sampler(score_backend=...)` →
  `CorpusOptions.score_backend` → `vmaf --backend $name`. When
  `auto` resolves to `cpu`, value passed downstream is `None` so
  corpus step omits explicit `--backend` flag and lets libvmaf
  use its own default (preserves legacy zero-flag invocation
  pattern). `_run_tune_per_shot` DELIBERATELY DOES NOT
  pre-resolve — `_build_per_shot_bisect_predicate` keeps
  historical
  `None if args.score_backend == "auto" else args.score_backend`
  conversion so `bisect_target_vmaf` receives `None` for auto and
  lets libvmaf pick live runtime at scoring time. Asymmetry is
  documented inline at top of `_run_tune_per_shot`; do not "fix"
  it without updating that contract and predicate tests.
- **`_build_per_shot_bisect_predicate` returns 2-tuple
  (ADR-0531).** Function now returns
  `(predicate_fn, bitrate_sidecar)` where `bitrate_sidecar` is
  `dict[tuple[int, int], float]` keyed by
  `(start_frame, end_frame)`. Predicate closure populates dict as
  each shot's bisect completes; `_run_tune_per_shot` annotates
  each `ShotRecommendation` via
  `dataclasses.replace(r, bitrate_kbps=...)`. Custom
  `--predicate-module` callers are unaffected (sidecar is empty
  for that path). Do not change function signature back to bare
  `PerShotPredicateFn` return without also wiring alternative
  bitrate capture path. Plan JSON schema now requires
  `bitrate_kbps` per shot, and its absence causes report
  renderer to show "—".
- **`ShotRecommendation.bitrate_kbps` defaults to NaN (ADR-0531).**
  Field carries measured segment bitrate from bisect predicate.
  NaN is correct for dry-run / synthetic predicates that never
  encode real segment. Plan-JSON emitter serialises NaN as `null`
  (RFC-8259-portable); report ingester treats `null` and absent as
  NaN and renders "—". Tests that construct `ShotRecommendation`
  directly need not set `bitrate_kbps` unless testing bitrate
  column.
- **`tune-per-shot` geometry auto-probe: patch `args` in-place
  before any downstream call (ADR-0542).** `_run_tune_per_shot`
  writes ffprobe-derived width, height, framerate, and
  total-frames back onto `args` namespace at top of function.
  This lets `_build_per_shot_bisect_predicate`, `merge_shots`,
  plan serialisation, and `detect_shots` call all receive
  consistent geometry without signature changes. Branch condition
  is
  `not _source_needs_rawvideo_demux(args.src)` — raw YUV (`.yuv` /
  `.raw`) sources still require explicit `--width` and `--height`
  and exit 2 if omitted. Do not add new geometry-consuming helper
  inside `_run_tune_per_shot` without reading `args.width` /
  `args.height` AFTER probe block, not before. Tests:
  `tests/test_tune_per_shot_container_src.py`.
- **`compare --no-bisect` skips bisect;
  `_run_compare_crf_sweep` owns schema-v3 output (ADR-0542).**
  When `args.no_bisect` is truthy, `_run_compare()` delegates
  immediately to
  `_run_compare_crf_sweep(args, encoders)` — normal bisect path
  is not entered. Sweep function calls `bisect._encode_and_score`
  directly (no iterative search), builds
  `{"schema_version": 3, "mode": "crf_sweep", "rows": [...]}`
  payload, and writes / prints JSON only (CSV / Markdown
  rendering is follow-up). `--target-vmaf` / `--target-vmafs` are
  parsed but act as label-only annotation; they do not influence
  encode loop. Do not short-circuit `_run_compare` before
  format-validation block (format guard still applies). Tests:
  `tests/test_compare_no_bisect.py`.
- **Pathlib-only filesystem ops (ADR-0936).** Fork-owned modules
  under `tools/vmaf-tune/src/` and `tools/vmaf-tune/tests/` use
  `pathlib.Path` exclusively for filesystem operations — no
  `os.path.*`, `os.replace`, `os.symlink`, `os.path.getsize`,
  `os.path.splitext`, builtin `open()` on path argument, or
  `glob.glob`. ruff `PTH` ruleset (flake8-use-pathlib) is enabled
  in `pyproject.toml` and will fail lint on regression. When
  refactoring atomic-rename plumbing (e.g. `EncodeCache.put` blob
  commit), prefer `tmp_path.replace(dst)` over
  `os.replace(tmp_path, dst)` — they are semantically identical,
  pathlib-method form.
- **`CodecAdapter` Protocol must declare every field every
  concrete adapter relies on (ADR-0888).**
  `codec_adapters/__init__.py` declares `CodecAdapter`
  `typing.Protocol`; every concrete adapter (`X264Adapter`,
  `LibaomAdapter`, NVENC / AMF / QSV / VideoToolbox / VVenC /
  SvtAv1 / libvpx) implements contract. When adding new field to
  *every* concrete adapter — most recently `presets: tuple[str, ...]`
  field consumed by `ladder._default_sampler_preset` — promote it
  to Protocol in same change. Concrete-only field that callers
  reach via `getattr(adapter, "presets")` silently drops type
  safety and pyright flags every cross-adapter call site. Protocol
  is also spec for `_REGISTRY: dict[str, CodecAdapter]` table;
  pyright variance rules require Protocol fields to be `Final` /
  read-only iff every adapter uses frozen dataclass — track that
  audit separately if new mutable-field adapter ever lands.
- **`_ladder_point_from_row` returns union annotation hides
  (ADR-0888).** `tools/vmaf-tune/src/vmaftune/ladder.py` documents
  that `UncertaintyLadderPoint` is deliberately NOT subclass of
  `LadderPoint` (the "subclassing would require runtime isinstance
  gymnastics" comment). Function returns either type at runtime,
  depending on whether row carries `vmaf_interval`; downstream tests
  (`test_ladder.py::test_build_ladder_default_sampler_preserves_vmaf_interval`)
  assert runtime variant. Return annotation is kept as narrower
  `LadderPoint` with explicit `cast` because widening whole
  `Ladder.points` chain cascades through public ladder API
  (`build_ladder`, `make_default_sampler`, `select_knees`,
  `convex_hull`). If future change does promote union into public
  surface, lift `cast` and audit every `Ladder.points` consumer
  for `isinstance` discriminators.

## _stamp_tracked_default_sentinels tuple invariant (ADR-1048)

Hard-coded tuple in `_stamp_tracked_default_sentinels` (cli.py
~line 147) must contain `dest=` value of every
`_TrackedDefaultAction` flag, not flag name. `ladder --duration`
uses `dest="duration_s"`, so both `"duration"` (corpus) and
`"duration_s"` (ladder) must be in tuple. When adding new
`_TrackedDefaultAction` flag with non-standard `dest=` argument,
add dest to tuple at same time or sentinel will never be set.

## HISS-04 helper-ordering invariants (tools-tune burn-down)

Splitting oversized functions into helpers moved three ordering
contracts out of single function bodies, where they were implicit,
into call sites. Each is silent when broken — no test name points at
it directly.

- **`_append_compare_rows` runs before `_append_sweep_rows`**
  (`report.py`, `build_encoder_profile`). Sweep pass reads
  `len(recommendations)` for its provisional `index`. Swapping calls
  renumbers rows before final sort overwrites them; result differs
  only if sort is ever made stable on `index`.
- **Sweep chart draw order** (`report.py`, `_sweep_plot_fn._plot`):
  codec curves, then `_draw_sweep_pareto`, then
  `_draw_sweep_failures`, then `_style_sweep_axes`. Matplotlib
  z-order plus `_style_sweep_axes` reading finished artist list via
  `get_legend_handles_labels` both depend on this. Reordering
  silently drops legend entries.
- **`_build_input_args` emits `-f rawvideo` block before seek args**
  (`encode.py`). `-ss` / `-t` must stay input-side, ahead of `-i`, or
  ffmpeg decodes whole source. ADR-0506 / Bug #V6-1.

`_parity_probe_args` (`tests/test_fast_parity.py`) holds probe
parameters shared with Go twin `cmd/vmafx-tune`. Changing width,
height, framerate, preset or `sample_chunk_seconds` there without
matching twin breaks parity test, not either implementation.
