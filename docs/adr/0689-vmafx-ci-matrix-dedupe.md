<!-- markdownlint-disable MD013 MD060 -->
# ADR-0689: VMAFX CI Matrix Deduplication

- **Status**: Accepted
- **Date**: 2026-05-28
- **Deciders**: lusoris
- **Tags**: `ci`, `build`, `vmafx`

## Context

The fork's CI build matrix in `.github/workflows/libvmaf-build-matrix.yml` and
`.github/workflows/tests-and-quality-gates.yml` accumulated redundant legs over
successive PRs without a systematic review. As of 2026-05-28 the matrix ran
20 active build rows plus separate test jobs, with several rows that were proper
subsets of other rows (the plain CPU legs covered no failure mode not already
caught by the DNN legs). The VMAFX rebrand plan (ADR-0686 umbrella) called for
"CI matrix: dedupe properly — catch everything but don't run a hundred times;
avoid the current N-way matrix fan-out."

Additionally, `tests-and-quality-gates.yml` contained two Vulkan cross-backend
jobs (`vulkan-vif-cross-backend` and `vulkan-parity-matrix-gate`) that ran the
identical workload — same binary, same fixtures, same lavapipe ICD, same
Netflix normal pair — with the newer parity gate being a strict superset of the
older per-feature diff job.

## Decision

We remove the following legs from the PR CI matrix and consolidate as described:

### Removed from `libvmaf-build-matrix.yml` — `libvmaf-build` job

| Removed row | Rationale |
|---|---|
| `Build — Ubuntu gcc (CPU)` | Proper subset of `Build — Ubuntu gcc (CPU) + DNN`. The DNN build compiles the entire CPU source tree with the same compiler, additionally links ORT, and runs `meson test`. No unique regression class. |
| `Build — Ubuntu clang (CPU)` | Proper subset of `Build — Ubuntu clang (CPU) + DNN`. Same rationale as above. |
| `Build — macOS clang (CPU)` | Proper subset of `Build — macOS clang (CPU) + DNN` (`experimental: true`). macOS DNN leg already validates the macOS clang + libvmaf surface. |
| `Build — macOS Vulkan via MoltenVK (advisory)` | Was `continue-on-error: true` and `experimental: true` — never a merge gate. Consumed ~12+ min of macOS runner time per PR. Known fragile dependency: `GL_EXT_shader_atomic_int64` via Metal Tier-2 argument buffers (ADR-0338 known limitations). Moved to `nightly.yml` where the advisory cadence is appropriate. |
| `Build — Ubuntu CUDA` (dynamic, gcc+nvcc) | Proper subset of `Build — Ubuntu SYCL + CUDA` (combined leg). All CUDA translation units are compiled by nvcc in the combined leg. The static-library CUDA leg (`Build — Ubuntu CUDA Static`) is retained because it exercises a distinct NVCC-static interaction that the dynamic combined leg does not. |

### Removed from `tests-and-quality-gates.yml`

| Removed job | Rationale |
|---|---|
| `vulkan-vif-cross-backend` | Pure duplicate of `vulkan-parity-matrix-gate`. Both jobs: build CPU+Vulkan with the same meson flags, select lavapipe (`VK_LOADER_DRIVERS_SELECT: '*lvp*'`), run against the Netflix normal pair (576×324), and diff per-frame feature scores. The parity gate uses `cross_backend_parity_gate.py` with calibrated ULP tolerances from `gpu_ulp_calibration.yaml` and covers all 17 features in one invocation; the removed job used `cross_backend_vif_diff.py` in 12 sequential steps. No coverage is lost. |

### Added to `nightly.yml`

| New nightly job | Rationale |
|---|---|
| `moltenvk-build` | Preserves macOS MoltenVK coverage at a lower cadence; `continue-on-error: true` unchanged since MoltenVK-specific SPIR-V failures are a known-limitation class, not a correctness gate. |

### Retained legs (with rationale for keeping)

