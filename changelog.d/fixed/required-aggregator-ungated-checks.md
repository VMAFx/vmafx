- **Forty-four CI checks reported a verdict on every pull request and blocked
  nothing; nine of them were red.** Branch protection requires a single status
  context, `Required Checks Aggregator`, so the `const required = [...]` array in
  `.github/workflows/required-aggregator.yml` is the merge gate in full. It
  carried 45 of the 89 names that reported on PR #1518. Ungated and failing at
  the time: `Coverage Gate`, `Ubuntu gcc`, `Ubuntu clang`, `Ubuntu ARM clang`,
  `Ubuntu gcc static`, `macOS clang`, `macOS clang+DNN`, `macOS Metal` and
  `macOS Clang+Metal`. No macOS leg was gated at all, the plain Ubuntu gcc and
  clang builds were not gated although their `+DNN` siblings were, and
  `Coverage Gate` was not gated although `Coverage GPU` was.
  `scripts/ci/check-aggregator-names.sh` reported OK throughout and was right to:
  the array and the `# required-aggregator` markers agreed with each other. The
  gap was the declared state, not drift.

  The array now carries 78 names — the platform and backend build legs, the
  Windows ARM64 NEON lane, the always-on test and lint gates, the FFmpeg, Docker,
  dev-container and Rust consumer contracts, both documentation builds, and the
  `Semgrep OSS` and `gitleaks` code-scanning verdicts. `Coverage Gate`,
  `MCP Smoke`, `Markdown Lint`, `No Conflict Markers`,
  `Tiny-Model Registry Validate` and `Windows ARM64 MSVC` additionally join
  `strictMustReport`, so for those six a check that never appears is a failure
  rather than an assumed path skip. Checks that cannot report on an ordinary pull
  request — schedule-only, master-push-only, label-gated, routing steps, and one
  job with no failing path — stay out, each with its reason recorded in ADR-1297.

  Three check names changed so the gate is coherent: `Tidy SYCL (advisory)` is now
  `Tidy SYCL` and has lost its `continue-on-error` (ADR-0217's condition — one
  green master run — has been met six times over), the docs site build reports as
  `Docs Site Build` instead of the bare job id `build`, and the doxygen job
  reports as `Doxygen Public API` instead of the bare job id `doxygen`. The
  `experimental: true` flag is gone from the two macOS matrix rows that carried
  it: `continue-on-error` never neutralised the check-run conclusion, so the flag
  only made the workflow claim those legs were advisory while nothing acted on
  the claim. ADR-1297.
