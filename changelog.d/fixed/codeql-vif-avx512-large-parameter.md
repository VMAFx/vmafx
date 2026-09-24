- **CodeQL `cpp/large-parameter` alerts 1108–1112 in `core/src/feature/x86/vif_avx512.c`
  resolved.** Internal stage helpers extracted under Research-2046 took
  128-byte `VifPair512` and 256-byte `VifTaps8` aggregates by value. Under
  System V AMD64 and Windows x64 ABIs, aggregates larger than 64 bytes cannot
  be passed in vector registers, forcing caller stack allocation and copies
  when out of line. The helpers now take aggregate inputs via `const *`. Under
  `-O3`, inlining folds pointer dereferences into registers without spilling;
  the emitted machine code across `vif_subsample_rd_*_avx512` and
  `vif_statistic_*_avx512` is instruction-identical under GCC 16 x86-64
  System V ABI `-O3` (measured by disassembly diff of the compiled object).
  The host structural stack-alignment scan reports 0 violations; definitive
  Win64 acceptance remains the hosted MinGW build. The public ABI
  (`vif_avx512.h`) is unchanged. Tri-way bit-exactness across scalar, AVX2, and
  AVX-512 is proven by `test_integer_vif_avx512_stages` with a red-capable
  perturbation check, sanitizers, and a clean host structural stack scan.
  Research-2098.
