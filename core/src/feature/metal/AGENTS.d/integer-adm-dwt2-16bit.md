---
paths:
  - core/src/feature/metal/integer_adm.metal
  - core/src/feature/metal/integer_adm_metal.mm
invariant: Integer ADM 16-bit vertical DWT sums use 64 bits.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# Integer ADM 16-bit vertical DWT sums in 64 bits (T-GPU-ADM-DWT2-16BIT-INT32-OVERFLOW-2026-09-18)

- `integer_adm.metal`, raw vertical DWT kernel: `accum_lo` / `accum_hi` are
  `long`. 16-bit low-pass sum passes `INT_MAX` once three samples reach
  42456; CPU forms it in int64 (`adm_dwt2_vpass16_tap4()`).
- Unverified on Apple silicon (no hardware in fleet); change mirrors
  CUDA and HIP twins, which are measured byte-identical.
