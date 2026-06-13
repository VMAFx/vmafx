# ADR-0145: `motion_v2` NEON SIMD — bit-exact to scalar

- **Status**: Accepted
- **Date**: 2026-04-23
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: simd, neon, motion, bit-exact, performance

## Context

The fork's `motion_v2` feature extractor shipped with scalar C +
AVX2 + AVX-512 paths but no NEON variant
(`core/src/feature/integer_motion_v2.c` dispatches only under
`#if ARCH_X86`). Aarch64 users ran the scalar reference, which
gives ~1× throughput of the AVX2 path on x86 and is the slowest
feature in the metric pipeline for that ISA. T3-4 in
`.workingdir2/BACKLOG.md` scoped the gap-fill.

The `motion_v2` algorithm is a two-phase integer pipeline over
`prev - cur`:

- **Phase 1**: 5-row vertical convolve with a truncated Gaussian
  filter `{3571, 16004, 26386, 16004, 3571}` (sums to 65532); the
  result is rounded + arithmetic-right-shifted by `bpc` to produce
  an `int32_t` `y_row`. Mirror reflection at top/bottom boundaries.
- **Phase 2**: 5-tap horizontal convolve on `y_row`, rounded +
  arithmetic-right-shifted by 16 to `int32_t`, absolute-valued, and
  accumulated per row and across rows into a `uint64_t` SAD score.
  Mirror reflection at left/right boundaries.

There are two entry points — `motion_score_pipeline_8` for 8-bit
inputs (shift by 8) and `motion_score_pipeline_16` for 10 / 12-bit
inputs (shift by `bpc`).

Two subtle numerical invariants surfaced while porting:

1. **Arithmetic vs logical shift**. The scalar reference uses C's
   `>>` on `int64_t` / `int32_t`, which GCC and Clang implement as
   arithmetic (sign-preserving) shift. The fork's existing AVX2
   variant, however, uses `_mm256_srlv_epi64` (*logical* shift); for
   negative accumulator values this produces a different low-32-bit
   result than scalar. AVX2 has not been re-audited against scalar
   post-port, and the existing bit-exact test likely does not
   exercise negative tails. The NEON port must match scalar
   (arithmetic shift).
2. **Runtime-variable shift by `bpc`**. NEON has no direct
   equivalent to `_mm256_srlv_epi64`; the arithmetic form is
   `vshlq_s64(v, vneg_s64(bpc_vec))`, where a negative shift count
   is interpreted as a right shift.

## Decision

We will port `motion_v2` to NEON in a new fork-local TU
[`core/src/feature/arm64/motion_v2_neon.c`](../../core/src/feature/arm64/motion_v2_neon.c)
with a companion header
[`motion_v2_neon.h`](../../core/src/feature/arm64/motion_v2_neon.h).
The NEON path:

1. Matches the scalar reference `motion_score_pipeline_{8,16}` in
   [`integer_motion_v2.c`](../../core/src/feature/integer_motion_v2.c)
   byte-for-byte. Verified via QEMU user-mode diff
   (cpumask=0 vs cpumask=255) on `src01_hrc00/01_576x324.yuv`.
2. Uses arithmetic right-shift throughout (`vshrq_n_s64(v, 16)`
   for the known Phase-2 shift by 16, and
   `vshlq_s64(v, -(int64_t)bpc)` for the Phase-1 shift by `bpc` in
   the 16-bit pipeline). This deliberately diverges from the
   fork's AVX2 variant's `_mm256_srlv_epi64` (logical) — see
   §Consequences for the follow-up audit.
3. Dispatches via `VMAF_ARM_CPU_FLAG_NEON` in
   [`integer_motion_v2.c`](../../core/src/feature/integer_motion_v2.c)
   under `#if ARCH_AARCH64`, mirroring the existing AVX2 /
   AVX-512 dispatch blocks.
4. Is registered in the `arm64_sources` list of
   [`core/src/meson.build`](../../core/src/meson.build).
