# Container image invariants

## Node model root

`Dockerfile.node` stages the contents of `model/` with
`cp -r model/. /dist/model/`; do not copy the directory itself. The runtime
copy maps that staging root to `/usr/local/share/vmafx/model`, which is also the
exact `VMAFX_MODEL_DIR`. The builder must continue to assert that
`/dist/model/vmaf_v0.6.1.json` exists so a nested `model/model/` layout fails at
build time instead of producing a worker that cannot resolve its packaged
model.

## Base images come from `build-config.env` (ADR-1231)

Do not write a base image into a Dockerfile in this directory. Every base is an
`ARG` whose default mirrors the root-level `build-config.env`; edit that file
and run `make base-images-sync`, never the `ARG` line by hand.

`COPY --from=<digest-pinned image>` counts as a base-image pin and is rejected
by `scripts/ci/check-base-image-single-source.sh`. Declare a named stage
instead — `FROM ${CUDA_RUNTIME} AS cuda-runtime-libs`, then
`COPY --from=cuda-runtime-libs …`. BuildKit prunes unused stages, so the extra
stage is free. Four pins hidden this way were the most out-of-date images in
the repository.

See [docs/development/base-images.md](../docs/development/base-images.md).
