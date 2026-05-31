<!-- markdownlint-disable MD013 MD060 -->
# ADR-0592: Remove float_vif_score weak HSACO stub now that real HIP kernel ships

- **Status**: Accepted
- **Date**: 2026-05-18
- **Deciders**: lusoris, Claude
- **Tags**: hip, build, cleanup

## Context

ADR-0379 (PR #1025, 2026-03-29) landed a real HIP kernel for `float_vif`
at `core/src/feature/hip/float_vif/float_vif_score.hip` and registered
it in `core/src/meson.build` under `hip_kernel_sources`. A later
follow-up (PR #1303, "extend hip_hsaco_stubs.c to cover all 13 weak
symbols") added a `__attribute__((weak))` 1-byte stub for
`float_vif_score_hsaco` to `core/src/feature/hip/hip_hsaco_stubs.c` so
the host link always succeeded — even on configurations where the real
HSACO blob did not build.

With `enable_hipcc=true` builds, the linker now sees two definitions of
`float_vif_score_hsaco`: the strong xxd-embedded blob from the real
`.hip` source and the weak 1-byte stub. The link rule resolves the
strong symbol correctly, but the weak/strong array-size mismatch
triggers `-Wlto-type-mismatch` warnings and leaves dead C source in the
stubs TU. The user direction was unambiguous: paraphrased, "no stubs
anywhere" for HIP HSACO symbols that have a real kernel behind them.

## Decision

Remove the `VMAF_HSACO_WEAK_STUB(float_vif_score_hsaco)` entry from
`core/src/feature/hip/hip_hsaco_stubs.c`. The real
`float_vif_score.hsaco` blob produced from
`core/src/feature/hip/float_vif/float_vif_score.hip` becomes the only
definition of the symbol, the LTO warning disappears, and the stub TU
shrinks by one line. The remaining 11 weak stubs (four ADM kernels, two
ssimulacra2 kernels, and five other HIP extractors whose `.hip` sources
are not yet portable) stay in place — each gets the same one-line
removal when its kernel becomes real.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Remove `VMAF_HSACO_WEAK_STUB(float_vif_score_hsaco)` (chosen) | Eliminates the LTO warning, removes dead code, matches user direction. | Linker now hard-requires the real `.hip` to build under `enable_hipcc=true`. | Selected — the failure mode is loud and immediate (link error during dev) rather than silent (init returning `-ENOSYS` at runtime). |
| Keep the weak stub indefinitely | No behaviour change; defence-in-depth. | Leaves dead code; LTO keeps warning; user directive ignored. | Rejected — user explicitly asked for stub removal. |
| Suppress the LTO warning with a pragma instead of removing the stub | Single-pragma fix; preserves the fallback. | The fallback is no longer needed (the real `.hip` has been building cleanly since PR #1025). | Rejected — masking dead-code warnings hides drift. |

## Consequences

- **Positive**: One fewer LTO warning, one fewer line of dead C, and the
  HSACO symbol table is unambiguous (one strong definition only).
- **Negative**: A future configuration that flips `enable_hip=true` on
  without `enable_hipcc=true` will fail to link with an unresolved
  reference to `float_vif_score_hsaco`. Acceptable — that combination
  has never been a supported product surface (the only reason to enable
  HIP without `hipcc` is build-system probing, which the host TU's
  `#ifdef HAVE_HIPCC` guard already handles).
- **Neutral / follow-ups**: As the remaining 11 weak stubs convert to
  real `.hip` kernels in future PRs, each gets the same one-line
  removal. `hip_hsaco_stubs.c` will eventually be deleted entirely
  when the last kernel ports.

## References

- [ADR-0379](0379-hip-float-vif.md) — original HIP float_vif port (PR #1025).
- [ADR-0536](0536-per-shot-bitrate-predicate-chain.md) — original stubs file design (ADM-only weak stubs).
- PR #1303 — extension that introduced the `float_vif_score_hsaco`
  weak stub (one of 13 weak symbols added at once).
- Source: `req` (paraphrased: "no stubs anywhere — replace the weak
  HSACO stub with the real kernel build path"; the user's PR
  assignment instructed direct removal of the
  `float_vif_score_hsaco` row from `hip_hsaco_stubs.c` after
  confirming the real kernel builds).
