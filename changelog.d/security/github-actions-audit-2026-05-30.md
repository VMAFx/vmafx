- **GitHub Actions hardening audit (ADR-0875)** — periodic audit of the
  fork's 24 workflows. Findings: every `uses:` was already pinned to a
  40-character commit SHA. Two workflows were missing a top-level
  `permissions:` block (`go-ci.yml`, `rust-ci.yml`) — both backfilled
  with `contents: read`. Five `actions/checkout` steps that do not
  push back to git were missing `persist-credentials: false`
  (`sanitizers.yml` x2, `supply-chain.yml` x3 — the `build-artifacts`,
  `sbom`, and `mcp-build` jobs do not need a write token because the
  downstream `sign` / `slsa-provenance` / `mcp-sign` /
  `mcp-publish-pypi` / `attach-to-release` jobs do their own
  checkouts). `lint-and-format.yml` and `libvmaf-build-matrix.yml`
  were intentionally skipped — they are in-flight under PR #342 and
  PR #325 and will be re-audited after those merge.
