<!-- markdownlint-disable MD013 MD060 -->
# AGENTS.md — ai/

Orientation for agents working on tiny-AI **training** side. Parent:
[../AGENTS.md](../AGENTS.md).

## Scope

Python package for training, exporting, registering tiny-AI
checkpoints, consumed by [core/src/dnn/](../core/src/dnn/AGENTS.md)
at runtime. Stack: PyTorch + Lightning → ONNX.

```text
ai/
  pyproject.toml   # package metadata (training-only deps)
  src/             # vmaf-train CLI + model defs + dataset loaders
  tests/           # pytest unit tests
  configs/         # dataset manifests + training recipes
  lpips_export.py  # re-export richzhang/PerceptualSimilarity → ONNX
```

## Ground rules

- **Parent rules** apply: see [../AGENTS.md](../AGENTS.md).
- **Boundary = `.onnx` + sidecar JSON on disk.** Training lives
  here, runtime lives in `core/src/dnn/`; two communicate only
  through files in `model/tiny/`. No imports cross this boundary.
- **Every shipped `.onnx` has registry entry** in
  [`../model/tiny/registry.json`](../model/tiny/) with sha256,
  upstream source, license, and opset. See
  [ADR-0039](../docs/adr/0039-onnx-runtime-op-walk-registry.md).
- **ONNX opset**: export requests opset 17 but torch dynamo may
  emit 18 (downconvert sometimes fails in
  `onnx.version_converter`). Record emitted opset in registry
  sidecar rather than failing export.
- **ImageNet normalisation lives in graph**, not in C helper. For
  any ImageNet-family model, absorb inverse ImageNet transform into
  exported graph so C side uses shared
  `vmaf_tensor_from_rgb_imagenet()` helper unchanged. See
  [ADR-0041](../docs/adr/0041-lpips-sq-extractor.md).
- **Roundtrip-validate** every export against `onnxruntime` to
  atol=1e-5 before committing. See
  [ADR-0021](../docs/adr/0021-training-stack-pytorch-lightning.md).
- **Docs**: every new model or training recipe ships page under
  `docs/ai/` in same PR. See
  [ADR-0042](../docs/adr/0042-tinyai-docs-required-per-pr.md).
- **Package `__init__.py` carries `__all__`** — every fork-added
  Python package under `ai/` (top-level `ai/`, `ai/data/`,
  `ai/train/`, `ai/src/vmaf_train/`, `ai/src/vmaf_train/data/`, ...)
  declares `__all__` as machine-readable public-surface contract,
  plus module docstring enumerating sub-modules, plus Lusoris + SPDX
  header. For namespace packages, `__all__` lists sub-module names;
  for re-export packages it lists re-exported symbols; for
  `__version__`-only packages it's `["__version__"]`. See
  [ADR-0911](../docs/adr/0911-init-py-export-completeness-audit.md).
- **Bisect-cache fixture is content-stable** — `ai/testdata/bisect/`
  = deterministic default for nightly `bisect-model-quality`
  workflow. Regenerate committed synthetic cache via
  `python ai/scripts/build_bisect_cache.py` with seeds
  `FEATURE_SEED=20260418` / `MODEL_SEED=20260419`. Same script can
  materialise real DMOS/MOS-aligned parquet via `--source-features` +
  optional `--target-column`; that path must preserve canonical-six
  feature order and still normalise output target column to `mos`.
  CI runs script with `--check`.

  As of ADR-0262, parquet leg of check uses logical
  `pyarrow.Table.equals` content comparison (schema + row count +
  values), tolerating writer-version-string drift in `created_by`
  parquet header. ONNX still compares byte-for-byte via
  `filecmp.cmp(shallow=False)`, so ONNX-side determinism must stay
  intact. **Do not** remove
  `model.producer_name = "vmaf-train.bisect-cache"`,
  `model.producer_version = "1"`, or
  `model.ir_version = 9` pins in `_save_linear_fr`: those three
  lines stabilise ONNX bytes across `onnx` minor versions. See
  [ADR-0262](../docs/adr/0262-bisect-cache-logical-comparison.md) +
  [ADR-0109](../docs/adr/0109-nightly-bisect-model-quality.md) +
  [Research-0001](../docs/research/0001-bisect-model-quality-cache.md).

## Governing ADRs

- [ADR-0020](../docs/adr/0020-tinyai-four-capabilities.md) — four capabilities (C1–C4).
- [ADR-0021](../docs/adr/0021-training-stack-pytorch-lightning.md) — PyTorch + Lightning training stack.
- [ADR-0023](../docs/adr/0023-tinyai-user-surfaces.md) — `vmaf-train` CLI as one of four surfaces.
- [ADR-0036](../docs/adr/0036-tinyai-wave1-scope-expansion.md) — Wave 1 scope (LPIPS, MobileSal, TransNet V2, …).
- [ADR-0039](../docs/adr/0039-onnx-runtime-op-walk-registry.md) — runtime op-allowlist + registry schema.
- [ADR-0041](../docs/adr/0041-lpips-sq-extractor.md) — LPIPS export pattern (ImageNet-in-graph).
- [ADR-0042](../docs/adr/0042-tinyai-docs-required-per-pr.md) — doc-substance rule.
- [ADR-0218](../docs/adr/0218-mobilesal-saliency-extractor.md) — MobileSal saliency extractor (T6-2a) ships smoke-only synthetic ONNX placeholder under `model/tiny/mobilesal.onnx`; C extractor binds tensors by name (`input` → `saliency_map`) so real upstream MobileSal export drops in without C changes. Saliency-weighted FR features and `tools/vmaf-roi` CTU sidecar = T6-2b follow-up — do not bundle into T6-2a surface.
- [ADR-0286](../docs/adr/0286-saliency-student-fork-trained-on-duts.md) — `saliency_student_v1` = fork-trained tiny U-Net replacing `mobilesal_placeholder_v0` as production weights for `mobilesal` extractor. Tensor-name contract (`input`, `saliency_map`) and NCHW shapes unchanged from ADR-0218, so any future weights swap (multi-dataset student, distilled u2netp, …) drops in without C changes. v1's decoder uses `ConvTranspose` for stride-2 upsampling because `Resize` was not on allowlist at v1's training time; constraint lifted by [ADR-0258](../docs/adr/0258-onnx-allowlist-resize.md). DUTS-TR images *not* committed in-tree; only trained `.onnx` + sidecar are. Trainer at [`scripts/train_saliency_student.py`](scripts/train_saliency_student.py); reproducer in [`docs/ai/models/saliency_student_v1.md`](../docs/ai/models/saliency_student_v1.md).
- [ADR-0332](../docs/adr/0332-saliency-student-v2-resize-decoder.md) — `saliency_student_v2` = Resize-decoder ablation on v1 recipe. Decoder upsampler swaps to `F.interpolate(scale=2, bilinear, align_corners=False)` + `nn.Conv2d(k=3)` (ONNX `Resize` mode=`linear`, `coordinate_transformation_mode=half_pixel` per ADR-0258); every other architectural decision held identical to v1 so ablation is single-variable. v2 ships as parallel artefact under `model/tiny/saliency_student_v2.onnx` — **v1 stays as production weights for `mobilesal` extractor** until follow-up PR validates v2 in real ROI encodes. v1 trainer (`train_saliency_student.py`) and v2 trainer (`train_saliency_student_v2.py`) MUST stay byte-identical outside `_ResizeConv` / `nn.ConvTranspose2d` swap and model-class name; any v1 recipe change diverging from v2 (or vice versa) destroys clean-ablation property. Future `saliency_student_v3` (multi-dataset / larger student per ADR-0286 backlog) = new ADR, not fork of v2.
- [ADR-0396](../docs/adr/0396-video-saliency-extension.md) — video-saliency follow-ups evaluated at encoder block granularity before model promotion. `ai/scripts/eval_saliency_per_mb.py` = measurement harness: pairs predicted/ground-truth masks by stem, reduces each mask to fixed block means, thresholds blocks, reports macro/micro IoU. Keep this script dependency-light (`numpy` only; `.npy` + PGM masks) so it stays usable in training sandboxes without image I/O stacks.
- [ADR-0109](../docs/adr/0109-nightly-bisect-model-quality.md) — nightly bisect workflow + synthetic placeholder cache.
- [ADR-0235](../docs/adr/0235-codec-aware-fr-regressor.md) — codec-aware FR regressor (`fr_regressor_v2`). `CODEC_VOCAB` in [`src/vmaf_train/codec.py`](src/vmaf_train/codec.py) is **closed and order-stable** — index of each codec = one-hot column index baked into trained ONNX. Adding codec appends to tuple, bumps `CODEC_VOCAB_VERSION`; reordering silently invalidates every shipped `fr_regressor_v2_*.onnx`. `FRRegressor(num_codecs=0)` must remain v1 single-input contract — flipping default would break every existing `model/tiny/fr_regressor_v1.onnx` consumer. Feature-dump scripts emit `codec` column tagged at call site (BVI-DVC: `"x264"`, Netflix Public: `"unknown"`); never silently default to codec that doesn't match what script encoded.
- [ADR-0305](../docs/adr/0305-encoder-knob-space-pareto-analysis.md) — **knob-sweep corpus invariant.** 12,636-cell sweep at `runs/phase_a/full_grid/comprehensive.jsonl` (gitignored, locally generated) = source of truth for `tools/vmaf-tune/codec_adapters/*` recipe defaults. Pareto frontiers stratified per `(source, codec, rc_mode)` slice — never collapsed to global hull (companion [Research-0063](../docs/research/0063-encoder-knob-space-cq-vs-vbr-stratification.md) shows global-hull failure mode regresses NVENC h264/hevc by ~4 VMAF at cq=30). **Recipes regressing vs bare encoder at matched bitrate within same slice MUST NOT ship as adapter defaults.** Regression-detection check lives in `ai/scripts/analyze_knob_sweep.py` (`detect_recipe_regressions(...)`), exercised by `ai/tests/test_knob_sweep_analysis.py::test_recipe_regression_detection`; new codec adapter PRs cite per-(codec, rc_mode) hull row from `reports/summary.md` (or "no hull entry yet — bare default") in PR description. Methodology + scaffolded findings: [Research-0077](../docs/research/0077-encoder-knob-space-pareto-frontiers.md).
- [ADR-0650](../docs/adr/0650-signal-mix-audit.md) — **signal-mix audit is advisory.** `ai/scripts/signal_mix_audit.py` reads already-extracted parquet/JSONL tables, renders coverage, redundancy, complementary-intersection, and blind-spot reports. Must remain side-effect free: no feature extraction, no checkpoint export, no corpus mutation, no CI gating by default. Adding new metric families or table columns -> update family regexes and `docs/ai/signal-mix-audit.md` together so reports keep naming missing HDR/panel, saliency/ROI, texture, NR/MOS, and codec-profile signals in human terms.
- **Feature-correlation JSON is finite-only.**
  `ai/scripts/feature_correlation.py` treats `NaN` and both infinities as
  incomplete feature/target rows, rejects non-finite
  `--redundancy-threshold` values during argument parsing, and validates the
  complete report with `allow_nan=False` before its atomic manifest write.
  Preserve the missing-scikit-learn empty-map contract and the repeated
  constant-column check after complete-case filtering; neither `NaN` nor
  `Infinity` is an RFC-8259 JSON value.
