<!-- markdownlint-disable MD013 MD060 -->
# ADR-0173: PTQ int8 audit implementation — registry schema + scripts + CI gate (T5-3)

- **Status**: Accepted
- **Date**: 2026-04-25
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: tiny-ai, onnx, quantization, registry, ci, fork-local

## Context

[ADR-0129](0129-tinyai-ptq-int8-modes.md) (Proposed) defined the
**policy**: per-model `quant_mode` field in
`model/registry.json`, three modes (`static`, `dynamic`, `qat`),
calibration-set field for static, and a CI accuracy gate. The ADR
explicitly mandated an **"audit-first" sequence**:

> 1. Audit PR: extend the registry schema, add the three scripts,
>    add the CI accuracy-gate leg, but do not change any existing
>    model's `quant_mode` from `fp32`. Purely infrastructural.
> 2. Per-model quantisation PRs: one PR per model, each with its
>    own accuracy-drop measurement.

This ADR is the audit-first PR. No model in the repo flips its
`quant_mode` here; the harness lands so the per-model PRs that
follow have a place to plug in.

## Decision

### 1. Registry schema extension

[`model/tiny/registry.schema.json`](../../model/tiny/registry.schema.json)
gains three optional fields per model entry:

| Field | Type | Default | Purpose |
|---|---|---|---|
| `quant_mode` | enum `fp32` / `static` / `dynamic` / `qat` | `fp32` | Selects the int8 path. `fp32` = ship the .onnx as-is (current behaviour). |
| `quant_calibration_set` | string (path relative to repo root) | absent | Required for `quant_mode: "static"`; the calibration tensor blob. |
| `quant_accuracy_budget_plcc` | number in `[0, 1]` | `0.01` | Maximum allowed Pearson-correlation drop on the per-model VMAF soak fixture vs the fp32 baseline. The CI `ai-quant-accuracy` job fails any quantised model that exceeds this budget. |

Existing entries in `registry.json` are unchanged — JSON schema
defaults make the new fields optional. The default `quant_mode`
of `fp32` preserves the current loader behaviour for every model
shipped today.

### 2. Three quantisation scripts under `ai/scripts/`

- **`ptq_dynamic.py`** — wraps
  `onnxruntime.quantization.quantize_dynamic`. Single-arg call;
  no calibration data needed. Output: `<input>.int8.onnx` next
  to the fp32 source.
- **`ptq_static.py`** — wraps `quantize_static` with a
  `CalibrationDataReader` that yields per-input slices from a
  numpy `.npz` file. Format: one entry per ONNX input name, each
  containing a stack of `[N, ...]` representative inputs. The
  calibration path comes from the registry's
  `quant_calibration_set` field (or a CLI override).
- **`qat_train.py`** — **scaffold only** for this PR. Wires the
  CLI surface and prints a "QAT integration is scaffolded but not
  yet wired into the Lightning trainer" message. The follow-up
  PR that runs QAT on a concrete model lands the trainer hook
  alongside its accuracy-drop measurement, per ADR-0129's
  audit-first sequence (the trainer extension and the model
  evaluation are the same review unit).

All three scripts emit `<input>.int8.onnx` next to the fp32
source. Sidecar JSONs are NOT auto-updated; the per-model PR
that flips `quant_mode` in `registry.json` also updates the
matching sidecar.

### 3. Sidecar parser + new public enum

[`core/src/dnn/model_loader.h`](../../core/src/dnn/model_loader.h)
gains a `VmafModelQuantMode` enum (FP32 / DYNAMIC / STATIC / QAT)
and a matching field on `VmafModelSidecar`. The C-side parser at
`vmaf_dnn_sidecar_load` reads the new `quant_mode` string from the
sidecar JSON; unknown values fall back to FP32 (fail-safe default).

The follow-up PR that flips a model to int8 will also wire the
loader to prefer `<basename>.int8.onnx` when the sidecar's
`quant_mode != FP32`. That logic isn't in this audit-first PR
because no model needs it yet — landing the load-redirection logic
without a model that exercises it would be untested code.

