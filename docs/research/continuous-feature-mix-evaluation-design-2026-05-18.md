# Continuous Feature-Mix Evaluation Pipeline — Design Specification

**Date**: 2026-05-18
**Status**: Draft — pending ADR-0565 review
**Author**: Lusoris / Claude Code
**Companion ADR**: [ADR-0565](../adr/0565-continuous-feature-mix-eval-pipeline.md)

---

## 1. Problem Statement

The fork ships three overlapping prediction surfaces on top of Netflix's
classic SVM VMAF stack:

1. **Classic SVM regressor** — `vmaf_v0.6.1`, `vmaf_4k_v0.6.1`, upcoming
   HDR model (`vmaf_hdr_v*`). Each consumes a fixed feature mix chosen at
   Netflix training time.
2. **Fork-local tiny-AI models** — `vmaf_tiny_v2/v3/v4`, `fr_regressor_v1/v2/v3`,
   `nr_metric_v1`, `konvid_mos_head_v1`. Each also consumes a fixed feature
   mix, but that mix was chosen against a specific corpus at a specific point
   in time.
3. **Per-shot encode predictor** — `predictor_<codec>.onnx` family, trained
   from Phase A corpus data to predict VMAF from cheap probe-encode signals.

The core problem is that no automated process answers the following questions
after any change to the feature set, model set, codec set, display target, or
tuning preset:

- Does each predictor still add accuracy beyond the Netflix SVM baseline?
- What is the best feature subset for each (target_model × codec × display ×
  tuning_preset × corpus) cell, balancing accuracy, parsimony, and extraction
  cost?
- How wide are the uncertainty intervals on those marginal gains?
- Does the gain exceed the configurable "operator-meaningful" threshold needed
  to justify shipping?

Several near-term events make this urgent:

- **Netflix HDR VMAF model**: Netflix is expected to release a new model that
  consumes `speed_chroma`, `speed_temporal`, and possibly additional features
  (per ADR-0559 and upstream branch tracking). This adds a new model dimension
  to the evaluation grid.
- **CHUG-HDR corpus**: An HDR-corpus equivalent of CHUG is expected imminently.
  HDR content changes which features are informative and which models are
  appropriate, requiring a new corpus dimension.
- **Codec evolution**: NVENC, SVT-AV1, and AMF adapters evolve continuously.
  The per-codec predictor family and the codec-aware `fr_regressor_v2` each
  need re-validation whenever adapter defaults change.
- **Feature set churn**: ADR-0559 added `speed_chroma` and `speed_temporal` to
  extraction scripts. Future upstream additions may follow. Each addition is a
  potential input to any model in the registry.

Without a continuous evaluation pipeline, the fork accumulates technical debt
in the form of models that may no longer be the best choice for their declared
target cell, and predictor value claims that cannot be audited against current
data.

**Scope boundary**: This pipeline is an *evaluation and monitoring* surface,
not a training surface. It consumes pre-extracted feature columns from existing
corpus JSONLs and parquet files; it does not re-run libvmaf extraction. The
training loop (`predictor_train.py`, `ai/scripts/train_*.py`) remains separate.

---

## 2. Eval Grid Schema

### 2.1 Design rationale

A (model × codec × display × tuning_preset × corpus) cell is the atomic unit
of evaluation. The grid grows as dimensions expand; many combinations are
invalid (e.g., HDR model × SDR display, NR metric × FR codec predictor). The
schema must encode both the valid cells and their gate conditions.

The representation uses YAML for human-readable authoring with a Python
dataclass layer for programmatic consumption. JSON was rejected because
comments and multi-line strings are important for documenting cell semantics.
Jinja-style templating was rejected to keep the schema parseable without a
template engine at import time.

### 2.2 Top-level schema

