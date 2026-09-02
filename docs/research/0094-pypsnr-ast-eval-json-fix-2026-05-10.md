<!-- markdownlint-disable MD013 MD060 -->
# Research-0094 — T-PYPSNR-AST-EVAL: `str`/`ast.literal_eval` log serialization incompatible with numpy 2.x

**Date**: 2026-05-10
**Branch**: fix/pypsnr-ast-eval
**Status**: Closed — fix merged

## Summary

`PyFeatureExtractorMixin._get_feature_scores` and `NoReferenceFeatExtractor._get_feature_scores`
wrote per-frame score logs via Python's `str()` built-in and read them back with `ast.literal_eval`.
Under numpy 1.x, `str(np.float64(34.79))` produced `'34.79'` — a plain numeric literal that
`ast.literal_eval` accepted. Under numpy 2.x + Python 3.14, the same call produces
`'np.float64(34.79)'` — a call expression that `ast.literal_eval` correctly rejects for
security reasons. All eight `test_run_pypsnr_*` test cases raised:

```text
ValueError: malformed node or string on line 1:
  Call(func=Attribute(value=Name(id='np', ...), attr='float64', ...), ...)
```

## Root cause

`numpy` changed the `__repr__` / `__str__` output of scalar types between 1.x and 2.x.
In numpy 2.0, `np.float64.__str__` now delegates to `np.float64.__repr__`, which includes
the type name to aid debugging. This is intentional and documented in the numpy 2.0 migration
guide. `ast.literal_eval` was never a correct parser for arbitrary Python repr output — it only
handles a strict subset of literal expressions (strings, bytes, numbers, tuples, lists, dicts,
sets, booleans, and None). The bug was latent since numpy scalars were first written to log files
this way; it only became visible on Python 3.14 + numpy 2.x.

## Decision: `json.dump` / `json.load`

JSON is the canonical structured serialization format for this use case:

- `json.dump` writes a standards-compliant text representation.
- `json.load` parses it without evaluating arbitrary Python expressions.
- No custom encoder is needed: the PSNR path's numpy scalars (`psnr_y/u/v`) are the result of
  `min(10 * np.log10(...), max_db)`. Python's built-in `min` returns a plain Python `float`
  when one of its arguments is a plain `float` (the `max_db` constant). The numpy scalar is
  coerced. Verified: `type(min(np.float64(34.79), 60.0))` returns `<class 'float'>`.
- For `NoReferenceFeatExtractor`, the `ref_scores_mtx` / `dis_scores_mtx` values enter the
  dict via `.tolist()`, which produces native Python lists of Python floats — fully JSON-serializable.

## Alternatives considered

| Option | Rejected because |
|---|---|
| `repr()` + `ast.literal_eval` | The existing broken approach. Not safe under numpy 2.x. |
| Custom `json.JSONEncoder` subclass handling `np.floating` / `np.integer` | Unnecessary: all values already serialize as plain Python types by the time they enter the log dict (see above). Adding an encoder adds complexity with no benefit. |
| `numpy.save` / `numpy.load` (binary npy format) | Overkill for a simple per-frame dict list; not human-readable; adds a binary format dependency to a text log pipeline. |
| Pin numpy to 1.x | Rejected by the constraint document: Python 3.14 + numpy 2.x is the supported environment. |
| Forward-compat shim (detect old format, migrate) | Not needed: these logs are transient per-run scratch files under `workdir/`; they are never shared across runs, so old-format files are never present when the new code runs. |

## Files changed

- `python/vmaf/core/feature_extractor.py`:
  - Line 9: `import ast` → `import json`
  - Lines 719-721 (`PyFeatureExtractorMixin._get_feature_scores`): `log_str = log_file.read(); log_dicts = ast.literal_eval(log_str)` → `log_dicts = json.load(log_file)`
  - Line 823 (`PyPsnrFeatureExtractor._generate_result`): `log_file.write(str(log_dicts))` → `json.dump(log_dicts, log_file)`
  - Line 968 (`NoReferenceFeatExtractor._generate_result`): `log_file.write(str(log_dict))` → `json.dump(log_dict, log_file)`
  - Lines 977-979 (`NoReferenceFeatExtractor._get_feature_scores`): `log_str = log_file.read(); log_dict = ast.literal_eval(log_str)` → `log_dict = json.load(log_file)`

## Test result

`PYTHONPATH=$PWD/python python3 -m pytest python/test/feature_extractor_test.py -k pypsnr` — 8/8 passed on Python 3.14 + numpy 2.x. All `assertAlmostEqual(places=4)` values are unchanged (the fix is serialization-only; no numeric computation was modified).
