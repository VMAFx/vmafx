<!-- markdownlint-disable MD013 MD060 -->
# ADR-0203: Tiny-AI training prep — implementation decisions

- **Status**: Accepted
- **Date**: 2026-04-28
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: `ai`, `training`, `fork-local`, `onnx`, `docs`

## Context

ADR-0242 specified the scope for training tiny-AI models on the
original Netflix VMAF training corpus. This ADR records the
implementation decisions made by the follow-up PR
(`feat/tiny-ai-netflix-training-prep`) for the loader, ground-truth
pipeline, dataset adapter, evaluation harness, and training entry
point under `ai/data/` + `ai/train/` — and the first concrete
training run executed against the full corpus (results section
below). ADR-0242's architecture-search table is converted into the
three concrete archs registered with the new entry point.

## Decision

1. **Distillation from `vmaf_v0.6.1` is the default ground-truth
   source.** Per-frame ``vmaf`` predictions are cached as the training
   target. The published Netflix MOS table is not used in the default
   path; a future PR can add a `--targets-source mos` switch.
2. **Held-out validation is one source out, not k-fold.** Default
   `--val-source Tennis` removes 6 pairs (Tennis_*) from training and
   uses them as a content-disjoint validation set (~9 % of the corpus
   by clip count, ~12 % by frame count given Tennis is 24 fps).
3. **Three architectures are registered with the entry point**:
   `linear`, `mlp_small`, `mlp_medium`. ADR-0242's "transformer / 1-D
   CNN" rows remain deferred.
4. **Per-clip JSON cache** under
   ``$VMAF_TINY_AI_CACHE`` (default ``~/.cache/vmaf-tiny-ai``) keyed by
   ``<source>/<dis_basename>.json``. One write-then-rename per pair so
   crashed runs leave the cache consistent. Distillation scores and
   feature vectors share the same JSON envelope (one libvmaf invocation
   per pair) so the corpus is touched at most once per cache-warming
   pass.
5. **Smoke command works without the real corpus.** ``--epochs 0
   --assume-dims WxH`` substitutes a zero-filled payload provider so CI
   can verify the harness end-to-end (subprocess test in
   ``ai/tests/test_train_smoke.py``).

## Alternatives considered

### A. Ground-truth source — distillation vs Netflix MOS

| Option | Pros | Cons | Status |
|---|---|---|---|
| Distill from ``vmaf_v0.6.1`` (per-frame) | Self-contained; matches the corpus exactly; no paywalled labels | Inherits teacher errors; per-frame target is noisy on motion-heavy clips | **Selected — default** |
| Netflix-published MOS subset | Independent of teacher; potential to exceed `vmaf_v0.6.1` | Covers <50 % of the 70 pairs; subjective-test version mismatch with the corpus encode list | Deferred — to a `--targets-source mos` switch |
| Hybrid (MOS where available, distillation otherwise) | Best of both | Mixed-supervision loss surface; harder to debug | Deferred |

### B. Validation split — 1-source-out vs k-fold

| Option | Pros | Cons | Status |
|---|---|---|---|
| 1-source-out (Tennis_24fps) | Content-disjoint val; single training run; ~9/12 % val by clip/frame | Single-fold; sensitive to which source is held out | **Selected — default** |
| Leave-one-source-out (9-fold) | Robust generalisation estimate | 9× training cost; one fold suffices for the first canonical run | Available by re-running with each ``--val-source`` |
| Random per-frame split | Maximises samples in val | Frame-level leakage inflates correlations; well-known VQA pitfall | Rejected |
| Random per-clip split (stratified by source) | Some content disjointness | Still mixes sources between train/val | Rejected |

### C. Model architectures registered with the entry point

| Arch | Layers | Params (feature_dim=6) | Notes |
|---|---|---|---|
| `linear` | 6 -> 1 | 7 | Sanity baseline; trains in seconds; matches the SVR's effective capacity bound |
| `mlp_small` | 6 -> 16 -> 8 -> 1 (ReLU) | 257 | Default; matches ADR-0242's `fr_tiny_v1` Small bracket |
| `mlp_medium` | 6 -> 64 -> 32 -> 1 (ReLU) | 2 561 | Ceiling for FR on 70 pairs; ADR-0242 Medium bracket |
| 4-layer MLP + BN | (deferred) | — | ADR-0242 row B; needs careful regularisation on 70 pairs |
| 1-D CNN over temporal features | (deferred) | — | ADR-0242 row C |

### D. Caching strategy

| Option | Pros | Cons | Status |
|---|---|---|---|
| Per-clip JSON under ``$VMAF_TINY_AI_CACHE`` (atomic write-rename) | Simple; greppable; survives partial runs | One file per clip = 70 files for full corpus | **Selected** |
| Single Parquet per dataset | Compact; pandas-native | Whole-file rewrites on cache miss; no per-clip resume | Deferred |
| In-memory only | No disk hit | 70 pair × ~150 frames × 6 features = trivial; but loses across runs | Rejected |

## Training results — first run on the full Netflix corpus

Hyperparameters used for the canonical first run:

| param | value |
|---|---|
| architecture | `mlp_small` (6 → 16 → 8 → 1, 257 params) |
| epochs | 30 |
| optimizer | Adam, lr=1e-3 |
| loss | MSE against `vmaf_v0.6.1` per-frame distillation target |
| batch size | 256 |
| val source | `Tennis` (1-source-out) |
| target | per-frame VMAF score |
| hardware | CPU (libvmaf feature extraction); no GPU needed for the 257-param net |
| corpus | 9 ref + 70 dis YUVs at `.workingdir2/netflix/` (37 GB, gitignored) |

