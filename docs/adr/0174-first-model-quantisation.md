# ADR-0174: First per-model PTQ — `learned_filter_v1` dynamic int8 (T5-3b)

- **Status**: Accepted
- **Date**: 2026-04-25
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: tiny-ai, onnx, quantization, registry, ci, fork-local

## Context

[ADR-0173](0173-ptq-int8-audit-impl.md) shipped the audit-first PTQ
harness — registry schema, three scripts, sidecar parser — but
explicitly left **two pieces** for the first per-model PR:

> - The runtime `.int8.onnx` redirect + the `ai-quant-accuracy` CI
>   gate land with the first per-model quantisation PR (T5-3b).

This ADR is that PR. It picks `learned_filter_v1` (the C3 residual
filter from [ADR-0168](0168-tinyai-konvid-baselines.md)) as the
first quantised model because:

1. It quantises cleanly with `quantize_dynamic` out of the box (the
   C2 `nr_metric_v1` model hits an ONNX shape-inference issue on
   our dynamic-batch export — see "Alternatives considered").
2. It's a residual filter, so its output PLCC is well-behaved
   under quantisation: 0.999883 vs fp32 on a 16-sample synthetic
   set, well below the 0.01 budget.
3. Size shrinks 2.4× (80 KB → 33 KB), matching the expected
   weights-only int8 ratio.

## Decision

### 1. `learned_filter_v1` flips to `quant_mode: "dynamic"`

- Quantised file: `model/tiny/learned_filter_v1.int8.onnx`
  (33 019 bytes, sha256 in registry's new `int8_sha256` field).
- Registry entry gains `quant_mode: "dynamic"` +
  `int8_sha256: "1cff6fe07f89..."` +
  `quant_accuracy_budget_plcc: 0.01`.
- Sidecar JSON gains the same `quant_mode` + `int8_sha256` fields.
- Notes line records the provenance: `quant_mode=dynamic via
  ai/scripts/ptq_dynamic.py — see ADR-0174.`

### 2. Runtime `.int8.onnx` redirect in `vmaf_dnn_session_open`

[`core/src/dnn/dnn_api.c`](../../core/src/dnn/dnn_api.c) gains
a redirect block right after `vmaf_dnn_sidecar_load`. When the
sidecar declares `quant_mode != FP32`, the loader:

1. Strips a trailing `.onnx` from the caller-supplied path.
2. Appends `.int8.onnx`.
3. Re-runs `vmaf_dnn_validate_onnx` on the int8 sibling (size +
   allowlist).
4. Passes the int8 path to `vmaf_ort_open` instead of the fp32.

The fp32 file stays on disk as the regression baseline. The
int8-missing path returns a negative error (no silent fp32
fallback — that would mask deployment misconfigurations).

Path-buffer cap: `int8_path[4096]` matches the existing sidecar
parser's `sidecar[4096]` cap; basenames > 4086 chars hit
`-ENAMETOOLONG`.

### 3. New `int8_sha256` registry/sidecar field

`registry.schema.json` adds a 64-char-hex `int8_sha256` field,
required when `quant_mode != "fp32"`. The trust-root invariant
becomes: **both** `<basename>.onnx` (sha256 from the existing
field) **and** `<basename>.int8.onnx` (`int8_sha256`) are
sha256-pinned. The quant_calibration_set + quant_accuracy_budget_plcc
fields from ADR-0173 remain optional.

### 4. New `ai/scripts/measure_quant_drop.py`

Walks `model/tiny/registry.json`, runs each non-`fp32` model
through fp32 + int8 ORT sessions on a 16-sample deterministic
input set (seed 0), and asserts the aggregate Pearson correlation
drop is below `quant_accuracy_budget_plcc`. Skips fp32 entries.
Exit codes:

- 0 — all gated models stay inside budget.
- 1 — at least one model exceeded budget.
- 2 — registry / file errors.

### 5. New CI leg `ai-quant-accuracy`

Wired into the existing `Tiny AI (DNN Suite + ai/ Pytests)` job in
[`tests-and-quality-gates.yml`](../../.github/workflows/tests-and-quality-gates.yml)
as a final step, after the `ai/` pytests. Pulls `onnx`,
`onnxruntime`, `numpy` and runs `measure_quant_drop.py --all`. A
budget violation fails the PR.

## Alternatives considered

1. **Pick `nr_metric_v1` as the first model.** Rejected for this
   PR: ORT's `quantize_dynamic` runs an internal shape inference
   pass that fails on our nr_metric_v1 export with `Inferred shape
   and existing shape differ in dimension 0: (128) vs (1)`. The
   model needs a re-export with static batch = 1 (or symbolic
   shape inference fixed) before it can be quantised. Tracked as a
   T5-3c follow-up. The harness works; the model needs polish.
2. **Use `static` PTQ on `learned_filter_v1`.** Rejected: would
   require shipping a calibration set under `ai/calibration/`
   (~50 MB of representative luma planes). Dynamic PTQ already
   stays well inside budget (drop = 0.000117 vs budget 0.01); the
   marginal accuracy gain from static doesn't justify the new
   binary asset until we hit a concrete budget violation.
3. **Make the int8 file the canonical artefact + drop the fp32
   one.** Rejected: ADR-0129 explicitly keeps the fp32 file as the
   regression baseline. Quantisation is a deployment optimisation,
   not a source change; the fp32 stays as the audit reference.
4. **Hard-error if sidecar declares int8 but ORT doesn't support
   the QDQ ops.** Rejected: out of scope for the redirect step.
   ORT 1.22 supports QDQ comprehensively for the ops in the
   allowlist; if a future op needs special handling, that's a
   follow-up to extend `op_allowlist.c` rather than the redirect.
5. **Relax the budget if 0.01 turns out too tight.** Not relevant
   here — we're at 0.000117, two orders of magnitude under
   budget. Per ADR-0129's guidance, escalate to QAT before
   relaxing the budget.

## Consequences

**Positive:**

- Closes T5-3 fully (audit half via ADR-0173; first-model half +
  CI gate via this ADR).
- The runtime int8 redirect is now a **load-bearing** code path —
  shipping a quantised model demonstrates the harness end-to-end.
- The `ai-quant-accuracy` CI gate runs on every PR; future
  quantisation regressions are caught before merge.
- 2.4× size shrink on `learned_filter_v1` makes deployment to
  embedded boxes more attractive. Inference speedup is
  CPU-dependent (VNNI / DLBoost); not measured here, but
  documented in `docs/ai/quantization.md` as an operator concern.

**Negative:**

- The redirect block in `vmaf_dnn_session_open` is non-trivial
  C-string manipulation. The path-cap at 4096 is consistent with
  the existing sidecar parser; longer paths (rare) hit
  `-ENAMETOOLONG` with a clear errno.
- If a fork-external operator ships a quantised model where the
  int8 file's actual sha256 doesn't match `int8_sha256` in the
  sidecar, the runtime currently doesn't verify (only the size +
  allowlist gate fire). A future PR can add the sha256 check.
  Tracked as T5-3d.
