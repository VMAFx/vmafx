<!-- markdownlint-disable MD060 -->
# Netflix-Grade Encoding Pipeline Roadmap — 2026-05-19

Planning-only document. No implementation decisions are final until the user
selects an item and the corresponding ADR is accepted.

---

## Items Covered

| # | Item | ADR | Research | Effort |
|---|------|-----|----------|--------|
| 1 | Dynamic Optimizer (DO) | [ADR-0613](../adr/0613-dynamic-optimizer.md) | [Research 0609](../research/0609-dynamic-optimizer-research.md) | 4–5 days |
| 2 | Per-shot ABR rendition | [ADR-0614](../adr/0614-per-shot-abr-rendition.md) | [Research 0610](../research/0610-per-shot-abr-rendition-research.md) | 6–7 days |
| 3 | Fast NR pre-scoring | [ADR-0615](../adr/0615-fast-nr-prescoring.md) | [Research 0611](../research/0611-fast-nr-prescoring-research.md) | 3 days |
| 4 | VMAF NEG integration | [ADR-0616](../adr/0616-vmaf-neg-integration.md) | [Research 0612](../research/0612-vmaf-neg-integration-research.md) | 1.5 days |
| 5 | Cross-shot complexity weighting | [ADR-0617](../adr/0617-cross-shot-complexity-weighting.md) | [Research 0613](../research/0613-cross-shot-complexity-weighting-research.md) | 4.5 days |
| 6 | Content-aware classifier | [ADR-0618](../adr/0618-content-aware-classifier.md) | [Research 0614](../research/0614-content-aware-classifier-research.md) | 8 days |

---

## Dependency Graph

```text
                   ┌──────────────────────────────────────────┐
                   │                                          │
  4: VMAF NEG      │   (standalone — no prerequisites)        │
  (ADR-0616)       │   Smallest win: model files in-tree.     │
  1.5 days         │                                          │
                   └──────────────────────────────────────────┘

  3: Fast NR       ──────────────────────────────────────────►  unlocks:
  (ADR-0615)         inner loop oracle for DO, ABR, cross-shot     │
  3 days                                                           │
         │                                                         ▼
         ├──────────────────────────────────────────►  1: Dynamic Optimizer
         │                                              (ADR-0613) 4–5 days
         │                                              (runs faster with NR)
         │
         ├──────────────────────────────────────────►  2: Per-shot ABR
         │                                              (ADR-0614) 6–7 days
         │                                              (NR reduces probe cost)
         │
         └──────────────────────────────────────────►  5: Cross-shot weighting
                                                        (ADR-0617) 4.5 days
                                                        (NR makes λ-bisect cheap)

  6: Content       ──────────────────────────────────────────►  feeds:
  Classifier         routing table → items 4, 1, 2, 3, 5         │
  (ADR-0618)         (auto-selects NEG, tune=, ladder priors)     │
  8 days             Requires ADR-0616 (NEG) to land first.       ▼

  Legend:
  ──►  "enables / accelerates"
  Solid boxes = can ship standalone
  Dashed boxes = benefits from, but does not block
```

### Formal dependency table

| Item | Hard prerequisites | Soft prerequisites (accelerate) |
|------|-------------------|--------------------------------|
| 4 (NEG) | None | — |
| 3 (NR) | None | — |
| 6 (Classifier) | Item 4 (NEG, for routing) | Item 3 (NR, if complexity proxy shared) |
| 1 (DO) | None | Item 3 (NR oracle) |
| 2 (ABR rendition) | Per-title ladder (ADR-0295, in-tree) | Item 3 (NR probe cost) |
| 5 (Cross-shot) | None | Item 3 (NR oracle for λ-bisect) |

---

## Recommended Sequencing

### Sequencing A: Smallest-win-first

Optimises for early visible value; each item ships in a standalone PR.

```text
Week 1:  Item 4 (NEG) — 1.5 days. Immediate value for codec comparisons.
Week 1:  Item 3 (NR)  — 3 days.   Unlocks speed improvements for all later items.
Week 2:  Item 5 (Cross-shot) — 4.5 days. Builds on NR; improves title quality.
Week 3:  Item 1 (DO)  — 4–5 days. Uses NR oracle; improves boundary placement.
Week 4:  Item 2 (ABR rendition) — 6–7 days. Largest encoding benefit.
Week 5–6: Item 6 (Classifier) — 8 days. Orchestrates all above.
```

Total: ~28 days / ~6 weeks.

### Sequencing B: Prerequisite-first

Implements the full stack from bottom up; maximises reuse during development.

```text
Week 1:  Item 4 (NEG) — 1.5 days.
Week 1:  Item 3 (NR)  — 3 days.
Week 2:  Item 6 (Classifier) — 8 days (builds routing infra that DO + ABR use).
Week 3:  Item 5 (Cross-shot) — 4.5 days.
Week 4:  Item 1 (DO)  — 4–5 days.
Week 5:  Item 2 (ABR rendition) — 6–7 days.
```

Total: ~28 days / ~6 weeks (same; sequencing B front-loads the classifier risk).

### Recommended: Sequencing A (smallest-win-first)

