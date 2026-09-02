<!-- markdownlint-disable MD060 -->
# ADR-0275: `vmaf_tiny_v3` and `vmaf_tiny_v4` join dynamic-PTQ family (T5-3d follow-up)

- **Status**: Accepted
- **Date**: 2026-05-03
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: tiny-ai, onnx, quantization, registry, fork-local

## Context

[ADR-0173](0173-ptq-int8-audit-impl.md) shipped the audit-first PTQ
harness. [ADR-0174](0174-first-model-quantisation.md) flipped the
first per-model entry — `learned_filter_v1` — into `quant_mode:
"dynamic"`. [ADR-0248](0248-nr-metric-v1-ptq.md) added `nr_metric_v1`
once its `value_info` shape-inference issue was resolved.

The tiny-AI ladder for VMAF feature fusion now has three rungs:
[`vmaf_tiny_v2`](../ai/models/vmaf_tiny_v2.md) (`mlp_small`, ~257
params), [`vmaf_tiny_v3`](../ai/models/vmaf_tiny_v3.md) (`mlp_medium`,
~769 params; ADR-0241), and
[`vmaf_tiny_v4`](../ai/models/vmaf_tiny_v4.md) (`mlp_large`, ~3 073
params; ADR-0242). v2's quantisation analysis is moot — its fp32
ONNX is 2 446 bytes; the weight tensors are a tiny fraction of that
and an int8 sidecar would not deliver a meaningful size win. v3 and
v4 are the first VMAF feature-fusion tier where the question is
worth asking.

This ADR closes that gap by shipping dynamic-PTQ int8 sidecars for
both v3 and v4 so the runtime redirect from ADR-0174 has a target
when an operator opts into v3 or v4 with quantisation enabled in
their registry overlay.

## Decision

We will (1) produce `vmaf_tiny_v3.int8.onnx` and
`vmaf_tiny_v4.int8.onnx` via `ai/scripts/ptq_dynamic.py`, (2) add
both models to `model/tiny/registry.json` with `quant_mode:
"dynamic"`, `int8_sha256`, and `quant_accuracy_budget_plcc: 0.01`,
(3) mirror those fields into the per-model sidecars
`model/tiny/vmaf_tiny_v3.json` and `vmaf_tiny_v4.json`, and (4)
extend the `ai-quant-accuracy` CI gate's coverage transparently
(the gate already iterates every non-`fp32` registry entry).

Both models stay inside the 0.01 PLCC budget by two orders of
magnitude on the Netflix-features parquet (~11k rows of canonical-6
inputs the registered v3/v4 graphs were trained on):

| Model | fp32 → int8 size | PLCC drop (vs fp32 on Netflix) | Headroom vs 0.01 budget |
| --- | --- | --- | --- |
| `vmaf_tiny_v3` | 4 496 B → 4 267 B (×0.95) | 0.000120 | ×83 |
| `vmaf_tiny_v4` | 14 046 B → 7 769 B (×0.55, -45 %) | 0.000145 | ×69 |

The PLCC self-similarity (int8 vs fp32 on the same inputs) is
0.999963 / 0.999958. KoNViD cross-corpus drop on the canonical-6
parquet (~270k rows) is 0.000177 / 0.000080 — both still inside
budget.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Static PTQ with calibration data | Slightly tighter accuracy; per-channel scales | Requires shipping a calibration `.npz` (~1 MB of canonical-6 vectors); dynamic already inside budget by ~70× | Rejected. ADR-0174 precedent: don't add the calibration-asset cost until a budget violation forces it. |
| Per-channel dynamic (`--per-channel`) | Marginal accuracy improvement on weight-rich models | Negligible PLCC delta on graphs this small (already at 1e-4 drop); slightly larger int8 file from per-row scale arrays | Rejected. The per-tensor default already lands two orders below budget; per-channel is a follow-up only if a future architecture rung erodes headroom. |
| Skip v3 entirely (it shrinks only 5 %) | Avoids shipping a barely-smaller sidecar | Breaks "every quantisable rung is registered" CI invariant; runtime redirect would surprise operators who set `quant_mode=dynamic` on v3 | Rejected. The size win is small, but the gate-coverage and registry-completeness wins justify the 4 KB on-disk cost. |
| Wait for QAT (ADR-0129) | Best-in-class accuracy retention | Requires a training-time pipeline; v3 / v4 trainers don't yet emit a quant-aware graph | Rejected for now. PTQ inside budget is the cheaper first step; QAT is tracked as a global escalation lever, not a per-model blocker. |

