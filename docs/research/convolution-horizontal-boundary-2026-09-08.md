# Horizontal SIMD convolution boundary repair

The native VIF sanitizer probe found a 32-byte read across the final plane
boundary in `convolution_f32_avx_s_1d_h_scanline`. Both the original integration
source and the scalar VIF cleanup reproduced it. The AVX-512 twin had the
same source-start/output-offset mismatch.

## Cause and repair

The wrappers compute `j_vec_end` as the first final scalar output. The helper
mistook that value for a count of source starts, then stored each vector at
`source_start + radius`. Its extra output lanes were subsequently overwritten
by the scalar tail. Ordinary score comparisons therefore missed illegal reads
from those discarded lanes.

The helper now processes `max(j_vec_end - radius, 0)` source starts: whole
vectors followed by masked per-tap loads and masked stores. The scalar split
is clamped for tiny widths. Each ISA shares one horizontal pass among normal,
squared and cross-product wrappers. Vertical filtering, per-pixel tap order,
AVX2 multiply/add operations, AVX-512 FMA and the original final scalar region
are preserved. The live SIMD filter limit is **17 taps** in `convolution.h`;
the previous package guide's 33-tap claim was stale.

This is an implementation repair under ADR-0141/1142 and the existing
ADR-0143/0504 arithmetic contracts. No new policy or API decision is introduced.
Moving the scalar tail to a newly rounded vector boundary was rejected because
it changes which AVX-512 outputs use FMA. Adding padding was rejected because
it hides the out-of-bounds operation and changes the caller allocation contract.

## Reproduction and validation

```sh
meson test -C build --print-errorlogs test_convolution_horizontal
```

The test is registered for x86 builds with assembly enabled and uses the
configured private kernel objects, including shared-library builds. Runtime
CPU checks skip unsupported ISAs explicitly. AVX-512 cases are compiled only
when that backend is enabled. The test covers all odd filter widths 1–17,
widths 1–49, heights 1/3/17 and normal/square/xy modes: 7,938 cases when both
ISAs execute. Every final buffer row ends exactly at its logical width;
intermediate row padding is NaN-poisoned. ASan detects reads of discarded
lanes that output-only assertions cannot detect.

Retained old/new probes compare every output byte on deliberately padded
inputs, separately for each ISA, under GCC 15.2 default, off and fast
contraction settings. All 7,938 cases match in each setting. The repaired tight
sweep passes ASan/UBSan with leak detection; original AVX2 16×1 and AVX-512
24×1 controls fail on out-of-bounds horizontal reads with a 17-tap filter.

The original VIF probe also passes: 244 native/scalar cases plus temporal
output match the retained original score bytes, and the previously failing
native run now passes ASan/UBSan with leak detection. Configured clang-tidy
measures zero diagnostics in both kernels and the new test. The scoped writer
leaves the already-zero baseline entries unchanged. Scoped cppcheck clears
these files but retains findings in the unchanged common test harness/header;
this is not a full-tree cppcheck pass.

Shared-library builds with float features disabled pass the new test;
AVX-512-disabled builds run the AVX2 subset; assembly-disabled builds omit
the test target. These checks use private build copies.

The private CPU-only container uses the retained RC1 image and a copied build;
no shared container or GPU job is modified. Commands, source hashes, raw logs,
comparison outputs and failed setup controls are retained under
`.workingdir2/evidence/convolution-horizontal-boundary-2026-09-08/`; disposable
binaries/builds live in the matching `.workingdir2/cache/` directory. These
component checks are not a complete release, full-tree sanitizer, Windows or
GPU acceptance claim. Netflix golden assertions and tolerances are unchanged.
