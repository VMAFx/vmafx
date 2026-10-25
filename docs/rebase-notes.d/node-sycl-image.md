## Node SYCL image (2026-10-24)

- `docker/Dockerfile.node`: Added `builder-oneapi2026` stage invoking
  `scripts/ci/build-oneapi-libvmaf.sh` and removed ad-hoc runtime stages.
  `node-sycl-assembled` copies libvmaf, libvmafx, vmaf, `/stage/lib/intel/`,
  and `/stage/licenses/` from `builder-oneapi2026`. Added licence check, notices,
  source fetch, and export stages. Final target `node-sycl` sets
  `ENV VMAFX_BACKEND=sycl` and `LD_LIBRARY_PATH="/usr/local/lib:/usr/local/lib/intel"`.
- `docker/Dockerfile.production-gpu`: Refactored stage `builder-oneapi2026` to
  delegate Meson configuration, ninja build, and staging to
  `scripts/ci/build-oneapi-libvmaf.sh`.
- `scripts/ci/build-oneapi-libvmaf.sh`: Shared oneAPI build script compiling
  libvmaf with `-Denable_sycl=true`, staging files per
  `tools/rc1-tester/image/sycl-runtime.json`, and running licence scans.
- `tools/rc1-tester/image/licensing.json`: Added `production-node-sycl-image`
  component combining `production-node-image` and SYCL runtime components.
- `.github/workflows/docker-publish-operator-node.yml`: Added `publish-node-sycl`
  job publishing `ghcr.io/vmafx/vmafx-node:<tag>-sycl` for amd64 with cosign
  attestations and licence artifacts.
