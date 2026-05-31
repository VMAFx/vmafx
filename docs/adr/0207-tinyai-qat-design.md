<!-- markdownlint-disable MD060 -->
# ADR-0207: Tiny-AI Quantization-Aware Training (QAT) — design

- **Status**: Accepted
- **Date**: 2026-04-28
- **Deciders**: <lusoris@pm.me>, Claude (Anthropic)
- **Tags**: ai, quantization, dnn, tiny-ai, fork-local

## Context

The fork's tiny-AI surface ships post-training quantization (PTQ)
end-to-end via [ADR-0173](0173-ptq-int8-audit-impl.md) (audit
harness + registry plumbing) and [ADR-0174](0174-first-model-quantisation.md)
(first per-model PTQ on `learned_filter_v1`). Both ADRs explicitly
defer Quantization-Aware Training (QAT) — the
[`ai/scripts/qat_train.py`](../../ai/scripts/qat_train.py) scaffold
ships with a `NotImplementedError` and a docstring pointing at the
deferred work.

The 2026-04-28 backlog audit ([Section A.2.1](../backlog-audit-2026-04-28.md))
flagged QAT as untracked. Per the
[Section-A audit decisions](../../.workingdir2/decisions/section-a-decisions-2026-04-28.md)
§A.2.1, the user direction is **implement, do not close** — QAT
becomes backlog row **T5-4** with implementation scope. This ADR
locks the QAT pass design before code lands.

The substantive forces driving the design:

- **PTQ accuracy floor**: Research-0006's per-model PLCC budgets
  are ~0.005 (static PTQ) and ~0.01 (dynamic PTQ). On a tiny model
  with few layers there is little room for QAT to improve over
  static PTQ — the regression survey in `Research-0006 §1` puts
  QAT at 0.0002–0.003 PLCC drop. Whether QAT *measurably* helps on
  fork-trained models is the empirical question this ADR
  authorises us to answer.
- **Training-time cost**: QAT requires a finetune phase after
  fp32 convergence. Research-0006 §4 estimates ~50% extra training
  time on `tiny-vmaf-v2`-class models, ~10 min on the smaller
  `learned_filter_v1` / `nr_metric_v1` shipped today. Cheap
  enough to default to QAT once a model exhausts PTQ budget.