```yaml
# predictor_bench.yaml — eval grid definition
version: "1"
seed: 42                            # global reproducibility seed

# Corpora available for evaluation.
# Each entry names a feature-parquet or corpus JSONL that has already
# been extracted. The pipeline never re-extracts; it reads columns.
corpora:
  netflix_public:
    path: ".workingdir2/netflix/features.parquet"
    type: parquet
    color_space: sdr
    has_hdr: false
    sources:                        # LOSO source IDs
      - BigBuckBunny
      - BirdsInCage
      - CrowdRun
      - ElFuente1
      - ElFuente2
      - FoxBird
      - OldTownCross
      - Seeking
      - Tennis

  chug_sdr:
    path: ".workingdir2/chug/features_sdr.parquet"
    type: parquet
    color_space: sdr
    has_hdr: false
    sources: auto                   # inferred from 'source' column

  chug_hdr:
    path: ".workingdir2/chug/features_hdr.parquet"
    type: parquet
    color_space: hdr
    has_hdr: true
    sources: auto
    pending: true                   # skip if file does not exist

  konvid_1k:
    path: ".workingdir2/konvid/features.parquet"
    type: parquet
    color_space: sdr
    has_hdr: false

# Target models — what are we predicting?
# The pipeline fits a probe model (see §4) for each target; the
# Netflix SVM output is the baseline.
target_models:
  vmaf_v0_6_1:
    label: "VMAF v0.6.1 (SDR)"
    display: sdr
    feature_columns:                # the columns the SVM itself uses
      - adm2
      - vif_scale0
      - vif_scale1
      - vif_scale2
      - vif_scale3
      - motion2
    baseline: svm                   # always compare against the SVM

  vmaf_hdr_v1:
    label: "VMAF HDR v1 (anticipated)"
    display: hdr
    feature_columns:
      - adm2
      - vif_scale0
      - vif_scale1
      - vif_scale2
      - vif_scale3
      - motion2
      - speed_chroma
      - speed_temporal
    baseline: svm
    pending: true                   # skip until model is shipped upstream

# Codec families for conditioning the codec-aware predictor cells.
# "any" means the cell is corpus-wide (not per-codec).
codecs:
  any:
    label: "All codecs (corpus-level)"
  libx264:
    label: "x264 (H.264)"
  libx265:
    label: "x265 (HEVC)"
  libsvtav1:
    label: "SVT-AV1"
  hevc_nvenc:
    label: "NVENC HEVC"

# Display profiles — governs which target_models are valid.
display_profiles:
  sdr:
    label: "SDR (BT.709)"
    allowed_models: [vmaf_v0_6_1]
  hdr_pq:
    label: "HDR PQ (BT.2100)"
    allowed_models: [vmaf_hdr_v1]

# Tuning presets — how the operator is encoding.
tuning_presets:
  default:
    label: "Default (no tuning)"
  film_grain:
    label: "Film-grain denoising (--tune grain)"
  animation:
    label: "Animation (--tune animation)"

# Fork-local predictors whose marginal value is under evaluation.
# Each entry names a column that must be present in the corpus or
# can be computed from corpus columns via the named adapter.
fork_predictors:
  vmaf_tiny_v2:
    type: onnx
    path: "model/tiny/vmaf_tiny_v2.onnx"
    input_features: [adm2, vif_scale0, vif_scale1, vif_scale2, vif_scale3, motion2]
    output_column: vmaf_tiny_v2_pred
  fr_regressor_v2:
    type: onnx
    path: "model/tiny/fr_regressor_v2.onnx"
    input_features: [adm2, vif_scale0, vif_scale1, vif_scale2, vif_scale3, motion2]
    output_column: fr_regressor_v2_pred

# Gate thresholds — per-metric minimum deltas for a predictor to be
# considered "adding value". Configurable per deployment.
value_gates:
  default:
    plcc_delta_min: 0.02
    srocc_delta_min: 0.02
    rmse_delta_max_factor: 0.95    # must improve RMSE by ≥5 %

# Cell exclusion rules — combinatorial cells to skip.
# Rules are evaluated as Python expressions; 'model', 'corpus',
# 'codec', 'display', 'tuning' are in scope.
exclusions:
  - rule: "model.display != corpus.color_space"
    reason: "HDR model requires HDR corpus and vice versa"
  - rule: "model.pending and corpus.pending"
    reason: "Both sides pending; skip until at least one materialises"
  - rule: "codec != 'any' and corpus.type != 'parquet'"
    reason: "Codec conditioning requires parquet with codec column"
```

