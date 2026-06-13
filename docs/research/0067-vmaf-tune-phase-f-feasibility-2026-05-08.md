<!-- markdownlint-disable MD060 -->
# Research-0067: `vmaf-tune` Phase F — adaptive recipe-aware composition feasibility

- **Date**: 2026-05-08
- **Companion ADR**: [ADR-0325](../adr/0325-vmaf-tune-phase-f-auto.md) (Proposed)
- **Parent ADR**: [ADR-0237](../adr/0237-quality-aware-encode-automation.md)
  (umbrella)
- **Status**: Snapshot at proposal time. Implementation PRs (F.1+) will
  refine the cost numbers from real measurements; this digest stays as
  the "why a deterministic decision tree, not a learned policy"
  reference.

## Question

Phases A through E of `vmaf-tune` each ship as a standalone CLI
subcommand:

- `corpus` — Phase A grid sweep ([ADR-0237](../adr/0237-quality-aware-encode-automation.md)).
- `fast` — Phase A.5 proxy + Bayesian recommend ([ADR-0276](../adr/0276-vmaf-tune-fast-path.md)).
- `recommend` — coarse-to-fine target-VMAF / target-bitrate predicate
  ([ADR-0306](../adr/0306-vmaf-tune-coarse-to-fine.md)).
- `predict` — per-title CRF prediction (Phase C, fr_regressor_v2 client).
- `tune-per-shot` — Phase D per-shot CRF orchestration
  ([ADR-0276 phase-d](../adr/0276-vmaf-tune-phase-d-per-shot.md)).
- `recommend-saliency` — saliency-aware ROI tuning
  ([ADR-0293](../adr/0293-vmaf-tune-saliency-aware.md)).
- `ladder` — Phase E per-title ABR ladder
  ([ADR-0295](../adr/0295-vmaf-tune-phase-e-bitrate-ladder.md)).
- `compare` — codec-vs-codec head-to-head.

Plus orthogonal modes: HDR auto-detect ([ADR-0300](../adr/0300-vmaf-tune-hdr-aware.md)),
sample-clip ([ADR-0301](../adr/0301-vmaf-tune-sample-clip.md)),
resolution-aware model selection ([ADR-0289](../adr/0289-vmaf-tune-resolution-aware.md)).

The operator-facing question — "what is the cheapest encode that
hits VMAF ≥ 92 for **this** content?" — currently requires the
operator to know which subcommands to compose, in what order, and
which orthogonal flags apply. The user's vision text (paraphrased
from the 2026-05-08 ChatGPT exchange) frames the long-term direction
as "an adaptive encoding ecosystem around community-generated
training data, perceptual analysis and continual model improvement";
Phase F is the first composition layer that exposes that ecosystem
as a single CLI verb.

The question this digest answers: **can a deterministic decision
tree pick the right composition for the operator without
sacrificing the expressiveness or reproducibility of the underlying
phases?**

## Current composition cost (manual)

The colleague's "day per movie" complaint from the original
predictor plan motivates the cost table. Numbers below are wall-clock
estimates for a typical 2-hour 1080p source on a 12-core x86-64
workstation (no GPU encoder), encoding with `libx264 medium` to a
single rendition at target VMAF 92.