- [ADR-0661](../docs/adr/0661-ai-run-manifest-provenance.md) — **AI sidecars carry shared run provenance.** Use `aiutils.run_manifest.write_run_manifest()` for new standalone training/export/fetch/extraction sidecars instead of inventing per-script argument/path JSON; use `build_run_provenance()` only when embedding provenance block into existing stable report schema. CHUG-facing MOS runs record `train_chug_hdr_mos_head.py` as user entrypoint and `train_konvid_mos_head.py` as `shared_trainer` because wrapper delegates into shared loop. Matching Claude workflow = `.claude/skills/ai-run-manifest/SKILL.md`.
- [ADR-0680](../docs/adr/0680-ai-cli-helper-pattern.md) — **AI batch CLIs share parser boilerplate.** Use `aiutils.cli_helpers.make_argument_parser()` and `collect_cli_argv()` for new operator-facing scripts. Batch manifest runners must use `add_batch_manifest_arguments()` so manifest/report/fail-fast flags stay consistent while table-specific parser fields remain local to each runner.
- [ADR-0681](../docs/adr/0681-ai-script-bootstrap-helper.md) — **Direct AI scripts share import bootstrap.** Use `ai/scripts/_script_bootstrap.py::bootstrap_ai_script(__file__)` before importing `aiutils`, sibling materializers, or optional `vmaf-tune` helpers from directly executable `ai/scripts/*.py` file. Do not add fresh ad hoc `sys.path.insert(...)` blocks unless helper lacks required import root and same PR extends its tests/docs.
- [ADR-0668](../docs/adr/0668-ai-derived-table-provenance.md) — **Derived feature tables need replay manifests.** `extract_k150k_features.py`, `combine_full_feature_parquets.py`, `enrich_k150k_parquet_metadata.py`, `konvid_to_full_features.py`, and `bvi_dvc_to_full_features.py` write `<out>.manifest.json` by default using `aiutils.run_manifest`. Do not add new operator-facing refreshed parquet/table builders leaving no input/output/argv sidecar; anonymous local parquet files aren't acceptable training evidence.
- [ADR-0669](../docs/adr/0669-ai-corpus-jsonl-provenance.md) — **Corpus JSONL merge outputs need replay manifests.** `aggregate_corpora.py` and `merge_corpora.py` write `<output>.manifest.json` by default with shared `run_provenance`, counters, and schema/dedup policy. Keep JSONL row schemas stable, put run-level evidence in sidecar.
- [ADR-0670](../docs/adr/0670-ai-legacy-corpus-extraction-manifests.md) — **Legacy trainer-input builders need replay manifests.** `extract_full_features.py`, `konvid_to_vmaf_pairs.py`, and `bvi_dvc_to_corpus_jsonl.py` write sibling manifest sidecars by default. Do not add or revive corpus/extraction scripts creating local trainer-input parquet/JSONL without row/clip counters, input/output path evidence, and shared `run_provenance`. BVI-DVC JSONL rows must stay current with `vmaftune.CORPUS_ROW_KEYS`; missing HDR/shot/encoder-stat signals = explicit unavailable defaults, not omitted columns.
- [ADR-0676](../docs/adr/0676-mos-corpus-adapter-manifests.md) — **MOS corpus source adapters need replay manifests.** `chug_to_corpus_jsonl.py`, `konvid_1k_to_corpus_jsonl.py`, `konvid_150k_to_corpus_jsonl.py`, `youtube_ugc_to_corpus_jsonl.py`, `lsvq_to_corpus_jsonl.py`, `live_vqc_to_corpus_jsonl.py`, and `waterloo_ivc_to_corpus_jsonl.py` write `<output>.manifest.json` by default. Keep new MOS adapter CLIs on same sidecar contract so model cards can cite source-root, row-cap, attrition, and `run_provenance` evidence before aggregation.
- [ADR-0677](../docs/adr/0677-ai-dataset-fetch-manifests.md) — **Dataset fetch helpers need replay manifests before conversion.** `fetch_konvid_1k.py` writes `<root>/fetch_manifest.json` by default, and `fetch_youtube_ugc_subset.py` keeps its content `--manifest` while adding `<manifest>.run-manifest.json`. Keep future downloader/fetcher CLIs on same pattern so local corpus roots can prove URL, selection, archive, and output evidence before JSONL/parquet builders consume them.
- [ADR-0665](../docs/adr/0665-fast-nr-calibration-quality-guard.md) — **Fast-NR calibration sidecars are quality-gated.** `ai/scripts/calibrate_nr_threshold.py` must not write tune-facing `calibration_threshold` values unless fitted sample count and NR-vs-FR PLCC pass script's gates. Operator explicitly uses `--allow-weak-calibration` for diagnostic sidecars otherwise. Weak real-corpus fits = model/training backlog, not reason to loosen `vmaf-tune` thresholds.
- [ADR-0657](../docs/adr/0657-second-opinion-feature-materializer.md) — **second-opinion materialisation stays table-side.** `ai/scripts/materialize_second_opinion_features.py` joins already-generated NR/MOS scorer JSON onto feature tables; must not invoke, vendor, or link third-party VQA competitors. Keep output columns namespaced as `second_opinion_<scorer>_*`, preserve row key by default, treat duplicate scorer/key rows as data poisoning rather than averaging them silently.
- [ADR-0674](../docs/adr/0674-second-opinion-materializer-batch-manifest.md) — **second-opinion batch manifests only orchestrate joins.** `ai/scripts/batch_materialize_second_opinion_features.py` may select multiple feature tables and score sidecars. Must call shared table-side materializer; must not invoke external scorer binaries or duplicate key/status/column logic.
- [ADR-0663](../docs/adr/0663-mos-label-materializer.md) — **MOS labels are explicit real-training inputs.** `ai/scripts/materialize_mos_labels.py` joins subjective MOS labels onto already-extracted feature tables, stays table-side: no feature extraction, corpus downloading, or training. `train_konvid_mos_head.py` must not silently synthesize data when explicit real-corpus paths produce zero labelled rows; use `--smoke` for synthetic CI/load-path checks and reject low-coverage real joins unless operator deliberately lowers materializer threshold.
- [ADR-0675](../docs/adr/0675-mos-label-materializer-batch-manifest.md) — **MOS-label batch manifests only orchestrate joins.** `ai/scripts/batch_materialize_mos_labels.py` may select multiple feature tables and label sources. Must call shared table-side materializer; must not duplicate MOS parsing, key-normalisation, match-rate, or overwrite logic.
- [ADR-0672](../docs/adr/0672-saliency-materializer-temporal-controls.md) — **saliency materializer rows must be attributable.** `ai/scripts/materialize_saliency_features.py` exposes same temporal reducers as `vmaf-tune` (`mean`, `ema`, `max`, `motion-weighted`), records `saliency_model_id`, `saliency_aggregator`, and `saliency_ema_alpha` for newly materialized rows. Do not overwrite or invent provenance metadata for skipped pre-existing saliency columns; use `--overwrite` when intentionally replacing them.
- [ADR-0963](../docs/adr/0963-ai-nan-propagation-guards-round25.md) — **NaN propagation guards in `eval.correlations` and `tune._read_best_metric`.** `correlations()` raises `ValueError` on empty inputs, returns `plcc=0.0, srocc=0.0` (with `RuntimeWarning`) for constant-valued inputs. 0.0 sentinel is intentional: gate logic uses `>=` and NaN would silently fail every comparison. `_read_best_metric()` returns `float("inf")` when all metric rows are NaN (diverged training run), preventing Optuna study corruption. Do not change these sentinels without updating downstream `_gate` comparison semantics in `bisect_model_quality.py`.
- [ADR-0993](../docs/adr/0993-konvid-ugc-bvi-saliency-batch-launch.md) — **KoNViD / UGC / BVI-DVC saliency batch manifests.** In-tree manifests live under `ai/batch-manifests/saliency/`. KoNViD-150K manifest (`konvid-150k.json`) fully wired to `konvid_150k.jsonl` with `path_column=src`, `root=.corpus/konvid-150k/k150ka_extracted/`. UGC (`ugc.json`) and BVI-DVC (`bvi-dvc.json`) = scaffolded stubs with `tables: []` until path-enriched corpus JSONL generated for each (see `_status` / `_resolution` comments in each manifest). Never populate UGC tables using `source` identifier column from full-feature parquet — contains corpus IDs, not file paths. Never populate BVI-DVC tables using `key` column — contains encode parameters, not file paths.
- [ADR-1097](../docs/adr/1097-ai-script-atomic-writes.md) — **Cache and output file writes must be atomic.** All per-clip cache JSON writes in `ai/scripts/` and all final Parquet / JSONL output writes must go through `aiutils.file_utils.write_text_atomic` (for JSON/text) or `aiutils.parquet_utils.write_parquet_atomic` (for Parquet). Both helpers write to sibling temp file, rename atomically so crash mid-write never leaves partially-truncated file poisoning subsequent resume logic. Do **not** use `Path.write_text(...)` or bare `df.to_parquet(dest)` for any file a resume loop tests with `path.is_file()`. `write_manifest_json` in `aiutils.run_manifest` also made atomic (transparent to callers). `extract_k150k_features._write_parquet_from_rows` uses same pattern as established precedent.
- [ADR-1173](../docs/adr/1173-ai-teacher-follows-default-model.md) — **AI teacher model follows default model single source.** AI training and extraction scripts resolve teacher model through `ai.data.scores.resolve_teacher_model()` (imports `DEFAULT_MODEL` from `vmaftune.defaultmodel`), falling back to `$VMAF_MODEL_PATH` then `DEFAULT_MODEL`. Feature producers stamp `teacher_model` on every row and manifest; combiners and trainers refuse mixed-teacher tables without `--assume-teacher`; raw feature extraction tables append `adm3` to `FULL_FEATURES` and K150K `FEATURE_NAMES` while canonical-6 student features remain frozen.

## Netflix-corpus training prep (ADR-0242 / ADR-0203)

Top-level [`ai/data/`](data/) and [`ai/train/`](train/) packages
(distinct from `vmaf_train` package under `src/`) host runnable
Netflix-corpus prep stack:

- [`ai/data/netflix_loader.py`](data/netflix_loader.py) — pair distorted
  YUVs with their ref by parsing Netflix ladder filename
  convention. `iter_pairs(data_root, *, sources=, max_pairs=,
  assume_dims=)` = only public surface.
- [`ai/data/feature_extractor.py`](data/feature_extractor.py) — wraps
  libvmaf CLI in JSON mode. Defaults to
  `core/build-cpu/tools/vmaf`; honours `$VMAF_BIN`. Raises
  `RuntimeError` with explicit build instructions on missing binary.
- [`ai/data/scores.py`](data/scores.py) — `vmaf_v0.6.1` distillation
  scores (per-frame + pooled). Honours `$VMAF_MODEL_PATH`.
