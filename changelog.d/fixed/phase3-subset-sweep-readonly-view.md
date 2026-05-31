- `ai/scripts/phase3_subset_sweep.py` — fix
  `ValueError: output array is read-only` raised by
  `_standardize_inplace` when `--standardize` runs against a parquet
  feature table whose blocks pandas materialises as read-only views.
  `_loso_sweep` now forces a writeable copy via
  `to_numpy(copy=True)`; `_standardize_inplace` additionally guards
  the contract by raising a clear `ValueError` if either fold-array
  is read-only (rather than no-op'ing on a copy and silently
  dropping the standardisation step). Flagged by PR #458's
  unit-test coverage expansion. Three regression tests added to
  `ai/tests/test_phase3_subset_sweep_unit.py`.