| Step | Today's command | Wall-time | Operator effort |
|---|---|---|---|
| 1. Pick codec (intuition or heuristic) | n/a | 0 (manual) | Read content, guess |
| 2. Build (preset, CRF) corpus | `vmaf-tune corpus` over 6 cells × full encode | ≈ 70 min | Type 6 flags |
| 3. Refine corpus around target | `vmaf-tune recommend --target-vmaf 92` (coarse-to-fine) | ≈ 12 min | Type 4 flags |
| 4. Decide per-title vs per-shot | `vmaf-tune tune-per-shot` (TransNet V2 + per-shot encode) | ≈ 35 min | Read shot count + type 5 flags |
| 5. Decide ladder rungs | `vmaf-tune ladder` over 5 resolutions | ≈ 90 min | Pick resolution set |
| 6. Decide saliency on / off | `vmaf-tune recommend-saliency` re-run | ≈ 15 min | Pick threshold |
| 7. Decide HDR vs SDR | inspect ffprobe; flip `--hdr` | ≈ 1 min | Read color metadata |
| 8. Pick winning codec via compare | `vmaf-tune compare` over { x264, x265, svt-av1 } | ≈ 3.5 h | Type 8 flags |
| **Total (sequential, naive)** | | **≈ 5.5–6 h** | **≈ 8 manual decisions** |
| Total (with predictor in step 3) | drops step 2+3 to ≈ 3 min on hot proxy | **≈ 5 h** | unchanged |

The wall-time floor is dominated by codec comparison + ladder; the
operator-effort floor is dominated by the eight manual decisions
that today have no automation gluing them together. Phase F's
target is to collapse the eight decisions into one CLI invocation
and let the harness short-circuit the expensive steps when the
answer is obvious.

## Phase F decision tree (pseudocode)

The decision tree must fit on one page (project rule per the design
brief). Anything not shown below delegates to the existing
subcommand contracts.

```text
auto(src, target_vmaf, max_budget_kbps, allow_codecs):
    # 1. Cheap probe — ffprobe metadata only
    meta = probe(src)                    # res, codec, fps, color, duration
    is_hdr = detect_hdr(meta)            # ADR-0300

    # 2. Resolution short-circuit (ADR-0289)
    if meta.height < 2160 and not user_overrode_ladder:
        rungs = [meta.resolution]        # single-rung ladder
    else:
        rungs = ladder.candidate_rungs(meta)

    # 3. Codec short-circuit
    if len(allow_codecs) == 1:
        codecs = allow_codecs
    elif user_pinned_codec:
        codecs = [user_pinned_codec]
    else:
        codecs = compare.shortlist(allow_codecs, meta)  # filter by license/HW

    # 4. Predictor pass per (rung, codec) — Phase C
    plan = []
    for rung in rungs:
      for codec in codecs:
        v = predict.crf_for_target(rung, codec, target_vmaf, meta)
        # v is one of: GOSPEL | LIKELY | FALL_BACK
        if v.verdict == FALL_BACK:
            v = recommend.coarse_to_fine(rung, codec, target_vmaf)
        plan.append((rung, codec, v))

    # 5. Per-shot vs whole-source — Phase D gate
    if duration > 5min and shot_variance(src) > 0.15:
        plan = [tune_per_shot.refine(p) for p in plan]

    # 6. Saliency gate — content-type heuristic
    if meta.content_class in {animation, screen_content}:
        plan = [recommend_saliency.maybe_apply(p) for p in plan]

    # 7. Final scoring on real encode of the chosen rung × codec
    winner = pick_pareto(plan, target_vmaf, max_budget_kbps)
    return realise(winner, hdr=is_hdr)
```

Lines: 22 logical, well under the 30-line ceiling. Every branch is
an `if`/loop over an existing subcommand's public contract; Phase F
adds **no** new sub-phase.

## Cost model

Per-decision wall-clock budget on the same 12-core 1080p workstation.
Numbers in seconds unless noted; `N_shots` ≈ 60 for a 2-hour title.

| Decision | Function | Wall-time | Notes |
|---|---|---|---|
| Probe + HDR detect | `ffprobe` once | < 0.5 s | Cheap; always run. |
| Resolution shortlist | `ladder.candidate_rungs` | < 0.1 s | Pure metadata. |
| Codec shortlist | `compare.shortlist` | < 0.1 s | License + HW availability filter. |
| Predictor pass | `predict.crf_for_target` per (rung, codec) | 5–50 ms each | fr_regressor_v2 inference; budget 1 s for ≤ 20 cells. |
| Coarse-to-fine fallback | `recommend.coarse_to_fine` | 60–180 s | Only on FALL_BACK verdict. |
| Per-shot refine (D) | `tune_per_shot.refine` | 8–25 min | Gated; skip on short / low-variance content. |
| Saliency (B2) | `recommend_saliency.maybe_apply` | 30–90 s | Gated by content class. |
| Final encode | `encode.run_encode` (winner only) | 10–60 min | Real encode at the chosen rung × codec; not a sweep. |

