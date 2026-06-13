# Research digest 0088: hardware-capability priors for the FR-regressor corpus

- **Date**: 2026-05-08
- **Authors**: Lusoris, Claude (Anthropic)
- **Companion ADR**: [ADR-0335](../adr/0335-hardware-capability-priors.md)
- **Status**: Finalised — feeds the implementation in
  [`ai/data/hardware_caps.csv`](../../ai/data/hardware_caps.csv).

## Question

Can the FR-regressor's training corpus be enriched with
per-architecture metadata about new (Battlemage / RDNA4 / Blackwell)
and historical (Alchemist / RDNA3 / Ada Lovelace) GPU encode
generations, so the model has structural priors about what each
piece of hardware can do — without contaminating the predictor with
biased third-party benchmark numbers?

## Why now

The fork's corpus today only carries encode-row metadata it
measures itself (encoder name, preset, CRF, observed scores). The
predictor cannot distinguish "AV1 on Blackwell" from "AV1 on
RDNA3" beyond the encoder string token — no signal about codec
profile caps, encoder-block count, tensor / NPU presence, or
vendor lineage. Vendor docs publish that capability matrix; we can
ship it as a small static prior table.

## What was investigated

The contributor pack compiled candidate web sources into three
categories:

1. **Vendor-published throughput / quality benchmarks** —
   "8x faster H.264", "4K60 AV1 at 60 fps", PR-deck slides.
2. **Vendor capability matrices** — Video Codec SDK release notes,
   AMF / oneVPL feature tables, product overview pages listing
   codec support, max resolution, encode-block count, driver
   floor.
3. **Third-party reviews & community wikis** — Wikipedia,
   wikichip, tech-press articles citing the above.

For each category we asked: does shipping it as a static prior
help or harm the FR-regressor?

## Findings

### Category 1 (benchmark numbers): NO-GO

Vendor-published throughput / quality numbers come from
benchmarks the vendor controls — favourable clips, optimal preset
choices, hand-tuned thresholds. The numbers are **not** comparable
across vendors and routinely shift across driver branches. Worse,
they bake in a *prior* about how good the encoder is, which the
predictor will fit on instead of learning from the corpus's own
measured rows. Concretely, a "Blackwell AV1 is 30 % faster than
Ada AV1" prior column would let the model shortcut the actual
score signal during cross-validation and inflate apparent
PLCC / SROCC.

This category is excluded.

### Category 2 (capability matrix): GO

Capability metadata is structural, not measured. "This generation
supports AV1" is a yes/no fact derived from the encoder block's
silicon. It does not encode quality or performance — only the
*search space* of what the corpus pipeline can ask the hardware to
do. Shipping it as a prior column lets the predictor learn
generation-specific patterns without leaking targets.

This category is shipped.

### Category 3 (community wikis): NO-GO

Wikipedia and wikichip aggregate the same vendor docs as
category 2 but are mutable, occasionally wrong, and lack the audit
trail needed for a prior column. The schema in
`ai/scripts/hardware_caps_loader.py` rejects any source URL on
`wikipedia.org` or `wikichip.org`. Operators must cite the vendor
primary doc directly.

## What the table ships

Six rows on 2026-05-08 — the three named-target architectures plus
their immediate predecessors as comparison anchors:

- Intel Alchemist (2022) and Battlemage (2024)
- AMD RDNA3 (2022) and RDNA4 (2025)
- NVIDIA Ada Lovelace (2022) and Blackwell (2025)

NVIDIA Hopper was considered as a Blackwell-anchor row but has no
NVENC silicon on the H100 / H200 SKUs and is therefore outside the
scope of an encode-capability fingerprint. The loader's schema
forbids `encoding_blocks=0`.

Per-row fields: vendor, generation year, codecs supported, max
resolution per codec, encoding-block count, tensor-core flag,
NPU-present flag, minimum driver version, primary source URL,
verification date.

## Reproducer

```bash
# Load and dump the table.
python ai/scripts/hardware_caps_loader.py

# Resolve a single (encoder, arch) pair to a feature vector.
python ai/scripts/hardware_caps_loader.py \
    --encoder av1_nvenc --arch blackwell

# Run the schema + round-trip + vector tests.
pytest ai/tests/test_hardware_caps.py -v
```

## Operational follow-ups

- Re-walk the table when a new GPU generation lands or when a row's
  `verified_date` falls more than 12 months behind master. There
  is no automatic rotation.
- The loader returns a fixed-shape dict. Trainers consuming
  `hwcap_*` columns should treat `hwcap_known=0` rows as
  null-valued for every other `hwcap_*` field — equivalent to
  "no hardware-capability prior available for this row".
- If a future research pass identifies category-2 fields the
  current schema misses (e.g. `b_frames_supported`, `roi_present`),
  add them via a new ADR — not a silent CSV column bump — so the
  exclusion of category-1 stays auditable.

## References

- NVIDIA Video Codec SDK 12.1 / 13.0 docs (NVENC capability matrix
  per generation): used as primary source for Ada Lovelace and
  Blackwell rows.
- AMD GPUOpen "AMD Video Core Next (VCN) 4.0" + AMF SDK pages:
  used as primary source for RDNA3 and RDNA4 rows.
- Intel Arc desktop B-series overview + Arc Graphics video-encoding
  technical article: used as primary source for Alchemist and
  Battlemage rows.
- Source: `req` (user direction in implementation task issued
  2026-05-08 specifying the prior-only scope and the
  category-1 exclusion).
