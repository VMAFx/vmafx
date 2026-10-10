<!-- markdownlint-disable MD013 -->
# Container image invariants

## Node model root

`Dockerfile.node` stages contents of `model/` via
`cp -r model/. /dist/model/`; do not copy directory itself. Runtime copy
maps that staging root to `/usr/local/share/vmafx/model` = exact
`VMAFX_MODEL_DIR`. Builder must keep asserting `/dist/model/vmaf_v0.6.1.json`
exists, so nested `model/model/` layout fails at build time, not as worker
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
Ubuntu 26.04 (`ubuntu:26.04@sha256:...`) and must equal `DEV_BASE` exactly,
including its digest; never re-introduce `nvidia/cuda` base images. narrow
`dev/ubuntu-26.04-cuda.Dockerfile` compatibility image is part of this owner;
Alpine, Arch, and Fedora compatibility files are not. Toolkit compiler and
runtime packages install via `scripts/ci/install-cuda-toolkit.sh`
(`--mode=builder` or `--mode=runtime`) with exact `package=version` operands and
installed-version checks. `build-config.env` owns release lock and exact
toolkit/nvcc/cudart versions; series-only apt package is not pin.
Renovate discovers `CUDA_VERSION` through official NVIDIA redist HTML index
(`custom.nvidia-cuda-redist`), never through `nvidia/cuda` Docker tags.
`Dockerfile.production-gpu` uses neither `CUDA_*` nor `ROCM_RUNTIME`: every GPU
builder + runtime = `RELEASE_BUILDER_BASE` (ADR-1517); `ROCM_BUILDER` only as
image `install-rocm-from-image.sh` streams `/opt/rocm` from.

oneAPI bases (ADR-1368): `ONEAPI_BUILDER` and `ONEAPI_RUNTIME` equal
`RELEASE_BUILDER_BASE` exactly, digest included (gate). Never bring back
`intel/oneapi-basekit` / `intel/oneapi-runtime`: 2025 images carry
compute-runtime 25.18, which segfaulted every Arc B580 SYCL run; 2026 images
cannot pair compiler + runtime on Debian 13 glibc.

See [docs/development/base-images.md](../docs/development/base-images.md).

## Production licence gate (ADR-1513)

`Dockerfile.production`: `cli` + `server` copy receipt of `cli-licence-check` /
`server-licence-check` (`licensing.py check`, kinds `production-cli-image` /
`production-server-image`). Distroless CLI, no interpreter: notices written
on copy of tree (`cli-notices`), copied back (`cli-with-notices`).
`*-source-export` = corresponding source, pushed as `<tag>-source` /
`<tag>-server-source` by `.github/actions/image-licence-artifacts` (also SPDX
SBOM per platform via `actions/attest`). Wheels built in `/build-venv`; runtime
`/venv` = runtime lock + wheels only. New file / package / wheel lib / licence
in image -> `tools/rc1-tester/image/licensing.json` same PR, else build fails.
Label `org.opencontainers.image.licenses` = `vmafx-binaries` licence set
(test-held).

`Dockerfile.production-gpu` (ADR-1517): `final-cuda13` / `final-rocm10` /
`final-oneapi2026` copy receipt of `<variant>-licence-check` (kinds
`production-cuda-image` / `-rocm-image` / `-oneapi-image`); notices on copy of
tree (`<variant>-notices`); `<variant>-source-export` pushed as
`<tag>-<variant>-source`. Runtime = only vendor files libvmaf loads: CUDA none
(build fails on NVIDIA file or NEEDED), ROCm `hip-runtime.json` ->
`/usr/local/lib/rocm`, Intel `sycl-runtime.json` -> `/usr/local/lib/intel`,
staged by `prepare_build.py` in builder to `/stage`. Records take tester vendor
components by reference (`"from"` + `rewrite`), never copies. ROCm targets =
`dist_amdgpu_targets` of ROCm's `dist_info.json`.

## Go service and node licence gate (ADR-1514)

`Dockerfile.operator`, `Dockerfile.go-server`, `Dockerfile.node` (`node-cpu`):
final target copies receipt of licence-check stage; go-builder runs
`licensing.py scan-go` + `go-licences`; `*source-export` built per platform in
build jobs, merged to `<image>:<tag>-source` by composite action. Node: FFmpeg
never `--enable-nonfree`; `ffmpeg-builder-cpu` writes `/ffmpeg-source/`
(patched tree via `git archive`, `CONFIGURE.txt`, patch series) + runs
`record-copied-debian-libs`; `rclone-bin` = `go install` at `RCLONE_VERSION`
(build-config.env), rclone smoke run under `env -u RCLONE_VERSION` (rclone
reads `RCLONE_*` as flags). `libvmaf.so*` copies via `find -maxdepth 1`, never
glob (`libvmaf.so.3.0.0.p/`). `fuse-tools` stage (ADR-1593): setuid
`fusermount3` (4755) + util-linux `mount`/`umount` (0755, setuid stripped) +
lib closure at Debian paths (`fusermount3` clears env: no LD_LIBRARY_PATH) ->
`runtime-base` `COPY /fuse-tools/root/ /`; record
`/usr/local/share/vmafx/fuse-tools/packages.list` = `dpkg-copied` component
`fuse-tools`. No `mount`/`umount` -> every mount fails where `/etc/mtab`
exists (Docker always). Guards: `test-docker-image-runtime-contract.sh`,
publish smoke mount.

