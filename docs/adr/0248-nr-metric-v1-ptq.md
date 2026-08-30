<!-- markdownlint-disable MD060 -->
# ADR-0248: `nr_metric_v1` joins dynamic-PTQ family (T5-3d)

- **Status**: Accepted
- **Date**: 2026-04-29
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: tiny-ai, onnx, quantization, registry, fork-local

## Context

[ADR-0173](0173-ptq-int8-audit-impl.md) shipped the audit-first PTQ
harness. [ADR-0174](0174-first-model-quantisation.md) flipped the
first model — `learned_filter_v1` — into `quant_mode: "dynamic"` and
explicitly deferred `nr_metric_v1` because
`onnxruntime.quantization.quantize_dynamic` raised `Inferred shape and
existing shape differ in dimension 0: (128) vs (1)` during its
internal shape inference pass.

PR #174 (T5-3e empirical PTQ accuracy) hit the same class of failure
on `vmaf_tiny_v1*.onnx` and traced the root cause: `torch.onnx.export`
duplicates every initialiser into `graph.value_info` with static-shape
annotations that do not survive the dynamic batch axis substitution.
ORT's pre-quantisation shape inference then fails when the duplicated
record disagrees with the canonical shape on the initialiser. PR #174
introduced a `_save_inlined` helper in
`ai/scripts/measure_quant_drop_per_ep.py` that strips every
`value_info` entry whose name collides with an initialiser. Inspecting
the shipped `model/tiny/nr_metric_v1.onnx` confirmed the same
duplicate pattern (29 initialisers, all 29 mirrored in
`graph.value_info`).

## Decision

We will (1) port the PR #174 strip pattern into the canonical export
path and the dynamic-PTQ entry point, (2) re-save the existing
`nr_metric_v1.onnx` with the `value_info` duplicates stripped (the
inference-graph semantics are unchanged — initialisers carry their
own canonical shape — and ONNX-Runtime CPU produced bit-identical
output before vs after on a deterministic 16-sample input set), and
(3) flip `nr_metric_v1` to `quant_mode: "dynamic"` with the same
0.01-PLCC budget as `learned_filter_v1`. The drop measured by
`ai/scripts/measure_quant_drop.py` is **0.007674** (PLCC 0.992326),
inside budget.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Strip duplicates only inside `ptq_dynamic.py` (no export-side fix) | Smallest blast radius; existing on-disk model bytes stay frozen | Every future re-export of any tiny model would re-introduce the bug; the next model to quantise hits it again | Rejected. The cost of also updating `exports.py` is two lines, the cost of repeating the diagnosis is hours per model. |
| Re-train + re-export from a Lightning checkpoint with `dynamo=True` and a fresh `dynamic_axes` spec | Yields a graph the upstream tooling produces cleanly; future-proof against torch.onnx legacy quirks | No `runs/c2_konvid/last.ckpt` is committed; KoNViD-1k corpus is not redistributable; would block T5-3d on a full retraining cycle just to reproduce the same weights | Rejected. The fp32 weights are already audited (sha256-pinned in registry); re-saving with `value_info` stripped preserves the audit chain. |
| Pin a workaround in `onnxruntime` | Long-term cleanest if upstream accepts | Requires new release dependency; `onnxruntime` 1.22 is the floor across the rest of the harness | Rejected. The strip is a five-line ONNX-level transform; an upstream patch would take longer than this entire PR. |
| Promote `nr_metric_v1` to `static` PTQ instead of `dynamic` | Slightly better accuracy; per-channel calibration | Requires shipping a calibration `.npz`; `dynamic` already inside budget by 23% | Rejected. ADR-0174 precedent: don't add the calibration-asset cost until a budget violation forces it. |

## Consequences

- **Positive:**
  - C2 (`nr_metric_v1`) is now part of the quantised family;
    end-to-end PTQ flow now covers two of the three production tiny
    models (the third — LPIPS-Sq — is upstream-derived and out of
    scope for fork-local quantisation decisions).
  - The `value_info` strip in `export_to_onnx` makes every future
    fork-trained tiny model PTQ-clean by construction.
  - The `value_info` strip inside `ptq_dynamic.py` makes the entry
    point robust against pre-existing fork-local ONNX files that
    pre-date the export-side fix.
  - 2.0× size shrink (119 KB → 58 KB).
- **Negative:**
  - The on-disk `nr_metric_v1.onnx` sha256 changes
    (`60c2bd59…` → `75eff676…`). All consumers that pinned the
    pre-T5-3d hash need to roll forward. Same audit trail applies as
    to a normal model refresh.
  - PLCC drop (0.0077) is much higher than `learned_filter_v1`
    (0.000117). Still inside the 0.01 budget, but the headroom is
    only 23% — a future architectural change to the C2 path could
    cross the line. Tracked in `docs/ai/quantization.md` as the
    motivating follow-up to revisit `static` PTQ if budget headroom
    erodes.
- **Neutral / follow-ups:**
  - `nr_metric_v1` Sigstore bundle is still a placeholder
    (`nr_metric_v1.onnx.sigstore.json` is populated at release time
    by `.github/workflows/supply-chain.yml`) — same lifecycle as the
    fp32 file.

## Tests

- `python ai/scripts/ptq_dynamic.py model/tiny/nr_metric_v1.onnx`
  produces a 59 797-byte int8 file.
- `python ai/scripts/measure_quant_drop.py model/tiny/nr_metric_v1.onnx`
  reports `[PASS] nr_metric_v1 mode=dynamic PLCC=0.992326
  drop=0.007674 budget=0.0100`.
- `python ai/scripts/validate_model_registry.py` reports `OK: 6
  registry entries valid against registry.schema.json`.
- The CI `ai-quant-accuracy` step (introduced in ADR-0174) now
  exercises both `learned_filter_v1` and `nr_metric_v1`.

## Reproducer

```bash
# (one-time, on the existing on-disk fp32 file)
python - <<'PY'
import onnx
m = onnx.load("model/tiny/nr_metric_v1.onnx")
init = {t.name for t in m.graph.initializer}
keep = [vi for vi in m.graph.value_info if vi.name not in init]
del m.graph.value_info[:]
m.graph.value_info.extend(keep)
onnx.save(m, "model/tiny/nr_metric_v1.onnx", save_as_external_data=False)
PY

# Quantise + gate.
python ai/scripts/ptq_dynamic.py model/tiny/nr_metric_v1.onnx
python ai/scripts/measure_quant_drop.py model/tiny/nr_metric_v1.onnx
```

## References

- [ADR-0173](0173-ptq-int8-audit-impl.md) — PTQ audit harness.
- [ADR-0174](0174-first-model-quantisation.md) — first per-model PTQ
  (`learned_filter_v1`); deferred `nr_metric_v1` to T5-3c, absorbed
  here as T5-3d.
- [ADR-0168](0168-tinyai-konvid-baselines.md) — `nr_metric_v1`
  baseline.
- PR #174 (T5-3e empirical PTQ) — origin of the `_save_inlined`
  workaround.
- BACKLOG row T5-3d — per-model PTQ umbrella.
- `req` — user direction 2026-04-29: implement T5-3d first sub-bullet
  (re-export `nr_metric_v1` with explicit dynamic batch axis + run
  T5-3 PTQ pipeline).
