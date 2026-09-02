# Research digest 0014 — `psnr_hvs` NEON sister port (T3-5-neon)

- **Status**: Active (captures the decision path for ADR-0160)
- **Related ADRs**: [ADR-0159](../adr/0159-psnr-hvs-avx2-bitexact.md)
  (AVX2, prior art), [ADR-0160](../adr/0160-psnr-hvs-neon-bitexact.md)
  (this port), [ADR-0139](../adr/0139-ssim-simd-bitexact-double.md)
  (float-accumulator rule), [ADR-0141](../adr/0141-touched-file-cleanup-rule.md)
  (NOLINT scope), [ADR-0145](../adr/0145-motion-v2-neon-bitexact.md)
  (precedent: NEON after AVX2 as separate PRs)
- **Related rebase-notes**: [§0052](../rebase-notes.md) covers
  both SIMD TUs jointly.

## The question

Can we match the AVX2 `psnr_hvs` port (ADR-0159) on aarch64 under
the same byte-for-byte Netflix-golden bit-exactness contract, and
if so, what is the smallest-surface diff to audit side-by-side
against the AVX2 TU?

## What we already had

- **Scalar reference**:
  [`third_party/xiph/psnr_hvs.c`](../../core/src/feature/third_party/xiph/psnr_hvs.c)
  — Xiph/Daala 8×8 integer DCT + contrast-sensitivity weighting
  masking, floats only for per-block means / variances / mask
  / error accumulators.
- **AVX2 sister**:
  [`x86/psnr_hvs_avx2.c`](../../core/src/feature/x86/psnr_hvs_avx2.c)
  — vectorizes the DCT 8 columns in parallel via `__m256i` (one
  register per row, 8× int32). Non-SIMD helpers (`load_block_and_means`,
  `compute_vars`, `compute_masks` minus the DCT call,
  `accumulate_error`, `calc_psnrhvs_avx2`) are plain scalar C,
  bit-exact to scalar by construction.
- **AVX2 bug fix mid-flight**: `accumulate_error` originally used
  a local `float ret = 0` accumulator, returning the per-block
  total to the caller which added it to the outer `ret`. IEEE-754
  non-associativity broke scalar summation order and drifted the
  Netflix golden by ~5.5e-5. Fix: thread `ret` by pointer. Lesson
  captured in rebase-notes §0052 invariant #3. **This port
  inherits the fixed signature.**
- **NEON precedent**:
  [`arm64/motion_v2_neon.c`](../../core/src/feature/arm64/motion_v2_neon.c)
  (ADR-0145) — confirmed the NEON-after-AVX2 split works well
  and the test harness layout (new `test_psnr_hvs_neon.c`
  arch-gated in `meson.build`) is reusable.

## Design axis — lane-width bridging (AVX2 8-wide → NEON 4-wide)

| Strategy | Code shape | Diff against AVX2 TU | Correctness risk | Picked? |
| --- | --- | --- | --- | --- |
| **Half-wide split: each 8-row register becomes `lo` (cols 0-3) + `hi` (cols 4-7); butterfly runs twice per DCT pass, transpose is 4×4×4 + block swap** | Mirror AVX2 line-for-line with `int32x4_t` substituted for `__m256i`; add one extra butterfly call per DCT pass | Almost one-to-one — each AVX2 butterfly call becomes two NEON calls, nothing else changes | Low — lane-wise SIMD is commutative over the pure-int32 butterfly, and the transpose rearranges lanes without touching values | **Yes** |
| Process 4 rows in parallel (not 8) | 4 rows × int32 = one register per row; run butterfly once per pass but on only 4 rows, then another 4 | Two 4-row passes through the transpose; higher register pressure | Same bit-exactness story but the diff vs AVX2 becomes "restructure pass layout" instead of "halve the lane count" | No — harder to audit against AVX2 |
| Inline 8×8 DCT scalar + only SIMD the mask / error loops | Doesn't touch DCT; still a speedup on mask reductions | Very different from AVX2; doesn't match the DCT-is-hot-kernel profile | Float SIMD reductions violate ADR-0139 without per-lane scalar tails | No — DCT is the hot path by a wide margin |
| SVE / SVE2 port | Length-agnostic; future-proof | Completely different SIMD style; `whilelt`, `svwhilelo` etc. | Very few aarch64 consumer cores ship SVE2 as of 2026-Q2; CI hardware doesn't have it | No — deferred; track for a later PR when hardware catches up |

