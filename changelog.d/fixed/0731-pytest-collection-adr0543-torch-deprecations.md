- Fix `ModuleNotFoundError: No module named 'vmaf_mcp'` collection crash in
  `mcp-server/vmaf-mcp/tests/` by adding `pythonpath = ["src"]` to the pytest
  config (Issue 1 from PR #1553 audit).
- Fix ADR-0543 backend-enforcement test incorrectly picking up the pre-ADR-0543
  system `vmaf` binary instead of the in-tree build; the resolver now skips any
  binary that does not advertise `--backend` in its help text (Issue 2).
- Suppress PyTorch 2.9+ `DeprecationWarning` for the legacy TorchScript ONNX
  exporter in `ai/` tests; the fork intentionally pins `dynamo=False` per
  ADR-0207 and the warnings originate inside `torch/onnx/__init__.py` (Issue 3).
