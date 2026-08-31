<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1057: Revert float-ADM SIMD dispatch wiring (PR #685) — NEON FMA divergence unfixable in scope

- **Status**: Superseded by PR #1161's scoped float-parity and Darwin integer-compatibility contracts
- **Date**: 2026-06-06
- **Deciders**: Lusoris
- **Tags**: `simd`, `neon`, `float-adm`, `integer-adm`, `darwin`, `revert`, `correctness`

## Context

PR #685 (`b1a6c0d62`) wired the `AdmSimdDispatch` table in `adm_tools.c` so that the
float-ADM AVX2/AVX-512/NEON kernels (previously compiled but never called) would actually
execute at runtime. It also added `core/test/test_float_adm_simd.c` with a
`test_float_adm_dwt2_bitexact` subtest.

The `test_float_adm_dwt2_bitexact` subtest fails on ARM CI with a 1-ULP FMA gap: the
NEON DWT2 kernel uses fused multiply-add by default, while the scalar reference under
`VMAF_CPU_MASK=0` does not. A follow-up carve-out attempt via `#pragma clang fp
contract(off)` (PR #690) was insufficient to suppress the FMA consistently across all
ARM toolchain configurations tested in CI.

The SIMD kernels remain compiled; they were simply never dispatched before PR #685.
Reverting restores that pre-existing state: the SIMD code is present in the tree as a
performance opportunity, but no dispatch logic calls it. ARM CI is unblocked immediately.
The forward fix — disabling FMA contraction reliably or restructuring the NEON kernel to
avoid the contraction entirely — requires a focused correctness investigation that is
out-of-scope for the current merge train.

## Decision

We will revert PR #685 (`b1a6c0d62`) in full. The revert removes:

- The `AdmSimdDispatch` table and `adm_prime_simd_dispatch()` from `adm_tools.c`/`.h`.
- The `adm_prime_simd_dispatch()` call added to `float_adm.c::init()`.
- `core/test/test_float_adm_simd.c` and its `meson.build` entries.
- The `changelog.d/perf/float-adm-simd-dispatch-wire.md` fragment.
- The `docs/rebase-notes.md` entry for the now-reverted PR.

This is acceptable per user direction: "fix or revert if forward fix is intractable."

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep #685 and suppress FMA per-TU | Preserves the performance improvement | `#pragma clang fp contract(off)` insufficient on all ARM toolchains; requires audit of every compiler version in CI matrix | Forward fix is intractable in current scope |
| Keep #685 and skip ARM test | Unblocks ARM CI immediately | Leaves a broken test silently hidden; masks a real numerical divergence | Violates correctness-first rule; would require a NOLINT without sufficient justification |
| Revert #685 fully | Restores known-good ARM CI; SIMD code still compiled (latent perf opportunity) | Silent perf gap vs dispatched SIMD; requires a follow-up PR to rewire correctly | Chosen — safest path given current scope |

## Consequences

- **Positive**: ARM CI test suite passes again; no numerical divergence in test gate.
- **Negative**: Float-ADM SIMD kernels revert to never-dispatched state; the silent
  performance gap that existed before PR #685 is restored.
- **Neutral / follow-ups**: A follow-up PR should rewire the dispatch with an explicit
  FMA-free DWT2 kernel for NEON (or a compile-time guard that provably disables
  contraction) and re-introduce the bit-exactness test with a tolerance-bounded rather
  than exact comparison if needed.

## Update (2026-06-27) — scalar-guard follow-up reverted

The follow-up attempted in PR #1060 took the opposite tack from this ADR: instead of
leaving the NEON kernel undispatched, it kept the FMA-free NEON DWT2 and forced the
**scalar** `adm_dwt2_s` / `adm_dwt2_lo_s` to `-ffp-contract=off` on aarch64 (so the two
sides would agree). That made the NEON-vs-scalar unit test pass, but it changed the
**scalar** ADM result on aarch64 — and the scalar path is what the akiyo
`disable_enhn_gain` quality tests exercise on ARM. The akiyo ADM score drifted from
`88.030463` to `88.030322`, failing the assertion on the ARM build matrix. The x86 golden
gate (D24) was unaffected (the guard was `ARCH_AARCH64`-gated), which is why the drift was
not caught before merge.

Per global rule #1 (golden scores are immutable; fix the code, never the assertion), the
PR #1060 scalar guard was reverted (this PR): `adm_tools.c` returns to its FMA-default
scalar arithmetic on aarch64, restoring `88.030463`, and `test_float_adm_simd.c` (which asserted
the now-reverted bit-exactness) was removed. The NEON DWT2 parity gap returns to the
"undispatched / FMA-free, not bit-exact with scalar" state this ADR describes. The
later PR #1161 investigation refined the lesson: changing every operation in
`adm_tools.c` was too broad, but the scalar DWT2 itself must have an explicit stable
contract shared by its SIMD twin. Any future change still requires the full ARM quality
suite, not only the x86 golden gate and QEMU unit parity.

## Update (2026-08-31) — scoped float parity plus Darwin integer compatibility

PR #1161 first removed the file-wide scalar `fp contract(off)` pragma because it changed
unrelated ADM reductions. The new bit-exact parity test then exposed Clang contracting
the scalar `adm_dwt2_s` accumulation while the guarded NEON twin stayed split. A first
compiler-matched attempt used `vfmaq_laneq_f32` / `fmaf` under Clang and passed that unit
test, but the full macOS Python suite returned `88.030459` instead of the immutable Darwin
`88.030322` value in three akiyo tests.

Treating that end-to-end failure as evidence about float DWT2 was incorrect. The affected
model consumes only integer ADM, integer motion, and integer VIF. C++23/C++26 libsvm
objects, libsvm compiled with contraction disabled, and scalar integer-ADM dispatch all
produced the same `88.030459` result. A source bisect instead located the change at
`a013c1410`: correcting `adm_dwt2_8_neon`'s first output column from three horizontal taps
to four made Linux AArch64 match the scalar reference, but the locked Darwin assertion had
recorded the older Apple AArch64 boundary result.

The float parity correction remains independently required and is deliberately narrow:

- `adm_dwt2_s` uses a function-scoped Clang `contract(off)` pragma and GCC
  `optimize("-ffp-contract=off")` attribute. The guard must not move back to file scope.
- `float_adm_dwt2_neon.c` retains its dedicated `-ffp-contract=off` static library and uses
  +0 initialization followed by four explicit `vmulq_laneq_f32` plus `vaddq_f32` steps in
  the vector loop. The initial addition preserves scalar-identical signed-zero behavior.
- The NEON scalar tail and horizontal bands use the same left-to-right split multiply/add
  sequence, including +0 initialization. No `vfmaq` or `fmaf` remains.

The exhaustive `test_float_adm_dwt2_neon` geometry/stride suite passes under Clang 22 and
GCC 16 AArch64 cross-builds through QEMU, including a signed-zero case that fails if the
NEON accumulator starts with tap 0 instead of +0. AArch64 disassembly shows separate
`fmul` / `fadd` instructions and no `fmla` in either implementation.

The integer decision keeps both correctness contracts explicit:

- `adm_dwt2_8_neon()` remains the universal four-tap kernel and stays bit-exact with the
  scalar reference on every platform.
- Production Apple AArch64 dispatch alone selects `adm_dwt2_8_neon_apple_legacy()`. The
  wrapper first runs the universal kernel, then overwrites only `j == 0` with the historical
  three-tap horizontal boundary calculation. Linux AArch64 continues to dispatch the
  universal kernel.
- `test_adm_dwt2_neon` proves universal scalar parity, proves the Apple wrapper against an
  independent legacy reference, and asserts that the compatibility result differs only in
  the first output column.

A forced-wrapper Clang 22 AArch64/QEMU end-to-end run returns
`integer_adm2_egl_1=0.95743264231661729` and `vmaf=88.030316780575973`; the latter differs
from the immutable Darwin value by only `0.000005219424027`, well inside its `places=4`
contract. The universal path remains `0.95743329964815937` / `88.030458811118052`.

Alternatives were rejected as follows: changing the Netflix assertion is globally
forbidden; disabling NEON still produces `88.030459` through the corrected scalar path;
restoring the three-tap defect in the universal kernel breaks Linux ARM scalar parity; and
an output-score offset would hide the feature-level cause. The named Apple dispatch wrapper
is the smallest compatibility boundary that preserves the universal kernel. No Netflix
golden assertion, snapshot, or parity tolerance changed.

## References

- Introducing commit: `b1a6c0d62` (PR #685)
- Follow-up attempt: PR #690 (`rebase/pr690-onto-master`, `#pragma clang fp contract(off)`)
- Integer first-column correction: `a013c1410` (PR #1134)
- ADR-0418: float-ADM double accumulator contract
- ADR-0214: GPU parity CI gate
- req: "fix or revert if forward fix is intractable" (user direction, 2026-06-06)
- req: "Never modify Netflix golden-data assertions" (project global rule)
