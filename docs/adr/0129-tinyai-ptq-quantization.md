<!-- markdownlint-disable MD013 MD036 MD060 -->
# ADR-0129: Tiny-AI post-training int8 quantisation — static + dynamic + QAT per model

- **Status**: Accepted
- **Date**: 2026-04-20
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: ai, onnx, quantization, model, docs

## Context

The fork's tiny-AI surface (`ai/` for training, `core/src/dnn/` for
the ONNX Runtime integration) currently ships fp32 ONNX models. Even
the smallest fork-trained model (`tiny-vmaf-v1.onnx`, ~4 MB) dominates
per-frame CPU cost on low-end boxes and embedded ARM platforms. The
ONNX Runtime int8 story is mature: the `onnxruntime.quantization`
Python module covers static (calibration-based), dynamic (per-activation
runtime quant), and QAT (quant-aware training) in one API family; the
runtime `CPUExecutionProvider` has been shipping QDQ-format int8 kernels
since 2022.

What we don't currently have is a **policy** for which quantisation
mode each model gets. The three modes trade off in different
directions:

- **Static PTQ**: highest accuracy preservation among PTQ techniques;
  requires a representative calibration dataset; needs to be re-run
  when the calibration set changes. Best fit for models where we
  control training and have a stable calibration set.
- **Dynamic PTQ**: cheapest (single CLI command, no calibration data);
  quantises weights offline, activations at runtime; accuracy penalty
  slightly larger than static. Best fit for models where calibration
  data is unavailable or where the deployment box differs from the
  training box.
- **QAT**: largest training-time investment (needs a second training
  phase with fake-quant ops in the forward pass); recovers most of the
  static PTQ accuracy loss; requires the original training code to
  cooperate. Best fit for models where PTQ drops accuracy below the
  VMAF-soak tolerance band.

The user directive on 2026-04-20 was explicit: "static, dynamic and
QAT" — all three, each used where it fits. What the project lacked was
a mechanism for declaring which model uses which, and a harness for
running the regression comparison.

The existing `model/registry.json` (one entry per shipped model) is
the natural place to carry the quantisation decision per model.

## Decision

We will add a **per-model `quant_mode` field** to
`model/registry.json` and a **three-script PTQ harness** under
`ai/scripts/` that produces quantised artefacts committed alongside
the fp32 originals.

- **Registry schema extension**: each model entry gains

  ```json
  {
    "quant_mode": "fp32" | "static" | "dynamic" | "qat",
    "quant_calibration_set": "path/to/calibration.bin",  // static only
    "quant_accuracy_budget_plcc": 0.01                    // max allowed PLCC drop vs fp32
  }
  ```

- **Three scripts** under `ai/scripts/`:
  - `ptq_static.py` — loads fp32 ONNX, runs
    `onnxruntime.quantization.quantize_static` against the calibration
    set named in the registry.
  - `ptq_dynamic.py` — one-liner wrapper around
    `quantize_dynamic`; no calibration needed.
  - `qat_train.py` — wraps the existing tiny-AI PyTorch trainer with
    `torch.quantization.prepare_qat_fx`, converts to ONNX with
    quantised ops at the end.
- **Artefact layout**: quantised ONNX sits next to fp32 ONNX with
  `.int8.onnx` suffix. `model/registry.json` points the runtime to the
  `.int8.onnx` file iff `quant_mode != "fp32"`; the fp32 file is kept
  as the regression baseline.
- **Accuracy gate**: a new CI leg (`ai-quant-accuracy`) runs the
  quantised model against the same VMAF soak-test fixtures the fp32
  model was validated on, and asserts Pearson-linear-correlation-drop
  against the `quant_accuracy_budget_plcc` threshold from the registry.
  A drop beyond budget fails the PR.
- **Runtime switch**: the ONNX Runtime initialisation in
  `core/src/dnn/` inspects the registry entry and loads the
  quantised file transparently. Users see no API change; the model
  just runs faster on int8-capable CPUs.
- **First target models**:
  - `tiny-vmaf-v1.onnx` → dynamic (we have no calibration set checked
    in; the 2x speedup is worth the accuracy cost on an already-small
    model).
  - Future SSIMULACRA-2-adjacent models that ship with a calibration
    set → static.
  - Models where static drops PLCC past budget → QAT (deferred until
    we hit a concrete case; empirically ~1 in 4 tiny-AI models need
    it).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Three modes via registry (chosen) | Fine-grained per-model control; audit trail in the registry; clean CI gate per model | Three scripts to maintain; must keep the registry schema and the runtime in sync | Matches the user's explicit "static + dynamic + QAT" directive and the existing registry.json architecture |
