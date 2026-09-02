- **AdaptiveCpp / hipSYCL as a second SYCL toolchain
  ([ADR-0335](../docs/adr/0335-adaptivecpp-second-sycl-toolchain.md),
  [Research-0086](../docs/research/0086-sycl-toolchain-audit-2026-05-08.md)
  Topic B).** Contributors who do not want to install Intel's
  ~2.6 GB closed-source oneAPI base toolkit can now build the
  fork's `-Denable_sycl=true` path against AdaptiveCpp (formerly
  OpenSYCL / hipSYCL), the open-source LLVM-based SYCL
  implementation. Pass `-Dsycl_compiler=acpp` (default
  `--acpp-targets=generic`, override via `-Dsycl_acpp_targets`).
  Intel `icpx` remains the **primary** toolchain — fork-shipped
  binaries, Intel discrete-GPU codegen, and the OpenVINO / NPU
  enablement story stay icpx-coupled. New header
  `core/src/feature/sycl/sycl_compat.h` exposes a
  `VMAF_SYCL_REQD_SG_SIZE(N)` macro that reduces to
  `[[intel::reqd_sub_group_size(N)]]` under icpx and to a no-op
  under AdaptiveCpp; ten previously hard-coded kernel-attribute
  call sites switch to the macro. New
  [`docs/development/sycl-toolchains.md`](../docs/development/sycl-toolchains.md)
  documents the install paths (Arch AUR `adaptivecpp` 25.10.0-2),
  the per-toolchain capability matrix, and the numerical
  conformance gap (acpp output is not bit-identical to icpx; both
  remain non-bit-identical to the scalar CPU golden, consistent
  with the existing CPU-only golden-gate rule). The
  `--acpp-targets=omp` path lets a future CI lane exercise SYCL
  TUs on stock `ubuntu-latest` runners without Intel hardware,
  catching toolchain-monoculture bugs.
