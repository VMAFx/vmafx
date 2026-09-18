<!-- markdownlint-disable MD013 -->

# ADR-1256: Dispatch CAMBI's spatial-mask row SIMD kernels only where they measurably beat scalar

- **Status**: Proposed
- **Date**: 2026-09-18
- **Deciders**: Lusoris
- **Tags**: simd, avx2, avx512, neon, cambi, perf, upstream-port, fork-local

## Context

Upstream Netflix/vmaf `86da14d03` vectorizes CAMBI's two spatial-mask row
kernels, `compute_dp_row` and `compute_mask_row`, with AVX2 and dispatches them
on any AVX2 CPU. The fork keeps AVX-512 and NEON twins of CAMBI kernels, so a
port brings three ISAs, not one.

Measured against the release build's own scalar code (see
[Research-2062](../research/2062-cambi-spatial-mask-simd.md)), two things did
not hold up. Upstream's AVX2 dp row is slower than scalar when built with Clang
(0.74x) or icx (0.64x), the compiler of the published container; only GCC shows
a gain (1.23x). And on aarch64 both GCC and Clang already auto-vectorize the
scalar mask row into the same NEON instructions a hand kernel would use, so a
NEON mask row removes nothing. A twin that is present is not automatically a
twin worth dispatching.

## Decision

A spatial-mask row kernel is dispatched on an ISA only when it beats the scalar
code on a measured run, or, where no hardware is available to time it (NEON,
verified under `qemu-aarch64`), only when its instruction count per column
clearly drops below the compiled scalar loop. Under that rule we dispatch
`compute_dp_row` on AVX2, AVX-512 and NEON, and `compute_mask_row` on AVX2 and
AVX-512. `compute_mask_row_neon` stays in the tree, parity-tested, and
undispatched. The AVX2 dp row is the fork's decoupled-carry variant, which
keeps a single add on the loop-carried chain and is 1.81x (Clang, icx) to 2.25x
(GCC) faster than scalar, instead of upstream's. The AVX2 mask row compares
with a 2^31 bias so it is exact for every input, not only for box sums under
2^31. The scalar kernels become the non-static reference functions declared in
`cambi.h`, as upstream made them, and remain the default binding.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Measured dispatch, fork kernels (**chosen**) | Every dispatched path is faster on every compiler measured; all paths bit-exact for any input | Diverges from upstream's AVX2 source; one NEON twin is built but unused | — |
| Port upstream's AVX2 verbatim, wire everything | Smallest diff against upstream; symmetric dispatch across ISAs | AVX2 dp row regresses under Clang and icx; NEON mask row adds nothing; signed compare only correct for the reachable input range | Ships a slowdown to the published binaries |
| Port upstream verbatim but dispatch its dp row only for GCC builds | Keeps upstream source | Compiler-dependent dispatch is fragile and untestable in one build | The decoupled carry is faster than upstream under GCC too (2.25x vs 1.23x) |
| Drop `compute_mask_row_neon` entirely | No unused code | Breaks the package's twin rule; loses a ready kernel if compilers stop vectorizing the scalar loop | Keeping it parity-tested costs little |

## Consequences

- **Positive**: CAMBI's spatial mask runs 1.5–3.2x faster per kernel on x86
  and the dp row is vectorized on aarch64, with byte-identical scores. The
  parity test checks the full uint32 input domain, including the
  signed/unsigned boundary upstream's compare would get wrong.
- **Negative**: `cambi_avx2.c` no longer matches upstream's text for these two
  functions; a future upstream sync must keep the fork's versions (recorded in
  `docs/rebase-notes.md` and the x86 / arm64 `AGENTS.md`).
- **Neutral / follow-ups**: timings are from one AMD Zen 5 host; Intel AVX-512
  and real aarch64 hardware are unmeasured. The older AVX-512 / NEON CAMBI
  kernels (derivative row, c-values row, range updates) have been undispatched
  since the upstream c-values layout port; re-wiring or retiring them is a
  separate decision under this same measure-first rule.

## References

- Upstream commit `86da14d03`, "feature/cambi: AVX2 vectorize spatial-mask dp
  row and mask row" (Netflix/vmaf).
- Task brief (paraphrased): port upstream's AVX2 dp/mask rows with credit, add
  AVX-512 and NEON twins, wire them like the existing CAMBI SIMD dispatch, and
  prove bit-exactness and unchanged scores.
- Follow-up direction from the coordinating session (paraphrased): benchmark
  each function against scalar before wiring dispatch; dispatch a SIMD variant
  only where it beats scalar on a measured run; wire NEON only if the op count
  clearly drops; leave a slower variant unwired or drop it.
- [Research-2062](../research/2062-cambi-spatial-mask-simd.md)
