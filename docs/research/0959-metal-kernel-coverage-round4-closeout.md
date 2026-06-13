<!-- markdownlint-disable MD014 -->
# Research digest — Metal kernel coverage round 4 (closeout)

**Date**: 2026-05-31
**ADR**: [ADR-0959](../adr/0959-metal-kernel-coverage-round4-closeout.md)
**Companion ADRs**: ADR-0214 (cross-backend parity gate), ADR-0361 (Metal
scaffold), ADR-0421 (first Metal kernel), ADR-0589 (Metal SSIM tolerance).

## Question

Are there any kernels under `core/src/feature/metal/` that still
lack a CPU-vs-Metal parity test after rounds 2 (PR #379) and 3
(PR #447)? And if 8/8 coverage is achieved, what structural guard
prevents silent regression the day a 9th kernel lands?

## Method

1. **Enumerate `.mm` / `.metal` files on disk:**

   ```text
   $ find core/src/feature/metal -maxdepth 3 -type f \
        \( -name '*.mm' -o -name '*.metal' \) | sort
   ```

   Yields 8 kernel pairs:
   `float_moment`, `float_motion`, `float_ms_ssim`, `float_psnr`,
   `float_ssim`, `integer_motion`, `integer_motion_v2`, `integer_psnr`.

2. **Enumerate `test_metal_*` tests on `master`:**

   ```text
   $ find core/test -maxdepth 2 -name 'test_metal*' -type f | sort
   ```

   Yields **only** `test_metal_install_header.c` + `test_metal_smoke.c`.
   No per-kernel parity tests on `master` (rounds 2 + 3 are still
   open DRAFTs as of 2026-05-31).

3. **Examined open Metal PRs:**
   - PR #379 (round 2, DRAFT): adds parity for `motion_v2`,
     `integer_psnr`, `float_psnr`, `float_ssim`.
   - PR #447 (round 3, DRAFT): adds parity for `integer_motion`,
     `float_motion`, `float_moment`, `float_ms_ssim`.
   - PR #351 (round 1, merged): registration audit for all 8 extractors.

   Sum across rounds = 8 kernels covered post-merge.

4. **Build-wiring audit:**
   Read `core/src/metal/meson.build`. Every `.mm` is in
   `metal_objcpp_lib` sources; every `.metal` has a `custom_target`
   producing a `.air` file; all 8 `.air` outputs fold into
   `metal_air_files` → `default.metallib`. **No dormant scaffolds —
   every kernel file on disk is in the build.**

5. **Dispatch-strategy audit:**
   Read `core/src/metal/dispatch_strategy.c`. The
   `g_metal_features[]` array carries all 8 `<name>_metal` names
   plus the provided-feature keys each kernel emits. No phantom
   entries; no missing entries.

## Findings

- **Kernel coverage**: **8 / 8 kernels** will be backed by per-kernel
  parity tests once PR #379 and PR #447 merge. No remaining gaps.
- **Build wiring**: 8 / 8 `.mm` + 8 / 8 `.metal` files wired into
  `metal_objcpp_lib` and `metal_air_files`. **No dormant scaffolds**
  (contrast with SYCL r4 PR #465 which surfaced `speed_chroma_sycl.cpp`
  as a dormant `.cpp` not in the build).
- **Structural gap**: the per-kernel tests name their target extractor
  inline; the suite passes silently when a new kernel ships without
  a test. The fork-wide cross-backend gate (ADR-0214) enforces
  parity *for tests that exist*; it does not enforce that every
  kernel HAS a test.

## Decision rationale

Ship a coverage-audit test that enumerates the 8 expected kernel
basenames and asserts:

1. Each `<basename>_metal` is registered via
   `vmaf_get_feature_extractor_by_name` (CPU-side, runs everywhere).
2. Each is accepted by `vmaf_metal_dispatch_supports` (gated on
   `-ENODEV` for non-Apple-Family-7 runners).
3. Plausible-looking phantom names (`vif_metal`, `adm_metal`,
   `ciede2000_metal`, `ssimulacra2_metal`) are *not* supported —
   wildcard-regression guard.
4. The basename list size matches the explicit
   `EXPECTED_KERNEL_COUNT` macro — defensive cross-check.

This pattern mirrors the CUDA round-4 closeout (PR #464, 19/19) and
SYCL round-4 closeout (PR #465). Hand-maintained list with an
explicit count is chosen over build-time glob because (a) meson
`files()` doesn't allow globs, (b) build-time codegen adds a CI step,
(c) SYCL r4 set the precedent. The trade-off is one audit-list edit
per new kernel, in exchange for explicit "every Metal kernel MUST
have registration + dispatch + parity test in the same PR" enforcement.

## Reproducer

```bash
# Audit step 1: enumerate kernels.
find core/src/feature/metal -maxdepth 3 -type f \
     \( -name '*.mm' -o -name '*.metal' \) | sort

# Audit step 2: enumerate existing parity tests.
find core/test -maxdepth 2 -name 'test_metal*parity*' -type f | sort

# Audit step 3: run the new closeout audit (CPU-only OK; dispatch
#               check skips with -ENODEV on non-Apple-Family-7).
cd core && meson setup build-cpu \
      -Denable_cuda=false -Denable_sycl=false -Denable_metal=enabled
ninja -C build-cpu test_metal_kernel_coverage_audit
meson test -C build-cpu test_metal_kernel_coverage_audit
```

## Cross-references

- ADR-0214 — cross-backend parity gate, places=4 default.
- ADR-0361 — Metal backend scaffold (T8-1).
- ADR-0421 — first Metal kernel `integer_motion_v2`.
- ADR-0460 — dispatch registry audit (HIP/Metal dispatch-support
  alignment).
- ADR-0589 — Metal `float_ssim` option parity + 1e-3 SSIM-family
  tolerance bound.
- PR #351 / PR #379 / PR #447 — Metal coverage rounds 1 / 2 / 3.
- PR #464 — CUDA kernel coverage round 4 (sibling).
- PR #465 — SYCL kernel coverage round 4 (sibling, with
  `speed_chroma_sycl.cpp` dormant-scaffold finding).
- `docs/research/0761-metal-backend-audit-20260529.md` — prior Metal
  audit that informed this closeout.
