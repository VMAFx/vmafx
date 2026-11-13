---
paths:
  - core/src/feature/arm64/moment_sve2.c
  - core/src/feature/arm64/ssimulacra2_sve2.c
invariant: SVE2 SIMD ports portability, build gating, and fallback guarantees.
area: simd-and-gpu
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# SVE2 SIMD Ports Portability Invariant

- **SVE2 SIMD ports (T7-38, PR #201 MERGED, ADR-0213)**
  — SSIMULACRA 2 PTLR + IIR-blur SVE2; same bit-exact contract
  as existing NEON ports per
  [ADR-0161](../../../../docs/adr/0161-ssimulacra2-simd-bitexact.md)
  / [ADR-0162](../../../../docs/adr/0162-ssimulacra2-iir-blur-simd.md)
  / [ADR-0163](../../../../docs/adr/0163-ssimulacra2-ptlr-simd.md).
