- **chore**: Per-instance design review for the ~16 Python CodeQL alerts
  that PR #538's mechanical sweep deferred. Narrowed bare `except:` /
  `except BaseException:` blocks to the actual exception classes raised
  on each path (`OSError`, `TypeError`, `ZeroDivisionError`,
  `AttributeError`, `NotImplementedError`, `ValueError`, `ImportError`,
  `RuntimeError`) — `KeyboardInterrupt` and `SystemExit` now propagate
  correctly. Replaced unnecessary `lambda x: f(x)` wrappers with the
  bare callable (`float`, `len`, `np.nanmean`); the closure-capturing
  `np.nanstd(..., ddof=deg_of_freedom)` lambda was preserved. Resolved
  the static MRO conflict on `_assert_an_asset` in three Noref feature
  extractors via explicit class-level binding; runtime MRO behaviour is
  unchanged. Fixed mixed-return paths in `Executor._slugify` (raises
  `AssertionError` on the unreachable branch) and
  `run_result_assembly.main` (explicit `return 0`). Branched the
  `RegressorMixin` / `ClassifierMixin` `get_stats` calls in `routine.py`
  so the classifier path no longer relies on a `TypeError` fallback.
  Removed unused locals (`ys_vec`, `vqm_rmse`) and dead `else: return
  None` branches in `_strred` / `_speed`. No semantic changes — every
  `except` narrowing was justified against the actual call sites.
