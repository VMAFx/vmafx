- MCP server now reports `isError=True` when the benchmark harness
  (`run_benchmark` → `bench_all.sh`) exits non-zero. Previously
  `_run_benchmark` returned a success-shaped payload regardless of
  exit code, so clients could not branch on failure (violates
  ADR-0608 E-1; sibling tools `_run_compare`, `_run_ladder`, and
  `_run_tune_per_shot` already raised). The error path now mirrors
  those siblings: raises `RuntimeError("benchmark failed (rc=...):
  ...")` so the MCP layer marks the call as `isError=True`.
- All eight async `proc.communicate()` sites in the MCP server now
  funnel through a new `_communicate_with_timeout()` helper with a
  configurable wall-clock deadline (default 600 s, override with
  `VMAF_MCP_SUBPROCESS_TIMEOUT_S`). Matches the per-call timeouts
  already on the synchronous `subprocess.run` sites so a hung child
  (e.g. ffmpeg stuck on a broken stream, vmaf-tune ladder spinning
  on a deadlocked encoder) can no longer wedge an MCP tool
  indefinitely.
- Five silent exception swallows (`except Exception: pass` /
  `continue`) now log via the module-level `logging.Logger` with
  `exc_info=True`: `_probe_backends`, `_compare_models`,
  `_load_vlm`, `_send_progress`, `_probe_backend`. Behaviour is
  unchanged (the swallow is still load-bearing — best-effort
  fallbacks and partial-result accumulation depend on it), but
  operators now see *why* a probe / VLM load / progress
  notification dropped.
- `_run_vmaf_score` no longer writes its JSON output to a
  predictable path (`/tmp/vmaf-mcp-{pid}-{taskname}.json`); it now
  uses `tempfile.NamedTemporaryFile(suffix='.json', delete=False)`
  and unlinks in a `try/finally`. Removes the symlink-attack /
  pre-create race window on multi-tenant hosts.