## Consequences

- **Positive:**
  - Closes the v3 / v4 gap in the dynamic-PTQ family. The registry's
    quantised set now covers `learned_filter_v1`, `nr_metric_v1`,
    `vmaf_tiny_v3`, `vmaf_tiny_v4`.
  - v4 in particular shrinks 45 % on disk, making it cheaper to bundle
    in deploys that pin v4 over v3 for absolute-top-of-ladder PLCC.
  - The `ai-quant-accuracy` CI gate's coverage matrix grows by two
    rows transparently — it already iterates `models[]` and skips
    `fp32` entries.
- **Negative:**
  - Two new int8 sidecar files in-tree (4 267 B + 7 769 B = ~12 KB
    total). Both are well under the "few-MB" external-data threshold,
    so they ship as committed binaries rather than via the
    sigstore + `.onnx.data` pattern (mirroring `learned_filter_v1` and
    `nr_metric_v1`).
  - v3's size delta is small (×0.95). The ADR's gate-coverage
    rationale stays the win; readers should not expect the
    `learned_filter_v1` 2.4× shrink on every model.
- **Neutral / follow-ups:**
  - Sigstore bundles for v3 / v4 fp32 + int8 are populated at release
    time by `.github/workflows/supply-chain.yml`; placeholder bundles
    are not added in this PR.
  - When v5 lands (if ever — ADR-0242 declared the ladder saturated),
    the same recipe applies: run `ptq_dynamic.py`, register fields,
    document.

## Tests

- `python ai/scripts/ptq_dynamic.py model/tiny/vmaf_tiny_v3.onnx`
  produces a 4 267-byte int8 file.
- `python ai/scripts/ptq_dynamic.py model/tiny/vmaf_tiny_v4.onnx`
  produces a 7 769-byte int8 file.
- `python ai/scripts/measure_quant_drop.py --all` reports
  `[PASS]` for both `vmaf_tiny_v3` (drop=0.000120) and
  `vmaf_tiny_v4` (drop=0.000145).
- `python ai/scripts/validate_model_registry.py` reports
  `OK: 12 registry entries valid against registry.schema.json`.

## Reproducer

```bash
# 1. Quantise.
python ai/scripts/ptq_dynamic.py model/tiny/vmaf_tiny_v3.onnx
python ai/scripts/ptq_dynamic.py model/tiny/vmaf_tiny_v4.onnx

# 2. Gate.
python ai/scripts/measure_quant_drop.py --all
# Expected: [PASS] for vmaf_tiny_v3 + vmaf_tiny_v4 (drops well under 0.01).

# 3. Schema validation.
python ai/scripts/validate_model_registry.py
```

## References

- [ADR-0129](0129-tinyai-ptq-quantization.md) — PTQ policy.
- [ADR-0173](0173-ptq-int8-audit-impl.md) — audit-first PTQ harness.
- [ADR-0174](0174-first-model-quantisation.md) — first per-model PTQ
  (`learned_filter_v1`); established `int8_sha256` +
  `quant_accuracy_budget_plcc` registry fields and the runtime
  `.int8.onnx` redirect.
- [ADR-0248](0248-nr-metric-v1-ptq.md) — `nr_metric_v1` PTQ; same
  recipe.
- [ADR-0241](0241-vmaf-tiny-v3-mlp-medium.md) — v3 ship decision.
- [ADR-0242](0242-vmaf-tiny-v4-mlp-large.md) — v4 ship decision.
- `req` — user direction 2026-05-03: paraphrased — "add INT8
  dynamic-PTQ sidecars for vmaf_tiny_v3 and vmaf_tiny_v4 with the
  ADR-0174 0.01-PLCC budget."
