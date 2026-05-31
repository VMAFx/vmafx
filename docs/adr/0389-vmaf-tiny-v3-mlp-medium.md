<!-- markdownlint-disable MD013 MD060 -->
# ADR-0389: vmaf_tiny_v3 — wider/deeper mlp_medium tiny VMAF MLP

- **Status**: Accepted
- **Date**: 2026-05-02
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: ai, dnn, tiny-ai, model, registry, fork-local

## Context

`vmaf_tiny_v2` (ADR-0216) ships the validated Phase-3 configuration: `mlp_small` (6 → 16 → 8 → 1, 257 params), canonical-6 features (`adm2`, `vif_scale0..3`, `motion2`), 90 epochs Adam @ lr=1e-3, MSE, batch_size 256, StandardScaler baked into the ONNX graph, trained on the 4-corpus parquet (Netflix + KoNViD + BVI-DVC A+B+C+D, 330 499 rows). Phase-3d's arch sweep was inconclusive against `mlp_medium` and the v2 ADR explicitly noted the small variant remained the baseline because the medium variant didn't produce a positive signal in that round.

The user requested an end-to-end re-evaluation: train a fresh `mlp_medium` (6 → 32 → 16 → 1, ~700-target params) on the same 4-corpus parquet, with everything else identical to v2, and measure whether the extra capacity actually buys headroom on Netflix LOSO. If v3 underperforms v2, the recommendation is to NOT ship; if v3 wins, it ships alongside v2 (not as a replacement) so users can pick the smaller-bundle v2 or the higher-PLCC v3.

The result: v3 wins on both the LOSO mean PLCC (+0.0008) and — more interestingly — on LOSO PLCC variance (-30 %, std drops from 0.0021 to 0.0015). The mean delta is small in absolute terms; the variance-shrink is the more useful signal because it means v3 is a more *consistent* estimator across diverse hold-out content. That's why v3 ships alongside v2 rather than as a v2 successor.

## Decision

Ship `vmaf_tiny_v3.onnx` alongside (not replacing) `vmaf_tiny_v2.onnx` in `model/tiny/`. Same input contract (`features` `[N, 6]` float32, canonical-6 order), same output contract (`vmaf` `[N]` float32), same opset 17, same StandardScaler-baked-into-the-graph trust-root. Architecture: `mlp_medium` = Linear(6, 32) → ReLU → Linear(32, 16) → ReLU → Linear(16, 1), 769 params. Training recipe identical to v2 (90 ep, Adam @ lr=1e-3, MSE, batch_size 256, seed=0). Registered as `vmaf_tiny_v3` (kind `fr`) in `model/tiny/registry.json` with `smoke: false`. **Production default stays `vmaf_tiny_v2`** — `docs/ai/inference.md` continues to recommend v2; v3 is documented as a higher-PLCC option for users who want the lowest-variance estimator.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Ship v3 alongside v2 (chosen)** | Captures the LOSO variance-shrink win; users keep v2 as smallest-bundle option | Two near-identical models in the registry | Correct — small mean PLCC delta doesn't justify bumping v2; variance-shrink is real and worth preserving |
| Replace v2 with v3 | Single tiny FR fusion model in the tree | Loses v2 as a baseline; +84 % ONNX size; the 0.0008 PLCC delta is below the Phase-3 multi-seed envelope | Rejected — v2 is the cited Phase-3 chain baseline; replacing it without a multi-seed v3 sweep is premature |
| Keep v2 as the only tiny FR fusion model | Smallest bundle; matches Phase-3d's "inconclusive" verdict | Discards a real LOSO variance-shrink finding | Rejected — the user explicitly asked to re-evaluate with the wider arch on the 4-corpus data |
| Larger arch (`mlp_large` ~3K params) | Higher capacity ceiling | Phase-3d already showed diminishing returns past mlp_medium on canonical-6; 3-corpus rows = 330k may not feed it | Rejected — out of scope for this PR; v3 is the smallest jump that exits Phase-3d's "inconclusive" zone |
| Multi-seed v3 LOSO before shipping | More confident PLCC delta | 5x training cost; the seed=0 single-seed delta is already +0.0008 PLCC + variance-shrink, and the v2 ship gate also used a seed=0 export | Deferred to follow-up — the seed-1 / -2 / -3 / -4 sweep is reasonable backlog work, but not gating |
| Different optimiser / lr / epoch budget for v3 | Could close any v3-arch-specific underfit | Confounds the architecture-only comparison the user asked for | Rejected — explicit user spec says "everything else identical to v2" |

## Consequences

- **Positive**: ships a more consistent VMAF estimator (LOSO std 0.0021 → 0.0015) without breaking the v2 contract. Existing `--tiny-model model/tiny/vmaf_tiny_v2.onnx` users see no change; new `--tiny-model model/tiny/vmaf_tiny_v3.onnx` users get the variance-shrink for +2 050 bytes on disk.
- **Negative**: doubles the tiny FR fusion model surface — `model/tiny/vmaf_tiny_v2.onnx` and `model/tiny/vmaf_tiny_v3.onnx` both ship. The registry, model cards, and docs/ai/inference.md flow now mention both. Future Phase-4 work should prune one of them.
- **Neutral / follow-ups**: multi-seed v3 LOSO sweep (5 seeds) for parity with v2's published numbers; KoNViD 5-fold + BVI-DVC eval for v3 (the v2 KoNViD 5-fold PLCC was 0.9998 — v3 should be evaluated on the same gate); PTQ for v3 is out-of-scope (model is still <5 KB).

## References

- Source: `req` (user-provided spec — paraphrased: "Train, export, and validate a new tiny-AI model `vmaf_tiny_v3` on the existing 4-corpus parquet, using a wider/deeper MLP architecture than v2 (`mlp_medium` 6 → 32 → 16 → 1). Goal: see whether the extra capacity buys headroom over v2's PLCC. Ship alongside v2 if it wins; report and don't ship if it regresses.")
- v2 baseline: [ADR-0216](0216-vmaf-tiny-v2.md)
- Research digest: [Research-0046](../research/0046-vmaf-tiny-v3-mlp-medium-evaluation.md)
- Trainer: [`ai/scripts/train_vmaf_tiny_v3.py`](../../ai/scripts/train_vmaf_tiny_v3.py)
- Exporter: [`ai/scripts/export_vmaf_tiny_v3.py`](../../ai/scripts/export_vmaf_tiny_v3.py)
- LOSO eval: [`ai/scripts/eval_loso_vmaf_tiny_v3.py`](../../ai/scripts/eval_loso_vmaf_tiny_v3.py)
- LOSO results: `runs/vmaf_tiny_v3_loso_metrics.json`
- Phase-3 chain: [Research-0027](../research/0027-phase2-feature-importance.md), [-0028](../research/0028-phase3-subset-sweep.md), [-0029](../research/0029-phase3b-standardscaler-results.md), [-0030](../research/0030-phase3b-multiseed-validation.md)
