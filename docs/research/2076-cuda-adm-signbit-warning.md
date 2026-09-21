# Research-2076: CUDA ADM sign-bit rounding without conversion warnings

## Scope

CUDA 13.4 reports diagnostic `#68-D` for each integer-ADM expression that
stores `1u << 31` in `int32_t`:

- `integer_adm/adm_csf.cu`, once in the scales 1-3 CSF path;
- `integer_adm/adm_cm.cu`, once in the fused ADM path and once in the fused
  AIM path.

This is not permission to correct the historical ADM rounding. ADR-0155 keeps
the negative rounding term until Netflix resolves Netflix/vmaf#955 and moves
its golden data. The goal is solely to express the already-required value
without an out-of-range unsigned-to-signed conversion.

## Reproducer

A focused CUDA 13.4 fatbin build with `DEVICE_CODE` defined reports the exact
warning twice for `adm_cm.cu` and once for `adm_csf.cu`:

```text
warning #68-D: integer conversion resulted in a change of sign
const int32_t add_bef_shift_flt = (1u << (shift_flt - 1));
```

The deterministic pass/fail signal is the CUDA compiler output: both
translation units must compile successfully with no `warning` diagnostic.

## Semantics

`shift_flt` is fixed at 32, so the old initializer computes unsigned
`0x80000000` and converts it to signed `int32_t`. The supported toolchains
materialize `-2147483648`; every subsequent `(product + bias) >> 32` therefore
uses the Netflix-compatible negative bias documented by ADR-0155.

`INT32_MIN` names that same required value directly. It removes the conversion
that NVCC diagnoses while leaving the addition, right shift, accumulator type,
and output bits unchanged.

## Alternatives

| Option | Result | Decision |
| --- | --- | --- |
| Use `INT32_MIN` | Preserves the exact signed value and removes the conversion | Chosen |
| Add an explicit cast | Hides the diagnostic around the same conversion and obscures intent | Rejected |
| Widen to `uint32_t` or `int64_t` | Corrects Netflix#955 and changes golden scores | Rejected by ADR-0155 |
| Suppress diagnostic `#68-D` | Leaves warning-producing code in place | Rejected |

No new ADR is needed: this is the only implementation spelling that both
preserves ADR-0155 and removes the compiler diagnostic.

## Validation

- CUDA 13.4 focused fatbin compilation: no warnings for either translation
  unit after the change.
- The isolated constant-spelling change produced a byte-identical
  `adm_cm.fatbin`, SHA-256
  `16f1606e4404a6eca8d42ea7b518941f18f6ac24e294f82b05572f25a2bc2087`.
- The isolated constant-spelling change produced a byte-identical
  `adm_csf.fatbin`, SHA-256
  `9c6d8fea1b143e5004760bae1c89de0fa9070c6776159d8e3d82905313bc940f`.
- The touched-file HISS audit exposed five oversized kernels. Splitting them
  into forced-inline helpers changes final binary layout, but
  `standardsctl audit -base docs/hiss-21-readme-cleanup` reports every touched
  file clean with no ratchet growth.
- The final CUDA build produces output exactly identical to the base build for
  `test_cuda_adm_parity`, `test_cuda_adm_parity_large`,
  `test_cuda_adm_wide_rounding`, and `test_cuda_adm_small_border` (empty
  stdout/stderr diffs for all four). The first three tests pass in both builds.
- `test_cuda_adm_small_border` exposes a pre-existing required-gate defect in
  both builds: CPU `adm3=0.97618931`, CUDA `adm3=0.97606859`, delta
  `1.21e-4` against the `1.00e-4` bound. Instrumentation isolated the entire
  drift to scale-0 AIM: the dispatched AVX2/AVX-512 CPU path reports zero,
  while scalar CPU and CUDA both report `0.00024139`. The x86 SIMD contrast
  threshold omits scalar's narrowing `(int16_t)` conversion for the centre
  taps. With the six sign-extension operations from commit `28552bd55`
  temporarily applied, default-dispatch CPU and CUDA differ by only
  `2.37e-11` for AIM and `3.03e-8` for adm3, and the test passes. The exact
  fix, standalone SIMD regression, and documentation are already carried by
  open PR #1507; this branch deliberately does not duplicate them. The
  threshold remains unchanged, and #1507 must land before or beneath this
  branch in the merge train.

No new runtime test file is warranted: the focused compile is the direct seam
for this diagnostic, while the four existing ADM tests prove that the final
refactor did not change runtime output relative to the base. The small-border
failure remains a real gate defect rather than a waiver for this batch.

## Delivery declarations

- Human-facing documentation: no user-visible behavior or public surface
  changes.
- ADR: no new decision; ADR-0155 remains authoritative.
- Rebase impact: preserve the direct `INT32_MIN` spelling at all three CUDA
  sites; recorded in `docs/rebase-notes.md` and the CUDA subtree instructions.
