<!-- markdownlint-disable MD013 MD060 -->
# Research-0135 — CHUG vmaf column: SDR baseline via vmaf_v0.6.1

**Date:** 2026-05-16
**Status:** Accepted (supersedes PR #898 Option A draft)
**Scope:** `ai/scripts/extract_k150k_features.py`, `FEATURE_NAMES`, CHUG/K150K parquet schema

---

## Background

PR #898 audited the CHUG/K150K feature extraction pipeline and found that the `vmaf`
column in the output parquet was always NaN.  The root cause was that no `--model`
argument was passed to the libvmaf CLI invocation, so libvmaf ran feature extractors
only and never dispatched the composite model.  PR #898 proposed Option A: remove
`vmaf` from `FEATURE_NAMES` entirely.

On 2026-05-16, the user overrode that approach with a direction to implement Option B:
compute the vmaf score explicitly via the SDR `vmaf_v0.6.1` model, document the
HDR-calibration caveat, and keep the column in the schema.

---

## Root cause (inherited from PR #898 audit)

The libvmaf CLI emits a `vmaf` JSON key per frame only when a composite model is
loaded via `--model`.  Without `--model`, the output contains only the raw feature
extractor values.  The extraction pipeline called the CLI with `--feature adm_cuda`,
`--feature vif_cuda`, etc. but never `--model version=vmaf_v0.6.1`.  Result: the
`vmaf` column in every produced parquet was `NaN` for every clip.

---

## Options considered

| | Option A — drop the column | Option B — compute it (chosen) |
|---|---|---|
| **Implementation** | Remove `"vmaf"` from `FEATURE_NAMES` and `_METRIC_ALIASES` | Add `--model version=vmaf_v0.6.1` to CUDA and CPU invocations |
| **Parquet schema** | 46 columns (22 features → 21, × 2 aggregates + clip_name + mos) | 48 columns unchanged |
| **Training signal** | No vmaf relationship in CHUG rows | vmaf relationship preserved for bitrate-ladder ranking |
| **HDR calibration** | N/A (not present) | Mis-calibrated on PQ HDR — SDR model, not HDR-aware |
| **CUDA overhead** | None | ~5–10 % additional wall-clock per clip for model dispatch |
| **Upgrade path** | Re-add column in a future PR when HDR model ships | Replace model arg when HDR model ships; column stays |
| **Schema stability** | Breaks downstream parquets trained expecting 48 columns | No break; schema unchanged |
| **User direction** | PR #898 initial | Per user 2026-05-16: "we need the vmaf relationship" |

**Decision: Option B** — per user direction 2026-05-16.  The vmaf relationship across
bitrate-ladder rungs is meaningful even with SDR calibration because ladder rows
within the same content share the same HDR mis-calibration offset, making relative
comparisons valid.

---

## HDR caveat

`vmaf_v0.6.1` was trained on SDR content (BT.709, SDR transfer).  When applied to
PQ HDR clips (SMPTE ST 2084, BT.2020), the model receives signal on a different
luminance scale.  Scores will be systematically lower than for equivalent-quality SDR
content and should not be compared against published SDR VMAF targets.

Within a CHUG bitrate ladder (same source content, same HDR encoding), all ladder
rungs share the same mis-calibration direction, so the model's **relative ordering**
of rungs remains meaningful for training a MOS-head regressor.  The absolute vmaf
values are not meaningful as HDR quality targets.

**Upgrade path:** when the Netflix HDR vmaf model ships, replace
`version=vmaf_v0.6.1` with the HDR model name in `_run_feature_passes`.  No schema
change is required.

---

## Performance impact

The `vmaf_v0.6.1` model dispatch runs on top of the existing feature extractor pass.
Per the PR #898 audit, adding `--model` to the CUDA invocation costs approximately
5–10 % additional wall-clock time per clip.  For a 150k-clip corpus at the default
8-worker parallelism, the additional cost is on the order of 30–60 minutes per full
extraction run.  This is an accepted trade-off (user direction 2026-05-16).

---

## Implementation

`_run_feature_passes` in `ai/scripts/extract_k150k_features.py` now appends
`["--model", "version=vmaf_v0.6.1"]` to both the CUDA backend args and the CPU
`no_cuda`/`no_sycl`/`no_vulkan` args.  This causes the libvmaf CLI to load the model
graph, run the composite score, and emit a `"vmaf"` key in each frame's `metrics`
dict.  The existing `_METRIC_ALIASES` entry `"vmaf": ("vmaf",)` picks it up without
any further change.

---

## Verification

Five-clip before/after demonstration (using the monkeypatched unit tests):

- **Before** (no `--model` arg): `vmaf_mean = NaN`, `vmaf_std = NaN` in every row.
- **After** (`--model version=vmaf_v0.6.1`): `vmaf_mean` and `vmaf_std` are finite
  floats; `test_vmaf_column_non_nan_in_aggregated_output` asserts this.

The tests `test_cuda_feature_passes_split_gpu_and_cpu_residual` and
`test_cpu_feature_pass_uses_generic_extractors` both assert that
`"version=vmaf_v0.6.1"` appears in the model args of every vmaf invocation.

---

## References

- PR #898 (superseded Option A): `fix(ai): remove always-NaN vmaf column from CHUG
  extraction`
- User direction 2026-05-16: the vmaf relationship must be preserved in CHUG output
- ADR-0346: FR-from-NR adapter pattern
- ADR-0362: NaN columns documented for identity pairs
- ADR-0382: K150K-A parallelism + CUDA split invariant
- ADR-0427: CHUG feature materialiser policy
