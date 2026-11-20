---
paths:
  - .github/workflows/libvmaf-build-matrix.yml
  - .github/workflows/windows-tester-bundle.yml
  - build-config.env
  - scripts/ci/test_windows_sycl_toolset_contract.py
invariant: Shared Windows SYCL pin in build-config.env. Verify installer size and SHA-256. Probe AOT before configure.
---
# Windows SYCL toolset (ADR-3568)

One Windows oneAPI pin in `build-config.env` for Builds and the tester zip.
Verify installer size and SHA-256 before extraction. Skip VS2026 integration.
Keep refused AOT set `tgllp,adl-s,adl-p,adl-n,rpl-s,rpl-p`. Probe before
configure. Pass accepted targets to Meson. SYCL zip overlay carries the AOT
parser for older source refs. Both selectors watch the shared pin and loader. Contract:
`scripts/ci/test_windows_sycl_toolset_contract.py`.
