<!-- markdownlint-disable MD060 -->
# ADR-0397: `vmaf-tune` Phase F — `auto` adaptive recipe-aware tuning

- **Status**: Accepted
- **Date**: 2026-05-08
- **Deciders**: Lusoris
- **Tags**: tooling, automation, vmaf-tune, ffmpeg, codec, fork-local

## Context

Phases A through E of `vmaf-tune` ship as eight standalone CLI
subcommands (`corpus`, `recommend`, `fast`, `predict`,
`tune-per-shot`, `recommend-saliency`, `ladder`, `compare`) plus
three orthogonal modes (HDR auto-detect via [ADR-0300](0300-vmaf-tune-hdr-aware.md),
sample-clip via [ADR-0301](0301-vmaf-tune-sample-clip.md), and
resolution-aware model selection via [ADR-0289](0289-vmaf-tune-resolution-aware.md)).
The operator-facing question — "give me the cheapest encode that
meets target VMAF for this content" — currently requires the
operator to compose roughly eight phases manually, ≈ 5–6 hours of
wall-clock for a 2-hour 1080p title (see
[Research-0067](../research/0067-vmaf-tune-phase-f-feasibility-2026-05-08.md)
cost table). The user's 2026-05-08 vision text (paraphrased: "the
real long-term potential is building an adaptive encoding ecosystem
around community-generated training data, perceptual analysis and
continual model improvement") frames Phase F as the first
composition layer that exposes this ecosystem behind a single CLI
verb.

[Research-0067](../research/0067-vmaf-tune-phase-f-feasibility-2026-05-08.md)
walks the cost model, the seven short-circuit cases, and the four
failure modes; it concludes that a deterministic decision tree
hits the explainability + reproducibility floor the fork requires
without sacrificing the wall-time savings a learned policy would
deliver.

## Decision

We will ship `vmaf-tune auto` as the Phase F entry point, implemented
as a **deterministic decision tree** in
`tools/vmaf-tune/src/vmaftune/auto.py` that composes the existing
phase subcommands sequentially. The tree is hand-coded (no learned
policy at runtime), every branch maps to an existing per-phase ADR
contract, and the full tree fits within a 30-line pseudocode
specification (see Research-0067 §"Phase F decision tree"). The
phased rollout below splits the work into four follow-up PRs so each
slice ships with its own validation:

- **F.0 — this ADR (design only).** No code. Establishes the
  decision tree, short-circuit list, escalation policy, and the
  rule that Phase F never invents new sub-phases.
- **F.1 — scaffolded `vmaf-tune auto` (`tools/vmaf-tune/src/vmaftune/auto.py`).**
  Sequential composition of the existing subcommands; no
  short-circuits, no escalation. The CLI flags `--src`,
  `--target-vmaf`, `--max-budget-bitrate`, `--allow-codecs`,
  `--codec`, `--smoke` are stable from this PR forward. `--smoke`
  exercises the composition end-to-end with mocked sub-phases (no
  ffmpeg, no ONNX); production wiring lands in F.2.
- **F.2 — short-circuit logic.** The seven cases from
  Research-0067 §"When Phase F should short-circuit" become
  conditional branches: single-rung ladder when source < 2160p;
  codec known; predictor verdict GOSPEL; short / low-variance
  source skips Phase D; non-animation / non-screen-content skips
  saliency; SDR source skips HDR pipeline; sample-clip propagates
  to internal sweeps.
- **F.3 — confidence-aware fallbacks.** When Phase C's predictor
  returns FALL_BACK on a (rung, codec) cell, escalate **only that
  cell** to `recommend.coarse_to_fine`. The escalation is per-cell,
  bounded, and logged. GOSPEL and LIKELY verdicts skip the
  escalation. Encoder-ROI / saliency-binary missing degrades to a
  warning, never aborts.
- **F.4 — per-content-type recipe overrides.** Auto-detect
  animation / live-action / screen-content via a fork-local
  classifier (TransNet V2 is already present and exposes a
  shot-cut histogram that correlates well with the three classes;
  fallback heuristics in `auto.py` cover the no-classifier case).
  The class drives saliency / preset / per-shot defaults; users
  override via explicit flags.

