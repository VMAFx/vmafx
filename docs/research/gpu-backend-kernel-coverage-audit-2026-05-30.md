<!-- markdownlint-disable MD060 -->
# GPU backend kernel coverage audit — 2026-05-30

## Scope

Cross-reference of the registered GPU feature extractors against
existing parity / smoke tests under `core/test/`, to identify coverage
gaps that would let a regression in a kernel's reduction, separable
filter, or per-plane accumulator escape CI.

## Method

```bash
# 1. Enumerate registered GPU extractors.
grep -rE '\.name\s*=\s*"[a-z0-9_]+_(cuda|hip|sycl|metal)"' \
  core/src/feature/{cuda,hip,sycl,metal} | sort

# 2. Enumerate test files referencing each.
find core/test -name '*test*cuda*' -o -name '*test*hip*' \
  -o -name '*test*sycl*' -o -name '*test*metal*'

# 3. Diff the two lists.
```

## Pre-PR coverage (master tip `bbcaa8d127`)

| Backend | Registered extractors | Parity-tested | Coverage % |
|---|---:|---:|---:|
| CUDA | 17 | 2 (`motion_cuda`, `vif_cuda`) | 12 % |
| HIP | 18 | 2 (`motion_hip`, `adm_hip`) | 11 % |
| SYCL | 17 | 2 (`motion_sycl`, `cambi_sycl`) | 12 % |
| Metal | 8 | 1 spot-asserted (`motion_v2_metal`) | 13 % |

## Post-PR coverage (this branch)

| Backend | Registered | Parity-tested | Δ | New |
|---|---:|---:|---:|---|
| CUDA | 17 | 4 | +2 | `psnr_cuda`, `ciede_cuda` |
| HIP | 18 | 4 | +2 | `psnr_hip`, `vif_hip` |
| SYCL | 17 | 4 | +2 | `psnr_sycl`, `vif_sycl` |
| Metal | 8 | 8 (registration audit) + 1 | +7 | full 8-extractor name + TEMPORAL-flag audit |

## Kernel selection rationale

Per-backend, picked the two highest-leverage kernels:

1. **PSNR** — per-plane SSE reduction; sensitive to work-group
   tiling and atomic-reduce ordering. Covers the most-shipped GPU
   compute pattern (host-side log10 plus device-side SSE).
2. **VIF** (HIP/SYCL) — separable Gaussian filter chain feeding
   M1..M4 statistical accumulators; sensitive to filter accuracy
   and accumulator order. CUDA already has `test_integer_vif_cpu_cuda_parity`.
3. **CIEDE2000** (CUDA) — CIE-Lab conversion + chroma-rotation
   kernel; sensitive to the colour-conversion path.

## Tolerance budget

Per ADR-0214:

- places=4 (1e-4) — unfiltered reductions (PSNR, CIEDE2000).
- places=3 (1e-3) — filtered features (VIF scale0).

GPUs are NOT bit-exact with CPU per the user-memory rule
`feedback_golden_gate_cpu_only`. Tighter tolerances would be
false-positives.

## Remaining gaps (follow-up backlog)

`core/test/` still lacks parity gates for 38 extractors:

- **CUDA** (13): `cambi_cuda`, `adm_cuda`, `float_psnr_cuda`,
  `float_vif_cuda`, `float_adm_cuda`, `float_motion_cuda`,
  `psnr_hvs_cuda`, `integer_ssim_cuda`, `float_ssim_cuda`,
  `float_ms_ssim_cuda`, `float_moment_cuda`, `speed_chroma_cuda`,
  `speed_temporal_cuda`.
- **HIP** (14): `ciede_hip`, `cambi_hip`, `float_psnr_hip`,
  `float_vif_hip`, `float_adm_hip`, `float_motion_hip`,
  `float_moment_hip`, `psnr_hvs_hip`, `integer_ssim_hip`,
  `float_ssim_hip`, `integer_ms_ssim_hip`, `motion_v2_hip`,
  `speed_chroma_hip`, `speed_temporal_hip`, `ssimulacra2_hip`.
- **SYCL** (13): `adm_sycl`, `ciede_sycl`, `psnr_hvs_sycl`,
  `integer_ssim_sycl`, `float_ssim_sycl`, `float_ms_ssim_sycl`,
  `float_psnr_sycl`, `float_vif_sycl`, `float_adm_sycl`,
  `float_motion_sycl`, `motion_v2_sycl`, `speed_chroma_sycl`,
  `speed_temporal_sycl`, `float_moment_sycl`, `ssimulacra2_sycl`.

Recommend tracking in `.workingdir2/BACKLOG.md` under
`gpu-coverage-tier-2`, sized at ~6 tests per follow-up PR to stay
within the 200–800 LOC bundle target.

## PR overlap audit

Confirmed no overlap with avoid-list PRs:

- **#289** (CUDA PTX unload) — touches `cuda/picture_cuda.c`,
  not kernel parity tests.
- **#290** (HIP ssimulacra2) — ships `ssimulacra2_hip` kernel + a
  bit-exactness gate scoped to that extractor only. Our HIP tests
  cover `psnr_hip` + `vif_hip`.
- **#293** (SYCL 4-extractor) — different SYCL extractors
  (`adm_sycl`, `float_motion_sycl`, etc.). Our SYCL tests cover
  `psnr_sycl` + `vif_sycl`.
- **#294** (Metal dispatch) — Metal dispatch-strategy assertion;
  our Metal test asserts the extractor-registration surface.
- **#308** (HIP/Metal -ENOSYS stubs) — scaffold-shape gates.
- **#315** (orphan tests) — wires already-written tests into
  meson; we add net-new test files.

## References

- `req` — user request asking for GPU-kernel test coverage push
  across CUDA + HIP + SYCL + Metal with the avoid-list above.
- ADR-0214 — cross-backend tolerance budget.
- ADR-0361 — Metal backend rollout.
- `feedback_golden_gate_cpu_only` (user memory) — GPU paths are not
  bit-exact with CPU.