### 2.3 Cell declaration

A cell is the cross-product of one entry from each of the five dimensions,
filtered by exclusion rules. The Python dataclass:

```python
@dataclasses.dataclass(frozen=True)
class EvalCell:
    model: str           # key in target_models
    corpus: str          # key in corpora
    codec: str           # key in codecs ("any" for corpus-wide)
    display: str         # key in display_profiles
    tuning: str          # key in tuning_presets
    cell_id: str         # "{model}__{corpus}__{codec}__{display}__{tuning}"
```

The CLI's `--cell` flag accepts a glob pattern against `cell_id`, enabling
targeted re-evaluation (e.g. `--cell '*hdr*'` to evaluate only HDR cells).

---

## 3. Feature Subset Search Strategy

### 3.1 Chosen strategy: greedy forward selection with SHAP verification

**Primary**: Greedy forward selection (GFS) starting from an empty feature set,
adding one feature per round that maximally improves the LOSO-CV PLCC.
Termination: PLCC gain < `min_delta = 0.002` or feature count exceeds
`max_features = 12` (configurable).

**Post-selection SHAP pass** (Phase 2): Compute SHAP values for the final
feature set. Features with mean absolute SHAP < 5 % of the top feature's SHAP
are flagged as pruning candidates. Advisory only; GFS selection is authoritative.

**Extraction-cost weighting**: Each feature declares a nominal cost in ms/frame.
When two features produce identical PLCC within `tol = 0.001`, the cheaper wins
the GFS round. This prevents `speed_temporal` displacing `motion2` on marginal
accuracy grounds.

**Strategy trade-off table**:

| Strategy | Verdict |
| --- | --- |
| GFS (chosen) | O(K²); auditable; no hyperparameter; best fit for K = 6–20, 10²–10⁴ cells |
| LASSO/ElasticNet | Inner-CV for λ; per-fold coefficient instability under LOSO — implement as Phase 3 cross-check |
| Mutual information ranking | Ranks features independently; misses collinearity; useful for pre-screening, not selection |
| Exhaustive search | O(2^K) — infeasible for K > 20 |

### 3.2 Probe model

GFS scores candidate sets using **ridge regression** (`alpha = 1.0`): fits in
milliseconds, cannot overfit on 6–20 features × thousands of rows, and its
PLCC is monotone in feature informativeness for well-scaled inputs. After GFS
selects the feature set, the full tiny-AI MLP is fit on that set for the final
PLCC/SROCC/RMSE numbers. Ridge selects; MLP measures.

---

## 4. Cross-Validation and Metrics Methodology

### 4.1 Primary CV regime: LOSO

**Leave-one-source-out (LOSO)** is the primary CV regime for corpora where a
`source` column identifies independent content (Netflix public: 9 sources;
CHUG: content IDs; BVI-DVC: scene IDs). LOSO is the fork's established
convention (see `docs/ai/loso-eval.md` and `ai/scripts/eval_loso_vmaf_tiny_v4.py`)
and answers the most honest question: does the predictor generalise to content
it has never seen?

For corpora without a meaningful source split (KoNViD-1k, LIVE-VQC, YouTube-UGC),
fall back to **stratified 5-fold CV** with the global seed for reproducibility.

### 4.2 Secondary metrics

Each cell reports:

| Metric | Symbol | Definition |
| --- | --- | --- |
| Pearson correlation | PLCC | Standard `corrcoef` on predicted vs target |
| Spearman correlation | SROCC | Rank-order Pearson |
| Root mean squared error | RMSE | On the VMAF 0–100 scale |
| Marginal PLCC delta | ΔPLCC | `PLCC(predictor) − PLCC(SVM_baseline)` |
| Marginal SROCC delta | ΔSROCC | Corresponding |
| 95 % CI on ΔPLCC | `[lo, hi]` | Bootstrap (B=1000) over LOSO folds |

