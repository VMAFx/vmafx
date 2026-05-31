<!-- markdownlint-disable MD013 -->
# Research-0135: CHUG vmaf Column Always NaN — Diagnosis and Removal

**Date:** 2026-05-16
**Finding source:** `.workingdir/audit-chug-extraction-cuda-2026-05-16.md` (comprehensive CHUG CUDA extractor audit)
**Scope:** K150K / CHUG full-feature extraction pipeline
**Decision:** Remove `vmaf` from FEATURE_NAMES (Option A) — the model output is neither emitted nor needed for MOS-head training.

## Problem

The CHUG parquet output includes a `vmaf` column in `FEATURE_NAMES` (line 157 of `ai/scripts/extract_k150k_features.py`), but:

1. No extractor in the pipeline — neither CUDA, CPU residual, nor the upstream libvmaf model — emits a `vmaf` JSON key.
2. The `_METRIC_ALIASES` mapping references only `("vmaf",)` but the corresponding value never appears in per-frame metrics.
3. As a result, every frame's lookup for the `vmaf` feature returns `float("nan")` (line 416, `_lookup_metric` fallthrough).
4. Aggregation produces `vmaf_mean = NaN` and `vmaf_std = NaN` for every clip.
5. Two columns (of the parquet's 48 total) are entirely NaN, wasting storage and potentially confusing downstream analysis.

## Root Cause

CHUG uses a **self-vs-self (NR-from-FR adapter)** pattern: the same decoded YUV is passed as both reference and distorted input. In this configuration:

- The VMAF model score requires a `--model` argument (e.g., `--model version=vmaf_v0.6.1`) to be computed.
- The extraction script does NOT pass `--model` to libvmaf; it invokes `vmaf` with **feature-only** extractors via `--feature` flags.
- Consequently, libvmaf outputs per-frame metrics for the 11 extractors (adm, vif, motion, etc.) but **never computes or outputs a `vmaf` model score**.

## Why vmaf Was Added

The column was likely included as a placeholder for a future tiny-AI model output (e.g., a learned VMAF approximation trained on the K150K/CHUG raw features). However:

1. The K150K/CHUG MOS-head training consumes **raw features only**, not model scores.
2. Including an always-NaN column in the parquet schema creates a contract that is never fulfilled.
3. No downstream consumer of the parquet currently expects a `vmaf` column to be populated.

## Options Considered

### Option A: Remove vmaf from FEATURE_NAMES (CHOSEN)

**Pros:**

- Simplifies the schema from 22 to 21 features (44 to 42 feature columns).
- Eliminates two NaN columns from the parquet.
- Removes the implicit contract that a `vmaf` feature is available.
- Fast: one-line change; no logic changes.
- Honest schema: the parquet reflects what is actually extracted.

**Cons:**

- If a future use case wants a tiny-AI VMAF approximation, a new feature name and extractor must be added (e.g., `vmaf_tiny_v1`).
- Does not reuse the `vmaf` slot.

**Effort:** Minimal.

### Option B: Add --model to the extraction invocation

**Pros:**

- Populates the `vmaf` column with real model scores.
- Keeps the current schema structure.

**Cons:**

- Requires running the VMAF model for every clip (O(N) model inference).
- CHUG self-vs-self mode produces trivial model output (high score because ref == dis).
- Adds ~5–10% wall time per clip with no training signal gain (for NR/self-vs-self, the model output is constant or near-constant).
- Violates the principle of "extract only what you use."

**Effort:** Medium (requires parameterizing the vmaf binary invocation and retesting).

## Justification for Option A

For K150K/CHUG MOS-head training:

1. **No training signal:** In self-vs-self mode (ref == dis), the VMAF model is designed to return a high score (close to 100). The per-clip model output would be nearly identical across all clips, providing zero training signal to the MOS head.
2. **Not used downstream:** The current MOS head training loop reads `FEATURE_NAMES` and trains on `<feat>_mean` and `<feat>_std` aggregates. It does not consume a `vmaf` column.
3. **Schema honesty:** The parquet schema should reflect what the extraction pipeline actually emits. Listing `vmaf` when it is always NaN is misleading.
4. **Future extensibility:** If a future recipe needs a VMAF model score or a learned approximation, it can add a new feature (e.g., `vmaf_model_v0_6_1`, `vmaf_tiny_approximation`) with an explicit extractor and proper documentation.

## Changes

1. Remove `"vmaf"` from the `FEATURE_NAMES` tuple (line 157 of `ai/scripts/extract_k150k_features.py`).
2. Remove the `"vmaf": ("vmaf",)` entry from `_METRIC_ALIASES` (line 184), as it is no longer referenced.
3. Update `ai/AGENTS.md` to document the invariant: "FEATURE_NAMES MUST only list columns the extraction pipeline actually emits; never include model outputs or unavailable features."
4. Update parquet schema documentation to reflect 21 features (42 feature columns) instead of 22.

## Reproducer

Before:

```bash
python ai/scripts/extract_k150k_features.py --limit 5
# Parquet schema: clip_name, mos, 22 × (mean, std) = 48 columns
# Column 43-44: vmaf_mean, vmaf_std (always NaN)
```

After:

```bash
python ai/scripts/extract_k150k_features.py --limit 5
# Parquet schema: clip_name, mos, 21 × (mean, std) = 46 columns
# vmaf_mean, vmaf_std removed
```

## References

- **Audit finding:** `.workingdir/audit-chug-extraction-cuda-2026-05-16.md` §Schema completeness for HDR clips, line 221–228.
- **K150K MOS-head training:** `ai/scripts/train_konvid_mos_head.py` (reads FEATURE_NAMES, trains on aggregates).
- **ADR-0362:** Self-vs-self NR-from-FR adapter semantics (negative consequences section).
- **Self-vs-self pattern:** Documented in the script docstring (line 11–12 of `extract_k150k_features.py`).
