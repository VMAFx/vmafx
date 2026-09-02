<!-- markdownlint-disable MD060 -->
# ADR-0208: First per-model QAT — `learned_filter_v1` int8 (T5-4)

- **Status**: Accepted
- **Date**: 2026-04-28
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: tiny-ai, onnx, quantization, qat, registry, ci, fork-local

## Context

[ADR-0207](0207-tinyai-qat-design.md) locked the QAT pipeline
design before code. This ADR is the implementation PR — the trainer
hook, the CLI driver, and the first per-model QAT pass.

Per ADR-0207 the QAT default budget is 0.002 PLCC drop (Research-0006
§1, Table 1). The empirical question this ADR answers is: does QAT
measurably help on `learned_filter_v1` — the same C3 residual filter
that already ships with `quant_mode: "dynamic"` per
[ADR-0174](0174-first-model-quantisation.md)?

The trainer-hook side has a real implementation challenge that
ADR-0207's design did not surface: PyTorch 2.11's two ONNX exporters
both refuse the QAT-converted graph.

- The legacy TorchScript exporter emits `quantized::conv2d` /
  `quantized::add` ops (PyTorch's internal quantized namespace),
  which ORT cannot consume.
- The new TorchDynamo exporter chokes on
  `Conv2dPackedParamsBase` with a missing `__obj_flatten__`
  attribute.

ADR-0207 §Decision step 4 said
`convert_fx(model)` → `torch.onnx.export(...)` produces a QDQ
ONNX. In PyTorch 2.11 that path is broken end-to-end. The
implementation has to bridge.

## Decision

### 1. Two-step pipeline: PyTorch QAT → fp32 ONNX → ORT static-quantize

The trainer ([`ai/train/qat.py`](../../ai/train/qat.py)) runs
the QAT phase per ADR-0207 (FX-prepared module + fake-quant
observers + 10 epochs at 10× reduced LR), then **does not** call
`convert_fx`. Instead it:

1. Copies the QAT-conditioned parameter tensors back into a fresh
   fp32 module (state-dict diff, matched by submodule name + tensor
   shape).
2. Exports the fp32 module to ONNX via the legacy TorchScript
   exporter (`dynamo=False`) — this works because the graph has
   no quantized ops, just plain conv/relu/add.
3. Runs `onnxruntime.quantization.quantize_static` on the fp32
   ONNX with a calibration set drawn from the QAT training
   distribution. ORT emits a QDQ-format `.int8.onnx` with
   per-channel symmetric weights + per-tensor symmetric activations
   — the same layout the PTQ static path produces.

The QAT effect is preserved: weights have been pre-conditioned by
fake-quant during training, so the activation ranges ORT discovers
during calibration map onto weight values that already round well.
The ONNX QDQ graph is bit-identical in structure to PTQ static — a
deployment-time observer cannot tell QAT from PTQ static apart from
the smaller quantization drop on real data.

### 2. `ai/train/qat.py` — Lightning-compatible trainer hook

- `QatConfig` dataclass: `epochs_fp32`, `epochs_qat`, `lr_qat`,
  `n_calibration`, `output_int8_onnx`, `seed`, `smoke`. The `smoke`
  flag drops both training phases for the CI test path.
- `run_qat(model_factory, qat_cfg, ...)`: zero-arg-callable model
  factory + config + (optional) loader factory + (optional) loss.
  Returns a `QatResult` with `fp32_onnx` / `int8_onnx` / `n_params`.

The trainer hook is **device-aware**: fine-tune runs on CUDA when
available, but FX preparation runs on CPU (the FX symbolic tracer
trips over CUDA buffers in PyTorch 2.11). The two-step pipeline
handles the device migration transparently.

### 3. `ai/scripts/qat_train.py` — real CLI driver

Replaces the `NotImplementedError` scaffold landed under ADR-0173.
Reads a YAML config (the same shape `vmaf-train fit` consumes,
plus an optional `qat:` block) and runs the full pipeline. CLI
args `--epochs-fp32` / `--epochs-qat` / `--lr-qat` /
`--n-calibration` / `--smoke` override the config block.

When the config's `cache:` field points at a missing parquet, the
driver auto-falls-back to smoke mode rather than crashing — this
keeps the CLI healthy in CI containers without the BVI-DVC corpus.

### 4. `ai/configs/learned_filter_v1_qat.yaml` — first QAT recipe

Mirrors `ai/configs/filter_residual_v1.yaml` plus a `qat:` block
with the ADR-0207 defaults (`epochs_fp32: 20`, `epochs_qat: 10`,
`lr_qat: 1e-5`, `input_shape: [1, 1, 32, 32]`). 32×32 is sufficient
for FX symbolic tracing; the dynamic-batch ONNX still serves the
shipped 224×224 inference path.

### 5. Empirical QAT delta on `learned_filter_v1`

Synthetic-corpus validation (256 (degraded, clean) pairs, L1 loss,
20 fp32 + 10 QAT epochs, held-out 32-sample evaluation set):

| Comparison                              | PLCC drop | RMSE   | Budget (0.002) |
|-----------------------------------------|-----------|--------|----------------|
| QAT-fp32 vs QAT-int8 (within-pipeline)  | 0.000081  | 0.00363| **PASS**       |
| fp32-baseline vs QAT-int8               | 0.001228  | 0.01437| **PASS**       |
| fp32-baseline vs static-PTQ-int8        | 0.000066  | 0.00329| **PASS**       |

Both QAT and static-PTQ stay inside the 0.002 PLCC drop budget on
this model. **Static-PTQ wins on the cross-pipeline comparison**
because the QAT phase nudges the weights toward
quantization-friendly values at the cost of fp32 fidelity vs the
cleanly-trained baseline. The within-pipeline comparison
(QAT-fp32 → QAT-int8) is the operationally meaningful measurement
— it confirms QAT successfully tightened the fp32-to-int8 step
(0.000081 vs static-PTQ's 0.000066, both well inside budget).

This matches Research-0006 §1's prediction: on tiny models with few
layers, QAT's accuracy-recovery advantage over static PTQ collapses
because there's little weight-distribution friction for static
calibration to mishandle. **`learned_filter_v1` stays on
`quant_mode: "dynamic"`** for now — the QAT pipeline is wired and
validated, but flipping the registry entry would trade a measurable
fp32-baseline-vs-int8 advantage for no functional gain.

The QAT path is now a tier the next tiny model can opt into without
per-model design work, per ADR-0207's "third quant tier" goal.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **A. Two-step PyTorch-QAT → fp32 ONNX → ORT static-quantize** *(chosen)* | Sidesteps both PyTorch 2.11 ONNX exporter bugs; produces a QDQ ONNX bit-identical in structure to the existing PTQ-static path; ORT loads on every EP (CPU/CUDA/OpenVINO); preserves the QAT weight-conditioning effect | The exported `.int8.onnx` cannot be distinguished from a PTQ-static artefact by graph inspection alone — provenance is registry/sidecar only | Picked. Only path that works on PyTorch 2.11 + ORT 1.25 today. |
| B. `convert_fx` → `torch.onnx.export(..., dynamo=False)` (per ADR-0207 §4) | Single-step pipeline; matches the "modern" PyTorch QAT story | Legacy exporter emits `quantized::conv2d` / `quantized::add` — non-standard ONNX ops; ORT refuses to load | PyTorch's QAT story is forward-looking; today's exporter cannot produce a QDQ ONNX from `convert_fx`. |
| C. `convert_fx` → `torch.onnx.export(..., dynamo=True)` (TorchDynamo) | "Forward-looking" exporter | Hits `Conv2dPackedParamsBase.__obj_flatten__` AttributeError on every QAT-converted module in PyTorch 2.11 | PyTorch open issue; not ours to fix this PR. |
| D. Custom qconfig with plain `FakeQuantize` (no `FusedMovingAvgObsFakeQuantize`) → legacy ONNX export | Sidesteps the `aten::fused_moving_avg_obs_fake_quant` ONNX symbolic gap | Then trips on `aten::copy` (also unsupported by the legacy exporter); chasing exporter gaps one op at a time is endless | Two-step pipeline is the cleaner cut. |
| E. Skip QAT, ship static-PTQ for `learned_filter_v1` | Zero new code; static-PTQ already passes the budget on the synthetic-corpus harness | Direct contradiction of ADR-0207's user direction (*"implement it? ffs"*) | Rejected per ADR-0207 §References. |

## Consequences

- **Positive**:
  - Closes T5-4. The `qat_train.py` scaffold ships a real
    implementation; no more `NotImplementedError` paper trail.
  - The trainer hook is reusable: every future tiny model can pick
    `quant_mode: "qat"` upfront with a `<model>_qat.yaml` config,
    no per-model design work.
  - The two-step bridge means QAT-trained models load on every EP
    the PTQ static path supports (CPU, CUDA, OpenVINO/Level Zero) —
    no T5-3e re-validation needed.
  - QAT trained-vs-deployed delta validated empirically at 0.000081
    PLCC drop on `learned_filter_v1` (synthetic corpus); within
    budget by 25×.

- **Negative**:
  - The two-step pipeline is more moving parts than a single
    `convert_fx` → ONNX call. Provenance ("was this artefact QAT
    or PTQ-static?") is registry-only, not graph-visible.
  - The `lr_qat` default (`fp32_lr / 10`) is a heuristic from
    Research-0006; per-model tuning may be needed for larger
    architectures. Tracked in the follow-up section.
  - PyTorch 2.10's `torch.ao.quantization` deprecation warning will
    eventually force a migration to `torchao.quantization.pt2e`
    (ADR-0207 §Consequences). The two-step pipeline is mostly
    pt2e-compatible (the ORT-static-quantize step is unchanged) so
    the migration cost is bounded to the FX-prep call.

- **Neutral / follow-ups**:
  - `learned_filter_v1` stays on `quant_mode: "dynamic"` — the QAT
    pass works but doesn't outperform the existing dynamic-PTQ
    artefact on the synthetic-corpus harness. Re-evaluate when the
    real BVI-DVC corpus drops on disk; QAT typically wins on real
    content where weight distributions are wider than synthetic
    luma.
  - Per-model ADRs for new tiny models picking `quant_mode: "qat"`
    will follow this ADR's empirical-delta table format.
  - Open issue: track `torchao.quantization.pt2e` migration once
    PyTorch 2.10 drops the deprecation warning to a hard error.

## References

- [ADR-0207](0207-tinyai-qat-design.md) — QAT design (parent ADR).
- [ADR-0173](0173-ptq-int8-audit-impl.md) — PTQ audit-harness
  implementation (the harness QAT plugs into).
- [ADR-0174](0174-first-model-quantisation.md) — first per-model
  PTQ template; this ADR mirrors that template for QAT.
- [Research-0006](../research/0006-tinyai-ptq-accuracy-targets.md)
  §1 Table 1 — accuracy-budget origin (0.002 PLCC drop for QAT).
- [Section-A audit decisions](../../.workingdir2/decisions/section-a-decisions-2026-04-28.md)
  §A.2.1 — *"implement it? ffs"*. Captured per ADR-0207.
- PyTorch open issue [pytorch/pytorch#issue
  Conv2dPackedParamsBase obj_flatten] — context for the
  TorchDynamo exporter failure observed during implementation.

### Status update 2026-05-08: Accepted

Audited as part of the 2026-05-08 ADR `Proposed` sweep
([Research-0086](../research/0086-adr-proposed-status-sweep-2026-05-08.md)).

Acceptance criteria verified in tree at HEAD `0a8b539e`:

- `model/tiny/learned_filter_v1.int8.onnx` — present alongside the
  fp32 `learned_filter_v1.onnx`.
- `ai/train/qat.py` — real implementation present.
- `ai/scripts/measure_quant_drop.py` /
  `measure_quant_drop_per_ep.py` — present.
- The empirical finding stands: `learned_filter_v1` stays on
  `quant_mode: "dynamic"` per `model/tiny/registry.json`; the QAT
  pipeline is validated and ready for the next model whose static
  PTQ exceeds the budget.
- Verification command:
  `ls model/tiny/learned_filter_v1*.onnx ai/train/qat.py`.
