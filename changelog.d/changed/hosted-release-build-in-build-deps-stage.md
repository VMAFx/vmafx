- Native release artifacts are built on a GitHub-hosted runner inside the
  `build-deps` container stage ([ADR-1346](../docs/adr/1346-hosted-slim-container-release-build.md),
  supersedes ADR-1178). `build-artifacts` in `supply-chain.yml` no longer waits
  for a self-hosted runner that is not registered. It builds the release tag's
  own `build-deps` stage (digest-pinned Ubuntu 26.04 plus Ubuntu archive
  packages, no third-party downloads) with
  `scripts/ci/build-dev-container-stage.sh build-deps`, with no external layer
  cache and no registry, then compiles, stages, stamps and verifies the bundle
  inside it with `docker run --pull never --network none`, refusing to build a
  checkout other than `GITHUB_SHA`. The Dev Container PR gate rehearses that
  release build on every container-affecting pull request. The Linux bundle is
  CPU-only, built without the ONNX Runtime backend, and needs glibc 2.43 or
  newer (Ubuntu 26.04-class); it does not run on Ubuntu 24.04 or Debian 13 in
  1.0.0-rc.1. `check-container-build.sh --verify` now rejects a symlinked
  provenance stamp.
