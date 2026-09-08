<!-- markdownlint-disable MD013 MD041 MD060 -->

# ADR-1224: CUDA Tile C++ is not adopted; the audit's incidental findings are

- **Status**: Proposed
- **Date**: 2026-09-07
- **Deciders**: Lusoris
- **Tags**: `cuda`, `performance`, `build`, `correctness`

## Context

CUDA 13.3 (May 2026) brought NVIDIA's Tile programming model to C++ —
`cuda_tile.h`, namespace `cuda::tiles`, `__tile_global__` kernels, and
`ct::mma()` for tensor-core multiply-accumulate. It was evaluated for the
fork's CUDA backend across six dimensions, with every claim adversarially
re-checked against the tree and the installed toolkit (nvcc 13.3.73, RTX 4090
`sm_89`, driver 610.57.04).

The evaluation's own verification refuted several of the arguments *against*
adoption, which is worth recording so they are not repeated:

- The architecture floor and the CUDA CI pin are **not** reasons. ADR-1223
  raises the floor to compute capability 8.0 and puts CI on 13.3.1 for
  unrelated reasons, so Tile's `sm_80` requirement costs the fork nothing.
- `ct::mma()` **does** accept `float` and `double` operands
  (`crt/cuda_tile.h:2652-2658`); "wrong dtype" is not the objection for the
  float kernels.
- NVIDIA does **not** state that thread configuration and memory scheduling
  are "not user-controllable". No such sentence exists in the Tile C++
  documentation; the actual text concerns thread count only, and grid
  dimensions remain with the caller.

What survives is narrower and stronger.

**There is no consumer for the feature's central primitive.** Integer
`ct::mma()` takes 8-bit operands with a 32-bit accumulator
(`crt/cuda_tile.h:2642`, `integer_mma_v`), and roughly two thirds of the CUDA
tree is int16/int32/int64 fixed point. The longest contraction anywhere is a
17-tap constant filter (`float_vif_score.cu:65`). VIF's statistics pass is
quadratic in its input (`filter1d.cu:178-181` accumulates `ref*ref`,
`ref*dis`, `dis*dis`), so no GEMM formulation exists at any precision. And
decisively, independent of dtype and architecture: `adm_csf_den.cu:101` and
`adm_cm.cu:266` apply a **rounding right-shift inside the accumulation**,
which is not a sum of products and therefore not an MMA. SpEED's 25x25
covariance is the only matrix-shaped work, and 25 is not a power of two.

**Tile extents must be powers of two** (`crt/cuda_tile.h:749`,
`is_pow2(T::static_extent(Seq))`). Verified by compilation: `shape<576>` and
`shape<1920>` are rejected. The fork's golden fixture is 576 wide and HD
frames are 1920, so a per-row reduction can never be one tile.

**Reduction order has no API surface.** `ct::sum()` takes a `rounding_mode`
and nothing else; a grep of all 4,064 lines of `cuda_tile.h` for
`associat|determinis|reduction order|reproducib` returns nothing. Against
ADR-0214's `places=4` contract that is unquantifiable, and the measured
headroom is thin — ADR-0990 records a `double`→`float` change in MS-SSIM
producing ~0.004 drift, ~80x the CI tolerance.

**The build pipeline bifurcates and one half fails silently.** Measured:
`--fatbin --enable-tile` drops the tile kernel and exits 0; `--tilefatbin`
drops the SIMT kernels *and* emits no PTX even when `compute_80,code=compute_80`
is passed explicitly — which destroys the unconditional JIT fallback the fork
guarantees. The two phase flags are not composable in one translation unit,
so every ported kernel would need a permanent SIMT twin.

## Decision

We will not adopt CUDA Tile C++. The evaluation's incidental findings are
adopted instead, in this change and its follow-ups:

1. `integer_ssim_score.cu` summed the two halves of an `int64` warp reduction
   independently, dropping the carry — replaced with the existing
   `warp_reduce(int64_t)` helper.
2. `docs/backends/cuda/overview.md` contradicted itself about whether the
   default model's ADM runs on device — resolved against the code.
