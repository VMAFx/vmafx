- **AVX-512 follow-up audit sweep (T3-9, 2026-05-09)
  ([Research-0089](../docs/research/0089-avx512-audit-sweep-2026-05-09.md);
  audit blocks appended to
  [ADR-0138](../docs/adr/0138-iqa-convolve-avx2-bitexact-double.md),
  [ADR-0161](../docs/adr/0161-ssimulacra2-simd-bitexact.md),
  [ADR-0162](../docs/adr/0162-ssimulacra2-iir-blur-simd.md),
  [ADR-0163](../docs/adr/0163-ssimulacra2-ptlr-simd.md)).**
  Bench-first re-audit of the three deferred AVX-512 candidates on
  AMD Ryzen 9 9950X3D (Zen 5). Methodology per BACKLOG: ship if 16-lane
  AVX-512 >=1.3x AVX2 on the Netflix normal pair, otherwise document
  as ADR-0180-style ceiling. Results: (a) `psnr_hvs` AVX-512 stays
  closed as AVX2 ceiling — theoretical headroom 1.11x, re-affirms
  ADR-0180 verdict on a faster machine; (b) `ssimulacra2` AVX-512
  re-affirmed at 1.461x (full PTLR + IIR + scoring pipeline,
  byte-identical to AVX2 across all 48 frames at full IEEE-754
  precision); (c) `iqa_convolve` AVX-512 re-affirmed at 1.300x via
  `float_ssim` and 1.173x via `float_ms_ssim` (the latter is sub-
  threshold but explained by 5-scale outer-loop amortisation at the
  smallest two scales — matches ADR-0138 §"Follow-up" prediction).
  No new SIMD code shipped; 0/2 SHIP candidates, 2/2 AUDIT-PASS,
  1/1 DOCUMENT (ceiling). Closes T3-9 BACKLOG row + former T3-10 +
  former T7-31. Reproducer in Research-0089.
