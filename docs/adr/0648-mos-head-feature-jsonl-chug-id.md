<!-- markdownlint-disable MD013 MD060 -->
# ADR-0648: CHUG HDR MOS Trainer Entry Point

- **Status**: Proposed
- **Date**: 2026-05-20
- **Deciders**: Lusoris, Codex
- **Tags**: ai, hdr, chug, mos, training

## Context

CHUG feature extraction now emits reference-aligned feature JSONL shards with
content-level `train` / `val` / `test` splits. Those rows are HDR subjective-MOS
labels, not SDR Netflix VMAF teacher labels and not KonViD rows. The existing
MOS trainer had a usable internal loader, but the operator-facing command was
`train_konvid_mos_head.py` and the committed default manifest id was
`konvid_mos_head_v1`.

Using a KonViD-named command for CHUG is misleading for two reasons:

- CHUG is the fork's interim HDR subjective-MOS signal until Netflix ships HDR
  VMAF weights.
- The current Netflix `vmaf_v0.6.1` teacher remains SDR/8-bit calibrated and
  must not be described as an HDR ground truth.

## Decision

Add a CHUG-named trainer entry point:
`ai/scripts/train_chug_hdr_mos_head.py`.

The wrapper defaults to the canonical local CHUG shard directory
`.corpus/chug/training/fr_canonical_shards/output/`, records
`chug_hdr_mos_head_v1` as the manifest id, and writes local outputs under
`.workingdir2/chug/`. It reuses the shared MOS-head training loop but passes
explicit impossible KonViD paths so a CHUG run cannot accidentally ingest local
KonViD JSONL beside the HDR shards.

The shared trainer also gains an internal `--feature-jsonl` input path and an
internal `--model-id` manifest override. These are hidden from the KonViD help
surface and used by the CHUG wrapper.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep documenting CHUG through `train_konvid_mos_head.py` | Smallest patch | Misnames the corpus and makes HDR MOS look like KonViD/SDR work | Rejected by user feedback and because it confuses the HDR model boundary |
| Rename the existing KonViD trainer wholesale | Cleans up the shared-code name | Breaks existing docs, tests, and committed `konvid_mos_head_v1` provenance in one PR | Too much migration risk for the immediate CHUG unlock |
| Add a thin CHUG wrapper over the shared trainer | Correct operator surface; minimal disruption; keeps committed KonViD model history intact | Leaves shared implementation in a KonViD-named module for now | Chosen as the low-risk step; a future refactor can move shared code into a generic module |

## Consequences

- **Positive**: CHUG HDR MOS training now has a truthful command and manifest id.
- **Positive**: The local HDR workflow no longer tells operators to pass CHUG
  shards through KonViD-named CLI flags.
- **Negative**: The shared implementation module still carries the historical
  KonViD filename until a larger refactor moves the common code.
- **Neutral / follow-ups**: Once the CHUG run clears or misses its gate, record
  the result in the local ledger and decide whether a redistributable fork-owned
  HDR MOS checkpoint can ship under the CHUG dataset's license posture.

## References

- [ADR-0426](0426-chug-hdr-corpus-ingestion.md)
- [ADR-0427](0427-chug-hdr-feature-materialisation.md)
- [ADR-0336](0336-konvid-mos-head-v1.md)
- Source: `req` — "well yeah and chug is hdr mos... so thats different because netflix (current) model is 8bit only etc... so its our model or the chug model we use for hdr until netflix finally releases their model"
- Source: `req` — "you talk about chug and the script is named konvid? ffs..."
