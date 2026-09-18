# Container image invariants

## Node model root

`Dockerfile.node` stages contents of `model/` via
`cp -r model/. /dist/model/`; do not copy the directory itself. Runtime copy
maps that staging root to `/usr/local/share/vmafx/model` = exact
`VMAFX_MODEL_DIR`. Builder must keep asserting `/dist/model/vmaf_v0.6.1.json`
exists, so nested `model/model/` layout fails at build time, not as a worker
unable to resolve its packaged model.

## Base images come from `build-config.env` (ADR-1231)

Do not write base image into Dockerfile in this directory. Every base = `ARG`
whose default mirrors root-level `build-config.env`; edit that file, run
`make base-images-sync`, never edit `ARG` line by hand.

`COPY --from=<digest-pinned image>` = base-image pin, rejected by
`scripts/ci/check-base-image-single-source.sh`. Declare named stage instead —
`FROM ${CUDA_RUNTIME} AS cuda-runtime-libs`, then
`COPY --from=cuda-runtime-libs …`. BuildKit prunes unused stages, so extra
stage = free. Four pins hidden this way were most out-of-date images in repo.

See [docs/development/base-images.md](../docs/development/base-images.md).

## Fedora optional SYCL repository

`dev/fedora-40.Dockerfile` writes seven literal oneAPI repository lines with
`printf '%s\n'` inside `ENABLE_SYCL=true`. Keep both signature checks,
repository-write → install → cleanup short-circuiting, and default-off
branch. Literal `\n` text cannot terminate Dockerfile heredoc: breaks parsing
even with branch disabled. Root Dockerfile's redirected while loop unrelated.
See [Research-2056](../docs/research/2056-fedora-scorecard-heredoc.md).
