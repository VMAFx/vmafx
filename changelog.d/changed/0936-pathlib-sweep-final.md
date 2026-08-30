- Replace the final `os.path.{dirname,abspath,join}` usages in the
  `tools/vmaf-roi-score` and `tools/vmaf-tune` console shims with
  `pathlib.Path` (ADR-0936). Fold in 10 additional `PTH` violations
  surfaced across `ai/scripts/`, `ai/src/corpus/`, `mcp-server/`,
  `scripts/ci/`, and `tools/vmaf-tune/src/` (`os.replace` →
  `Path.replace`, `os.path.getsize` → `Path.stat().st_size`,
  `os.path.expanduser` → `Path.home()`, `os.path.splitext` →
  `Path.suffix`, `os.symlink` → `Path.symlink_to`, builtin `open()` →
  `Path.open()`).
- Add `PTH` (flake8-use-pathlib) to `[tool.ruff.lint] select` to
  prevent regression; per-file-ignore the rule for the
  upstream-mirror trees (`python/**`, `compat/python-vmaf/**`,
  `testdata/**`) consistent with the existing style-family
  suppressions (ADR-0100).