The **baseline** for every marginal metric is the Netflix SVM score: the
`vmaf_v0.6.1` (or `vmaf_hdr_v*`) column already present in the corpus parquet.
No SVM re-fitting is needed; the SVM predictions are pre-computed columns.

### 4.3 Bootstrap confidence intervals

After LOSO completes and produces per-fold `(PLCC_predictor, PLCC_baseline)`,
bootstrap resampling (B = 1000, seeded by the global `seed`) over the fold
list produces the 95 % CI on `ΔPLCC`. This is narrow (9 folds = 9 bootstrap
samples), so for small-source corpora the CI is wide by design — reflecting
genuine uncertainty, not a pipeline bug.

For corpora using k-fold CV, bootstrap resamples the k fold scores. Results
store all B resampled deltas so the threshold gate can be re-evaluated at any
alpha without re-running.

### 4.4 Pass/fail gate

A cell passes if both:

```text
ΔPLCC_mean ≥ gate.plcc_delta_min  AND
CI_lo(ΔPLCC) > 0                   AND   # lower bound positive
ΔSROCC_mean ≥ gate.srocc_delta_min
```

`CI_lo(ΔPLCC) > 0` is the key gate: if the lower bound of the 95 % CI dips
below zero, the marginal gain is statistically indistinguishable from zero on
the available data, and the predictor should not be claimed as adding value for
that cell.

Gate thresholds are declared in the YAML per deployment context (default: PLCC
delta ≥ 0.02, SROCC delta ≥ 0.02 per the brief).

---

## 5. Storage

### 5.1 Per-cell result format

Results are stored as **DuckDB** (file at `runs/predictor_bench/results.duckdb`).
The schema:

```sql
CREATE TABLE cell_results (
    cell_id       VARCHAR NOT NULL,
    model         VARCHAR,
    corpus        VARCHAR,
    codec         VARCHAR,
    display       VARCHAR,
    tuning        VARCHAR,
    run_ts        TIMESTAMP,   -- UTC
    git_sha       VARCHAR,     -- HEAD at evaluation time
    grid_version  VARCHAR,     -- hash of predictor_bench.yaml
    feature_set   JSON,        -- ordered list of selected features
    plcc          DOUBLE,
    srocc         DOUBLE,
    rmse          DOUBLE,
    delta_plcc    DOUBLE,
    delta_srocc   DOUBLE,
    ci_lo_plcc    DOUBLE,
    ci_hi_plcc    DOUBLE,
    n_folds       INTEGER,
    gate_pass     BOOLEAN,
    gate_config   JSON,
    bootstrap_deltas JSON,     -- B=1000 raw bootstrap samples
    shap_summary  JSON,        -- {feature: mean_abs_shap}
    PRIMARY KEY (cell_id, run_ts)
);
```

**Why DuckDB**: Zero server infrastructure, single-file store, native Parquet
bridge, fast analytical SQL. "Show all cells where tiny-AI added < 0.01 PLCC":

```sql
SELECT cell_id, delta_plcc FROM cell_results
WHERE delta_plcc < 0.01 ORDER BY delta_plcc;
```

Alternatives: SQLite (no native Parquet bridge; slower GROUP BY), per-cell
JSON (no aggregate query), remote Postgres (infrastructure overkill for 10⁴
cells). DuckDB wins on all axes for this scale.

The DuckDB file is **gitignored** (`runs/` is already gitignored). CI publishes
it as a workflow artifact. The Markdown report (§8) is the committed artifact.

---

## 6. Compute Orchestration

### 6.1 Cell execution model

Per-cell evaluation is:

1. Load the corpus columns for the cell's (model, codec, corpus) into memory.
2. Run LOSO or k-fold GFS probe. Each GFS round fits one ridge regression
   per candidate feature (O(K²) fits).
3. Fit the final MLP on the selected feature set, evaluate LOSO, bootstrap.
4. Write one row to DuckDB.

Wall time per cell: seconds to ~2 minutes depending on corpus size and feature
count. A full grid of 10³ cells completes in under an hour on a single 8-core
host with no GPU (all computation is NumPy/scikit-learn on CPU).