**Chosen: half-wide split.** The AVX2 TU and NEON TU sit side-by-side
in reviewers' editors and diff trivially. Each AVX2
`od_bin_fdct8_simd(r0, ..., r7, ...)` becomes two NEON calls
(`..._simd(r0_lo, ..., r7_lo, ...)` then `..._simd(r0_hi, ..., r7_hi, ...)`).
The transpose is the only genuinely NEW piece — four 4×4 `vtrn*q_*`
stages + a top-right ↔ bottom-left block swap; the correctness
argument for the swap is written inline in `transpose8x8_s32`'s
doc comment.

## Design axis — aarch64-specific gotchas

### `vtrnq_s64` doesn't exist on aarch64

armv7 NEON had `vtrnq_s64` returning `int64x2x2_t` in one call.
aarch64 NEON split this into `vtrn1q_s64` and `vtrn2q_s64`
(returning single `int64x2_t` each). First compilation surfaced
the error; fix was mechanical. Documented inline in the
`transpose4x4_s32` doc comment so it doesn't re-appear on a
future rebase from a reference that happens to use armv7 forms.

### `#pragma STDC FP_CONTRACT OFF` is ignored by aarch64 GCC

Surfaces as `-Wunknown-pragmas` warning. Investigated:

- Without the pragma, does aarch64 GCC ever emit FMA fusion on
  the scalar float accumulators? Tested with `-O3` on the
  per-block loops: no `fmadd` / `fmla` emitted for the
  `err * csf` squared-then-summed pattern because each
  multiplication is in a separate expression from the
  addition (`ret += (err * csf[i][j]) * (err * csf[i][j])`).
  The inner `(a * b) * (a * b)` is a two-step multiply with no
  subsequent add in the same statement; `+= ...` is structurally
  a separate operation.
- aarch64 GCC's default FP-contract level is "off" across
  statements per the GCC manual (`-ffp-contract=off` is the
  default when no `-std=...` forces otherwise). The pragma is
  redundant on this compiler but harmless, and is kept for
  portability with toolchains that honour it (MSVC on ARM,
  clang).

### QEMU user-mode limits on 1080p × 10-bit

The 1080p 10-bit golden pairs (`checkerboard_1920_1080_10_3_{0,1,10}_0.yuv`)
segfault in `qemu-aarch64-static` when run under the
framework's threadpool. Verified this happens on master too
with any SIMD path enabled — it's a QEMU memory-map limit, not
a regression. Native-aarch64 CI + the Netflix CPU Golden Tests
gate will cover those cases.

## Verification plan

1. **Unit test**: `test_psnr_hvs_neon` with 5 subtests (3 random
   12-bit seeds + delta + constant). Must produce byte-identical
   DCT output to the scalar reference implementation duplicated
   inside the test TU.
2. **End-to-end 8-bit golden diff**:
   `src01_hrc00_576x324.yuv` vs `src01_hrc01_576x324.yuv` with
   `VMAF_CPU_MASK=0` (scalar) vs default (NEON). All
   `psnr_hvs_{y,cb,cr,psnr_hvs}` per-frame values must be
   byte-identical; only `<fyi fps>` differs (timing).
3. **CI native-aarch64**: the existing ubuntu-arm runner covers
   the 1080p × 10-bit pairs and the Netflix CPU Golden Tests
   required check.
4. **Clang-tidy**: zero warnings on the new NEON TU + scalar
   dispatch update (changed hunks only, per ADR-0141).

## Outcome

Landed as PR (pending) on branch `port/psnr-hvs-neon-t3-5`,
commit TBD. ADR-0160 accepted; rebase-notes §0052 carries both
SIMD sister TUs. ISA-parity matrix for psnr_hvs now covers
scalar + AVX2 + NEON; AVX-512 and SVE2 remain unscheduled.

## Open questions / follow-ups

- **AVX-512 `psnr_hvs`**: would extend AVX2 pattern (16 int32
  lanes per register, single butterfly call per pass). Not
  scheduled — AVX2 covers the x86 baseline and re-verifying
  bit-exactness against a different reduction tree is real work
  for no consumer benefit today.
- **SVE2 `psnr_hvs`**: revisit when CI ships SVE2 hardware
  routinely. Would exercise `whilelt` + predicated arith; a
  different auditability story vs. the fixed 4-wide NEON TU.
- **Perf micro-benchmark on native aarch64**: this PR did not
  ship a microbench (QEMU timings are not meaningful). Planned
  as a follow-up once the native-aarch64 runner is enrolled on
  shared CI; the AVX2 TU's 3.58× DCT speedup on x86 is the
  upper-bound reference.
