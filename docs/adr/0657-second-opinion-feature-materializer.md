<!-- markdownlint-disable MD060 -->
# ADR-0657: Second-Opinion Feature Materializer

- **Status**: Accepted
- **Date**: 2026-05-20
- **Deciders**: Lusoris agents
- **Tags**: ai, signal-mix, mos, external-bench

## Context

ADR-0650 made the signal-mix blind spots explicit: refreshed feature tables
still need no-reference and subjective-MOS second opinions so model retrains
can see where FR metrics saturate or disagree with human-rated UGC/HDR clips.
Some useful scorers are in-tree (`nr_metric_v1`); others, such as DOVER,
Q-Align, FAST-VQA, MUSIQ, and CLIP-IQA, are external projects with separate
licence and installation constraints.

The fork already keeps `tools/external-bench/` wrapper-only for licence
reasons. The feature-table path needs the same boundary: corpus materialisation
should accept scorer outputs, not embed third-party competitor execution inside
the training scripts.

## Decision

Add `ai/scripts/materialize_second_opinion_features.py`, a table-side joiner
that reads already-generated scorer JSON/JSONL and appends namespaced
`second_opinion_<scorer>_*` columns to parquet/JSONL feature tables.

The materializer:

- supports common feature-table formats: parquet, JSONL/NDJSON, and JSON rows;
- supports scalar score rows and external-bench wrapper-style frame payloads;
- joins by an inferred or explicit row key;
- records score, status, runtime, frame count, and score-file provenance;
- rejects duplicate `(scorer, key)` rows instead of silently averaging them;
- can mark, drop, or fail rows with missing scorer outputs.

`ai/scripts/signal_mix_audit.py` also recognises `second_opinion_*` and common
NR/MOS scorer names as no-reference / subjective-MOS evidence.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Run external competitors directly from corpus feature extraction | One command could produce all columns | Pulls licence-bound external tools into a training path and makes tests depend on optional binaries | Violates the wrapper-only posture and makes reproducibility worse |
| Extend `tools/external-bench/compare.py` into a corpus enricher | Reuses wrapper discovery | The benchmark harness is aggregate/report-oriented and currently loses row-table context | Keep benchmark comparison and feature-table enrichment separate |
| Join second-opinion scores inside each trainer | No extra operator step | Duplicates join logic across MOS heads, predictor training, and future refresh scripts | Central table materialisation is easier to audit and test |

## Consequences

- **Positive**: refreshed tables can carry NR/MOS second-opinion features
  without waiting for every scorer to become a first-class in-tree extractor.
- **Positive**: the licence boundary remains clean; external scorer execution
  stays out-of-tree or under wrapper-only harnesses.
- **Negative**: operators must run scorers separately and preserve a row key
  in their JSON/JSONL outputs.
- **Neutral / follow-ups**: run the materializer on CHUG, KoNViD/UGC, and
  Netflix-derived refreshed tables, then measure retrain impact with
  `signal_mix_audit.py` and the relevant MOS/predictor gates.

## References

- [ADR-0650](0650-signal-mix-audit.md)
- [docs/ai/signal-mix-audit.md](../ai/signal-mix-audit.md)
- User request: "well that sounds like a lot to do... go on then"
