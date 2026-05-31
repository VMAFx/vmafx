<!-- markdownlint-disable MD013 MD060 -->
# ADR-0388: Ingest BVI-CC as the second tiny-AI training corpus

- **Status**: Draft
- **Date**: 2026-05-02
- **Deciders**: TBD
- **Tags**: ai, fr-regressor, corpus, license, bristol

## Context

`fr_regressor_v1` (T6-1a, commit `e421d70`) trains on the Netflix
Public corpus only — 9 reference + 70 distorted clips, single MOS
convention, no explicit codec axis. The codec-aware
`fr_regressor_v2` plan in [ADR-0235](0235-codec-aware-fr-regressor.md)
needs subjective labels paired with a real codec sweep. The
companion research digest
[Research-0046](../research/0046-bristol-vi-lab-feasibility.md)
walks the Bristol VI-Lab BVI-* family and concludes that
**BVI-CC** is the smallest useful candidate: 9 references × 34
distorted variants across HM, AV1, VTM at four resolutions, with
DMOS labels.

The decision is whether to make BVI-CC the *first* Bristol corpus
ingest for the fork, ahead of BVI-DVC (no labels), BVI-AOM (no
labels, mixed-licence subset), or BVI-HD (single codec only).

## Decision

We will ingest BVI-CC as the second tiny-AI training corpus,
behind Netflix Public, in a single dedicated PR. The PR ships:
the JSON manifest at
`ai/src/vmaf_train/data/manifests/bvi-cc.json`, a
`mos_convention` column in the feature parquet schema, loader
support for inverted-DMOS-to-MOS normalisation, a per-clip
licence flag column (carried for forward-compatibility with
BVI-AOM), and the user-facing doc page at
`docs/ai/training-data.md`. Raw clips remain gitignored under
`.workingdir2/bristol/bvi-cc/`. The parquet itself is a
local-build artifact, not committed.

## Alternatives considered

| Option                                  | Pros                                                                                  | Cons                                                                                                                  | Why not chosen |
| --------------------------------------- | ------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------- | -------------- |
| **BVI-CC first** (chosen)               | DMOS labels; explicit HM / AV1 / VTM codec axis; small enough (~250–400 GB) to fit on one disk; CC-BY on derived data | Form-gated download (~2 day SLA); DMOS↔MOS conversion adds one normalisation step                                     | — |
| BVI-DVC first                           | Largest sequence count (800); widest resolution range; useful for parity soak         | No subjective labels — does not move fr_regressor_v2 forward; 700 GB – 1.2 TB footprint                               | Wrong corpus for the labelled-MOS goal; can come later for a separate parity-soak PR |
| BVI-AOM first                           | Authoritative size figure (124 GB packed); clean direct-S3 download (no form)         | No subjective labels; mixed licence — CableLabs subset is CC-BY-NC-ND 3.0 (no derivatives), needs per-clip licence map | Same labelling gap as BVI-DVC plus a licence-complexity tax we don't need on the first ingest |
| BVI-HD first                            | DMOS labels; simpler than BVI-CC                                                      | Single codec (HEVC + HEVC-SYNTH); no AV1 / VVC presence                                                               | Doesn't exercise the codec one-hot the v2 regressor was designed for |
| Stay on Netflix Public, defer BVI       | Zero new licence work; zero new disk                                                  | Blocks ADR-0235 indefinitely; cross-backend soak still has only 3 pairs                                               | Defers the actual problem |
| Pull two corpora at once (BVI-CC + AOM) | One round-trip                                                                        | TB-scale storage commitment; AOM licence map needs separate review; harder to attribute lift in v2                    | Violates the "one decision per PR" principle |

## Consequences

- **Positive**: `fr_regressor_v2` gains a labelled codec sweep
  with three encoders; the parquet schema gains a
  `mos_convention` column, unblocking any future MOS-bearing
  corpus from any lab; `docs/ai/training-data.md` becomes the
  single index of what the fork has ingested.
- **Positive**: A 10-clip BVI-CC subset becomes available for a
  weekly cross-backend parity soak (idle-GPU job), expanding the
  parity surface beyond the 3 Netflix golden pairs without
  touching the golden-data gate.
- **Negative**: Manifest and parquet schema churn — every
  downstream consumer of the parquet must accept the new
  `mos_convention` column. Acceptable cost; the column is
  additive.
- **Negative**: Disk pressure. The dev box must keep ~400 GB
  free for the BVI-CC corpus root in addition to the existing
  ~37 GB Netflix root.
- **Neutral**: Reproducibility for future contributors requires
  re-registering with Bristol; the in-repo manifest plus
  expected SHA-256 list is the audit trail.
- **Follow-ups**:
  - DMOS-to-MOS conversion test (places=4) added to
    `python/test/` against a 5-clip frozen subset.
  - Memory entry mirroring `project_netflix_training_corpus_local.md`
    once the corpus is local.
  - Decision deferred: BVI-DVC and BVI-AOM ingest. Revisit after
    measuring v2 lift from BVI-CC alone.

## References

- Companion: [Research-0046 — Bristol VI-Lab dataset feasibility](../research/0046-bristol-vi-lab-feasibility.md)
- Prior: [ADR-0019 — Tiny-AI Netflix training](0019-tiny-ai-netflix-training.md)
- Prior: [ADR-0042 — Tiny-AI docs required per PR](0042-tinyai-docs-required-per-pr.md)
- Prior: [ADR-0235 — Codec-aware fr_regressor v2](0235-codec-aware-fr-regressor.md)
- Source: `req` (user direction, 2026-05-02 — feasibility investigation for Bristol BVI-* corpora)
