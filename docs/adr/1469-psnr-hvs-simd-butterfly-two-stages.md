<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1469: the `psnr_hvs` SIMD butterfly is two functions, cut at the same statement in the AVX2 and the NEON twin

- **Status**: Accepted
- **Date**: 2026-10-02
- **Deciders**: lusoris
- **Tags**: `simd`, `avx2`, `neon`, `psnr-hvs`, `bit-exact`, `hiss`, `rc3`, `fork-local`

## Context

`core/src/feature/x86/psnr_hvs_avx2.c` and
`core/src/feature/arm64/psnr_hvs_neon.c` each held the 8-point DCT butterfly of
the scalar `od_bin_fdct8()` as one function, `od_bin_fdct8_simd()`, more than
80 lines long (82 in the NEON file). [ADR-0159](0159-psnr-hvs-avx2-bitexact.md) kept it in one piece so that it
reads line by line against the scalar source, and the function carried a
`readability-function-size` suppression that cites
[ADR-0141](0141-touched-file-cleanup-rule.md) §2. The repository's HISS-04
limit is 60 lines per function; both files had the finding in the baseline.

Fixing the masking threshold of `calc_psnrhvs_neon()`
(`T-PSNR-HVS-NEON-NOT-SCALAR-BITS-2026-10-02`) touches the NEON file. A touched
file loses its baseline exemptions, and the standing rule is that the audit's
`-touched-debt-delta-reason` is not used
([ADR-1298](1298-hiss-audit-merge-touched-scope.md),
[ADR-1289](1289-hip-ssimulacra2-host-helper-split.md)). So the function has to
meet the limit, in a file whose kernels must keep returning the scalar's bits.

## Decision

The butterfly is cut once. `od_bin_fdct8_even_simd()` holds the first 21
statements of the scalar (the +1/-1 butterflies and the even half, after which
`t0`, `t2`, `t4` and `t6` are final) and `od_bin_fdct8_odd_simd()` the remaining
13 (the embedded 4-point type-IV DST). Every statement keeps the scalar's text,
order and names; the working values travel in `od_fdct8_state`.
`od_bin_fdct8_simd()` keeps its signature, applies the input permutation and
calls the two. Both twins are cut at the same statement, and neither carries a
function-size suppression any more. This replaces the "network must stay
together" clause of ADR-0159 for these two files; the scalar source is not
touched.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep one function and pass `-touched-debt-delta-reason` | Smallest diff | Suppresses the gate rather than satisfying it | Out of bounds (ADR-1298, ADR-1289) |
| Cut only the NEON twin, the file the fix touches | The x86 object stays byte-identical | The twins stop reading side by side, against the twin-update rule of `core/src/feature/arm64/AGENTS.md`; the AVX2 finding stays in the baseline | The two files move together |
| Work on the fields directly (`s->t1 = ...`), no local copies | 17 lines shorter per stage | The statements are no longer the scalar's text | The side-by-side read is the reason the function existed in one piece |
| Reach 60 lines by deleting comments and blank lines | No structural change | Removes the constants' explanations, which mirror the scalar's | Trades the audit aid for a line count |
| One helper per embedded transform (four or five) | Shorter functions | More boundaries and more state passed than the limit needs | One cut is enough |

## Consequences

- **Positive**: both files report no HISS-04 finding and no clang-tidy
  warning, without a suppression.
- **Positive**: values are unchanged. `test_psnr_hvs_avx2` and
  `test_psnr_hvs_neon` compare the DCT with the scalar bit for bit,
  `test_psnr_hvs_dispatch_invariance` the whole extractor path, and the
  `psnr_hvs` reports of nine fixtures are identical before and after the cut
  on x86-64 and aarch64, with GCC and with clang.
- **Neutral**: the machine code is equivalent, not always identical. aarch64
  clang: identical apart from the line numbers of the `assert()` calls.
  aarch64 GCC: three loads name base and index register in the other order.
  x86-64 clang: commuted operands of `vpaddd`. x86-64 GCC: a larger stack
  frame for the DCT (`0x180` bytes, `0x160` before). No cost measured on
  x86-64: `psnr_hvs` takes 23.6 ms per 1920x1080 frame before and after with
  GCC, 29.0 and 28.4 ms with clang (user CPU time, minimum of seven
  interleaved runs of 48 frames on one core of a Ryzen 9 9950X3D).
- **Negative**: reading the network against the scalar now means reading two
  functions; the scalar stays one.
- **Neutral / follow-ups**: a change to `od_bin_fdct8()` upstream is mirrored
  in both stages of both twins; the cut stays where the even outputs are
  final.

## References

- [ADR-0159](0159-psnr-hvs-avx2-bitexact.md),
  [ADR-0160](0160-psnr-hvs-neon-bitexact.md) — the bit-exact ports.
- [ADR-0141](0141-touched-file-cleanup-rule.md),
  [ADR-0278](0278-t7-5-nolint-sweep.md) — the touched-file rule and the
  suppression this removes.
- [ADR-1289](1289-hip-ssimulacra2-host-helper-split.md),
  [ADR-1298](1298-hiss-audit-merge-touched-scope.md) — the escape hatch is
  not used.
- `docs/state.md`: `T-PSNR-HVS-NEON-NOT-SCALAR-BITS-2026-10-02`.
- Source: `req` (coordinator, 2026-10-02): "ADR-0160 is Accepted and frozen: a new ADR supersedes the false claim if the design changes; a pure bug fix cites it."
