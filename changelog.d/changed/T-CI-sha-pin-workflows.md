- **SHA-pin every GitHub Actions reference in `.github/workflows/*.yml`
  (OSSF Scorecard `Pinned-Dependencies` remediation).** Every
  `uses: <owner>/<repo>@<tag>` reference in the 13 fork workflows is
  now resolved to a 40-char commit SHA with the original semver
  preserved as a trailing `# vN.M.K` comment, mirroring the pattern
  already established for `ossf/scorecard-action`,
  `sigstore/cosign-installer`, `softprops/action-gh-release`, and
  `anchore/sbom-action`. 97 first-party and third-party action
  references converted across `docker-image.yml`, `docs.yml`,
  `ffmpeg-integration.yml`, `libvmaf-build-matrix.yml`,
  `lint-and-format.yml`, `nightly-bisect.yml`, `nightly.yml`,
  `release-please.yml`, `rule-enforcement.yml`, `scorecard.yml`,
  `security-scans.yml`, `supply-chain.yml`, and
  `tests-and-quality-gates.yml`. **Single documented holdout**: the
  `slsa-framework/slsa-github-generator/.github/workflows/generator_generic_slsa3.yml@v2.0.0`
  reusable-workflow ref in `supply-chain.yml` keeps its `vX.Y.Z` form
  per the SLSA generator maintainers' published guidance — GitHub
  Actions consumers cannot currently SHA-pin reusable-workflow refs in
  every code path, and the existing inline comment in
  `supply-chain.yml` already calls this out. **Why this matters**: the
  `vN` floating tag is an attacker-rotatable handle (a compromised
  upstream maintainer or a tag-overwrite supply-chain incident silently
  swaps the executed code under us); SHA pinning fixes the executed
  bytes and lets Dependabot surface bumps as reviewable diffs rather
  than as silent rotations. The change is a pure ref substitution — no
  action versions are bumped — so workflow behaviour is unchanged. See
  [ADR-0263](docs/adr/0263-ossf-scorecard-policy.md) (created by
  PR #337) and the OSSF Scorecard
  [Pinned-Dependencies check documentation](https://github.com/ossf/scorecard/blob/main/docs/checks.md#pinned-dependencies).
