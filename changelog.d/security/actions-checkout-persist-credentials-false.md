- **Workflows opt out of GITHUB_TOKEN persistence on checkout (S10)** —
  every `actions/checkout` step in workflows that do not push to the
  repository, open issues/PRs, or upload release artefacts now sets
  `persist-credentials: false`. The default behaviour writes
  `GITHUB_TOKEN` into the runner's local `.git/config`, leaving the
  credential visible to any later step that reads it. Touched workflows:
  `build.yml`, `docs.yml`, `docker-image.yml`,
  `docker-publish-production.yml`, `ffmpeg-integration.yml`,
  `go-ci.yml`, `libvmaf-build-matrix.yml`, `lint-and-format.yml`,
  `nightly-bisect.yml`, `nightly.yml`, `rule-enforcement.yml`,
  `rust-ci.yml`, `security-scans.yml`, `tests-and-quality-gates.yml`.
  Whitelisted workflows that legitimately need the credential
  (`release-please.yml`, `supply-chain.yml`, `upstream-watcher.yml`)
  are unchanged.
