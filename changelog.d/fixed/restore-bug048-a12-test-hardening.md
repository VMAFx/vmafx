<!-- markdownlint-disable MD041 MD013 -->
- **Test hardening and pytest pythonpath restored (BUG-048 Sec A12)**:
  Restores test infrastructure originally landed in commit `993c0ef81` (#1559)
  and silently reverted in `384d97d03`:
  - `mcp-server/vmaf-mcp/pyproject.toml`: restores `pythonpath = ["src"]` under
    `[tool.pytest.ini_options]` so `vmaf_mcp` is discoverable during test collection
    without requiring an editable pip install, eliminating `ModuleNotFoundError`.
  - `tools/vmaf-tune/tests/test_adr_0543_backend_enforcement.py`: restores
    `_binary_supports_backend_flag()` helper and wires it into `_resolve_vmaf_binary()`
    so pre-fork system binaries (e.g. `/usr/local/bin/vmaf`) that lack `--backend`
    are skipped rather than causing test failures (exit 255 != 100); updates source
    path checks to resolve `core/tools/vmaf.cpp`. Adds unit tests for `--backend`
    probing and binary resolver behavior.
  - PyTorch 2.10 deprecation filters: documented as MOOT. The warning was resolved
    at the root cause by PR #1518 (`T-TINYAI-TORCH-214-WARNINGS-2026-09-22`) by
    migrating `torch.onnx.export` callers to `dynamic_shapes = ({0: "batch"},)` under
    the tree-wide `filterwarnings = ["error"]` policy, making blanket suppression
    filters obsolete and contraindicated.
