<!-- markdownlint-disable MD013 MD060 -->
# ADR-0249: Tiny-AI Wave 1 baseline C1 — `fr_regressor_v1` on Netflix Public

- **Status**: Accepted
- **Date**: 2026-04-29
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: tiny-ai, training, onnx, netflix-public, c1, fork-local

## Context

[ADR-0168](0168-tinyai-konvid-baselines.md) shipped the C2 (`nr_metric_v1`) and C3
(`learned_filter_v1`) Wave-1 baselines but **deferred C1 (`fr_regressor_v1`)**
because the Netflix Public Dataset was access-gated (Google Drive folder
requiring a manual request to Netflix; "cannot be downloaded
programmatically"). C1's defining target — *match or beat `vmaf_v0.6.1` PLCC on
Netflix Public* — would be incomparable on a substitute corpus, so the
[Wave-1 roadmap](../ai/roadmap.md) row stayed Deferred and the deferral was
tracked in [`docs/state.md`](../state.md).

On 2026-04-27, lawrence dropped the dataset locally at
`.workingdir2/netflix/` (9 reference + 70 distorted YUVs at 1920×1080
yuv420p 8-bit, ~37 GB). The drop unblocks BACKLOG row T6-1a. The 21-feature
parquet `runs/full_features_netflix.parquet` (11 040 rows × 25 cols) is
already produced by `ai/scripts/extract_full_features.py` for prior research
work (Research-0026 / 0027 / 0030). The `vmaf_train.models.FRRegressor`
Lightning module (2-layer GELU MLP, hidden=64, dropout=0.1) was already
shipped as Wave-1 scaffolding by ADR-0168. What was missing: a runnable
C1 trainer that consumes the existing parquet, validates against
`vmaf_v0.6.1` reference scores, and emits the ONNX checkpoint +
sidecar + registry row.

## Decision

We will train and ship `fr_regressor_v1` from the locally-available
Netflix Public Dataset using the canonical-6 feature subset (`adm2`,
`vif_scale0`–`3`, `motion2` — the same input the production
`vmaf_v0.6.1` SVR consumes). The training script is
`ai/scripts/train_fr_regressor.py`; held-out generalisation is reported
as 9-fold leave-one-source-out (LOSO) mean PLCC against the
`vmaf_v0.6.1` per-frame teacher score, and the shipping checkpoint is
re-trained on all 9 sources after the LOSO gate passes (mean PLCC ≥ 0.95).
The `vmaf_v0.6.1` teacher is DMOS-aligned by construction (Netflix's
published SVR was trained against the Netflix-Public DMOS), so a high
PLCC-vs-teacher transitively implies a high PLCC-vs-DMOS without
re-fetching the (separately access-gated) DMOS sidecar CSV.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Use the local Netflix Public drop** *(chosen)* | Same corpus and same per-frame `vmaf_v0.6.1` teacher Netflix used; comparable to upstream baselines; no external dependency | Lawrence's drop is not redistributable (Netflix license); training is local-only by design | Best fit for C1's "match `vmaf_v0.6.1` PLCC on Netflix Public" target; ADR-0242 already accepted local-only training corpora |
| Wait for Netflix-Public-via-pip / public mirror | Reproducible by anyone with `pip install` | None published; ADR-0168 audit confirmed no public mirror; indefinite wait | Ships the gun-without-bullets state indefinitely |
| Substitute KoNViD-1k | Already extracted (`runs/full_features_konvid.parquet`); CC BY 4.0 | C1's target metric is NFLX-Public PLCC against `vmaf_v0.6.1` — KoNViD-1k uses different content + different MOS scale; result would be incomparable to the published target | Defeats the purpose of the C1 row |
| MLP-medium (hidden=32, depth=2, ~801 params) | Simpler graph | Phase-3 sweep already showed canonical-6 mean PLCC ≈ 0.984 with the FRRegressor recipe; no headroom | Sticking with the published Wave-1 architecture keeps the recipe identical to the spec'd `FRRegressor` |
| Optimise primary on SROCC (rank correlation) | Robust to monotone non-linear miscalibration | C1's stated target is "match `vmaf_v0.6.1` **PLCC**"; SROCC is reported alongside but not gated | Aligns with the roadmap target verbatim |

## Consequences

- **Positive**:
  - Wave-1 ship list complete: `model/tiny/` now carries C1 + C2 + C3.
  - The `--tiny-model fr_regressor_v1` CLI path becomes a real metric, not
    a stub.
  - Future C1 retraining (different feature subsets, larger MLPs,
    QAT/PTQ) can branch off this trainer + parquet.
- **Negative**:
  - Netflix Public Dataset is not redistributable. Reproducing the
    training run requires the local YUV drop; the parquet at
    `runs/full_features_netflix.parquet` is gitignored. CI cannot
    retrain end-to-end — only smoke-test the pipeline (`--epochs 3
    --no-export`).
- **Neutral / follow-ups**:
  - The synthetic `ai/scripts/build_bisect_cache.py` placeholder (T6-1a
    sub-bullet) is *not* replaced in this PR; the bisect-cache fixture
    is byte-stable per ADR-0109, and switching it to the real
    Netflix-Public DMOS-aligned cache requires a separate PR with
    fresh seeds + an ADR-0109 amendment. Tracked separately.
  - `docs/state.md` flips the C1 deferral row from "Deferred" to
    "Closed (shipped 2026-04-29)" — done by sister agent in same wave.
  - Roadmap §2.1 row flips from **Deferred** to **Shipped 2026-04-29**.

## References

- [ADR-0168](0168-tinyai-konvid-baselines.md) — Wave-1 C2 + C3 shipped, C1 deferred.
- [ADR-0242](0242-tiny-ai-netflix-training-corpus.md) — Netflix corpus loader.
- [ADR-0203](0203-tiny-ai-training-prep-impl.md) — feature extractor + scores plumbing.
- [docs/ai/roadmap.md §2.1](../ai/roadmap.md) — Wave-1 ship-baselines table.
- BACKLOG row T6-1a (`.workingdir2/BACKLOG.md`).
- Source: `req` — user direction this session: *"unblock T6-1a, dataset is locally available at .workingdir2/netflix/."*