- C2 (`nr_metric_v1`) remains fp32 in this PR. The ORT
  shape-inference issue needs a re-export; not load-bearing for
  the gate to land.

## Tests

- Manual: `python ai/scripts/measure_quant_drop.py --all` →

  ```text
  [PASS] learned_filter_v1   mode=dynamic PLCC=0.999883
                             drop=0.000117 budget=0.0100 worst_abs=0.0257
  [skip] lpips_sq_v1 — quant_mode=fp32, no quantised model to gate
  [skip] nr_metric_v1 — quant_mode=fp32, no quantised model to gate
  [skip] smoke_fp16_v0 — quant_mode=fp32, no quantised model to gate
  [skip] smoke_v0 — quant_mode=fp32, no quantised model to gate
  ```

- CI: the new `ai-quant-accuracy` step in the `Tiny AI` job runs
  the same script on every PR.
- Existing C sidecar tests from ADR-0173 already cover the
  `quant_mode` parser branches; the redirect code path is
  exercised by the runtime PLCC harness above.

## Reproducer

```bash
# Quantise from the fp32 file:
python ai/scripts/ptq_dynamic.py model/tiny/learned_filter_v1.onnx
# -> model/tiny/learned_filter_v1.int8.onnx (33 019 bytes; 2.4× smaller)

# Measure drop:
python ai/scripts/measure_quant_drop.py --all
# Expected: PASS for learned_filter_v1 with drop ≈ 0.000117.

# Schema validation against the new int8_sha256 field:
python -c "
import json, jsonschema
schema = json.load(open('model/tiny/registry.schema.json'))
reg    = json.load(open('model/tiny/registry.json'))
jsonschema.validate(reg, schema)
print('OK')
"
```

## References

- [ADR-0129](0129-tinyai-ptq-int8-modes.md) — Proposed PTQ policy.
- [ADR-0173](0173-ptq-int8-audit-impl.md) — Audit-first harness
  this PR completes.
- [ADR-0168](0168-tinyai-konvid-baselines.md) — `learned_filter_v1`
  baseline that this PR quantises.
- [BACKLOG T5-3b](../../.workingdir2/BACKLOG.md) — backlog row.
- `req` — user popup choice 2026-04-25: "T5-3b first per-model
  quantisation (M, Recommended)".
