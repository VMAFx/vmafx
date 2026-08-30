<!-- markdownlint-disable MD060 -->
# ADR-0661: AI run manifest provenance

- **Status**: Accepted
- **Date**: 2026-05-20
- **Deciders**: Lusoris maintainers
- **Tags**: ai, tooling, manifests, training

## Context

The fork is refreshing many AI-derived artifacts at once: Netflix-derived
regressors, KonViD/CHUG MOS heads, saliency and second-opinion materializers,
and codec-profile experiments. Several scripts already emit model sidecars or
evaluation reports, but the run identity is inconsistent: a CHUG-facing command
can delegate to the shared KonViD trainer, input paths may be local-only, and
CLI arguments are not recorded in a shared shape.

We need enough manifest provenance to reproduce a local training run or promote
its output into a model card without turning gitignored local corpus paths into
a CI contract.

## Decision

We will add a shared `aiutils.run_manifest` helper and have AI trainers emit a
`run_provenance` block in their JSON sidecars. The block records a schema name,
user-facing entrypoint script with SHA-256, normalized CLI arguments, named
input/output paths, and file hashes where the path exists. CHUG runs keep
`train_chug_hdr_mos_head.py` as the entrypoint while also recording the shared
trainer implementation that wrote the sidecar. FR-regressor trainers
(`fr_regressor_v1`, `fr_regressor_v2`, and `fr_regressor_v3`) use the same block
for their model sidecars; v1/v2 also copy it into their metrics JSON so a
gate-failed run can still be traced. The `vmaf_tiny_v2`, `vmaf_tiny_v3`, and
`vmaf_tiny_v4` exporters use the same block for their ONNX sidecars so exported
artifacts identify the checkpoint input and output targets. The
KoNViD C2/C3 exporter (`export_tiny_models.py`), FastDVDnet pre-filter
exporters (`export_fastdvdnet_pre.py` and its placeholder variant), and TransNet
V2 exporters (`export_transnet_v2.py` and its placeholder variant) also record
the same block in model sidecars so DNN feature-model refreshes identify the
checkpoint/upstream-weight inputs, parsed exporter arguments, ONNX output,
sidecar output, and registry target. The `export_ensemble_v2_seeds.py`
production seed exporter also uses the same block
for per-seed sidecars so a seed refresh records the corpus, PROMOTE verdict,
argv, per-seed output targets, and optional registry target. Tiny-VMAF evaluation
reports (`eval_loso_vmaf_tiny_v3.py`, `eval_loso_vmaf_tiny_v4.py`,
`eval_loso_vmaf_tiny_v5.py`, and `eval_multiseed_v3_v4.py`) also use the shared
block so refreshed LOSO and multi-seed reports identify the feature table,
hyperparameters, argv, and output report path. The ensemble production-flip
validator (`validate_ensemble_seeds.py`) uses the same block for
`PROMOTE.json` / `HOLD.json` verdicts so registry-flip evidence identifies the
LOSO directory, corpus root, seed list, gate thresholds, and verdict target.
The registry and saliency validation helpers (`validate_model_registry.py
--out-json` and `validate_saliency_student.py --out-json`) also use the same
block so CI/release evidence for registry consistency and saliency ONNX
allowlist/parity checks records the input files, argv, verdicts, and report
target.
The tiny-VMAF smoke validators (`validate_vmaf_tiny_v2.py`,
`validate_vmaf_tiny_v3.py`, and `validate_vmaf_tiny_v4.py`) record the same
block when `--out-json` is passed so promotion gate evidence identifies the
validated ONNX, feature parquet, optional comparison model(s), argv, gate
threshold, and report target.
The ensemble LOSO trainer (`train_fr_regressor_v2_ensemble_loso.py`) records
the same block in each `loso_seed{N}.json` report so the per-seed gate inputs
identify the corpus JSONL, training hyperparameters, argv, and report target
before the validator aggregates them.
The direct deep-ensemble trainer (`train_fr_regressor_v2_ensemble.py`) records
the same block in `fr_regressor_v2_ensemble_v1.json` so smoke or production
manifest refreshes identify the optional corpus parquet, member ONNX outputs,
registry target, argv, and manifest target.
The `vmaf-train` CLI (`ai/src/vmaf_train/cli.py`) records the same block in
durable `--json` reports for `validate-norm`, `profile`,
`audit-learned-filter`, `quantize-int8`, `cross-backend`, and
`bisect-model-quality`, including the model/feature/calibration inputs,
parsed thresholds, JSON target, and generated model output where applicable.
Feature-analysis reports (`ai/scripts/feature_correlation.py`) also use the
same block so correlation / mutual-information / feature-importance reports
identify the source parquet, target column, thresholds, argv, and report target.
Phase-3 subset-sweep reports (`ai/scripts/phase3_subset_sweep.py`) use the same
block so model-selection sweeps identify the source parquet, subset list, seed
policy, standardization flag, training hyperparameters, argv, and report target.
Per-EP quantisation reports (`ai/scripts/measure_quant_drop_per_ep.py`) use the
same block so CPU/CUDA/OpenVINO PTQ investigations identify the tiny-model
registry, optional fp32 baselines, selected execution providers, hardware tag,
argv, and both JSON and Markdown report targets.
Quantisation producer/gate scripts (`ptq_dynamic.py --report-out`,
`ptq_static.py --report-out`, `qat_train.py --report-out`, and
`measure_quant_drop.py --out-json`) use the same block so int8 model-card
evidence identifies the fp32/int8 model paths, calibration/config inputs,
size/gate statistics, argv, and report target.
Phase F recipe calibration (`ai/scripts/calibrate_phase_f_recipes.py`) uses the
same block so regenerated `vmaf-tune auto` content-recipe JSON identifies the
source corpus JSONL, row cap, argv, and calibrated recipe output target.
NR threshold calibration (`ai/scripts/calibrate_nr_threshold.py`) uses the same
block so regenerated `nr_metric_v1.json` calibration thresholds identify the
requested and actual corpus directories, `nr_metric_v1.onnx`, CRF grid, argv,
model JSON output, and Markdown report target.
Legacy evaluation reports (`eval_loso_mlp_small.py`, `eval_loso_3arch.py`,
`eval_probabilistic_proxy.py`, and `eval_saliency_per_mb.py`) also adopt the
same schema when they emit durable JSON so old model-card evidence and
saliency/probabilistic probes do not drift from the refreshed report contract.
The predictor-v2 real-corpus trainer
(`ai/scripts/train_predictor_v2_realcorpus.py`) uses the same report-level
block for `runs/predictor_v2_realcorpus/report.json`, including diagnostic
`--allow-empty` runs, so gate-failed per-codec predictor evidence records the
corpus roots, resolved corpus files, argv, and report target.
The `vmaf_tiny_v2`, `vmaf_tiny_v3`, `vmaf_tiny_v4`, and deferred
`vmaf_tiny_v5` training scripts record the same block in their `--out-stats`
JSON files so exporter inputs can be traced to the parquet table(s),
checkpoint target, stats target, argv, and hyperparameters that produced them.
The saliency-student trainers (`train_saliency_student.py` and
`train_saliency_student_v2.py`) record the same block in their `--metrics-out`
JSON files so DUTS-rooted saliency refreshes identify the training corpus
root, ONNX output, metrics output, argv, and hyperparameters that produced
the model-card evidence.
Table-side materializers and audits (`materialize_mos_labels.py`,
`materialize_second_opinion_features.py`, `materialize_saliency_features.py`,
and `signal_mix_audit.py`) use the same block for their audit/report JSON
outputs so refreshed feature-table evidence records the source tables, joined
label/score inputs, report thresholds, output targets, and argv.
CHUG feature extraction (`chug_extract_features.py`) uses the same block in its
local split manifest and HDR metadata audit JSON so HDR MOS training evidence
records the source CHUG JSONL, clip/cache directories, VMAF binary, split/audit
targets, argv, and extraction arguments before model training starts.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep one-off manifest JSON in each trainer | Minimal diff; no helper import | Repeats path hashing and argument normalization; CHUG/KonViD identity can drift again | The active backlog is explicitly about consolidating AI script-family plumbing |
| Store full environment snapshots | Captures more state | Leaks noisy host details, grows sidecars, and makes local-only runs look like reproducibility guarantees | The useful contract is input/output/config provenance, not a full machine image |
| Require a single config file for every run before adding provenance | Cleaner long-term config story | Blocks current HDR/CHUG refresh work and does not help existing command-line runs | Provenance is incremental and can later point at config files when those land |
| Leave table materializers out of ADR-0661 | Smaller scope | Recreates the exact blind spot that made refreshed MOS/saliency/second-opinion tables hard to audit | Materialized feature tables are durable AI inputs, so their audit JSON belongs in the same provenance family |
| Leave ensemble seed export sidecars as legacy JSON | No model-file delta unless seeds are refreshed | Fresh production seed sidecars would still lack corpus/verdict/argv lineage | Rejected because the exporter is the promotion boundary from gate evidence to shipped ONNXs |
| Leave ensemble LOSO reports as legacy JSON | Smaller trainer diff | Validator verdicts would carry provenance, but their source `loso_seed{N}.json` files would still be opaque | Rejected because seed reports are the durable gate inputs and often outlive the validator run |
| Leave registry and saliency validators stdout-only | No output-schema addition | Release/model-card evidence would still depend on CI scrollback for registry consistency and saliency ONNX parity | Rejected because validator reports are the durable proof that a model artifact is promotable |
| Leave the direct ensemble manifest as legacy JSON | Avoids touching an older smoke/production trainer | `fr_regressor_v2_ensemble_v1.json` would identify members but not the command, corpus, registry target, or member outputs that produced it | Rejected because the manifest is the top-level runtime entry point for the ensemble |
| Leave `vmaf-train --json` reports as plain JSON | No CLI helper diff | Model-card evidence from the user-facing CLI still loses input/threshold lineage | Rejected because these reports are the operator-facing promotion/audit artifacts |
| Leave feature-correlation reports as plain JSON | Smallest diff | Signal-mix audits would keep metrics but lose the exact source parquet and ranking parameters | Rejected because the feature-correlation report is durable analysis evidence, not a transient debug print |
| Leave Phase-3 subset sweeps as plain JSON | No schema delta | Model-selection sweeps would keep PLCC tables but lose seed / subset / standardization replay context | Rejected because Phase-3 outputs are durable model-selection evidence |
| Leave per-EP quantisation reports as plain JSON | No change to a gitignored investigation harness | GPU-EP PTQ evidence would keep PLCC tables but lose the registry, hardware tag, EP list, and optional baseline context | Rejected because quantisation reports are copied into model-card and research evidence before int8 models ship |
| Leave PTQ/QAT producer reports as terminal-only | No extra report file for local one-off quantisation | Int8 sidecars would still have no durable fp32/input/calibration/config lineage unless an operator copied shell logs by hand | Rejected because quantisation output is a shipped model artifact boundary |
| Leave Phase F recipe calibration as plain JSON | No runtime loader delta | Calibrated recipe JSON would not identify which corpus snapshot and row cap produced operator-facing `vmaf-tune auto` behaviour | Rejected because recipe JSON is a shipped tuning input, not a scratch report |
| Leave NR threshold calibration as plain JSON | No change to a slow calibration harness | `--fast-nr` would pick up a threshold without recording the corpus, CRF grid, model input, or report path that justified it | Rejected because NR thresholds directly affect user-facing bisect behaviour |
| Leave DNN feature-model exporters as legacy sidecars | Avoids touching older weight-conversion scripts | Fresh C2/C3, FastDVDnet, or TransNet sidecars would record runtime shape but not the checkpoint/upstream inputs or exporter command that produced them | Rejected because those sidecars are the reproducibility boundary for shipped DNN feature models |
| Leave saliency-student metrics as legacy JSON | No change to older DUTS trainers | Fresh v1/v2 metrics would report IoU and ONNX hashes but not the DUTS root, output targets, argv, or training hyperparameters that produced the evidence | Rejected because saliency model cards cite those metrics as durable production evidence |
| Leave tiny-VMAF validator gates as stdout-only | No CLI/output-schema addition | Model-card promotion evidence would keep PLCC/RMSE only in terminal logs with no hashed ONNX/parquet identity | Rejected because validator reports are the shortest replay path for v2/v3/v4 smoke gates |
| Leave CHUG split/audit JSON as plain local files | No CHUG extractor diff | HDR MOS training would know the model inputs but not which extractor command produced the split map and HDR preflight evidence | Rejected because CHUG split/audit files define the train/test boundary and HDR metadata validity before model training starts |

