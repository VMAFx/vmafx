- New test module `mcp-server/vmaf-mcp/tests/test_iserror_invariant.py`
  (44 tests) pins the MCP `isError=True` invariant from the 2026-05-19
  audit memory (`project_mcp_iserror_must_be_true` /
  [ADR-0608](docs/adr/0608-mcp-p0-fixes.md) §E-1): every tool in the
  dispatcher MUST raise on malformed or unknown input so the mcp library
  sets `isError=True` on the `CallToolResult`. Walks all nine
  argument-taking tools (`vmaf_score`, `describe_worst_frames`,
  `vmaf_score_encoded`, `eval_model_on_split`, `compare_models`,
  `describe_model`, `run_compare`, `run_ladder`, `run_tune_per_shot`)
  via parametrize and asserts the failure mode is "raise", not "return
  error JSON with isError implicitly False".
- Additional coverage for previously-untested error paths:
  `_validate_path` (allowlist + file-existence), `_run_vmaf_score`
  (binary-missing + backend-not-advertised), `_decode_to_yuv` and
  `_extract_frame_png` (ffmpeg-missing + pixfmt rejection +
  ffmpeg-nonzero-exit), `_ffprobe_geometry` (ffprobe-missing +
  no-video-stream + nonzero-exit), `_eval_model_on_split`
  (split-validation), `_compare_models` (per-model error accumulation
  documented as intentional batch semantics), `main()` /
  `_run` (entry-point dispatch for both `asyncio` and `anyio` backends),
  plus the HTTP transport env-var helpers `_resolve_port`,
  `_resolve_log_level`, and `_apply_env_overrides`. Coverage of
  `mcp-server/vmaf-mcp/src/vmaf_mcp/` rises from 61 % to 78 %
  (server.py 77 %→82 %; http_transport.py 0 %→64 %).
- Fixed: `_handle_metrics` (HTTP `/metrics` endpoint) no longer passes
  `prometheus_client.CONTENT_TYPE_LATEST` to the `content_type=` kwarg
  of `aiohttp.web.Response`. Modern aiohttp (≥3.13) rejects any
  `content_type=` value that contains `charset=` —
  `prometheus_client.CONTENT_TYPE_LATEST` is
  `text/plain; version=0.0.4; charset=utf-8` — and the endpoint was
  returning HTTP 500 instead of the Prometheus scrape body. The fix
  sets the full media-type via the explicit `Content-Type` header.
  Caught by the newly-enabled HTTP-transport test suite (previously
  silently skipped because `prometheus_client` was not pulled in as a
  test dependency).
- Fixed: `test_describe_model_onnx_no_metadata` in
  `mcp-server/vmaf-mcp/tests/test_p1_tools.py` now resolves the repo
  root via `srv._repo_root()` instead of the test-file-relative `REPO`
  constant. The test failed under `git worktree add /tmp/...` because
  the installed package resolved the canonical workspace while the
  fixture wrote to the worktree path; the two roots disagreed and the
  fake ONNX file was never found by the lookup.
