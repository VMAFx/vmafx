- **MCP `run_benchmark` tool** now returns a complete benchmark JSON instead of an
  error response. Three root causes were fixed: (1) spurious positional arguments
  (`-r`, `-d`, `--width`, `--height`) passed to `bench_all.sh` corrupted `$@` inside
  the sourced Intel oneAPI `setvars.sh`, causing a silent `set -euo pipefail` abort
  before any output fired; (2) `VMAF_BIN` was not injected into the subprocess
  environment, forcing a fallback to the absent in-tree binary path; (3) `set -u`
  (nounset) aborted the shell when `setvars.sh` referenced unset variables, bypassing
  the `|| true` guard. The tool schema no longer requires `ref`/`dis`/`width`/`height`
  (per-pair scoring uses the separate `vmaf_score` tool). (ADR-0513)
