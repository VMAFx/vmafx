# Floating-point VIF native lint repair — 2026-09-08

## Finding and scope

The exact RC1 integration source `8100a2caa` failed native clang-tidy in
`core/src/feature/vif.c`: 48 diagnostics included five dead stores promoted
to errors. The configured cppcheck run independently identified those stores
and an unexercised legacy `vifdiff` entry point. The function bodies also
exceeded the existing 60-line limit.

This implementation repair follows ADR-0141 and ADR-1142; it changes no
metric definition, feature option, public libvmaf header or FFmpeg interface.
No new policy decision or ADR is needed. The required C `NULL` portability
bracket follows [ADR-1138](../adr/1138-c-translation-units-keep-null.md).

## Preservation contract

- Keep the ten-plane aligned allocation in its original order and preserve
  filter selection, precomputed coefficient copies, convolutions, decimation,
  statistic calls, float-to-double promotions and scale reduction order.
- Keep `compute_vif`, `vifdiff` and `apply_frame_differencing` externally linked;
  declare them in their own internal header instead of making them static.
- Keep temporal differencing after the pixel offset, previous-frame copies,
  first-frame placeholders, callback return semantics and printed scores.
- Retain optional stage dumps. Their old undefined writer and null numerator/
  denominator pointers were unusable; the repaired path writes actual planes
  and the two initialized reduced floats, documented in the VIF guide.

Helper extraction uses typed workspace members, eliminating the old char-to-
float cast suppressions. Debug-only border values are evaluated only where
consumed. New lifecycle tests exercise the exported temporal entry point,
EOF/error cleanup, rejected zero/negative/overflowing dimensions and odd-width
row strides. Allocation checks bound aligned strides and all slab multipliers
before allocation; valid geometry and callback behavior are preserved.
No Netflix assertion or threshold
is changed. The CPU ratchet must be tightened by its scoped writer after
measurement, with unselected baseline entries preserved.

## Validation

Run the focused native tests in a configured CPU build:

```bash
meson test -C build --no-rebuild --print-errorlogs \
  test_vif_lifecycle test_float_vif_coverage test_float_vif_min_dim test_vif_simd
python3 scripts/ci/tidy-ratchet.py --lane cpu --build-dir build \
  --only core/src/feature/vif.c --only core/test/test_vif_lifecycle.c
```

The local receipt retains configured analysis, old/new score comparisons,
debug compilation and dump checks with exact source and command hashes.
Component evidence does not establish full integrated lint, backend parity or
RC1 readiness. See [VIF stage-dump usage](../metrics/vif.md#developer-only-floating-point-stage-dumps).

### Retained sanitizer follow-up

The supplementary native AVX ASan/UBSan probe fails at 16×17 in both the
original `8100a2caa` source and the refactored source: a 32-byte load in
`convolution_f32_avx_s_1d_h_scanline` crosses the final workspace boundary.
The scalar-only sanitizer probe passes. The matched old/new failure is retained
as `T-VIF-AVX-SMALL-FRAME-2026-09-08` for a separate convolution-boundary
repair; this lint change does not claim complete sanitizer acceptance.

### Measured component results

- Four configured native test targets pass, including the new lifecycle and
  allocation-boundary cases. No large allocation is needed by boundary tests.
- Old/new output is byte-identical across 244 numeric cases plus three-frame
  temporal output, including odd dimensions, native/scalar masks, skip-scale,
  precomputed filters, kernel widths and gain limits.
- Normal/debug clang-tidy reports no findings in the touched C files. The
  scoped writer reduces VIF's CPU allowance from 48 to zero and preserves
  all unselected entries. Scoped cppcheck clears the touched files; the test
  harness context still reports an unchanged `test.c` const-pointer finding.
- Debug compilation/run produces 36 initialized files; the eight reduced
  outputs are four bytes each. Missing-directory and short-write controls
  report errors while preserving scores. Scalar ASan/UBSan passes 122 cases
  plus temporal output; native AVX retains the failure described above.