Total budget when **all short-circuits hit** (1080p, single codec
allowed, GOSPEL predictor, low shot variance, photographic content):
≈ 11–61 min, dominated by the single final encode. Total when no
short-circuit hits (4K, 3 codecs, FALL_BACK, animation, high shot
variance): ≈ 3–4 h, still ≈ 33 % faster than the current naive
manual composition because Phase F skips redundant
encode-and-score sweeps once a verdict converges.

## When Phase F should short-circuit

These cases bypass entire phases:

1. **Single-rung ladder.** `meta.height < 2160` and no
   `--ladder-rungs` override → skip Phase E entirely; one rung at
   the source resolution.
2. **Codec already known.** `--codec=libx264` or `len(allow_codecs)==1`
   → skip `compare.shortlist` and the cross-codec encode pass.
3. **Predictor verdict GOSPEL.** Phase C returns GOSPEL on the
   chosen (rung, codec) → skip `recommend.coarse_to_fine`; trust
   the predicted CRF for the final encode.
4. **Short / low-variance source.** `duration < 5 min` or shot
   variance below the [ADR-0276 phase-d](../adr/0276-vmaf-tune-phase-d-per-shot.md)
   threshold → skip Phase D per-shot refine; one CRF for the whole
   title.
5. **Photographic, non-screen, non-animation content.** Saliency
   skipped unless an explicit `--saliency=on` override is set.
6. **SDR source.** HDR pipeline skipped; Phase F continues with the
   `vmaf_v0.6.1` model.
7. **Sample-clip already provided.** When the user passes
   `--sample-clip-seconds N`, every internal sweep inherits the
   same window via the [ADR-0301](../adr/0301-vmaf-tune-sample-clip.md)
   mechanism; no re-derivation.

## Failure modes

1. **Predictor confidence collapse.** fr_regressor_v2 returns
   FALL_BACK on every (rung, codec) combination. Phase F escalates
   to the existing coarse-to-fine path; the cost rises but the
   answer is still bounded. The escalation rule is **per cell**:
   a single FALL_BACK does not poison the others.
2. **Encoder ROI surface unavailable.** Saliency requested but
   `vmaf-roi` binary missing. Phase F downgrades to non-saliency
   encode and emits a warning row in the JSON output; never aborts.
3. **Source resolution / codec / framerate mismatch.** Source is
   1440p (no fork model exists) → falls back to `vmaf_v0.6.1` per
   [ADR-0289](../adr/0289-vmaf-tune-resolution-aware.md). VFR
   source → Phase F refuses (per the existing per-shot tool
   contract; non-trivial to bisect VFR safely).
4. **Budget overrun.** Predicted bitrate exceeds
   `--max-budget-bitrate` on every Pareto candidate → Phase F
   surfaces the closest candidate and exits non-zero with a
   diagnostic row; never silently picks an over-budget encode.
5. **Conflicting flags.** `--codec` AND `--allow-codecs` both set
   → the singular pin wins, sibling list is logged as ignored.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Deterministic decision tree (chosen)** | Explainable, reproducible, every branch maps to an existing ADR contract; trivially testable; no runtime ML dependency. | Hard-codes priorities; new sub-phases require tree edits. | Picked: matches the fork's "no learned policy at runtime" constraint and the user's "explainable, no closed services" framing. |