- [`ai/train/dataset.py`](train/dataset.py) — `NetflixFrameDataset`
  with explicit `payload_provider=` + `assume_dims=` injection points
  for unit tests.
- [`ai/train/eval.py`](train/eval.py) — PLCC / SROCC / KROCC / RMSE +
  latency. Either `onnx_path=` or `predictions=` (exactly one).
- [`ai/train/train.py`](train/train.py) — CLI entry point. Runs
  standalone (`python ai/train/train.py …`) or as module
  (`python -m ai.train.train`); both forms work because script
  fixes `sys.path` when `__package__` is empty.

**Rebase-sensitive invariants** (track when upstream Netflix/vmaf adds
its own training surface):

- **`ai/tests/conftest.py::requires_pytorch_lightning()` = canonical
  guard for tests importing `vmaf_train.models` (transitively
  `pytorch_lightning` → `torchmetrics` → `torchvision`).** Plain
  `pytest.importorskip("pytorch_lightning")` NOT sufficient — it
  only catches `ImportError`, while torchvision/torch ABI mismatches
  raise `RuntimeError("operator torchvision::nms does not exist")` at
  module load. New tests pulling lightning must call
  `requires_pytorch_lightning()` at module level (or use
  `_PYTORCH_LIGHTNING_ERROR` constant with `pytest.mark.skipif` for
  per-function gating). Guard is intentionally broad
  (`except Exception`) so future torch/torchvision/torchmetrics drift
  also routes to clean skip with actual error string. Behavior
  contract pinned by `ai/tests/test_conftest_pytorch_lightning_guard.py`.

- **`ai/pyproject.toml` `pythonpath = ["scripts"]` required for batch
  materializer tests.** Tests loading `ai/scripts/batch_materialize_*.py`
  via `importlib.spec_from_file_location` trigger `_script_bootstrap` at
  module-load time. Without `ai/scripts/` on `sys.path`, those tests fail
  with `ModuleNotFoundError: No module named '_script_bootstrap'` when
  invoked from repo root. `pythonpath = ["scripts"]` entry in
  `[tool.pytest.ini_options]` = canonical fix (ADR-0991). Do not
  remove it and do not substitute `PYTHONPATH=ai/scripts` prefix in CI
  step definitions — pyproject config = single source of truth.

- **`importlib.util` callers must pre-register module in `sys.modules`.**
  Python 3.14 introduced regression in `dataclasses._is_type()`
  (CPython gh-129861): it calls `sys.modules.get(cls.__module__).__dict__`
  which raises `AttributeError: 'NoneType' …` when module not yet
  in `sys.modules` at `exec_module()` time. Any caller loading
  `ai/scripts/` module via `importlib.util.spec_from_file_location` +
  `module_from_spec()` + `exec_module()` **must** insert
  `sys.modules[spec.name] = module` between `module_from_spec()` and
  `exec_module()`. Invariant applies to all such loaders in test
  suite and any automation harness. `_script_bootstrap.py` itself avoids
  crash by not using `from __future__ import annotations` (which delays
  annotation evaluation, can trigger bug in dataclass field
  resolution).

- `iter_pairs` filename regex = fork-specific. If upstream adds
  loader with different ladder convention, do NOT merge them — keep
  ours under `ai/data/` and theirs under whatever path they pick.
- Per-clip JSON cache schema (`{features:{feature_names,
  per_frame, n_frames}, scores:{per_frame, pooled}}`) consumed by
  both dataset and any downstream consumer. Bumping schema
  must invalidate `$VMAF_TINY_AI_CACHE` (or version-tag path).
- Smoke command `python ai/train/train.py --epochs 0
  --assume-dims 16x16` MUST stay runnable without a built `vmaf`
  binary — `_make_zero_payload` helper in `ai.train.dataset`
  injects fake payload so CI gates don't drag libvmaf build into
  Python test surface.
- **`vmaf_tiny_v2` ONNX contract (ADR-0244).** Shipped ONNX
  embeds StandardScaler `(mean, std)` as Constant `Sub` + `Div`
  nodes running before MLP. Runtime feeds raw canonical-6
  feature values; do NOT add external scaler step. Re-exporting
  via [`ai/scripts/export_vmaf_tiny_v2.py`](scripts/export_vmaf_tiny_v2.py)
  = only supported path — pulls `mean` / `std` from
  trainer checkpoint, bakes them as graph initialisers, so
  `model/tiny/registry.json` sha256 covers calibration values
  too. Input name = `features` ([N, 6] float32), output `vmaf`
  ([N] float32); feature column order fixed at
  `(adm2, vif_scale0, vif_scale1, vif_scale2, vif_scale3, motion2)`
  and must not be reordered without full Phase-3 re-validation.
