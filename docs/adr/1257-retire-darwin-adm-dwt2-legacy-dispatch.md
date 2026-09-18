<!-- markdownlint-disable MD013 MD041 MD060 -->

# ADR-1257: Retire the Darwin three-tap integer-ADM DWT2 compatibility dispatch

- **Status**: Accepted
- **Date**: 2026-09-18
- **Deciders**: Lusoris
- **Tags**: `simd`, `neon`, `integer-adm`, `darwin`, `testing`, `upstream-sync`

## Context

`adm_dwt2_8_neon` once summed only three of the four Daubechies-2 taps for
the first output column of every DWT2 row. `a013c1410` (PR #1134) corrected
the kernel, which made Linux AArch64 bit-exact with the scalar reference.
The macOS Python lane then reported `88.030459` against the fork-added
Darwin branch `88.030322 if _IS_DARWIN else 88.030463` in three akiyo
assertions of `python/test/vmafexec_test.py`. That Darwin value had been
recorded from the defective kernel. The 2026-08-31 update to
[ADR-1057](1057-revert-float-adm-simd-dispatch-neon-fma.md) (PR #1161)
kept the Darwin value by adding `adm_dwt2_8_neon_apple_legacy()`. The
wrapper ran the corrected kernel and then overwrote column 0 with the
three-tap result, and Apple AArch64 production dispatch used it through an
`#if defined(__APPLE__)` branch in `integer_adm.c`.

Netflix/vmaf `cba9343ed` (2026-09-14) fixes the same dropped tap upstream.
Upstream therefore has no three-tap result on any platform. Its assertion
for these tests is `88.030463` without a Darwin branch. Keeping the wrapper
would leave the fork as the only build that reproduces the defect, on one
operating system, through a code path that no other platform exercises.

## Decision

Apple AArch64 dispatches the universal `adm_dwt2_8_neon()` like every other
AArch64 platform, and `adm_dwt2_8_neon_apple_legacy()` is deleted together
with its declaration, its dispatch branch and its test. The three akiyo
assertions drop their Darwin branch and assert the platform-independent
`88.030463` at their existing `places=4`. The four-tap kernel measures
`88.030458811118052` on Linux AArch64 NEON under QEMU, identical to the x86
scalar and SIMD result. That is 4.2e-6 from the asserted value. ADR-1057's
forced-wrapper measurement puts the Darwin libm contribution for this model
at about 5e-6, so macOS stays well inside the 5e-5 `places=4` window. The
other two `_IS_DARWIN` branches in the same file (`132.732…` and `129.473…`)
come from [ADR-0418](0418-macos-test-recal-post-vif-sync.md)'s libm
difference in `svm_predict`, not from the DWT, and are unchanged.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep the Apple wrapper (status quo) | No assertion changes | Reproduces a fixed defect on one OS forever; diverges from upstream, which fixed it; a second NEON code path that only macOS CI runs | The compatibility value has no upstream counterpart left to be compatible with |
| Keep a Darwin branch but record the four-tap Darwin value | Pins the exact macOS number | The branch no longer distinguishes anything at `places=4`: Linux AArch64, x86 and upstream all agree with `88.030463` | Adds a platform branch with no measurable purpose |
| Restore the three-tap rule in the universal kernel | Single code path | Breaks scalar bit-exactness on every AArch64 platform (ADR-0138/0139) | Forbidden by the bit-exactness contract |

## Consequences

- **Positive**: one integer-ADM NEON code path on every AArch64 platform, which
  `test_adm_dwt2_neon` covers completely. macOS scores match Linux AArch64,
  x86 and upstream Netflix for the akiyo fixtures.
- **Negative**: macOS integer-ADM scores move by about 1.4e-4 VMAF on inputs
  where the first DWT2 column matters. This is the same correction every
  other platform took in PR #1134.
- **Neutral / follow-ups**: `python/test/vmafexec_test.py` is protected by
  the repository's golden-file edit guard. A maintainer applies the
  three-line assertion change by hand, in the same PR as the dispatch
  change. The macOS Python lane is the confirming run, because no macOS host
  was available when the change was made. ADR-1057's status now points
  here for its Darwin integer-compatibility contract. Its float-ADM parity
  contract is unaffected.

## References

- [ADR-1057](1057-revert-float-adm-simd-dispatch-neon-fma.md), update of
  2026-08-31: introduced the Apple compatibility wrapper.
- [ADR-0418](0418-macos-test-recal-post-vif-sync.md): the per-platform
  expected values that remain.
- `a013c1410` (PR #1134): the four-tap correction. `195f88a22` (PR #1161):
  the wrapper.
- Netflix/vmaf `cba9343ed` (feature/adm/arm64: fix dropped filter tap in
  adm_dwt2_8_neon column 0).
- Source: user decision relayed by the coordinating session on 2026-09-18.
  The triage offered to retire the Darwin legacy path, and the answer was
  "RETIRE".
