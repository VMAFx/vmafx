- Native release artifacts are built on a GitHub-hosted runner inside the
  `libvmaf-build` container stage ([ADR-1346](../docs/adr/1346-hosted-slim-container-release-build.md),
  supersedes ADR-1178). `build-artifacts` in `supply-chain.yml` no longer waits
  for a self-hosted runner that is not registered. It builds the release tag's
  own `libvmaf-build` stage with `scripts/ci/build-dev-container-stage.sh`, the
  script the Dev Container PR gate also uses, with no layer cache and no
  registry, then compiles, stages, stamps and verifies the bundle inside it with
  networking disabled. The Linux bundle is CPU-only, built without the ONNX
  Runtime backend, and needs glibc 2.43 or newer (Ubuntu 26.04-class).