| Pure-grid composition (today's manual workflow) | Zero new code; fully reproducible. | 8-step manual composition; ≈ 5–6 h wall-clock; high operator-error rate. | Not chosen: the reason Phase F is a backlog item. |
| Optuna over the full composition space | Strong optimum; reuses the Phase A.5 search infrastructure. | Per-source TPE warm-up cost; no closed-form way to express "skip Phase D when source is short"; opaque to operators ("why did it pick x265?"). | Not chosen: the optimum-over-recipes problem has too few independent samples per source for Bayesian search to beat a hand-tuned tree. |
| Learned policy (RL or supervised over a Phase A corpus) | Adapts to corpus drift; the long-term "continual model improvement" arm of the user's vision. | Requires a labelled "this composition was right" dataset that doesn't exist; runtime inference adds an ONNX dependency to the auto path; reproducibility suffers (model drift between runs); violates the fork's "no learned-policy at runtime" rule for the auto entry point. | Not chosen for v1; revisit as a research experiment once the deterministic tree has produced a labelled corpus. |
| One mega-subcommand replacing all phases | Single user surface. | Breaks every existing per-phase contract; downstream consumers (CI, MCP server) lose stable hooks. | Rejected: the per-phase ADRs explicitly carve those contracts. |

## Recommendation

Ship Phase F as a **deterministic decision tree** (`vmaf-tune auto`)
that composes the existing subcommand contracts. Phase F.0 (this
ADR) is design-only; F.1 lands a sequential composition (no
short-circuits, no escalation), F.2 adds the seven short-circuits
above, F.3 adds confidence-aware fallbacks, F.4 adds per-content-type
recipe overrides. The decision tree is the v1 surface; learned
policy stays a research follow-up after the tree has produced
enough labelled compositions for a future supervised baseline.

The recommendation **does not** add any new sub-phase: Phase F is
integration, not invention.

## F.4 recipe-override placeholders (status update 2026-05-09)

F.4 (`_apply_recipe_override`, shipping with PR adding
`tools/vmaf-tune/tests/test_auto_recipe_overrides.py`) ships four
named recipes plus a `default` empty recipe. The threshold values
documented below are **provisional placeholders**, sized close to a
conservative emergency floor so the recipes still produce sane
behaviour even if the F.5 calibration fit never lands.

| Class | Threshold key | Placeholder | Rationale (provisional) |
| ----- | ------------- | ----------- | ----------------------- |
| `animation` | `tight_interval_max_width` | `1.5` | Predictor residuals are tighter on flat colour fields than on photographic content. |
| `animation` | `target_vmaf_offset` | `+2.0` | Animation is intrinsically more compressible at a given perceptual quality. |
| `screen_content` | `target_vmaf_offset` | `+1.0` | Saliency-aware QP allocation pushes the achievable target slightly higher. |
| `live_action_hdr` | `tight_interval_max_width` | `1.2` | The fr_regressor predictor (ADR-0279) was largely trained on SDR; a wide HDR interval is more suspect. |
| `ugc` | `tight_interval_max_width` | `3.0` | UGC's higher upstream-encode noise makes wider predictor intervals the baseline. |
| `ugc` | `target_vmaf_offset` | `-1.0` | UGC's perceptual ceiling is capped by source-side artefacts. |

**F.5 calibration backlog**: once F.4 has emitted enough labelled
recipe applications to make a per-class fit statistically defensible,
F.5 fits the placeholders empirically. The fit lands as a one-line
edit to each recipe factory (no API change). The placeholders here
are deliberately conservative — F.5 should produce values within an
order of magnitude of these, not order-of-magnitude shifts. If the
calibration fit produces wildly different values, that's a signal
that the recipe taxonomy itself needs revisiting (a sub-class split,
e.g. cel-animation vs CGI-animation), not a threshold tweak.

Per memory `feedback_no_test_weakening`, the F.5 calibration must
**not** produce values that widen the production-flip gate. The
`target_vmaf_offset` shifts only the predictor's effective target;
the input `--target-vmaf` (the gate that ships models) is preserved
verbatim across F.4 and F.5.
