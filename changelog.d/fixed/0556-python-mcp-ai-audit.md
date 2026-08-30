### Python / MCP / AI silent-fallback audit (ADR-0556)

- **score.py**: `json.load` now wrapped in `JSONDecodeError` guard — a vmaf process killed
  mid-write no longer crashes the entire corpus run; the affected row receives a NaN score
  and a non-zero exit status instead.
- **bvi_dvc_to_full_features.py**: dir mode and zip mode now exit 2 with an actionable error
  message when no clips match the BVI-DVC naming convention for the given tier; previously the
  script silently wrote a zero-row parquet and returned 0.
- **auto.py**: `_load_calibrated_recipes` F.4 placeholder fallback promoted from `DEBUG` to
  `WARNING` so operators see the message at the default logging level.
- **server.py** (MCP): `list_backends` tool description now correctly lists all six backends
  (`cpu / cuda / sycl / vulkan / hip / metal`); previously omitted `vulkan` and `metal`.
- **validate_model_registry.py**: post-validation count-read failure now propagates as rc=1
  with an `ERROR:` message instead of printing the misleading `"OK: 0 entries valid"`.
