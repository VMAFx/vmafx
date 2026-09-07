- **The Linux SYCL toolchain version lives in one place, and moves to 2026.1.**
  Three workflows (`build.yml`, `ffmpeg-integration.yml`,
  `libvmaf-build-matrix.yml`) each spelled
  `intel-oneapi-compiler-dpcpp-cpp-2025.3` inline, so the version could only be
  bumped by editing every one and the tree drifted from the toolchain actually
  installed on the workstation. They now source `build-config.env` — which they
  already did for `LEVEL_ZERO_VERSION` — and install `${ONEAPI_APT_PACKAGE}`.
  `ONEAPI_VERSION` is `2026.1`. The config also records
  `ONEAPI_RUNTIME_APT_PACKAGES`, which names `intel-oneapi-umf` explicitly
  because `intel-oneapi-runtime-dpcpp-cpp` does not depend on it: without it
  `libumf.so.1` is missing and the adapter fails to load, which presents as
  "No device of requested type available" rather than as a link error. The
  Windows leg stays on 2025.3.0.372 — its offline-installer URL carries an
  opaque per-build GUID that cannot be derived from a version number — and is
  tracked as `T-ONEAPI-WINDOWS-CI-LAG-2026-09-07`.