5. Keeps every function below the ADR-0141
   `readability-function-size` budget (60 post-preprocessor lines)
   via small `static inline` helpers — `x_conv_block4_neon`,
   `x_conv_edge_one_col`, `y_conv_row_step{8,16}_neon`,
   `y_conv_col_scalar{8,16}`. Zero clang-tidy warnings on the
   touched file under the fork's strictest profile. No NOLINT.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **Scalar-only on aarch64** (status quo) | No new code to maintain | Aarch64 users pay the scalar cost; widens the ISA-parity gap in the metrics matrix | Rejected — the gap-fill is the point of T3-4 |
| **Port AVX2's `_mm256_srlv_epi64` pattern via `vshlq_u64`** (logical) | Mirrors AVX2 byte-for-byte | Would ship the latent AVX2 bug on NEON too; NEON path would diverge from scalar on negative-diff pixels, breaking the `/cross-backend-diff` contract | Rejected — scalar is the bit-exactness ground truth, not AVX2 |
| **Use `/add-simd-path --kernel-spec=…` scaffold** | Reuses the SIMD DX framework (ADR-0140) | The `widen-add-f32-f64` and `per-lane-scalar-double` kernel-specs target float arithmetic; `motion_v2` is pure integer. No existing macro fits; the skill would emit a `--kernel-spec=none` stub that still requires hand-written integer NEON | Skipped — hand-written port matches the integer algorithm more cleanly |
| **Shared helpers in a new `simd_dx_integer.h`** | Factors out the 5-row × 5-tap Gaussian mirror-convolve pattern for future integer NEON ports (`motion_v1`, `psnr_hvs`) | Zero other consumers today; premature abstraction | Deferred — revisit once a second integer-NEON consumer appears |

## Consequences

- **Positive**:
  - Aarch64 `motion_v2` now has a SIMD path; closes one of the
    last ISA-parity gaps in the metrics matrix.
  - `vif_mirror_tap` patterns proved useful; extracted helpers
    (`x_conv_block4_neon`, `y_conv_row_step{8,16}_neon`) keep the
    driver functions under ADR-0141's 60-line budget without
    compromising SIMD throughput (everything is `static inline`).
  - Bit-exact with the scalar reference under QEMU, so
    `/cross-backend-diff` passes on aarch64 without tolerance
    relaxation.
- **Negative**:
  - Surfaces an AVX2-vs-scalar divergence on `srlv_epi64`: the
    NEON path now matches scalar but the AVX2 path does not (for
    negative diffs). Netflix's existing bit-exact test likely
    doesn't exercise this, since the AVX2 path has been in the
    tree without a failing gate. Deferred follow-up: re-audit AVX2
    against scalar with a negative-diff test pair; if a delta
    surfaces, switch AVX2 to an arithmetic-shift emulation.
  - Integer NEON with 4-wide lanes vs AVX2's 8-wide: aarch64
    throughput will be roughly half the x86 AVX2 win (~2× over
    scalar rather than ~4×). Still a substantial improvement.
- **Neutral / follow-ups**:
  - Add
    [`docs/rebase-notes.md`](../rebase-notes.md) entry
    describing the arithmetic-shift invariant so a future upstream
    sync doesn't silently revert it.
  - Add
    [`core/src/feature/AGENTS.md`](../../core/src/feature/AGENTS.md)
    rebase-sensitive invariant (V-axis mirror `- 1`, shift
    arithmetic, 4-lane stride).
  - Queue follow-up T-N: audit AVX2 `srlv_epi64` path against
    scalar on a `motion_v2` corpus with negative-diff pixels; if
    divergence confirmed, open an AVX2 correctness PR.

## References

- Upstream scalar reference:
  [`integer_motion_v2.c`](../../core/src/feature/integer_motion_v2.c)
  (`motion_score_pipeline_{8,16}`, committed upstream
  `dae6c1a0`).
- Fork-local AVX2 variant:
  [`x86/motion_v2_avx2.c`](../../core/src/feature/x86/motion_v2_avx2.c).
- Related ADRs:
  [ADR-0140](0140-simd-dx-framework.md) — SIMD DX framework
  (kernel-specs for float-arithmetic patterns; integer patterns
  are out of scope there);
  [ADR-0141](0141-touched-file-cleanup-rule.md) — touched-file
  lint-clean rule, enforced here with no NOLINT.
- Backlog item: `.workingdir2/BACKLOG.md` T3-4 (gap-fill Step 2).
- Source: user direction 2026-04-23 (motion bundle popup;
  NEON-solo selected).
