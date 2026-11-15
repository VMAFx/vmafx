---
paths:
  - scripts/ci/clang-tidy-sycl.sh
  - scripts/ci/gen-sycl-compile-commands.py
  - .clang-tidy
invariant: icpx-aware clang-tidy; stock LLVM clang-tidy cannot resolve <sycl/sycl.hpp>.
area: build-and-parity
---
<!-- markdownlint-disable MD013 MD060 -->
# icpx-aware clang-tidy

Stock LLVM `clang-tidy` cannot resolve `<sycl/sycl.hpp>`. Use
[`scripts/ci/clang-tidy-sycl.sh`](../../../../../scripts/ci/clang-tidy-sycl.sh),
which injects oneAPI SYCL include path +
`-D__SYCL_DEVICE_ONLY__=0`, locates `icpx` via `$ICPX_ROOT` (or
`/opt/intel/oneapi/compiler/latest`). CI lane
`Tidy SYCL` runs wrapper. Required check since ADR-1297;
no longer advisory, no `continue-on-error`.
Adding new SYCL TU needs no AGENTS.md update — wrapper
finds it via changed-file diff. See
[ADR-0217](../../../../../docs/adr/0217-sycl-toolchain-cleanup.md).

- [ADR-0217](../../../../../docs/adr/0217-sycl-toolchain-cleanup.md) —
  icpx-aware clang-tidy wrapper.
