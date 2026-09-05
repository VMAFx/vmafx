- CI publishes the dev container's build toolchain to
  `ghcr.io/vmafx/vmafx-dev-builder` (tags `master` and `sha-<short>`) on pushes
  to `master` that touch the container inputs. This is the image the release
  path will build inside, removing the blocker that kept `supply-chain.yml`'s
  `build-artifacts` compiling on a bare runner in violation of ADR-1102. Pull
  requests are unchanged and publish nothing; the package is private, so its
  only reader is a `GITHUB_TOKEN`-authenticated job in this repository. See
  [ADR-1186](docs/adr/1186-publish-dev-builder-image.md).
