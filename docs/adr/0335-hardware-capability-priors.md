<!-- markdownlint-disable MD060 -->
# ADR-0335: Hardware-capability priors for the FR-regressor corpus

- **Status**: Accepted
- **Date**: 2026-05-08
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: `ai`, `corpus`, `data`, `docs`

## Context

The FR-regressor's training corpus today only carries metadata the
fork measures itself (encoder name, preset, CRF, observed VMAF /
fork-AI scores). The predictor cannot distinguish a Blackwell AV1
encode from an RDNA3 AV1 encode beyond an opaque encoder-string
token — no structural signal about codec profile caps, encoder
block count, tensor / NPU presence, or vendor lineage flows
through. Vendor docs publish a capability matrix per architecture;
the question is whether to enrich the corpus with it.

The companion research digest
(`docs/research/0088-hardware-capability-priors-2026-05-08.md`)
audited candidate web sources and split them into three categories:
vendor benchmarks, vendor capability matrices, and community wikis.
It established a category-1 NO-GO finding: shipping vendor-published
throughput / quality numbers would let the predictor shortcut on
biased priors instead of learning from measured rows. Capability
metadata (category 2) does not have that pathology — it describes
the search space, not the outcome.

## Decision

Ship a small static **capability fingerprint** table at
`ai/data/hardware_caps.csv` covering Battlemage, RDNA4, Blackwell
plus their immediate predecessors (Alchemist, RDNA3, Ada Lovelace),
six rows on 2026-05-08. Each row carries vendor / gen-year / codecs
supported / max resolution per codec / encoding-block count /
tensor-core flag / NPU flag / driver-min-version / primary source
URL / verified date. A loader at
`ai/scripts/hardware_caps_loader.py` reads the table and exposes a
`cap_vector_for(encoder, encoder_arch_hint)` function that emits
fixed-shape `hwcap_*` feature columns the corpus-ingest pipeline
merges into each encode row. The schema rejects benchmark-shaped
columns, community-wiki source URLs, empty fields, and zero
encoding-block rows.

## Alternatives considered

| Option                                                    | Pros                                                                | Cons                                                                                | Why not chosen                                                                                                                                          |
| --------------------------------------------------------- | ------------------------------------------------------------------- | ----------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Capability metadata only (this ADR)                       | Structural facts; no benchmark contamination; small audit surface   | Operator must hand-walk vendor docs; no automatic refresh                           | **Chosen.** Matches the digest's category-2 GO finding and the user's prior-only directive.                                                             |
| Capability metadata + vendor-published benchmarks         | Richer prior; "free" performance signal                             | Benchmark numbers are vendor-controlled and incomparable; leaks priors into PLCC/SROCC | Rejected per digest category-1 NO-GO. Performance signal must come from the fork's own measured rows.                                                   |
| Pull capabilities from Wikipedia / wikichip               | Pre-aggregated; one URL per arch                                    | Mutable; occasionally wrong; no audit trail                                         | Rejected per digest category-3 NO-GO. Loader rejects `wikipedia.org` and `wikichip.org` source URLs schema-side.                                        |
| Skip the prior table; rely on encoder-string tokens alone | Zero new code or data                                               | Predictor cannot learn generation-specific patterns; AV1-on-Blackwell looks identical to AV1-on-RDNA3 | Rejected. The whole point of the contributor-pack pass was to add structural priors the corpus does not measure on its own.                            |

## Consequences

- **Positive**:
  - FR-regressor gains structural priors per `(encoder, arch)`
    pair without contaminating training with biased benchmark
    numbers.
  - Schema check in the loader makes it impossible to silently
    add throughput / quality columns later — anyone trying to
    extend the table with category-1 data must amend or supersede
    this ADR first.
  - All capability claims are anchored to a vendor primary source
    URL with a verification date; a future operator can re-walk
    the table.
  - Loader returns a fixed-shape dict (`hwcap_*` keys) so the
    corpus parquet schema stays stable across resolved and
    unresolved rows.
- **Negative**:
  - Hand-curated table needs periodic re-walks (no automation).
    Mitigated by the small row count (~6–10) and the explicit
    `verified_date` column.
  - Coverage limited to encoders the fork already routes through
    `vmaf-tune` (NVENC, AMF, QSV families). CPU-only encoders
    return a blank fingerprint by design.
- **Neutral / follow-ups**:
  - Re-walk when a new generation lands or when any row's
    `verified_date` falls more than 12 months behind master.
  - Future schema extensions (e.g. `b_frames_supported`,
    `roi_present`) require a new ADR — not a silent column bump —
    so the category-1 exclusion stays auditable.
  - Corpus-ingest scripts that consume the loader (downstream of
    this PR) will land in their own commits referencing this
    ADR.

## References

- [`docs/research/0088-hardware-capability-priors-2026-05-08.md`](../research/0088-hardware-capability-priors-2026-05-08.md)
  — research digest with the three-way category split and
  category-1 NO-GO finding.
- [`docs/ai/hardware-capability-priors.md`](../ai/hardware-capability-priors.md)
  — operator-facing reference for the table and loader.
- [`ai/data/hardware_caps.csv`](../../ai/data/hardware_caps.csv)
  — the table itself, with vendor primary source URLs in the
  `source_url` column.
- [`ai/scripts/hardware_caps_loader.py`](../../ai/scripts/hardware_caps_loader.py)
  — loader and `cap_vector_for()` ingest helper.
- [ADR-0042](0042-tinyai-docs-required-per-pr.md) — tiny-AI
  per-PR doc-substance specialisation that this ADR satisfies via
  the `docs/ai/` page.
- [ADR-0108](0108-deep-dive-deliverables-rule.md) — six deep-dive
  deliverables rule (digest, decision matrix, AGENTS invariant,
  reproducer, CHANGELOG entry, rebase-notes entry) that this ADR
  satisfies.
- Source: `req` (user implementation task on 2026-05-08:
  "ship hardware-capability fingerprint feature columns for
  Battlemage / RDNA4 / Blackwell GPU generations … prior-only
  fill … capability metadata, NOT benchmark numbers").
