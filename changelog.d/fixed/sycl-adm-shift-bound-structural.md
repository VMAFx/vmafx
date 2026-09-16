
- **The SYCL integer-ADM bit-scan proves its own shift bound.** Both
  `get_best15_from32` equivalents in `core/src/feature/sycl/integer_adm_sycl.cpp`
  computed the shift amount as `17 - (31 - n)` and relied on a comment to argue
  it could not go negative. clang-tidy's `core.BitwiseShift` disagreed at
  **error** level, which fails the SYCL lane's build. The scan is now seeded at
  `n = 15` — the floor the `abs_oh >= 32768` guard already guarantees — `v` is
  masked to its 17 real bits, the shift is written directly as `n - 14`, and
  `n` is clamped to its true ceiling of 31. Each step is a no-op on real input:
  470,035 guarded values (exhaustive across the low boundary plus 400,000
  random) produce an identical `n`, with the shift landing in exactly [1, 17].
  The SYCL ADM parity tests confirm the kernel stays bit-exact with its CPU and
  CUDA / HIP twins.
