# Research-2046: Integer VIF AVX-512 stage cleanup

Date: 2026-09-08. Base: `6d228204fe83dfcff953bffd33d0693e553ee712`.
Scope: `core/src/feature/x86/vif_avx512.c` and its dedicated private test.

## Diagnosis and implementation

The configured Clang 22.1.8 profile reported oversized AVX-512 statistic and
subsample kernels. Mechanical cleanup alone left five function-size findings.
Private forced-inline helpers now separate residual logs, horizontal moments,
vertical moments and subsample row/block stages. Each accumulator preserves
its intrinsic operations, tap order, lane order, rounding and reduction.
The identical 8/16-bit horizontal bodies share one implementation while
retaining the original fused two-channel mean and three-channel energy loops.

The two ADR-0503 noinline/noclone 8-bit subsample block bodies remain unchanged
against the mechanical seed (normalized-body hashes are retained). They keep
register pressure isolated; no performance improvement is claimed without a
profile. Existing cited function-size exceptions at those two boundaries stay.
Two exact Cppcheck `constParameterPointer` exceptions preserve the
`VifPublicState *` callback type required by `VifState` dispatch. Buffer contents
are written through pointers; changing only these callbacks to pointer-to-const
would break that function-pointer contract. ADR-1138 retains `NULL` for Windows
and upstream C compatibility. No other new warning exception is introduced.

Dead initial accumulator writes are removed. Decimation index products widen
before multiplication. The intrinsic's `2048 * 17` constant uses `INT64_C`.
No filter, gain, depth, coefficient or output-schema policy changes.
No new ADR: implementation of the existing touched-file and bit-exact rules.
Alternatives: independent horizontal channel loops preserved bit-exact output
but caused an 8-bit slowdown. The final helpers restore original fused channel
scheduling. Relaxing lint or changing arithmetic would violate existing rules.

## Validation

The retained CPU image is
`sha256:4e2b0298690d730e5ccbd6bd62d4ba9a03935808d4a88549a84e6bf7562f577d`.
Runs use a new private output directory, read-only source, CPU affinity 28–31,
no network and no GPU. Evidence lives at
`.workingdir2/evidence/vif-avx512-lint-20260908`; build products remain in the
matching cache directory.

- Actual original/current AVX-512 libraries: 504 contexts, 1,008 frames,
  15,120 finite feature values bit-identical, including per-scale numerators
  and denominators. Inputs: 8/10/12/16 bit, 14 geometries from 17×17 through
  1920×1080, odd/tail widths, three patterns, gains 1/2/100.
- ASan + UBSan instrument both original and current AVX-512 objects. They
  compare full allocated storage bytes for 2,808 statistic and 702 subsample
  cases: every integer bit depth 8–16, all applicable scales, 13 widths
  9–1920, textured/checkerboard patterns and statistic gains 1/2/100.
- `test_integer_vif_avx512_stages`: 864 actual scalar/AVX-512 cases compare
  exact result bits and five final vertical rows including reflected padding.
  AVX-512 computes additional scratch lanes; the scalar comparison covers
  semantic rows, while the separate same-ISA control checks all storage bytes.
- Existing `test_integer_vif_log2` and `test_vif_skip_scale0` pass.

Reproduce the checked-in regression after configuring a CPU Meson build:

```bash
meson test -C build --print-errorlogs test_integer_vif_avx512_stages \
  test_integer_vif_log2 test_vif_skip_scale0
```

This is bounded component acceptance, not full `make lint`, full `make test`,
Netflix golden acceptance, a performance result, or release approval. Original
Netflix assertions remain unchanged. Final analyzer/ratchet and hook receipts
are recorded with the reviewed commit in the canonical evidence manifest.

## Scheduling controls and rejected candidate

Helper extraction initially let GCC fully unroll small horizontal loops when
the 8-bit filter specialized to 17 taps. The 8-bit statistic grew from 1,074
instructions / 18 ZMM stack references to 1,524 / 72. A control on the vertical
tap loop had no assembly effect and was rejected. GCC-only `unroll 1` on the
horizontal loops reduced this to 1,030 / 20, but separate per-channel loops
still regressed 8-bit wall time: seven alternating rounds with four warmup and
40 measured 1080p frames gave a 1.0321 new/original median ratio, versus 1.0044
for the same old binary against itself. That regression was not dismissed as
noise. The final correction restores the original fused channel loop structure
inside bounded helpers, retaining the horizontal GCC scheduling controls.
All rejected sources, assembly, raw timings and old/old controls are retained.
The final fused-loop timing uses the same seven alternating rounds and 40-frame
window. New/original ratios of medians for 8/10/12/16-bit are
1.0165/1.0255/1.0199/0.9865; paired medians are
1.0197/0.9968/1.0261/0.9918. Same-old paired medians are
1.0099/0.9994/1.0078/0.9933, with substantial outliers. The 8-bit nominal median
remains 1.65% slower; no performance-neutrality or speedup claim is made.
The fused 8-bit code has 977 instructions and 20 ZMM stack references, compared
with the original 1,074 and 18. This bounded sanity check recovers the original
scheduling structure and rejects the larger introduced regression; it is not a
portable benchmark. Explicit signed offset operands leave the measured 64-bit
machine code unchanged (retained byte comparison).

## Sources and invariants

- [ADR-0503](../adr/0503-vif-subsample-rd-8-loop-fission.md): preserved
  noinline boundaries and register-pressure rationale.
- [x86 invariants](../../core/src/feature/x86/AGENTS.md): tap/lane/ABI contract.
- [Human operator guide](../backends/x86/avx512.md#integer-vif-stage-checks).
- Installed Clang 22.1.8 and Cppcheck 2.21.1, the actual configured compile
  database, and original/current source/object hashes are retained in evidence.