- **`vmaf_tiny_v3` ships alongside v2 (ADR-0241).** Same ONNX
  contract as v2 (input `features [N, 6]` float32, output
  `vmaf [N]` float32, opset 17, scaler-baked-into-graph) — only
  architecture differs (`mlp_medium` 6 → 32 → 16 → 1, 769 params vs
  v2's `mlp_small` 257). **Production default stays v2**;
  [`docs/ai/inference.md`](../docs/ai/inference.md) and model-card
  cross-references both keep v2 as recommended `--tiny-model`.
  v3 = higher-PLCC / lower-variance option (Netflix LOSO mean
  PLCC 0.9986 ± 0.0015 vs v2's 0.9978 ± 0.0021). Do NOT replace v2
  with v3 wholesale. Both file paths referenced by name in
  user-facing docs and registry; small mean delta does not justify
  default flip without multi-seed + KoNViD 5-fold parity (documented
  as Phase-3e follow-up). Same scripts pattern:
  `train_vmaf_tiny_v3.py` / `export_vmaf_tiny_v3.py` /
  `validate_vmaf_tiny_v3.py` / `eval_loso_vmaf_tiny_v3.py` —
  do **not** modify v2 scripts when iterating on v3.
- **`vmaf_tiny_v3` and `vmaf_tiny_v4` opt-in tiers
  (ADR-0241 / ADR-0242).** v3 (`mlp_medium`, 769 params, ADR-0241)
  and v4 (`mlp_large`, 3 073 params, ADR-0242) ship *alongside* v2,
  not as replacements. Production default stays `vmaf_tiny_v2`. Three
  rungs share canonical-6 input contract, bundled
  StandardScaler, and 90 ep / Adam@1e-3 / MSE / bs=256 recipe;
  only architecture differs. **Do NOT modify v2 or v3 scripts
  when iterating on later rungs** — each version owns its own
  `train_vmaf_tiny_vN.py` / `export_vmaf_tiny_vN.py` /
  `validate_vmaf_tiny_vN.py` / `eval_loso_vmaf_tiny_vN.py` quartet.
  Arch ladder **stops at v4**: v3 → v4 LOSO PLCC delta =
  +0.0001 (well below 1 std), demonstrating saturation on
  canonical-6 + 4-corpus regime. Future quality gains require
  regime change (richer features, larger corpus, ensembles), not
  deeper MLPs. See ADR-0242 § Alternatives considered for
  mlp_huge rejection rationale.
- **Hardware-capability priors are prior-only (ADR-0335).**
  [`ai/data/hardware_caps.csv`](data/hardware_caps.csv) +
  [`ai/scripts/hardware_caps_loader.py`](scripts/hardware_caps_loader.py)
  ship per-architecture GPU encode-block fingerprints (codecs
  supported, max resolution, encoding-block count, tensor /
  NPU flags, driver floor) sourced exclusively from primary
  vendor docs. Loader's schema rejects benchmark-shaped
  columns (`fps_*`, `throughput`, `mbps`, `latency`, `watts`,
  `tdp`, `score_*`, `vmaf_*`), community-wiki source URLs
  (`wikipedia.org`, `wikichip.org`), empty fields, and zero
  encoding-block rows. Adding throughput / quality numbers to this
  surface is forbidden by ADR-0335 and companion research digest's
  category-1 NO-GO finding. Performance signal must come from
  corpus's own measured rows, not from static prior table. Schema
  extensions (new capability columns) require new ADR, not silent
  column bump.

## `fr_regressor_v1` (C1 baseline — ADR-0249)

Wave-1 C1 baseline trainer =
[`ai/scripts/train_fr_regressor.py`](scripts/train_fr_regressor.py). It
consumes `runs/full_features_netflix.parquet` (produced by
`ai/scripts/extract_full_features.py` over local Netflix Public
drop at `.corpus/netflix/`), runs 9-fold leave-one-source-out
(LOSO), exports `model/tiny/fr_regressor_v1.onnx` only when mean
LOSO PLCC ≥ 0.95 against `vmaf_v0.6.1` per-frame teacher.

**Contract row** (do not regress without ADR amendment):

- **Input** — `[N, 6]` float32, feature order
  `(adm2, vif_scale0, vif_scale1, vif_scale2, vif_scale3, motion2)`,
  standardised with per-feature `feature_mean` / `feature_std`
  vectors pinned in sidecar JSON. Standardisation is **not**
  baked into ONNX so callers can swap feature pools without
  re-export.
- **Output** — `[N]` float32, VMAF-scale (0–100 typical).
- **Architecture** — stock `vmaf_train.models.FRRegressor` with
  Wave-1 spec hparams (hidden=64, depth=2, dropout=0.1, GELU). Larger
  / smaller variants must register new model id, not overwrite this
  one.
- **Ship gate** — mean LOSO PLCC ≥ 0.95 vs `vmaf_v0.6.1`. Trainer
  exits 3, refuses to overwrite registry on failure; lowering
  threshold = soft-fail of policy, not code change.

**Rebase-sensitive invariants:**

- Canonical-6 feature order is load-bearing — `vmaf_v0.6.1`
  consumes same six features in same order, and ONNX
  graph weight matrix column-aligned to it. Reordering
  sidecar `feature_order` field invalidates checkpoint.
- Refresh PRs must point `--parquet` at current dated Netflix
  full-feature table (for example
  `runs/full_features_netflix_refresh_20260520.parquet`), not
  stale historical `runs/full_features_netflix.parquet`, and must
  update model card with new LOSO fold metrics. If
  `torch.onnx.export` leaves orphan `<model>.onnx.data` while
  saved ONNX has no external initializers, restore orphan file and
  commit only inline ONNX plus sidecar/registry changes.
- Netflix Public Dataset is non-redistributable. CI cannot retrain
  end-to-end; only smoke path
  (`python ai/scripts/train_fr_regressor.py --epochs 3 --no-export`)
  runs in CI when parquet is locally available.

## Saliency feature materialization (ADR-0655)

`ai/scripts/materialize_saliency_features.py` = reusable bridge from
row-oriented corpus tables to saliency-bearing training rows. Keep saliency
inference out of trainer hot loops: trainers consume `saliency_mean` /
`saliency_var` columns and report missing coverage, while this script owns
bounded FFmpeg decode, ffprobe fallback, model invocation, and
`saliency_status` audit column. Do not add corpus-specific one-off saliency
materializers unless future ADR explains why shared table utility cannot
represent corpus.
`ai/scripts/batch_materialize_saliency_features.py` is only orchestration over
shared materializer: batch manifests may select tables, roots, model ids,
and temporal reducers, but must not duplicate row decoding, saliency inference,
or status semantics.

## Dynamic-PTQ tiny-MLP family (ADR-0275)

`vmaf_tiny_v3` and `vmaf_tiny_v4` carry dynamic-PTQ int8 sidecars
produced by `ai/scripts/ptq_dynamic.py`. Recipe is identical to
`learned_filter_v1` (ADR-0174) and `nr_metric_v1` (ADR-0248): single
CLI invocation, no calibration data. On-disk size win is
proportional to weight mass — `mlp_large` (v4) shrinks 45 %,
`mlp_medium` (v3) shrinks 5 % because Constant scaler nodes and op
metadata dominate that graph. v2 (`mlp_small`) stays fp32: too
little weight mass for int8 sidecar to be worth audit cost.

**Invariants:**

- fp32 `<basename>.onnx` stays on disk as regression
  baseline; runtime redirect from ADR-0174 picks
  `.int8.onnx` sibling only when registry overlay declares
  `quant_mode != "fp32"`.
- `python ai/scripts/measure_quant_drop.py --all` = gate. Both
  v3 and v4 sit two orders of magnitude under 0.01 PLCC
  budget; treat any future drop > 1e-3 as regression worth
  investigating before merging int8 refresh.
- Re-running `ptq_dynamic.py` is deterministic on fixed fp32
  input — but sha256 of int8 output can shift across ORT
  versions. When ORT bumped, regenerate both sidecars and
  refresh `int8_sha256` in `model/tiny/registry.json` +
  `vmaf_tiny_v{3,4}.json` in same PR.

## Frame loader pixel formats

`ai/src/vmaf_train/data/frame_loader.py` = direct ffmpeg frame
ingest seam for C2/C3 training. Accepts `gray` as `HxW` arrays and
packed `rgb24` / `bgr24` / `rgba` / `bgra` as `HxWxC` arrays. Do not
silently accept planar or subsampled formats such as `yuv420p` in this
loader; those need explicit plane semantics before they're safe to
feed into training tensors.

## `fr_regressor_v2_ensemble_v1` — probabilistic head (ADR-0279)

Probabilistic successor to codec-aware
`fr_regressor_v2` = deep ensemble of N=5 v2 members trained under
distinct seeds, packaged as 5 ONNX files plus manifest sidecar
`model/tiny/fr_regressor_v2_ensemble_v1.json`. Trainer:
[`ai/scripts/train_fr_regressor_v2_ensemble.py`](scripts/train_fr_regressor_v2_ensemble.py);
evaluator: [`ai/scripts/eval_probabilistic_proxy.py`](scripts/eval_probabilistic_proxy.py);
model card:
[`docs/ai/models/fr_regressor_v2_probabilistic.md`](../docs/ai/models/fr_regressor_v2_probabilistic.md).

**Rebase-sensitive invariants:**

- **Per-member ONNX I/O contract = v2 two-input shape**: inputs
  `features [N, 6]` (canonical-6, StandardScaler-normalised by
  manifest's `feature_mean` / `feature_std`) + `codec_onehot
  [N, NUM_CODECS]`; output `score [N]` float32. Each member = stock
  `FRRegressor(num_codecs=NUM_CODECS)` — flipping that to
  single-input v1-shaped graph silently invalidates every shipped
  ensemble.
- **Manifest layout = runtime entry point**, not registry
  rows. Each member also added to `model/tiny/registry.json` as
  `kind: "fr"` (with id `<ensemble_id>_seed<N>`), so existing
  tiny-model verifier can SHA-256-check each member without a
  schema bump. Manifest sidecar's `members[]` list = canonical
  ordered set C-side adapter iterates over. Adding a schema-version
  field to `registry.schema.json` for `fr_ensemble` kind = future
  option; until then, ensemble lookups go through manifest, not
  registry.
- **Ensemble size is part of contract.** Manifest's
  `ensemble_size` field pins N; C-side adapter must open
  `ensemble_size` sessions. Changing N requires new ensemble id +
  manifest, not in-place mutation.
- **Confidence rule = one-of**: `confidence.method` either
  `"ensemble"` (use `gaussian_z` as multiplier on `sigma`) or
  `"ensemble+conformal"` (use `conformal_q_residual` instead).
  Trainer emits conformal scalar only when
  `--conformal-calibration-frac > 0` and calibration split is
  large enough; otherwise field stays `null`, Gaussian rule
  applies.
- **`CODEC_VOCAB` parity with v2 required.** Manifest pins
  `codec_vocab` + `codec_vocab_version`; runtime must refuse to
  load when these disagree with live `ai/src/vmaf_train/codec.py`
  vocabulary. Bumping vocabulary requires retraining ensemble;
  existing closed-vocabulary invariant from ADR-0235 carries over
  verbatim.
- **Historical smoke artefacts are retired.** ADR-0303 originally
  shipped synthetic 100-row / 1-epoch ensemble members as load-path
  probes. ADR-0321 replaced five seed ONNX files with
  full-corpus-trained production weights, added per-seed sidecars.
  Do not reintroduce `smoke: true` for
  `fr_regressor_v2_ensemble_v1_seed{0..4}` unless future ADR
  explicitly rolls production flip back.
- **Ensemble registry invariant (ADR-0303)**: each ensemble member's
  `smoke: true` registry row flips to `false` **only after** that
  individual seed clears `PLCC_i ≥ 0.95` LOSO ship gate
  (ADR-0235 / ADR-0291). Ensemble-mean entry — if/when one is
  added to `model/tiny/registry.json` as `fr_ensemble`-kind row —
  flips **only after all five seeds clear** per-seed gate *and*
  variance bound `max_i(PLCC_i) - min_i(PLCC_i) ≤ 0.005` holds.
  Decision lives in [`scripts/ci/ensemble_prod_gate.py`](../scripts/ci/ensemble_prod_gate.py);
  trainer emitting per-seed `loso_seed{N}.json` artefacts gate
  consumes =
  [`ai/scripts/train_fr_regressor_v2_ensemble_loso.py`](scripts/train_fr_regressor_v2_ensemble_loso.py).
  Do NOT flip individual seed rows by hand without running gate
  against real-corpus LOSO output. Variance bound protects
  predictive-distribution semantics; flipping seeds ad-hoc would
  silently bake in unbounded across-seed spread.
- **Registry-flip happened in ADR-0320**: five
  `fr_regressor_v2_ensemble_v1_seed{0..4}` rows in
  `model/tiny/registry.json` flipped `smoke: true → false` on
  2026-05-06 against passing
  `runs/ensemble_v2_real/PROMOTE.json` (mean PLCC = 0.9973,
  spread = 9.5e-4, both gate components green) produced by
  [`ai/scripts/validate_ensemble_seeds.py`](scripts/validate_ensemble_seeds.py).
  Verdict file committed at
  [`model/tiny/fr_regressor_v2_ensemble_v1_seed_flip_PROMOTE.json`](../model/tiny/fr_regressor_v2_ensemble_v1_seed_flip_PROMOTE.json)
  as immutable audit trail. **Going-forward invariant**: any
  future registry change for these `ensemble_v1` seed rows (sha256
  bump after retraining, smoke-flag mutation, ONNX path change)
  requires fresh `PROMOTE.json` verdict with mean PLCC ≥ 0.95 AND
  spread ≤ 0.005. Same two-part gate ADR-0303 defined, ADR-0320
  honoured. **Never** flip or mutate these rows during a
  `/sync-upstream` rebase or as side-effect of any other PR — the
  harness in
  [`ai/scripts/run_ensemble_v2_real_corpus_loso.sh`](scripts/run_ensemble_v2_real_corpus_loso.sh)
  and validator emit verdict file but **do not** mutate
  registry. Auto-flipping on PROMOTE was rejected in ADR-0309's
  alternatives matrix specifically because rebase-time mutation of
  shipped registry rows = foot-gun this invariant exists to
  prevent.
- **Ensemble production-flip now done (ADR-0321)**: as of
  2026-05-06 five `fr_regressor_v2_ensemble_v1_seed{0..4}` rows
  carry `smoke: false` and point at LOSO-gated, full-corpus-trained
  ONNX weights produced by
  [`ai/scripts/export_ensemble_v2_seeds.py`](scripts/export_ensemble_v2_seeds.py).
  Each row has sidecar
  `model/tiny/fr_regressor_v2_ensemble_v1_seed{N}.json` that mirrors
  canonical `fr_regressor_v2.json` shape (encoder vocab v2, codec
  block layout, scaler params) plus seed-specific gate evidence from
  `runs/ensemble_v2_real/PROMOTE.json`. **Going-forward rule**: any
  future flip (re-train + re-export) requires fresh
  `PROMOTE.json` from LOSO trainer and re-run of
  `export_ensemble_v2_seeds.py`. Both ONNX bytes and
  per-seed sidecars must regenerate together so
  `test_registry.sh` sha256 + sidecar-presence check stays green.
  Editing one without other is foot-gun: registry test
  catches sha256 drift, but stale sidecar's gate-evidence block
  would silently lie about provenance.
- **Canonical-6 JSONL schema is load-bearing (ADR-0319)**: LOSO
  trainer's `_load_corpus` accepts schema emitted by
  [`scripts/dev/hw_encoder_corpus.py`](../scripts/dev/hw_encoder_corpus.py)
  bit-for-bit — required keys per row =
  `(src, encoder, cq, frame_index, vmaf, adm2, vif_scale0..3, motion2)`.
  Codec block materialised as 12-slot `ENCODER_VOCAB` v2
  one-hot (mirrors `train_fr_regressor_v2.py`) + constant
  `preset_norm = 0.5` (corpus doesn't record preset) +
  `crf_norm` = `(cq - cq_min) / (cq_max - cq_min)`. Schema changes
  — column rename, encoder-vocab reorder, new required field —
  require an `ENCODER_VOCAB_VERSION` bump and full ensemble retrain
  per existing closed-vocabulary invariant. Fold-level
  StandardScaler fit on training rows only (mirrors
  `eval_loso_vmaf_tiny_v3.py`); leaking held-out source's
  distribution into scaler would silently inflate per-fold
  PLCC. See ADR-0319 §Decision and `_load_corpus`'s docstring.

## Quantization-Aware Training (ADR-0207 / ADR-0208)

QAT trainer hook lives in [`ai/train/qat.py`](train/qat.py); CLI
driver in [`ai/scripts/qat_train.py`](scripts/qat_train.py). Default
config example =
[`ai/configs/learned_filter_v1_qat.yaml`](configs/learned_filter_v1_qat.yaml).

**Pipeline (per ADR-0207 + ADR-0208 implementation bridge):**

1. fp32 warm-start training.
2. Graph capture with `torch.export` + fake-quant insertion via
   `torchao.quantization.pt2e.prepare_qat_pt2e` under
   `X86InductorQuantizer`'s default recipe: per-tensor `uint8`
   activation, per-channel symmetric `int8` weight (ADR-1293).
3. QAT fine-tune at 10× reduced LR.
4. Copy QAT-conditioned weights into fresh fp32 module, export
   to ONNX (torch.export-based exporter; the target is plain fp32,
   so the legacy path is not needed), then ORT static-quantize with
   calibration set drawn from QAT distribution. Output = a
   QDQ `.int8.onnx`.

**Rebase-sensitive invariants:**

- Two-step pipeline (PyTorch QAT → fp32 ONNX → ORT
  static-quantize) is load-bearing. Do NOT collapse to
  `convert_fx → torch.onnx.export` — both PyTorch 2.11 ONNX
  exporters refuse `convert_fx` output (legacy emits
  `quantized::conv2d`; TorchDynamo trips on
  `Conv2dPackedParamsBase.__obj_flatten__`). Re-check on each
  PyTorch upgrade.
- State-dict transfer in `_copy_qat_weights_into_fp32` matches
  by submodule name + tensor shape. `torch.export` capture keeps
  the original parameter names (measured: 20/20 tensors transfer
  on `LearnedFilter`), but models using top-level `nn.Sequential`
  still break this; the `RuntimeError("0 tensors copied")` guard
  catches it.
- An exported graph module rejects `.train()` / `.eval()` and
  needs torchao's `move_exported_model_to_train` / `_to_eval`.
  `_set_mode()` dispatches on `isinstance(module,
  torch.fx.GraphModule)` because `_qat_fine_tune` runs against
  both the raw Lightning module and the prepared graph. Do not
  reintroduce a bare `.eval()` on the QAT model.
- Graph capture runs on CPU (`torch.export` is flaky on CUDA
  buffers here); trainer migrates to CPU before
  `prepare_qat_pt2e` and back to accelerator afterwards.
- `torch.ao.quantization` is deprecated wholesale and raises a
  `DeprecationWarning` the `filterwarnings = ["error"]` setting
  turns into a test failure. The QAT hook moved to
  `torchao.quantization.pt2e` in ADR-1293; do not restore
  `prepare_qat_fx` or `get_default_qat_qconfig_mapping` on a
  rebase. `torch.export.export_for_training` does not exist in
  torch 2.14 — use `torch.export.export(...).module()`.
- The pt2e recipe keeps the weight side byte-identical
  (`int8`, `per_channel_symmetric`, `ch_axis=0`, [-128, 127])
  and widens the activation range from the old mapping's
  reduce-range [0, 127] to [0, 255]. That matches ORT
  `quantize_static`, which bakes the activation ranges that
  actually ship; the narrower range was the mismatch.

## Local workflow

```bash
pip install -e ai/
vmaf-train --help
vmaf-train register model/tiny/lpips_sq.onnx   # adds to registry.json
python ai/lpips_export.py                      # re-export LPIPS from the reference repo

# Netflix-corpus training (ADR-0203):
bash ai/scripts/run_training.sh
```

## fr_regressor_v2 — codec block layout (ADR-0272)

`ai/scripts/train_fr_regressor_v2.py` consumes vmaf-tune Phase A
JSONL corpus, emits `model/tiny/fr_regressor_v2.onnx`. Codec
block layout is **load-bearing** — bumping it requires re-train.
Pinned invariants:

- `ENCODER_VOCAB = ("libx264", "libx265", "libsvtav1", "libvvenc",
  "libvpx-vp9", "unknown")`. Order matches encoder-onehot index
  baked into trained ONNX. Append-only; bump
  `ENCODER_VOCAB_VERSION` and re-train when adding new entry.
- 8-D codec block layout: `[encoder_onehot[0..5], preset_norm,
  crf_norm]`. Both `preset_norm` and `crf_norm` live in `[0, 1]`.
- `crf_norm = crf / 63.0` — `63` = union upper bound across
  supported encoders (libsvtav1 / libvpx-vp9 max).
- `preset_norm = preset_ordinal / 9.0` — per-encoder ordinal table
  in `train_fr_regressor_v2.py::PRESET_ORDINAL`. libsvtav1's numeric
  0..13 presets squashed to 0..9.
- Two-input ONNX: `features` (N, 6) + `codec` (N, 8) -> `score` (N,).
  Mirrors LPIPS-Sq two-input precedent (ADR-0040 / ADR-0041).
- StandardScaler applied to `features` only; codec block
  passes through unscaled. `feature_mean` / `feature_std` ship in
  sidecar JSON.

Current shipped ONNX is from `--smoke` mode and is registered
`smoke: true` in `model/tiny/registry.json`. Production training run
is gated on multi-codec Phase A corpus + per-frame feature emission
in Phase A schema. See ADR-0272 + Research-0054.

## BVI-DVC corpus ingestion (ADR-0310)

Bristol VI Lab BVI-DVC reference corpus = second training
shard for `fr_regressor_v2` alongside Netflix Public drop.
Pipeline: `bvi_dvc_to_full_features.py` (parquet + cached libvmaf
JSON) → `bvi_dvc_to_corpus_jsonl.py` (vmaf-tune `CORPUS_ROW_KEYS`
rows) → `merge_corpora.py` (concatenate with Netflix shard, dedup
by `(src_sha256, encoder, preset, crf)`).

**Rebase-sensitive invariants:**

- BVI-DVC is research-only. Archive
  (`.corpus/bvi-dvc-raw/BVI-DVC Part 1.zip`), extracted MP4s
  (`.corpus/bvi-dvc-extracted/`),
  feature parquet (`runs/full_features_bvi_dvc_*.parquet`), JSONL
  corpus shard (`runs/bvi_dvc_corpus.jsonl`), and cached vmaf JSON
  (`~/.cache/vmaf-tiny-ai-bvi-dvc-full/`) **never committed**. Fork
  redistributes derived `fr_regressor_v2_*.onnx` weights only —
  corpus-must-be-license-compatible-or-stay-local applies uniformly
  across `ai/` corpora (Netflix Public, BVI-DVC, KoNViD,
  YouTube-UGC).
- Merge contract = `vmaftune.CORPUS_ROW_KEYS` from
  `tools/vmaf-tune/src/vmaftune/__init__.py`. Bumping
  `SCHEMA_VERSION` means re-running BVI-DVC adapter to backfill
  new fields. Merge utility refuses any row missing required key —
  fail-loud by design.
- Natural-key tuple `(src_sha256, encoder, preset, crf)` = dedup
  contract. Re-encodes of same source under new `(preset, crf)`
  legitimately appear as distinct rows; do not fold them by
  `src_sha256` alone.
- Production-weights flip stays gated on
  [ADR-0303](../docs/adr/0303-fr-regressor-v2-ensemble-flip.md).
  Adding BVI-DVC to corpus does NOT authorise re-shipping
  `fr_regressor_v2.onnx` without re-running ensemble gate.
- `bvi_dvc_to_full_features.py` writes
  `runs/full_features_bvi_dvc_<tier>.manifest.json` by default with
  input mode, tier, cache/model inputs, feature order, row/clip counts,
  and ADR-0661 `run_provenance`; keep manifest beside any refreshed
  local parquet.
- `bvi_dvc_to_full_features.py` accepts two mutually exclusive input
  modes: `--bvi-zip` (original; streams MP4s from archive) and
  `--bvi-dir` (ADR-0527; enumerates pre-extracted `.mp4` / `.yuv` files
  from directory). Tier inferred from resolution via closed
  `_RES_TO_TIER` dict — any new BVI-DVC release with a
  non-standard resolution requires adding row there before file
  gets picked up. Dir-mode path does **not** delete source files
  after processing; zip-mode path deletes temporarily extracted
  MP4 after processing (existing behaviour).

## KoNViD-1k full-feature refresh

`ai/scripts/konvid_to_full_features.py` = regeneration path for
`runs/full_features_konvid.parquet` and
`runs/full_features_konvid_with_folds.parquet`. Mirrors
`konvid_to_vmaf_pairs.py`'s synthetic-FR recipe (source MP4 as
reference, libx264 CRF 35 distorted side) but emits current
`FULL_FEATURES` tuple plus `vmaf` from fork CPU binary.

**Rebase-sensitive invariants:**

- Use `core/build-cpu/tools/vmaf` or explicitly verified fresh
  dev-container binary. Do not let script fall back to
  `/usr/local/bin/vmaf`; system installs have previously lacked
  fork-only extractors such as `motion_v2`, `ssimulacra2`, and the
  SpEED features.
- Folded parquet's `source=fold0..fold4` assignment = stable
  balanced hash over clip keys. Intentionally does not depend on
  directory enumeration order or row count, because
  `eval_multiseed_v3_v4.py` treats `source` as held-out fold key.
- Default cache path includes `FULL_FEATURES` count and CRF.
  Feature tuple or distortion recipe changing -> write new cache
  namespace rather than reusing stale per-clip libvmaf JSON.
- Aggregate `runs/full_features_*corpus*.parquet` files rebuilt via
  `ai/scripts/combine_full_feature_parquets.py`, not ad hoc notebook
  concatenation. Output schema =
  `corpus, source, frame_index, codec, <FULL_FEATURES>, vmaf`.
- `konvid_to_full_features.py` writes
  `runs/full_features_konvid.manifest.json` by default with KoNViD
  root/cache/model inputs, fold settings, selected/processed clip counts,
  output paths, and ADR-0661 `run_provenance`; keep it with refreshed
  parquet pair.

## v5 corpus-expansion probe — research-only (ADR-0287)

`*_vmaf_tiny_v5.py` scripts
(`fetch_youtube_ugc_subset.py`, `extract_ugc_features.py`,
`train_vmaf_tiny_v5.py`, `eval_loso_vmaf_tiny_v5.py`) = research
infrastructure for deferred v5 corpus-expansion probe. **No
`vmaf_tiny_v5.onnx` ships** — 1-σ ship gate did not clear (Δ PLCC =
+0.00005 at seed=0, far below 1-σ_v2 threshold). Extending these
scripts:

- Do not add `vmaf_tiny_v5` row to
  `model/tiny/registry.json` unless follow-up run clears ship gate
  documented in
  [ADR-0287](../docs/adr/0287-vmaf-tiny-v5-corpus-expansion.md).
- Fetcher hits public GCS bucket (`gs://ugc-dataset/`,
  CC-BY); raw videos and resulting
  `runs/full_features_ugc.parquet` must NEVER be committed
  (`runs/` and `.corpus/` trees are gitignored).
- `extract_ugc_features.py` emits same current `FULL_FEATURES`
  schema as other full-feature refresh scripts. Older versions
  intentionally populated only canonical-6, forced rest to NaN;
  do not restore that shortcut when refreshing `full_features_5corpus`.
- Dual-arm LOSO trains 18 mlp_small models
  (9 v2-baseline plus 9 v5-candidate); single invocation
  wall-time ~10–25 min depending on CPU. Do NOT launch it
  concurrently with another training process — two share
  BLAS threads and serialise badly.

## KonViD-150k MOS-corpus ingestion (ADR-0325)

**Script:** `ai/scripts/konvid_150k_to_corpus_jsonl.py`

### Rebase-sensitive invariants

- Adapter accepts two local layouts under `.corpus/konvid-150k/`:
  URL `manifest.csv` plus `clips/`, or split score-drop layout
  `k150ka_scores.csv` / `k150kb_scores.csv` plus
  `k150ka_extracted/` / `k150kb_extracted/`. Do not remove split
  discovery path unless staged corpus is migrated first.
- Explicit `--manifest-csv` remains strict. That file missing ->
  adapter must fail instead of falling back to split discovery; this
  catches typoed operator paths.
- Emitted JSONL schema is still shared MOS-corpus schema. Split
  score rows do not add `split` column to output; missing score-drop
  metadata is represented as `mos_std_dev = 0.0` and `n_ratings = 0`.

## CHUG HDR MOS-corpus ingestion (ADR-0426)

**Script:** `ai/scripts/chug_to_corpus_jsonl.py`

### Rebase-sensitive invariants

- CHUG data is local-only under `.corpus/chug/`. Do not commit the
  public `chug.csv`, downloaded MP4s, emitted JSONL, trained local
  CHUG heads, or derived features. README/license mismatch is
  handled by treating dataset as non-commercial/share-alike until
  clarified.
- CHUG's public `mos_j` column on 0-100 axis. Adapter preserves
  it as `mos_raw_0_100` and maps trainer-facing `mos` onto `[1, 5]`
  via `1 + 4 * mos_raw_0_100 / 100` so existing MOS-head trainer
  does not drop every row as out-of-range. Do not remove raw field
  or silently change scale.
- Adapter preserves CHUG HDR / ladder metadata (`chug_bitladder`,
  `chug_resolution`, `chug_bitrate_label`, orientation, manifest
  geometry, and source content name) as optional JSONL fields. Existing
  MOS-head training ignores those columns today; future HDR models may
  consume them explicitly.
- CHUG feature materialiser is governed by ADR-0427. It pairs each
  distorted row with matching `chug_content_name` reference row,
  decodes both sides as 10-bit 4:2:0, and scales distorted side to
  reference geometry before libvmaf extraction. Changing that alignment
  policy changes training distribution and requires new ADR.
- CHUG train/validation/test splits are content-level, not row-level.
  `ai/scripts/chug_extract_features.py` hashes `chug_content_name` with
  seed `chug-hdr-v1` into deterministic 80/10/10 partitions and writes
  chosen `split` plus `chug_split_key` into every feature row. Do
  not split bitrate-ladder rows independently; that leaks same
  source content across validation.
- Local HDR metadata audit (`--audit-output`) is a pre-training
  gate for CHUG experiments. Preserve its ffprobe transfer / primaries /
  pix-fmt counters and malformed-PQ/HLG-without-BT.2020 row list when
  touching materialiser.
- Per ADR-0651, CHUG feature materialiser also writes per-row
  `feature_ref_*` and `feature_dis_*` HDR/display metadata copied from
  ffprobe (`codec_name`, `pix_fmt`, `color_transfer`, normalized
  `transfer_class`, primaries, colorspace/range, MaxCLL/MaxFALL-style
  static metadata). Preserve unknown/null values explicitly; do not
  infer display-panel capability from clip metadata alone.
- Per ADR-0652, same decode pass writes luma-domain visual-signal
  primitives (`luma_std`, `sharpness_laplacian_var`,
  `highfreq_abs_mean`, `noise_lap_mad`) for both reference and
  distorted clips plus `feature_delta_*` distorted-minus-reference
  fields. These are diagnostic blur/noise/grain proxies; do not treat
  them as a replacement for a trained NR VQA model.
- `ai/scripts/enrich_k150k_parquet_metadata.py` is recovery path for
  FULL_FEATURES parquet jobs that were started without `--metadata-jsonl`.
  It must match metadata by `clip_name` / JSONL basename, fill missing
  metadata cells by default, and keep feature/MOS columns unchanged unless
  `--overwrite-metadata` is explicitly passed.

## K150K-A corpus extraction (ADR-0362, ADR-0382, ADR-0431)

**Script:** `ai/scripts/extract_k150k_features.py`
**Branch:** `chore/ensemble-kit-gdrive-quickstart`

### Rebase-sensitive invariants

- **Parquet writes at-end only — never per-flush (Research-0135 Win 1).**
  Rows accumulated in memory throughout run, appended to JSONL staging file
  (`<out>.rows.jsonl`) for crash durability. Parquet written exactly once at
  end via `_write_parquet_from_rows`. Old `_flush_parquet` helper (which read
  growing parquet on every 200-clip flush) removed. Restartability still
  guaranteed by `.done` checkpoint file; staging file adds second durability
  layer so rows aren't lost on unclean exit. Do not re-introduce per-flush
  parquet writes — they make total parquet I/O O(N²) over corpus size.
- **Staging file is main-process-only (Research-0135).**
  `_append_row_to_staging` called only from main process inside the
  `as_completed()` loop, after `fut.result()` returns. Worker subprocesses
  must never write to staging file. Violating single-writer semantics on
  staging file would corrupt it without error.
- **ffprobe skipped when sidecar has geometry (Research-0135 Win 2).**
  `_geometry_from_sidecar(meta)` reads `chug_width_manifest`,
  `chug_height_manifest`, `chug_framerate_manifest`, and optionally
  `chug_bit_depth` from CHUG JSONL sidecar row. Sidecar metadata already
  loaded in `jsonl_meta` for enrichment; no extra I/O needed. Any required
  field absent (K150K-A clips, incomplete rows) -> function returns `None`
  and `_probe_geometry(mp4)` called as fallback. Do not remove fallback —
  K150K-A clips have no sidecar.
- **Binary requirement:** script requires `core/build-cpu/tools/vmaf`
  (fork build); system `/usr/local/bin/vmaf` v3.0.0 lacks `ssimulacra2`
  and `motion_v2`. `--vmaf-bin` default (in `main()`) now points to
  `core/build-cpu/tools/vmaf`. Do NOT switch to `build-cuda/tools/vmaf`
  as default — CUDA binary has latent CLI double-write bug when
  `--feature <x>` is combined with auto-loaded default VMAF model
  (see Research-0096 / ADR-0382 for details).
- **CUDA split invariant:** operators explicitly passing CUDA-capable
  `--vmaf-bin` -> script must use explicit CUDA extractor names for the
  GPU-safe pass and `--cpu-vmaf-bin` for residual CPU pass
  (`float_ssim`, `cambi`). Do not re-collapse this into one generic
  `--backend cuda` invocation; CHUG/K150K 10-bit clips can fail
  `context could not be synchronized` through that path.
- **Parallelism model:** script uses `concurrent.futures.ProcessPoolExecutor`
  with `--threads-cuda` workers (default 8). Each worker fully independent.
  `--threads-cuda` flag named for historical reasons; controls outer
  process parallelism for both CPU and split CUDA modes. Do not switch to
  threading — libvmaf subprocess invocations not thread-safe for concurrent
  parallel pipelines.
- **Checkpoint thread-safety:** `_append_done()` called only from main
  process (after `fut.result()` returns in the `as_completed()` loop). Do not
  call it from worker processes — append-only guarantee relies on single-writer
  semantics.
- **NaN propagation:** `ciede2000` and `psnr_hvs` return `null` from vmaf
  when ref == distorted (identity pair). All-NaN columns are **expected** —
  do not treat them as extraction failures. `np.errstate(all="ignore")`
  in `_aggregate_frames()` suppresses numpy warning; preserve it.
- **Column-order lock:** `FEATURE_NAMES` (line ~121) defines the 21-feature
  column order (parquet schema v2) downstream loaders depend on. Appending
  is safe; reordering or removing entries breaks existing parquets and any
  trained model that consumed them. Increment parquet schema version in a
  separate ADR if reordering becomes necessary. **Schema v2 invariant
  (ADR-0431):** `ssimulacra2` omitted from K150K/CHUG self-vs-self
  extraction. In identity pairs (ref == distorted) it produces a
  constant ~100, yielding zero training signal while consuming
  30–50% of GPU time per clip. Operating in FR-from-NR mode (same
  video on both sides) -> all difference-based metrics
  (difference-based ssimulacra2, ciede2000, psnr_hvs, ADM, VIF)
  degenerate; see ADR-0362 §Negative consequences. CPU-only
  ssimulacra2 extraction remains available for genuine FR pairs
  where it is informative.
- **FEATURE_NAMES completeness invariant:** all `FEATURE_NAMES` entries
  must map to JSON keys emitted by pipeline (CUDA extractors, CPU
  residual, or `--model` dispatch). `vmaf` entry = model composite score
  emitted via `--model` arg in `_run_feature_passes`; all other entries
  = raw features emitted via `--feature` arguments.
- **vmaf column computed via vmaf_v0.6.1 (Research-0135):** `vmaf`
  column in CHUG/K150K output parquets is computed by dispatching the
  SDR `vmaf_v0.6.1` model via `--model version=vmaf_v0.6.1` in the
  libvmaf CLI invocation. Model is SDR-trained and mis-calibrated on PQ
  HDR clips; scores valid for relative bitrate-ladder comparison within
  a content group but not meaningful as absolute HDR quality targets.
  Replace with Netflix HDR model when it ships (change `--model` arg in
  `_run_feature_passes`; no schema change required). Do NOT remove the
  `--model` arg without an ADR — vmaf relationship across ladder rungs
  is a required training feature per user direction 2026-05-16.
- **Checkpoint format:** `.done` file is append-only, one clip name per
  line, no header. Changing format without migration breaks in-progress
  runs. `_load_done_set()` / `_append_done()` helpers = single-exit-point
  for reads and writes; add any format change there.
- **`.done` = authoritative ledger; parquet row count must match on
  restart (ADR-0862).** Restart no-op branch in `main()` compares
  `len(done_set)` against `_parquet_row_count(args.out)` plus rows
  recovered from JSONL staging file. Raises `RuntimeError` on
  mismatch instead of silently writing `status=complete-noop` and
  returning 0. Do not weaken this check to warning. Do not
  auto-truncate `.done`. Do not skip it under any `--allow-*` flag.
  Silent confirmation of row deficit = exact failure mode this
  guard prevents (Bug-3 RCA 2026-05-30 lost ~92 K rows). End-of-run
  write path has matching `len(rows) == len(recovered_rows) + ok`
  assert; preserve it through any future refactor of
  `as_completed` accounting loop.
- **fsync parquet before unlinking staging (ADR-0862).** Both the
  no-op branch and end-of-run write path call `_fsync_path(args.out)`
  AFTER the parquet rename(2) and BEFORE
  `staging_path.unlink(missing_ok=True)`. Helper fsyncs file
  and its parent directory so rename(2) is durable before the
  companion unlink can race ahead of it on power loss. Do not reorder
  these calls or drop the `fsync` — staging-as-WAL design depends
  on the parquet being durable when the WAL is discarded.
- **JSONDecodeError surface (ADR-0862).** `_load_staging_rows`
  reports the count of malformed lines to stderr as a WARNING.
  Do not revert this to silent `continue`: a truncated-tail
  staging file = leading indicator worker died mid-write,
  and the operator needs to know.
- **Gitignore:** `runs/full_features_k150k.parquet` and
  `runs/k150k_extract.log` are gitignored (152K-clip output not tracked).
  Do not commit these files.
- **FR-from-NR adapter:** the script does NOT call `NrToFrAdapter` from
  the Python training harness — builds vmaf CLI argv directly with
  ref == distorted, which is the lighter-weight equivalent. Any upstream
  refactor of the Python adapter is irrelevant to this script.
- **FR-corpus misuse guard (ADR-0509):** script = no-reference adapter.
  Running it on a full-reference corpus (CHUG: `chug_ref==1` references
  paired with bitrate-ladder distortions for same
  `chug_content_name`) silently produces a parquet where every clip is
  scored against itself. Every difference-based metric collapses to
  its identity-pair floor (`adm2 == vif_* == 1.0`, `psnr_y == 60`,
  `ciede2000 / psnr_hvs == NaN`, `vmaf ~= 99`); the parquet carries
  zero training signal. `detect_fr_corpus_misuse(meta_by_clip)` returns
  `{misuse_detected: bool, ref_count, dis_count, content_groups_with_both,
  example}`; `main()` exits 2 before spawning any worker when the loaded
  sidecar carries the FR signature. Use `ai/scripts/chug_extract_features.py`
  for FR corpora — it pairs each distorted row with its matching
  reference. `--allow-fr-from-nr` opt-in flag is reserved for genuine
  identity-pair studies on an FR corpus; do NOT default-on it in any
  script or recipe. Guard runs on the **loaded** `jsonl_meta` dict
  (after `_load_jsonl_metadata` filters the raw sidecar), so
  `_load_jsonl_metadata`'s keep-list MUST preserve `chug_ref` and
  `chug_content_name`. Pinned by 3 unit tests
  (`test_detect_fr_corpus_misuse_*`) in `ai/tests/test_extract_k150k_features.py`.

## v3 retrain invariant — `ENCODER_VOCAB` 13 → 16 (ADR-0302)

`ENCODER_VOCAB_V3` parallel constant in
[`scripts/train_fr_regressor_v2.py`](scripts/train_fr_regressor_v2.py)
documents target 16-slot vocab (adds `libsvtav1`,
`h264_videotoolbox`, `hevc_videotoolbox` to v2's 13 slots). **Live
`ENCODER_VOCAB` and `ENCODER_VOCAB_VERSION = 2` are source of truth**
until follow-up retrain PR clears LOSO PLCC ship gate.

**Invariants v3 retrain PR must honour** (per ADR-0235 +
ADR-0291 + ADR-0302):

- Schema bump (v2 → v3) requires fresh LOSO run clearing **mean
  LOSO PLCC ≥ 0.95** across all 9 Netflix sources (matches gate
  ADR-0291 cleared on v2). Trainer must exit non-zero and refuse
  to overwrite registry entry on failure — same pattern
  `fr_regressor_v1` already enforces.

  **Status (ADR-0323, 2026-05-06):** First v3 LOSO run shipped
  under [`ai/scripts/train_fr_regressor_v3.py`](scripts/train_fr_regressor_v3.py)
  on NVENC-only Phase A corpus (5,640 rows, 9 sources × 4 CQs).
  Mean LOSO PLCC = **0.9975 ± 0.0018** (every source above 0.99) —
  comfortably clears 0.95 ship gate. Model ships under
  `model/tiny/fr_regressor_v3.onnx` with `smoke: false`. Live
  `ENCODER_VOCAB_VERSION = 2` in [`scripts/train_fr_regressor_v2.py`](scripts/train_fr_regressor_v2.py)
  **stays authoritative for `fr_regressor_v2.onnx`** until separate
  "promote v3 to authoritative" PR — this PR ships v3 as parallel
  checkpoint, not v2 replacement. Future v3 retrains (on
  multi-codec corpus drop) must continue to clear 0.95 floor and
  must additionally measure ADR-0235 multi-codec lift floor
  (≥+0.005 PLCC over `fr_regressor_v1`); lift floor not yet
  measurable on NVENC-only corpus, so this PR's gate = 0.95
  floor only.
- Multi-codec lift over v1 single-input regressor must remain
  **≥ +0.005 PLCC**. ADR-0235 set this as codec-block invariant;
  v2 production checkpoint cleared it comfortably and v3 must
  not regress.
- In-tree v2 ONNX (`model/tiny/fr_regressor_v2.onnx`) **must
  not be replaced** until new v3 ONNX clears gate. Load-fallback
  shim collapses unknown v3 strings into v2
  `unknown` column and lets v2 keep serving every consumer in the
  meantime.
- Append-only ordering is load-bearing — 13 v2 slot indices
  (0..12) keep their column positions verbatim under v3; three
  new slots append at indices 13/14/15. Reordering silently
  invalidates every shipped `fr_regressor_v2_*.onnx`. ADR-0235
  documents this rule for `CODEC_VOCAB`; ADR-0302 §Decision
  re-asserts it for `ENCODER_VOCAB`.
- Slot strings must match vmaf-tune codec-adapter registry keys
  exactly (`libsvtav1`, `h264_videotoolbox`, `hevc_videotoolbox`).
  ADR-0235 §References pins this rule globally for all corpus
  emitters.

## `fr_regressor_*` namespace map (ADR-0349)

`fr_regressor` lineage carries two orthogonal axes. Encoder-vocab
versioning runs on `_v{N}` (v1 = no codec block, v2 = 13-slot, v3 = 16-slot).
Feature-set versioning runs as `_v{N}plus_features` suffix on the matching
encoder-vocab base. Names below are claimed; do **not** reuse them for
unrelated workstreams.

| Name | Encoder vocab | Feature axis | Status |
|---|---|---|---|
| `fr_regressor_v1` | none (single-input) | canonical-6 | shipped (ADR-0249) |
| `fr_regressor_v2` | v2 (13-slot) | canonical-6 + 8-D codec block | shipped (ADR-0272 / ADR-0291) |
| `fr_regressor_v2_ensemble_v1_seed{0..4}` | v2 (13-slot) | canonical-6 + 8-D codec block | shipped (ADR-0279) |
| `fr_regressor_v3` | v3 (16-slot) | canonical-6 + 18-D codec block | shipped (ADR-0302 / ADR-0323) |
| `fr_regressor_v3plus_features` | v3 (16-slot) | canonical-6 + `encoder_internal` + shot-boundary + `hwcap` | **reserved** (ADR-0349) — registry row lands with future PR that ships the `.onnx` |

Reservation is documentation-only because
[`core/test/dnn/test_registry.sh`](../core/test/dnn/test_registry.sh)
treats every registry row as hard contract (file must exist, sha256 must
match, sidecar must accompany every `smoke: false` entry); stub row would
fail CI on day one. Future `_v3plus_features` PR populates row in
the same commit that ships `.onnx`. See
[ADR-0349](../docs/adr/0349-fr-regressor-v3-namespace.md) for namespace
decision and rejected alternatives.

## MOS-head v1 invariants — `konvid_mos_head_v1` (ADR-0336, Phase 3 of ADR-0325)

Fork's first head trained against subjective MOS (not VMAF)
ships under
[`ai/scripts/train_konvid_mos_head.py`](scripts/train_konvid_mos_head.py),
with trained ONNX and human-readable model card at
[`model/konvid_mos_head_v1.onnx`](../model/konvid_mos_head_v1.onnx) and
[`model/konvid_mos_head_v1_card.md`](../model/konvid_mos_head_v1_card.md).
Invariants any follow-up retrain or corpus-shape PR must honour:

- **Feature-column order is load-bearing.** `FEATURE_SCHEMA_KONVID_V1`
  maps to `FEATURE_COLUMNS = CANONICAL_6 + EXTRA_FEATURES`, exact
  11-D layout baked into trained KonViD ONNX and consumed by
  `tools/vmaf-tune/src/vmaftune/predictor.py::_predict_mos_via_head`.
  6 canonical columns occupy indices 0..5; 5 extras
  (`saliency_mean`, `saliency_var`, `shot_count_norm`,
  `shot_mean_len_norm`, `shot_cut_density`) occupy 6..10. Reordering
  silently invalidates every shipped `konvid_mos_head_v1.onnx`.
  New experimental layouts must be separate named schemas in
  `FEATURE_SCHEMAS`, not in-place edits to `FEATURE_COLUMNS`.
- **ENCODER_VOCAB v4 expansion is append-only.** v4 vocab ships
  with single `"ugc-mixed"` slot per ADR-0325 §Decision. LSVQ +
  YouTube-UGC ingestion landing -> new slots append at end;
  existing trained ONNX stays loadable and predictor's per-shot
  one-hot widens transparently.
- **MOS range = `[1.0, 5.0]`, baked into graph.** Trainer wraps
  MLP output in `MOS_MIN + (MOS_MAX - MOS_MIN) * sigmoid(raw)`;
  adversarial input cannot drive prediction outside `[1, 5]`.
  Predictor surfaces (`Predictor.predict_mos` +
  `_predict_mos_via_head`) carry additional clamp as
  belt-and-braces. Do not change range without schema bump +
  retrain.
- **Production-flip gate is not lowered on real-corpus failures.**
  Per memory `feedback_no_test_weakening` and ADR-0325 §Production-flip
  gate, failing real-corpus retrain ships head with
  `Status: Proposed`, *not* relaxed gate. Threshold values
  (`PLCC ≥ 0.85`, `SROCC ≥ 0.82`, `RMSE ≤ 0.45`, `spread ≤ 0.005`)
  are constants in trainer (`GATE_*`); changing them requires
  new ADR.
- **Predictor fallback path is documented behaviour, not a bug.**
  ONNX missing -> `Predictor.predict_mos` returns
  `(predicted_vmaf - 30) / 14` clamped to `[1, 5]`. That's
  documented contract; tests
  (`tools/vmaf-tune/tests/test_predict_mos.py::test_predict_mos_falls_back_when_onnx_missing`)
  pin it. Removing fallback breaks every dev host that hasn't
  pulled ONNX.
- **CHUG HDR MOS uses CHUG-named entry point.**
  `ai/scripts/train_chug_hdr_mos_head.py` = operator-facing
  command for CHUG HDR subjective-MOS experiments. May reuse the
  same small MOS-head training loop, but docs and local commands must
  not tell operators to pass CHUG shards through KonViD-named
  flags. Local CHUG manifests use `chug_hdr_mos_head_v1` so HDR
  MOS signal isn't confused with committed SDR KonViD head. The
  CHUG wrapper defaults to `FEATURE_SCHEMA_CHUG_HDR_WIDE_V1`
  (`chug-hdr-wide-v1`): canonical-6 means, p10/p90/std temporal
  aggregates, and HDR ladder / geometry metadata. Keep that 34-D order
  append-only for local CHUG checkpoints; use `--feature-schema
  konvid-v1` only for ablation runs against older 11-D baseline.

## Knob-sweep recipe-regression policy (ADR-0308)

Cited from regression-detection invariant in
[ADR-0305](../docs/adr/0305-encoder-knob-space-pareto-analysis.md)
and policy decision in
[ADR-0308](../docs/adr/0308-encoder-knob-sweep-recipe-regression-policy.md);
populated findings are in
[Research-0080](../docs/research/0080-encoder-knob-sweep-findings.md).
Extending `ai/scripts/analyze_knob_sweep.py` or anything that
consumes its output:

- Recipe regression is *structural* iff it reproduces on **≥7 of
  the 9** corpus sources within a single
  `(codec, rc_mode, recipe, preset, q)` cell. Structural regressions
  are forbidden as `tools/vmaf-tune/codec_adapters/*` defaults and
  forbidden as `vmaf-tune recommend` outputs without explicit
  override. Known-structural cells are listed in
  Research-0080 §Aggregated-bad-recipe-patterns; do not promote any
  of them to an adapter-level default in a follow-up PR.
- Recipe regression that hits 1-6 sources is *content-dependent*
  and is filtered at recommend-time via per-slice hull lookup,
  not at adapter-default time.
- Do NOT modify `ai/scripts/analyze_knob_sweep.py` to relax
  `bitrate_tol_pct` (default 5.0) or `vmaf_tol` (default 0.1)
  without an ADR. Tolerances calibrated against per-frame VMAF
  noise floor and bitrate quantisation in libavformat muxers;
  loosening them silently masks structural cluster (see ADR-0305
  §Consequences).
- Detector is an **offline** gate (3-hour sweep, ~2 GiB JSONL,
  single-host variance); do not wire it into CI without first
  designing smaller stratified sample that reproduces
  structural patterns. Tracked as follow-up in ADR-0308 §Decision
  point 4.
- Corpus producer (`hw_encoder_corpus.py`) currently emits
  `(src, actual_kbps, vmaf, enc_ms, recipe)` while
  `analyze_knob_sweep.SweepRow` consumes
  `(source, bitrate_kbps, vmaf_score, encode_time_ms,
  is_bare_default)`. Producer-side rename not yet landed
  (SCHEMA_VERSION=3 follow-up per ADR-0308 §Decision point 5); any
  analysis run goes through a throw-away wrapper that performs
  the rename in-process. Do NOT modify
  `analyze_knob_sweep.py` to accept both spellings.

## `u2netp` fork-local mirror invariants (ADR-0412 / ADR-0671)

Fork ships a release-artefact mirror for the upstream U-2-Net
`u2netp` checkpoint via GitHub Release attachments. Scaffold
(license, model card, operator doc, supply-chain staging step)
landed in PR scope ADR-0412; exporter landed in ADR-0671; binary
upload is a separate PR.

- **Never commit `model/u2netp_mirror.onnx` or
  `model/u2netp_mirror.pth` to git.** Both paths are gitignored
  (see `.gitignore`). Binary lives in GitHub Release assets
  only — signed via Sigstore, hashed for SLSA, paired with
  `LICENSES/LicenseRef-Apache-2.0-u2netp.txt` at upload time. Binary
  upload PR ever attempting to commit either file -> ADR-0412
  contract is broken; reject the PR.
- **Exporter imports upstream code; does not vendor it.**
  `ai/scripts/export_u2netp_mirror.py` expects an audited local
  `xuebinqin/U-2-Net` checkout plus `u2netp.pth`, then exports
  ONNX and a `u2netp-mirror-export-manifest-v1` sidecar. Keep this
  boundary intact: copying U-2-Net source into this repository or
  silently accepting non-Apache license text breaks ADR-0671.
- **Recommended saliency weights remain
  `saliency_student_v1`** (ADR-0286, fork-trained DUTS student
  under BSD-2-Clause-Patent). `u2netp_mirror` is the named
  *fallback* for upstream-lineage citation, comparative
  evaluation, or downstream pipelines pinned to upstream
  behaviour. Do NOT flip `model/tiny/registry.json`'s default
  `mobilesal` resolution to `u2netp_mirror_v1` without an ADR
  superseding ADR-0286.
- **Apache-2.0 §4 (a) + (c) compliance is non-negotiable.**
  Every release that carries `u2netp_mirror_v*` must also carry
  `LICENSES/LicenseRef-Apache-2.0-u2netp.txt` with its attribution block
  intact. supply-chain.yml staging step pairs them
  automatically; future refactor decoupling them -> downstream
  operators inherit a license-non-compliant artefact. §4 (b)
  applies only to ONNX rewraps (export script writes a
  `metadata_props` block recording the conversion provenance);
  verbatim `.pth` redistribution does not trigger (b). §4 (d) is
  moot — upstream ships no NOTICE file.
- **Binary upload PR re-pins upstream commit.** Scaffold-time
  pin is HEAD `ac7e1c81`. Binary upload PR
  must verify upstream `LICENSE` SPDX is still Apache-2.0
  and tree still carries no NOTICE file at
  upload-time HEAD, then bump model card's commit pin
  accordingly.

- MOS-corpus row schema emitted by
  `ai/scripts/lsvq_to_corpus_jsonl.py` (ADR-0367) is byte-identical
  to KonViD-150k Phase 2 adapter
  (`ai/scripts/konvid_150k_to_corpus_jsonl.py`) modulo the
  `corpus` and `corpus_version` literals. Both are consumed
  through one trainer-side data loader. Do NOT widen schema
  in only one adapter — adding or removing a column means a
  lockstep edit across both, plus a `corpus_version` bump.

- **CHUG display-profile training is trainer-side context, not
  corpus-schema mutation.** `train_chug_hdr_mos_head.py` keeps
  `chug-hdr-wide-v1` as no-profile default, and auto-selects
  `chug-hdr-display-v1` only when `--display-profile-json` is supplied
  without an explicit `--feature-schema`. Row-local display columns win
  over the target profile so future multi-display HDR corpora remain
  usable. Do not widen CHUG JSONL adapters to carry one operator's
  local panel profile; profiles are recorded in the emitted manifest
  with their source sha256.

  `ai/scripts/youtube_ugc_to_corpus_jsonl.py` (ADR-0368) is
  byte-identical to the LSVQ adapter
  (`ai/scripts/lsvq_to_corpus_jsonl.py`, ADR-0333) and the
  KonViD-150k Phase 2 adapter modulo the `corpus` and
  `corpus_version` literals. All three are consumed through one
  trainer-side data loader. Do NOT widen schema in only one
  adapter — adding or removing a column means a lockstep edit
  across all three, plus a `corpus_version` bump. The synthesised
  bucket-URL path (`--bucket-prefix` flag) is YouTube-UGC-specific
  because the canonical `original_videos.csv` ships without a
  `url` column; do not back-port that synthesis seam to the LSVQ
  / KonViD-150k adapters where it would mask manifest-CSV bugs.

  `ai/scripts/waterloo_ivc_to_corpus_jsonl.py` (ADR-0369) is
  byte-identical to the LSVQ adapter
  (`ai/scripts/lsvq_to_corpus_jsonl.py`, ADR-0333) and the
  KonViD-150k Phase 2 adapter
  (`ai/scripts/konvid_150k_to_corpus_jsonl.py`, ADR-0325 Phase 2)
  modulo the `corpus` and `corpus_version` literals. All three
  adapters are consumed through one trainer-side data loader. Do
  NOT widen schema in only one adapter — adding or removing
  a column means a lockstep edit across all three (plus a
  `corpus_version` bump on each). Waterloo IVC adapter
  records MOS verbatim on the dataset's native **0–100** scale,
  diverging from KonViD / LSVQ's 1–5 Likert scale; cross-corpus
  rescaling is a trainer-side concern and is NOT applied at
  ingest time on either adapter. Trainer-side normaliser
  must read each row's `corpus` literal to pick the correct
  per-shard rescale factor.

- **`vmaf_train.train.TrainConfig`, `vmaf_train.registry.ModelMetadata`,
  and `vmaf_train.data.datasets.ManifestEntry` are pydantic v2
  `BaseModel`s, not `@dataclass`es.** (ADR-0934.) They parse
  operator-supplied YAML / JSON, so the boundary needs declared
  validators + `extra="forbid"` + line-numbered errors. Every other
  dataclass in `ai/src/vmaf_train/` (`NormReport`, `BisectResult`,
  `EvalReport`, `CrossBackendReport`, `ModelAudit`, `ProfileReport`,
  `QuantizationReport`, `AllowlistReport`, `Splits`, `Entry`, etc.)
  is producer-controlled and stays as `@dataclass` on purpose — do
  not mass-convert "for consistency". Rebase rule: new
  dataclass ingesting `yaml.safe_load(...)` or `json.loads(...)`
  output via `**doc` or `cls(field=doc["field"], ...)` becomes
  `BaseModel`; new dataclass produced by typed function call
  stays `@dataclass`. `ModelMetadata.to_json()` round-trips via
  `model_dump(mode="json")` + `json.dumps(indent=2, sort_keys=True)`;
  do not switch it to `BaseModel.model_dump_json()` (different
  formatting — would invalidate sidecar goldens).

- **`extract_k150k_features.py` must fail loud, never write a silent
  garbage row.** Two corruption paths were closed (T-K150K-TRAINING-DATA-
  INTEGRITY-2026-06-20) and the invariants must survive rebases:
  (1) `_process_clip` **raises** on an empty frame list — never let an
  all-`NaN` aggregate row reach the corpus + `_append_done` (it would be
  dropped with no retry). (2) The MOS-label join tolerates a
  filename↔`video_name` extension mismatch via an `mp4.stem` fallback and
  is guarded by an up-front coverage check that hard-fails the zero-match
  case before any multi-day GPU extraction starts. Do not "simplify" the
  lookup back to a single `mos_map.get(clip_name, NaN)` — that is the bug.
  Staging→`.done` write order is deliberately staging-first (crash
  leaves the clip re-processable; the final parquet dedups by `clip_name`,
  `keep="last"`); do not reorder it.

- **AI teacher model single source and table provenance invariants (ADR-1173).**
  (1) AI training and extraction scripts resolve their teacher model through
  `ai.data.scores.resolve_teacher_model()` (which imports `DEFAULT_MODEL` from
  `vmaftune.defaultmodel`), falling back to `$VMAF_MODEL_PATH` then `DEFAULT_MODEL`.
  No script under `ai/` may hardcode `"vmaf_v0.6.1"` or any literal model fallback.
  (2) Feature producers (`extract_full_features.py`, `extract_k150k_features.py`,
  `bvi_dvc_to_full_features.py`, `extract_ugc_features.py`, `konvid_to_full_features.py`,
  `konvid_to_vmaf_pairs.py`, `bvi_dvc_to_corpus_jsonl.py`) unconditionally write a
  `teacher_model` column on every row.
  (3) Combiners and trainers (`combine_full_feature_parquets.py`,
  `train_vmaf_tiny_v5.py`, `eval_loso_vmaf_tiny_v5.py`) verify teacher uniformity within
  and across all input shards. Shards with different teacher models are strictly refused.
  Tables lacking a `teacher_model` column are rejected unless `--assume-teacher <name>`
  is explicitly passed for legacy datasets.
  (4) Raw extraction feature lists (`FULL_FEATURES` in `ai/data/feature_extractor.py` and
  `FEATURE_NAMES` in `ai/scripts/extract_k150k_features.py`) include `"adm3"`. The
  canonical-6 student feature set (`DEFAULT_FEATURES`: `adm2`, `vif_scale0..3`, `motion2`)
  remains strictly frozen.
