- **`ssim_accumulate_avx512` crashed on Windows hosts with AVX-512.**
  The Microsoft x64 ABI guarantees only 16-byte stack alignment, and
  its unwind contract prevents the `and $-64, %rsp` frame realignment
  gcc emits freely on SysV — but gcc's MinGW target still allocated
  64-byte-aligned `zmm` spill slots and addressed them as a fixed
  offset from `%rsp`. The resulting `vmovaps %zmm28,0x1a0(%rsp)` is
  correctly aligned only when the caller happens to leave `%rsp` at
  one residue in four, so `float_ssim` and `float_ms_ssim` faulted on
  most call paths with a general-protection fault — which Windows
  reports, misleadingly, as an access violation on a *read* of
  `0xFFFFFFFFFFFFFFFF` (a #GP supplies no faulting address, so the
  field is filled with -1). Present in MinGW gcc 14.2.1 and 16.2.0;
  neither `-mstackrealign` nor `-mprefer-vector-width=256` suppresses
  it. The spill is gone: the kernel now rebuilds its seven broadcast
  constants inside the 16-pixel block instead of holding them live
  across the loop, which keeps the function under 32 live vector
  values. Twelve extra instructions, no arithmetic change — scores
  over the 48-frame Netflix fixture are bit-identical at
  `--precision=max`, proven non-vacuously by a control perturbation
  that does move them. A whole-tree audit of every x86 SIMD
  translation unit found this to be the only affected function, and
  `scripts/ci/check-win64-stack-alignment.py` now disassembles the
  built Windows objects on the `Windows MinGW64` lane so the class
  cannot return silently — CI's Windows runners have no AVX-512, so
  the test leg never executes this path and could not have caught it.
  ADR-1254, research digest 2061.
