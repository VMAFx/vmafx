### AVX-512 and NEON ports for CAMBI `calculate_c_values_row`

`calculate_c_values_row_avx2` now has AVX-512 (16-lane i32 gather) and NEON
(vectorised mask-zero skip, scalar per-pixel inner loop) siblings. The AVX-512
path is dispatched automatically on Skylake-X+ / Zen 4 hosts; the NEON path
on aarch64. Output is bit-identical to the scalar reference (integer pipeline,
no float reduction tree). See ADR-0452.