Rationale: Item 4 (NEG) is a 1.5-day win with zero training and zero new
dependencies — the model files exist, the code gap is parameter plumbing.
Item 3 (NR) unblocks three other items and its 3-day cost is recovered in
the first DO or cross-shot run. Starting with the classifier (Sequencing B)
front-loads the largest item and delays visible wins for 8 days.

---

## "If We Only Do 2, Do These 2"

**Item 4 (VMAF NEG) + Item 3 (Fast NR pre-scoring).**

Rationale:

- Item 4 is the cheapest item (1.5 days), has no dependencies, and immediately
  unblocks correct codec-comparison workflows. The model files are in-tree;
  this is pure parameter plumbing.
- Item 3 provides a 2–4× wall-time reduction on bisect, which affects every
  future use of `vmaf-tune per-shot`, `ladder`, and the planned DO. It also
  lays the calibration groundwork that Items 1, 2, and 5 depend on for
  tractable runtime.

Together these two items take ~4.5 days and leave the codebase in a better
state than today for every subsequent item.

---

## Netflix Upstream Inventory

The following relevant tooling was found in the Netflix upstream at
`github.com/Netflix/vmaf`:

| Component | Upstream status | Fork gap |
|-----------|----------------|----------|
| VMAF NEG model files | In `model/` (in-tree in fork) | CLI integration only |
| CAMBI banding detector | `resource/doc/cambi.md` | In-tree as libvmaf feature |
| Per-shot CLI | Not found in upstream at time of audit | Full implementation needed |
| Dynamic Optimizer | Not open-sourced (blog post only) | Full implementation needed |
| Content classifier | Not open-sourced | Full implementation needed |
| ABR rendition picker | Not open-sourced | Full implementation needed |

The `Netflix/aom-encoder-flag-recommendations` repository was not accessible
during this research pass (SSL failure); it may contain AV1-specific encoder
flag recommendations that are relevant to Item 6's routing table. This should
be checked when implementing the classifier.

---

## Open Questions for the User

The following questions must be resolved before implementation begins on the
indicated items:

### Q1 (Items 1, 5): Dynamic Optimizer boundary drift

The DO post-pass may shift shot boundaries from TransNet's output by ±N frames.
What is the acceptable boundary drift? Downstream consumers (chapter markers,
ad-break insertion, caption sync) may depend on TransNet boundaries being
respected within a tolerance.

**Options**: (a) Hard lock — DO may not shift any boundary; only merge/split.
(b) Soft lock — DO may shift by ≤ 24 frames (≤ 1 second at 24fps).
(c) No lock — DO may freely recut; responsibility of downstream consumers to
re-derive from the DO output.

### Q2 (Item 3): NR calibration: global or per-content-type threshold?

Should `δ_fast` (the NR uncertainty zone width) be a single global value
calibrated on the full Netflix corpus, or per-content-type (e.g. looser
for animation, tighter for sports)?

**Options**: (a) Global single threshold (simpler; less accurate).
(b) Per-genre threshold (requires Item 6 classifier to be implemented first).
(c) Per-clip empirical threshold (complex; deferred).

### Q3 (Item 5): Title-level quality floor semantics

Should the `floor_vmaf` (minimum per-shot quality) be a hard constraint
(abort encode if violated) or a soft penalty (accept with a logged warning)?

**Options**: (a) Hard — any shot below floor is a pipeline error.
(b) Soft — accept with warning; flag in the output JSON for operator review.
(c) Configurable — hard by default, --soft-floor flag for graceful degradation.

### Q4 (Item 6): Ollama dependency as runtime requirement

The hybrid classifier requires Ollama with a vision model for genre tags.
Is Ollama an acceptable runtime dependency for `vmaf-tune` in production
(CI, cloud encoding workers), or should the classifier degrade gracefully
to A3-only (no genre tags) when Ollama is absent?

**Options**: (a) Ollama required; `vmaf-tune classify` fails if absent.
(b) Ollama optional; graceful A3-only fallback (genre = `unknown`).
(c) Ollama optional; fallback to Claude Vision API if configured.

### Q5 (Item 2): HLS compatibility for per-shot resolution switches

HLS requires consistent resolution per variant stream. If per-shot ABR
rendition produces mixed-resolution segments, how should HLS packaging be
handled?

**Options**: (a) Always upscale to max resolution (bit waste but HLS compliant).
(b) DASH only — drop HLS support for per-shot resolution output.
(c) Resolution clustering — group shots by rung; produce one HLS variant
per rung, short shots at rung boundary treated as up-/down-scale.

---

## Research Retrieval Notes

Direct access to Netflix Tech Blog (netflixtechblog.com) failed during this
research pass with SSL certificate errors. Relevant posts were identified by
URL but not successfully fetched:

- "Dynamic Optimizer: A Perceptual Video Encoding Optimization Framework"
- "Per-Title Encode Optimization" (2015)
- "Per-Shot Encoding for High-Quality Video Streaming"
- "Toward a Better Quality Metric for the Video Community" (VMAF NEG)

The upstream Netflix VMAF models documentation was retrieved successfully
via GitHub API (2026-05-19) and confirms NEG model files are publicly
available. The research digests cite arXiv papers that were successfully
retrieved and are documented with retrieval dates.

---

## Changelog

This roadmap is planning-only. No changelog entry is generated until an
item's implementing PR merges. Each ADR's implementing PR will add an entry
to `changelog.d/added/`.