### 4. CI `ai-quant-accuracy` gate (deferred to follow-up)

ADR-0129 calls for a new CI leg that runs the quantised model
against the per-model VMAF soak fixture and asserts the PLCC drop
is below `quant_accuracy_budget_plcc`. **Not in this PR**: the gate
needs (a) at least one quantised model checked in, and (b) a
soak-fixture pinned in `python/test/`. Both arrive in the per-model
quantisation PRs that follow. Tracked as T5-3b.

This audit-first PR is therefore intentionally limited to the
**static surfaces** — schema, scripts, sidecar parser, docs. The
moving CI leg lands when there's a model to gate.

## Alternatives considered

1. **Land everything in one mega-PR** — schema, scripts, gate,
   first quantised model. Rejected explicitly by ADR-0129's
   audit-first directive: each quantisation decision should be
   reviewable in isolation against its own accuracy measurement.
2. **Skip the QAT scaffold** until a model needs it. Rejected: the
   CLI surface should exist now so future operators discover the
   path; the `NotImplementedError` body is the right shape for an
   "intentionally incomplete" stub (audit trail in
   `git log -- ai/scripts/qat_train.py`).
3. **Embed quant_mode in the sidecar JSON only** (skip the
   registry field). Rejected for the same reason ADR-0129
   rejected it: the registry is the trust root, and per-model
   audit needs the field at registry level so a registry-only
   reader can answer "what's quantised?" without opening every
   sidecar.
4. **Default to `quant_mode: dynamic` for new models**. Rejected:
   the audit-first sequence wants every quantisation to be a
   conscious per-model decision with its own PLCC measurement.
   Default `fp32` keeps that property by construction.

## Consequences

**Positive:**

- Closes the "policy → code" gap from ADR-0129 (Proposed) without
  changing any shipped model's behaviour.
- Per-model quantisation PRs (T5-3b, T5-3c, ...) now have a clear
  landing surface: edit registry entry, run `ptq_*.py`, attach the
  PLCC drop in the PR description.
- The C-side enum + sidecar field land before any model uses them,
  so the loader stays trivially compatible with old sidecars
  (default FP32).

**Negative:**

- Three Python scripts that are partially exercised by this PR
  (only `ptq_dynamic.py` is a pure wrapper; `ptq_static.py` needs
  a real calibration set; `qat_train.py` is a scaffold).
  Acceptable per the audit-first sequence.
- CI accuracy gate is not yet wired. T5-3 is therefore not 100%
  closed by this PR — it lands the harness; the gate lands with
  the first quantised model. Tracked explicitly in BACKLOG as
  T5-3b.

## Tests

- `ai/tests/test_ptq_scripts.py` (new) — smoke that `ptq_dynamic.py`
  `ptq_static.py` import cleanly and surface useful CLI help. The
  full quantisation round-trip needs `onnxruntime.quantization`
  installed; the test marker auto-skips if not.
- `core/test/dnn/test_model_loader.c` (extended) — new sub-test
  that parses a sidecar JSON with `"quant_mode": "dynamic"` and
  asserts `out->quant_mode == VMAF_QUANT_DYNAMIC`. Also covers the
  unknown-value fallback (`"foo"` → FP32) and the absent-field
  default.

## References

- [ADR-0129](0129-tinyai-ptq-int8-modes.md) — Proposed policy this
  ADR implements.
- [Research-0006](../research/0006-tinyai-ptq-accuracy-targets.md) —
  accuracy regression targets, ORT API comparison.
- [BACKLOG T5-3 / T5-3b](../../.workingdir2/BACKLOG.md) — backlog
  rows; T5-3b is the new follow-up for the CI accuracy gate.
- [ONNX Runtime quantization docs](https://onnxruntime.ai/docs/performance/model-optimizations/quantization.html)
- `req` — user popup choice 2026-04-25: "T5-3 PTQ int8 audit (M,
  Recommended)".
