---
paths:
  - .github/workflows/libvmaf-build-matrix.yml
  - scripts/ci/test_windows_sycl_toolset_contract.py
invariant: Windows SYCL installer size and digest pinned; AOT probe precedes configure and enforces refused set.
---
# Windows SYCL toolset (ADR-3567)

Pin oneAPI installer URL, byte size and SHA-256; verify size and digest before
extraction. Keep refused AOT set `tgllp,adl-s,adl-p,adl-n,rpl-s,rpl-p`.
Probe targets before configure; pass accepted targets to Meson. Contract:
`scripts/ci/test_windows_sycl_toolset_contract.py`.
