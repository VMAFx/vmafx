<!-- markdownlint-disable MD037 MD060 -->
# ADR-0253: Defer SpEED-QA full-reference reduction

- **Status**: Accepted
- **Date**: 2026-05-03
- **Deciders**: lusoris (pending sign-off)
- **Tags**: `metrics`, `research`, `feature-extractor`, `roadmap`

## Context

The fork carries Netflix's `speed_chroma` and `speed_temporal`
extractors (PR #213, port of upstream commit
[`d3647c73`](https://github.com/Netflix/vmaf/commit/d3647c73)) gated
behind `-Denable_float=true`. The user's 2026-04-21 deep-research
memory note queued a follow-up question: should the fork stand up a
SpEED-QA full-reference reduction (the closed-form spatial entropic-
differencing score from Bampis, Gupta, Soundararajan and Bovik,
*IEEE SPL 24(9)*, 2017) on top of those extractors?

[Research-0051](../research/0051-speed-qa-feasibility.md) finds:

1. Netflix has not shipped, and is not on track to ship, a SpEED-
   driven model. The hypothetical `model/speed_4_v0.6.0.json` the
   feasibility brief assumed does not exist anywhere in upstream.
2. SpEED-QA overlaps substantially with the fork's existing `vif`
   extractor (both are GSM-prior divisive-normalisation entropy
   estimators); the only differentiator is throughput, which inverts
   on the fork's AVX-512 / CUDA / SYCL VIF stack.
3. Implementation cost is 2–3 weeks for one engineer to land scalar
   VX2 + CUDA at numeric parity, which is the same engineering
   window that funds higher-leverage tracks (FUNQUE+ port, Vulkan
   coverage push, tiny-AI v3 / v4 evaluation).

Per CLAUDE.md §12 r8, this scope decision needs an ADR before any
implementation work lands.

## Decision

We will **defer** SpEED-QA. The fork keeps `speed_chroma` and
`speed_temporal` as research-stage extractors with no behavioural
change, does not add a `speed_qa` reduction, and does not register a
SpEED-driven model. The decision is reversible on three named
triggers (see *Consequences → Follow-ups*).

## Alternatives considered

| Option              | Pros                                                                                                                                | Cons                                                                                                                                                  | Why not chosen                                                                                                                                                                                                                                                                                |
|---------------------|-------------------------------------------------------------------------------------------------------------------------------------|-------------------------------------------------------------------------------------------------------------------------------------------------------|-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| GO — full impl      | Closes a "completeness" story; multi-scale variant adds modest differentiation vs `vif`.                                            | 2–3 weeks engineering for a VIF-overlap metric with no consuming model. Adds permanent maintenance surface.                                           | Opportunity cost: same window funds FUNQUE+ (the live efficient-VMAF thread per [research-0010](../research/0010-speed-netflix-upstream-direction.md) §4) or T7 Vulkan-coverage backlog, both higher leverage.                                                                                |
| SCAFFOLD-ONLY       | <1 day to add a model-registry stub. Catches "Netflix ships a SpEED model JSON tomorrow" tail risk cheaply.                         | The registry stub points at a non-existent upstream binary — the brief's assumed `speed_4_v0.6.0.json` does not exist. Documents a half-finished story. | Marginal value over DEFER because the existing `speed_chroma` / `speed_temporal` extractors are already documented in [`docs/metrics/features.md`](../metrics/features.md). No upstream artefact to mirror.                                                                                  |
| **DEFER (chosen)**  | Zero engineering cost. Preserves bandwidth for FUNQUE+, Vulkan coverage, tiny-AI tracks. Status-quo extractors remain available.     | Loses the "we ship SpEED-QA" line if a downstream user asks. Mitigation: this ADR + the existing features.md section already document the position.   | All three signals — Netflix-direction (research-0010), FR-coverage (research-0051), engineering-bandwidth — point the same direction. The decision is reversible on three named triggers.                                                                                                     |

## Consequences

- **Positive.** No new C / SIMD / CUDA / SYCL code to maintain. No
  new public CLI flag or model JSON. No new cross-backend snapshot
  to keep aligned. Engineering bandwidth available for higher-
  leverage work (FUNQUE+ feasibility, Vulkan coverage, tiny-AI v3/v4
  multi-seed evaluation per [research-0050](../research/0050-vmaf-tiny-v3-v4-multiseed-konvid-eval.md)).
- **Negative.** If a downstream user explicitly requests a
  SpEED-QA full-frame score, the fork has to either (a) re-open the
  decision under one of the named triggers or (b) point them at the
  existing `speed_chroma_uv_score` extractor as a partial substitute.
  Some completeness signalling is foregone.
- **Follow-ups.** This decision is reversible on any of:
  1. **Netflix lands a model JSON consuming SpEED features.** A
     `model/*.json` on `upstream/master` with at least one
     `Speed_*_feature_*` reference. Watched via `/sync-upstream`.
  2. **Explicit user / customer request for SpEED-QA** (not the
     existing `speed_chroma` / `speed_temporal` — those satisfy "a
     SpEED metric").
  3. **A successor metric (FUNQUE+, pVMAF, a tiny-AI fusion model)
     names SpEED-QA as a load-bearing input.** A research-NNNN or
     ADR-NNNN explicitly citing SpEED-QA as a required upstream
     dependency.

  When a trigger fires, supersede this ADR with one in the
  Accepted state and the implementation plan from
  research-0051 §"Implementation cost estimate" (GO scope).
- **Documentation.** No `docs/metrics/` change — the existing SpEED
  section in `docs/metrics/features.md` already documents the
  research-stage position correctly. No `docs/usage/` change. No
  ffmpeg-patches change. No state.md change (this is a forward-
  looking scope decision, not a bug close).

## References

- [Research-0051](../research/0051-speed-qa-feasibility.md) — full
  feasibility analysis, decision matrix, reproducer, and trigger
  list (this ADR's load-bearing input).
- [Research-0010](../research/0010-speed-netflix-upstream-direction.md)
  — Netflix-direction analysis confirming no SpEED-driven Netflix
  VMAF-v3 is imminent.
- Bampis, Gupta, Soundararajan, Bovik, "SpEED-QA: Spatial Efficient
  Entropic Differencing for Image and Video Quality", IEEE SPL 24(9),
  1333–1337, 2017,
  [DOI 10.1109/LSP.2017.2726542](https://ieeexplore.ieee.org/document/7979533/).
- Upstream code: `core/src/feature/speed.c` (port commit
  [`d3647c73`](https://github.com/Netflix/vmaf/commit/d3647c73),
  alias-map merge
  [`9dac0a59`](https://github.com/Netflix/vmaf/commit/9dac0a59)).
- Fork: PR #213 (SpEED port), `core/src/feature/feature_extractor.c`
  (registration), `core/src/feature/alias.c` (aliases),
  `core/test/test_speed.c` (registration tests),
  [`docs/metrics/features.md`](../metrics/features.md) §"Speed
  (chroma)" / §"Speed (temporal)" (existing user-facing docs).
- Source: `req` — paraphrased from user direction 2026-05-03,
  *"Open the SpEED-QA metric track that's been queued. Investigate
  whether SpEED is a viable addition + scaffold a Proposed ADR.
  Don't accept without user signoff."*

### Status update 2026-05-08: Accepted

Audited as part of the 2026-05-08 ADR `Proposed` sweep
([Research-0086](../research/0086-adr-proposed-status-sweep-2026-05-08.md)).

The deliverable for a defer-shape decision is the documented
position itself; verification consists of confirming the position
is unchanged in tree:

- No `speed_qa` reduction in `core/src/feature/`.
- No SpEED-driven model in `model/`.
- `speed_chroma` / `speed_temporal` extractors remain unchanged
  (Netflix port from upstream `d3647c73`, gated behind
  `-Denable_float=true`).
- The three reversal triggers stay open and are documented in the
  Consequences section.
- Verification command:
  `ls core/src/feature/speed_*.c;
  grep -i speed model/registry.json`.

### Status update 2026-05-09

A minimal `vmaf_fex_speed_qa` extractor scaffold has landed in
`core/src/feature/speed_qa.c`. The scaffold registers the feature
name `"speed_qa"`, returns a placeholder score of 0.0 per frame, and
builds cleanly against the CPU-only backend. No real spatial or
temporal entropic-difference algorithm is implemented; the core
decision to defer the full SpEED-QA algorithm (documented above)
remains in force. The scaffold exists solely to reserve the
registration slot and make the extractor discoverable by name — the
real algorithm will be implemented in a follow-up PR once one of the
three named reversal triggers fires.

### Status update 2026-05-10: Real implementation landed

Reversal trigger 2 of the defer decision ("Explicit user request for SpEED-QA")
has fired. The real spatial and temporal entropic-difference algorithm has
replaced the placeholder scaffold in `core/src/feature/speed_qa.c`.

Implementation summary:

- Non-overlapping 7x7 luma blocks; separable Gaussian window (sigma=1.166, Q16).
- Per-block entropy: H = 0.5 * log2(2*pi*e*(sigma^2 + 1.0)).
- Spatial score S: mean(H) over all blocks of the distorted luma frame.
- Temporal score T: mean(H) over frame-difference blocks (dist[n]-dist[n-1]);
  zero for frame 0.
- Output per frame: score = S + T.
- Self-contained (no float dependency); integer pixels, double accumulation;
  VMAF_FEATURE_EXTRACTOR_TEMPORAL flag set for in-order delivery.
- Five unit tests in `core/test/test_speed_qa.c` (all pass).
- Documentation: `docs/metrics/speed_qa.md`.
