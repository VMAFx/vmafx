<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1057: Revert float-ADM SIMD dispatch wiring (PR #685) — NEON FMA divergence unfixable in scope

- **Status**: Superseded — follow-up landed on branch `fix/neon-fma-float-adm-dwt2`
  (FMA-safe `float_adm_dwt2_neon.c` dispatched + aarch64 scalar FMA-contract
  guard in `adm_tools.c` + `test_float_adm_simd` bit-exactness gate). See the
  "Follow-up resolution" note under Consequences.
- **Date**: 2026-06-06
- **Deciders**: Lusoris
- **Tags**: `simd`, `neon`, `float-adm`, `revert`, `correctness`

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

### Follow-up resolution (2026-06-27, branch `fix/neon-fma-float-adm-dwt2`)

The follow-up landed and closed the gap with an **exact** (not tolerance-bounded)
comparison. Two corrections to this ADR's original diagnosis emerged during the fix:

1. The FMA-safe NEON DWT2 kernel (`float_adm_dwt2_neon.c`, explicit `vmulq` + `vaddq`,
   meson lib `arm64_adm_dwt2_neon_lib` built `-ffp-contract=off`) and its runtime
   dispatch (`adm_dwt2_dispatch` in `adm.c`) were already present on master. The only
   missing piece was the bit-exactness test, plus a deeper root cause.
2. The ~1-ULP divergence was **not** solely the NEON intrinsics fusing. The *scalar*
   reference `adm_dwt2_s` / `adm_dwt2_lo_s` was itself being FMA-contracted on aarch64
   (GCC `-ffp-contract=fast`, Clang `on`), because `adm_tools.c` is built without
   `-ffp-contract=off` and its `#ifdef HAVE_CONFIG_H` include guard was dead (so
   `ARCH_AARCH64` never reached the TU). The dispatched scalar-fallback therefore
   diverged from the FMA-free NEON path. Fix: include `config.h` unconditionally and
   add an aarch64-only `-ffp-contract=off` guard (`#pragma clang fp contract(off)` +
   GCC `optimize` attribute) on the two scalar DWT2 functions. x86 is left byte-identical
   (`ARCH_AARCH64` undefined there; GCC x86 default is already `-ffp-contract=off`), so
   the Netflix golden gate is untouched.

Verified bit-exact (memcmp, 9 fixtures) on a real meson aarch64 cross-build under
`qemu-aarch64` with both GCC 16.1 and Clang 22; the pre-fix scalar diverged on all 9.
Gated by `core/test/test_float_adm_simd.c` in the `fast`/`simd` suites.

## References

- Introducing commit: `b1a6c0d62` (PR #685)
- Follow-up attempt: PR #690 (`rebase/pr690-onto-master`, `#pragma clang fp contract(off)`)
- ADR-0418: float-ADM double accumulator contract
- ADR-0214: GPU parity CI gate
- req: "fix or revert if forward fix is intractable" (user direction, 2026-06-06)
