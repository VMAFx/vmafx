- **docs / simd**: T3-9 (a) — `psnr_hvs` AVX-512 re-benched on Zen 5
  (post `9cd2a354`, full AVX-512: f/dq/cd/bw/vl/ifma/vbmi) and closed
  as **AVX2 ceiling** under the bench-first methodology. `perf record`
  on the Netflix normal pair (576×324, 48 frames) shows
  `calc_psnrhvs_avx2` at 78.42 % cycle share (scalar tail locked by
  ADR-0138/0139 bit-exactness) vs `od_bin_fdct8x8_avx2` at 14.82 %
  (the only piece AVX-512 could widen). Amdahl ceiling caps wall-clock
  improvement at 17.4 % (1.17× over AVX2); realistic 2-block batch
  projects 1.07–1.08× — well below the T3-9 1.3× ship gate.
  Re-validates [ADR-0180](docs/adr/0180-cpu-coverage-audit.md)'s
  2026-04-26 verdict empirically. Closure ADR
  [ADR-0350](docs/adr/0350-psnr-hvs-avx512-ceiling.md);
  ADR-0160 gains a status-update appendix pointing here. Sub-rows
  T3-9 (b) (`ssimulacra2_*` AVX-512+NEON drift audit) and (c)
  (`iqa_convolve` AVX-512) bench independently in their own
  follow-up PRs.
