# AGENTS.md — core/src/feature/third_party/xiph

Xiph third-party scalar reference, under feature tree. Parent:
[../../AGENTS.md](../../AGENTS.md).

## Scope

```text
third_party/xiph/
  psnr_hvs.c       # Xiph/Daala 8×8 integer DCT scalar reference for psnr_hvs
```

Provenance: `psnr_hvs.c` from Xiph.Org / Daala (Copyright 2001–2012
Xiph.Org and contributors, BSD-3-Clause; see file header). Netflix
imported it. Fork preserves byte-for-byte except where ADR-0159 /
ADR-0160 force lockstep updates with SIMD ports.

## Ground rules

- **Parent rules apply** (see [../../AGENTS.md](../../AGENTS.md) +
  [../../../AGENTS.md](../../../AGENTS.md)).
- **Preserve Xiph license header verbatim.** Upstream-mirror file for
  entire psnr_hvs feature; third-party copyright notice load-bearing
  for BSD-3-Clause attribution chain.
- **`#pragma STDC FP_CONTRACT OFF` at TU level** — keep. Scalar TU =
  bit-exact reference for AVX2 + NEON ports.

## Rebase-sensitive invariants

- **8×8 DCT butterfly block = bit-exact reference for three TUs**:
  1. `psnr_hvs.c` (this TU — scalar)
  2. [`../../x86/psnr_hvs_avx2.c`](../../x86/psnr_hvs_avx2.c) (ADR-0159)
  3. [`../../arm64/psnr_hvs_neon.c`](../../arm64/psnr_hvs_neon.c) (ADR-0160)

  Butterfly change here -> matched edits to both SIMD TUs, **same
  PR**, plus re-run of `test_psnr_hvs_{avx2,neon}` in
  [`../../../test/`](../../test/). One without others = bit-exactness
  regression, caught only by cross-backend parity gate after full
  run.

- **`accumulate_error()` threads `ret` by pointer.** ADR-0159 burned
  this lesson into AVX2 port; ADR-0160 mirrored it into NEON:
  local-float accumulator inside helper drifts Netflix golden by
  ~5.5e-5. Scalar TU here = reference for that threading shape —
  local accumulator here cascades to Netflix golden break.

- **`od_coeff` int32 layout fixed at 8×8 → flat 64-element array.**
  AVX2 vectorises across 8 rows in parallel via `__m256i`; NEON
  splits each 8-column row into low/high halves (`int32x4_t × 2`).
  Both paths assume row-major flat layout — never transpose buffer
  at scalar level without coordinated SIMD edits.

## Twin-update rules

Edit `psnr_hvs.c` -> walk these files first, before committing:

- `../../x86/psnr_hvs_avx2.c` — ADR-0159 lockstep contract.
- `../../arm64/psnr_hvs_neon.c` — ADR-0160 lockstep contract.
- `../../../test/test_psnr_hvs_avx2.c` +
  `../../../test/test_psnr_hvs_neon.c` — bit-exact regression
  tests, via [`simd_bitexact_test.h`](../../../test/simd_bitexact_test.h)
  harness (ADR-0245).

Re-run `test_psnr_hvs_avx2` and `test_psnr_hvs_neon` locally before
pushing — both gates part of fork's strict bit-exactness contract.

## Upstream-sync notes

`psnr_hvs.c` is **not** under Netflix/vmaf's master tree; shipped
to Netflix fork from Xiph upstream. Fork inherited Netflix's
import. On `/sync-upstream`:

- Netflix bumps Xiph (unlikely — psnr_hvs stable) -> check diff
  against AVX2 + NEON ports before merging.
- Xiph revises Daala 8×8 DCT (extremely unlikely — Daala project
  wound down) -> treat as ADR-required event: paired SIMD update
  mandatory.

## Governing ADRs

- [ADR-0159](../../../../../docs/adr/0159-psnr-hvs-avx2-bitexact.md) —
  psnr_hvs AVX2 DCT bit-exactness contract.
- [ADR-0160](../../../../../docs/adr/0160-psnr-hvs-neon-bitexact.md) —
  psnr_hvs NEON DCT bit-exactness contract.
