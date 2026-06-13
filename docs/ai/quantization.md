# Tiny-AI int8 quantisation

The fork supports three post-training quantisation (PTQ) modes for
shipped tiny-AI ONNX models, plus quantisation-aware training (QAT).
Each model carries its quant decision in `model/tiny/registry.json`
and an accuracy budget that the CI harness enforces against the fp32
baseline.

Audited and scaffolded in
[ADR-0173](../adr/0173-ptq-int8-audit-impl.md); policy origin
[ADR-0129](../adr/0129-tinyai-ptq-quantization.md).

## Per-model registry fields

| Field                          | Type                                                | Default | Required when                          |
|--------------------------------|-----------------------------------------------------|---------|----------------------------------------|
| `quant_mode`                   | `fp32` / `dynamic` / `static` / `qat`               | `fp32`  | always present (default fp32)          |
| `quant_calibration_set`        | path (relative to repo root)                        | absent  | `quant_mode == "static"`               |
| `quant_accuracy_budget_plcc`   | number in `[0, 1]`                                  | `0.01`  | always (the CI gate honours per-entry) |

`fp32` keeps the loader on the `<basename>.onnx` file. The other three
modes redirect the loader to a sibling `<basename>.int8.onnx`
produced by the scripts below; the fp32 file stays on disk as the
regression baseline.

## Mode selection

| Mode | Accuracy | Cost to produce | Best for |
| --- | --- | --- | --- |
| `fp32` | reference | none | new models, debug builds |
| `dynamic` | small accuracy hit (~0.5%) | one CLI call | models without a calibration set; deployment box differs from training box |
| `static` | small accuracy hit (~0.2%) | one calibration pass | models we own + control + can pin a calibration set |
| `qat` | reference (within ~0.05%) | extra training phase, ~1.5× fp32 train time | models where static drops accuracy past the per-model budget |

Pick the cheapest mode that stays inside the
`quant_accuracy_budget_plcc` budget.

## Producing int8 artefacts

### Dynamic PTQ

```bash
python ai/scripts/ptq_dynamic.py model/tiny/nr_metric_v1.onnx \
    --report-out runs/nr_metric_v1_dynamic_ptq.json
# -> model/tiny/nr_metric_v1.int8.onnx
```

No calibration data needed. Wraps `onnxruntime.quantization.quantize_dynamic`.
When `--report-out` is supplied, the JSON records the fp32/int8 byte sizes,
per-channel setting, output path, and ADR-0661 `run_provenance`.

### Static PTQ

Build a calibration `.npz` first — one entry per ONNX input name, each
a stack of `[N, ...]` representative samples. Then:

```bash
python ai/scripts/ptq_static.py model/tiny/nr_metric_v1.onnx \
    --calibration ai/calibration/nr_metric_v1.npz \
    --report-out runs/nr_metric_v1_static_ptq.json
```

The output goes to `<input>.int8.onnx`. Add the calibration path to
the registry's `quant_calibration_set` field. The optional report includes the
calibration input names/sample count, size ratio, and `run_provenance`.

### Quantisation-aware training (QAT)

```bash
python ai/scripts/qat_train.py \
    --config ai/configs/learned_filter_v1_qat.yaml \
    --output model/tiny/learned_filter_v1.int8.onnx \
    --report-out runs/learned_filter_v1_qat.json
```

QAT is the third quant tier — pick it when static PTQ exceeds the
per-model `quant_accuracy_budget_plcc` budget, or when the
QAT-vs-static delta on real content justifies the ~50 % extra
training-time cost (Research-0006 §4). On tiny models with few
layers (~10 K parameters and below) QAT and static-PTQ tend to
agree to inside the 0.002 budget — pick static-PTQ for cost. On
larger architectures with wider weight distributions QAT typically
wins; the empirical delta is captured per-model in each model's
ADR (e.g. [ADR-0208](../adr/0208-learned-filter-v1-qat-impl.md)).

**Pipeline.** Per [ADR-0207](../adr/0207-tinyai-qat-design.md) the
QAT pass runs in three phases: (1) fp32 warm-start training,
(2) FX fake-quant insertion via
`torch.ao.quantization.quantize_fx.prepare_qat_fx` with the default
symmetric per-tensor activation + per-channel weight qconfig, (3)
QAT fine-tune at 10× reduced learning rate (defaulting to
`fp32_lr / 10`). Phase 4 — ONNX export — bridges
PyTorch 2.11's two broken ONNX exporters by copying the
QAT-conditioned weights back into a fresh fp32 module, exporting the
fp32 graph, then running `onnxruntime.quantization.quantize_static`
with a calibration set drawn from the QAT training distribution.
The output is a QDQ-format `.int8.onnx` bit-identical in structure
to the static-PTQ artefact — the QAT effect is preserved entirely
through weight pre-conditioning.

**CLI knobs.** `--epochs-fp32` (default 20), `--epochs-qat`
(default 10), `--lr-qat` (default fp32-lr / 10), `--n-calibration`
(default 64), `--smoke` (skip both training phases — for CI / dev
round-trip), and `--report-out` (optional JSON with fp32/int8 outputs,
parameter count, phase settings, and `run_provenance`).

