# Bisect-model-quality fixture cache

Deterministic cache used by [`.github/workflows/nightly-bisect.yml`](../../../.github/workflows/nightly-bisect.yml).
Regenerable from [`ai/scripts/build_bisect_cache.py`](../../scripts/build_bisect_cache.py).

## Layout

```text
features.parquet      256 rows × 6 default features + mos column
models/model_NN.onnx  8 linear FR models (input "input", output "score")
```

Total committed size: ~16 KB.

## Why these bytes are committed

CI re-runs `build_bisect_cache.py --check` before exercising the bisect.
The check regenerates everything from fixed seeds and asserts content
equality against the committed tree:

- `features.parquet` is compared via `pyarrow.Table.equals` (schema +
  row count + values). The parquet header's `created_by` writer-version
  string is intentionally ignored — see
  [ADR-0262](../../../docs/adr/0262-bisect-cache-logical-comparison.md).
- `models/model_NN.onnx` is compared byte-for-byte. ONNX serialisation
  is deterministic here because `producer_name`, `producer_version`, and
  `ir_version` are pinned in `_save_linear_fr`.

This catches silent drift in row values, schema, model weights, opset,
or graph topology between Python / pyarrow / onnx versions and catches
accidental edits, while tolerating cosmetic writer-version churn.

## Why the timeline is regression-free

All 8 committed models are tiny perturbations of the optimal weights
(`weights = ones / N_FEATURES + N(0, 1e-4)`), so `bisect-model-quality
--min-plcc 0.85` returns `first_bad_index = None`. A red nightly means
the wiring broke (parquet drift, onnx drift, runtime crash), not that
a real quality model regressed.

The synthetic-regression case — "introducing a bad ONNX trips the
alert" — is covered by
[`ai/tests/test_bisect_model_quality.py::test_bisect_localises_first_bad`](../../tests/test_bisect_model_quality.py),
which builds a `bad_from=5` timeline at runtime.

## Synthetic default and real-feature mode

The committed cache remains synthetic by default so every checkout can
regenerate it without private or manually-provisioned datasets. When a
DMOS/MOS-aligned feature table exists locally, the same generator can
materialise the bisect layout from that real table:

```bash
python ai/scripts/build_bisect_cache.py \
  --source-features runs/dmos_features.parquet \
  --target-column dmos
```

The source parquet must contain `adm2`, `vif_scale0`, `vif_scale1`,
`vif_scale2`, `vif_scale3`, and `motion2`, plus a target column. If
`--target-column` is omitted, the generator tries `mos`, `dmos`,
`target`, then `score`. The output parquet always normalises the target
column to `mos` because that is the `bisect-model-quality` CLI contract.

Real-feature mode also fits the tiny linear ONNX timeline from the
provided target, then applies deterministic tiny perturbations so the
nightly verdict remains "no regression in range". This is the
Research-0001 swap path without requiring the real dataset bytes in the
repository.

## Regeneration

```bash
python ai/scripts/build_bisect_cache.py            # rewrite in place
python ai/scripts/build_bisect_cache.py --check    # CI-style drift check
```

After regenerating, commit the resulting parquet + ONNX bytes.