### 6.2 Local parallel execution

The CLI's `--workers N` flag controls `concurrent.futures.ProcessPoolExecutor`
parallelism. The default is `min(os.cpu_count(), 8)` to leave headroom for
other processes. Each worker receives a single `EvalCell` and writes its result
row independently (DuckDB's WAL supports concurrent writers).

### 6.3 CI trigger strategy

Two modes:

**On-demand** (Phase 1–2): a manual GitHub Actions workflow dispatch
(`workflow_dispatch`) accepting optional `--cell` glob and `--corpus` filter.
No automatic trigger. The operator runs it when feature sets, models, or codec
adapters change.

**Scheduled + PR-triggered** (Phase 3): a nightly cron (`schedule: cron:
'0 3 * * *'`) evaluates the full grid against pre-extracted corpora. A
PR-triggered gate evaluates only the cells whose YAML keys were modified in the
PR diff. The gate fails if any previously-passing cell now fails the value gate.

The decision to defer automatic PR gating to Phase 3 is deliberate: Phase 1
needs a stable YAML schema and a validated corpus pipeline before gating
merges on it.

### 6.4 GitHub Actions matrix

The per-cell parallelism is expressed as a matrix job only in Phase 3. Phase 1
uses a single job with `--workers` parallelism. A full 10³-cell grid is well
within GitHub Actions' job limits (256 max) if decomposed as `--cell` glob
batches, but this complexity is premature for Phase 1.

---

## 7. Trigger Conditions

| Event | Phase 1 | Phase 2 | Phase 3 |
| --- | --- | --- | --- |
| Manual dispatch | Yes | Yes | Yes |
| PR touches `predictor_bench.yaml` | No | Yes (run changed cells) | Yes |
| PR touches `model/tiny/*.onnx` or `model/*.json` | No | Yes | Yes |
| PR touches `ai/data/feature_extractor.py` | No | Yes | Yes |
| PR touches any `codec_adapters/*.py` | No | No | Yes |
| Nightly cron (full grid) | No | No | Yes |
| CHUG/BVI-DVC corpus re-extract completes | Manual prompt | Manual prompt | Automated via corpus-ready event |

---

## 8. Visualization and Report

The `report` subcommand generates a Markdown document from the DuckDB results.
It is committed to `docs/ai/predictor-bench-report.md` (overwritten) on every
scheduled run and on manual runs in CI.

**Report structure**:

```markdown
# Predictor-Bench Report — <date> (<git-sha>)

## Executive summary

- N cells evaluated; M cells gate-passing
- Top-performing tiny-AI: <model> (mean ΔPLCC = X.XX)
- Cells with negative marginal value: list

## Per-model leaderboard

Table: model | mean_delta_plcc | mean_delta_srocc | n_cells_pass/total

## Per-corpus breakdown

Table: corpus | model | selected_features | PLCC | SROCC | RMSE | ΔPLCC | gate

## Per-codec heatmap (ASCII or Markdown table)

Rows: codecs. Columns: models. Cell: ΔPLCC (colored green/red)

## Feature selection stability

For each model × corpus, the GFS-selected feature set.
Highlights features that appear in ≥80 % of cells vs <20 % of cells.

## Confidence intervals

Table of cells where CI_lo(ΔPLCC) < 0 (marginal gain not significant).

## Open regressions

Cells that passed in the previous run but fail in the current run.
```

The report is generated by the `predictor-bench report` subcommand and is
integrated into the mkdocs documentation tree under `docs/ai/`.

---

## 9. CLI Surface

### 9.1 Naming decision

The pipeline lives in **`tools/vmaf-tune/`** as a new `predictor-bench`
subcommand of `vmaf-tune`. This avoids a new top-level package, reuses the
existing CLI dispatch infrastructure, and keeps the workflow discoverable
alongside the other `vmaf-tune` operator tools.

Rejected alternative: a new `tools/vmaf-eval/` package. It would fragment the
operator surface (two commands to install, two entry points to document) without
providing different capabilities.

**Subcommand surface**:

```bash
vmaf-tune predictor-bench run   [OPTIONS]   # evaluate cells, write to DuckDB
vmaf-tune predictor-bench report [OPTIONS]  # render Markdown from DuckDB
vmaf-tune predictor-bench show  [OPTIONS]   # print cell results as table
vmaf-tune predictor-bench diff  [OPTIONS]   # compare two run timestamps
```

### 9.2 `run` flags

```text
--grid PATH         Path to predictor_bench.yaml (default: predictor_bench.yaml)
--cell GLOB         Filter cells by cell_id glob (default: all non-pending)
--corpus KEYS       Comma-separated corpus keys to include
--model KEYS        Comma-separated model keys
--codec KEYS        Comma-separated codec keys
--workers N         Parallel workers (default: min(cpu_count, 8))
--db PATH           DuckDB file path (default: runs/predictor_bench/results.duckdb)
--cv {loso,kfold}   CV override (default: corpus-driven)
--folds N           k for k-fold when --cv=kfold (default: 5)
--bootstrap-n N     Bootstrap samples for CI (default: 1000)
--seed N            Global RNG seed (default: from grid YAML)
--dry-run           Print cells that would run; do not evaluate
--force             Re-evaluate cells even if a run_ts already exists for today
--max-features N    GFS maximum feature set size (default: 12)
--min-delta F       GFS termination threshold (default: 0.002)
```

### 9.3 `report` flags

```text
--db PATH           DuckDB to read from
--run-ts TS         Report for a specific timestamp (default: latest)
--out PATH          Output Markdown file (default: stdout)
--compare-ts TS     Add regression column vs a previous timestamp
```

---

## 10. Integration with Existing Artifacts

### 10.1 Corpus JSONLs and Parquet

The pipeline treats any parquet file with the expected feature columns as an
eligible corpus. The YAML `corpora[*].path` may point to:

- A Parquet produced by `ai/scripts/chug_extract_features.py` or
  `extract_full_features.py`.
- A JSONL corpus from `vmaftune.corpus` (loaded via pandas; the pipeline
  auto-converts to an in-memory DataFrame).

No corpus re-extraction is triggered. Missing feature columns are logged as
warnings; cells requiring those columns are skipped.

### 10.2 Tiny-AI model cards

The `fork_predictors` YAML section references the same ONNX paths as the
model cards under `docs/ai/models/`. The pipeline's `run` command reads the
registry (`model/tiny/registry.json`) to cross-check that the ONNX SHA-256
matches the card's pin before evaluation. A mismatch raises a hard error.

### 10.3 Saliency and MOS-head pickles

The `konvid_mos_head_v1` and `saliency_student_v*` models are registered as
`fork_predictors` with `type: onnx`. Their outputs are pre-computed columns in
the corpus parquet; the pipeline does not re-run inference. If the output column
is absent, the cell is skipped with a logged warning.

### 10.4 Netflix SVM baseline

The SVM score column (`vmaf_v0.6.1` or the HDR equivalent) must be a
pre-computed column in the corpus. The pipeline does not fit or invoke the SVM;
it reads the column directly. This matches how `eval_loso_vmaf_tiny_v4.py`
already uses the parquet.

---

## 11. Phasing Plan

### Phase 1 — MVP (estimated 5–8 engineer-days)

**Deliverables**:

- `predictor_bench.yaml` schema definition + Python parser + dataclasses
- Cell enumeration with exclusion rules
- GFS with ridge probe model (scikit-learn)
- LOSO and k-fold CV
- Bootstrap CI computation
- DuckDB write path
- `predictor-bench run` subcommand wired into `vmaf-tune` CLI
- `predictor-bench report` (ASCII tables, no mkdocs integration)
- Unit tests covering grid parsing, GFS, CI, gate evaluation
- Smoke test: single cell on the 9-source Netflix parquet

**Not in Phase 1**: SHAP, GitHub Actions integration, scheduled runs, per-codec
heatmaps, the `diff` subcommand.

### Phase 2 — Polished (estimated 4–6 engineer-days)

**Deliverables**:

- SHAP post-selection verification (via `shap` library, optional dependency)
- `predictor-bench diff` subcommand for regression detection
- mkdocs integration: `docs/ai/predictor-bench-report.md` generated and
  committed on each run
- Manual GitHub Actions `workflow_dispatch` trigger with `--cell` and
  `--corpus` parameters
- CHUG-HDR and `vmaf_hdr_v1` cells enabled once data materialises
- Extraction-cost weighting in GFS (feature cost table in YAML)
- `predictor-bench show` subcommand with DuckDB SQL passthrough

### Phase 3 — CI-integrated (estimated 5–7 engineer-days)

**Deliverables**:

- Nightly cron GitHub Actions workflow (`predictor-bench-nightly.yml`)
- PR-triggered gate: run changed cells when `predictor_bench.yaml` or
  `model/tiny/*.onnx` changes
- Regression alert: PR comment when a previously-passing cell fails the gate
- LASSO cross-check alongside GFS for the feature selection stability report
- Per-GPU matrix job decomposition for corpora requiring re-extraction
- `predictor-bench report --compare-ts` regression column in the Markdown

**Total estimated effort**: 14–21 engineer-days across three phases.

---

## 12. Open Questions

1. **HDR model feature set**: Netflix has not published the HDR model's exact
   input features. The YAML stub assumes `speed_chroma + speed_temporal` plus
   the canonical-6 based on upstream branch signals (ADR-0559). If the shipped
   model uses a different set, the grid YAML needs updating before HDR cells
   can be evaluated. *Unblocked by the upstream-feature-additions agent
   (a8472e67e6286c976).*

2. **Feature-coverage audit dependency**: The feature-coverage audit agent
   (a22b44be9dedc00d4) is cataloguing which features each model consumes. If
   its output lands first, the `target_models[*].feature_columns` entries in
   the YAML should be populated from that catalog rather than the stubs above.

3. **Codec conditioning data availability**: The per-codec cells (all non-`any`
   codecs) require the corpus parquet to include a `codec` column identifying
   which encoder produced each row. The Netflix public parquet does not have
   this column. Either the Phase A JSONL corpus (which does have `encoder`)
   must be the source for codec-conditioned cells, or the parquet extraction
   pipeline must be extended to include the codec provenance. This is a data
   availability gate, not a pipeline design gate.

4. **SHAP optional dependency policy**: The fork's op-allowlist CI does not
   include `shap`. If SHAP is added as an optional dependency for Phase 2,
   it should be declared in a new extras group (`predictor-bench[shap]`) and
   its absence should degrade gracefully (skip SHAP step, log a warning).

5. **DuckDB version pinning**: DuckDB's API has broken backwards compatibility
   between minor versions. The `pyproject.toml` for `vmaf-tune` should pin
   `duckdb >= 0.10, < 2` and the dev container should match. This is a
   follow-up for Phase 1 implementation.

6. **Gate threshold calibration**: The default `plcc_delta_min = 0.02` and
   `srocc_delta_min = 0.02` thresholds were specified in the task brief as the
   "operator-meaningful" bar. These are not derived from empirical data on the
   current corpus. Phase 2 should include a calibration exercise that reports
   the empirical distribution of ΔPLCC across all cells so operators can
   choose thresholds relative to that distribution.

7. **Corpus re-extraction triggering**: Phase 3's `corpus-ready event` trigger
   is described as "automated" but the mechanism (webhook, cron check for
   file modification time, CI artifact publication event) is not specified.
   This is a Phase 3 design question deferred until Phase 2 is operational.

8. **Predictor vs. predictor-bench naming**: The `predictor.py` module in
   `vmaftune` is the per-shot encode predictor (CRF picker). The new
   `predictor-bench` subcommand evaluates the accuracy of all predictors.
   These are different things with similar names. The subcommand name
   `predictor-bench` is chosen to emphasise the "bench" (benchmark) role, but
   `feature-mix` or `eval-feature-mix` are alternatives if the similarity
   causes confusion. This is a naming decision left for review.