3. `nvcc --threads` is wired to a meson option, measured byte-identical.
4. The `adm_cm` register-pressure finding is tracked for its own measured PR.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Do not adopt; take the incidental findings (chosen) | Keeps one programming model per backend; banks three real fixes the audit surfaced | Leaves the TMA question unmeasured | — |
| Adopt for the float kernels only | `ct::mma()` does accept float/double | The float kernels are stencils and reductions, not contractions; and it still bifurcates the build and loses the PTX floor | No contraction to accelerate |
| Adopt for SpEED's covariance | Genuinely matrix-shaped | M=N=25 is not a power of two (measured: `shape<25,25>` and `shape<24,24>` both fail to compile); the operand is 25 overlapping shifted windows, never materialised; the accumulator is `double` for CPU parity; and the one true GEMM runs on the host at 0.39 MFLOP | Fails on four independent grounds |
| Wait for CUDA 13.4+ | Costs nothing today | Waiting is not a decision, and leaves the audit's findings unbanked | Recorded under "what would change the answer" instead |

## Consequences

- **Positive**: the CUDA backend keeps one programming model, stays diffable
  against its HIP twin, and keeps its unconditional `compute_80` PTX floor.
  Three defects/wins land instead.
- **Negative**: no tensor-core or TMA acceleration. The TMA half is genuinely
  unmeasured — see below.
- **Neutral / follow-ups**: a timeboxed TMA spike on `float_vif_compute` is
  authorised to close the one open question. `adm_cm_aim_line_kernel_8` sits at
  REG:255 with 336-344 B spill and 16.7% occupancy on `sm_89` and gets its own
  measured PR.

### What would change the answer

Re-evaluate only if all of 1-3 hold, or 4 lands on its own with numbers:

1. `ct::mma()` gains 16/32-bit integer operands. Necessary but **not
   sufficient** — the per-term rounding shift in `adm_csf_den` / `adm_cm` is
   not a sum of products at any dtype.
2. NVIDIA publishes a reduction-order or association guarantee, or `ct::sum()`
   gains an association parameter.
3. Non-power-of-two tile extents, or a documented pattern for runtime row
   widths that does not reintroduce grouping-dependent rounding.
4. A measured TMA win — NVIDIA frames tiles as access to the tensor memory
   accelerator as well as tensor cores, and nobody has measured whether
   TMA-driven staging beats the hand-written shared-memory tiling in
   `float_vif_compute`. The ncu digests put that kernel at 0.84 waves/SM and
   48.6% achieved occupancy, i.e. launch-width limited rather than
   staging-limited, so this is not obviously promising — but it is untested.

Two preconditions apply regardless: `--tilefatbin` must emit PTX (or the fork
must consciously drop its JIT floor), and NVIDIA-hardware parity CI must exist
— ADR-0214 concedes the CUDA lane is `if: false`.

### CUDA 13.3 items worth taking

Ranked, from the same evaluation. Only the first changes emitted code.

1. **CUDA 13.3.1 in CI** (ADR-1223, already in flight). 13.3 fixes a compiler
   bug present since 12.8 that "could cause compiler-inserted thread
   reconvergence to fail and leave stale or corrupted values in registers";
   `docs/research/0734-cuda-13.3-non-blog-findings.md` rates it CRITICAL.
2. **`nvcc --threads`** — adopted here. Measured 22/22 byte-identical fatbins
   at `-t 4` vs `-t 1`.
3. **Register-pressure tuning on `adm_cm`** — its own PR.
4. **`--std c++23`** — verified byte-identical; take opportunistically.
5. **`nvprune`** for container images only — cuts embedded fatbins 88%, but
   strips the PTX JIT fallback, so never for the portable release `.so`.

Explicitly rejected: **`--split-compile`** — three identical invocations
produced three different fatbin hashes, which breaks build reproducibility and
therefore the keyless Sigstore/SLSA signing story. CompileIQ is not in the
installed toolkit and its GEMM/attention win class does not exist here.

## References

- [Develop High-Performance GPU Kernels in C++ with NVIDIA CUDA Tile](https://developer.nvidia.com/blog/develop-high-performance-gpu-kernels-in-cpp-with-nvidia-cuda-tile/)
- `/opt/cuda/targets/x86_64-linux/include/crt/cuda_tile.h` (13.3.73) —
  `is_pow2` at 749, `integer_mma_v` at 2642, `ct::sum` at 2363.
- [ADR-1223](1223-cuda-ampere-architecture-floor.md) — the compute-capability
  floor and the CI bump, decided independently.
- [ADR-0214](0214-gpu-parity-ci-gate.md) — the places=4 gate this would put at
  risk.