| Retained row | Why kept |
|---|---|
| `Build — Ubuntu ARM clang (CPU)` | Unique architecture (`ubuntu-24.04-arm`); catches NEON / alignment issues. |
| `Build — Ubuntu i686 gcc (CPU, no-asm)` | Pins ADR-0151 Netflix#1481 contract; unique cross-compile surface. |
| `Build — Ubuntu gcc (CPU) + DNN` [REQUIRED] | Required check; also now serves as primary gcc CPU gate. |
| `Build — Ubuntu clang (CPU) + DNN` [REQUIRED] | Required check; also now serves as primary clang CPU gate. |
| `Build — macOS clang (CPU) + DNN` (`experimental`) | Only remaining macOS general-CPU leg. |
| `Build — Ubuntu Vulkan (T5-1b runtime)` | Unique Vulkan backend build on Linux. |
| `Build — Ubuntu HIP (T7-10b runtime)` [REQUIRED] | Required check; covers AMD HIP backend build. |
| `Build — macOS Metal (T8-1 scaffold)` | Only macOS Metal stub test (validates -ENOSYS contract). |
| `Build — Ubuntu gcc Static (CPU)` | Unique: validates `libvmaf.pc` static pkgconfig. |
| `Build — Ubuntu CUDA Static` | Unique: NVCC-static interaction not covered by dynamic build. |
| `Build — Ubuntu SYCL` | Unique: icpx-only SYCL device-image link path. |
| `Build — Ubuntu SYCL + CUDA` | Unique: icpx+nvcc combined link path interaction. |
| `Build — Windows MinGW64 (CPU)` [REQUIRED] | Only MinGW64 Windows gate. |
| `Build — Windows MSVC + CUDA (build only)` [REQUIRED] | Only MSVC+NVCC Windows gate. |
| `Build — Windows MSVC + oneAPI SYCL (build only)` [REQUIRED] | Only MSVC+icx-cl Windows gate. |
| `vulkan-parity-matrix-gate` | Retained; supersedes the removed `vulkan-vif-cross-backend`. |

### Required-aggregator impact

None. Every removed leg was advisory (not listed in `required-aggregator.yml`).
The aggregator's `required` array is unchanged. No branch-protection update needed.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep all legs, add nightly-only flag | No PR risk | Does not reduce per-PR cost; required-check slots unchanged | The goal is reducing per-PR wall-clock and runner cost, not just moving the explosion |
| Remove DNN legs, keep bare CPU | Simpler bare builds | Loses ORT C-API compilation coverage on PRs; DNN legs are required checks | DNN legs carry more signal than bare legs at equal cost |
| Remove ARM + i686 legs | Fastest reduction | Loses cross-arch coverage; ARM and i686 catch alignment and ABI issues that x86-64 misses | Unique signal justified the cost |

## Consequences

- **Positive**: PR matrix reduces from 20 active build rows to 15. `tests-and-quality-gates.yml`
  loses one complete duplicate job. MoltenVK advisory coverage is preserved in nightly.
  Approximate wall-clock saving per PR: ~15–25 minutes of runner time (3 × Ubuntu CPU legs
  1 × macOS Vulkan MoltenVK leg dropped).
- **Negative**: A regression that only manifests on plain CPU gcc/clang without ORT linked in
  would not be caught on the PR matrix (effectively zero risk — ORT is header-included, not
  invasive; the DNN flag adds ORT link steps only).
- **Neutral / follow-ups**: The `continue-on-error` field on the `libvmaf-build` job is
  simplified to `false` unconditionally (the only row that set it to `true` was the
  MoltenVK row, which is now gone from this job).

## References

- Parent umbrella: ADR-0686 (VMAFX Phase 1B rebrand plan)
- ADR-0338 (macOS MoltenVK advisory lane semantics)
- ADR-0214 (GPU-parity matrix gate / T6-8)
- ADR-0120 (DNN-enabled build matrix legs)
- ADR-0151 (i686 cross-build Netflix#1481 contract)
- ADR-0115 (Windows build in unified matrix)
- `req` (paraphrased): user direction to "dedupe properly — catch everything but don't run a hundred times; avoid the current N-way matrix fan-out"
