---
paths:
  - core/meson_options.txt
  - core/meson.build
  - core/src/meson.build
invariant: SYCL feature TUs compile only when meson setup -Denable_sycl=true.
area: build-and-parity
---
<!-- markdownlint-disable MD013 MD060 -->
# Build

SYCL feature TUs compile only when `meson setup -Denable_sycl=true`.
Requires oneAPI (`source /opt/intel/oneapi/setvars.sh`) or equivalent
DPC++ toolchain with `icpx` on PATH.
