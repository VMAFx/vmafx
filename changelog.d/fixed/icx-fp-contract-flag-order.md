
- **FMA contraction was silently on in every strict-FP carve-out under the
  Intel compiler.** `-fp-model=precise` implies `-ffp-contract=on`, so
  spelling the pair as `['-ffp-contract=off'] + _x86_simd_strict_fp_extra`
  re-enabled the very contraction the list exists to disable. Twelve
  carve-outs were affected — six AVX2, three AVX-512 and the three
  scalar-reference libraries — and `core/test/meson.build` built the SIMD
  tests' own copies of the scalar references the same way, which is why an
  earlier attempt to reorder only the source side broke
  `test_ssimulacra2_simd`: it moved one side of every comparison. Both files
  now put `-fp-model=precise` first and `-ffp-contract=off` last. Under icx
  2026.0 this takes `ssimulacra2` from `host-isa=-38.37695186087862` against
  `scalar=-38.376932759633718` to bit-identical, and the full suite is green
  at 150 tests. On GCC and Clang the icx list is empty, so nothing changes.
  Closes `T-ICX-FP-CONTRACT-FLAG-ORDER-2026-09-07`.
