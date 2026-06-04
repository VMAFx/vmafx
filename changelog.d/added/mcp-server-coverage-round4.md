## test(mcp-server): coverage push round 4 — describe + compare + ladder + edge cases

Added 69 new pytest cases in `mcp-server/vmaf-mcp/tests/test_coverage_round4.py`
targeting residual gaps in `server.py` and `http_transport.py`:

- `_describe_model`: not-found ValueError, direct-path resolution, stem-name
  lookup, onnx/pkl format labels.
- `_run_compare` / `_run_ladder` / `_run_tune_per_shot`: binary-not-found and
  non-zero-exit error paths; non-JSON format passthrough for ladder.
- `_eval_model_on_split`: invalid split, 'all' split, key-column train/val/test
  split, partial-failure path in `_compare_models`.
- `_run_benchmark`: non-zero exit raises RuntimeError (current behavior).
- `_list_extractors`: OSError-on-read skip, duplicate deduplication.
- `_nan_to_none` / `_dumps_strict`: NaN/Inf scalar+dict+list, allow_nan=False.
- `_subprocess_timeout_s`: default / env override / invalid / negative / zero.
- `_communicate_with_timeout`: timeout kills process and raises RuntimeError.
- `_strip_model_ext`, `_model_resolution_class`: branch coverage.
- `_describe_image_with_vlm`: loaded=True pipeline=None path.
- `_pick_worst_frames`: n > available returns all.
- `_call_tool`: unknown tool, eval_model_on_split dispatch, compare_models dispatch.
- `http_transport`: /healthz, /readyz (503), /metrics, /v1/score success + 400 +
  500; auth middleware (401 no-token, 401 wrong-token, 200 correct-token, 413
  oversized body); `_resolve_bind_host`, `_resolve_auth_token`, `_no_auth_mode`,
  `_build_ssl_context`, `_log_with_rid`.
