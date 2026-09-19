- **Two security fixes that later merges had silently reverted are back, and each now has a gate
  that runs.** Both were recorded as fixed in this changelog while the vulnerable code was on
  `master`.
  - **Vendored cJSON (embedded MCP server, `-Denable_mcp=true`).** The cJSON 1.7.18 → 1.7.19
    re-vendor (PR #883) restored upstream's eleven `sprintf` / `strcpy` calls and the unclamped
    `cJSON_GetArraySize`, undoing ADR-0683 (PR #1536) and ADR-1061 (PR #725). They are replaced
    again with bounded `snprintf` / `memcpy`, and the array size saturates at `INT_MAX`. cJSON's
    `goto`-based and over-long functions are split into helpers so the file meets the same
    standards as the rest of the tree (ADR-1142); on 3,103 inputs, including every
    allocation-failure point, its output, parse-error offsets and allocation counts are
    byte-identical to pristine 1.7.19 under ASan + UBSan. The pdjson depth limit and overflow
    guards from PR #725 were checked and had survived.
  - **Undefined behaviour in cJSON on a NaN score.** `cJSON_CreateNumber` and
    `cJSON_SetNumberHelper` cast the double to `int` after two range checks that NaN passes, which
    is undefined behaviour, and the MCP server hands them a VMAF score that can be NaN. This is
    upstream cJSON behaviour, found by the new test under clang's UBSan. Both now use one
    saturating conversion that maps NaN to 0; the printed JSON is unchanged (`null`).
  - **Shell injection.** PR #414 carried stale copies of two scripts and undid PR #350:
    `scripts/ci/sycl-bench-env.sh` again interpolated the oneAPI prefix (`$ONEAPI_PREFIX` or the
    version argument) into a `bash -c "…"` body, where a prefix such as `x'$(payload)'` runs
    arbitrary code, and the dev container's entrypoint again ran its GPU probe through `eval`.
    The prefix is passed as a positional argument again and the probe runs as argv.
  - **Why nothing noticed, and what changed.** The banned-function Semgrep rule excluded
    `core/src/mcp/3rdparty/` and `.semgrepignore` listed `cJSON.c`; both exclusions are gone,
    the commit-time hook no longer skips `pdjson.{c,h}`, and a planted-defect test
    (`scripts/ci/tests/test_semgrep_vendored_scope.py`) fails if vendored code is ever hidden
    from that rule again. The existing `scripts/ci/test-sycl-bench-env.sh` would have failed on
    the reverted helper, but no workflow or hook ran it; it and the new
    `scripts/ci/tests/test-dev-mcp-entrypoint-probe.sh` are now pre-commit hooks, which the
    required Pre-Commit CI job runs on every pull request. New `core/test/test_cjson.c` runs in
    the `fast` suite of every build, with or without `enable_mcp`.
