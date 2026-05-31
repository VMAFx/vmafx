<!-- markdownlint-disable MD013 MD060 -->
# ADR-0390: vmaf_tiny_v4 — mlp_large arch (opt-in only; arch ladder stops here)

- **Status**: Accepted
- **Date**: 2026-05-02
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: `ai`, `tiny-ai`, `model`, `inference`

## Context

PR #294 (parent ADR-0241) shipped `vmaf_tiny_v3` (mlp_medium, 6→32→16→1, 769 params), achieving Netflix 9-fold LOSO PLCC=0.9986 ± 0.0015 (vs v2's 0.9978 ± 0.0021). The PR's own report flagged a Phase-3e candidate `mlp_large` (6→64→32→16→1, ~2.7K params) for follow-up evaluation — does the next rung on the architecture ladder buy further headroom, or does the canonical-6 / 4-corpus regime saturate at v3's capacity?

This ADR records the empirical answer.

## Decision

We ship `vmaf_tiny_v4` (mlp_large, 3 073 params) as an **opt-in-only** model alongside v2 (production default) and v3 (opt-in higher-tier). The architecture ladder **stops at v4** — we will not pursue mlp_huge or further capacity scaling on the same canonical-6 + 4-corpus regime. Future quality gains require a regime change (more features, different corpus, or a fundamentally different fusion strategy), not a wider MLP.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Stay at v3 (mlp_medium); do not ship v4** | Smallest opt-in surface; clear "v3 is the top" story. | Loses the sub-rounding +0.0001 LOSO PLCC; user explicitly requested an empirical evaluation of the v4 candidate. | Rejected — task requires producing v4 + an empirical SHIP / NO-SHIP determination. |
| **Ship v4 as opt-in only, document arch ladder stops** *(chosen)* | Honest empirical record; preserves v3 as opt-in tier; closes the "is v4 worth it" question for future maintainers; +0.0001 PLCC + identical std vs v3 means no regression. | Adds a third tiny-AI fusion model surface (3 of them now). 14 KB ONNX vs v3's 4.5 KB (~3x). | Selected — task spec calls for SHIP if PLCC ≥ v3, which v4 narrowly passes. The "arch ladder stops here" guidance is the load-bearing future-protection. |
| **Ship v4 as production default, retire v3** | Single highest-PLCC model, less surface area. | +0.0001 mean PLCC delta is below natural single-seed noise; cannot justify retiring v3 (which already has its own ADR-0241). v4's 14 KB ONNX is 5.7x v2's 2.5 KB. | Rejected — gain is statistically indistinguishable from noise; cost is real. |
| **Train mlp_huge (6→128→64→32→16→1)** as v5 | Tests further saturation. | v4's flat result vs v3 already demonstrates saturation on the current regime. Spending compute + ONNX bytes on a parallel ladder rung that the v3→v4 result predicts will be flat is wasteful. | Rejected — saturation evidence is decisive enough; document the stop and move on. |

## Consequences

- **Positive**: v4 is registered + signed + smoke-validated, available to users who want a third tier without hand-rolling. The ADR's "arch ladder stops here" rationale prevents future agents/maintainers from spending cycles training v5/v6 on the same regime. Establishes a saturation reference point for canonical-6 + 4-corpus + 90-epoch Adam recipe.
- **Negative**: Three concurrent vmaf_tiny_* models (v2 default, v3 + v4 opt-in). Slightly more documentation surface. v4's 14 KB ONNX is ~6x v2's; trivial in absolute terms.
- **Neutral / follow-ups**: Future quality gains on tiny VMAF fusion regressors require *regime change* (richer feature set, larger corpus, multi-seed averaging, ensemble) — not deeper MLPs. If a maintainer revisits the arch ladder, this ADR is the prior art for "we already tried; it saturated".

## References

- Parent ADR-0241 (v3 mlp_medium, ladder candidate).
- Research digest: `docs/research/0048-vmaf-tiny-v4-mlp-large-evaluation.md`.
- LOSO metrics: `runs/vmaf_tiny_v4_loso_metrics.json` (9 folds, single seed for parity with v3).
- Source: PR #294 body — "v4 candidate: mlp_large (6 → 64 → 32 → 16 → 1, ~2.7K params); SHIP if PLCC ≥ v3's, DO NOT SHIP otherwise". Verbatim user direction in this session: train + benchmark v4 and report SHIP / NO-SHIP / OPT-IN per the gate.
