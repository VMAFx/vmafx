# Research-0090: LSVQ corpus feasibility for `nr_metric_v1`

- **Date**: 2026-05-08
- **Author**: agent (LSVQ ingestion task)
- **Companion ADR**: [ADR-0367](../adr/0367-lsvq-corpus-ingestion.md)

## TL;DR

LSVQ (Ying et al. ICCV 2021) is the canonical large-scale NR-VQA
training corpus the field publishes against. It is hosted on
Hugging Face under
[`teowu/LSVQ-videos`](https://huggingface.co/datasets/teowu/LSVQ-videos)
under CC-BY-4.0, distributing one canonical train split
(``LSVQ_whole_train``, ~28 056 clips) and two test splits
(``LSVQ_test`` ~7 400 clips, ``LSVQ_test_1080p`` ~3 600 clips).
Per-clip MOS is on the native 1.0–5.0 Likert scale; per-clip
``mos_std_dev`` and ``n_ratings`` are typically published
alongside the MOS column. Total raw working set is ~500 GB
end-to-end, so the ingestion adapter defaults to a 500-row
laptop-class subset and gates whole-corpus runs behind an
explicit ``--full`` opt-in.

## Why LSVQ on top of KonViD-150k

The fork's `nr_metric_v1` (~19 K params) needs corpus breadth to
compete with DOVER-Mobile (PLCC 0.853 KoNViD / 0.867 LSVQ_test,
~9.86 M params) on the canonical NR-VQA leaderboards. KonViD-150k
(ADR-0325 Phase 2) covers community-uploaded UGC video, but every
modern open-weight NR-VQA paper (DOVER, FAST-VQA / FasterVQA,
Q-Align, MaxVQA, MVQA) trains on LSVQ specifically and benchmarks
PLCC against ``LSVQ_test`` / ``LSVQ_test_1080p``. Adding LSVQ
makes cross-paper comparison possible without changing model
architecture.

## Manifest CSV: where does it live

The canonical LSVQ split CSVs are distributed by the Hugging Face
mirror at `teowu/LSVQ-videos` (mirrored from the original
[Patch-VQ](https://github.com/baidut/PatchVQ) author drop). Header
columns vary slightly across the ICCV-2021 author drop, the
Hugging Face mirror, and the DOVER / FAST-VQA redistributions —
the adapter accepts every observed alias for filename / URL /
MOS / SD / rating-count columns. Bare-stem ``name`` columns
(``"0001"`` rather than ``"0001.mp4"``) are common in the LSVQ
release; the adapter transparently appends the
``--clip-suffix`` (default ``.mp4``).

## License

CC-BY-4.0 per the dataset card on Hugging Face (verified
2026-05-08). This is permissive enough to ship derived
`nr_metric_v1_*.onnx` weights with attribution, but the raw
clips and per-clip MOS values stay local-only on this fork to
avoid bundling the dataset itself with the repo (same posture
as ADR-0310 BVI-DVC and ADR-0325 KonViD-150k).

## Decision matrix

See [ADR-0367 §Alternatives considered](../adr/0367-lsvq-corpus-ingestion.md#alternatives-considered).

## Open follow-ups

- ENCODER_VOCAB v4 trainer-side collapse to `"ugc-mixed"` for
  KonViD-150k + LSVQ rows (separate PR).
- Held-out evaluation harness wiring for ``LSVQ_test`` /
  ``LSVQ_test_1080p`` so PLCC / SROCC / KRCC vs LSVQ becomes a
  CI-comparable number rather than an ad-hoc one.
- Corpus-level rescaling audit if the cross-corpus distribution
  (KonViD MOS vs LSVQ MOS vs BVI-DVC objective scores) turns out
  to need a per-shard normaliser. None applied at ingest time
  today.

## References

- Ying, Z., Mandal, M., Ghadiyaram, D., Bovik, A. C.,
  "Patch-VQ: 'Patching Up' the Video Quality Problem," ICCV 2021.
- Hugging Face dataset card:
  <https://huggingface.co/datasets/teowu/LSVQ-videos>
  (license: CC-BY-4.0, verified 2026-05-08).
- Patch-VQ author drop: <https://github.com/baidut/PatchVQ>.
- Tiny-AI SOTA deep-dive digest:
  [`docs/research/0086-tiny-ai-sota-deep-dive-2026-05-08.md`](0086-tiny-ai-sota-deep-dive-2026-05-08.md).
