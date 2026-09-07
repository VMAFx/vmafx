- Every container base image now comes from a single file,
  [`build-config.env`](build-config.env), instead of being pinned by hand in
  each Dockerfile. Changing the Debian generation, the Go release or a GPU SDK
  is a one-line edit; `make base-images-sync` propagates it and
  `scripts/ci/check-base-image-single-source.sh` fails the build on drift.
  Closes four Debian 12 pins (`distroless/cc-debian12`,
  `distroless/static-debian12`, two `golang:1.27-bookworm`) and two Ubuntu
  24.04 CUDA pins, and brings four base pins that were hidden inside
  `COPY --from=<image>` under the same gate. See
  [ADR-1231](docs/adr/1231-base-image-single-source.md) and
  [docs/development/base-images.md](docs/development/base-images.md).
- The `vmafx-controller` image now runs as UID 65532 instead of root. Its base
  was `gcr.io/distroless/cc-debian12` *without* the `:nonroot` suffix; it now
  shares the `:nonroot` runtime pin used by every other image, with an explicit
  `USER` line. Both listen ports (8080, 9090) are above 1024, so no capability
  is required.
