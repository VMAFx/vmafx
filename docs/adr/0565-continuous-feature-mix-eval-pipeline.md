<!-- markdownlint-disable MD013 MD060 -->
# ADR-0565: Continuous Feature-Mix Evaluation Pipeline (predictor-bench)

- **Status**: Proposed
- **Date**: 2026-05-18
- **Deciders**: lusoris
- **Tags**: ai, vmaf-tune, predictor, eval, corpus, fork-local, ci

## Context

The fork ships multiple prediction surfaces (SVM VMAF, `vmaf_tiny_v2/v3/v4`,
`fr_regressor_v1/v2/v3`, `nr_metric_v1`, `konvid_mos_head_v1`, per-shot
encode predictors) whose accuracy claims are validated at training time but
never re-checked automatically when the feature set, models, codecs, or
corpora change. Several near-term events make this technical debt critical:

- Netflix is expected to release an HDR VMAF model consuming `speed_chroma`
  and `speed_temporal` (per ADR-0559). This adds a new model dimension.
- The CHUG-HDR corpus will arrive soon, requiring evaluation on HDR content.
- Codec adapters evolve; the per-codec `predictor_<codec>.onnx` family and
  the codec-aware `fr_regressor_v2` each need re-validation on adapter changes.
- ADR-0559 added two features to extraction scripts; additional upstream
  additions may follow.

No pipeline currently answers: "does this predictor still add accuracy over the
Netflix SVM baseline for this (model × codec × display × tuning × corpus)
cell, and what is the best feature subset for it?" The result is an accumulating
drift between what the model cards claim and what would be measured on current
data.

## Decision

We will build a **continuous feature-mix evaluation pipeline** named
`predictor-bench`, implemented as a `vmaf-tune predictor-bench` subcommand
family (`run`, `report`, `show`, `diff`). The pipeline:

1. Declares the evaluation grid in a YAML file (`predictor_bench.yaml`) that
   enumerates all (target_model × corpus × codec × display × tuning_preset)
   cells and the exclusion rules that prune invalid combinations.
2. Evaluates each cell by running greedy forward selection (GFS) with a
   ridge-regression probe to select the best-N feature subset, then evaluating
   the selected subset with the full MLP architecture under LOSO or k-fold CV.
3. Computes PLCC, SROCC, RMSE, and their marginal deltas against the Netflix
   SVM baseline, with 95 % bootstrap confidence intervals on the deltas.
4. Stores results in a DuckDB file (`runs/predictor_bench/results.duckdb`)
   and generates a Markdown report (`docs/ai/predictor-bench-report.md`).
5. Evaluates a configurable pass/fail gate (default: ΔPLCC ≥ 0.02, ΔSROCC ≥
   0.02, CI lower bound positive) indicating whether each fork-local predictor
   adds value for each cell.

The pipeline is phased: Phase 1 (MVP, ~5–8 days) delivers local execution and
unit tests; Phase 2 (~4–6 days) adds SHAP verification, mkdocs integration,
and a manual CI trigger; Phase 3 (~5–7 days) adds nightly scheduling, PR-gate
regression detection, and LASSO cross-check.

The full design is in
[`docs/research/continuous-feature-mix-evaluation-design-2026-05-18.md`](../research/continuous-feature-mix-evaluation-design-2026-05-18.md).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| New top-level `tools/vmaf-eval/` package | Clean namespace separation | Second entry point to install, document, and maintain; fragments the operator surface | Rejected — one CLI is better than two |
| Extend `ai/scripts/eval_loso_*.py` ad hoc | No new infrastructure | One script per model; no shared schema, no grid enumeration, no DuckDB aggregation; each new model requires a new script | Rejected — this is exactly the ad-hoc accumulation the pipeline replaces |
| YAML + Makefile matrix | Simple; no Python boilerplate | Makefile cell enumeration is error-prone; no CI diff, no DuckDB, no Markdown report generation | Rejected — insufficient for Phase 3 regression detection |
| Run evaluations per PR only (no nightly) | Lower CI cost | Regressions may not be caught until a PR that coincidentally touches a model file; HDR cells cannot be gated pre-merge | Deferred to Phase 3 as the scheduled trigger; not rejected outright |
| Exhaustive feature search | Globally optimal subset | O(2^K) — infeasible for K > 20 features | Rejected — GFS with SHAP verification is the best accuracy/cost trade-off |

## Consequences

- **Positive**: Every change to the feature set, model registry, or codec
  adapters can be evaluated against all relevant cells in a single command.
  Model cards can cite `predictor-bench` cell IDs for their accuracy claims.
  The DuckDB store enables retrospective queries ("show all cells where tiny-AI
  added < 0.01 PLCC"). The YAML-declared exclusion rules make the HDR/SDR
  boundary explicit and auditable.
- **Negative**: A new dependency (`duckdb`) is introduced to `vmaf-tune`.
  The DuckDB version must be pinned carefully. The GFS + LOSO loop is
  O(K² × n_folds) per cell; for very large corpora (KoNViD-150k) and large
  feature sets this may take minutes per cell. Phase 1 defers the GitHub
  Actions integration, so the first phase is not automatically triggered.
- **Neutral / follow-ups**: The `predictor_bench.yaml` schema version is pinned
  at `"1"`; schema changes must bump it and migrate existing results or mark
  old rows as incompatible. The DuckDB file is gitignored; CI publishes it as
  a workflow artifact. A follow-up ADR is needed for the Phase 3 nightly
  trigger mechanism and the corpus-ready event definition.

## References

- [`docs/research/continuous-feature-mix-evaluation-design-2026-05-18.md`](../research/continuous-feature-mix-evaluation-design-2026-05-18.md) — full design spec.
- [ADR-0559](0559-feature-coverage-audit.md) — feature-coverage audit that
  prompted the HDR model dimension.
- [ADR-0395](0395-predictor-stub-models-policy.md) — predictor stub-models
  policy; the pipeline validates when stubs should be replaced.
- [ADR-0042](0042-tinyai-docs-required-per-pr.md) — per-PR doc bar for
  tiny-AI surfaces; `predictor-bench report` satisfies this for pipeline output.
- [ADR-0249](0249-fr-regressor-v1.md), [ADR-0291](0291-fr-regressor-v2-prod-ship.md),
  [ADR-0323](0323-fr-regressor-v3-train-and-register.md) — fr_regressor
  lineage; their LOSO PLCC numbers are the baseline the pipeline will track.
- [`docs/ai/loso-eval.md`](../ai/loso-eval.md) — existing LOSO convention
  the pipeline formalises.
- Feature-coverage audit agent output (a22b44be9dedc00d4) — populates the
  `target_models[*].feature_columns` entries in the YAML if its PR lands first.
- Upstream-feature-additions agent (a8472e67e6286c976) — enumerates Netflix
  HDR model inputs; informs the `vmaf_hdr_v1` pending cell.
- Source: `req` — per task brief, 2026-05-18.