## oneAPI production image (ADR-1368, ADR-1517)

`builder-oneapi2026` runs `scripts/ci/install-intel-oneapi.sh --mode=builder`,
then `scripts/ci/install-intel-ocloc.sh --components build`, then stages
`sycl-runtime.json` files (`prepare_build.py intel-runtime`). `oneapi2026-assembled`
runs `install-intel-ocloc.sh --components runtime` in `/tmp/vmafx`, purges
`intel-ocloc intel-opencl-icd intel-igc-opencl-2 ocl-icd-libopencl1`, then
ca-certificates + python3, same `RUN` (Level Zero only, as tester). Versions
come from `build-config.env` only (`ONEAPI_APT_VERSION`, `INTEL_NEO_VERSION`,
`LEVEL_ZERO_VERSION`). Load-bearing: NEO `runtime` set (B580 crash without it);
`libumf.so.1` in `sycl-runtime.json` (adapters need it); adapter `ldd` check
on `/usr/local/lib/intel`; `ldd` + `--version` of `vmaf`; `USER 65532:65532`. `final-oneapi2025`
stays alias stage; publish tags digest `-oneapi2026` and `-oneapi2025`
(HISS-14). Retire alias only in breaking release with `!` + `Migration:` footer.
`scripts/release/tests/test-docker-image-runtime-contract.sh` pins all of it.

## FFmpeg stable-release mirror

`build-config.env` owns `FFMPEG_TAG`; `docker/Dockerfile.node` carries generated default mirror and must not choose release independently. current baseline is `n9.0.2`, and every update must replay all entries in
`ffmpeg-patches/series.txt` cumulatively before mirror changes. Run
`python3 scripts/ci/ffmpeg_patch_stack.py --check` after refresh; per-patch
`git apply --check` does not model stack's cumulative context.

Every maintained FFmpeg builder configures with `--fatal-warnings` and scans
complete compiler log for `warning:`. same contract is mirrored by root CUDA image, `Dockerfile.ffmpeg`, `dev/Containerfile`, and
`docker/Dockerfile.node`; `scripts/ci/test_e2e_runtime_contract.py` pins all
four. Fix new diagnostics in source without warning suppressions or component
removal. Patch 0019 owns 126-diagnostic GCC 14/16 hardening for n9.0.2
baseline. Use `scripts/ci/checkout-annotated-tag.sh` for FFmpeg checkout;
direct shallow clones emit warning for annotated release tag and violate
same zero-diagnostic image contract.

## Partial libvmaf builder closure

`docker/Dockerfile.node` and root `Dockerfile.go-server` copy only source
needed by their `vmaf-builder` stages. Keep that partial context closed over
all configure inputs: both must copy
`scripts/ci/check-msvc-clz-shim.sh` before `meson setup`. Their build package
sets must include `xxd` (otherwise default built-in models silently turn
off) and `make` (GCC's numeric LTO partitioning invokes it; without it linker warns and falls back to serial LTRANS).

Stage `libvmaf.so*` with `cp -a` so SONAME symlink chain survives. Also
stage Meson's generated `meson-private/libvmaf.pc`; never synthesize it from
`VMAFX_VERSION`. pkg-config version is libvmaf interface version
(`3.0.0`), not release-please product tag (`dev` in local build).
`scripts/ci/test_e2e_runtime_contract.py` pins these invariants for both
Dockerfiles.

## Fedora optional SYCL repository

`dev/fedora-40.Dockerfile` writes seven literal oneAPI repository lines with
`printf '%s\n'` inside `ENABLE_SYCL=true`. Keep both signature checks,
repository-write → install → cleanup short-circuiting, and default-off
branch. Literal `\n` text cannot terminate Dockerfile heredoc: breaks parsing
even with branch disabled. Root Dockerfile's redirected while loop unrelated.
See [Research-2056](../docs/research/2056-fedora-scorecard-heredoc.md).

## nv-codec-headers pin

`build-config.env` owns `NV_CODEC_HEADERS_TAG` / `NV_CODEC_HEADERS_COMMIT`.
`ARG NV_CODEC_HEADERS_COMMIT` of `Dockerfile.tester` + `Dockerfile.production-gpu` repeat commit.
Never bump one copy alone: `scripts/ci/tests/test_nv_codec_headers_single_source.py`
fails on drift. Published rc records in `licensing.json` keep own commit.
