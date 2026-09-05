<!-- markdownlint-disable MD013 MD032 MD041 MD060 -->

# Research digest: AI teacher single source, table provenance, and combiner refusal

- **Date**: 2026-09-04
- **Scope**: Aligning AI distillation pipelines with the fork default model (ADR-1168)
- **Governing ADR**: [ADR-1173](../adr/1173-ai-teacher-follows-default-model.md)
- **Status**: Implemented and verified

## 1. Context and Problem Statement

When ADR-1168 established `VMAF_DEFAULT_MODEL_VERSION` in `core/include/libvmaf/model.h`
(mirrored in `tools/vmaf-tune/src/vmaftune/defaultmodel.py` as `DEFAULT_MODEL`), it unified
the CLI, MCP, and tuning surfaces. However, the `ai/` subtree was left out:
`scripts/ci/check-default-model-single-source.sh` included a blanket exemption `^ai/` in
`allow_re`.

Consequently, multiple AI feature extraction and corpus preparation scripts continued to
hardcode `vmaf_v0.6.1`. Furthermore, extracted feature tables (Parquet) recorded no
teacher provenance on disk. This created two risks:

1. Distilled models trained on newer datasets would inadvertently drift between
   `vmaf_v0.6.1` and `vmaf_v1.0.16_3d0h`.
2. Multi-dataset combiners could silently merge tables scored with different teacher models,
   contaminating regression targets.

## 2. Architecture of the Resolution

### 2.1 Single-Source Resolver (`ai/data/scores.py`)

`ai.data.scores.resolve_teacher_model()` handles resolution with deterministic precedence:

1. Explicit caller argument (`--vmaf-model`, `--model`, `model=`)
2. Environment variable `$VMAF_MODEL_PATH` (accepting a model file or version name; directory paths are ignored)
3. Single-source default imported from `vmaftune.defaultmodel.DEFAULT_MODEL`

The returned `ResolvedTeacherModel` provides:

- `arg`: Formatted CLI argument (e.g. `version=vmaf_v1.0.16_3d0h` or `path=/path/to/model.json`)
- `name`: Clean model identifier string (e.g. `vmaf_v1.0.16_3d0h` or stem)
- `is_path`: Boolean indicating whether a file path is used
- `resolved`: Canonical resolved target

### 2.2 Row-Level Provenance and Combiner Refusal

Every feature extractor now writes a `teacher_model` string column on each row:

- `extract_full_features.py`
- `extract_k150k_features.py`
- `bvi_dvc_to_full_features.py`
- `extract_ugc_features.py`
- `konvid_to_full_features.py`
- `konvid_to_vmaf_pairs.py`
- `bvi_dvc_to_corpus_jsonl.py`

`combine_full_feature_parquets.py`, `train_vmaf_tiny_v5.py`, and `eval_loso_vmaf_tiny_v5.py`
enforce:

1. Every input table must contain a `teacher_model` column, or `--assume-teacher <name>`
   must be explicitly provided.
2. All rows within a table must share the same teacher model.
3. All input tables must share the exact same teacher model.

If any mismatch is detected, processing fails fast with an explicit error.

### 2.3 Feature Space Alignment (`adm3`)

- Appended `"adm3"` to `FULL_FEATURES` in `ai/data/feature_extractor.py` (expanding
  from 22 to 23 features).
- Appended `"adm3"` to `FEATURE_NAMES` in `ai/scripts/extract_k150k_features.py`.
- Preserved the canonical-6 `DEFAULT_FEATURES` (`adm2`, `vif_scale0..3`, `motion2`)
  for student models intact.

### 2.4 CI Single-Source Gate

Removed `^ai/` from `allow_re` in `scripts/ci/check-default-model-single-source.sh`. Added
test case 4b in `scripts/ci/tests/test-default-model-single-source.sh` asserting that any
unapproved hardcoded default model literal in `ai/scripts/` causes immediate gate failure.

## 3. Verification Evidence

1. **Single-Clip CPU Smoke**:
   - Running `scores.py` against golden test pair `src01_hrc00_576x324.yuv` and `src01_hrc01_576x324.yuv`:
     - Default teacher: `vmaf_v1.0.16_3d0h` -> pooled VMAF `82.816059`
     - Explicit override `--model vmaf_v0.6.1`: -> pooled VMAF `76.667831` (matches Netflix golden)
2. **Combiner Refusal Unit Tests**:
   - `ai/tests/test_combine_full_feature_parquets.py`: 8/8 passed, confirming refusal of mixed-teacher tables and legacy unprovenanced tables without `--assume-teacher`.
3. **CI Gate Tests**:
   - `scripts/ci/tests/test-default-model-single-source.sh`: 23/23 test cases passed.
