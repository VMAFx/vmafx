- **Copyright headers drop vendor tool notices and the provenance gate enforces ADR-0861.**
  Four tracked shell scripts (`scripts/ci/setup-envtest.sh`,
  `scripts/dev/test-cleanup-agent-state.sh`, `scripts/release/verify-release-version.sh`,
  `scripts/release/tests/test-verify-release-version.sh`) retained residual dual-notice
  lines missed by the ADR-0861 sweep. Those lines are removed while preserving Lusoris
  copyright and SPDX licence identifiers, and `scripts/dev/relicense_fork_files.py --check`
  now fails if a header copyright notice names a prohibited vendor or tool.