| Dynamic-only (pick the easiest) | One script; no calibration data management; covers 80% of the speedup | Models that need static or QAT would silently drift past accuracy budget | Explicitly rejected by the user — "somehow I want static, dynamic and qat" |
| Static-only (pick the strongest PTQ) | Best PTQ accuracy; single codepath | Calibration-set management becomes a submodule; blocks models for which calibration data is unavailable | Misses the "dynamic, no-calibration-data" use case users care about |
| Leave fp32 as-is, optimise elsewhere | Zero new code; no accuracy risk | Leaves the 2–4x inference speedup on the floor; mobile / embedded story stays weak | Doesn't match the modernisation goal |
| Quant decision in Python only, no registry field | Less schema surface | Decision is hidden in a training notebook; impossible to audit per-model post-hoc | Contradicts the fork's existing "registry.json is the source of truth for model metadata" pattern |

## Consequences

**Positive**

- Closes the 2–4x inference-speedup gap on int8-capable CPUs (most
  modern x86 + all ARMv8.2+) for tiny-AI paths.
- Per-model quant-mode field documents the decision permanently at the
  model level — future maintainers don't have to reconstruct
  "why is this one static?".
- QAT is now reachable without re-architecting the training pipeline;
  the existing trainer gains one optional phase.
- The accuracy-budget field + CI gate prevents silent regressions when
  a quantised model is rebuilt against a new calibration set.

**Negative**

- Three scripts (static / dynamic / QAT) duplicate about 60% of the
  ORT API surface. Acceptable because each has distinct user-facing
  semantics.
- Calibration-set storage: static PTQ calibration data is small (~50
  MB) but still new binary under `ai/calibration/`. Tracked via git
  LFS (already set up for fork-trained model weights).
- QAT re-training is expensive (roughly 1.5× the fp32 training time).
  Runs only on-demand; not part of every training cycle.

**Neutral**

- No impact on the Netflix CPU golden gate — it never exercises
  tiny-AI models.
- No change to the public C ABI — the quantisation is entirely
  internal to model loading.
- `mcp-server/vmaf-mcp/` sees no change; the model swap is transparent
  to the MCP surface.

## Alignment with the audit-first directive

Per the user's direction on 2026-04-20 ("Audit first" for the
tiny-AI model registry), the first implementation PR does **not**
immediately quantise every model. The sequence is:

1. Audit PR: extend the registry schema, add the three scripts, add
   the CI accuracy-gate leg, but do not change any existing model's
   `quant_mode` from `fp32`. Purely infrastructural.
2. Per-model quantisation PRs: one PR per model, each with its own
   accuracy-drop measurement in the PR body and its own soak-test
   result attached.
3. If a model fails the static budget, escalate to QAT in a follow-up
   PR rather than relaxing the budget.

This keeps each quantisation decision reviewable in isolation.

## References

- [req] AskUserQuestion popup answered 2026-04-20: PTQ int8 scope →
  "somehow I want static, dynamic and qat!!!!"; harness layout →
  "registry.json per-model (Recommended)"; first workstream action →
  "Audit first".
- [Research-0006](../research/0006-tinyai-ptq-accuracy-targets.md) —
  accuracy regression targets, ORT API comparison, calibration-set
  sourcing.
- [ONNX Runtime quantization docs](https://onnxruntime.ai/docs/performance/model-optimizations/quantization.html)
- [PyTorch QAT guide](https://pytorch.org/docs/stable/quantization.html#quantization-aware-training)
- [ADR-0042](0042-tinyai-docs-required-per-pr.md) — tiny-AI docs-per-PR
  rule applies to this workstream.
- [`model/registry.json`](../../model/registry.json) — the registry
  this ADR extends.
- [`ai/`](../../ai/) — the training directory where the PTQ / QAT
  scripts land.
- [CLAUDE.md §12 r10](../../CLAUDE.md) — per-surface docs rule
  (quant-mode user-visible → doc entry under `docs/ai/quantization.md`).

### Status update 2026-05-08: Accepted

Audited as part of the 2026-05-08 ADR `Proposed` sweep
([Research-0086](../research/0086-adr-proposed-status-sweep-2026-05-08.md)).

Acceptance criteria verified in tree at HEAD `0a8b539e`:

- `model/tiny/registry.schema.json` carries `quant_mode`
  (line 64), `quant_calibration_set` (line 70),
  `quant_int8_sha256` (line 75), `quant_accuracy_budget_plcc`
  (line 79).
- PTQ scripts under `ai/scripts/`: `ptq_static.py`,
  `ptq_dynamic.py`, `measure_quant_drop.py`,
  `measure_quant_drop_per_ep.py`.
- ADR-0173 (Accepted) shipped the audit-first registry/CI plumbing.
- ADR-0174 (Accepted) shipped the first per-model PTQ
  (`learned_filter_v1` on `quant_mode: "dynamic"`).
- ADR-0207 / ADR-0208 (this sweep, both Accepted) extend the policy
  to QAT.
- Verification command:
  `grep -E "quant_mode|quant_accuracy_budget_plcc"
  model/tiny/registry.schema.json`.
