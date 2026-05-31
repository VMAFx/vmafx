<!-- markdownlint-disable MD018 MD060 -->
# Research-0089: Multi-corpus MOS aggregation — scale unification feasibility

- **Date**: 2026-05-09
- **Author**: @Lusoris (and Claude as agent)
- **Companion ADR**: [ADR-0340](../adr/0340-multi-corpus-aggregation.md)
- **Tags**: ai, training, corpus, fork-local

## 1. Problem framing

The fork's in-flight MOS-corpus ingestion PRs (#447 KonViD-150k,
#471 LSVQ, #481 YouTube UGC, #485 Waterloo IVC 4K-VQA) each emit a
JSONL on the *source* dataset's native MOS scale. The downstream
trainers (#487 predictor v2 real-corpus, #491 KonViD MOS head v1)
want to learn from all shards simultaneously. Without a scale
unification step, the trainer sees three different target
distributions and the regression head learns the wrong thing.

The question: how compatible *are* these scales, and what conversion
is honest?

## 2. Source-scale review

| corpus | scale | citation | access (2026-05-09) |
|--------|-------|----------|---------------------|
| KonViD-1k | 1–5 ACR Likert (5-point absolute category rating) | Hosu et al., QoMEX 2017 §III | <http://database.mmsp-kn.de/konvid-1k-database.html> |
| KonViD-150k | 1–5 ACR Likert (same protocol as KonViD-1k) | Götz-Hahn et al., IEEE Access 2021 §III.B (companion ICIP 2019) | <https://database.mmsp-kn.de/konvid-150k-vqa-database.html> |
| LSVQ | 1–5 ACR Likert, crowd-sourced | Ying et al. (Patch-VQ), CVPR 2021 §4.1 | <https://github.com/baidut/PatchVQ> |
| YouTube UGC | 1–5 ACR (5 = best, 1 = worst) | Wang et al., MMSP 2019 §3.2 | <https://media.withyoutube.com/> |
| Waterloo IVC 4K-VQA | 0–100 continuous (DCR-like numerical-category) | Cheon & Lee, CVPR-W 2016 §III.B | <https://ece.uwaterloo.ca/~zduanmu/cvpr2016_4kvqa/> |
| Netflix Public | VMAF 0–100 (objective proxy via `vmaf_v0.6.1`) | `core/include/libvmaf/model.h` | repo-local |

## 3. Conversion choices — affine vs. quantile

**Affine** (`unified = slope * native + intercept`) is the simplest
possible map and the cheapest to explain to future maintainers. For
the four 1–5 ACR corpora, the canonical map onto 0–100 is
`unified = (mos - 1) * 25` — `1.0` ⇒ `0`, `5.0` ⇒ `100`,
midpoint `3.0` ⇒ `50`. Waterloo and Netflix are already on the
0–100 axis and pass through identity.

**Quantile-mapping** would compensate for any non-linearity between
the four ACR corpora — e.g. KonViD-1k saturating at 4.5 while LSVQ
extends to 4.9 due to differing rater pools. The literature does
*not* document such non-linearity for these specific corpora at a
level that justifies the implementation cost. Quantile-mapping
introduces dataset-specific compression that's hard to inspect and
hard to reverse; affine preserves the original distribution shape
and keeps `mos_native` round-trippable.

**Z-score** would erase absolute-quality semantics — a z-score of 0
means "median for this corpus", not a fixed quality level. The
trainer cannot calibrate against the VMAF reference axis, which is
the whole point of choosing 0–100 as the unified axis.

**Decision:** affine. Document the assumption explicitly in the
ADR's `## Consequences` so a future maintainer with evidence of
non-linearity has a clear single point to revise (the
`SCALE_CONVERSIONS` table).

## 4. Cross-corpus dedup — last-write vs. uncertainty-weighted

Two corpora can include the same source clip (a viral YouTube clip
in both KonViD-1k and YouTube UGC, for instance). Last-write-wins
(the existing `merge_corpora.py` policy for encode-grid corpora) is
non-deterministic across machine layouts because the operator's
`--inputs` ordering shifts. For *MOS* corpora the more useful
property is "keep the row with the tightest rater agreement", which
is what `mos_std_dev` tracks. The aggregator therefore picks the
row with the smaller `mos_std_dev`; ties keep first-seen (which is
deterministic given a stable `--inputs` order, and the operator
rarely cares which of two identically-uncertain rows survives).

A missing or zero `mos_std_dev` is treated as "unknown
uncertainty" and loses to any positive std-dev — this avoids a
zero-uncertainty Netflix VMAF row (objective proxy, no rater
agreement to report) silently outranking an LSVQ row that *does*
report rater dispersion.

## 5. Out-of-range and unknown-label handling

Per the fork's `feedback_no_test_weakening` rule, the aggregator
**drops** rows whose native MOS falls outside the published range
(with a small slack for float noise) rather than clipping them.
Same policy for rows whose `corpus` label is not in
`SCALE_CONVERSIONS`. The unified JSONL is therefore always a strict
subset of the inputs, with row counts surfaced in the run summary
under `dropped_bad_scale` / `dropped_unknown_corpus`. An operator
can audit the difference between input row counts and `rows_out`
deterministically.

## 6. Open questions deferred to follow-up ADRs

- **Cross-calibration via VMAF.** A more principled unification
  would run VMAF on every clip and align corpora to the VMAF score
  rather than via published Likert range. Cost: re-running ~150k
  clips through the libvmaf CLI. Defer until the simpler affine
  unification proves insufficient against held-out PLCC / SROCC
  gates.
- **Bradley–Terry / paired-comparison corpora.** Future corpora may
  publish paired-comparison scores (no absolute MOS). They would
  need a non-affine entry in `SCALE_CONVERSIONS` and an explicit
  ADR; this PR does not pre-plan that surface.

## 7. References

- ADR-0310 — BVI-DVC corpus ingestion (`merge_corpora.py` sibling
  for encode-grid corpora).
- ADR-0325 — KonViD ingestion (Phase 1 + Phase 2).
- Issue threads on PRs #447 / #471 / #481 / #485 / #487 / #491
  (in flight 2026-05-09).
