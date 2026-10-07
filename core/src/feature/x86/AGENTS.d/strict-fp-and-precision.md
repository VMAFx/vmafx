---
paths:
  - core/src/meson.build
  - core/test/test_strict_fp_compiler_args.py
invariant: Every library of this directory takes vmaf_strict_fp_args; no FMA on load-bearing reductions.
---
# Strict FP and Floating-Point Precision Policies

- **No FMA on load-bearing reductions.** `#pragma STDC FP_CONTRACT
  OFF` set at TU level on every kernel participating in
  ADR-0138 (`iqa_convolve` widen-then-add) or ADR-0139 (SSIM
  per-lane scalar-double accumulate). Compiler's default
  `-ffp-contract=fast` would silently fuse `a + b * c`, break
  bit-identity vs scalar.

## Strict FP for every x86 SIMD library (ADR-1415)

Every library of this directory in `core/src/meson.build` takes
`vmaf_strict_fp_args`: general `x86_avx2` / `x86_avx512` ones and
named carve-outs alike. Reason: scalar references sit in baseline libraries
(no FMA exists there), kernels here end in plain-C tails under `-mfma` /
`-mavx512f`; icx contracts those under `-fp-model=precise` alone, GCC does
not. Fused multiply-add wanted -> write `_mm256_fmadd_ps` /
`_mm512_fmadd_ps` / `vmaf_fmaf_exact()`, never rely on contraction. New
library -> strict args + entry in `STRICT_TARGETS`
(`core/test/test_strict_fp_compiler_args.py`). Test TU that compiles
scalar reference itself -> `_simd_strict_fp_args` (icx default = fast
model; `test_integer_adm_simd` failed on icx without it). Check after
flag change: `objdump -d <object> | grep -c vfmadd` for GCC and icx
build; GCC objects must not change.

- [ADR-0918](../../../../../docs/adr/0918-llvm-ir-diff-harness.md) —
  LLVM IR diff harness. **Rebase-sensitive invariant**: any compiler
  bump (`dev/Containerfile` clang version, GitHub Actions runner image,
  `.github/workflows/*.yml` clang install lines) MUST be accompanied by
  local `make ir-diff` run. If snapshots under
  `testdata/ir-snapshots/` drift, never regenerate them blindly —
  investigate which intrinsic / FMA / FP-contract behaviour changed,
  confirm it does not break bit-exact contract that ADRs 0125
  / 0138 / 0139 froze. Only after confirming intent preserved (or
  ADRs updated) is `make ir-diff-update` appropriate, with
  justification in commit message.
