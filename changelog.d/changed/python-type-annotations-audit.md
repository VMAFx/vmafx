- Tighten Python type annotations across `ai/src/aiutils/`,
  `ai/src/corpus/`, `mcp-server/vmaf-mcp/`, and `tools/vmaf-tune/src/vmaftune/`
  after a mypy `--strict` audit. Surfaced and fixed three real bugs:
  (1) `_run_benchmark()` in `mcp-server/vmaf-mcp/src/vmaf_mcp/server.py`
  was defined twice (the older copy was silently shadowed at import time
  by the progress-token-aware version 575 lines later — any future edit
  to the dead copy would have been invisible at runtime); (2) `seen:
  set[str]` in `list_extractors` actually held `tuple[str, str]`
  containment-check pairs; (3) a `for r in data.ladder_rungs` loop in
  `tools/vmaf-tune/src/vmaftune/report.py` reused the loop variable
  `r` that was previously bound to a different dataclass type
  (`CodecRow`), confusing both readers and type checkers. Touched 15
  files in total; per-package mypy `--strict` deltas:
  `ai/src` 7 → 0, `mcp-server/vmaf-mcp/src` 16 → 0, `tools/vmaf-tune/src`
  261 → 196. No behaviour changes — all targeted package tests pass.
