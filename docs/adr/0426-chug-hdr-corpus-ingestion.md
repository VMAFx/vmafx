<!-- markdownlint-disable MD013 MD060 -->
# ADR-0426: CHUG HDR corpus ingestion

- **Status**: Accepted
- **Date**: 2026-05-14
- **Deciders**: Lusoris, Codex
- **Tags**: ai, hdr, corpus, mos, training, license

## Context

The fork has enough SDR / existing-teacher tiny-AI training evidence to
make several narrow discoveries, but HDR claims are still blocked by
missing subjective HDR data and by the not-yet-shipped Netflix HDR
teacher model. CHUG is a public UGC-HDR VQA dataset with 5,992 videos,
856 HDR references, bitrate-ladder encodes, and AMT MOS labels. The
CSV manifest is public and the videos are externally hosted on S3.

The repo README advertises CC BY-NC 4.0, while `license.txt` contains
CC BY-NC-SA 4.0 text. We need to treat the data as non-commercial and
share-alike until clarified.

## Decision

We will add CHUG as a local-only MOS-corpus ingestion path under
`ai/scripts/chug_to_corpus_jsonl.py`. The adapter downloads/probes clips
under `.workingdir2/chug/`, emits `corpus="chug"` JSONL rows, preserves
CHUG-specific HDR/laddder metadata, stores raw CHUG MOS as
`mos_raw_0_100`, and maps `mos` onto the fork MOS-head `[1, 5]` axis via
`1 + 4 * mos_raw_0_100 / 100`. No CHUG media, MOS CSV, or derived
corpus rows are committed.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Wait for Netflix HDR VMAF only | Avoids subjective-MOS license ambiguity; directly trains against the eventual FR teacher. | Leaves NR/MOS HDR work idle and cannot study UGC-HDR distortions before the teacher lands. | Rejected. CHUG gives immediate subjective HDR signal and complements the future FR teacher. |
| Commit a sampled CHUG CSV or derived JSONL | Makes CI fixtures easy and reproducible. | Commits per-video MOS values from a non-commercial/share-alike dataset; risks license drift. | Rejected. Keep all CHUG data under `.workingdir2`; tests use synthetic rows only. |
| Treat CHUG MOS as native 0-100 in `mos` | Preserves source scale verbatim. | The existing MOS-head trainer enforces `[1, 5]`; feeding 0-100 rows would be skipped. | Rejected. Preserve raw MOS separately and map the trainer-facing field explicitly. |

## Consequences

- **Positive**: HDR subjective-data work can start immediately; the
  adapter reuses the shared MOS-corpus ingest base, resumable downloads,
  ffprobe geometry, and JSONL dedup path.
- **Negative**: CHUG-derived weights are research/non-commercial until
  license status is clarified; a full ingest is a local hardware /
  bandwidth job and cannot run in CI.
- **Neutral / follow-ups**: Add feature extraction over the CHUG JSONL
  before making production-quality HDR MOS claims. Re-run once the
  Netflix HDR teacher model lands so FR-HDR and MOS-HDR findings can be
  compared.

## References

- CHUG repository: <https://github.com/shreshthsaini/CHUG>
- CHUG paper DOI: <https://doi.org/10.1109/ICIP55913.2025.11084488>
- Source: `req` — "and we need this as well <https://github.com/shreshthsaini/CHUG>"
- Source: `req` — "yeah well download, prep and train lol... thats a local hardware background job..."
- Source: `req` — "and then lets unlock fucking hdr baby"
