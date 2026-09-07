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
- The same file now pins the **toolchain versions CI installs**, not just
  container bases. ROCm was `7.2.4` in one workflow and `7.2.3` in another — so
  CI validated a ROCm the published images never shipped — and the Level Zero
  loader existed at four versions at once (`v1.18.5`, `v1.28.0` twice,
  `v1.29.0`, `1.32.0`), a class of skew that surfaces at runtime as "No device
  of requested type available" rather than as a build failure. Workflow steps
  source `build-config.env`, and `scripts/ci/check-workflow-versions.py` fails
  the build if a literal reappears.
- Renovate is reconfigured so a base-image bump updates the config and every
  Dockerfile mirror in one PR. Its built-in `dockerfile` manager understands
  `ARG X=image` + `FROM $X` and would otherwise have updated only the mirrors,
  failing the new gate on Renovate's own pull requests.
- ROCm images move to the Ubuntu 26.04 variant of 10.0.0. The libraries are
  copied out of the vendor image into a Debian 13 runtime, so this was checked
  rather than assumed: the `/opt/rocm/core-10.0/lib` layout is identical in both
  variants and the copied closure needs at most `GLIBC_2.28` against Debian 13's
  2.41. No release image is on Ubuntu 24.04 any more except oneAPI, whose
  migration is tracked separately.
