---
paths:
  - core/test/float_ssim_order_frame.h
  - core/test/test_cuda_float_ssim_order.c
  - core/test/test_cuda_float_ssim_exact_contract.py
invariant: Shared float_ssim_order_frame.h is byte-identical across twin lanes and never edited; tests assert exact CPU bits.
---
<!-- markdownlint-disable MD013 -->
# Shared fixture: float_ssim_order_frame.h

- One 64x64 8-bit 4:2:0 pair; CPU `float_ssim` = `0xb4e2b622`, per-block sum
  of same terms = `0xb4e2b621` (`T-GPU-FLOAT-SSIM-FRAME-SUM-ORDER-2026-10-02`).
- Added byte-identical by CUDA, HIP, SYCL lanes (git merges identical adds).
  NEVER edit, reformat or regenerate: sha256 pinned in
  `test_cuda_float_ssim_exact_contract.py`. One include per twin test, never
  second copy of data.
- User: `test_cuda_float_ssim_order.c` (CPU bits == header's constant, twin
  bits == CPU bits, with + without `enable_lcs`).