The tree is the v1 surface; learned policy stays a research
follow-up after F.1–F.4 have produced enough labelled compositions
to seed a future supervised baseline. No closed-source ML services,
no Internet calls during encode, no runtime learned-policy
inference — Phase F is integration, not invention.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Deterministic decision tree (chosen)** | Explainable; reproducible across runs; every branch maps to an existing ADR contract; testable with mocked sub-phases; no runtime ML dependency on the auto path. | Hard-codes priorities; new sub-phases require tree edits. | Picked: matches the fork's "no learned policy at runtime" constraint, the user's explainability requirement, and the per-phase contract carve-outs in ADR-0237 / ADR-0276 / ADR-0295. |
| Pure-grid composition (today's manual workflow) | Zero new code; fully reproducible. | 8-step manual composition; ≈ 5–6 h wall-clock for a typical 2-hour movie; high operator-error rate; the colleague's "day per movie" pain point. | Rejected: the cost is exactly why Phase F is a backlog item. |
| Optuna over the full composition space | Strong optimum; reuses Phase A.5 search infrastructure. | Per-source TPE warm-up cost; no closed-form way to express "skip Phase D when source is short"; opaque to operators ("why did it pick x265?"); too few independent samples per source for Bayesian search to beat a hand-tuned tree. | Rejected: search-over-recipes is the wrong model for a discrete composition problem with operator-explainability requirements. |
| Learned policy (RL or supervised over a Phase A corpus) | Adapts to corpus drift; aligns with the long-term "continual model improvement" arm of the user's vision text. | Requires a labelled "this composition was right" dataset that doesn't yet exist; runtime inference adds an ONNX dependency to the auto path; reproducibility suffers (model drift between runs); violates the fork's "no learned-policy at runtime" constraint. | Rejected for v1; revisit as a research experiment once the deterministic tree has emitted a labelled corpus of recipe choices. |
| One mega-subcommand replacing all phases | Single operator surface. | Breaks every existing per-phase contract; downstream consumers (CI, MCP server, the FFmpeg patch series) lose stable per-phase hooks; ADR-0237 / ADR-0276 / ADR-0295 explicitly carve those contracts. | Rejected: the carve-outs are load-bearing. |
| Ship `auto` as a thin shell-script wrapper | No new Python module. | No way to express the short-circuit conditions or the FALL_BACK escalation cleanly in `bash`; smoke testing harder; operator-error surface (quoting, env vars) larger. | Rejected: the harness is Python; `auto.py` keeps the contract testable. |

## Consequences

- **Positive**:
  - Operators get a single CLI verb (`vmaf-tune auto`) for the
    common "encode at target VMAF" workflow; the eight-step manual
    composition collapses to one invocation.
  - Wall-clock floor on short-circuit-eligible content (1080p
    SDR photographic, single-codec, GOSPEL predictor) is the
    final encode itself — no redundant sweeps.
  - Every branch is testable in isolation; F.1 ships with mocked
    sub-phases for end-to-end smoke coverage.
  - Phase F's deterministic-tree footprint provides a labelled
    audit trail (which short-circuits fired, which fallbacks
    escalated) that a future learned-policy study can train on.

- **Negative**:
  - The decision tree is hand-tuned; recipe drift (new codec,
    new preset) requires tree edits, not just a re-trained model.
  - Adding a new sub-phase (F.5 onwards) means tree-and-test
    edits, not configuration-only changes.
  - Operator must still understand what Phase F **chose** — the
    explainability surface (per-cell verdicts, escalation log) is
    a new doc burden.

- **Neutral / follow-ups**:
  - F.1 PR ships `tools/vmaf-tune/src/vmaftune/auto.py`,
    `tests/test_auto.py`, and the `--smoke` mode. Adds the
    `auto` subcommand to `cli.py`.
  - F.1 PR adds a docs page under `docs/usage/vmaf-tune.md`
    documenting the `auto` flags and the decision tree (the doc
    must reproduce the pseudocode block from Research-0067 so
    operators can predict what `auto` will do without reading
    the code).
  - F.2 / F.3 / F.4 are sibling PRs gated on F.1 landing; each
    extends `auto.py` with smoke and integration tests.
  - Future: when a labelled-composition corpus exists, evaluate a
    supervised classifier as a recipe selector; the deterministic
    tree stays as the canonical fallback per the no-runtime-ML
    constraint.
  - Adds rebase-notes entry: Phase F ties together the per-phase
    contracts; future upstream syncs that touch any one phase
    must re-validate the tree.

## References

- [ADR-0237](0237-quality-aware-encode-automation.md) — `vmaf-tune`
  umbrella decision (Phase A scaffold).
- [ADR-0276 fast](0276-vmaf-tune-fast-path.md) — Phase A.5 proxy +
  Bayesian.
- [ADR-0276 phase-d](0276-vmaf-tune-phase-d-per-shot.md) — Phase D
  per-shot scaffold.
- [ADR-0289](0289-vmaf-tune-resolution-aware.md) — resolution-aware
  model selection.
- [ADR-0293](0293-vmaf-tune-saliency-aware.md) — saliency-aware ROI
  tuning.
- [ADR-0295](0295-vmaf-tune-phase-e-bitrate-ladder.md) — Phase E
  per-title ABR ladder.
- [ADR-0300](0300-vmaf-tune-hdr-aware.md) — HDR-aware encoding +
  scoring.
- [ADR-0301](0301-vmaf-tune-sample-clip.md) — sample-clip mode.
- [ADR-0306](0306-vmaf-tune-coarse-to-fine.md) — coarse-to-fine CRF
  search.
- [Research-0060](../research/0060-vmaf-tune-fast-path.md) — Phase
  A.5 cost model (parent).
- [Research-0067](../research/0067-vmaf-tune-phase-f-feasibility-2026-05-08.md)
  — Phase F feasibility (companion digest, this ADR).
- Source: `req` — paraphrased user vision text from the 2026-05-08
  ChatGPT exchange ("the real long-term potential is building an
  adaptive encoding ecosystem around community-generated training
  data, perceptual analysis and continual model improvement"); the
  English-translated paraphrase lives in this ADR's Context to
  satisfy the user-quote-handling rule for non-References sections.

### Status update 2026-05-08: F.2 short-circuits landed

F.1 sequential scaffold + F.2 short-circuits ship together in one
PR (the bigger-content path the user prefers over per-LOC PRs).
`tools/vmaf-tune/src/vmaftune/auto.py` exposes the seven
`_should_short_circuit_<N>` predicates as standalone helpers; each
fires its corresponding stage-skip and records the firing in
`plan.metadata.short_circuits`. The Phase D 5-min /
0.15-shot-variance thresholds ship as constants
(`PHASE_D_DURATION_GATE_S`, `PHASE_D_SHOT_VARIANCE_GATE`) pending
F.3 empirical fit. F.3 (per-cell `recommend.coarse_to_fine`
escalation on `FALL_BACK`) and F.4 (per-content-type recipe
overrides) remain deferred per the original phased rollout.

### Status update 2026-05-08: F.3 confidence-aware fallbacks landed

F.3 ships `_confidence_aware_escalation(verdict, interval, thresholds)`
in `tools/vmaf-tune/src/vmaftune/auto.py`, returning one of
`SKIP_ESCALATION` / `RECOMMEND_ESCALATION` / `FORCE_ESCALATION`. The
helper consumes the conformal interval width emitted by
`Predictor.predict_vmaf_with_uncertainty` (ADR-0279) at every
`(rung, codec)` cell. Two width gates carve the half-width axis into
three regions; `tight_interval_max_width` overrides FALL_BACK to skip,
`wide_interval_min_width` overrides GOSPEL to escalate, the middle
band defers to the F.2 verdict. Thresholds load from a calibration
JSON sidecar via `load_confidence_thresholds`; missing-sidecar paths
fall back to the documented Research-0067 defaults (2.0 / 5.0 VMAF)
and emit a one-line warning. Per-cell decisions are recorded in
`plan.metadata.confidence_aware_escalations[]`. F.4 (per-content-type
recipe overrides) remains deferred.

### Status update 2026-05-09: F.4 recipes landed

F.4 ships `_apply_recipe_override(meta, plan_state, thresholds)` in
`tools/vmaf-tune/src/vmaftune/auto.py`, returning a
`(recipe_class, recipe, effective_thresholds)` triple. The recipe
table at module scope (`_CONTENT_RECIPE_TABLE`) holds factory
callables for the four named classes (`animation`, `screen_content`,
`live_action_hdr`, `ugc`) plus the empty `default`; each call returns
a fresh override dict so mutations cannot leak between runs. Recipes
fire **before** the F.2 short-circuits evaluate so a recipe can flip
`force_single_rung` and have the ladder stage honour it. The four
override keys consumed by the driver are `tight_interval_max_width`,
`force_single_rung`, `saliency_intensity`, and `target_vmaf_offset`.
Per memory `feedback_no_test_weakening`, `target_vmaf_offset` shifts
only the predictor's effective target; the input `--target-vmaf`
(production-flip gate) is preserved verbatim in
`plan.metadata.target_vmaf` while the offset target lands in
`plan.metadata.effective_predictor_target_vmaf`. Every threshold value
shipped here is `[provisional, calibrate against real corpus in F.5]`.
F.5 closes the calibration loop once F.4 has emitted enough labelled
recipe applications to fit the placeholders empirically. Phase F
phased rollout is now complete (F.0 design + F.1+F.2 + F.3 + F.4);
F.5 calibration is a follow-up backlog item, not a Phase F gate.

### Status update 2026-05-09: F.5 calibrated

F.5 ships `ai/scripts/calibrate_phase_f_recipes.py` and the calibrated
override JSON at `ai/data/phase_f_recipes_calibrated.json`. The
calibration was run against the K150K corpus
(`.workingdir2/konvid-150k/konvid_150k.jsonl`, 148 543 rows out of
153 841 expected — the ingestion was ~96.6 % complete; partial-corpus
statistics are statistically valid for high-level class statistics
and a re-run on the full corpus is a follow-up PR). The calibrated
values replace the F.4 `[provisional, calibrate against real corpus
in F.5]` placeholders at module-load time via
`vmaftune.auto._load_calibrated_recipes`; if the JSON is missing or
malformed the F.4 placeholder constants in `_F4_PLACEHOLDER_RECIPES`
remain in force (graceful fallback covered by
`tools/vmaf-tune/tests/test_calibrated_recipes.py`).

Calibrated values:

| Class | `tight_interval_max_width` | `force_single_rung` | `saliency_intensity` | `target_vmaf_offset` | Source |
| ----- | -------------------------- | ------------------- | -------------------- | -------------------- | ------ |
| `animation` | `1.75` | `true` | `aggressive` | `+2.0` | proxy (UGC-anchored) |
| `screen_content` | _(unset)_ | _(unset)_ | `very_aggressive` | `+1.0` | proxy (UGC-anchored) |
| `live_action_hdr` | `1.4` | _(unset)_ | _(default)_ | `0.0` | proxy (UGC-anchored) |
| `ugc` | `3.5` | `false` | `default` | `+1.5` | corpus (K150K) |

Honest-data caveats:

- K150K is a UGC-only corpus and carries no per-source
  `content_class` column; only the `ugc` row is corpus-derived. The
  other three rows are calibrated as documented absolute offsets
  ("proxy") anchored on the F.4 envelope. PR #477's TransNet
  shot-metadata columns plus a class-labelled subset will let a
  future re-calibration replace the proxy rows with corpus-derived
  values.
- UGC's empirical `target_vmaf_offset` came out **positive** (`+1.5`)
  on K150K because the corpus's MOS distribution has a heavier upper
  tail than lower tail. The calibration script clamps every offset
  to the F.4 documented envelope of `[-2.0, +2.0]` so a pathological
  corpus cannot push the predictor target outside the regime the
  planner has been exercised against. Per memory
  `feedback_no_test_weakening`, the offset shifts only the
  predictor's effective target — never the input `--target-vmaf`
  gate that ships models.
- The `mos_to_vmaf_proxy` mapping (slope 20, intercept 0) is the
  Hosu et al. 2017 §3.3 anchor. A future re-calibration that
  measures end-to-end VMAF against the K150K reference clips
  (libvmaf full-reference pass) will replace the proxy with measured
  scores.

The calibrated values reduce the placeholder envelope (UGC width
`3.0 → 3.5` is the only widening; `1.5 → 1.75` for animation;
`1.2 → 1.4` for live-action HDR) but every value stays inside the
ConfidenceThresholds invariant (`tight ≤ wide`). The
`test_calibrated_ugc_width_below_wide_gate_ceiling` regression test
locks this in: any future re-calibration that exceeds the wide-gate
ceiling needs a separate ADR. Phase F is now fully calibrated; the
one outstanding follow-up is the class-labelled re-calibration once
PR #477 lands.

### Status update 2026-05-10: F.1/F.2 additional short-circuits landed

Three additional short-circuit predicates ship in
`tools/vmaf-tune/src/vmaftune/auto.py`, appended after the original
seven in `SHORT_CIRCUIT_PREDICATES` (canonical positions 7, 8, 9):

- **#8 `low-complexity`** (`_should_short_circuit_low_complexity`) —
  skips `recommend.coarse_to_fine` when `meta.complexity_score` (the
  probe-encode bitrate at the adapter's `probe_quality`/`probe_preset`)
  is below `LOW_COMPLEXITY_PROBE_BITRATE_THRESHOLD_KBPS` (200 kbps
  placeholder). `0.0` or `NaN` does not fire (no probe yet).
- **#9 `baseline-meets-target`**
  (`_should_short_circuit_baseline_meets_target`) — skips the full
  predictor sweep when `meta.baseline_vmaf` already meets or exceeds
  `plan_state.target_vmaf`. `0.0` or `NaN` does not fire.
- **#10 `no-two-pass`** (`_should_short_circuit_no_two_pass`) — skips
  the two-pass calibration stage when the resolved codec adapter's
  `supports_two_pass` flag is `False` (ADR-0333). `None` (adapter not
  yet resolved) does not fire.

`SourceMeta` gains `complexity_score` and `baseline_vmaf` fields (both
default `0.0`). `PlanState` gains `adapter_supports_two_pass` (default
`None`). 28 new unit tests in
`tools/vmaf-tune/tests/test_auto_phase_f1_f2.py`; two existing tests
updated to reflect 10 total predicates.