## Consequences

- **Positive**: local MOS-head, FR-regressor, vmaf_tiny export, vmaf_tiny and
  legacy evaluation, saliency/probabilistic probe, and ensemble validation
  artifacts can be traced back to the command, script revision, input files, and
  output targets that produced them.
- **Positive**: predictor-v2 real-corpus gate reports now carry the same
  reproducibility block as the model-card evidence they feed.
- **Positive**: vmaf_tiny training stats now bridge the pre-export gap between
  refreshed parquets and ONNX sidecars.
- **Positive**: MOS label, saliency, second-opinion, and signal-mix audit JSONs
  now preserve the table inputs and report thresholds that produced refreshed
  training evidence.
- **Positive**: fresh ensemble seed sidecars now preserve the PROMOTE verdict
  and corpus identity that justified shipping the exported ONNXs.
- **Positive**: ensemble LOSO seed reports now preserve the exact corpus,
  argv, and training arguments that produced validator gate inputs.
- **Positive**: registry and saliency validation reports now preserve the
  input files, argv, check verdicts, and report target needed for release and
  model-card evidence.
- **Positive**: direct ensemble manifests now preserve the corpus, member-output,
  registry, argv, and manifest context that produced the top-level runtime entry.
- **Positive**: `vmaf-train --json` reports now carry the same reproducibility
  context as the script-family artifacts they complement.
