- **Four large CI workflows fit the praetor engine's 64 KiB input bound
  again.** `docker-publish-operator-node.yml`, `libvmaf-build-matrix.yml`,
  `lint-and-format.yml` and `tests-and-quality-gates.yml` had grown past
  64 KiB, so the engine's `rust-msrv` audit stopped reading the tree. Their
  long comments now live in
  [`docs/development/ci-workflow-notes/`](docs/development/ci-workflow-notes/tests-and-quality-gates.md).
  Repeated run blocks moved into tested scripts: the published-image smoke
  tests into `scripts/release/smoke-published-image.sh`, the four index
  merges into the `merge-platform-index` composite action, and the cached
  apt installs of the test workflow into `scripts/ci/apt-install-cached.sh`.
  The new `check-workflow-size` pre-commit hook fails any workflow above
  64 KiB.
