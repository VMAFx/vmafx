---
paths:
  - core/src/feature/arm64/**
  - .github/workflows/lint-and-format.yml
invariant: New NEON/SVE2 TU scaffolding, arm64-only clang-tidy ratchet measurement, upstream sync, and governing ADRs.
---
<!-- markdownlint-disable MD013 -->
# Adding TUs, arm64 clang-tidy ratchet, and governing ADRs

## Adding a new NEON / SVE2 TU

Use [`/add-simd-path`](../../../../../.claude/skills/add-simd-path/SKILL.md).
Skill scaffolds TU + header + dispatch entry + bit-exact regression test using
shared [`simd_bitexact_test.h`](../../../test/simd_bitexact_test.h) harness
(ADR-0245).

## Lint: this directory has exactly one lane (ADR-1283)

Only `arm64` clang-tidy ratchet lane measures these TUs. `cpu`, `cuda`,
`hip` and `sycl` lanes all build x86, so none of their compile databases holds
command for anything here, and `exclude_untidyable()` in
`.github/workflows/lint-and-format.yml` drops `^core/src/feature/arm64/` from
fast `Tidy Changed` job for same reason. Before lane existed
`vif_neon.c` carried 36 findings that nothing had ever reported.

Re-measure after any change here, and commit tightened baseline in same
PR (ADR-1142 — cleaned file whose baseline was not tightened fails `exit 3`):

```bash
meson setup build-arm64 core --cross-file build-aux/aarch64-linux-gnu.ini \
    -Denable_cuda=false -Denable_sycl=false -Db_lto=false
ninja -C build-arm64            # generated model-JSON -> C sources must exist
make tidy-ratchet LANE=arm64 TIDY_RATCHET_BUILD_DIR=build-arm64
```

Needs aarch64 cross gcc and glibc sysroot; clang-tidy is handed same
`--target` / `--sysroot` so it parses `<arm_neon.h>` and `<arm_sve.h>` as
AArch64 rather than against host's x86 headers.

## Upstream-sync notes

Same rules as [`../x86/AGENTS.md`](../../x86/AGENTS.md): every TU carries Netflix
copyright header at structural level. On `/sync-upstream` walk AVX twin +
scalar reference + shared SIMD-tail reduction helper before merging.
Cross-backend parity gate at `places=4` catches drift only after full run.

## Governing ADRs

Full list: [../AGENTS.md §Governing ADRs](../../AGENTS.md).
Directory invariants:

- [ADR-0125](../../../../../docs/adr/0125-ms-ssim-decimate-simd.md) — MS-SSIM
  decimate separable SIMD.
- [ADR-0139](../../../../../docs/adr/0139-ssim-simd-bitexact-double.md) — SSIM
  accumulate per-lane scalar-double reduction.
- [ADR-0140](../../../../../docs/adr/0140-simd-dx-framework.md) — `simd_dx.h`
  framework.
- [ADR-0145](../../../../../docs/adr/0145-motion-v2-neon-bitexact.md) — `motion_v2`
  NEON arithmetic-shift contract.
- [ADR-0160](../../../../../docs/adr/0160-psnr-hvs-neon-bitexact.md) — `psnr_hvs`
  NEON DCT.
- [ADR-0161](../../../../../docs/adr/0161-ssimulacra2-simd-bitexact.md) +
  [ADR-0162](../../../../../docs/adr/0162-ssimulacra2-iir-blur-simd.md) +
  [ADR-0163](../../../../../docs/adr/0163-ssimulacra2-ptlr-simd.md) +
  [ADR-0213](../../../../../docs/adr/0213-ssimulacra2-sve2.md) +
  [ADR-0252](../../../../../docs/adr/0252-ssimulacra2-host-xyb-simd.md) —
  SSIMULACRA 2 SIMD ports (NEON + SVE2 + host-path).
- [ADR-0245](../../../../../docs/adr/0245-simd-bitexact-test-harness.md) — shared
  bit-exact test harness.
- [ADR-0584](../../../../../docs/adr/0584-moment-sve2-port.md) — `float_moment`
  SVE2 VLA f32→f64 port.
