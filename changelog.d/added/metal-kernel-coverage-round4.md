### Added — Metal kernel coverage round 4 (closeout)

- `core/test/test_metal_kernel_coverage_audit.c` — meta-audit
  regression guard that enumerates every `.mm` kernel basename under
  `core/src/feature/metal/` and asserts each has a registered
  `<basename>_metal` extractor plus a row in
  `vmaf_metal_dispatch_supports`. Defends against silent gaps the day
  a future kernel ships without a registration row, a dispatch row, or
  a per-kernel CPU-vs-Metal parity test.
- Coverage closeout: **8 / 8** Metal kernels are now backed by a
  per-kernel parity test (rounds 2 + 3 in PRs #379 / #447) plus this
  round-4 structural audit. Aligns Metal with CUDA round 4 (PR #464,
  19/19 kernels) and SYCL round 4 (PR #465) audit-by-enumeration
  precedent.
- See [ADR-0959](../../docs/adr/0959-metal-kernel-coverage-round4-closeout.md).
