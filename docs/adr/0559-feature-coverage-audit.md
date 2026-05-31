<!-- markdownlint-disable MD013 MD036 MD060 -->
# ADR-0559: Feature Coverage Audit — Add speed_chroma + speed_temporal to Extraction Scripts (HDR-model prep)

- **Status**: Accepted
- **Date**: 2026-05-18
- **Deciders**: lusoris, Claude Code agent
- **Tags**: ai, feature-extraction, speed, hdr, corpus, fork-local

## Context

The cross-backend parity matrix (PR #1328 / ADR-0555) flagged `speed_chroma`
and `speed_temporal` as CPU-only extractors with no GPU twin. Separately, the
Netflix `speed_ported` upstream branch signals that these two extractors are
the most likely inputs to a future Netflix HDR VMAF model. To be in a position
to evaluate such a model against this fork's corpora — or to train a fork-owned
HDR surrogate — the extraction scripts must include these features.

A systematic audit (see research digest
`docs/research/feature-coverage-audit-2026-05-18.md`) found:

1. `speed_chroma` and `speed_temporal` exist as CPU-only extractors in
   `core/src/feature/speed.c` and are registered in
   `feature_extractor_list[]` under `#if VMAF_FLOAT_FEATURES`.
2. Only `ai/scripts/extract_k150k_features.py` already included them (added
   2026-05-15). All other extraction scripts (`chug_extract_features.py`,
   `extract_full_features.py`, `bvi_dvc_to_full_features.py`) omit them.
3. All current corpus JSONL files lack populated speed-feature columns: either
   the columns are absent entirely, or they are present but all-NaN from a
   run that predated the extractor addition.
4. No shipped SVM or tiny-AI ONNX model currently consumes speed features.
5. No HDR VMAF model exists in the Netflix upstream tree as of 2026-05-18.

## Decision

1. Add `speed_chroma` and `speed_temporal` to the `FULL_FEATURES` tuple in
   `ai/data/feature_extractor.py` and to the `_METRIC_TO_EXTRACTOR` lookup
   map, so all scripts that import from that module pick up the change.
2. Add speed features explicitly to `bvi_dvc_to_full_features.py`'s local
   `FULL_FEATURES` copy and `EXTRACTORS` tuple (this script does not import
   from the shared module).
3. Add speed features to `chug_extract_features.py`'s `FULL_FEATURES` set
   via the shared module update (the script uses `FULL_FEATURES` from the
   module; no local copy to patch).
4. Do NOT trigger any actual re-extraction in this PR — corpus re-extraction
   is expensive and is the responsibility of the corpus-reextraction-assessment
   agent (a8f22d538ea137ac0). This PR only ensures new runs pick up the
   features.
5. Add a coverage-gap note to the `konvid_mos_head_v1` model card noting that
   speed features were not part of its training feature set.
6. Column ordering: speed features are appended at the END of the feature
   tuples in all scripts to preserve the parquet schema version lock described
   in `ai/AGENTS.md §K150K-A corpus extraction invariants`.

## Alternatives Considered

| Option | Pros | Cons |
|---|---|---|
| Add to every script individually (no shared module change) | Surgical, no shared-state risk | Drift risk — future scripts won't inherit them |
| Add only to CHUG script (highest corpus priority) | Minimal scope | BVI-DVC / Netflix scripts remain stale |
| Trigger a full corpus re-extract in this PR | Corpora immediately populated | Hours of GPU time; outside this PR's scope; re-extract agent already tasked |
| Wait for GPU twins before adding to scripts | Clean GPU/CPU parity | Blocking: GPU twins are in parallel PRs; scripts should not be gated on GPU |

Chosen option: update shared module + patch scripts individually where local
copies exist. Re-extract deferred to the corpus agent.

## Consequences

**Positive**

- All extraction scripts that consume `FULL_FEATURES` from the shared module
  will automatically include speed features on the next run.
- Future corpus runs produce populated `speed_temporal` / `speed_chroma_u/v/uv`
  columns without requiring another script-level patch.
- When the Netflix HDR model lands (or the fork trains one), the corpus
  extraction infrastructure is already correct.

**Negative**

- Existing corpora remain stale until re-extract. Training code must tolerate
  NaN in speed columns for backward compatibility (already the case per
  `ai/data/feature_extractor.py` NaN-handling contract).
- The `bvi_dvc_to_full_features.py` local `FULL_FEATURES` copy drifts one
  update behind until someone consolidates the two copies. The comment
  "Keep in sync" is already present; this ADR documents the divergence.

**Neutral / Follow-ups**

- GPU twins for `speed_chroma` and `speed_temporal` are tracked in ADR-0557
  (CUDA) and ADR-0558 (HIP) by parallel agents; this ADR is independent.
- `vmaf_tiny_v1` / `vmaf_tiny_v1_medium` lack model cards; that gap is noted
  in the research digest but not addressed here (pre-dates ADR-0042; low risk
  since v1 is superseded by v2–v4).

## References

- `docs/research/feature-coverage-audit-2026-05-18.md` — full audit
- ADR-0555 (cross-backend parity matrix, identified the GPU twin gap)
- ADR-0557 (speed_temporal CUDA port, parallel agent)
- ADR-0558 (speed_chroma GPU port, parallel agent)
- ADR-0346 (FR-from-NR adapter, K150K extraction context)
- ADR-0382 (K150K-A parallelism, where speed features were first added)
- `core/src/feature/speed.c` — extractor implementation
- Netflix upstream `speed_ported` branch — upstream context
- `req: feature-coverage-audit-2026-05-18` (task directive)
