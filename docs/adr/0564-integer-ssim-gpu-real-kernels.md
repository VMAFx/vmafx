<!-- markdownlint-disable MD013 MD060 -->
# ADR-0564: Real integer_ssim GPU kernels (CUDA, HIP, SYCL) — replace silent float_ssim substitution

- **Status**: Accepted
- **Date**: 2026-05-18
- **Deciders**: lusoris, Claude (Anthropic)
- **Tags**: `cuda`, `hip`, `sycl`, `ssim`, `kernel`, `correctness`, `gpu`, `fork-local`

## Context

All three GPU backends (CUDA, HIP, SYCL) silently substituted `float_ssim` when
the caller requested `integer_ssim` (feature name `"ssim"`). Specifically:

- **CUDA**: `integer_ssim_cuda.c` registered `vmaf_fex_integer_ssim_cuda` but provided
  the `"float_ssim"` feature. The kernel (`ssim_score.cu`) was an 11-tap floating-point
  Gaussian — a different algorithm from the CPU `integer_ssim`.
- **HIP**: `integer_ssim_hip.c` used float intermediates (`float *d_ref_mu` etc.) instead
  of the int64 intermediates the real algorithm requires.
- **SYCL**: `integer_ssim_sycl.cpp` registered only `vmaf_fex_float_ssim_sycl`; there
  was no `vmaf_fex_integer_ssim_sycl` at all.

The CPU `integer_ssim` algorithm (`core/src/feature/integer_ssim.c`) uses:

- A 9-tap Gaussian kernel with **integer** weights `[2,9,28,55,68,55,28,9,2]`
  (sigma=1.5, `KERNEL_WEIGHT=256`, sum=256)
- `int64_t` accumulators for all per-pixel moments (mux, muy, x2, xy, y2, w)
- Boundary-truncation: out-of-image taps are skipped; `w` tracks only in-bounds weight
- Final SSIM formula computed in `double` from the int64 moments

`float_ssim` uses an 11-tap floating-point Gaussian — different kernel, different
arithmetic, systematically different scores. This meant every VMAF run on a GPU backend
silently reported float_ssim values in the "ssim" field, an undetectable correctness bug.

## Decision

We add real integer_ssim GPU kernels for all three backends and register them as
`vmaf_fex_integer_ssim_{cuda,hip,sycl}` providing feature `"ssim"`.

**CUDA** (`ssim_cuda.c` + `cuda/integer_ssim/integer_ssim_score.cu`):

- Two-pass design: Pass 1 is a 9-tap horizontal int64 accumulation; Pass 2 is a 9-tap
  vertical int64 accumulation + double SSIM formula + per-block double partial sum.
- Host reads back per-block double partials and int64 weights, computes
  `ssim = sum(partials) / sum(weights)`.
- Confirmed bit-exact (places=6) vs CPU on the Netflix golden 576×324 8bpc fixture:
  all 48 frames differ by 0.00e+00. The new extractor is distinct from the pre-existing
  `vmaf_fex_integer_ssim_cuda` (misnomer, actually provides float_ssim — retained for
  back-compat but registered under `"float_ssim"`).

**HIP** (`integer_ssim_hip.c` rewrite + pre-existing `hip/integer_ssim/integer_ssim_score.hip`):

- The `.hip` kernel already used int64 moments (per ADR-0533); the host glue used float
  intermediates — structural mismatch. Host glue rewritten to match: 6×int64 device
  buffers, two readback slots (double partials, int64 weights), same accumulation logic.
- HIP wavefront-64 (GCN/RDNA): paired int32 shuffles for int64 lane reduction.

**SYCL** (`integer_ssim_sycl.cpp` appended extractor):

- fp64-free constraint (ADR-0220): the Intel Arc A380 Level Zero driver rejects SPIR-V
  modules that use `double` inside kernel lambdas. SSIM formula therefore uses `float32`
  in-kernel (int64 moments remain exact; only the final SSIM division is float32).
- Expected precision: places=4–5 vs CPU (float32 SSIM accumulation drift).
- Documented in code and noted in parity matrix.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep float_ssim on GPU, document it clearly | Zero development cost | The "ssim" feature field would silently be wrong; parity matrix would forever show a correctness gap | Unacceptable — the entire purpose of integer_ssim is reproducibility with the CPU reference |
| Single float32 kernel for all three backends | Simple; avoids int64 register pressure | float32 SSIM is not bit-exact with the CPU integer algorithm; fails the places=6 gate | Would repeat the existing bug in a cleaner form |
| Maintain only CUDA + defer HIP/SYCL | Smaller scope | HIP already had the kernel; SYCL int64 extractor is straightforward | HIP kernel already existed; deferring wastes existing work |
| Port the 11-tap float_ssim to GPU and rename | Avoids int64 complexity | The CPU reference `"ssim"` is integer_ssim, not float_ssim | Wrong metric entirely |

## Consequences

- **Positive**: `--feature ssim` on CUDA/HIP now produces bit-exact results (places=6)
  vs CPU. SYCL produces places=4–5 results due to the fp64-free constraint — documented
  and flagged in the parity matrix.
- **Negative**: The pre-existing `integer_ssim_cuda.c` / `vmaf_fex_integer_ssim_cuda`
  symbol is now a misnomer (it provides `"float_ssim"`). It is retained for link-compat
  but must not be confused with the new `vmaf_fex_integer_ssim_cuda` in `ssim_cuda.c`.
  Future cleanup should rename `integer_ssim_cuda.c` → `float_ssim_cuda.c`.
- **Neutral / follow-ups**: HIP acceptance gate requires real hipcc + AMD GPU hardware;
  SYCL gate requires an Intel Arc device. CUDA gate confirmed locally on RTX 4090.
  The HIP host-glue rewrite did not change the kernel (already correct per ADR-0533).

## References

- `core/src/feature/integer_ssim.c` — CPU reference implementation
- `core/src/feature/cuda/integer_ssim/integer_ssim_score.cu` — new CUDA kernel
- `core/src/feature/cuda/ssim_cuda.c` — new CUDA host glue
- `core/src/feature/hip/integer_ssim_hip.c` — rewritten HIP host glue
- `core/src/feature/hip/integer_ssim/integer_ssim_score.hip` — pre-existing HIP kernel
- `core/src/feature/sycl/integer_ssim_sycl.cpp` — new SYCL extractor appended
- [ADR-0220](0220-sycl-fp64-free-kernel-constraint.md) — fp64-free SYCL kernel constraint
- [ADR-0533](0533-hip-all-extractors-registration-sweep.md) — HIP extractor registration sweep
- Source: req (the user required bit-exact GPU integer_ssim; "Don't ship 'almost-right' integer-ssim — the whole purpose of integer SSIM is bit-exactness. Better to keep dispatching CPU on a backend than ship a wrong kernel.")
