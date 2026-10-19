---
paths:
  - scripts/ci/check-container-build.sh
  - scripts/ci/tests/test-check-container-build.sh
invariant: One accepted image identity, `vmaf-dev-mcp`, written in two roots of `dev/Containerfile`; new identity = ADR + marker.
area: release
---
<!-- markdownlint-disable MD013 MD060 -->
# Container-build provenance gate (ADR-1102, ADR-1346, ADR-1354)

`check-container-build.sh` accepts one identity: `CANONICAL_TITLE`
(`vmaf-dev-mcp`), `image_title` `dev/Containerfile` writes in its two
roots: `build-deps` (every later dev stage, `libvmaf-build`, `dev-mcp`,
inherits it) and `release-build` (Debian 13 release build image, ADR-1354;
same bytes). Retired `vmaf-sycl-arc-runner` (ADR-1178) and
`vmafx-*` aliases stay rejected; no image writes them. Adding identity =
new ADR plus marker write in image, never allowlist entry alone.
`--verify` rejects symlinked stamp, same as
`verify-native-release-artifacts.sh`; keep both gates agreeing.
`tests/test-check-container-build.sh` extracts marker from
`dev/Containerfile`, asserts exactly two writes (one in `build-deps`, one in
`release-build` rooted at `${RELEASE_BUILDER_BASE}`, byte-identical),
`gpu-sdks` -> `libvmaf-build` chain and `CANONICAL_TITLE` equality, plus
near-miss titles (case, prefix, whitespace, first-key-wins), symlinked and
dangling stamps, and native-verifier fixture that reaches provenance
check (executable `vmaf`, non-empty `libvmaf.so`) and asserts its message.
Stamp schema `vmafx-container-build-provenance/1` unchanged; `--verify` runs
on any host.
