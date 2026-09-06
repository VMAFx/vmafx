<!-- markdownlint-disable MD013 MD041 MD060 -->

# ADR-1205: The ssimulacra2 FMA unification extends to the scalar fallback and every GPU host copy

- **Status**: Proposed
- **Date**: 2026-09-06
- **Deciders**: Lusoris
- **Tags**: cuda, sycl, hip, metal, simd, correctness, feature-extractor, reproducibility

## Context

[ADR-0891](0891-simd-bit-exact-round2-fmaf-libvmaf-feature-icx.md) unified the YCbCr ->
linear-RGB conversion in `ssimulacra2` on a single-rounded fused multiply-add
so the vector kernels and their scalar tails agree bit-for-bit. The change
landed in the four SIMD implementations (AVX2, AVX-512, NEON, SVE2) **and in
the private scalar reference inside `core/test/test_ssimulacra2_simd.c`**, but
not in any of the five shipped copies that are not SIMD:

| Path | Conversion |
|---|---|
| `test_ssimulacra2_simd.c` reference | `fmaf()` |
| AVX2 / AVX-512 / NEON / SVE2 (and their scalar tails) | fused multiply-add |
| `core/src/feature/ssimulacra2.c` (shipped scalar fallback) | plain mul-then-add |
| `cuda/ssimulacra2_cuda.c`, `hip/ssimulacra2_hip.c`, `metal/ssimulacra2_metal.mm`, `sycl/ssimulacra2_sycl.cpp` | plain mul-then-add |

Because the SIMD test compares the SIMD kernels against its own private
reference rather than against the shipped scalar function, it asserted
bit-exactness, passed, and could not see that no shipped non-SIMD path matched.

The consequences are larger than the 1-ULP seed suggests, because the
ssimulacra2 pipeline is ill-conditioned downstream: the edge-diff term computes
`|img - blur(img)|`, a catastrophic cancellation, and the pooling takes a
4-norm, which is dominated by the few largest survivors. Measured on an
RTX 4090 at 256x144, a ~1.8e-07 difference in linear RGB on 3677 of 36864
pixels grew to 1.3e-06 in XYB, 6.6e-06 after the recursive Gaussian, and
**2.62e-03** in the final score against a places=4 (1e-4) gate. Forcing the CPU
to its scalar path made the CPU and CUDA scores bit-identical
(delta exactly 0.0), which proves the GPU kernels and host helpers were
correct all along and the divergent side was the CPU SIMD reference.

This is therefore not only a GPU-parity defect: on a host without AVX2 the
shipped scalar fallback produced different `ssimulacra2` scores than on a host
with it.

## Decision

We will use `fmaf()` in the same three positions and the same order as
ADR-0891 in all five remaining copies — the shipped CPU scalar fallback and the
CUDA, HIP, Metal and SYCL host conversions — so that every shipped path
computes one value.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Add `fmaf()` to the five missed copies (chosen) | Every shipped path agrees; removes a host-dependent score; no change to already-correct SIMD output | Five files must stay in sync | — |
| Remove FMA from the SIMD kernels instead | Also unifies; arguably simpler arithmetic | Reverses ADR-0891, costs SIMD throughput, and changes scores on the overwhelmingly common AVX2/AVX-512 hosts | Rejected — would move the majority path, not the minority |
| Compile the affected TUs with `-ffp-contract=fast` and rely on the compiler | No source change | Empirically ineffective — rebuilding with contraction off left the delta at 2.62e-03, so contraction was never the mechanism; also leaves behaviour at the mercy of compiler and flags | Rejected — measured and falsified |
| Widen the ssimulacra2 parity tolerance | Zero code change | Hides a real cross-host reproducibility bug | Rejected |

## Consequences

- **Positive**: CPU-vs-CUDA `ssimulacra2` agreement improves from 2.62e-03 to
  ~2.8e-09, uniformly across 256x144 .. 1280x720. The scalar fallback now
  produces the same score as the SIMD paths, so `ssimulacra2` no longer depends
  on the host ISA.
- **Negative**: `ssimulacra2` scores produced by a *non-SIMD* host before this
  change do not reproduce after it. That path was the incorrect one, and no
  published fork artifact is known to have come from a non-SIMD host; scores
  from AVX2/AVX-512/NEON/SVE2 hosts are unchanged.
- **Neutral / follow-ups**: `test_ssimulacra2_simd.c` should eventually assert
  against the *shipped* scalar function rather than a private copy of it; until
  it does, this class of drift can recur. Tracked as a follow-up rather than
  fixed here to keep this change reviewable.

## References

- [ADR-0891](0891-simd-bit-exact-round2-fmaf-libvmaf-feature-icx.md) — the original FMA
  unification this completes.
- Reproducer: `meson test -C build --suite=gpu test_cuda_ssimulacra2_parity`
  on a CUDA host.
- Source: `req` — user direction to fix the outstanding GPU parity failures.