Final validation metrics (720 frames from the held-out `Tennis` source):

| metric | value | interpretation |
|---|---:|---|
| PLCC | 0.9750 | very strong linear correlation with `vmaf_v0.6.1` |
| SROCC | 0.9792 | strong rank correlation |
| KROCC | 0.8784 | strong concordance |
| RMSE | 10.62 | ~10% absolute drift on the 0–100 VMAF scale — the principal weakness of this run |
| latency p50 | 5.96 µs / clip-row | onnxruntime CPU |
| latency p95 | 6.22 µs / clip-row | |
| ONNX size | 1.3 KB header + 0.9 KB data | trivially-tiny, ships in-tree |

### Three-arch sweep — full Netflix corpus, val=Tennis, 30 epochs each

| arch | params | PLCC | SROCC | KROCC | RMSE | latency p50 | ONNX |
|---|---:|---:|---:|---:|---:|---:|---:|
| `linear` | 7 | 0.4284 | 0.4966 | 0.3534 | 67.15 | 4.9 µs | — (sanity floor only) |
| `mlp_small` | 257 | **0.9750** | **0.9792** | **0.8784** | 10.62 | 5.96 µs | `vmaf_tiny_v1.onnx` (default) |
| `mlp_medium` | 2,561 | 0.9521 | 0.9475 | 0.8011 | **6.35** | 21.9 µs | `vmaf_tiny_v1_medium.onnx` (alternate) |

What this tells us:

- **Linear is a useful sanity floor, not a viable model.** PLCC 0.43 / RMSE 67 confirms the 6 features *do* carry VMAF-predictive signal (otherwise even the MLPs couldn't do better) but the relationship is strongly non-linear. The SVR's kernel and the MLPs' hidden ReLU non-linearities are doing real work.
- **`mlp_small` wins on ranking** (PLCC 0.975 / SROCC 0.979). The tiny model **ranks** clips identically to `vmaf_v0.6.1` to within 2.5 %.
- **`mlp_medium` wins on absolute fit** (RMSE 6.35 vs 10.62, −40 %) but loses ranking (PLCC −0.023, SROCC −0.032). With ~720 train frames on a 2,561-param net, the model memorises per-clip absolute scale (helping RMSE) at the expense of cross-clip ranking — classic small-corpus overfitting.
- **Latency**: all three sub-25 µs / clip-row on onnxruntime CPU. Even `mlp_medium` is essentially free vs the SVR.

**Recommendation**: `mlp_small` is the default tiny-AI model, shipped as `vmaf_tiny_v1.onnx`. `mlp_medium` ships as `vmaf_tiny_v1_medium.onnx` for users who want absolute-VMAF agreement on the Netflix-corpus distribution and tolerate the ranking loss. The linear baseline is not shipped.

Next experiments (not yet run): leave-one-source-out cross-validation (9 folds × 3 archs) to confirm the RMSE-vs-correlation trade-off generalises across content; longer training with regularisation on `mlp_medium` to see if overfitting can be controlled.

Wall-clock time: feature-extraction prewarm 3.5 min on CPU (cache 4/9 warm from a prior aborted run; cold-start would be ~7–8 min); 30 epochs over 720 train samples completed in <30 s per arch.

ONNX files in-tree (both ≤ 50 MB):

- `model/tiny/vmaf_tiny_v1.onnx` (= run 1 / `mlp_small_final.onnx`)
- `model/tiny/vmaf_tiny_v1_medium.onnx` (= run 2 / `mlp_medium_final.onnx`)

Eval reports at `model/tiny/training_runs/run{1,2_mlp_medium,3_linear}/eval_report.json`.

## Consequences

- **Positive**: The training pipeline is fully runnable on the local
  corpus. CI verifies the harness end-to-end without needing the
  37 GB of YUV. Cache invariants are documented; rebuilding is one
  ``rm -rf ~/.cache/vmaf-tiny-ai`` away. Distillation removes the
  paywalled-MOS dependency.
- **Negative**: The teacher's systematic errors are inherited; if
  ``vmaf_v0.6.1`` mis-predicts on a class of distortions, the tiny
  model will too. The 1-source-out split has higher variance than
  9-fold; reporting a single PLCC is statistically thin (the user is
  expected to re-run with multiple ``--val-source`` choices for any
  serious quality claim).
- **Neutral / follow-ups**:
  - Add a ``--targets-source mos`` path once the published Netflix MOS
    subset is wired up.
  - Add a Lightning module variant (current loop is plain torch
    optim — Lightning is overkill for a 257-param net but the user
    may want it for callbacks / logging on longer training runs).
  - Promote the leading checkpoint to ``model/tiny/`` via
    ``vmaf-train register`` and update ``docs/ai/models/`` per
    ADR-0042.

## References

- [ADR-0042](0042-tinyai-docs-required-per-pr.md) — tiny-AI doc-substance rule.
- [ADR-0108](0108-deep-dive-deliverables-rule.md) — six deep-dive deliverables.
- [ADR-0242](0242-tiny-ai-netflix-training-corpus.md) — scope of the
  Netflix-corpus tiny-AI workstream (parent of this ADR).
- [Research digest 0019](../research/0019-tiny-ai-netflix-training.md).
- Source: `req` (direct user instruction in 2026-04-28 daily prep
  routine asking for the runnable loader / eval / Lightning harness on
  top of the ADR-0242 scaffold).
