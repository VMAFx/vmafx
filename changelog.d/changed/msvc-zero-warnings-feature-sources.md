- The scoring sources compile without an MSVC warning (about 8,000 C4305 / C4244 /
  C4267 / C4334 sites across the CPU extractors, their SIMD twins and the CUDA
  host code): every implicit double-to-float, 64-to-32-bit and size_t-to-int
  conversion is written out, and a float table carries the `f` suffix only where
  the literal converts to the same bits. No score changes: each touched
  translation unit compiles to the same machine code as before (checked with
  GCC on x86-64, clang on aarch64 and the CUDA host objects), and the Netflix
  golden gate is unchanged.
