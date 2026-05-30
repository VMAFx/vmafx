# AGENTS.md — core/src/feature/third_party/xiph

Orientation for agents working on the Xiph third-party scalar
reference under the feature tree. Parent: [../../AGENTS.md](../../AGENTS.md).

## Scope

```text
third_party/xiph/
  psnr_hvs.c       # Xiph/Daala 8×8 integer DCT scalar reference for psnr_hvs
```

Provenance: `psnr_hvs.c` is from Xiph.Org / Daala (Copyright
2001–2012 Xiph.Org and contributors, BSD-3-Clause; see file header).
Netflix imported it; the fork preserves it byte-for-byte except
where ADR-0159 / ADR-0160 force lockstep updates with the SIMD
ports.

## Ground rules

- **Parent rules** apply (see [../../AGENTS.md](../../AGENTS.md) +
  [../../../AGENTS.md](../../../AGENTS.md)).
- **Preserve the Xiph license header verbatim.** This is the
  upstream-mirror file for the entire psnr_hvs feature; the
  third-party copyright notice is load-bearing for the BSD-3-Clause
  attribution chain.
- **`#pragma STDC FP_CONTRACT OFF` at TU level** — keep it. The
  scalar TU is the bit-exact reference for the AVX2 + NEON ports.

## Rebase-sensitive invariants

- **The 8×8 DCT butterfly block is the bit-exact reference for
  three TUs**:
  1. `psnr_hvs.c` (this TU — scalar)
  2. [`../../x86/psnr_hvs_avx2.c`](../../x86/psnr_hvs_avx2.c) (ADR-0159)
  3. [`../../arm64/psnr_hvs_neon.c`](../../arm64/psnr_hvs_neon.c) (ADR-0160)

  Any change to the butterfly here requires matched edits to both
  SIMD TUs in the **same PR**, plus a re-run of
  `test_psnr_hvs_{avx2,neon}` in [`../../../test/`](../../test/).
  Doing one without the others is a bit-exactness regression
  caught only by the cross-backend parity gate after a full run.

- **`accumulate_error()` threads `ret` by pointer.** ADR-0159
  burned this lesson into the AVX2 port and ADR-0160 mirrored it
  into NEON: a local-float accumulator inside the helper drifts
  the Netflix golden by ~5.5e-5. The scalar TU here is the
  reference for that threading shape — a refactor that
  introduces a local accumulator here cascades to a Netflix
  golden break.

- **The `od_coeff` int32 layout is fixed at 8×8 → flat
  64-element array.** AVX2 vectorises across 8 rows in parallel
  via `__m256i`; NEON splits each 8-column row into low/high
  halves (`int32x4_t × 2`). Both paths assume row-major flat
  layout — do **not** transpose the buffer at scalar level
  without coordinated SIMD edits.

## Twin-update rules

When editing `psnr_hvs.c`, walk these files before committing:

- `../../x86/psnr_hvs_avx2.c` — ADR-0159 lockstep contract.
- `../../arm64/psnr_hvs_neon.c` — ADR-0160 lockstep contract.
- `../../../test/test_psnr_hvs_avx2.c` +
  `../../../test/test_psnr_hvs_neon.c` — bit-exact regression
  tests using the [`simd_bitexact_test.h`](../../../test/simd_bitexact_test.h)
  harness (ADR-0245).

Re-run `test_psnr_hvs_avx2` and `test_psnr_hvs_neon` locally
before pushing — both gates are part of the fork's strict
bit-exactness contract.

## Upstream-sync notes

`psnr_hvs.c` is **not** under Netflix/vmaf's master tree; it
shipped to the Netflix fork from the Xiph upstream. The fork
inherited Netflix's import. On `/sync-upstream`:

- If Netflix bumps Xiph (unlikely — psnr_hvs is stable), check
  the diff against the AVX2 + NEON ports before merging.
- If Xiph itself revises the Daala 8×8 DCT (extremely unlikely
  — the Daala project is wound down), treat the change as an
  ADR-required event: a paired SIMD update is mandatory.

## Governing ADRs

- [ADR-0159](../../../../../docs/adr/0159-psnr-hvs-avx2-bitexact.md) —
  psnr_hvs AVX2 DCT bit-exactness contract.
- [ADR-0160](../../../../../docs/adr/0160-psnr-hvs-neon-bitexact.md) —
  psnr_hvs NEON DCT bit-exactness contract.
