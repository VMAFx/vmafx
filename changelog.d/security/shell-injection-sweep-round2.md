- **Shell-injection sweep round 2** — hardens two remaining shell-execution
  sites that survived PR #313 (compat sweep), PR #318 (scripts hygiene), and
  PR #326 (ai/ tempfile + path safety):
  - `scripts/ci/sycl-bench-env.sh` — the helper's `bash -c "... source
    '$ROOT/setvars.sh' ..."` interpolated `$ROOT` (derived from the
    `$ONEAPI_PREFIX` environment variable or the `$VERSION` CLI argument)
    directly into the subshell's command string. A path whose name closes
    the single-quote literal early and OR-chains a fallback could execute
    arbitrary commands inside the helper subshell (the script's `set -e`
    blocked sequential `; …` chains but not `|| …` fallbacks). The fix
    forwards `$ROOT` as a positional argument (`$1`) to the subshell, so
    its contents are never re-parsed by the shell. Verified by the new
    `scripts/ci/test-sycl-bench-env.sh` regression suite.
  - `dev/scripts/dev-mcp-entrypoint.sh` — the GPU-backend visibility probe
    (`_probe_with_retry`) ran command strings through `eval "${cmd}"`. The
    only present callers pass hardcoded names (`sycl-ls`, `rocminfo`),
    but the eval form is a foot-gun for future callers that might pass
    operator-supplied programs. Replaced with direct argv invocation
    (`"${prog}" 2>&1 | grep -qE …`); future probes that need flags must
    add explicit handling rather than re-introduce `eval`.
- **New regression test: `scripts/ci/test-sycl-bench-env.sh`** — exercises
  the hardened oneAPI environment helper with four cases: clean prefix
  (expect export lines), hostile prefix with `' || touch <marker>; false #`
  payload (expect no marker file), hostile prefix with `$(touch <marker>)`
  payload inside single-quotes (expect no marker file — inert in both old
  and new forms), and missing version arg (expect non-zero exit). Confirmed
  the test fails on the pre-fix script and passes on the hardened version.
