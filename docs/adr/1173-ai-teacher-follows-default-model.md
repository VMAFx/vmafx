<!-- markdownlint-disable MD013 MD032 MD060 -->
# ADR-1173: AI Teacher Model Follows Default Model Single Source

- **Status:** Accepted
- **Date:** 2026-09-04
- **Deciders:** Lusoris
- **Supersedes:** none
- **Superseded by:** none

## Context

[ADR-1168](1168-default-model-single-source.md) established `VMAF_DEFAULT_MODEL_VERSION`
in `core/include/libvmaf/model.h`, mirrored in `tools/vmaf-tune/src/vmaftune/defaultmodel.py`
as `DEFAULT_MODEL`, as the single source of truth for the fork's default model
(`vmaf_v1.0.16_3d0h`).

However, the AI training and feature extraction pipelines (`ai/`) had been hardcoding
`"vmaf_v0.6.1"` across extractors (`extract_full_features.py`, `extract_k150k_features.py`,
`bvi_dvc_to_full_features.py`, `extract_ugc_features.py`, `konvid_to_full_features.py`,
`konvid_to_vmaf_pairs.py`, `bvi_dvc_to_corpus_jsonl.py`), dataset scripts, and documentation.
Furthermore, `scripts/ci/check-default-model-single-source.sh` carried a blanket exemption
`^ai/` in its `allow_re` filter, allowing hardcoded teacher fallbacks to proliferate without
CI checks.

This created two major issues:

1. **Model drift**: Whenever the fork updated its default model version, AI distillation
   pipelines quietly remained on the legacy `v0.6.1` model, creating a split brain between
   the fork's official scoring baseline and the teacher labels used to train distilled models.
2. **Missing data provenance and mixed-teacher corruption**: Extracted parquet feature tables
   did not stamp which teacher model produced the ground-truth VMAF scores. Combining shards
   or training on multi-dataset corpora could silently blend scores computed under different
   teacher models (e.g. mixing `v0.6.1` and `v1.0.16_3d0h`), producing invalid regression
   targets without any warning or error.

Additionally, raw feature tables needed `"adm3"` appended to `FULL_FEATURES` and K150K
`FEATURE_NAMES` for downstream research, while the student model's canonical-6 feature
contract (`DEFAULT_FEATURES`: `adm2`, `vif_scale0`..`vif_scale3`, `motion2`) had to remain
strictly preserved.

## Decision

1. **AI teacher model resolves from the ADR-1168 single source**:
   `ai.data.scores.resolve_teacher_model()` is the central resolver. It imports
   `DEFAULT_MODEL` from `vmaftune.defaultmodel`.
2. **Deterministic precedence order**:
   - Explicit caller/CLI argument (`--vmaf-model`, `--model`, `model=`)
   - Environment variable `$VMAF_MODEL_PATH` (accepting either a model name or direct path)
   - Single source default (`DEFAULT_MODEL` from `vmaftune.defaultmodel`)
   - The resolver returns a `ResolvedTeacherModel(arg, name, is_path, resolved, path)`
     named tuple providing both canonical identifier and path resolution.
3. **Teacher provenance stamped on every feature row**:
   All feature producers (`extract_full_features.py`, `extract_k150k_features.py`,
   `bvi_dvc_to_full_features.py`, `extract_ugc_features.py`, `konvid_to_full_features.py`,
   `konvid_to_vmaf_pairs.py`, `bvi_dvc_to_corpus_jsonl.py`) write a `teacher_model` column
   on every row and record teacher model metadata in run manifests.
4. **Combiners and trainers refuse mixed-teacher tables**:
   `combine_full_feature_parquets.py`, `train_vmaf_tiny_v5.py`, and
   `eval_loso_vmaf_tiny_v5.py` check teacher model uniformity within each table and across
   all input tables. If multiple teacher models are detected, execution is refused with
   a fatal error. Parquet tables without a `teacher_model` column are refused unless the
   operator explicitly supplies `--assume-teacher <name>` for legacy ingestion.
5. **Raw full feature tables append `adm3`**:
   `FULL_FEATURES` in `ai/data/feature_extractor.py` and `FEATURE_NAMES` in
   `ai/scripts/extract_k150k_features.py` append `"adm3"`. The student model's
   canonical-6 `DEFAULT_FEATURES` is unmodified.
6. **CI single-source gate enforces `ai/`**:
   The blanket `^ai/` exemption is removed from
   `scripts/ci/check-default-model-single-source.sh`. Any unapproved hardcoded default
   model literal in `ai/` fails the gate, as verified by
   `scripts/ci/tests/test-default-model-single-source.sh`.

## Consequences

- **Positive**: AI teacher distillation automatically stays aligned with the fork's
  canonical default model version.
- **Positive**: Every feature dataset carries row-level teacher provenance. Mixed-teacher
  corruption is impossible without deliberate operator circumvention.
- **Positive**: The CI gate actively catches any reintroduced hardcoded model defaults
  in `ai/`.
- **Negative**: Legacy parquet files generated before this change lack a `teacher_model`
  column and cannot be combined or evaluated without passing `--assume-teacher`.
- **Neutral / follow-ups**: Existing offline corpora will be backfilled or ingested with
  `--assume-teacher vmaf_v0.6.1`.

## Alternatives considered

| Option | Pros | Cons | Verdict |
| --- | --- | --- | --- |
| **Follow single source with row provenance and combiner refusal (chosen)** | Automatic alignment with fork default, row-level provenance, strict refusal of mixed targets, CLI override flexibility | Requires `--assume-teacher` for legacy parquets lacking provenance | Selected: robust, reproducible, and aligns with ADR-1168 architecture |
| Keep `vmaf_v0.6.1` permanently hardcoded in AI | No changes needed to existing training scripts | Permanent split-brain; student models distilled from obsolete teacher models; ignores ADR-1168 | Rejected |
| Silent fallback / lenient combining of mismatched teacher tables | Backward-compatible with mixed or legacy datasets | Silently trains on corrupted target distributions; impossible to trace score lineage | Rejected: violates fail-fast data integrity principles |
| Add `adm3` to student `DEFAULT_FEATURES` | Exposes 3rd-order ADM to student model | Alters student architecture, increases tiny-model parameter count and inference cost, breaks ONNX parity | Rejected: student contract canonical-6 is locked |

## References

- req — Align AI training and feature extraction pipelines with single source of truth; provide CLI overrides; stamp provenance; refuse mixed tables.
- [ADR-1168](1168-default-model-single-source.md) — The default VMAF model is defined in exactly one place.
- [ADR-0024](0024-netflix-golden-preserved.md) — Netflix golden preserved.
- [ADR-0108](0108-deep-dive-deliverables-rule.md) — Deep-dive deliverables rule.
- [ADR-0221](0221-changelog-adr-fragment-pattern.md) — Changelog and ADR fragment pattern.
