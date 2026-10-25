<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-3371: Real SYCL runtime and shared oneAPI build script for `vmafx-node-sycl`

- **Status**: Accepted
- **Date**: 2026-10-24
- **Deciders**: VMAFx maintainers
- **Tags**: `docker`, `container`, `sycl`, `node`, `licensing`, `ci`, `oneapi`

## Context

The `docker/Dockerfile.node` stage `node-sycl` previously declared `ENV VMAFX_BACKEND=sycl`
but copied `libvmaf` from stage `vmaf-builder`, which configured Meson with
`-Denable_sycl=false`. Consequently, the resulting "sycl" node container image scored on CPU
only and lacked the Level Zero loader and Intel runtime files declared in
`tools/rc1-tester/image/sycl-runtime.json`. This silent fallback to CPU scoring represented
a defect and violated the contract of the SYCL node variant.

Meanwhile, `docker/Dockerfile.production-gpu` stage `builder-oneapi2026` already built
`libvmaf` with `icx`/`icpx` and `-Denable_sycl=true`, installed Intel oneAPI and ocloc tools via
`scripts/ci/install-intel-oneapi.sh` and `install-intel-ocloc.sh`, staged the required runtime
files via `prepare_build.py intel-runtime tools/rc1-tester/image/sycl-runtime.json`, and scanned
licences. Replicating the Meson invocation and staging steps across Dockerfiles would violate
HISS-19 (single source of truth).

## Decision

We will extract the oneAPI libvmaf compilation, staging, and licence scanning logic into a
single shared script `scripts/ci/build-oneapi-libvmaf.sh`, invoked by both
`docker/Dockerfile.production-gpu` (stage `builder-oneapi2026`) and `docker/Dockerfile.node`
(stage `builder-oneapi2026`).

In `docker/Dockerfile.node`, `node-sycl` copies `libvmaf`, `libvmafx`, `vmaf`, and staged Intel
runtime files (`/stage/lib/intel/` and `/stage/licenses/`) from `builder-oneapi2026` instead of
`vmaf-builder`, sets `LD_LIBRARY_PATH="/usr/local/lib:/usr/local/lib/intel"`, retains FFmpeg,
Node runtime, MCP server, `ENV VMAFX_BACKEND=sycl`, and runs as nonroot.

The distroless runtime has no package manager, so stage `intel-gpu-runtime-libs` installs
Intel's GPU user-mode stack with `scripts/ci/install-intel-ocloc.sh --components runtime`
(the installer the production oneAPI image uses), purges the OpenCL packages, and exports the
remaining four (`libze1`, `libze-intel-gpu1`, `intel-igc-core-2`, `libigdgmm12`) with their
dpkg `status.d` records and `md5sums`. The Unified Runtime adapters load the Level Zero
loader at device discovery; without the stack, SYCL reports no device, and the build would
pass while the image could never score on a GPU. Stage `node-sycl-runtime-check` runs `ldd`
on the adapters, the GPU driver, `vmaf`, `libvmaf` and `libvmafx` and fails on any library it
cannot resolve. The final stage depends on that check.

The image licensing model is extended by adding `production-node-sycl-image` to
`tools/rc1-tester/image/licensing.json`, combining `production-node-image` dependencies with
the Intel runtime components (`intel-oneapi-dpcpp-runtime`, `intel-oneapi-umf`,
`intel-oneapi-tcm-hwloc`, `intel-gpu-stack`), and enforcing licence receipt generation.
`.github/workflows/docker-publish-operator-node.yml` publishes `ghcr.io/vmafx/vmafx-node:<tag>-sycl`
for `linux/amd64` with SBOM, cosign attestation, and licence artifacts.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **a) Shared builder script (chosen)** | Single source of truth for oneAPI build flag `-Denable_sycl=true`; zero Meson drift; strict HISS-19 compliance | Requires coordinating common staging paths between Dockerfiles | Chosen: eliminates duplication and enforces identical build flags |
| **b) Copy builder-oneapi2026 stage verbatim into Dockerfile.node** | Simple textual patch in Dockerfile.node | Duplicates ~75 lines of build script, flags, environment setup, and licence extraction; high maintenance drift | Violates HISS-19; Meson flag changes would need manual sync across both Dockerfiles |
| **c) Base node-sycl on production oneapi image** | Reuses already built production-gpu oneapi image layer | Couples node image to production CLI image packaging; imports unnecessary CLI utilities or requires stripping; alters base layering | Increases coupling across release image lifecycles and muddies node-specific base layering |

## Consequences

- **Positive**: `vmafx-node-sycl` has a functional SYCL runtime with Level Zero loader and AOT Intel GPU kernels; no silent fallback to CPU; oneAPI build flags and staging logic are maintained in one place.
- **Negative**: Building `node-sycl` requires oneAPI toolchain downloads (`ONEAPI_BUILDER`), increasing image build time when cache is cold.
- **Neutral / follow-ups**: Automated contract tests (`scripts/ci/tests/test_node_sycl_image_contract.py`) enforce that `node-sycl` copies exclusively from `builder-oneapi2026` and that `-Denable_sycl=true` is never duplicated outside the shared script.

## References

- Operator decision `vmafx-node-sycl-shape` (paraphrased)
- [ADR-1129](1129-release-container-runtime-alignment.md): Release container runtime alignment
- [ADR-1368](1368-oneapi-release-image-debian13.md): oneAPI release image on Debian 13
- [ADR-3358](3358-vmafx-sycl-ffmpeg-container.md): SYCL + patched-ffmpeg image
