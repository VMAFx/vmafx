<!-- markdownlint-disable MD013 -->

# ADR-1196: Dispatch the SpEED dense matrix product through bit-exact AVX2 / AVX-512 kernels

- **Status**: Accepted
- **Date**: 2026-09-06
- **Deciders**: Lusoris
- **Tags**: simd, feature, performance, testing

## Context

A `perf record` profile of the CPU feature pipeline under the **default**
model (`vmaf_v1.0.16_3d0h`) put a single static function at 20.78 % of total
samples: `matrix_mul` in [`core/src/feature/speed.c`](../../core/src/feature/speed.c).
It is the dense single-precision product that drives SpEED's Householder QR
(`matrix_qr_decomposition`) and the `Q^T B` solve (`solve_linear_system`),
called twice per QR iteration on the 25 x 25 covariance system (block size 5,
so 25 elements per block), for both the reference and the distorted plane, for
every frame.

`speed.c` is part of the generic C library, which compiles at the x86-64
baseline. The i-k-j loop therefore vectorised to 128-bit `mulps` / `addps`
with runtime alias-check versioning and a scalar remainder: six vector steps
plus one scalar element to cover the 25-column inner loop, with a
store-to-load round trip on the destination row for every one of the 25
accumulation steps. The machine running the profile has AVX-512 (the same
profile shows `adm_decouple_avx512`, `cambi_avx2` and friends), so roughly
three quarters of the available vector width sat idle in the hottest function
of the default-model run.

The constraint that shapes the decision: the fork's numerical contract forbids
score drift. Any change here has to be provably bit-identical, not merely
"within tolerance".

## Decision

We add `speed_matmul_avx2` and `speed_matmul_avx512` as runtime-dispatched
twins of a `speed_matmul_scalar` reference, selected in
`speed_dispatch_cpu_kernel()` from `vmaf_get_cpu_flags()` exactly like the
existing `compute_cov_kernel_*` family, and threaded to the call sites through
a `speed_matmul_fn` function pointer on `SpeedState`.

The bit-exactness argument is structural, not statistical. In
`dst[i][j] += x[i][k] * y[k][j]` the `j` axis is an **output** index, not a
reduction axis. Widening `j` changes how many independent output elements are
updated per instruction; it cannot change the order in which any single output
element accumulates over `k`. The only remaining way to perturb rounding is
FMA contraction, so both kernels keep the multiply and the add as separate
intrinsics and each compiles in its own `-ffp-contract=off` static library —
the same carve-out the tree already applies to `x86_ssim_avx2`,
`x86_psnr_hvs_avx2` and `x86_float_adm_avx2`. Both kernels keep the
destination row in vector registers across the whole `k` loop, which is where
most of the speedup comes from; that is a memory-traffic change, not an
arithmetic one.

`core/test/test_speed_simd.c` gates the result with `memcmp` equality against
`speed_matmul_scalar` — a stricter contract than the relative-tolerance gate
its covariance-kernel siblings use, and the right one here because the
argument above admits no residual at all.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **Runtime-dispatched bit-exact AVX2 / AVX-512 twins (chosen)** | 1.20x measured on the default-model CPU run; scores bit-identical on all three dispatch paths; fits the existing `compute_cov_kernel_*` pattern | Two more TUs and two more meson carve-outs; a third copy of the tail logic | — |
| Apply the Householder reflector as a rank-1 update (`Q ← Q − 2v(vᵀQ)`) instead of a full 25x25x25 product | Turns the QR from O(n⁴) to O(n³) — asymptotically far larger than a vector-width win | Changes the summation order, so the scores move | Rejected outright: the mandate for this pass was bit-identical wins only. Written up as future work in [research digest 2030](../research/2030-speed-matmul-and-cambi-cpu-hot-path.md) so the option is not lost |
| Skip the known-identity leading block that `matrix_minor()` writes | Cheap to implement | Skipping `0.0f * y` terms is only value-identical, not bit-identical, once signed zeros reach `get_sign()` | Cannot be proven safe within this PR's evidence budget |
| Add `restrict` to `matrix_mul` and leave it to the compiler | One-line change, no new files | Removes the alias-check versioning but keeps the 128-bit baseline width and the per-`k` store/load round trip; a fraction of the win | Leaves most of the 20.78 % on the table |
| Build `speed.c` itself with `-mavx512f` | No new files at all | Unconditionally illegal on non-AVX-512 hosts and unresponsive to `--cpumask`; the fork dispatches at runtime by policy | Breaks the CPU-flag contract |
| Do nothing; publish the profile only | Zero risk | Leaves a measured, provable 20 % of the default-model run unclaimed | The win is provable, so "provable wins only" argues for landing it |

