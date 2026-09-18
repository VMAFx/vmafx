<!-- markdownlint-disable MD060 -->
# ADR-1258: Keep the fork 64-bit only; retire the resurrected i686 lane

- **Status**: Accepted, Supersedes [ADR-0151](0151-i686-ci-netflix-1481.md)
- **Date**: 2026-09-18
- **Deciders**: Lusoris
- **Tags**: build, ci, x86, netflix-upstream, fork-local

## Context

ADR-0691 (Accepted, 2026-05-28) decided the fork is 64-bit only and removed
the `Build — Ubuntu i686 gcc (CPU, no-asm)` lane from
`.github/workflows/libvmaf-build-matrix.yml` (commit `9aa008e70`, #1564). The
same day, `384d97d03`, the squash-merge of the `libvmaf/` → `core/` rename
that "merge-resolves 42 master commits", brought the row back. No ADR asked
for it. ADR-0728 (`bfd4c436b`, #52) landed after that merge and restated the
removal in `docs/development/deprecations.md` ("32-bit x86 is unsupported; the
fork is 64-bit only"), but it changed no workflow, so the restored row went
unnoticed. It has run on every PR since, compile-only with
`-Denable_asm=false` as ADR-0151 had set it up, and was later renamed
`Ubuntu i686 gcc`. ADR-1234 then built preflight's `m32` stage on the
assumption that the lane was deliberate.

Checking an i686 build with asm enabled on 2026-09-18 showed what that lane
was and was not covering:

- Asm builds failed on three x86-64-only intrinsics: `_mm_extract_epi64` in
  `adm_avx2.c` and `adm_avx512.c`, and `_mm_cvtsi128_si64` in `psnr_avx2.c`.
  That is Netflix#1481's cause, never fixed because the lane pinned the
  workaround.
- The lane never ran a test. Run natively, 2 tests fail without asm and 7
  with it. GCC and Clang use the x87 FPU on 32-bit x86, and its 80-bit
  intermediates make scalar float code round differently from x86-64 and
  from the SIMD kernels. On the Netflix golden pair, 11 of 15 metrics differ
  from x86-64 by up to 5.1e-5. With `-msse2 -mfpmath=sse` all tests pass and
  every feature metric matches x86-64.

So the lane looked like 32-bit coverage while testing none of it. The
maintainer was first asked to make 32-bit a tested platform with an SSE2
floor, and chose that. When the recorded 64-bit-only decision and the
accidental resurrection were then put in front of them, they chose to keep
the fork 64-bit only (see References). The popup named that decision
ADR-0728, because the deprecation notice cites it; the decision is
ADR-0691's.

## Decision

We keep ADR-0691's decision that the fork is 64-bit only, which ADR-0728
restated, and remove what contradicts it:

- The `Ubuntu i686 gcc` matrix row, its dependency step and its
  `matrix.i686` conditions are removed from `libvmaf-build-matrix.yml`.
- `scripts/dev/preflight.sh` drops its `m32` stage, which only existed to
  mirror that lane. This changes the stage list ADR-1234 chose; the rest of
  ADR-1234 stands.
- The three x86-64-only intrinsic calls are replaced with 32-bit-safe forms
  (`extract_epi64_128()` beside the existing `extract_epi64()` fallback, and
  `_mm_storel_epi64`). They cost nothing on x86-64 and keep the x86 sources
  portable, but they are hygiene, not a support promise.
- No 32-bit float policy is set. With nothing building 32-bit, an SSE2 floor
  would be a rule nobody checks.
- `build-aux/i686-linux-gnu.ini` stays, unreferenced by CI, because historical
  documents link to it.

ADR-0151's status becomes Superseded by ADR-1258. ADR-0691 decided to drop
its lane first, without marking it superseded, but that removal was undone
the same day; the lane ADR-0151 set up ran until this ADR removed it.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Keep 64-bit only; retire the lane and `m32` (chosen) | Matches ADR-0691, the container-first posture (ADR-0686) and what is shipped; no CI or preflight time on an unsupported target | 32-bit portability regressions go unnoticed | — |
| Support 32-bit again: SSE2 floor, asm on, tests in CI | Real 32-bit coverage; scores match x86-64 | Reverses ADR-0691 for a target the project ships nothing for; one more lane to keep green | The maintainer chose 64-bit only once the recorded decision was on the table |
| Keep the resurrected lane as it was (compile-only, no asm) | No change | Keeps an accidental lane that implies support it does not test, and contradicts ADR-0691 | Coverage that tests nothing is misleading |
| Remove the lane but keep `m32` | Still catches 32-bit width assumptions | Enforces a portability property the project does not promise | Same reason as the lane |

## Consequences

- **Positive**: CI, preflight and the docs agree with ADR-0691 again. The
  x86 SIMD sources no longer carry Netflix#1481's cause.
- **Negative**: nothing notices if a change breaks 32-bit compilation or
  32-bit scores. Anyone building 32-bit x86 themselves is on their own,
  including x87 score drift unless they build with `-msse2 -mfpmath=sse`.
- **Neutral / follow-ups**: the other lanes ADR-0691 and ADR-0689 removed,
  which the same merge restored, and the lane removals ADR-0728 describes but
  never made, are recorded in ADR-1259.

## References

- [ADR-0691](0691-vmafx-drop-legacy-build-paths.md) (64-bit-only decision
  upheld), [ADR-0728](0728-native-build-sunset.md) (restated it in docs only),
  [ADR-0151](0151-i686-ci-netflix-1481.md) (superseded by this ADR),
  [ADR-1234](1234-local-preflight-gate.md) (stage list amended),
  [ADR-0686](0686-vmafx-rebrand-aggressive-modernization.md).
- Commits `9aa008e70` (#1564, ADR-0691, lane removed), `384d97d03` (layout
  rename merge, lane resurrected) and `bfd4c436b` (#52, ADR-0728, docs only).
- Netflix/vmaf issue [#1481](https://github.com/Netflix/vmaf/issues/1481).
- Popup, 2026-09-18, first round, question 1: "Require SSE2 everywhere
  (Recommended)"; question 2: "Default options + run tests (Recommended)".
  Both were asked without ADR-0728's decision in view.
- Popup, 2026-09-18, second round, question 1 (32-bit x86, with ADR-0728 and
  the resurrection shown): "Stay 64-bit only (ADR-0728)".
