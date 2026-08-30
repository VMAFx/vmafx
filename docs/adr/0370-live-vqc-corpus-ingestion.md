<!-- markdownlint-disable MD060 -->
# ADR-0370 — LIVE-VQC MOS-corpus ingestion for `nr_metric_v1`

| Field | Value |
|-------|-------|
| **Status** | Accepted |
| **Date** | 2026-05-09 |
| **Tags** | ai, training, corpus, license, fork-local |

## Context

The fork's `nr_metric_v1` NR-VQA model (ADR-0325 Phase 2) trains primarily
on KonViD-150k (~150 k clips, social-network UGC) and LSVQ (~39 k clips,
Patch-VQ author drop, CC-BY-4.0). Both corpora skew toward smartphone
social-media content: portrait-oriented clips, heavy platform re-compression,
limited scene diversity. YouTube UGC (ADR-0368) and Waterloo IVC 4K-VQA
(ADR-0369) add breadth but do not cover the consumer-device capture variety
present in LIVE Lab collections.

LIVE Video Quality Challenge (LIVE-VQC; Sinno & Bovik, IEEE TIP 2019) is a
585-video dataset collected by the LIVE Lab at UT Austin. Videos were
recorded by volunteers on consumer smartphones and tablets across diverse
real-world scenes (indoor / outdoor, night / day, multiple devices),
producing authentic in-the-wild distortions absent from controlled studio
corpora. Per-clip MOS values are on a 0–100 continuous scale, collected via
the LIVE Lab's online crowdsourcing framework.

The dataset fills a gap in the existing training shards: authentic
consumer-device capture on a scale small enough to store and iterate on a
single workstation (585 clips ≈ a few GB), making it practical to add
without pipeline-size concerns.

## Decision

Add LIVE-VQC as the sixth MOS-corpus shard for `nr_metric_v1`, following the
same adapter pattern as LSVQ (ADR-0367), YouTube UGC (ADR-0368), and
Waterloo IVC (ADR-0369):

1. New `ai/scripts/live_vqc_to_corpus_jsonl.py` — adapter with resumable
   per-clip downloads (curl subprocess seam), ffprobe geometry probe, and
   JSONL output using the corpus_v3 schema (ADR-0366).
2. `corpus` field literal: `"live-vqc"`.
3. MOS recorded **verbatim** on the native 0–100 continuous scale — no
   rescaling at ingest time (matches Waterloo IVC posture; downstream
   `aggregate_corpora.py` normalises per corpus).
4. `corpus_version` default: `"live-vqc-2019"` (IEEE TIP 2019 publication).
5. Min-row floor: 50 (smallest plausible useful subset of the 585-clip
   corpus; well below any full or filtered run).
6. Default max-rows: 200 (laptop-class subset); `--full` for all 585 clips.
7. Two manifest shapes accepted: (a) canonical headerless two-column
   `<filename>,<mos>` (the minimal spreadsheet export from the dataset page),
   (b) standard adapter CSV with named columns (LSVQ / KonViD-150k header
   convention).
8. New `ai/tests/test_live_vqc.py` — 18 tests exercising both CSV shapes,
   resumable downloads, attrition tolerance, refuse-tiny cutoff, geometry
   parse, broken-clip skip, dedup, corpus constants, and schema key set.
9. `docs/ai/mos-corpora.md` updated with the LIVE-VQC table row, quick-start
   commands, and licence entry.
10. `docs/ai/live-vqc-ingestion.md` per-corpus operator guide.

## Alternatives considered

| Alternative | Reason not chosen |
|-------------|-------------------|
| Skip LIVE-VQC; rely on LSVQ + KonViD-150k | Both cover overlapping content distributions (social-network UGC); LIVE-VQC's device-capture variety and small footprint make it the cheapest diversity add available. |
| Merge LIVE-VQC into the LSVQ adapter as an optional split | Dataset provenance, MOS scale, and acquisition path differ; a separate adapter keeps each corpus self-contained and independently versionable per the family convention. |
| Re-scale MOS to 1–5 at ingest time | The family policy is verbatim ingest + aggregator-side normalisation; introducing a rescaling exception here would be inconsistent with Waterloo IVC (ADR-0369). |
| Download clips directly from the UT Austin server in CI | The dataset requires a manual request; the adapter is a conversion tool for operators who already have access, not an automated CI downloader. |

## Consequences

**Positive:**

- Adds authentic consumer-device-capture UGC to the training mix, diversifying
  content away from social-network UGC dominance.
- 585 clips ≈ a few GB — fits on any developer workstation without a special
  storage budget; ingest completes in minutes.
- Follows the established family pattern; no new abstractions required.

**Negative / caveats:**

- LIVE-VQC MOS is on 0–100 scale (not 1–5 ACR Likert). Trainer code consuming
  mixed shards must use `aggregate_corpora.py` or apply per-corpus
  normalisation; raw mixing of LIVE-VQC rows with KonViD / LSVQ rows will
  produce biased gradients.
- Dataset acquisition requires a request form at the UT Austin LIVE Lab page;
  the adapter cannot auto-download clips. Operators must obtain the archive
  separately.
- `mos_std_dev` and `n_ratings` are 0.0 / 0 when the canonical two-column
  CSV is used (the minimal MOS export does not include inter-rater spread);
  downstream code that weights by `n_ratings` will treat these rows as
  unweighted.

## References

- req: "open a NEW PR adding LIVE-VQC dataset ingestion to the AI/training
  stack" (user direction, 2026-05-09)
- Sinno, Z., Bovik, A. C., "Large-Scale Study of Perceptual Video Quality,"
  IEEE Transactions on Image Processing, 28(2), pp. 612–627, Feb. 2019.
  DOI: 10.1109/TIP.2018.2875341
- Dataset page: <https://live.ece.utexas.edu/research/LIVEVQC/>
- [ADR-0325](0325-konvid-150k-corpus-ingestion.md) — KonViD-150k ingestion
- [ADR-0366](0366-corpus-schema-v3.md) — corpus_v3 schema
- [ADR-0367](0367-lsvq-corpus-ingestion.md) — LSVQ ingestion (template adapter)
- [ADR-0368](0368-youtube-ugc-corpus-ingestion.md) — YouTube UGC ingestion
- [ADR-0369](0369-waterloo-ivc-4k-corpus-ingestion.md) — Waterloo IVC ingestion
- [ADR-0340](0340-multi-corpus-aggregation.md) — aggregation / normalisation
