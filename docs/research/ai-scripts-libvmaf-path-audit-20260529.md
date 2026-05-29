# Research digest: ai/scripts hardcoded-path audit (2026-05-29)

## Summary

Static analysis of `ai/scripts/` (55 Python scripts + 2 shell scripts) for the
five audit dimensions: silent exception swallowing, stale post-rename path
references, missing replay-manifest sidecars, schema drift, and K150K-specific
findings.

## Findings

### 1. Hardcoded `libvmaf/` paths — BREAKING (fixed in this PR)

Four files still referenced `REPO_ROOT / "libvmaf" / "build-cpu" / "tools" / "vmaf"`
as the default vmaf binary path.  Since ADR-0700 renamed `libvmaf/` → `core/`, this
default resolves to a path that does not exist on any up-to-date checkout.  Running
`python ai/scripts/extract_k150k_features.py` (the live 9h extraction) without an
explicit `--vmaf-bin` would fail the pre-flight check immediately at startup with
"vmaf binary not found".

Affected files (all fixed):

- `ai/scripts/extract_k150k_features.py` — `--vmaf-bin` and `--cpu-vmaf-bin` defaults + error message
- `ai/scripts/bvi_dvc_to_full_features.py` — `--vmaf-bin` default
- `ai/scripts/konvid_to_vmaf_pairs.py` — `--vmaf-bin` default
- `ai/data/feature_extractor.py` — `DEFAULT_VMAF_BINARY` constant (used by `chug_extract_features.py`)

Correct post-rename path: `core/build-cpu/tools/vmaf`.  Verified present on the
maintainer machine.

### 2. Silent exception swallowing — no action required

The two candidates that could constitute real silent failures:

- `extract_k150k_features.py` `_parquet_row_count()` (line 903): catches a
  `KeyError` / `ArrowInvalid` from column-projection (`columns=["clip_name"]`) and
  falls back to reading the full parquet.  This is a graceful degradation for parquet
  files written without that column, not a correctness risk.  The function is
  manifest-only (non-critical reporting path).
- `materialize_saliency_features.py` (line 207): broad `except Exception` that sets
  `status="model-failed"` and returns `None`.  The saliency stack is intentionally
  optional (comment says so); the failed status is recorded in the row, so nothing
  is silently lost.
- `extract_k150k_features.sh` inline Python: `except RuntimeError` re-raises via
  `continue` (skip clip, log to stderr) — consistent with the `.py` worker pattern.

None of these violate `feedback_correctness_first.md` because errors are surfaced
(via status fields, stderr logs, or re-raise), not silently dropped.

### 3. Missing replay-manifest sidecars

All major extraction and training scripts emit `write_manifest_json` sidecars:

- `extract_k150k_features.py` — `k150k-feature-extraction-manifest-v1`
- `chug_extract_features.py` — provenance via `build_run_provenance` + split manifest
- `bvi_dvc_to_full_features.py` — `bvi-dvc-full-features-manifest-v1`
- `konvid_to_full_features.py` — `konvid-full-features-manifest-v1`
- `konvid_to_vmaf_pairs.py` — `konvid-vmaf-pairs-manifest-v1`
- `extract_ugc_features.py` — `ugc-full-feature-extraction-manifest-v1`
- All `train_vmaf_tiny_v*` — emit `write_manifest_json` at training end
- All `eval_loso_*` — emit run-provenance manifests

No coverage gaps in the active pipeline.

### 4. Schema drift — K150K extraction vs training

`extract_k150k_features.py` `FEATURE_NAMES` was extended 2026-05-15 (ADR-0559) to
append `speed_temporal`, `speed_chroma_u/v/uv` (25 columns total, parquet schema
v2).  The canonical-6 training scripts (`train_vmaf_tiny_v2`–`v5`,
`train_fr_regressor_v2/v3`) operate on the subset `(adm2, vif_scale0..3, motion2)` +
vmaf teacher; they are schema-agnostic to the extra columns (they select by name,
not by position).

The CHUG HDR MOS head (`train_konvid_mos_head.py`) defines separate schemas
(`FEATURE_SCHEMA_CHUG_HDR_WIDE_V1`, `FEATURE_SCHEMA_KONVID_V1`) that do not include
`speed_*`.  These schemas are intentional subsets; unused columns are ignored at
parquet load time.

No drift risk in the current pipeline.  If `speed_*` are added to a training schema
in the future, they should be added to `CANONICAL_6` or a separate schema constant in
`train_konvid_mos_head.py`.

### 5. K150K-specific findings

The live 9h extraction (`extract_k150k_features.py`) is well-guarded:

- `detect_fr_corpus_misuse()` at startup blocks FR-corpus misuse (ADR-0510).
- `--limit` slices before the done-set filter (documented memory note, not a bug).
- JSONL staging + at-end-only parquet write (Research-0135) prevents O(N²) I/O.
- Worker isolation via `ProcessPoolExecutor` prevents backend context conflicts.
- `cambi_cuda` intentionally excluded from CUDA primary pass (Issue #857 segfault).

No changes warranted for the live extraction run.