## Consequences

- **Positive**: the default-model CPU run is 1.20x faster on the measured
  fixture (see the digest for the raw numbers, the load average during each
  run, and the spread). The kernels also cover the rectangular
  25 x `num_blocks` solve, so the gain scales with frame size rather than
  being a fixed-size micro-optimisation.
- **Positive**: `speed_matmul_scalar` is now exported rather than static, so
  the parity test compares the SIMD twins against the *actual* production
  reference instead of a hand-copied duplicate — the failure mode
  `test_speed_simd.c` already documents for its covariance kernels.
- **Negative**: `matrix_mul` gained a function-pointer parameter that has to
  be threaded through `matrix_qr_decomposition` and `solve_linear_system`.
  Upstream Netflix has no such parameter, so a future `/sync-upstream` that
  touches those signatures will conflict. Recorded in
  [`docs/rebase-notes.md`](../rebase-notes.md).
- **Negative**: two more `-ffp-contract=off` static libraries in
  `core/src/meson.build`. The carve-out list is now six entries long and is
  starting to want a helper function.
- **Neutral / follow-ups**: `si_mat_mul` in
  [`speed_internal.c`](../../core/src/feature/speed_internal.c) is the
  ADR-0964 duplicate of the same loop, used by the host side of the GPU
  SpEED twins. It is deliberately **left scalar** in this PR: no GPU backend
  currently completes a scored run longer than one motion batch
  (`T-GPU-MOTION-FLUSH-DOUBLE-EMIT-2026-09-06` in
  [`docs/state.md`](../state.md)), so the benefit cannot be measured on this
  host, and an unmeasured change is not a win. Wiring it to the same
  dispatch is bit-exact by the identical argument whenever that bug clears.
- **Neutral / follow-ups**: with `matrix_mul` down from 20.78 % to 5.67 %,
  `preprocess_and_extract_cambi` is now the largest single entry at 28.17 %.
  The digest ranks its sub-costs and marks which of them are order-sensitive.

## Carbon / footprint

- **Runtime energy**: 1.20x fewer CPU-seconds for the same default-model
  score on the measured fixture (median 2.4820 s → 2.0674 s of `ru_utime +
  ru_stime` for 1440 frames of 576x324, 9 interleaved repetitions per side,
  1-minute load average 2.81).
  Measurement command and load averages are in the research digest.
- **Build-time**: two additional small TUs; no measurable change to a full
  `ninja -C core/build` (1340 targets).

## References

- Research digest: [`docs/research/2030-speed-matmul-and-cambi-cpu-hot-path.md`](../research/2030-speed-matmul-and-cambi-cpu-hot-path.md)
- [ADR-0964](0964-implement-speed-internal-and-wire-gpu-speed-extractors.md) — why `speed_internal.c` duplicates this math
- [ADR-0245](0245-simd-bitexact-test-harness.md) — the `simd_bitexact_test.h` harness the new tests use
- [ADR-1057](1057-revert-float-adm-simd-dispatch-neon-fma.md) — the existing `-ffp-contract=off` precedent
- Epic #1245 item 3 (profile the hot paths and land the wins that fall out)