**Config.** YAML mirrors the `vmaf-train fit` shape plus a `qat:`
block. See [`ai/configs/learned_filter_v1_qat.yaml`](../../ai/configs/learned_filter_v1_qat.yaml)
for a complete example.

**Trainer API.** `ai.train.qat.run_qat(...)` exposes the same
pipeline for direct Python invocation (used by tests and by future
`vmaf-train qat` subcommand).

## CI accuracy gate (`ai-quant-accuracy`)

Wired into the `Tiny AI (DNN Suite + ai/ Pytests)` job in
[`tests-and-quality-gates.yml`](../../.github/workflows/tests-and-quality-gates.yml)
as of [ADR-0174](../adr/0174-first-model-quantisation.md). The job
calls `ai/scripts/measure_quant_drop.py --all`, which walks the
registry, runs each non-`fp32` model through fp32 + int8 ORT
sessions on a deterministic 16-sample synthetic input set
(seed 0), and asserts the aggregate Pearson correlation drop is
below the per-model `quant_accuracy_budget_plcc`. Budget violation
fails the PR.

Run locally with:

```bash
python ai/scripts/measure_quant_drop.py --all \
    --out-json runs/quant_drop_gate.json
```

`--out-json` preserves the per-model gate rows and the same ADR-0661
`run_provenance` block as the producer scripts. Use it for model-card evidence
or when comparing a refreshed int8 sidecar against a previous CI gate.

## Currently quantised models

| Model id | Mode | Size shrink | Measured drop | Budget |
| --- | --- | --- | --- | --- |
| `learned_filter_v1` | dynamic | 2.4× (80 KB → 33 KB) | 0.000117 (PLCC 0.999883) | 0.01 |
| `nr_metric_v1` | dynamic | 2.0× (119 KB → 58 KB) | 0.007674 (PLCC 0.992326) | 0.01 |
| `vmaf_tiny_v3` | dynamic | 0.95× (4 496 B → 4 267 B) | 0.000120 (PLCC 0.999880) | 0.01 |
| `vmaf_tiny_v4` | dynamic | 1.8× (14 046 B → 7 769 B) | 0.000145 (PLCC 0.999855) | 0.01 |

The original `nr_metric_v1` ONNX export tripped ORT's internal
shape inference during `quantize_dynamic` with `Inferred shape and
existing shape differ in dimension 0: (128) vs (1)`. Root cause:
`torch.onnx.export` emitted every initialiser into
`graph.value_info` with static-shape annotations that did not
survive the dynamic batch axis substitution. The exporter
(`ai/src/vmaf_train/models/exports.py`) and the dynamic-PTQ entry
point (`ai/scripts/ptq_dynamic.py`) now strip those duplicates —
same workaround introduced for `vmaf_tiny_v1*.onnx` in PR #174
(T5-3e). Tracked as T5-3d.

`vmaf_tiny_v3` and `vmaf_tiny_v4` joined the dynamic-PTQ family in
[ADR-0275](../adr/0275-vmaf-tiny-v3-v4-ptq.md). Their model cards
carry the per-model reproduction commands and measured PLCC drops:
[`vmaf_tiny_v3`](models/vmaf_tiny_v3.md#quantisation-dynamic-ptq-int8-sidecar-adr-0275)
and
[`vmaf_tiny_v4`](models/vmaf_tiny_v4.md#quantisation-dynamic-ptq-int8-sidecar-adr-0275).

## Per-model PR template

When proposing a model for quantisation:

1. Run `ai/scripts/ptq_<mode>.py` to produce the int8 file.
2. Compute fp32 vs int8 PLCC on the soak fixture.
3. In the PR description: paste the PLCC numbers + the ratio of
   inference time fp32 / int8 on at least one CPU.
4. Update `model/tiny/registry.json`:
   - flip `quant_mode` to the chosen mode,
   - set `quant_accuracy_budget_plcc` (default 0.01 = 1 PLCC point),
   - add `quant_calibration_set` if `static`.
5. Land the int8 ONNX next to the fp32 file.

The reviewer compares the measured drop against the budget. If a
static run misses budget, escalate to QAT in a follow-up PR — don't
relax the budget.

## Caveats

- **All shipped int8 sidecars are currently dynamic PTQ.** Static PTQ
  and QAT stay supported by the harness, but no shipped registry row
  uses `quant_mode: "static"` or `quant_mode: "qat"` yet.
- **Calibration sets are not redistributable** by default. Operators
  build their own from a parquet feature cache (the
  `ai/scripts/build_calibration_set.py` helper is queued — until it
  lands, hand-craft the `.npz`).
- **VNNI / DLBoost** speedup applies only on Intel CPUs Cascade Lake
  and newer; ARMv8.2+ has int8 dot-product. On CPUs without either,
  the int8 path runs slower than fp32 due to QDQ overhead. The
  loader is bit-depth-agnostic — it still picks the int8 model when
  the registry says so; runtime perf is the operator's problem to
  measure.
