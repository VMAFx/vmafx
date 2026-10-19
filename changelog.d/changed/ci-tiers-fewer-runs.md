- **CI runs the tier a pull request owes, not the whole suite on every push
  ([ADR-2169](docs/adr/2169-ci-fewer-runs.md)).** A pull request from this repository
  (Renovate included) runs lint, format, the fast suite and the governance gates; the
  platform, GPU, coverage, container and FFmpeg lanes run on the master push, on pull
  requests from forks and on an own pull request labelled `ci: full`. The generated
  release pull request runs the release contract only until it carries
  `autorelease: cut`. Draft pull requests start no job. Renovate groups minor and patch
  updates into one weekly pull request and rebases only on conflict; security updates
  still open at any time. See "Which jobs run when" in `docs/development/ci.md`.
