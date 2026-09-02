- `renovate.json`: add eight new `customManagers` entries so Renovate
  auto-tracks every `ARG`-pinned dependency in `dev/Containerfile`:
  Intel compute-runtime/NEO (`ARG NEO_VER`), Intel gmmlib (`ARG GMMLIB_VER`),
  Intel Graphics Compiler/IGC (`ARG IGC_VER`, semver portion before `+build`),
  Level Zero loader (`ARG LEVEL_ZERO_VER`), ONNX Runtime (`ARG ORT_VERSION`),
  SVT-AV1 (`ARG SVTAV1_VERSION`, via `gitlab-tags`), VVenC (`ARG VVENC_VERSION`),
  and AMD AMF headers (`ARG AMF_VERSION`). The existing FFmpeg manager is
  extended to also cover `ARG FFMPEG_TAG` in the Containerfile. All dev-image
  deps are grouped with `automerge: false` and `labels: ["dependencies", "dev-image"]`
  due to container-rebuild blast radius. Three deps remain untracked pending
  a Containerfile ARG refactor: `cuda-toolkit-13-2` (version in package name,
  dash-separated format), `cuda-keyring_1.1-1` (Debian revision, not semver),
  and `NV_CODEC_HEADERS_REF` (raw git SHA, no release tags).
  See [ADR-0605](docs/adr/0605-renovate-custommgr-dev-image.md).
