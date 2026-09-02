# Research-0647: `fr_regressor_v1` refresh on the 2026-05-20 Netflix table

## Question

Does the refreshed Netflix full-feature table still clear the ADR-0249 C1 ship
gate, and how much do the shipped `fr_regressor_v1` metrics move?

## Inputs

- Feature table:
  `runs/full_features_netflix_refresh_20260520.parquet`
- Table shape: `11190` rows, `30` columns
- Trainer:
  `PYTHONPATH=ai/src .venv/bin/python ai/scripts/train_fr_regressor.py`
- Model recipe: unchanged ADR-0249 `FRRegressor`
  (`hidden=64`, `depth=2`, `dropout=0.1`, canonical-6 input)
- Ship gate: mean LOSO PLCC >= `0.95`

## Result

| Source | PLCC | SROCC | RMSE |
| --- | ---: | ---: | ---: |
| BigBuckBunny | 0.9962 | 0.6277 | 4.466 |
| BirdsInCage | 0.9993 | 0.9997 | 1.854 |
| CrowdRun | 0.9997 | 0.9998 | 1.005 |
| ElFuente1 | 0.9992 | 0.9963 | 1.635 |
| ElFuente2 | 0.9964 | 0.9969 | 3.169 |
| FoxBird | 0.9967 | 0.9954 | 2.352 |
| OldTownCross | 0.9992 | 0.9998 | 1.931 |
| Seeking | 0.9989 | 0.9962 | 1.982 |
| Tennis | 0.9982 | 0.9982 | 1.356 |

Summary:

- Mean PLCC: `0.9982 ± 0.0014`
- Mean SROCC: `0.9567 ± 0.1234`
- Mean RMSE: `2.194 ± 1.049`
- Final all-source in-sample PLCC: `0.9993`
- Exported ONNX sha256:
  `b57dee2509290d77c7980f8f23aa1380f64937c485d1b1d1e5f78c13a3a54c63`

## Interpretation

The refreshed model clears the unchanged PLCC ship gate by a wide margin and
improves PLCC variance relative to the previous sidecar. The low BigBuckBunny
SROCC is a real caveat: rank ordering within that fold is weaker even though
linear agreement remains high. This does not block the C1 checkpoint because
ADR-0249 defines PLCC-vs-teacher as the ship gate, but it should be checked
again when the aggregate 4/5-corpus models are refreshed.

## Reproducer

```bash
PYTHONPATH=ai/src .venv/bin/python ai/scripts/train_fr_regressor.py \
  --parquet runs/full_features_netflix_refresh_20260520.parquet \
  --metrics-out runs/fr_regressor_v1_refresh_20260520_metrics.json

PYTHONPATH=ai/src .venv/bin/python - <<'PY'
import onnx
onnx.checker.check_model(onnx.load("model/tiny/fr_regressor_v1.onnx"))
PY

bash core/test/dnn/test_registry.sh
```
