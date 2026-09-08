# Integer ADM SIMD native lint cleanup — 2026-09-08

## Scope and finding

The release CPU profile reports 42 actionable Cppcheck findings in
`adm_avx2.c` and 43 in `adm_avx512.c`: unnecessarily broad temporary scopes,
read-only pointer targets and two fixed arrays per file. Both translation
units already have zero clang-tidy warnings. Fixing the first pointer layer
exposes six additional read-only filter aliases per twin.

The correction narrows declarations to their existing loops and branches and
qualifies local descriptors, source/filter aliases and fixed arrays. It does
not change a callback signature, buffer layout, arithmetic expression,
rounding, shift, clipping, reduction order or AVX-512 prefetch. Existing
ADR-0138/0139/0141 function-size exceptions remain unchanged.

## Decision and alternatives

This implements the existing touched-file policy (ADR-0141/1142), with no new
architecture or policy decision. Suppressing these diagnostics would retain
avoidable debt. Constifying callback parameters would broaden the change to
dispatch twins unnecessarily. Rewriting the kernels or altering their numeric
expressions is outside this declaration-only correction.

No user-visible behavior, public header or FFmpeg surface changes; the public
surface documentation rule is therefore exempt. Package invariants, changelog
and rebase notes accompany the source.

## Verification procedure

Configure the existing release CPU profile and run:

```sh
meson test -C build --print-errorlogs test_integer_adm_simd test_adm_dwt2_x86 \
  test_integer_adm_min_dim test_adm_angle_flag test_adm_csf_representable \
  test_adm_coverage test_adm_csf
python3 scripts/ci/tidy-ratchet.py --lane cpu --build-dir build \
  --only core/src/feature/x86/adm_avx2.c \
  --only core/src/feature/x86/adm_avx512.c --write
```

Same-ISA validation compares original and corrected static libraries on the
same CPU/toolchain. The unchanged integer ADM extractor runs with debug
per-scale scores and p-norm 2, 3 and 4. Inputs cover 8/10/12/16 bits, widths
17/31/32/33/63/64/65/127/128/129/256 with paired odd/even heights, textured,
checkerboard and constant patterns, and identical/distorted frame pairs.
AVX2 disables AVX-512 with CPU mask 48; AVX-512 uses mask 0. Both ISAs must
be available for this comparison. Round-trip `%.17g` scores are compared as
IEEE-754 bytes; timing metadata is excluded. These are same-ISA regression
checks, not a claim that all ISA scores match each other.

## Result

GCC 15.2 release CPU build and all seven listed tests pass. Both ISAs execute:
792 contexts / 1,584 frames produce 28,512 finite metric values, all bitwise
identical between original and corrected libraries (14,256 per ISA).
Clang-tidy 22.1.8 reports zero warnings and zero uncited suppressions for both
TUs; the guarded scoped writer leaves their existing zero baseline unchanged.
Cppcheck has no actionable findings in either touched TU. The focused project
also includes the unchanged scalar caller to resolve dispatch references;
its included headers still produce four unused-function findings, so that
project-wide Cppcheck exit remains nonzero. Branch-limit information is
retained rather than hidden. This is not a full-tree lint pass.

Exact local commands, source/library hashes, logs and numeric outputs are
retained under `.workingdir2/evidence/adm-simd-native-lint-2026-09-08/`.
No Netflix assertions, snapshots, thresholds or golden values are changed.
Focused validation does not establish full-tree lint, Windows/GPU behavior
or RC1 acceptance.
