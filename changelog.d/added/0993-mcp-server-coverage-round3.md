## mcp-server: pytest coverage push — round 3

Added 56 new pytest cases for `mcp-server/vmaf-mcp/src/vmaf_mcp/` targeting
residual coverage gaps in `server.py` and `http_transport.py`.

Coverage delta: server.py 86% → 94% (+8 pp), http_transport.py 84% → 85%,
overall 86% → 92% (+6 pp).

Key areas covered:
- `_probe_backends` timeout/OSError fallback (cpu-only assumption on failure)
- `_probe_backends` cache hit path on repeated calls
- `_infer_backend_from_payload` all branches (vulkan/gpu/cpu/empty-frames)
- `_pick_worst_frames` full contract (ranking, n-limit, missing keys, alternates)
- `_describe_image_with_vlm` all return-path branches (string, list-of-dict,
  list-of-string, TypeError compatibility fallback)
- `_extract_frame_png` success path and 10-bit pixel format mapping
- `_run_benchmark` FileNotFoundError and non-zero-rc-with-empty-output branch
- `_list_extractors` missing feature_dir (returns [] gracefully)
- `_list_extractors` C source parse with synthetic struct definitions
- `_infer_backend_from_sym` all backend suffix branches
- `_describe_model` ambiguous-match and absolute-path-resolution paths
- `_eval_model_on_split` error paths (missing mos column, no feature columns,
  too-few-samples)
- `_vmaf_binary` env override and fallback chain
- `_list_backends` binary-missing fallback
- `_allowed_roots` container /workspace path always included
- tool inputSchema validation (bitdepth enum subset, pixfmt enum, all tool names)
- `_run_compare` / `_run_ladder` / `_run_tune_per_shot` optional keyword branches
- `http_transport._build_metrics` construction contract
- `http_transport.make_score_handler` coroutine binding
- `http_transport._handle_score` 400 on missing fields, 500 on scorer error
