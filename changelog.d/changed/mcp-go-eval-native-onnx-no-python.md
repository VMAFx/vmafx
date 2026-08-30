- MCP server (Go `eval_model_on_split` / `compare_models`): replace the
  embedded `python3 -c` helper script with a fully native Go
  implementation. The Go server previously shelled out to Python with an
  inline script that imported `numpy`, `pandas`, `scipy` and
  `onnxruntime`; that subprocess was the last hard Python dependency in
  `cmd/vmafx-mcp` and the remaining blocker for the ADR-0704 Stage 2
  sunset of `mcp-server/vmaf-mcp`. The work now splits across:
  - new `pkg/modeleval` (pure Go, no cgo): parquet feature-cache reader
    (`github.com/parquet-go/parquet-go`), the SHA-256 `vmaf-train-splits-v1`
    train/val/test bucketing, and the PLCC / SROCC / RMSE statistics
    (Spearman uses scipy-compatible average ranks for ties);
  - new `pkg/libvmaf.DNNSession` (cgo): the ONNX forward pass, bound to
    libvmaf's own `vmaf_dnn_session_open` / `_run` / `_close` named-tensor
    API. Reusing libvmaf's ONNX Runtime keeps a third-party ONNX package
    out of the Go module and inherits the model size cap plus the operator
    allowlist that `vmaf_dnn_session_open` applies (ADR-0211).
  A libvmaf built without ONNX Runtime returns `-ENOSYS` from every DNN
  entry point, which surfaces as a clear "built without DNN support;
  rebuild with -Denable_dnn=enabled" error — the Go counterpart of the
  Python server's missing-`[eval]`-extra behaviour, so the tool degrades
  rather than breaking.
- MCP server: `compare_models` now closes every ONNX session it opens.
  The Python `_compare_models` leaked one ORT session per evaluated model
  (`docs/research/0983-python-surfaces-bug-audit-2026-05-31.md`).
- MCP server: `eval_model_on_split` / `compare_models` responses are now
  emitted from typed Go structs whose field order reproduces the Python
  server's dict insertion order (`model`, `features`, `split`, `n`,
  `plcc`, `srocc`, `rmse`, `columns`). The previous map-based result was
  re-serialised in Go's alphabetical key order, so the two servers'
  JSON differed in key sequence.
- MCP server: a degenerate split (constant predictions, zero variance)
  now fails with an explicit `correlation undefined: input has zero
  variance` error. scipy returns `NaN` there and Python emits a bare
  `NaN`, which is not valid JSON; Go's `encoding/json` refuses to
  marshal it, so the previous behaviour was an opaque
  "failed to marshal result".
