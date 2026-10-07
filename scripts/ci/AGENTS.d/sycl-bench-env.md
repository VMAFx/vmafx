---
paths:
  - scripts/ci/sycl-bench-env.sh
  - scripts/ci/test-sycl-bench-env.sh
invariant: `$ROOT` reaches `bash -c` only as positional argument of single-quoted body; test hook stays wired.
---
<!-- markdownlint-disable MD013 MD060 -->
# `sycl-bench-env.sh`: the prefix stays out of the `bash -c` body

## Workflow coupling

| Script | Workflow lane(s) that invoke it | What couples them |
| --- | --- | --- |
| `sycl-bench-env.sh` | (sourced via `eval "$(scripts/ci/sycl-bench-env.sh <version>)"` by any caller that needs side-by-side oneAPI activation; no workflow invokes it today) | `$ROOT` (from `$ONEAPI_PREFIX` env or version argument, both externally controlled) must stay out of any `bash -c "..."` body. It reaches helper subshell as positional argument: `bash -c '... source "$1/setvars.sh" ...' _ "$ROOT"`, where body is single-quoted literal. Interpolated, prefix that closes quote (`x' \|\| <payload>; false #`, or `x'$(<payload>)'`) runs arbitrary code; `set -e` blocks neither. **This fix (PR #350) was reverted once by stale squash-merge (PR #414) and re-applied on 2026-09-19** — when resolving conflict here, never take double-quoted form. `test-sycl-bench-env.sh` is gate. |
| `test-sycl-bench-env.sh` | `.pre-commit-config.yaml` — `test-sycl-bench-env` hook (pre-commit + pre-push); required `Pre-Commit` CI job runs it with `--all-files` | Side-channel oracle: marker file under `mktemp -d` that hostile `$ONEAPI_PREFIX` would create, plus check that hostile prefix is still sourced as literal path. It existed when fix was reverted and would have failed (4 of 7 cases fail on vulnerable form), but nothing ran it. Do not unwire hook; regression test that no gate executes protects nothing. |
