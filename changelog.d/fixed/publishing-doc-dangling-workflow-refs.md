- `docs/development/publishing.md` described a CI pipeline that does not
  exist. Its "CI integration" section named `release.yml` and
  `cross-backend.yml` — neither is a file in `.github/workflows/` — and claimed
  both run inside `dev/Containerfile`, from which it concluded that a passing
  local container build predicts the CI gates. No workflow in the repository
  runs any build in `dev/Containerfile`; the real chain is `release-please.yml`
  → `supply-chain.yml` (native artifacts, built on the `ubuntu-latest` host) →
  `docker-publish-production.yml` (images, built from `docker/Dockerfile.production*`),
  with backend parity being a `cross-backend` job inside
  `tests-and-quality-gates.yml` (disabled with `if: false` pending a
  self-hosted GPU runner). The page now carries a table of the four real
  entries and says which of them containerise. The host-side-builds table also
  claimed the Netflix golden gate does not run on the host "because CI uses the
  container environment"; the `netflix-golden` job builds with host meson/ninja
  on `ubuntu-latest`, and the row now says so and explains that a verification
  job is out of the policy's scope in the first place.
