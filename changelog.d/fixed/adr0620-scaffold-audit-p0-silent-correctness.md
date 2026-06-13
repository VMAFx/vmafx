- **Python harness: three P0 silent-correctness fixes (ADR-0620)**
  - `routine.py:604` — replaced bare `except Exception: print/fallback` with
    `raise CalibrationError(...) from exc`; new `allow_uncalibrated=False`
    parameter on `run_test_on_dataset` preserves existing behaviour for callers
    that genuinely need the uncalibrated-normalisation fallback.
  - `train_test_model.py:354` — replaced silent `np.zeros` substitution for
    missing `ys_label_stddev` with `raise MissingLabelStddevError`; callers
    can pass `assume_unit_stddev=True` to `plot_scatter` to opt in to unit bars.
  - `local_explainer.py:121` — replaced silent `model[0]` pick from an ensemble
    list with `raise EnsembleNotSupportedError` for `len(model) > 1`; single-
    element lists continue to unwrap transparently.
  - 16 regression tests added in `python/test/test_adr0620_scaffold_audit_p0.py`.
  - Closes T-PYTHON-ROUTINE-SWALLOWED-EXCEPTION, T-PYTHON-TRAIN-TEST-STD-ZERO,
    T-PYTHON-LOCAL-EXPLAINER-HACKY.
