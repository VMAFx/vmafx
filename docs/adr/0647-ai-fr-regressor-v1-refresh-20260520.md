<!-- markdownlint-disable MD060 -->
# ADR-0647: Refresh `fr_regressor_v1` from the 2026-05-20 Netflix feature table

- **Status**: Accepted
- **Date**: 2026-05-20
- **Deciders**: Lusoris, Codex
- **Tags**: ai, tiny-ai, model-refresh, netflix-public, fr-regressor, fork-local

## Context

The May 2026 bugfix wave changed enough upstream signal that table-derived AI
artifacts must be treated as stale: feature-extraction defaults now include the
speed features, the fork CPU `vmaf` binary path is explicit in refresh scripts,
CHUG pairing was repaired, and the dev-container encoder probes no longer hide
QSV/NVENC runtime failures. The first unblocked refresh is
`fr_regressor_v1`, because it depends only on the Netflix Public full-reference
feature table and not on the still-running KoNViD refresh or the downstream CHUG
HDR/MOS experiments.

The refreshed Netflix table is
`runs/full_features_netflix_refresh_20260520.parquet` with `11190` rows and
`30` columns. The training command reused the existing ADR-0249 recipe:

```bash
PYTHONPATH=ai/src .venv/bin/python ai/scripts/train_fr_regressor.py \
  --parquet runs/full_features_netflix_refresh_20260520.parquet \
  --metrics-out runs/fr_regressor_v1_refresh_20260520_metrics.json
```

## Decision

Refresh the shipped `model/tiny/fr_regressor_v1.onnx` checkpoint, sidecar, and
registry row from the 2026-05-20 Netflix full-feature table without changing the
model architecture or ship gate. The model remains the ADR-0249 C1 baseline:
canonical-6 input, stock `FRRegressor` hidden=64/depth=2/dropout=0.1, opset 17,
and PLCC-only ship gate at mean LOSO PLCC >= 0.95.

The refreshed run produced:

| Metric | Previous sidecar | 2026-05-20 refresh |
| --- | ---: | ---: |
| LOSO mean PLCC | 0.9977 | 0.9982 |
| LOSO PLCC std | 0.0025 | 0.0014 |
| LOSO mean SROCC | 0.9972 | 0.9567 |
| LOSO mean RMSE | 2.172 | 2.194 |
| In-sample PLCC | 0.9983 | 0.9993 |

The low mean SROCC is driven by the BigBuckBunny fold (`0.6277`) while its PLCC
remains high (`0.9962`). That is documented as a ranking-shape caveat, not a
ship blocker: `fr_regressor_v1` has always been gated on PLCC vs the
`vmaf_v0.6.1` teacher.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Refresh `fr_regressor_v1` from the new Netflix table only | Unblocked now; keeps the ADR-0249 C1 recipe stable; immediately proves whether the refreshed extractor path changed the baseline | Does not consume KoNViD/CHUG/HDR rows | Chosen. This is the smallest honest artifact refresh and gives a clean provenance boundary |
| Wait for KoNViD and CHUG, then refresh every model in one PR | One large provenance reset | Blocks an already-ready C1 refresh behind unrelated corpus jobs; creates a very large model-artifact PR | Rejected. The backlog asks to keep learning while long jobs run |
| Change architecture or ship gate during refresh | Could chase the BigBuckBunny SROCC caveat | Mixes a retrain/provenance update with a modeling decision; would invalidate ADR-0249 comparability | Rejected. Architecture and gate changes need their own decision after aggregate corpus refresh |
| Keep the old checkpoint and only update docs | Zero runtime churn | Leaves a known stale shipped artifact after the extractor/default refresh wave | Rejected. The refresh passed the existing gate and improves PLCC variance |

## Consequences

- **Positive**:
  - `fr_regressor_v1` sidecar and registry now reflect the current fork feature
    extractor defaults.
  - PLCC mean and variance improve slightly while preserving the same model
    contract.
  - The first AI refresh step is independent of the long-running KoNViD job.
- **Negative**:
  - BigBuckBunny rank correlation is low in the refreshed run. This is visible
    in the model card and should be revisited after the aggregate 4/5-corpus
    retrain, but it does not violate the ADR-0249 PLCC ship gate.
- **Neutral / follow-ups**:
  - KoNViD, CHUG/HDR, BVI-DVC aggregate, `vmaf_tiny_v2/v3/v4`, codec-aware
    regressors, ensemble seeds, MOS heads, and encoder predictors remain in the
    AI-refresh backlog.

## References

- [ADR-0249](0249-fr-regressor-v1.md) — original `fr_regressor_v1` recipe and
  ship gate.
- [docs/ai/models/fr_regressor_v1.md](../ai/models/fr_regressor_v1.md) —
  updated model card.
- [docs/research/0647-ai-fr-regressor-v1-refresh-20260520.md](../research/0647-ai-fr-regressor-v1-refresh-20260520.md)
  — run digest and fold metrics.
- Source: `req` — user direction this session: "well now we have to train and
  learn on everything as well?"
