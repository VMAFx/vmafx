<!-- markdownlint-disable MD013 MD060 -->
# Research-0048: vmaf_tiny_v4 (mlp_large) evaluation — does the arch ladder saturate?

- **Date**: 2026-05-02
- **Status**: Complete
- **Authors**: Lusoris, Claude (Anthropic)
- **Companion ADR**: [ADR-0242](../adr/0242-vmaf-tiny-v4-mlp-large.md)

## Question

PR #294 (ADR-0241) shipped v3 (mlp_medium, 769 params) as the next rung above v2 (mlp_small, 257 params). The PR's own follow-up report flagged a Phase-3e candidate `mlp_large` (6→64→32→16→1, ~2.7K params): **does the next rung buy further LOSO PLCC headroom, or has the canonical-6 + 4-corpus regime saturated?**

## Methodology

Identical to v3's research-0046 except for the architecture function. Recipe:

- Features: canonical-6 = `(adm2, vif_scale0, vif_scale1, vif_scale2, vif_scale3, motion2)`.
- Preprocessing: corpus-wide StandardScaler (mean / std baked into ONNX graph as Constant nodes).
- Optimiser: Adam @ lr=1e-3, MSE loss, 90 epochs, batch_size 256, seed=0.
- Training corpus: 4-corpus parquet (Netflix Public + KoNViD-1k + BVI-DVC A+B+C+D, 330 499 rows).
- Architecture: `nn.Linear(6,64) → ReLU → nn.Linear(64,32) → ReLU → nn.Linear(32,16) → ReLU → nn.Linear(16,1)` — 3 073 params (the spec's "~2.7K" estimate undercounts by ~370; exact number is irrelevant, the saturation question stands).
- Validation: 5000-row Netflix smoke test (PLCC ≥ 0.97 gate) + 9-fold Netflix LOSO (single seed for parity with v3's eval; v2 was 5-seed).

## Results

### Architecture ladder comparison

| Model | Arch | Hidden | Params | ONNX size | Train RMSE | NF LOSO mean PLCC | NF LOSO std PLCC |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: |
| vmaf_tiny_v2 | mlp_small  | 6→16→8→1        |   257 |  2 446 B | 0.153 | 0.9978 (5-seed) | 0.0021 |
| vmaf_tiny_v3 | mlp_medium | 6→32→16→1       |   769 |  4 496 B | 0.112 | 0.9986 (1-seed) | 0.0015 |
| **vmaf_tiny_v4** | **mlp_large**  | **6→64→32→16→1** | **3 073** | **14 046 B** | **0.104** | **0.9987 (1-seed)** | **0.0015** |

### Per-fold LOSO breakdown (v4)

| Source | n | PLCC | SROCC | RMSE |
| --- | ---: | ---: | ---: | ---: |
| BigBuckBunny  | 1500 | 0.9995 | 0.9964 | 0.86 |
| BirdsInCage   | 1440 | 0.9999 | 0.9998 | 0.30 |
| CrowdRun      | 1050 | 0.9999 | 0.9998 | 0.76 |
| ElFuente1     | 1260 | 0.9993 | 0.9952 | 1.13 |
| ElFuente2     | 1620 | 0.9989 | 0.9996 | 1.22 |
| FoxBird       |  900 | 0.9952 | 0.9955 | 2.82 |
| OldTownCross  | 1050 | 0.9992 | 0.9999 | 1.23 |
| Seeking       | 1500 | 0.9989 | 0.9961 | 1.63 |
| Tennis        |  720 | 0.9977 | 0.9978 | 1.15 |
| **Mean**      |      | **0.9987** | **0.9978** | **1.23** |
| **Std (n-1)** |      | **0.0015** | **0.0020** | **0.70** |

### v3 → v4 delta

- Mean PLCC: +0.0001 (well below 1 std of either model).
- Mean SROCC: +0.0001.
- Std PLCC: identical (0.0015).
- Train-set RMSE: −0.008 (small improvement, reflecting overfit headroom not generalisation).
- ONNX size: +9 550 B (3.1x v3, 5.7x v2).

The per-fold table shows v4 wins narrowly on 5 / 9 folds and matches or loses on the rest — well within single-seed noise.

## Interpretation

The architecture ladder **saturates** at v3 on this regime. v4 trains to a marginally better train-set RMSE (over-fits a bit harder thanks to the ~4x parameters), but the held-out PLCC is statistically flat. This is consistent with the canonical-6 feature set being information-bottlenecked: with only 6 input dimensions, a 769-param MLP already has enough capacity to represent the per-frame VMAF target, and adding more capacity buys nothing on out-of-distribution sources.

## Decision matrix (mirrors ADR-0242)

| Option | Outcome |
| --- | --- |
| Stay at v3, don't ship v4 | Loses the empirical saturation evidence; user explicitly asked for v4. |
| **Ship v4 as opt-in, document ladder stops** *(chosen)* | Records saturation; protects future agents from re-running the same experiment. |
| Ship v4 as production default, retire v3 | +0.0001 PLCC < single-seed noise; not justified. |
| Train mlp_huge as v5 | v3→v4 saturation already predicts a flat outcome; not worth the compute. |

## Follow-ups

- Future quality gains require **regime change**: richer features, larger corpus, multi-seed averaging, or a fundamentally different fusion strategy (e.g. ensemble, distillation from a frame-level CNN). A wider MLP is no longer the lever.
- A multi-seed v3 + v4 LOSO study (5+ seeds, matching v2's protocol) would tighten the variance estimate and confirm the +0.0001 mean delta is noise, not signal. Optional follow-up; not gating.
- The 3-tier (v2 default, v3 + v4 opt-in) story is documented in `docs/ai/inference.md`; downstream users select via `--tiny-model model/tiny/vmaf_tiny_v4.onnx`.

## References

- ADR-0242 (this digest's companion).
- ADR-0241 (parent — v3 mlp_medium ladder candidate).
- PR #294 body (v3 ship + v4 candidate flag).
- LOSO JSON: `runs/vmaf_tiny_v4_loso_metrics.json`.
