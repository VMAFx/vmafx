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

CUDA base images (ADR-1306): `CUDA_BUILDER` and `CUDA_RUNTIME` are digest-pinned
Ubuntu 26.04 (`ubuntu:26.04@sha256:...`); never re-introduce `nvidia/cuda` base
images. Toolkit compiler and runtime packages install via
`scripts/ci/install-cuda-toolkit.sh` (`--mode=builder` or `--mode=runtime`).

See [docs/development/base-images.md](../docs/development/base-images.md).

## FFmpeg stable-release mirror

`build-config.env` owns `FFMPEG_TAG`; `docker/Dockerfile.node` carries a
generated default mirror and must not choose a release independently. The
current baseline is `n9.0.2`, and every update must replay all entries in
`ffmpeg-patches/series.txt` cumulatively before the mirror changes. Run
`python3 scripts/ci/ffmpeg_patch_stack.py --check` after refresh; a per-patch
`git apply --check` does not model the stack's cumulative context.

Every maintained FFmpeg builder configures with `--fatal-warnings` and scans
the complete compiler log for `warning:`. The same contract is mirrored by the
root CUDA image, `Dockerfile.ffmpeg`, `dev/Containerfile`, and
`docker/Dockerfile.node`; `scripts/ci/test_e2e_runtime_contract.py` pins all
four. Fix new diagnostics in source without warning suppressions or component
removal. Patch 0019 owns the 126-diagnostic GCC 14/16 hardening for the n9.0.2
baseline. Use `scripts/ci/checkout-annotated-tag.sh` for the FFmpeg checkout;
direct shallow clones emit a warning for the annotated release tag and violate
the same zero-diagnostic image contract.

## Partial libvmaf builder closure

`docker/Dockerfile.node` and root `Dockerfile.go-server` copy only the source
needed by their `vmaf-builder` stages. Keep that partial context closed over
all configure inputs: both must copy
`scripts/ci/check-msvc-clz-shim.sh` before `meson setup`. Their build package
sets must include `xxd` (otherwise the default built-in models silently turn
off) and `make` (GCC's numeric LTO partitioning invokes it; without it the
linker warns and falls back to serial LTRANS).

Stage `libvmaf.so*` with `cp -a` so the SONAME symlink chain survives. Also
stage Meson's generated `meson-private/libvmaf.pc`; never synthesize it from
`VMAFX_VERSION`. The pkg-config version is the libvmaf interface version
(`3.0.0`), not the release-please product tag (`dev` in a local build).
`scripts/ci/test_e2e_runtime_contract.py` pins these invariants for both
Dockerfiles.

## Fedora optional SYCL repository

`dev/fedora-40.Dockerfile` writes seven literal oneAPI repository lines with
`printf '%s\n'` inside `ENABLE_SYCL=true`. Keep both signature checks,
repository-write → install → cleanup short-circuiting, and default-off
branch. Literal `\n` text cannot terminate Dockerfile heredoc: breaks parsing
even with branch disabled. Root Dockerfile's redirected while loop unrelated.
See [Research-2056](../docs/research/2056-fedora-scorecard-heredoc.md).