- **Positive**: feature-correlation reports now carry the source parquet and
  ranking-parameter context needed to replay signal-mix audits.
- **Positive**: Phase-3 subset-sweep reports now carry the model-selection
  sweep context needed to replay broader-feature decisions.
- **Positive**: per-EP quantisation reports now carry the hardware and model
  input context needed to replay CPU/CUDA/OpenVINO PTQ findings.
- **Positive**: PTQ/QAT producer and quant-drop gate reports now carry the
  fp32/int8 model, calibration/config, size/gate, argv, and report context
  needed to replay int8 model promotion evidence.
- **Positive**: Phase F recipe calibration JSON now carries the corpus and CLI
  context needed to replay `vmaf-tune auto` content-recipe thresholds.
- **Positive**: NR threshold calibration JSON now carries the corpus, model,
  CRF-grid, and report context needed to replay `--fast-nr` skip thresholds.
- **Positive**: DNN feature-model sidecars now carry the checkpoint or upstream
  weight inputs and exporter arguments needed to replay C2/C3, FastDVDnet, and
  TransNet refreshes.
- **Positive**: saliency-student metrics now carry the DUTS root, ONNX output,
  metrics output, argv, and training arguments needed to replay v1/v2 refreshes.
- **Positive**: tiny-VMAF smoke-validation reports now carry the validated ONNX,
  parquet slice, comparison model, argv, gate threshold, and report target
  needed to replay promotion checks.
- **Positive**: CHUG split manifests and HDR audit JSONs now carry the extractor
  input/output and argv context needed to replay HDR MOS feature-table evidence.
- **Positive**: CHUG manifests stay CHUG-named even though the implementation
  shares the KonViD training loop.
- **Negative**: sidecars become slightly larger and include local path names.
- **Neutral / follow-ups**: remaining `train_` / `export_` / `eval_` /
  `validate_` / materializer script families can adopt the helper as they move
  from ad hoc CLI state toward shared config plumbing.

## References

- Research: [Research-0661](../research/0661-ai-run-manifest-provenance.md)
- Related: [ADR-0658](0658-project-modernization-audit.md)
- Source: req: "well go on i guess we have enough backlog..."
