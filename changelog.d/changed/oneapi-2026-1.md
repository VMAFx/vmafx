- The SYCL toolchain moves to **Intel oneAPI 2026.1.1**, installed from Intel's
  apt repository onto Debian 13 rather than pulled as an Intel container image.
  The published tag is now `-oneapi2026` (was `-oneapi2025`). Compiler and
  runtime are pinned to the same version because the SYCL soname changed across
  the 2025/2026 boundary (`libsycl.so.8` → `libsycl.so.9`), which is a hard ABI
  break rather than the usual backward-compatible case — a 2025-built binary
  cannot load against a 2026 runtime at all.
- The oneAPI runtime image now ships on Debian 13 like every other release
  image. Intel publishes for Ubuntu only, and Ubuntu 26.04's glibc 2.43 is newer
  than Debian 13's 2.41, so binaries built in Intel's image could not have run
  on the runtime this project ships. The image also installs the Intel NEO
  compute driver explicitly: Intel's runtime image bundles it and Debian does
  not package it, so omitting it produces an image where SYCL loads and every
  device query then fails.
- **The dev container was silently frozen on oneAPI 2025.3.2.** It installed the
  unversioned `intel-basekit` meta-package precisely because Intel used to bump
  it — but Intel has retired the kit meta-packages (`intel-basekit` and
  `intel-oneapi-base-toolkit` both stop at 2025.3.2) and only ships 2026 as
  component packages. apt reported no error. The dev container now installs the
  same pinned component package as the release images and fails the build if
  `icpx` is not the 2026 compiler.
- `docker/dev/ubuntu-26.04-sycl.Dockerfile` actually builds on Ubuntu 26.04 now.
  It was pinned to 24.04 under a comment claiming Intel published no 26.04
  variant; Intel does, under the `intel/oneapi` repository that replaced the
  retired `intel/oneapi-basekit`.
- **The published oneAPI image gains zero-copy DMA-BUF import.** It never had
  it: Intel's basekit builder carried Level Zero but not VA-API, so
  `HAVE_SYCL_DMABUF` was undefined and the path compiled out to its stub, while
  the dev container built it in. The release image was quietly less capable than
  the container it was developed in. The builder now installs `libva-dev` and
  asserts at configure time that both Level Zero and VA-API are present.
