<!-- markdownlint-disable MD038 MD060 -->
# Research digest — LLVM IR diff harness for bit-exact SIMD paths

**Date**: 2026-05-31
**Companion ADR**: [ADR-0918](../adr/0918-llvm-ir-diff-harness.md)
**Scope**: Fork-local. CPU-only (x86 AVX2). Diagnostic tooling.

## Question

PR #339 and PR #382 both took two review rounds to land because the
bit-exact SIMD gate (Netflix golden CPU JSON snapshot) tripped without
pointing at a cause. Both ended up being compiler-induced: clang's
`-ffp-contract=on` default fused `fmul + fadd` into FMA inside paths
that the ADR-0138 / ADR-0139 fixes had explicitly disclaimed via
`#pragma STDC FP_CONTRACT OFF`. How do we catch that class of bug
*before* the score gate fires?

## Hypothesis

If the compiler's floating-point semantics change between builds (FMA
emitted where there wasn't one before, instruction reassociation,
`-ffast-math` leak into a translation unit), the change is visible at
the LLVM IR level — strictly earlier than the asm and strictly earlier
than the score. A per-function IR snapshot diffed at build time
should:

1. Detect the failure class deterministically.
2. Name the affected function in the failure message.
3. Quantify the FMA delta (`ref=0 cur=9`) directly, so the dev
   reads the cause in one glance.

## Check (cheap falsifier)

Compiled the three seed source files locally with
`clang-22.1.5 -O2 -mavx2 -mfma -emit-llvm -S` and counted
`@llvm.fma.*` / `@llvm.fmuladd.*` references per function:

| File | Notable function | FMA count | Notes |
|---|---|---:|---|
| `psnr_hvs_avx2.c` | `calc_psnrhvs_avx2` | 0 | ADR-0138 pragma honored — bit-exact path |
| `psnr_hvs_avx2.c` | `od_bin_fdct8x8_avx2` | 0 | Same module, same pragma |
| `ms_ssim_decimate_avx2.c` | `ms_ssim_decimate_avx2` | 18 | Explicit `_mm256_fmadd_ps` intrinsics — intended |
| `ms_ssim_decimate_avx2.c` | `h_pass_scalar` (inlined helper) | 9 | `fmaf(x, y, acc)` in scalar fallback — intended |
| `ssimulacra2_avx2.c` | `ssimulacra2_ssim_map_avx2` | 0 | ADR-0139 invariant |
| `ssimulacra2_avx2.c` | `ssimulacra2_blur_plane_avx2` | 0 | ADR-0139 IIR blur — bit-exact |

Conclusion: **absolute FMA count is not the signal — *delta from
snapshot* is**. A function whose IR baseline is `FMAs=0` must stay
at 0; a function whose baseline is `FMAs=18` must stay at exactly 18
in exactly the same positions. Both shapes are caught by snapshot
diffing.

## Drift-detection sanity check

Mutated `testdata/ir-snapshots/h_pass_scalar.ll` (deleted 5 lines) and
re-ran `make ir-diff`:

```text
DRIFT    h_pass_scalar  FMAs: ref=9 cur=9
--- snapshot/h_pass_scalar.ll
+++ current/h_pass_scalar.ll
@@ -17,6 +17,12 @@
   %19 = icmp slt i32 %15, 0
   %20 = icmp slt i32 %16, 0
   %21 = icmp slt i32 %17, 0
+  %22 = insertelement <4 x i1> poison, i1 %21, i64 0
+  %23 = insertelement <4 x i1> %22, i1 %20, i64 1
+  %24 = insertelement <4 x i1> %23, i1 %19, i64 2
+  %25 = insertelement <4 x i1> %24, i1 %18, i64 3
+  %26 = select <4 x i1> %25, <4 x i32> %6, <4 x i32> zeroinitializer
+  %27 = insertelement <4 x i32> poison, i32 %17, i64 0
```

Exit code 1; harness names the function and prints the unified diff
inline. Hypothesis confirmed.

## Normalisation cost

LLVM IR contains a lot of non-semantic noise that would otherwise
cause false positives across clang minor-version bumps:

- `!dbg !NNN` / `!tbaa !NNN` / `!range !NNN` references on every
  instruction
- `!N = !{...}` metadata table at the bottom
- `attributes #N = { ... }` group definitions
- `source_filename`, `target triple`, `target datalayout` headers
- Per-version reordering of `dso_local` / `local_unnamed_addr`
  modifiers

The normaliser drops all of these and leaves only opcode-level IR.
Tested by re-running with `-g` vs without; normalised outputs are
identical.

## Why not `-ffp-contract=off` globally

Rejected — it disables `_mm256_fmadd_ps` emission everywhere including
the paths in `ms_ssim_decimate_avx2.c` that explicitly *want* FMAs (18
per call). The right behaviour is "FMA where the ADR says yes, no FMA
where the ADR says no", and the only mechanism for that fine-grained
control is per-translation-unit `#pragma STDC FP_CONTRACT OFF` plus a
snapshot gate to verify the pragma stuck.

## Why not run as a default CI gate

The harness adds N clang compiles per PR (one per SIMD source). Most
PRs don't touch SIMD. The PR template already nudges devs toward
`/cross-backend-diff` when SIMD is touched; the same template entry
will be extended (separate PR) to nudge `make ir-diff` for the same
trigger. Manual + advisory is cheaper than steady CI minutes for a
gate that fires only on compiler bumps.

## Open follow-ups

1. PR template: add `- [ ] If I touched SIMD, I ran `make ir-diff`` to
   the existing SIMD checklist row. Deferred to a follow-up PR so this
   one stays small.
2. AVX-512 / NEON: add entries as those bit-exact paths mature. The
   `cflags:` per-entry field is already in the YAML schema for it.
3. Container clang version pin: dev/Containerfile currently pulls
   whatever clang the base image ships. Once the harness has a few
   weeks of green runs, consider pinning to a tested version.