- **Determinism**: `_load_session` in the LOSO eval harness
  (PR #165, [`ai/scripts/eval_loso_mlp_small.py`](../../ai/scripts/eval_loso_mlp_small.py))
  already documents one ONNX-export determinism gotcha (the
  external_data location rename); QAT adds another (FakeQuant
  observer placement + qparam folding). The ADR pins the export
  path so the registry's `int8_sha256` field stays reproducible.
- **Pairs with T5-3e** (PTQ on CUDA + Intel Arc accelerators):
  QAT-trained models must round-trip through the same EP set,
  not just CPU EP. The export path picked here doubles as the
  T5-3e validation surface.

## Decision

We will implement QAT via PyTorch's `torch.ao.quantization`
modern API, fine-tuning a fp32-pretrained checkpoint with
FakeQuant observers inserted via `prepare_qat_fx`, then exporting
through `convert_fx` → `torch.onnx.export(..., opset_version=17)`
into the existing `.int8.onnx` registry slot. The pipeline is:

1. **fp32 phase** — train the model normally for the configured
   epoch count. Output: a Lightning checkpoint reused as the QAT
   warm-start.
2. **Fake-quant insertion** — `prepare_qat_fx(model, qconfig_mapping,
   example_inputs)` with the **default symmetric per-tensor weight
   qconfig** (`torch.ao.quantization.get_default_qat_qconfig_mapping("x86")`)
   and per-channel weight observers for `nn.Linear` / `nn.Conv2d`
   layers. Activations stay per-tensor symmetric. This matches
   the PTQ static recipe in Research-0006 §2 so the QAT-vs-static
   delta is attributable to training, not to qconfig drift.
3. **QAT fine-tune phase** — train for a configurable smaller
   number of epochs (default 10 for tiny models per Research-0006
   §4). Use a 10×-reduced learning rate. Train against the same
   loss + dataloaders as the fp32 phase.
4. **Convert + export** — `convert_fx(model)` → `torch.onnx.export(
   ..., opset_version=17, do_constant_folding=True)`. Output is a
   QDQ-format `.int8.onnx` with per-channel weight quantization,
   per-tensor activation quantization, and folded qparams.
5. **Registry handoff** — pass through the existing PTQ harness
   (`ai/scripts/measure_quant_drop.py`) for the PLCC budget gate.
   QAT models register with `quant_mode="qat"` (extending the
   existing `"static"` / `"dynamic"` enum) and the same
   `int8_sha256` sidecar pin used by PTQ models.

The default budget for `quant_accuracy_budget_plcc` on QAT models
is **0.002** (Research-0006 §1 Table 1). A model that exceeds
the budget remains in fp32 — the runtime fallback path in
`vmaf_dnn_session_open` already handles this case.

The trainer hook lives in `ai/train/qat.py` (new) and is wired
into `ai/scripts/qat_train.py`'s entry point. The Lightning module
gains a `--qat` flag that runs phase 1 → 2 → 3 in one invocation;
phase 4 runs as a post-train step.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **A. Modern `torch.ao.quantization` (`prepare_qat_fx`/`convert_fx`)** *(chosen)* | Stable since PyTorch 1.13; FX-graph traceable models cover all current tiny models; round-trips through ONNX opset 17 cleanly | Requires the model be FX-traceable (no Python control flow in `forward`); PR-time cost to validate FX-traceability for each shipped model | Picked. Tiny-AI models in this fork are MLPs / small CNNs — all FX-traceable today. |
| B. Legacy `torch.quantization.prepare_qat` (eager mode) | No FX requirement; simpler API surface | Deprecated in PyTorch since 2.0; manual `QuantStub` / `DeQuantStub` insertion; harder to maintain qconfig parity with PTQ static path | Modern API is mandatory by the time the next PyTorch upgrade lands; investing in the deprecated API now buys nothing. |
| C. ONNX Runtime QAT-equivalent path (Microsoft `Olive` toolkit) | Single-tool ONNX-only flow; no PyTorch dependency at quant time | Olive is ORT-internal tooling, not stable for fork-local use; produces QAT models by exporting fp32 to ONNX *first*, then training in ORT, which inverts our PyTorch-first training flow | Olive's "QAT in ORT" path needs ONNX-as-source; the fork trains in PyTorch. Round-tripping back to PyTorch for finetune defeats the point. |
| D. Skip QAT, pin PTQ static + tighten the budget | Zero new code; per Research-0006 §1 the typical static-PTQ-vs-fp32 PLCC drop on a tiny MLP sits at the lower 0.001 end | User explicitly directed *implement, do not close* (§A.2.1); also leaves the `tiny-vmaf-v2` prototype path under `ai/prototypes/` without a sub-0.002 PLCC option | Direct contradiction of user direction. |

## Consequences

- **Positive**:
  - Closes T5-4. Makes the `qat_train.py` scaffold honest — no
    more `NotImplementedError` paper trail.
  - Tightens the per-model accuracy budget option (PLCC drop
    floor of ~0.002 vs static PTQ's ~0.005).
  - Adds a third `quant_mode` value (`"qat"`) to the registry,
    giving the audit harness three rungs (`"dynamic"` →
    `"static"` → `"qat"`) instead of two.
  - Future PRs that train new tiny models can pick `"qat"`
    upfront without per-model design work.

- **Negative**:
  - +1 trainer dependency surface (`torch.ao.quantization` and its
    deprecation cadence — Pytorch 2.x renames every 12-18 months).
  - +50% training-time cost when QAT is enabled (Research-0006
    §4). Acceptable for tiny models; documented in `docs/ai/training.md`.
  - Adds an FX-traceability requirement to every new tiny-AI
    model architecture. Models with Python control flow in
    `forward` will need refactor before QAT applies — block at
    QAT enablement, not at fp32 train time.

- **Neutral / follow-ups**:
  - Implementation PR opens once this ADR ships and lands.
    Scope: `ai/train/qat.py` + `ai/scripts/qat_train.py`
    real-implementation + `ai/configs/<model>_qat.yaml`
    examples + a smoke-test PR row + the registry schema bump
    (`quant_mode` enum extension).
  - Pairs with **T5-3e** (PTQ on CUDA + Intel Arc accelerators):
    QAT models must round-trip through the same EP set as PTQ
    models. The implementation PR validates on at least
    `learned_filter_v1` int8 across CPU EP + CUDA EP + (where
    available) OpenVINO / Level Zero EP on Arc.
  - Update [`docs/ai/quantization.md`](../ai/quantization.md) to
    mention the third quant tier alongside dynamic / static.
  - Update [Research-0006](../research/0006-tinyai-ptq-accuracy-targets.md)
    §1's accuracy-budget table to include the empirical QAT vs
    PTQ delta once the first QAT model lands.
  - The first QAT model gets its own per-model ADR (mirroring
    ADR-0174 for `learned_filter_v1` PTQ) so the empirical
    delta is captured per-model, not per-pass.

## References

- [ADR-0129](0129-tinyai-ptq-quantization.md) — original PTQ scope decision.
- [ADR-0173](0173-ptq-int8-audit-impl.md) — PTQ audit-harness implementation.
- [ADR-0174](0174-first-model-quantisation.md) — first per-model PTQ.
- [Research-0006](../research/0006-tinyai-ptq-accuracy-targets.md) — accuracy
  budgets, ORT API surface, and QAT cost estimates.
- [Section-A audit decisions](../../.workingdir2/decisions/section-a-decisions-2026-04-28.md)
  §A.2.1 — *user response: "implement it? ffs"*. Captured as the
  binding direction for this ADR's scope.
- [PyTorch Quantization (`torch.ao.quantization`)](https://pytorch.org/docs/stable/quantization.html)
  — modern API surface.
- [NVIDIA "Achieving FP32 Accuracy for INT8 Inference Using QAT"](https://developer.nvidia.com/blog/achieving-fp32-accuracy-for-int8-inference-using-quantization-aware-training-with-tensorrt/)
  — QAT recipe heuristics; the 95%-fp32-recovery target.

### Status update 2026-05-08: Accepted

Audited as part of the 2026-05-08 ADR `Proposed` sweep
([Research-0086](../research/0086-adr-proposed-status-sweep-2026-05-08.md)).

Acceptance criteria verified in tree at HEAD `0a8b539e`:

- `ai/train/qat.py` — present (real Lightning-compatible
  `run_qat` + `QatConfig`).
- `ai/scripts/qat_train.py` — present (real CLI driver, no longer
  the prior `NotImplementedError` scaffold).
- ADR-0208 (this sweep, Accepted) is the first per-model QAT
  application validating the pipeline end-to-end on
  `learned_filter_v1`.
- Verification command:
  `ls ai/train/qat.py ai/scripts/qat_train.py`.
