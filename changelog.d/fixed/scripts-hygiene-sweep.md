- **`scripts/release/concat-changelog-fragments.sh` and
  `scripts/perf/bench-multi-resolution.sh` hygiene sweep
  (5 findings).** The release fragment-concatenator gained a
  `trap EXIT` cleanup for its two `mktemp` scratch files (previously
  leaked on SIGINT / set -e abort) plus a sanity check that refuses
  to overwrite `CHANGELOG.md` when the rendered output is missing
  the `## [Unreleased]` header — the awk splice keys on that
  header, and its absence silently produced a no-op `mv` with no
  caller signal. The multi-resolution bench was reworked to honour
  `$TMPDIR` (replacing hardcoded `/tmp/` `mktemp` templates with a
  single trap-cleaned `SCRATCH_DIR` and per-call `LOG_DIR`),
  resolve the oneAPI install root via `${ONEAPI_ROOT:-/opt/intel/oneapi}`
  plus a newest-first sort of `/opt/intel/oneapi-*/setvars.sh`
  (replacing the hardcoded `oneapi-2025.3` path that stealth-deprecated
  on every toolkit upgrade), capture `vmaf` stderr to a per-call err
  file and surface it as a `vmaf_error` field on cells that exit
  non-zero (previously: `2>/dev/null` paired with a catch-all that
  emitted `null` scores indistinguishable from real OOM / missing-model
  / GPU-init failures), and capture `ncu` stderr to surface a
  `ncu_error` field instead of `|| true`-discarding it (driver-counter
  permission errors and similar setup failures were otherwise reported
  as a silent empty `ncu` field). Both scripts pass `bash -n` and
  shellcheck `stable` clean.
