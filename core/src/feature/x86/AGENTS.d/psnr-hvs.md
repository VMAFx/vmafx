---
paths:
  - core/src/feature/x86/psnr_hvs_avx2.c
  - core/src/feature/third_party/xiph/psnr_hvs.c
invariant: Butterfly block is byte-identical across three; AVX-512 closed as ceiling.
---
# PSNR-HVS DCT SIMD Implementation and Ceilings

| Group | TUs that move in lockstep |
| --- | --- |
| **PSNR-HVS DCT** (ADR-0159 + ADR-0350) | `psnr_hvs_avx2.c` + `../arm64/psnr_hvs_neon.c` + scalar `../third_party/xiph/psnr_hvs.c`. Butterfly block is byte-identical across three. **No `psnr_hvs_avx512.c` — AVX-512 closed as ceiling under T3-9 (a) per [ADR-0350](../../../../../docs/adr/0350-psnr-hvs-avx512-ceiling.md): `perf record` cycle share is 78.42 % scalar tail (locked by ADR-0138/0139 bit-exactness) vs 14.82 % DCT, capping 16-lane widening at 1.07–1.08× over AVX2 (Amdahl ceiling 1.17×) — well below T3-9's 1.3× ship gate.** Re-bench gate: any future upstream change to Xiph/Daala scalar that shifts per-block summation tree requires re-running [Research-0091 §7](../../../../../docs/research/0091-psnr-hvs-avx512-bench-2026-05-09.md) before claiming ceiling still holds. |

- [ADR-0159](../../../../../docs/adr/0159-psnr-hvs-avx2-bitexact.md) —
  `psnr_hvs` AVX2 DCT.
