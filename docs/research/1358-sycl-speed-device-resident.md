<!-- markdownlint-disable MD013 MD060 -->
# Research-1358: Device-resident SpEED on SYCL — what the device must do to match the CPU bit for bit

- **Status**: Active
- **Workstream**: [ADR-1358](../adr/1358-sycl-speed-device-resident-linalg.md), [ADR-0567](../adr/0567-speed-chroma-temporal-real-gpu.md), [ADR-0220](../adr/0220-sycl-fp64-fallback.md)
- **Last updated**: 2026-09-29

## Question

`speed_chroma_sycl` and `speed_temporal_sycl` ran the pixel work on the device
but the anti-alias filter, the 16x decimation, the 25x25 eigenvalue problem, the
QR factorisation and the Q^T B multiply on the host, with 11 and 9 `queue.wait()`
calls per frame. The maintainer asked for no host round trips ("there shouldnt
be any gpu cpu rountrips"). Two questions followed: can the whole chain run on
an fp64-less device (ADR-0220), and can it then match the CPU reference
(`speed.c`) exactly, including the regular/singular decision?

## Sources

- `core/src/feature/speed.c` (CPU reference), `speed_internal.c` (host helpers
  the GPU twins used), `vif_tools.c` + `common/convolution_avx.c` +
  `common/convolution_internal.h` (the filter the CPU actually runs on x86).
- The previous investigation of a one-ulp SYCL/CPU drift in the means,
  `T-SYCL-SPEED-CHROMA-V-PARITY-2026-09-16` in `docs/state.md`, which ended by
  moving the means back to the host because the ulp "is not yet explained".
- icpx 2026.1 (`vmaf-dev-mcp` image), Arc B580 (`bmg-g21`, fp64) and UHD 770
  (`adl-s`, no fp64), WSL2 Level Zero.

## Findings

1. **`-fp-model=precise` does not stop FMA contraction in SYCL kernels.** A
   probe of `a * b + c` written as one expression differed from the host in
   1 188 795 of 4 194 304 random cases (28%); the same product in a named
   temporary matched. `-ffp-contract=off` placed after `-fp-model=precise`
   (the order `core/src/meson.build` already documents for the x86 strict
   libraries) gives 0 mismatches, and it survives the meson split of compile
   and link. `#pragma clang fp contract(off)` has no effect on device code.
2. **Device fp32 `/` and `sqrt` are not correctly rounded by default.** 28%
   of random divisions and 8% of square roots differ from the host.
   `-foffload-fp32-prec-div/-sqrt` fix both only when passed to the final
   link: with the meson split (flags on the object, plain `-fsycl` link) the
   mismatch is unchanged, so a per-TU flag cannot deliver them, and a link
   flag would change every SYCL extractor. `sycl::ext::intel::math::fdiv_rn`
   and `fsqrt_rn` are exact under the split and fp64-free (they run on the
   UHD 770), but cost about 15x a hardware division: the eigenvalue kernel
   went from 0.38 ms to 5.7 ms per frame. The pipeline uses them only as the
   fallback of an exact-residual rounding step (next item).
3. **Fast correctly rounded division and square root.** Take the hardware
   approximation, refine it once with an FMA residual, then look at the two
   adjacent floats around it. Their exact residuals (`fma(-q, b, a)`,
   `fma(-s, s, x)`) prove whether the true value lies between them; if so,
   the nearer one (ties to even; a square root is never a midpoint) is the
   correctly rounded result, otherwise the software routine answers. 0
   mismatches on 16 777 216 operands spanning exponents -126..127 on both
   GPUs; the eigenvalue kernel is back to about 0.96 ms per frame on the B580.
   This is also the unexplained ulp of the 2026-09-16 means investigation:
   the device divided the mean's sum by the count with a non-correctly-rounded
   division.
4. **The host's `log2f` is (nearly) correctly rounded; the device's is not.**
   The icx build of `vmaf` resolves `log2f` in libimf: 33 of 20 000 000
   arguments differ from `(float)log2((double)x)`. glibc's `log2f` differs in
   18 589 of 20 000 000. `sycl::log2` differs from the host in about 9% of
   the arguments SpEED feeds it. The pipeline evaluates log2 in fp32 pairs
   (`ln m = 2 atanh s`, series to s^21) to about 2^-45 and rounds once.
5. **Every fp64 expression of the reference has an exact fp32 form.**
   `EIGENVALUE_EPS` is the fp64 `1e-6`, and `0x1.0c6f7ap-20f +
   0x1.6bdb1ap-49f` equals it exactly, so `fabsf(sd) < 1e-6 * s` is decided
   exactly from two FMA-exact products; `x < 1e-6` for a float x is
   `x <= (float)1e-6` because no float lies between the two. `-0.5 * tau * xv`
   in fp64 rounds once to the same fp32 value as `(-0.5f * tau) * xv`. The
   prescale coordinate `(y + 0.5) * ratio - 0.5` is exact as an fp32 pair.
   The covariance sum is fp64 in the reference (with a SIMD-specific
   accumulation order); the device carries it in fp32 pairs with exact
   differences and products, which reproduces the reference's fp32 result
   whenever the fp64 sum does not sit within about 2^-44 of an fp32 rounding
   boundary.
6. **The CPU filter is order-deterministic.** The AVX2 convolution and its
   scalar border path both accumulate `f[k] * x` from k = 0 with separate
   multiply and add, so a per-output sequential sum reproduces it; only the
   16x-decimated sample points are needed, which removes 255/256 of the
   anti-alias work.
7. **Launch count matters more than kernel time at small frames.** With the
   chain enqueued kernel by kernel (eight launches, an upload, a result copy),
   `speed_chroma_sycl` at 576x324 cost 3.57 ms per frame on the B580 and 4.05
   ms on the UHD 770 through WSL2 Level Zero, although the kernels themselves
   take about 1.5 ms. Recording the chain once as a SYCL command graph per
   channel binding and replaying it brought the B580 to 0.89 ms per frame. At
   4K the upload and the CLI's own frame reads dominate either way.
8. **Result.** On the Netflix 576x324 pair (48 frames) and 50 frames of BBB
   3840x2160, every per-frame `speed_chroma_u/v/uv` and `speed_temporal` value
   is identical to `--backend cpu` on both GPUs (max abs diff 0). The host
   split it replaces matched on 1-9 of 48/50 frames, max abs diff 4.2e-5.

## Alternatives explored

- **Host linear algebra with fewer waits** (batch the four channels into one
  round trip). Keeps the round trip the maintainer asked to remove and still
  needs the host float plane for the filter.
- **Single work-item eigen/QR with a software fp64.** Exact by construction
  only for one CPU SIMD path (the AVX2 and AVX-512 covariance kernels round
  differently), and orders of magnitude slower.
- **`-foffload-fp32-prec-div/-sqrt` on the image link.** Changes the numerics
  of every SYCL extractor at once; out of scope and unmeasured for the others.
- **Private arrays for the tridiagonal QR sweep.** 1.10 ms against 0.96 ms
  with the diagonals in local memory.

## Open questions

- The linear-algebra kernel is latency-bound on one work item (about 45
  implicit QR sweeps per channel). A sub-group formulation of the sweep does
  not exist in the reference order; the remaining cost is about 1 ms per
  frame on the B580 and 2-4 ms on the UHD 770.
- The CUDA and HIP twins keep the host residual; the algorithm here is the
  port target (`docs/state.md`).

## Related

- ADR-1358, ADR-0567, ADR-0964, ADR-1218, ADR-0220, ADR-0214.
- `docs/state.md`: `T-SYCL-FP-MODEL-PRECISE-CONTRACTS-2026-09-29`,
  `T-CUDA-SPEED-HOST-RESIDUAL-2026-09-29`, `T-HIP-SPEED-HOST-RESIDUAL-2026-09-29`.
